// Nubix — stream session orchestration.
//
// Session flow, watchdogs and recovery heuristics are ported from green-nx
// (src/switch/stream/engine.cpp, GPL-3.0, (c) the green-nx authors): the
// gssv retry ladder (home console wake-up / registration, dead media path),
// remote ICE gathering that waits for a real (priority > 1000) candidate,
// the off-thread keepalive with worker-stall watchdog, media/decode stall
// watchdogs, input-channel death detection with same-session re-signaling,
// input revive after media disruption, handshake / TransactionStart handling,
// serverInitiatedDisconnect and rumble. The libpeer socket pump of the
// original is replaced by libdatachannel's own threads (see WebRtc).
//
// Threads (all owned by Streamer::Impl):
//   worker  — signaling state machine + per-transport event loop/watchdogs
//   decode  — consumes depacketized access units, feeds VideoDecoder
//   input   — 125 Hz gamepad sender (contiguous sequence numbers)
//   rtcp    — NACK flush (20 ms), RR + REMB + stats (1 s)
//   keepalive — gssv /keepalive (per session), worker-stall watchdog
//   voice   — VoiceChat capture thread (mic -> Opus -> WebRtc::sendMicOpus), per transport
//   reneg   — voice chat renegotiation: gssv POST + poll of the second offer, posts an event
// libdatachannel callbacks never block on anything the worker holds while
// calling into WebRtc: they only push events, feed the depacketizer and the
// audio player, or record rumble.
//
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "stream/streamer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include <libavutil/frame.h>
}

#include "core/http.hpp"
#include "core/log.hpp"
#include "platform/audio_in.hpp"  // complete type for VoiceChat's default source argument
#include "platform/platform.hpp"
#include "stream/audio.hpp"
#include "stream/rtp.hpp"
#include "stream/video.hpp"
#include "stream/voice.hpp"
#include "stream/webrtc.hpp"

namespace xc {

const char* streamStateName(StreamState s) {
    switch (s) {
        case StreamState::Idle: return "Idle";
        case StreamState::Starting: return "Starting";
        case StreamState::Queued: return "Queued";
        case StreamState::Provisioning: return "Provisioning";
        case StreamState::Connecting: return "Connecting";
        case StreamState::Streaming: return "Streaming";
        case StreamState::Ended: return "Ended";
        case StreamState::Failed: return "Failed";
    }
    return "?";
}

namespace {

using json = nlohmann::json;

// ---- tunables (values from green-nx unless noted) ---------------------------
// Decode queue limits. Every queued AU is latency (the newest decoded frame is shown), so a
// backlog is flushed and resynced on an IDR instead of being worked off: hard cap ~100 ms at
// 60 fps, or a soft backlog of kSoftQueuedVideo AUs whose oldest has waited kSoftQueueMs.
constexpr size_t kMaxQueuedVideo = 6;
constexpr size_t kSoftQueuedVideo = 3;
constexpr uint64_t kSoftQueueMs = 60;
constexpr uint64_t kDecodeReportMs = 10000;     // decoder speed check window
constexpr double kFrameIntervalMs = 1000.0 / 60.0;
constexpr long kQuitDeleteTimeoutSec = 5;       // final DELETE when the user stops / app exits
constexpr long kDeleteTimeoutSec = 20;
constexpr int kInputPeriodMs = 8;               // 125 Hz input cadence
constexpr int kInputHeartbeatMs = 50;           // resend unchanged pad state (ARCHITECTURE.md §5)
constexpr int kInputBackoffMs = 100;            // after a refused SCTP send
constexpr int kKeyframeMinIntervalMs = 1000;    // PLI + control request rate limit
constexpr int kRtcpTickMs = 20;                 // NACK flush cadence
constexpr int kReportIntervalMs = 1000;         // RR + REMB + stats
constexpr int kGatherTimeoutMs = 5000;          // local ICE gathering
constexpr int kSdpAnswerTimeoutMs = 60000;      // green-nx: 120 polls x 500 ms
constexpr int kSdpAnswerVoiceTimeoutMs = 30000; // voice offer: fall back without voice sooner
constexpr int kRemoteIceTimeoutMs = 15000;      // green-nx gather deadline (home consoles: Teredo)
constexpr int kRemoteIceCloudTimeoutMs = 4000;  // cloud: the first answer already carries usable candidates
constexpr int kRemoteIcePollMs = 300;
constexpr int kRemoteIceQuietPolls = 4;         // no new candidates for 4 polls -> complete
constexpr unsigned long kRealCandidatePriority = 1000;  // placeholder front candidate has 100
constexpr int kDeadMediaPathMs = 12000;         // ICE up, channels never open
constexpr int kNegotiationTimeoutMs = 45000;    // remote set -> handshake complete
constexpr int kFirstFrameTimeoutMs = 30000;     // handshake -> first decoded frame
constexpr int kChannelsReadyGraceMs = 5000;     // HandshakeAck -> control/input open
constexpr int kDisconnectGraceMs = 5000;        // "disconnected" may recover (ICE consent)
constexpr int kMediaStallMs = 10000;            // no RTP at all
constexpr int kDecodeResyncMs = 5000;           // no decoded frame -> depacketizer reset + IDR
constexpr int kDecodeStallMs = 15000;           // no decoded frame -> give up
constexpr int kWorkerStallMs = 10000;           // worker event loop wedged
constexpr int kInputDeadSeconds = 3;            // all input sends failing -> re-signal
constexpr int kMaxResignals = 3;                // per 2-minute window
constexpr int kResignalWindowMs = 120000;
constexpr int kStatePollMs = 1000;
constexpr int kWaitTimeQueryMs = 15000;
constexpr int kQueueCapMs = 2 * 60 * 60 * 1000; // WaitingForResources: wait as long as the server queues us
constexpr int kStateCapMs = 210000;             // any other gssv state that should move
constexpr int kResumeStateCapMs = 30000;        // re-signal: give up on the old session quickly
constexpr int kMaxPollErrors = 5;
// Voice chat renegotiation (second offer, isMediaStreamsChatRenegotiation).
constexpr int kRenegTimeoutMs = 10000;          // POST + answer, else abandoned (stream continues)
constexpr int kRenegPollMs = 500;
constexpr int kRenegMicWaitMs = 2000;           // after the handshake, wait this long for the mic before renegotiating
constexpr int kChatLogInfoMessages = 5;         // chat channel traffic: first N at INFO, then DEBUG

uint64_t nowMs() { return platform::monotonicMs(); }

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// Type-checked string field of server JSON: a missing key or any non-string value (null, number,
// object) yields "" instead of the type_error json::value() throws.
std::string jstr(const json& j, const char* key) {
    if (!j.is_object()) return std::string();
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// "candidate:<foundation> <component> <transport> <priority> <address> <port> typ ..."
// (optionally prefixed with "a="). Returns false if the line has no priority field.
bool candidatePriority(const std::string& line, unsigned long& prio) {
    std::string c = line;
    if (c.rfind("a=", 0) == 0) c.erase(0, 2);
    std::istringstream ss(c);
    std::string tok;
    for (int field = 0; ss >> tok; ++field) {
        if (field == 3) {
            char* end = nullptr;
            prio = std::strtoul(tok.c_str(), &end, 10);
            return end && *end == '\0';
        }
    }
    return false;
}

std::string trimCandidate(std::string c) {
    while (!c.empty() && (c.back() == '\r' || c.back() == '\n' || c.back() == ' ')) c.pop_back();
    return c;
}

std::string preview(const std::string& s, size_t n = 220) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

void resolutionSize(const std::string& alias, int& w, int& h) {
    if (alias == "720" || alias == "720HQ") {
        w = 1280;
        h = 720;
    } else {
        w = 1920;
        h = 1080;
    }
}

}  // namespace

// =============================================================================

struct Streamer::Impl {
    Impl(Config& c, Auth& a) : cfg(c), auth(a) {}

    Config& cfg;
    Auth& auth;

    // ---- session parameters (copied at start) ----
    SessionKind kind = SessionKind::Cloud;
    std::string target;
    bool f2pOnly = false;
    Settings settings;
    std::string installId;

    // ---- threads / lifecycle ----
    std::mutex lifeMu;  // serializes doStart()/stopLocked()
    std::thread workerThr, decodeThr, inputThr, rtcpThr;
    // Lifecycle thread: runs start/stop work (thread joins, final DELETE) so neither the UI
    // thread nor the App's shared worker ever blocks on a session teardown.
    std::mutex lifeQMu;
    std::condition_variable lifeQCv;
    std::deque<std::function<void()>> lifeQ;
    std::thread lifeThr;
    bool lifeQuit = false;               // lifeQMu
    bool lifeClosing = false;            // lifeQMu: shutdownLife() in progress
    std::atomic<uint64_t> startGen{0};   // bumped by every start()/stopAsync()/stop()
    std::atomic<bool> active{false};     // a start was requested and not stopped since
    std::atomic<bool> quit{false};
    std::atomic<bool> workerDone{true};
    std::atomic<bool> running{false};  // threads exist and must be joined

    // ---- public state ----
    std::atomic<StreamState> state{StreamState::Idle};
    mutable std::mutex statusMu;
    std::string status;
    std::atomic<int> queueWaitSec{-1};

    // ---- current transport ----
    std::mutex rtcMu;
    std::shared_ptr<WebRtc> rtc;
    std::atomic<uint32_t> rtcGen{0};
    std::atomic<bool> handshakeDone{false};
    std::atomic<bool> pcConnected{false};
    std::atomic<bool> gotFrame{false};      // this transport decoded a frame
    std::atomic<bool> everStreamed{false};  // any transport of this session decoded a frame
    std::atomic<bool> resuming{false};      // same-session re-signal in progress
    std::atomic<bool> serverEnded{false};
    std::string serverEndReason;            // statusMu

    // ---- worker events (from libdatachannel callbacks and the renegotiation thread) ----
    struct Event {
        enum Type { ChannelOpen, ChannelClose, Text, PcState, RenegResult } type;
        uint32_t gen;
        std::string ch, text;  // RenegResult: text = answer SDP ("" = failed), ch = failure reason
    };
    std::mutex evMu;
    std::condition_variable evCv;  // events + quit wakeups for the worker
    std::deque<Event> events;

    // ---- video ----
    std::mutex depackMu;
    RtpH264Depacketizer depack;
    bool waitKeyframe = true;  // depackMu: drop AUs until an IDR after (re)start/overflow
    struct AccessUnit {
        std::vector<uint8_t> data;
        uint32_t rtpTs;
        uint32_t gen;
        uint64_t queuedMs;
    };
    std::mutex frameMu;
    std::condition_variable frameCv;
    std::deque<AccessUnit> frameQ;
    std::atomic<bool> keyframeWanted{false};
    VideoDecoder decoder;
    std::atomic<bool> decoderOk{false};

    // ---- audio ----
    AudioPlayer audio;
    std::atomic<bool> audioOk{false};
    // Small reorder window in front of the decoder: a packet one slot late is still played
    // instead of being concealed and then dropped. Also the audio RR statistics.
    RtpOpusReceiver audioRx;     // internally locked
    std::mutex audioDrainMu;     // keeps pop -> pushOpus in order across threads
    std::vector<uint8_t> audioScratch;  // audioDrainMu
    // Party / chat voice delivered as its own SSRC (WebRtc onChatAudioRtp): separate reorder
    // window + RR statistics, played through AudioPlayer::pushVoiceOpus.
    RtpOpusReceiver chatRx;             // internally locked
    std::vector<uint8_t> chatScratch;   // audioDrainMu
    std::atomic<uint64_t> chatRxPackets{0};  // per session

    // ---- voice chat (microphone) ----
    // The VoiceChat of the current transport (created at HandshakeAck, stopped before the
    // PeerConnection closes). Readers (UI thread) copy the pointer under voiceMu.
    mutable std::mutex voiceMu;
    std::shared_ptr<VoiceChat> voice;
    std::atomic<bool> micMuted{false};        // user preference, kept for the process lifetime
    std::atomic<bool> voiceSessionOff{false}; // this session fell back to no voice (offer refused)
    std::atomic<int> chatMsgs{0};             // chat data channel messages seen (log level)
    // Per transport, worker thread only:
    bool txVoice = false;                     // offer negotiated voice (sendrecv + chatStream)
    bool txHome = false;
    proto::OfferMic txMic;
    std::string txInitialAnswer;
    bool txMicAccepted = false;               // answer audio is sendrecv/recvonly (server takes our mic)
    std::vector<std::string> txLocalCandidates;  // for the ICE exchange after the renegotiation
    std::string txUfrag;
    uint64_t renegDueAt = 0;                  // handshake time; mic Live/Muted wait ends kRenegMicWaitMs later
    bool renegStarted = false;
    bool renegNoChatLogged = false;
    std::atomic<int> chatStreamVersion{-1};   // exchangeResponse.chatStream of the answer
    std::atomic<bool> voiceRenegotiated{false};
    // Renegotiation POST/poll thread (never touches WebRtc; reports through an event).
    std::thread renegThr;
    std::atomic<bool> renegAbort{false};

    // ---- counters ----
    std::atomic<uint64_t> videoBytes{0};
    std::atomic<uint64_t> lastMediaMs{0};
    std::atomic<uint64_t> lastDecodeMs{0};
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<uint64_t> decodeUsAvg{0};
    std::atomic<uint32_t> decodeErrors{0};
    std::atomic<uint64_t> lastKeyframeReqMs{0};
    std::atomic<uint32_t> keyframeReqs{0};
    std::atomic<uint64_t> workerTick{0};
    std::atomic<int> videoLossPct{0};
    std::atomic<uint32_t> queueFlushes{0};

    // ---- input ----
    std::mutex inputMu;  // serializer + send order (contiguous sequence numbers)
    proto::InputSerializer serializer;
    std::mutex padMu;
    GamepadState pad;
    std::atomic<bool> padDirty{true};
    std::atomic<uint32_t> inSentSec{0}, inFailSec{0}, inRxSec{0};
    std::atomic<uint64_t> inputRxLastMs{0};

    // ---- rumble ----
    std::mutex vibMu;
    Vibration vib;
    bool vibPending = false;
    bool vibLogged = false;

    // ---- stats ----
    mutable std::mutex statsMu;
    StreamStats st;
    std::atomic<int> frameW{0}, frameH{0};

    // ===================== helpers =====================

    // Session progress from the worker side. Ignored once quit is set: a session being torn down
    // must not overwrite the state a newer start() already published.
    void setStatus(const std::string& s) {
        if (quit) return;
        {
            std::lock_guard<std::mutex> lk(statusMu);
            if (status == s) return;
            status = s;
        }
        XC_LOGI("stream: %s", s.c_str());
    }
    void setState(StreamState s) {
        if (quit) return;
        publishState(s);
    }
    // Unconditional (start() / stopLocked()).
    void publishState(StreamState s) {
        StreamState prev = state.exchange(s);
        if (prev != s) XC_LOGI("stream state: %s -> %s", streamStateName(prev), streamStateName(s));
    }
    // A failure the user caused by cancelling is not a failure.
    void fail(const std::string& msg) {
        if (quit) return;
        XC_LOGE("stream FAIL: %s", msg.c_str());
        {
            std::lock_guard<std::mutex> lk(statusMu);
            status = msg;
        }
        setState(StreamState::Failed);
    }
    void endSession(const std::string& msg) {
        if (quit) return;
        {
            std::lock_guard<std::mutex> lk(statusMu);
            status = msg;
        }
        setState(StreamState::Ended);
    }

    // Interruptible sleep; false if quit was requested.
    bool sleepMs(int ms) {
        std::unique_lock<std::mutex> lk(evMu);
        evCv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return quit.load(); });
        return !quit;
    }

    std::shared_ptr<WebRtc> currentRtc() {
        std::lock_guard<std::mutex> lk(rtcMu);
        return rtc;
    }

    bool sendText(WebRtc& r, const char* ch, const std::string& text) {
        bool ok = r.sendText(ch, text);
        if (ok)
            XC_LOGD("send [%s] %s", ch, preview(text).c_str());
        else
            XC_LOGW("send [%s] FAILED: %s", ch, preview(text, 120).c_str());
        return ok;
    }

    // PLI + control keyframe request, at most once per second unless forced.
    void requestKeyframeInternal(bool force) {
        if (!handshakeDone) return;
        uint64_t now = nowMs();
        uint64_t last = lastKeyframeReqMs.load();
        if (force) {
            lastKeyframeReqMs = now;
        } else {
            if (last && now >= last && now - last < kKeyframeMinIntervalMs) return;
            // Several threads may ask at once: only the CAS winner sends.
            if (!lastKeyframeReqMs.compare_exchange_strong(last, now)) return;
        }
        std::shared_ptr<WebRtc> r = currentRtc();
        if (!r) return;
        keyframeReqs++;
        r->requestKeyframe();  // RTCP PLI: what xCloud actually acts on
        r->sendText("control", proto::controlKeyframeRequest());
        XC_LOGD("keyframe requested (%u so far)", keyframeReqs.load());
    }

    void pushEvent(Event ev) {
        {
            std::lock_guard<std::mutex> lk(evMu);
            events.push_back(std::move(ev));
        }
        evCv.notify_all();
    }

    // ===================== libdatachannel callbacks =====================

    WebRtc::Callbacks makeCallbacks(uint32_t gen) {
        WebRtc::Callbacks cb;
        cb.onVideoRtp = [this, gen](const uint8_t* d, size_t n) { onVideoRtp(gen, d, n); };
        cb.onAudioRtp = [this, gen](const uint8_t* d, size_t n) { onAudioRtp(gen, d, n); };
        cb.onSenderReport = [this, gen](bool video, uint32_t ssrc, uint64_t ntp) {
            if (gen != rtcGen.load()) return;
            if (video) {
                std::lock_guard<std::mutex> lk(depackMu);
                depack.stats().onSenderReport(ntp);
            } else if (ssrc != 0 && ssrc == chatRx.ssrc()) {
                chatRx.stats().onSenderReport(ntp);  // separate party/chat voice stream
            } else {
                audioRx.stats().onSenderReport(ntp);
            }
        };
        cb.onText = [this, gen](const std::string& ch, const std::string& text) {
            if (gen != rtcGen.load()) return;
            pushEvent(Event{Event::Text, gen, ch, text});
        };
        cb.onBinary = [this, gen](const std::string& ch, const uint8_t* d, size_t n) {
            if (gen != rtcGen.load()) return;
            onBinary(ch, d, n);
        };
        cb.onChannelOpen = [this, gen](const std::string& ch) {
            if (gen != rtcGen.load()) return;
            pushEvent(Event{Event::ChannelOpen, gen, ch, std::string()});
        };
        cb.onChannelClosed = [this, gen](const std::string& ch) {
            if (gen != rtcGen.load()) return;
            pushEvent(Event{Event::ChannelClose, gen, ch, std::string()});
        };
        cb.onChatAudioRtp = [this, gen](const uint8_t* d, size_t n) { onChatAudioRtp(gen, d, n); };
        cb.onState = [this, gen](const std::string& s) {
            if (gen != rtcGen.load()) return;
            pcConnected = (s == "connected");
            pushEvent(Event{Event::PcState, gen, std::string(), s});
        };
        return cb;
    }

    void onVideoRtp(uint32_t gen, const uint8_t* d, size_t n) {
        if (gen != rtcGen.load()) return;
        lastMediaMs = nowMs();
        videoBytes += n;
        bool overflow = false, softOverflow = false;
        bool queued = false;
        bool gatedDrop = false;
        size_t flushed = 0;
        {
            std::lock_guard<std::mutex> lk(depackMu);
            depack.push(d, n);
            std::vector<uint8_t> au;
            uint32_t ts = 0;
            bool key = false;
            while (depack.popFrame(au, ts, key)) {
                if (waitKeyframe) {
                    if (!key) {  // never feed the decoder a P-frame without its IDR
                        gatedDrop = true;
                        continue;
                    }
                    waitKeyframe = false;
                    XC_LOGI("video: keyframe (%zu bytes), decoding resumes", au.size());
                }
                const uint64_t now = nowMs();
                std::lock_guard<std::mutex> fl(frameMu);
                const bool hard = frameQ.size() >= kMaxQueuedVideo;
                const bool soft = !hard && frameQ.size() >= kSoftQueuedVideo &&
                                  now - frameQ.front().queuedMs > kSoftQueueMs;
                if (hard || soft) {
                    // The decoder is behind: every queued AU is added latency. Dropping single
                    // AUs corrupts the reference chain, so clear and restart from an IDR.
                    flushed += frameQ.size();
                    frameQ.clear();
                    waitKeyframe = true;
                    overflow = overflow || hard;
                    softOverflow = softOverflow || soft;
                    if (!key) {
                        gatedDrop = true;
                        continue;
                    }
                    waitKeyframe = false;
                }
                frameQ.push_back(AccessUnit{std::move(au), ts, gen, now});
                au = std::vector<uint8_t>();
                queued = true;
            }
        }
        if (queued) frameCv.notify_one();
        if (overflow || softOverflow) {
            XC_LOGW("video: decoder %zu frames behind (%s), flushed, waiting for a keyframe", flushed,
                    overflow ? "queue full" : "backlog too old");
            queueFlushes++;
        }
        // Keep asking while the gate drops frames: a single lost PLI/IDR must not freeze the
        // picture until the 5 s decode watchdog (requestKeyframeInternal throttles to 1/s).
        if (overflow || softOverflow || gatedDrop) keyframeWanted = true;
    }

    void onAudioRtp(uint32_t gen, const uint8_t* d, size_t n) {
        if (gen != rtcGen.load()) return;
        lastMediaMs = nowMs();
        audioRx.push(d, n);  // statistics + reorder window
        drainAudio();
    }

    // Move every in-order (or timed-out gap) Opus packet from the reorder window to the player.
    // Called per audio packet and from the 20 ms RTCP tick (so a gap times out without traffic).
    void drainAudio() {
        std::lock_guard<std::mutex> lk(audioDrainMu);
        uint32_t ts = 0;
        int lostBefore = 0;
        while (audioRx.pop(audioScratch, ts, lostBefore))
            if (audioOk) audio.pushOpus(audioScratch.data(), audioScratch.size(), lostBefore);
        drainChatLocked();
    }

    // Second PT-111 SSRC (party / chat voice as its own stream): mixed into the output.
    void onChatAudioRtp(uint32_t gen, const uint8_t* d, size_t n) {
        if (gen != rtcGen.load()) return;
        std::lock_guard<std::mutex> lk(audioDrainMu);
        if (n >= 12) {
            // WebRtc replaces the chat SSRC only after the old one went quiet: start the
            // reorder window and the voice decoder fresh instead of waiting for the latch.
            const uint32_t ssrc = (uint32_t(d[8]) << 24) | (uint32_t(d[9]) << 16) | (uint32_t(d[10]) << 8) | d[11];
            const uint32_t prev = chatRx.ssrc();
            if (prev != 0 && prev != ssrc) {
                chatRx.reset();
                if (audioOk) audio.resetVoice();
            }
        }
        chatRx.push(d, n);
        drainChatLocked();
    }

    // Caller holds audioDrainMu.
    void drainChatLocked() {
        uint32_t ts = 0;
        int lostBefore = 0;
        while (chatRx.pop(chatScratch, ts, lostBefore)) {
            if (!audioOk) continue;
            audio.pushVoiceOpus(chatScratch.data(), chatScratch.size(), lostBefore);
            chatRxPackets++;
        }
    }

    // Server -> client input-channel reports (rumble, server metadata). Runs on a
    // libdatachannel thread; only touches its own small locks.
    void onBinary(const std::string& ch, const uint8_t* d, size_t n) {
        if (ch == "chat") {
            // Logged only (voice goes over RTP); nothing is ever sent on this channel.
            if (chatMsgs.fetch_add(1) < kChatLogInfoMessages)
                XC_LOGI("recv [chat] %zu binary bytes", n);
            else
                XC_LOGD("recv [chat] %zu binary bytes", n);
            return;
        }
        if (ch != "input") {
            XC_LOGD("recv [%s] %zu binary bytes", ch.c_str(), n);
            return;
        }
        inRxSec++;
        inputRxLastMs = nowMs();
        Vibration v;
        if (proto::parseVibration(d, n, v)) {
            std::lock_guard<std::mutex> lk(vibMu);
            vib = v;
            vibPending = true;
            if (!vibLogged) {
                vibLogged = true;
                XC_LOGI("rumble: first server vibration report received");
            }
        }
        uint32_t w = 0, h = 0;
        if (proto::parseServerMetadata(d, n, w, h)) {
            XC_LOGI("server metadata: stream %ux%u", w, h);
            if (w && h && !frameW) {
                frameW = static_cast<int>(w);
                frameH = static_cast<int>(h);
            }
        }
    }

    // ===================== helper threads =====================

    // Thread entry guard: an exception (bad_alloc, a JSON type_error, ...) must end the session
    // with an error instead of std::terminate taking the whole app down mid-stream.
    template <typename F>
    void guarded(const char* what, F&& body) {
        try {
            body();
        } catch (const std::exception& e) {
            XC_LOGE("stream: %s thread: unexpected exception: %s", what, e.what());
            fail(std::string("Internal error in the ") + what + " thread: " + e.what());
        } catch (...) {
            XC_LOGE("stream: %s thread: unexpected exception", what);
            fail(std::string("Internal error in the ") + what + " thread");
        }
    }

    void decodeMain() { guarded("decode", [this] { decodeLoop(); }); }
    void inputMain() { guarded("input", [this] { inputLoop(); }); }
    void rtcpMain() { guarded("rtcp", [this] { rtcpLoop(); }); }

    void decodeLoop() {
        uint64_t prevFrames = 0;
        // Decoder speed check: software decode slower than the frame interval cannot keep up and
        // ends in queue flushes; say so in the log (and suggest 720p) instead of failing silently.
        uint64_t windowStart = nowMs(), windowUs = 0, windowAus = 0, slowWindows = 0;
        while (!quit && !workerDone) {
            AccessUnit au;
            {
                std::unique_lock<std::mutex> lk(frameMu);
                frameCv.wait_for(lk, std::chrono::milliseconds(20),
                                 [this] { return quit.load() || workerDone.load() || !frameQ.empty(); });
                if (quit || workerDone) break;
                if (frameQ.empty()) continue;
                au = std::move(frameQ.front());
                frameQ.pop_front();
            }
            if (au.gen != rtcGen.load()) continue;  // belongs to a closed transport
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = decoder.decode(au.data.data(), au.data.size(), static_cast<int64_t>(au.rtpTs));
            windowUs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
            ++windowAus;
            const uint64_t tnow = nowMs();
            if (tnow - windowStart >= kDecodeReportMs) {
                const double avgMs = windowAus ? static_cast<double>(windowUs) / 1000.0 / windowAus : 0;
                const uint32_t flushes = queueFlushes.exchange(0);
                if (avgMs > kFrameIntervalMs * 0.9 || flushes > 0) {
                    ++slowWindows;
                    XC_LOGW("video: decode %.1f ms/frame vs %.1f ms frame interval, %u backlog flush(es) in %llu s%s",
                            avgMs, kFrameIntervalMs, flushes, static_cast<unsigned long long>((tnow - windowStart) / 1000),
                            settings.resolution != "720" ? " - the decoder is too slow for this resolution, pick 720p in Settings"
                                                         : "");
                } else if (slowWindows) {
                    XC_LOGI("video: decode %.1f ms/frame, keeping up again", avgMs);
                    slowWindows = 0;
                }
                windowStart = tnow;
                windowUs = windowAus = 0;
            }
            uint64_t fd = decoder.framesDecoded();
            decodeUsAvg = static_cast<uint64_t>(decoder.avgDecodeMs() * 1000.0);
            if (fd != prevFrames) {
                prevFrames = fd;
                framesDecoded = fd;
                lastDecodeMs = nowMs();
                if (au.gen == rtcGen.load() && !gotFrame.exchange(true)) {
                    XC_LOGI("video: first frame decoded");
                    everStreamed = true;
                }
            }
            if (!ok) {
                decodeErrors++;
                requestKeyframeInternal(false);
            }
        }
    }

    void inputLoop() {
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        GamepadState last;
        uint64_t lastSend = 0, backoffUntil = 0;
        uint32_t lastGen = 0;
        while (!quit && !workerDone) {
            next += std::chrono::milliseconds(kInputPeriodMs);
            auto now = clock::now();
            if (next < now - std::chrono::milliseconds(100)) next = now;  // resync after a stall
            std::this_thread::sleep_until(next);
            if (!handshakeDone || !pcConnected) continue;
            std::shared_ptr<WebRtc> r = currentRtc();
            if (!r) continue;
            uint32_t gen = rtcGen.load();
            if (gen != lastGen) {  // new transport: full state once, no stale backoff
                lastGen = gen;
                backoffUntil = 0;
                lastSend = 0;
            }
            uint64_t t = nowMs();
            if (backoffUntil && t < backoffUntil) continue;
            if (!r->isChannelOpen("input")) continue;
            GamepadState cur;
            {
                std::lock_guard<std::mutex> lk(padMu);
                cur = pad;
            }
            bool dirty = padDirty.load();
            if (!dirty && cur == last && lastSend && t - lastSend < kInputHeartbeatMs) continue;
            bool ok;
            {
                // Build only when the send is certain to be attempted and roll back a refused one:
                // the server stops applying input after a sequence gap (green-nx #45).
                std::lock_guard<std::mutex> lk(inputMu);
                ok = r->sendBinary("input", serializer.gamepad(cur, 0));
                if (!ok) serializer.rollback();
            }
            if (ok) {
                inSentSec++;
                last = cur;
                lastSend = t;
                backoffUntil = 0;
                padDirty = false;
            } else {
                inFailSec++;
                backoffUntil = t + kInputBackoffMs;
            }
        }
    }

    void rtcpLoop() {
        uint64_t lastReport = nowMs();
        uint64_t prevBytes = videoBytes.load(), prevFrames = framesDecoded.load();
        uint64_t prevMicBytes = 0;
        const WebRtc* prevMicRtc = nullptr;
        while (!quit && !workerDone) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRtcpTickMs));
            std::shared_ptr<WebRtc> r = currentRtc();
            std::vector<uint16_t> nacks;
            bool needKey = false;
            {
                std::lock_guard<std::mutex> lk(depackMu);
                nacks = depack.takeNackList();
                needKey = depack.needKeyframe();
            }
            if (keyframeWanted.exchange(false)) needKey = true;
            if (r && !nacks.empty()) {
                uint32_t ssrc = r->videoSsrc();
                if (ssrc) r->sendNack(ssrc, nacks);
            }
            if (needKey) requestKeyframeInternal(false);
            drainAudio();  // lets a reorder-window gap time out even when no audio arrives

            uint64_t now = nowMs();
            if (now - lastReport < kReportIntervalMs) continue;
            double dt = static_cast<double>(now - lastReport) / 1000.0;
            lastReport = now;

            RtpReceiveStats v;
            RtpVideoCounters vc;
            {
                std::lock_guard<std::mutex> lk(depackMu);
                v = depack.stats().snapshotForReport();
                vc = depack.counters();
            }
            RtpReceiveStats a = audioRx.stats().snapshotForReport();
            RtpReceiveStats c;
            if (chatRx.ssrc() != 0) c = chatRx.stats().snapshotForReport();
            int micKbps = 0;
            if (r) {
                const WebRtc::MicTxStats mt = r->micTxStats();
                // A new transport restarts its counters: no delta across PeerConnections.
                if (r.get() != prevMicRtc || mt.bytes < prevMicBytes) {
                    prevMicRtc = r.get();
                    prevMicBytes = mt.bytes;
                }
                if (dt > 0) micKbps = static_cast<int>(static_cast<double>(mt.bytes - prevMicBytes) * 8.0 / 1000.0 / dt);
                prevMicBytes = mt.bytes;
            }
            if (r) {
                std::vector<RtpReceiveStats> blocks;
                if (v.ssrc && v.packets) blocks.push_back(v);
                if (a.ssrc && a.packets) blocks.push_back(a);
                if (c.ssrc && c.packets) blocks.push_back(c);
                if (!blocks.empty()) r->sendReceiverReport(blocks);
                if (r->videoSsrc()) r->sendRemb(static_cast<uint32_t>(settings.bitrateKbps) * 1000u);
            }
            int lossPct = static_cast<int>(v.fractionLost * 100 / 256);
            videoLossPct = lossPct;

            uint64_t bytes = videoBytes.load(), frames = framesDecoded.load();
            StreamStats s;
            s.width = frameW;
            s.height = frameH;
            s.fps = dt > 0 ? static_cast<double>(frames - prevFrames) / dt : 0;
            s.bitrateKbps = dt > 0 ? static_cast<double>(bytes - prevBytes) * 8.0 / 1000.0 / dt : 0;
            s.decodeMs = static_cast<double>(decodeUsAvg.load()) / 1000.0;
            s.lossPercent = v.fractionLost * 100.0 / 256.0;
            s.rttMs = vc.rttMs ? static_cast<int>(vc.rttMs) : -1;  // NACK round-trip estimate
            s.audioBufferMs = audioOk ? audio.bufferedMs() : 0;
            s.voiceNegotiated = chatStreamVersion.load() >= 1;
            s.voiceRenegotiated = voiceRenegotiated.load();
            s.micTxKbps = micKbps;
            s.chatRxPackets = chatRxPackets.load();
            prevBytes = bytes;
            prevFrames = frames;
            {
                std::lock_guard<std::mutex> lk(statsMu);
                st = s;
            }
        }
    }

    // gssv keepalive runs off the worker: it is a blocking HTTPS call (green-nx: a
    // stalled pump every 15 s otherwise). It also watches the worker heartbeat.
    void keepaliveMain(std::string base, std::string token, Settings gs, SessionInfo s, std::atomic<bool>* stop) {
        guarded("keepalive", [&] { keepaliveLoop(base, token, gs, s, stop); });
    }

    // gs: the session's gssv settings (xhome: android/720 fingerprint, see runSession).
    void keepaliveLoop(const std::string& base, const std::string& token, const Settings& gs, const SessionInfo& s,
                       std::atomic<bool>* stop) {
        Http::AbortScope abortOnStop(stop);  // join in ~Keepalive must not wait for a 25 s request
        GssvClient gssv(base, token, gs);
        bool authReported = false;
        int interval = gssv.keepAliveSeconds(s);
        if (interval < 5) interval = 20;
        XC_LOGI("keepalive every %d s", interval);
        uint64_t next = nowMs() + static_cast<uint64_t>(interval) * 1000;
        uint64_t prevRound = nowMs();
        bool stallReported = false;
        int failures = 0;
        while (!quit && !*stop) {
            uint64_t now = nowMs();
            // Worker-stall watchdog. A >2 s gap in our own rounds means the whole
            // process was suspended: skip that round instead of misreading it.
            uint64_t tick = workerTick.load();
            if (now - prevRound <= 2000 && !stallReported && tick && now > tick && now - tick > kWorkerStallMs) {
                stallReported = true;
                XC_LOGE("stream worker stalled for %llu ms", static_cast<unsigned long long>(now - tick));
                fail("Stream engine stalled, please start the stream again");
            }
            prevRound = now;
            if (now < next) {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(100, next - now)));
                continue;
            }
            uint64_t started = nowMs();
            if (gssv.keepAlive(s)) {
                failures = 0;
            } else if (!quit && !*stop) {
                XC_LOGW("keepalive failed (%d in a row)", ++failures);
                if (gssv.authRejected() && !authReported) {
                    authReported = true;
                    auth.invalidateStreaming();  // the next session re-authorizes first
                }
            }
            uint64_t took = nowMs() - started;
            if (took >= 1000) XC_LOGD("keepalive took %llu ms", static_cast<unsigned long long>(took));
            next = nowMs() + static_cast<uint64_t>(interval) * 1000;
        }
    }

    struct Keepalive {
        std::atomic<bool> stop{false};
        std::thread thr;
        ~Keepalive() {
            stop = true;
            if (thr.joinable()) thr.join();
        }
    };

    // ===================== transport =====================

    void resetTransportState() {
        handshakeDone = false;
        pcConnected = false;
        gotFrame = false;
        serverEnded = false;
        {
            std::lock_guard<std::mutex> lk(depackMu);
            depack.reset();
            waitKeyframe = true;
        }
        {
            std::lock_guard<std::mutex> lk(frameMu);
            frameQ.clear();
        }
        {
            std::lock_guard<std::mutex> lk(audioDrainMu);
            audioRx.reset();
            chatRx.reset();
        }
        if (audioOk) {
            audio.reset();  // new transport: fresh RTP sequence + Opus state
            audio.resetVoice();
        }
        txVoice = false;
        txMic = proto::OfferMic();
        txInitialAnswer.clear();
        txMicAccepted = false;
        txLocalCandidates.clear();
        txUfrag.clear();
        renegDueAt = 0;
        renegStarted = false;
        renegNoChatLogged = false;
        chatStreamVersion = -1;
        voiceRenegotiated = false;
        chatMsgs = 0;
        {
            std::lock_guard<std::mutex> lk(evMu);
            events.clear();
        }
        lastMediaMs = 0;
        lastDecodeMs = 0;
        lastKeyframeReqMs = 0;
        keyframeWanted = false;
        inSentSec = 0;
        inFailSec = 0;
        inRxSec = 0;
        inputRxLastMs = 0;
        padDirty = true;
    }

    // Stop the microphone and the renegotiation thread of the current transport. Runs before
    // every PeerConnection close (closePeer) so neither sends into a closing WebRtc. Called on
    // the worker or (after the worker joined) the lifecycle thread, never on the capture or
    // renegotiation thread itself (it joins them).
    void stopVoice() {
        std::shared_ptr<VoiceChat> v;
        {
            std::lock_guard<std::mutex> lk(voiceMu);
            v = std::move(voice);
            voice.reset();
        }
        if (v) {
            v->stop();
            XC_LOGI("voice: microphone stopped");
        }
        renegAbort = true;
        if (renegThr.joinable()) renegThr.join();
    }

    // Detach and close the current PeerConnection. Callbacks of the old generation are
    // ignored from here on; close() is called without holding any of our locks.
    void closePeer() {
        stopVoice();
        std::shared_ptr<WebRtc> old;
        {
            std::lock_guard<std::mutex> lk(rtcMu);
            old = std::move(rtc);
            rtc.reset();
            rtcGen++;
        }
        handshakeDone = false;
        pcConnected = false;
        if (old) {
            old->close();
            XC_LOGI("peer connection closed");
        }
        {
            std::lock_guard<std::mutex> lk(frameMu);
            frameQ.clear();
        }
    }

    enum class PeerOutcome { Finished, RetryFresh, Resignal };

    // Re-announce the pad in-band (green-nx revive_input): ClientMetadata (consumes a
    // sequence number, rolled back on failure) + gamepadChanged false/true.
    void reviveInput(WebRtc& r, const char* reason) {
        {
            std::lock_guard<std::mutex> lk(inputMu);
            if (!r.sendBinary("input", serializer.clientMetadata())) serializer.rollback();
        }
        sendText(r, "control", proto::controlGamepadChanged(0, false));
        sendText(r, "control", proto::controlGamepadChanged(0, true));
        padDirty = true;
        uint64_t rx = inputRxLastMs.load(), now = nowMs();
        XC_LOGI("input revive (%s): pad re-announced, server input last seen %s", reason,
                rx && now >= rx ? (std::to_string((now - rx) / 1000) + "s ago").c_str() : "never");
    }

    // Start the microphone for transport gen (voice negotiated in the offer). The sink runs on
    // the capture thread: it sends through the current WebRtc unless the transport changed.
    void startVoice(uint32_t gen) {
        if (currentVoice()) return;  // already running for this transport
        auto v = std::make_shared<VoiceChat>();
        VoiceChat::FrameSink sink = [this, gen](const uint8_t* opus, size_t n, uint32_t samples48k, bool marker) {
            if (gen != rtcGen.load()) return;
            std::shared_ptr<WebRtc> r = currentRtc();
            if (r) r->sendMicOpus(opus, n, samples48k, marker);
        };
        const bool muted = micMuted.load();
        if (!v->start(std::move(sink), muted)) {
            XC_LOGW("voice: microphone thread could not be started, streaming without voice chat");
            return;
        }
        XC_LOGI("voice: microphone starting (%s)", muted ? "muted" : "live");
        std::lock_guard<std::mutex> lk(voiceMu);
        voice = std::move(v);
    }

    std::shared_ptr<VoiceChat> currentVoice() const {
        std::lock_guard<std::mutex> lk(voiceMu);
        return voice;
    }

    void afterHandshakeAck(WebRtc& r, uint32_t gen) {
        sendText(r, "control", proto::controlAuthorization());
        sendText(r, "control", proto::controlGamepadChanged(0, true));
        // Pin the resolution; without it servers pick 1440p for desktop-class clients.
        sendText(r, "control", proto::controlResolution(settings.resolution));
        int w, h;
        resolutionSize(settings.resolution, w, h);
        for (const std::string& m : proto::messageInitSequence(settings, installId, w, h)) sendText(r, "message", m);
        {
            std::lock_guard<std::mutex> lk(inputMu);
            if (!r.sendBinary("input", serializer.clientMetadata())) {
                serializer.rollback();
                XC_LOGW("input ClientMetadata send failed");
            }
        }
        handshakeDone = true;
        padDirty = true;
        // Ask for an IDR right away (PLI + control) instead of waiting for the periodic one.
        requestKeyframeInternal(true);
        XC_LOGI("handshake complete, capabilities sent");
        if (txVoice && txMicAccepted)
            startVoice(gen);
        else if (txVoice)
            XC_LOGI("voice: microphone stays off (server did not accept it on this connection)");
        else if (!settings.voiceChat)
            XC_LOGI("voice: disabled in settings");
        else
            XC_LOGI("voice: not negotiated for this connection, microphone stays off");
    }

    // Returns true if the session must end (server disconnect).
    bool handleMessageChannel(WebRtc& r, const std::string& text, bool& ackReceived) {
        json j = json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            // Not JSON: still honour a disconnect notice (green-nx matches on the substring).
            if (contains(text, "serverInitiatedDisconnect")) {
                setServerEnded("");
                return true;
            }
            return false;
        }
        const std::string type = jstr(j, "type");
        if (type == "HandshakeAck") {
            ackReceived = true;
            return false;
        }
        if (type != "TransactionStart" && type != "Message") {
            if (type == "Error") XC_LOGW("message channel error: %s", preview(text).c_str());
            return false;
        }
        const std::string id = jstr(j, "id");
        const std::string tgt = jstr(j, "target");
        const std::string cv = jstr(j, "cv");
        const std::string content = jstr(j, "content");
        const bool transaction = type == "TransactionStart";

        if (contains(tgt, "serverInitiatedDisconnect")) {
            std::string reason;
            json c = json::parse(content, nullptr, false);
            if (!c.is_discarded() && c.is_object()) reason = jstr(c, "reason");
            if (transaction) sendText(r, "message", proto::transactionComplete(id, "{}"));
            setServerEnded(reason);
            return true;
        }
        if (contains(tgt, "/streaming/systemUi/messages/ShowMessageDialog")) {
            json c = json::parse(content, nullptr, false);
            if (!c.is_discarded() && c.is_object())
                XC_LOGI("server dialog: \"%s\" — %s", jstr(c, "TitleText").c_str(), jstr(c, "ContentText").c_str());
            // No dialog UI: accept with the first (default) button.
            if (transaction) sendText(r, "message", proto::transactionComplete(id, "{\"Result\":0}"));
            return false;
        }
        if (contains(tgt, "/streaming/properties/titleinfo")) {
            XC_LOGI("title info: %s", preview(content).c_str());
        } else if (contains(tgt, "/streaming/touchcontrols/")) {
            // touch layouts are irrelevant for a gamepad client
        } else {
            XC_LOGD("unhandled %s target %s", type.c_str(), tgt.c_str());
        }
        if (transaction) {
            if (contains(tgt, "/streaming/properties/") || contains(tgt, "/streaming/touchcontrols/")) {
                sendText(r, "message", proto::transactionComplete(id, "{}"));
            } else {
                // XStreaming answers unknown transactions with "Unhandled" so the server does not wait.
                json u = {{"type", "Unhandled"}, {"id", id}, {"target", tgt}, {"cv", cv}};
                sendText(r, "message", u.dump());
            }
        }
        return false;
    }

    void setServerEnded(const std::string& reason) {
        XC_LOGI("server ended the session%s%s%s", reason.empty() ? "" : " (", reason.c_str(),
                reason.empty() ? "" : ")");
        {
            std::lock_guard<std::mutex> lk(statusMu);
            serverEndReason = reason;
        }
        serverEnded = true;
    }

    PeerOutcome runPeer(GssvClient& gssv, const SessionInfo& s) {
        const bool home = kind == SessionKind::Home;
        if (!resuming) setState(StreamState::Connecting);
        setStatus(resuming ? "Reconnecting..." : "Negotiating connection...");
        resetTransportState();

        auto r = std::make_shared<WebRtc>();
        uint32_t gen;
        {
            std::lock_guard<std::mutex> lk(rtcMu);
            gen = ++rtcGen;
        }
        struct Closer {
            Impl* self;
            ~Closer() { self->closePeer(); }
        } closer{this};
        // Voice chat: the audio m-line goes sendrecv with our mic SSRC and the POST asks for
        // chatStream. Off (setting, or this session fell back): byte-identical to before.
        const bool voiceWanted = settings.voiceChat && !voiceSessionOff.load();
        if (!r->init(makeCallbacks(gen), voiceWanted)) {
            fail("Failed to create the WebRTC connection");
            return PeerOutcome::Finished;
        }
        {
            std::lock_guard<std::mutex> lk(rtcMu);
            if (gen != rtcGen.load()) return PeerOutcome::Finished;
            rtc = r;
        }

        // ---- local description / candidates ----
        std::string ufrag, pwd, fingerprint, err;
        std::vector<std::string> local;
        if (!r->gatherLocal(ufrag, pwd, fingerprint, local, kGatherTimeoutMs, &quit)) {
            if (quit) return PeerOutcome::Finished;
            fail("Local network setup (ICE gathering) failed");
            return PeerOutcome::Finished;
        }
        if (quit) return PeerOutcome::Finished;
        XC_LOGI("gathered %zu local candidates (ufrag %s)", local.size(), ufrag.c_str());
        for (const auto& c : local) XC_LOGD("  local  cand: %s", c.c_str());
        txHome = home;
        txVoice = false;
        if (voiceWanted) {
            const WebRtc::MicInfo mi = r->micInfo();
            if (mi.ssrc != 0) {
                txMic.ssrc = mi.ssrc;
                txMic.cname = mi.cname;
                txMic.msid = mi.msid;
                txMic.trackId = mi.trackId;
                txVoice = true;
            } else {
                XC_LOGW("voice: no microphone send stream on the peer connection, offering without voice chat");
            }
        }
        std::string offer =
            proto::buildOffer(ufrag, pwd, fingerprint, home, settings.resolution, txVoice ? &txMic : nullptr, 2);
        XC_LOGD("offer sdp:\n%s", offer.c_str());

        // ---- SDP exchange ----
        if (!gssv.sendSdpOffer(s, offer, err, txVoice)) {
            if (txVoice && !quit) {
                // Never let voice chat cost the stream: renegotiate this session without it.
                XC_LOGW("voice: offer with voice chat refused (%s), retrying without voice chat", err.c_str());
                voiceSessionOff = true;
                return resuming ? PeerOutcome::RetryFresh : PeerOutcome::Resignal;
            }
            if (resuming) {
                XC_LOGW("re-signal refused (sdp): %s", err.c_str());
                return PeerOutcome::RetryFresh;
            }
            fail("SDP offer rejected: " + err);
            return PeerOutcome::Finished;
        }
        std::string answer;
        {
            // With voice chat offered, give up on the answer sooner: the fallback without voice
            // then still has the rest of the user's patience.
            uint64_t deadline = nowMs() + (txVoice ? kSdpAnswerVoiceTimeoutMs : kSdpAnswerTimeoutMs);
            bool ready = false;
            bool rejected = false;
            int errors = 0;
            SdpAnswerInfo info;
            err.clear();
            while (!quit && !ready) {
                if (gssv.pollSdpAnswer(s, info, ready, err)) {
                    answer = info.sdp;
                } else {
                    XC_LOGW("sdp poll failed: %s", err.c_str());
                    // The server refused the exchange itself: polling again will not change it.
                    if (info.rejected) {
                        rejected = true;
                        break;
                    }
                    if (++errors >= kMaxPollErrors) break;
                }
                if (ready) break;
                if (nowMs() > deadline) break;
                if (!sleepMs(500)) return PeerOutcome::Finished;
            }
            if (quit) return PeerOutcome::Finished;
            if (!ready || answer.empty()) {
                // Never let voice chat cost the stream: a refused exchange or no answer to the
                // voice offer is retried once without voice chat (as for a refused POST above).
                if (txVoice) {
                    XC_LOGW("voice: no usable SDP answer to the voice chat offer (%s%s), retrying without voice chat",
                            rejected ? "refused: " : "", err.empty() ? "timeout" : err.c_str());
                    voiceSessionOff = true;
                    return resuming ? PeerOutcome::RetryFresh : PeerOutcome::Resignal;
                }
                if (resuming) return PeerOutcome::RetryFresh;
                fail(err.empty() ? "Timed out waiting for the SDP answer" : "SDP exchange failed: " + err);
                return PeerOutcome::Finished;
            }
            chatStreamVersion = info.chatStream;
            XC_LOGI("voice: answer chat=%d chatStream=%d", info.chat, info.chatStream);
        }
        txInitialAnswer = answer;
        txLocalCandidates = local;
        txUfrag = ufrag;
        XC_LOGI("answer received (%zu bytes)", answer.size());
        XC_LOGD("answer sdp:\n%s", answer.c_str());

        // ---- ICE exchange ----
        // Our candidates go out over /ice too (the official client posts them explicitly).
        if (!local.empty() && !gssv.sendIce(s, local, ufrag, err)) XC_LOGW("local candidate post failed: %s", err.c_str());

        // xCloud trickles its candidates via /ice. It first returns a placeholder front
        // candidate (priority 100, 13.104.x) that never answers STUN; the real (Teredo)
        // candidate can arrive seconds later, so keep polling until one with priority > 1000.
        std::vector<std::string> remote;
        {
            std::set<std::string> seen;
            uint64_t deadline = nowMs() + (kind == SessionKind::Home ? kRemoteIceTimeoutMs : kRemoteIceCloudTimeoutMs);
            int quiet = 0;
            bool hasReal = false;
            while (!quit && nowMs() < deadline) {
                std::vector<std::string> got;
                bool ready = false;
                size_t before = remote.size();
                if (gssv.pollIce(s, got, ready, err)) {
                    for (auto& c : got) {
                        std::string line = trimCandidate(c);
                        if (line.empty() || !seen.insert(line).second) continue;
                        unsigned long prio = 0;
                        if (candidatePriority(line, prio) && prio > kRealCandidatePriority) hasReal = true;
                        remote.push_back(line);
                    }
                } else {
                    XC_LOGW("ice poll failed: %s", err.c_str());
                }
                quiet = remote.size() == before ? quiet + 1 : 0;
                if (hasReal && quiet >= kRemoteIceQuietPolls) break;
                if (!sleepMs(kRemoteIcePollMs)) break;
            }
            if (quit) return PeerOutcome::Finished;
            if (!hasReal && !remote.empty())
                XC_LOGW("no remote candidate with priority > %lu, trying what we have", kRealCandidatePriority);
        }
        XC_LOGI("collected %zu remote candidates", remote.size());
        for (const auto& c : remote) XC_LOGD("  remote cand: %s", c.c_str());
        if (remote.empty()) {
            XC_LOGW("server sent no ICE candidates");
            return PeerOutcome::RetryFresh;
        }
        std::vector<std::string> expanded = proto::expandTeredoCandidates(remote);
        if (expanded.size() != remote.size())
            for (const auto& c : expanded) XC_LOGD("  expanded cand: %s", c.c_str());
        if (!r->setRemote(answer, expanded)) {
            if (txVoice && !quit) {
                XC_LOGW("voice: answer to the voice chat offer could not be applied, retrying without voice chat");
                voiceSessionOff = true;
                return resuming ? PeerOutcome::RetryFresh : PeerOutcome::Resignal;
            }
            fail("The server's SDP answer could not be applied");
            return PeerOutcome::Finished;
        }
        if (txVoice) {
            // The answer's direction is the server's view: sendonly/inactive = it will not take our mic.
            const std::string dir = r->answerAudioDirection();
            txMicAccepted = dir == "sendrecv" || dir == "recvonly";
            XC_LOGI("voice: negotiated=%s audio=%s mic=%s", chatStreamVersion.load() >= 1 ? "yes" : "no",
                    dir.empty() ? "?" : dir.c_str(), txMicAccepted ? "accepted" : "refused");
            if (!txMicAccepted)
                XC_LOGW("voice: server answered audio %s, microphone not accepted", dir.empty() ? "(none)" : dir.c_str());
        }
        XC_LOGI("remote description set, checking connectivity");
        return eventLoop(gssv, s, *r, gen);
    }

    // ===================== voice chat renegotiation =====================

    // Worker: the chat renegotiation is due once the server offered chatStream, the handshake
    // is done and the microphone is open (official ChatStreamManager.enable()). Once per
    // transport; any failure leaves the mic on the initial sendrecv audio line.
    void maybeStartRenegotiation(GssvClient& gssv, const SessionInfo& s, WebRtc& r, uint32_t gen) {
        if (!txVoice || !txMicAccepted || !handshakeDone || renegStarted) return;
        // Party voice is received over the chat stream whether or not our mic works, so a mic
        // failure only delays the renegotiation (at most kRenegMicWaitMs), it never blocks it.
        const uint64_t now = nowMs();
        if (!renegDueAt) renegDueAt = now;
        std::shared_ptr<VoiceChat> v = currentVoice();
        const VoiceChat::State vs = v ? v->state() : VoiceChat::State::Unavailable;
        const bool micReady = vs == VoiceChat::State::Live || vs == VoiceChat::State::Muted;
        if (!micReady && now - renegDueAt < static_cast<uint64_t>(kRenegMicWaitMs)) return;
        const int cs = chatStreamVersion.load();
        if (cs < 1) {
            if (!renegNoChatLogged) {
                renegNoChatLogged = true;
                XC_LOGI("voice: server did not negotiate chatStream, sending mic on initial audio line only");
            }
            return;
        }
        renegStarted = true;
        XC_LOGI("voice: renegotiation start (chatStream=%d, mic %s)", cs, v ? voiceStateName(vs) : "none");
        std::string u, p, fp;
        if (!r.beginRenegotiation(u, p, fp)) {
            r.abortRenegotiation();  // roll back a local offer that was set but is unusable (no-op otherwise)
            XC_LOGW("voice: renegotiation failed: no local offer");
            return;
        }
        const std::string offer2 = proto::buildOffer(u, p, fp, txHome, settings.resolution, &txMic, 3);
        XC_LOGD("renegotiation offer sdp:\n%s", offer2.c_str());
        if (renegThr.joinable()) renegThr.join();  // a finished thread of an earlier transport
        renegAbort = quit.load();
        const std::string initial = txInitialAnswer;
        const std::vector<std::string> cands = txLocalCandidates;
        const std::string iceUfrag = u.empty() ? txUfrag : u;
        try {
            renegThr = std::thread([this, &gssv, s, offer2, initial, cands, iceUfrag, gen] {
                try {
                    renegotiate(gssv, s, offer2, initial, cands, iceUfrag, gen);
                } catch (const std::exception& e) {
                    XC_LOGW("voice: renegotiation thread: %s", e.what());
                    pushEvent(Event{Event::RenegResult, gen, std::string("failed: ") + e.what(), std::string()});
                }
            });
        } catch (const std::exception& e) {
            XC_LOGW("voice: renegotiation failed: cannot start thread (%s)", e.what());
            r.abortRenegotiation();
        }
    }

    // Renegotiation thread: POST the second offer, poll for its answer (bounded), report.
    // Only gssv calls (GssvClient is safe to share across threads) and the event queue.
    void renegotiate(GssvClient& gssv, const SessionInfo& s, const std::string& offer, const std::string& initial,
                     const std::vector<std::string>& cands, const std::string& iceUfrag, uint32_t gen) {
        Http::AbortScope abortOn(&renegAbort);
        const uint64_t start = nowMs();
        auto stopped = [this] { return quit.load() || renegAbort.load(); };
        auto report = [&](const std::string& why, const std::string& answer) {
            if (!stopped()) pushEvent(Event{Event::RenegResult, gen, why, answer});
        };
        std::string err;
        if (!gssv.sendSdpRenegotiation(s, offer, err)) {
            report("failed: " + (err.empty() ? std::string("offer refused") : err), std::string());
            return;
        }
        const uint64_t posted = nowMs();
        XC_LOGI("voice: renegotiation offer posted");
        int errors = 0;
        bool sawInitial = false;
        while (!stopped()) {
            // sleep in short steps so a teardown never waits for a whole poll interval
            for (int waited = 0; waited < kRenegPollMs && !stopped(); waited += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (stopped()) return;
            SdpAnswerInfo info;
            bool ready = false;
            if (!gssv.pollSdpAnswer(s, info, ready, err)) {
                XC_LOGD("voice: renegotiation poll failed: %s", err.c_str());
                if (++errors >= kMaxPollErrors) {
                    report("failed: " + err, std::string());
                    return;
                }
            } else if (ready && !info.sdp.empty()) {
                // GET /sdp keeps returning the initial answer until the second one exists; that
                // is never the reply to this offer (xbox.com keeps polling too).
                if (info.sdp == initial) {
                    sawInitial = true;
                } else {
                    XC_LOGI("voice: renegotiation answer received (%zu bytes, %llu ms)", info.sdp.size(),
                            static_cast<unsigned long long>(nowMs() - posted));
                    // xbox.com re-runs the ICE exchange after every answer, the chat renegotiation
                    // included (same candidates and ufrag). Its outcome never decides the result.
                    if (!cands.empty() && !stopped()) {
                        std::string iceErr;
                        bool iceOk = gssv.sendIce(s, cands, iceUfrag, iceErr);
                        if (iceOk && !stopped()) {
                            std::vector<std::string> got;
                            bool iceReady = false;
                            iceOk = gssv.pollIce(s, got, iceReady, iceErr);
                            if (iceOk) XC_LOGD("voice: renegotiation ice poll: %zu candidates", got.size());
                        }
                        if (iceOk)
                            XC_LOGI("voice: renegotiation ice exchange ok");
                        else
                            XC_LOGW("voice: renegotiation ice exchange failed: %s", iceErr.c_str());
                    }
                    report(std::string(), info.sdp);
                    return;
                }
            }
            if (nowMs() - start >= static_cast<uint64_t>(kRenegTimeoutMs)) {
                report(sawInitial ? "timed out (server returned only the initial answer)" : "timed out", std::string());
                return;
            }
        }
    }

    // Worker: apply (or abandon) the renegotiation answer. Never ends the stream.
    void finishRenegotiation(WebRtc& r, const Event& ev) {
        if (renegThr.joinable()) renegThr.join();  // it posted this event as its last action
        if (!ev.text.empty()) {
            if (r.finishRenegotiation(ev.text)) {
                voiceRenegotiated = true;
                XC_LOGI("voice: renegotiation ok");
                return;
            }
            r.abortRenegotiation();
            XC_LOGW("voice: renegotiation failed: answer could not be applied");
            return;
        }
        r.abortRenegotiation();
        if (ev.ch.rfind("timed out", 0) == 0)
            XC_LOGW("voice: renegotiation %s", ev.ch.c_str());
        else
            XC_LOGW("voice: renegotiation %s", ev.ch.c_str());
    }

    PeerOutcome eventLoop(GssvClient& gssv, const SessionInfo& sess, WebRtc& r, uint32_t gen) {
        const uint64_t negotiationStart = nowMs();
        uint64_t connectedAt = 0, disconnectedAt = 0, handshakeAt = 0, ackAt = 0;
        uint64_t lastSecond = nowMs(), lastLoop = nowMs();
        bool sentHandshake = false, ackPending = false, decodeResynced = false, idrWaitLogged = false;
        int inputDeadSeconds = 0;
        uint64_t reviveDue = 0, reviveFiredAt = 0, lastRevive = 0;
        int autoRevives = 0;
        // Audio counters are cumulative per AudioPlayer: prime them so the first second of a
        // re-signaled transport is not read as a fresh loss burst (green-nx pre8 death loop).
        AudioPlayer::Stats prevAudio = audioOk ? audio.stats() : AudioPlayer::Stats();
        std::string pcState = "new";
        workerTick = nowMs();

        struct TickClear {
            std::atomic<uint64_t>& t;
            ~TickClear() { t = 0; }
        } tickClear{workerTick};

        while (!quit) {
            // ---- events ----
            std::deque<Event> evs;
            {
                std::unique_lock<std::mutex> lk(evMu);
                if (events.empty())
                    evCv.wait_for(lk, std::chrono::milliseconds(20), [this] { return quit.load() || !events.empty(); });
                evs.swap(events);
            }
            if (quit) break;
            for (Event& ev : evs) {
                if (ev.gen != gen) continue;
                switch (ev.type) {
                    case Event::PcState:
                        if (ev.text != pcState) {
                            XC_LOGI("peer state: %s", ev.text.c_str());
                            pcState = ev.text;
                        }
                        if (pcState == "connected") {
                            if (!connectedAt) connectedAt = nowMs();
                            disconnectedAt = 0;
                        } else if (pcState == "disconnected" && !disconnectedAt) {
                            disconnectedAt = nowMs();
                        }
                        break;
                    case Event::ChannelOpen:
                        XC_LOGI("data channel open: %s", ev.ch.c_str());
                        break;
                    case Event::ChannelClose:
                        XC_LOGI("data channel closed: %s", ev.ch.c_str());
                        break;
                    case Event::RenegResult:
                        finishRenegotiation(r, ev);
                        break;
                    case Event::Text:
                        if (ev.ch == "message") {
                            XC_LOGD("recv [message] %s", preview(ev.text).c_str());
                            bool ack = false;
                            if (handleMessageChannel(r, ev.text, ack)) break;
                            if (ack && !handshakeDone && !ackPending) {
                                ackPending = true;
                                ackAt = nowMs();
                                setStatus("Starting the stream...");
                            }
                        } else if (ev.ch == "chat") {
                            // Voice runs over RTP; the chat channel is only logged, never written.
                            if (chatMsgs.fetch_add(1) < kChatLogInfoMessages)
                                XC_LOGI("recv [chat] %s", preview(ev.text).c_str());
                            else
                                XC_LOGD("recv [chat] %s", preview(ev.text).c_str());
                        } else {
                            XC_LOGD("recv [%s] %s", ev.ch.c_str(), preview(ev.text).c_str());
                        }
                        break;
                }
            }

            // A helper thread (keepalive stall watchdog) may have declared the session dead.
            if (state.load() == StreamState::Failed) return PeerOutcome::Finished;

            const uint64_t now = nowMs();
            // Elapsed time since t, 0 when t lies in the future. Timestamps set while handling this
            // iteration (after `now` was read) are a few µs ahead of it: plain `now - t` on uint64
            // wraps around and fires every timeout at once.
            auto since = [now](uint64_t t) -> uint64_t { return now > t ? now - t : 0; };
            // The process can be suspended (PS5 rest mode, debugger): every thread freezes while
            // the clock runs on, so a gap is not a stall. Restart the watchdog windows.
            if (now - lastLoop > 2000) {
                if (lastMediaMs) lastMediaMs = now;
                if (lastDecodeMs) lastDecodeMs = now;
            }
            lastLoop = now;
            workerTick = now;

            // ---- session end / transport death ----
            if (serverEnded) {
                std::string reason;
                {
                    std::lock_guard<std::mutex> lk(statusMu);
                    reason = serverEndReason;
                }
                endSession(reason.empty() ? "The session was ended by the server"
                                          : "The session was ended by the server (" + reason + ")");
                return PeerOutcome::Finished;
            }
            if (pcState == "failed" || pcState == "closed" ||
                (disconnectedAt && since(disconnectedAt) > kDisconnectGraceMs)) {
                if (!everStreamed && !resuming) {
                    // The session itself (and its queue slot) is still ours: renegotiate WebRTC
                    // on it before giving it up for a fresh one, which may mean queueing again.
                    XC_LOGW("WebRTC connection %s before streaming started: re-signaling", pcState.c_str());
                    return PeerOutcome::Resignal;
                }
                if (handshakeDone || resuming) {
                    XC_LOGW("WebRTC connection %s mid-stream: re-signaling", pcState.c_str());
                    return PeerOutcome::Resignal;
                }
                fail("Connection to the server was lost");
                return PeerOutcome::Finished;
            }

            // ---- data channels / handshake ----
            if (!sentHandshake && r.isChannelOpen("message")) {
                if (sendText(r, "message", proto::messageHandshake())) {
                    sentHandshake = true;
                    setStatus(resuming ? "Reconnecting..." : "Handshaking...");
                }
            }
            if (ackPending) {
                bool ready = r.isChannelOpen("control") && r.isChannelOpen("input");
                if (ready || since(ackAt) > kChannelsReadyGraceMs) {
                    if (!ready) XC_LOGW("control/input channel not open %d ms after HandshakeAck", kChannelsReadyGraceMs);
                    ackPending = false;
                    afterHandshakeAck(r, gen);
                    handshakeAt = now;
                    if (!resuming) setStatus("Waiting for video...");
                }
            }
            // Dead media path: ICE/DTLS up but SCTP channels never come up. A fresh session
            // re-rolls that server-side fault.
            if (connectedAt && !sentHandshake && since(connectedAt) > kDeadMediaPathMs) {
                XC_LOGW("connected but data channels never opened: dead media path");
                return PeerOutcome::RetryFresh;
            }
            if (!handshakeDone && since(negotiationStart) > kNegotiationTimeoutMs) {
                if (resuming || !connectedAt) return PeerOutcome::RetryFresh;
                fail("Connection timed out");
                return PeerOutcome::Finished;
            }

            // ---- voice chat renegotiation (mic open, or at most kRenegMicWaitMs after the handshake) ----
            maybeStartRenegotiation(gssv, sess, r, gen);

            // ---- first frame / streaming ----
            if (gotFrame) {
                if (state != StreamState::Streaming) setState(StreamState::Streaming);
                if (resuming) {
                    resuming = false;
                    XC_LOGI("re-signal complete, stream resumed");
                }
                setStatus("Streaming");
            } else if (handshakeDone) {
                // xCloud may start mid-GOP or drop the first request: keep asking (throttled 1/s).
                requestKeyframeInternal(false);
                if (!idrWaitLogged && since(handshakeAt) > 3000) {
                    idrWaitLogged = true;
                    XC_LOGW("still waiting for the first video frame (%u keyframe requests)", keyframeReqs.load());
                }
                if (since(handshakeAt) > kFirstFrameTimeoutMs) {
                    if (resuming) return PeerOutcome::RetryFresh;
                    if (!everStreamed) {
                        XC_LOGW("no video %d ms after the handshake: re-signaling the session", kFirstFrameTimeoutMs);
                        return PeerOutcome::Resignal;
                    }
                    fail("No video received from the server");
                    return PeerOutcome::Finished;
                }
            }

            // ---- media watchdogs (after the first frame) ----
            if (gotFrame) {
                uint64_t lm = lastMediaMs.load();
                if (lm && now > lm && now - lm > kMediaStallMs) {
                    XC_LOGW("no video or audio for %d ms", kMediaStallMs);
                    return PeerOutcome::Resignal;
                }
                uint64_t ld = lastDecodeMs.load();
                if (ld && now > ld) {
                    if (now - ld > kDecodeResyncMs && !decodeResynced) {
                        // Units going in, nothing coming out: wipe the assembler and gate on a clean IDR.
                        decodeResynced = true;
                        {
                            std::lock_guard<std::mutex> lk(depackMu);
                            depack.reset();
                            waitKeyframe = true;
                        }
                        {
                            std::lock_guard<std::mutex> lk(frameMu);
                            frameQ.clear();
                        }
                        requestKeyframeInternal(true);
                        XC_LOGW("video decode stalled %d ms: depacketizer reset, waiting for a clean IDR",
                                kDecodeResyncMs);
                    } else if (now - ld <= kDecodeResyncMs) {
                        decodeResynced = false;
                    }
                    if (now - ld > kDecodeStallMs) {
                        fail("Video stalled for 15 s, please start the stream again");
                        return PeerOutcome::Finished;
                    }
                }
            }

            // ---- once per second: input health, revive, telemetry ----
            if (now - lastSecond >= 1000) {
                lastSecond = now;
                uint32_t sent = inSentSec.exchange(0), failed = inFailSec.exchange(0), rx = inRxSec.exchange(0);
                if (handshakeDone && gotFrame) {
                    StreamStats ss;
                    {
                        std::lock_guard<std::mutex> lk(statsMu);
                        ss = st;
                    }
                    XC_LOGD("stats| %dx%d %.1ffps %.0fkbps dec=%.1fms loss=%.1f%% abuf=%dms in: sent=%u fail=%u rx=%u",
                            ss.width, ss.height, ss.fps, ss.bitrateKbps, ss.decodeMs, ss.lossPercent,
                            ss.audioBufferMs, sent, failed, rx);
                    // Dead data channel (green-nx #61): SCTP association gone, media still flowing.
                    if (sent == 0 && failed > 0) {
                        if (++inputDeadSeconds >= kInputDeadSeconds) {
                            XC_LOGW("input channel dead for %d s: re-signaling", kInputDeadSeconds);
                            return PeerOutcome::Resignal;
                        }
                    } else {
                        inputDeadSeconds = 0;
                    }
                    // A loss burst can wedge the server's input pipeline while our sends still
                    // succeed: after it settles, re-announce the pad (bounded churn).
                    // Any shape of media disruption counts: controllers also died on pure latency
                    // spikes where only the audio ring (drops/underruns) showed the hit.
                    AudioPlayer::Stats as = audioOk ? audio.stats() : AudioPlayer::Stats();
                    auto delta = [](uint64_t cur, uint64_t prev) { return cur >= prev ? cur - prev : 0; };
                    bool burst = videoLossPct.load() >= 10 || delta(as.lost, prevAudio.lost) >= 3 ||
                                 delta(as.droppedMs, prevAudio.droppedMs) >= 150 ||
                                 delta(as.underruns, prevAudio.underruns) >= 3;
                    prevAudio = as;
                    if (burst && !reviveDue && !reviveFiredAt && autoRevives < 3 &&
                        (!lastRevive || now - lastRevive > 45000))
                        reviveDue = now + 2000;
                    if (reviveDue && now >= reviveDue) {
                        reviveDue = 0;
                        lastRevive = now;
                        ++autoRevives;
                        reviveInput(r, "after media disruption");
                        reviveFiredAt = now;
                    }
                    if (reviveFiredAt) {
                        uint64_t rxl = inputRxLastMs.load();
                        if (rxl > reviveFiredAt) {
                            XC_LOGI("input revive: server input traffic is back");
                            reviveFiredAt = 0;
                        } else if (now - reviveFiredAt > 8000) {
                            reviveFiredAt = 0;  // quiet games never send any; not judged
                        }
                    }
                }
            }
        }
        return PeerOutcome::Finished;  // stop requested
    }

    // ===================== session (gssv) =====================

    enum class AttemptResult { Done, RetryFresh, SessionFailed };

    AttemptResult runAttempt(GssvClient& gssv, const std::string& base, const std::string& token,
                             const Settings& gssvSettings, SessionInfo& s, std::string& sessionError, int& resignals,
                             uint64_t& resignalWindow) {
        bool connected = false;
        std::string lastState;
        uint64_t stateSince = nowMs(), nextWaitQuery = 0;
        int pollErrors = 0;
        std::unique_ptr<Keepalive> ka;
        std::string err;

        while (!quit) {
            if (state.load() == StreamState::Failed) return AttemptResult::Done;
            if (!gssv.pollState(s, err)) {
                XC_LOGW("session state poll failed: %s", err.c_str());
                if (++pollErrors >= kMaxPollErrors) {
                    if (resuming) return AttemptResult::RetryFresh;
                    fail("Lost contact with the streaming service: " + err);
                    return AttemptResult::Done;
                }
                if (!sleepMs(kStatePollMs)) break;
                continue;
            }
            pollErrors = 0;
            const uint64_t now = nowMs();
            if (s.state != lastState) {
                XC_LOGI("session state: %s", s.state.c_str());
                lastState = s.state;
                stateSince = now;
            }

            if (s.state == "WaitingForResources") {
                if (!resuming) setState(StreamState::Queued);
                if (kind == SessionKind::Cloud && now >= nextWaitQuery) {
                    nextWaitQuery = now + kWaitTimeQueryMs;
                    queueWaitSec = gssv.waitTimeSeconds(target);
                }
                int w = queueWaitSec;
                if (!resuming) {
                    // Waiting is the expected path here: the server moves us to ReadyToConnect
                    // when a slot frees up. Show the estimate plus how long we have waited.
                    const uint64_t waited = (now - stateSince) / 1000;
                    char el[16];
                    std::snprintf(el, sizeof el, "%llu:%02llu", static_cast<unsigned long long>(waited / 60),
                                  static_cast<unsigned long long>(waited % 60));
                    setStatus((w > 0 ? "Waiting in queue, about " + std::to_string((w + 59) / 60) + " min left"
                                     : std::string("Waiting in queue for a free server")) +
                              " (waiting " + el + ")");
                }
            } else {
                queueWaitSec = -1;
            }

            if (s.state == "ReadyToConnect" && !connected) {
                if (!resuming) setState(StreamState::Provisioning);
                setStatus(resuming ? "Reconnecting..." : "Authenticating...");
                std::string lpt;
                if (!auth.fetchLpt(lpt, err) || !gssv.connect(s, lpt, err)) {
                    if (resuming) return AttemptResult::RetryFresh;
                    fail("Session authentication failed: " + err);
                    return AttemptResult::Done;
                }
                connected = true;
            } else if (s.state == "Provisioned") {
                if (!ka) {
                    ka.reset(new Keepalive);
                    try {
                        ka->thr = std::thread(&Impl::keepaliveMain, this, base, token, gssvSettings, s, &ka->stop);
                    } catch (const std::exception& e) {
                        fail(std::string("Could not start the keepalive thread: ") + e.what());
                        return AttemptResult::Done;
                    }
                }
                PeerOutcome o = runPeer(gssv, s);
                if (o == PeerOutcome::Finished) return AttemptResult::Done;
                if (o == PeerOutcome::RetryFresh) return AttemptResult::RetryFresh;
                // Same-session re-signal (green-nx #61): the backend accepts a fresh SDP/ICE
                // exchange into the same sessionPath. Budgeted over a moving window.
                if (quit) break;
                uint64_t t = nowMs();
                if (!resignalWindow || t - resignalWindow > static_cast<uint64_t>(kResignalWindowMs)) {
                    resignalWindow = t;
                    resignals = 0;
                }
                if (++resignals > kMaxResignals) {
                    fail("The connection keeps dropping, please start the stream again");
                    return AttemptResult::Done;
                }
                XC_LOGI("re-signaling the same session (%d/%d in this window)", resignals, kMaxResignals);
                resuming = true;
                setStatus("Reconnecting...");
                connected = false;
                stateSince = nowMs();
                lastState.clear();
                continue;  // poll state again right away
            } else if (s.state == "Failed") {
                if (resuming) {
                    XC_LOGW("resumed session unrecoverable: %s %s", s.errorCode.c_str(), s.errorMessage.c_str());
                    return AttemptResult::RetryFresh;
                }
                sessionError = s.errorCode;
                if (!s.errorMessage.empty()) sessionError += (sessionError.empty() ? "" : ": ") + s.errorMessage;
                if (sessionError.empty()) sessionError = "unknown error";
                return AttemptResult::SessionFailed;
            } else if (s.state != "WaitingForResources") {
                if (!resuming) {
                    setState(StreamState::Provisioning);
                    setStatus("Preparing your session...");
                }
            }

            uint64_t cap = resuming ? kResumeStateCapMs
                                    : (s.state == "WaitingForResources" ? kQueueCapMs : kStateCapMs);
            if (now - stateSince > cap) {
                if (resuming) return AttemptResult::RetryFresh;
                sessionError.clear();  // timeout
                return AttemptResult::SessionFailed;
            }
            if (!sleepMs(kStatePollMs)) break;
        }
        return AttemptResult::Done;
    }

    struct Offering {
        std::string name, base, token;
    };

    // Offerings to try for this session, in order, from a token snapshot.
    std::vector<Offering> buildOffers(const Tokens& t) const {
        std::vector<Offering> offers;
        if (kind == SessionKind::Home) {
            if (!t.xhomeGs.empty() && !t.xhomeBase.empty()) offers.push_back({"xhome", t.xhomeBase, t.xhomeGs});
        } else {
            const bool haveF2p = !t.xcloudF2pGs.empty() && !t.xcloudF2pBase.empty();
            if (f2pOnly && haveF2p) offers.push_back({"xgpuwebf2p", t.xcloudF2pBase, t.xcloudF2pGs});
            if (!t.xcloudGs.empty() && !t.xcloudBase.empty()) offers.push_back({"xgpuweb", t.xcloudBase, t.xcloudGs});
            if (!f2pOnly && (settings.f2pFallback || offers.empty()) && haveF2p)
                offers.push_back({"xgpuwebf2p", t.xcloudF2pBase, t.xcloudF2pGs});
        }
        return offers;
    }

    void runSession() {
        const bool home = kind == SessionKind::Home;
        setState(StreamState::Starting);
        setStatus(home ? "Signing in to your Xbox..." : "Signing in to xCloud...");
        std::string err;
        if (!auth.ensureFresh(err)) {
            fail("Sign-in failed: " + err);
            return;
        }
        if (quit) return;

        std::vector<Offering> offers = buildOffers(cfg.tokens());
        if (offers.empty()) {
            fail(home ? "Remote play is not available for this account"
                      : "Cloud gaming is not available for this account");
            return;
        }
        // gssv requests of an xhome session always carry the android/720 device fingerprint: the
        // console agent answers windows/tizen with AgentCommandError (green-nx engine.cpp builds
        // the home session with QualityTier::P720). The in-band resolution / capabilities and the
        // SDP offer still follow the user's setting.
        Settings gssvSettings = settings;
        gssvSettings.resolution = gssv::deviceTierFor(home, settings.resolution);

        size_t offerIdx = 0;
        std::vector<bool> cleaned(offers.size(), false);
        bool authRetried = false;
        // Home: a play request against a sleeping console wakes it but fails with
        // AgentCommandError while it boots its streaming service, and a freshly booted
        // console sits in WaitingForServerToRegister for a while. Cloud gets one retry to
        // re-roll a dead media path.
        const int attempts = home ? 6 : 2;
        bool registering = false;
        int resignals = 0;
        uint64_t resignalWindow = 0;

        for (int attempt = 0; attempt < attempts && !quit; ++attempt) {
            const Offering off = offers[offerIdx];
            if (attempt > 0) {
                std::string of = " (attempt " + std::to_string(attempt + 1) + " of " + std::to_string(attempts) + ")";
                setState(StreamState::Starting);
                setStatus(registering ? "Your console is still registering..." + of
                          : home      ? "Waking your console..." + of
                                      : "Retrying the connection...");
                // Home consoles need ~5 s to boot streaming; cloud needs the dead session's
                // teardown to release the account's slot.
                if (!sleepMs(home ? 5000 : 3000)) break;
            }
            GssvClient gssv(off.base, off.token, gssvSettings);
            if (!cleaned[offerIdx]) {
                setStatus("Cleaning up old sessions...");
                gssv.cleanupActive(kind);
                cleaned[offerIdx] = true;
                if (quit) break;
            }
            setState(StreamState::Starting);
            setStatus("Requesting a session...");
            XC_LOGI("requesting %s session via %s (attempt %d of %d, device tier %s)", home ? "home" : "cloud",
                    off.name.c_str(), attempt + 1, attempts, gssvSettings.resolution.c_str());
            SessionInfo s;
            if (!gssv.play(kind, target, s, err)) {
                if (quit) break;
                if (gssv.authRejected()) {
                    // The stored gsToken looked valid but was refused (revoked, entitlement
                    // change, wall clock jumped): re-authorize once and start over.
                    auth.invalidateStreaming();
                    if (authRetried) {
                        fail("Xbox refused the sign-in for streaming. Please try again or sign in again.");
                        return;
                    }
                    authRetried = true;
                    setStatus(home ? "Signing in to your Xbox again..." : "Signing in to xCloud again...");
                    std::string aerr;
                    if (!auth.ensureFresh(aerr)) {
                        fail("Sign-in failed: " + aerr);
                        return;
                    }
                    if (quit) break;
                    offers = buildOffers(cfg.tokens());
                    if (offers.empty()) {
                        fail(home ? "Remote play is not available for this account"
                                  : "Cloud gaming is not available for this account");
                        return;
                    }
                    offerIdx = 0;
                    cleaned.assign(offers.size(), false);
                    --attempt;  // does not count as a failed attempt
                    continue;
                }
                std::string all = err + " " + s.errorCode + " " + s.errorMessage;
                if (contains(all, "OfferingDoesNotContainTitle") && offerIdx + 1 < offers.size()) {
                    ++offerIdx;
                    XC_LOGI("title not in %s, retrying with %s", off.name.c_str(), offers[offerIdx].name.c_str());
                    --attempt;  // does not count as a failed attempt
                    continue;
                }
                fail("Could not start the session: " + err);
                return;
            }
            XC_LOGI("session created: %s", s.sessionPath.c_str());

            std::string sessionError;
            resuming = false;
            AttemptResult res =
                runAttempt(gssv, off.base, off.token, gssvSettings, s, sessionError, resignals, resignalWindow);
            resuming = false;
            closePeer();
            XC_LOGI("stopping session %s", s.sessionPath.c_str());
            {
                Http::AbortScope mustRun(nullptr);  // the DELETE has to go out even when cancelled
                // ... but bounded: a user waiting on stop (or app exit) must not sit out 20 s.
                gssv.stop(s, quit ? kQuitDeleteTimeoutSec : kDeleteTimeoutSec);
            }
            if (gssv.authRejected()) auth.invalidateStreaming();  // re-authorize before the next one
            if (quit || res == AttemptResult::Done) return;

            if (res == AttemptResult::RetryFresh) {
                if (attempt == attempts - 1) {
                    fail(everStreamed ? "The connection to the server was lost"
                                      : "The server's media connection never came up");
                    return;
                }
                XC_LOGW("retrying with a fresh session");
                continue;
            }
            // SessionFailed
            if (sessionError.empty()) {
                fail("Timed out waiting for a session");
                return;
            }
            XC_LOGW("session attempt %d failed: %s", attempt + 1, sessionError.c_str());
            if (contains(sessionError, "OfferingDoesNotContainTitle") && offerIdx + 1 < offers.size()) {
                ++offerIdx;
                --attempt;
                continue;
            }
            registering = contains(sessionError, "WaitingForServerToRegister");
            bool consoleNotReady = registering || contains(sessionError, "AgentCommandError");
            if (!consoleNotReady || attempt == attempts - 1) {
                fail(consoleNotReady ? "Your console never finished registering for remote play. Turn Remote "
                                       "Features off and on, or restart the console, then try again."
                                     : "Session failed: " + sessionError);
                return;
            }
        }
    }

    void workerMain() {
        // stop() sets quit: in-flight gssv/auth requests on this thread then abort within ~1 s
        // instead of running into their timeouts.
        Http::AbortScope abortOnQuit(&quit);
        guarded("session", [this] { runSession(); });
        try {
            closePeer();
        } catch (const std::exception& e) {
            XC_LOGE("stream: closing the peer connection threw: %s", e.what());
        }
        queueWaitSec = -1;
        if (audioOk) audio.setMuted(true);
        workerDone = true;
        frameCv.notify_all();
        XC_LOGI("stream worker finished (%s)", streamStateName(state.load()));
    }

    // Raise quit and wake every waiter. Setting it under evMu closes the lost-wakeup window of
    // sleepMs()/eventLoop (predicate checked, then wait entered after the notify).
    void signalQuit() {
        {
            std::lock_guard<std::mutex> lk(evMu);
            quit = true;
        }
        renegAbort = true;  // in-flight renegotiation requests abort too
        evCv.notify_all();
        {
            std::lock_guard<std::mutex> lk(frameMu);
        }
        frameCv.notify_all();
    }

    void joinThreads() {
        for (std::thread* t : {&workerThr, &decodeThr, &inputThr, &rtcpThr})
            if (t->joinable()) t->join();
    }

    // Caller holds lifeMu. Reports Idle/"Stopped" afterwards only while startGen still equals
    // publishGen (checked under statusMu, like start() bumps it): a newer start() owns the
    // public state and must not be overwritten. kNoPublish: never report.
    static constexpr uint64_t kNoPublish = ~uint64_t(0);
    void stopLocked(uint64_t publishGen) {
        if (running) {
            XC_LOGI("stream stop requested");
            signalQuit();
            // The worker closes the PeerConnection and DELETEs the gssv session itself.
            joinThreads();
            closePeer();  // no-op unless the worker bailed out early
            if (audioOk) audio.close();
            audioOk = false;
            if (decoderOk.exchange(false)) decoder.close();
            {
                std::lock_guard<std::mutex> lk(frameMu);
                frameQ.clear();
            }
            running = false;
            workerDone = true;
        }
        std::lock_guard<std::mutex> lk(statusMu);
        StreamState s = state.load();
        if (publishGen == startGen.load() && s != StreamState::Failed && s != StreamState::Ended) {
            publishState(StreamState::Idle);
            status = "Stopped";
        }
    }

    // ---- lifecycle thread ----

    void lifeLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(lifeQMu);
                lifeQCv.wait(lk, [this] { return lifeQuit || !lifeQ.empty(); });
                if (lifeQ.empty()) return;  // lifeQuit and nothing left
                job = std::move(lifeQ.front());
                lifeQ.pop_front();
            }
            try {
                job();
            } catch (const std::exception& e) {
                XC_LOGE("stream: lifecycle job failed: %s", e.what());
                fail(std::string("Internal error: ") + e.what());
            }
        }
    }

    // Queue a lifecycle job (starts the thread on first use). False if it cannot run.
    bool postLife(std::function<void()> job) {
        std::lock_guard<std::mutex> lk(lifeQMu);
        if (lifeClosing) return false;  // stop() is shutting the thread down
        if (!lifeThr.joinable()) {
            lifeQuit = false;
            try {
                lifeThr = std::thread(&Impl::lifeLoop, this);
            } catch (const std::exception& e) {
                XC_LOGE("stream: cannot start the lifecycle thread: %s", e.what());
                return false;
            }
        }
        lifeQ.push_back(std::move(job));
        lifeQCv.notify_one();
        return true;
    }

    // Drop queued jobs, let the running one finish, join the thread.
    void shutdownLife() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(lifeQMu);
            lifeQ.clear();
            lifeQuit = true;
            lifeClosing = true;
            t = std::move(lifeThr);
        }
        lifeQCv.notify_all();
        if (t.joinable()) t.join();
        std::lock_guard<std::mutex> lk(lifeQMu);
        lifeClosing = false;
    }

    struct StartParams {
        SessionKind kind;
        std::string target;
        bool f2pOnly;
    };

    // Lifecycle thread: release the previous session, then bring up a new one (unless a newer
    // start/stop superseded this request meanwhile).
    void doStart(const StartParams& p, uint64_t gen) {
        std::lock_guard<std::mutex> life(lifeMu);
        if (running) stopLocked(kNoPublish);
        if (gen != startGen.load()) return;  // cancelled while the previous session wound down

        kind = p.kind;
        target = p.target;
        f2pOnly = p.kind == SessionKind::Cloud && p.f2pOnly;
        settings = cfg.settings();
        installId = cfg.installId();
        quit = false;
        queueWaitSec = -1;
        serverEnded = false;
        everStreamed = false;
        resuming = false;
        handshakeDone = false;
        pcConnected = false;
        gotFrame = false;
        videoBytes = 0;
        framesDecoded = 0;
        decodeUsAvg = 0;
        decodeErrors = 0;
        keyframeReqs = 0;
        queueFlushes = 0;
        workerTick = 0;
        videoLossPct = 0;
        frameW = 0;
        frameH = 0;
        voiceSessionOff = false;
        chatRxPackets = 0;
        {
            std::lock_guard<std::mutex> lk(inputMu);
            serializer = proto::InputSerializer();
        }
        {
            std::lock_guard<std::mutex> lk(padMu);
            pad = GamepadState();
        }
        {
            std::lock_guard<std::mutex> lk(vibMu);
            vibPending = false;
            vibLogged = false;
        }
        {
            std::lock_guard<std::mutex> lk(statsMu);
            st = StreamStats();
        }
        {
            std::lock_guard<std::mutex> lk(statusMu);
            serverEndReason.clear();
        }
        setState(StreamState::Starting);
        setStatus("Starting...");

        XC_LOGI("stream start: %s %s (res %s, %d kbps, voice chat %s%s)", kind == SessionKind::Home ? "home" : "cloud",
                target.c_str(), settings.resolution.c_str(), settings.bitrateKbps, settings.voiceChat ? "on" : "off",
                settings.voiceChat && micMuted.load() ? ", mic muted" : "");

        if (!decoder.init(4)) {
            fail("The video decoder could not be initialised");
            active = false;
            return;
        }
        decoderOk = true;
        audioOk = audio.init();
        if (audioOk)
            audio.setMuted(false);
        else
            XC_LOGW("audio output unavailable, streaming without sound");

        workerDone = false;
        running = true;
        try {
            workerThr = std::thread(&Impl::workerMain, this);
            decodeThr = std::thread(&Impl::decodeMain, this);
            inputThr = std::thread(&Impl::inputMain, this);
            rtcpThr = std::thread(&Impl::rtcpMain, this);
        } catch (const std::exception& e) {
            // Thread limit (std::system_error): unwind whatever started and fail cleanly.
            XC_LOGE("stream: cannot start the session threads: %s", e.what());
            fail(std::string("Could not start the stream threads: ") + e.what());
            signalQuit();
            workerDone = true;
            joinThreads();
            closePeer();
            if (audioOk) audio.close();
            audioOk = false;
            if (decoderOk.exchange(false)) decoder.close();
            running = false;
            active = false;
        }
    }
};

// =============================================================================

Streamer::Streamer(Config& cfg, Auth& auth) : d_(new Impl(cfg, auth)) {}
Streamer::~Streamer() { stop(); }

bool Streamer::start(SessionKind kind, const std::string& titleOrServerId, bool f2pOnly) {
    Impl& d = *d_;
    // An active session: requested and not stopped, unless it already finished on its own
    // (Ended/Failed) - that one is released first by doStart().
    if (d.active && !(d.running && d.workerDone)) {
        XC_LOGW("stream start ignored: a session is already running");
        return false;
    }
    uint64_t gen;
    {
        // Publish Starting now: the UI must not read a previous session's Failed/Ended state
        // while the lifecycle thread is still releasing it (see stopLocked's publishGen).
        std::lock_guard<std::mutex> lk(d.statusMu);
        gen = ++d.startGen;
        d.status = "Preparing...";
        d.publishState(StreamState::Starting);
    }
    d.active = true;
    Impl::StartParams p{kind, titleOrServerId, f2pOnly};
    if (!d.postLife([&d, p, gen] { d.doStart(p, gen); })) {
        d.active = false;
        {
            std::lock_guard<std::mutex> lk(d.statusMu);
            d.status = "Could not start the stream (out of threads)";
        }
        d.publishState(StreamState::Failed);
        return false;
    }
    return true;
}

void Streamer::stopAsync() {
    Impl& d = *d_;
    const uint64_t gen = ++d.startGen;  // cancels a queued start
    d.active = false;
    if (d.running) d.signalQuit();      // the worker starts aborting right away
    d.postLife([&d, gen] {
        std::lock_guard<std::mutex> life(d.lifeMu);
        d.stopLocked(gen);
    });
}

StreamState Streamer::state() const { return d_->state.load(); }

std::string Streamer::statusText() const {
    std::lock_guard<std::mutex> lk(d_->statusMu);
    return d_->status;
}

// gssv exposes no queue position, only an estimated wait (see queueWaitSeconds()).
int Streamer::queuePosition() const { return -1; }

int Streamer::queueWaitSeconds() const {
    return d_->state.load() == StreamState::Queued ? d_->queueWaitSec.load() : -1;
}

void Streamer::setGamepad(const GamepadState& st) {
    std::lock_guard<std::mutex> lk(d_->padMu);
    d_->pad = st;
}

bool Streamer::takeVibration(Vibration& out) {
    std::lock_guard<std::mutex> lk(d_->vibMu);
    if (!d_->vibPending) return false;
    out = d_->vib;
    d_->vibPending = false;
    return true;
}

AVFrame* Streamer::currentFrame() {
    if (!d_->decoderOk) return nullptr;
    AVFrame* f = d_->decoder.latest();
    if (f && f->width > 0 && f->height > 0) {
        d_->frameW = f->width;
        d_->frameH = f->height;
    }
    return f;
}

StreamStats Streamer::stats() const {
    std::lock_guard<std::mutex> lk(d_->statsMu);
    StreamStats s = d_->st;
    if (d_->frameW) {
        s.width = d_->frameW;
        s.height = d_->frameH;
    }
    return s;
}

void Streamer::requestKeyframe() { d_->requestKeyframeInternal(false); }

void Streamer::setMicMuted(bool muted) {
    Impl& d = *d_;
    d.micMuted = muted;
    if (std::shared_ptr<VoiceChat> v = d.currentVoice()) v->setMuted(muted);
}

bool Streamer::micMuted() const { return d_->micMuted.load(); }

MicState Streamer::micState() const {
    std::shared_ptr<VoiceChat> v = d_->currentVoice();
    if (!v) return MicState::Off;
    switch (v->state()) {
        case VoiceChat::State::Unavailable: return MicState::Unavailable;
        case VoiceChat::State::Muted: return MicState::Muted;
        case VoiceChat::State::Live: return MicState::Live;
        case VoiceChat::State::Off:
        case VoiceChat::State::Starting: return MicState::Off;
    }
    return MicState::Off;
}

float Streamer::micLevel() const {
    std::shared_ptr<VoiceChat> v = d_->currentVoice();
    return v ? v->level() : 0.0f;
}

void Streamer::stop() {
    Impl& d = *d_;
    const uint64_t gen = ++d.startGen;
    d.active = false;
    if (d.running) d.signalQuit();
    d.shutdownLife();  // queued starts are dropped; a running job finishes first
    std::lock_guard<std::mutex> life(d.lifeMu);
    d.stopLocked(gen);
}

}  // namespace xc
