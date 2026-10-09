// Nubix — libdatachannel wrapper.
//
// PeerConnection layout follows GreenOvercast (src/session/webrtc_session.zig, MPL-2.0): recvonly
// H.264 track with mid "video", then Opus with mid "audio", then the data channels (whose m-line
// therefore gets mid "0"), all created before the offer; ICE gathering is awaited, the ICE
// credentials and DTLS fingerprint are taken from libdatachannel's own local description, and
// the server answer is applied verbatim with remote candidates on mid "0".
//
// Voice chat (optional): the audio track becomes sendrecv and announces a microphone SSRC; Opus
// frames go out as raw RTP on that track (no media handler), with an RTCP SR + SDES for the mic
// SSRC once per second. Incoming PT-111 audio is split into the game stream (answer a=ssrc, else
// the first packet; never overwritten except after 2 s of silence) and one chat/party voice stream
// (any other SSRC), delivered through onAudioRtp / onChatAudioRtp. A second offer/answer round
// (chat media-stream renegotiation, xbox.com ChatStreamManager) reuses the PeerConnection.
//
// RTCP feedback (PLI with sender SSRC 0, generic NACK, compound RR+SDES with LSR/DLSR, REMB) is
// built by hand following green-nx deps/patches/libpeer-switch.patch (GPL-3.0) and sent through
// the video track: a track without media handler passes RTCP straight to the SRTP transport
// regardless of its direction (libdatachannel impl/track.cpp Track::outgoing).
//
// Incoming media: a PeerConnection-wide media handler sees every decrypted packet first. It
// parses sender reports, latches the media SSRCs by payload type, and delivers RTP whose SSRC the
// answer did not announce (libdatachannel would drop it — home consoles omit a=ssrc for audio,
// see green-nx). Everything else is delivered through the tracks' onMessage callbacks.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Portions derived from GreenOvercast (https://github.com/Producdevity/GreenOvercast), MPL-2.0, used here under GPL-3.0 (MPL-2.0 section 3.3).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "stream/webrtc.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

#include "core/log.hpp"
#include "core/protocol.hpp"

namespace xc {

namespace {

constexpr uint8_t kDefaultVideoPt = 102;
constexpr uint8_t kDefaultAudioPt = 111;
const char* const kStunServer = "stun:stun.l.google.com:19302";
// Game audio SSRC silent this long while another PT-111 SSRC flows -> the other one becomes game.
constexpr int64_t kGameTakeoverMs = 2000;
// A different chat SSRC replaces the latched one only after it was silent this long (one voice
// stream at a time; packets of a concurrent extra stream are dropped instead of interleaved).
constexpr int64_t kChatSwitchMs = 500;
constexpr int64_t kMicSrIntervalMs = 1000;
constexpr int64_t kMicLogIntervalMs = 10000;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Channel label / sub-protocol pairs (as in green-nx; reliable + ordered).
struct ChannelSpec {
    const char* label;
    const char* protocol;
};
constexpr ChannelSpec kChannels[] = {
    {"chat", "chatV1"},
    {"control", "controlV1"},
    {"input", "1.0"},
    {"message", "messageV1"},
};

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void set16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = static_cast<uint8_t>(x >> 8);
    v[at + 1] = static_cast<uint8_t>(x);
}

bool isRtcpPacket(const uint8_t* p, size_t n) {
    if (n < 8) return false;
    const uint8_t pt = p[1] & 0x7F;
    return pt >= 64 && pt <= 95;  // RFC 5761 §4
}

const char* stateName(rtc::PeerConnection::State s) {
    switch (s) {
    case rtc::PeerConnection::State::New: return "new";
    case rtc::PeerConnection::State::Connecting: return "connecting";
    case rtc::PeerConnection::State::Connected: return "connected";
    case rtc::PeerConnection::State::Disconnected: return "disconnected";
    case rtc::PeerConnection::State::Failed: return "failed";
    case rtc::PeerConnection::State::Closed: return "closed";
    }
    return "unknown";
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

void initRtcLoggerOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        const rtc::LogLevel lvl = logLevel() == LogLevel::Debug ? rtc::LogLevel::Info : rtc::LogLevel::Warning;
        rtc::InitLogger(lvl, [](rtc::LogLevel level, std::string message) {
            LogLevel l = LogLevel::Debug;
            switch (level) {
            case rtc::LogLevel::Fatal:
            case rtc::LogLevel::Error: l = LogLevel::Error; break;
            case rtc::LogLevel::Warning: l = LogLevel::Warn; break;
            case rtc::LogLevel::Info: l = LogLevel::Info; break;
            default: l = LogLevel::Debug; break;
            }
            log(l, "rtc: %s", message.c_str());
        });
    });
}

uint32_t randomSsrc() {
    std::random_device rd;
    uint32_t v = 0;
    while (v == 0) v = (static_cast<uint32_t>(rd()) << 1) ^ static_cast<uint32_t>(rd());
    return v;
}

uint32_t randomU32() {
    std::random_device rd;
    return (static_cast<uint32_t>(rd()) << 1) ^ static_cast<uint32_t>(rd());
}

const char* directionName(rtc::Description::Direction d) {
    switch (d) {
    case rtc::Description::Direction::SendOnly: return "sendonly";
    case rtc::Description::Direction::RecvOnly: return "recvonly";
    case rtc::Description::Direction::SendRecv: return "sendrecv";
    case rtc::Description::Direction::Inactive: return "inactive";
    default: return "unknown";
    }
}

// What the server answer says about the media sections.
struct AnswerInfo {
    std::set<uint8_t> videoPts, audioPts;
    std::set<uint32_t> videoSsrcs, audioSsrcs;
    uint32_t firstAudioSsrc = 0;  // first a=ssrc of the audio section, in SDP order
};

AnswerInfo parseAnswer(const std::string& sdp) {
    AnswerInfo info;
    std::istringstream in(sdp);
    std::string line;
    enum class Sec { None, Video, Audio, Other } sec = Sec::None;
    std::string kind;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("m=", 0) == 0) {
            kind = line.substr(2, line.find(' ') == std::string::npos ? std::string::npos : line.find(' ') - 2);
            sec = kind == "video" ? Sec::Video : kind == "audio" ? Sec::Audio : Sec::Other;
            continue;
        }
        if (sec != Sec::Video && sec != Sec::Audio) continue;
        if (line.rfind("a=mid:", 0) == 0) {
            const std::string mid = line.substr(6);
            if (mid == "video") sec = Sec::Video;
            else if (mid == "audio") sec = Sec::Audio;
        } else if (line.rfind("a=rtpmap:", 0) == 0) {
            const size_t sp = line.find(' ');
            if (sp == std::string::npos) continue;
            const int pt = std::atoi(line.substr(9, sp - 9).c_str());
            std::string codec = line.substr(sp + 1);
            std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) { return std::tolower(c); });
            if (pt < 0 || pt > 127) continue;
            if (sec == Sec::Video && codec.rfind("h264/", 0) == 0) info.videoPts.insert(static_cast<uint8_t>(pt));
            if (sec == Sec::Audio && codec.rfind("opus/", 0) == 0) info.audioPts.insert(static_cast<uint8_t>(pt));
        } else if (line.rfind("a=ssrc:", 0) == 0) {
            const uint32_t ssrc = static_cast<uint32_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
            if (ssrc == 0) continue;
            (sec == Sec::Video ? info.videoSsrcs : info.audioSsrcs).insert(ssrc);
            if (sec == Sec::Audio && info.firstAudioSsrc == 0) info.firstAudioSsrc = ssrc;
        }
    }
    return info;
}

// State shared with every libdatachannel callback. Callbacks hold a shared_ptr to it, never to
// WebRtc, and are gated by `mu` + `closed` so that none fires after close() returned.
struct Shared {
    std::recursive_mutex mu;  // held while a user callback runs; recursive so callbacks may call back in
    bool closed = false;
    WebRtc::Callbacks cb;

    std::atomic<uint32_t> videoSsrc{0}, audioSsrc{0};  // audioSsrc = game audio
    std::atomic<uint32_t> chatSsrc{0};                   // separate chat/party voice stream

    // Game vs chat audio demux state (routeAudio).
    std::mutex audioMu;
    int64_t gameLastRxMs = 0;
    uint64_t gameRxPackets = 0;
    int64_t chatLastRxMs = 0;
    uint64_t chatDropped = 0;
    int64_t chatDropLogMs = 0;

    std::mutex infoMu;  // answer-derived demux info
    std::set<uint8_t> videoPts{kDefaultVideoPt}, audioPts{kDefaultAudioPt};
    std::set<uint32_t> routable;  // SSRCs libdatachannel routes to a track (announced by a=ssrc)
    std::set<uint32_t> answerVideoSsrcs, answerAudioSsrcs;

    std::mutex gatherMu;
    std::condition_variable gatherCv;
    bool gatherDone = false;
    std::vector<std::string> trickled;

    enum class Kind { Unknown, Video, Audio };
    Kind classifyPt(uint8_t pt) {
        std::lock_guard<std::mutex> lk(infoMu);
        if (videoPts.count(pt)) return Kind::Video;
        if (audioPts.count(pt)) return Kind::Audio;
        return Kind::Unknown;
    }
    bool isRoutable(uint32_t ssrc) {
        std::lock_guard<std::mutex> lk(infoMu);
        return routable.count(ssrc) != 0;
    }
    Kind classifySsrc(uint32_t ssrc) {
        if (ssrc != 0 && ssrc == videoSsrc.load()) return Kind::Video;
        const uint32_t game = audioSsrc.load();
        if (ssrc != 0 && ssrc == game) return Kind::Audio;
        // The chat/party voice SSRC: the streamer tells it apart from game audio by SSRC.
        if (ssrc != 0 && ssrc == chatSsrc.load()) return Kind::Audio;
        std::lock_guard<std::mutex> lk(infoMu);
        if (answerVideoSsrcs.count(ssrc)) return Kind::Video;
        // Sender reports of a chat SSRC must not feed the game audio stats (LSR/DLSR).
        if (game == 0 && answerAudioSsrcs.count(ssrc)) return Kind::Audio;
        return Kind::Unknown;
    }
    // Game audio SSRC from the answer's first audio a=ssrc (only while none is known).
    void setGameAudioFromAnswer(uint32_t ssrc) {
        if (ssrc == 0) return;
        std::lock_guard<std::mutex> lk(audioMu);
        if (audioSsrc.load() != 0) return;
        audioSsrc = ssrc;
        gameLastRxMs = nowMs();
        gameRxPackets = 0;
        XC_LOGI("webrtc: game audio ssrc %08x (answer)", ssrc);
    }
    // Every PT-111 RTP packet ends up here (from the global handler or the audio track).
    void routeAudio(const uint8_t* d, size_t n) {
        const uint32_t ssrc = be32(d + 8);
        if (ssrc == 0) return;
        const int64_t now = nowMs();
        bool chat = false;
        {
            std::lock_guard<std::mutex> lk(audioMu);
            const uint32_t game = audioSsrc.load();
            if (game == 0) {
                audioSsrc = ssrc;
                gameLastRxMs = now;
                gameRxPackets = 1;
                XC_LOGI("webrtc: game audio ssrc %08x (first packet)", ssrc);
            } else if (ssrc == game) {
                gameLastRxMs = now;
                ++gameRxPackets;
            } else if (gameRxPackets == 0 || now - gameLastRxMs >= kGameTakeoverMs) {
                // The announced game SSRC never delivered, or went silent while this one flows
                // (server-side SSRC change): this stream is the game audio now.
                audioSsrc = ssrc;
                gameLastRxMs = now;
                gameRxPackets = 1;
                if (chatSsrc.load() == ssrc) chatSsrc = 0;
                XC_LOGI("webrtc: game audio ssrc %08x (takeover)", ssrc);
            } else {
                const uint32_t cur = chatSsrc.load();
                if (cur == ssrc) {
                    chatLastRxMs = now;
                    chat = true;
                } else if (cur == 0 || now - chatLastRxMs >= kChatSwitchMs) {
                    chatSsrc = ssrc;
                    chatLastRxMs = now;
                    chat = true;
                    XC_LOGI("webrtc: chat audio ssrc %08x detected (game %08x)", ssrc, game);
                } else {
                    ++chatDropped;
                    if (now - chatDropLogMs >= kMicLogIntervalMs) {
                        chatDropLogMs = now;
                        XC_LOGD("webrtc: extra voice ssrc %08x while chat %08x active, dropped %llu packet(s)", ssrc,
                                cur, static_cast<unsigned long long>(chatDropped));
                    }
                    return;
                }
            }
        }
        if (chat) chatMedia(d, n);
        else media(Kind::Audio, d, n);
    }

    void latch(Kind k, uint32_t ssrc) {
        std::atomic<uint32_t>& a = k == Kind::Video ? videoSsrc : audioSsrc;
        const uint32_t prev = a.exchange(ssrc);
        if (prev != ssrc)
            XC_LOGI("webrtc: %s SSRC %08x%s", k == Kind::Video ? "video" : "audio", ssrc, prev ? " (changed)" : "");
    }

    void media(Kind k, const uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (closed) return;
        if (k == Kind::Video && cb.onVideoRtp) cb.onVideoRtp(d, n);
        else if (k == Kind::Audio && cb.onAudioRtp) cb.onAudioRtp(d, n);
    }
    void chatMedia(const uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!closed && cb.onChatAudioRtp) cb.onChatAudioRtp(d, n);
    }
    void channelClosed(const std::string& ch) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!closed && cb.onChannelClosed) cb.onChannelClosed(ch);
    }
    void senderReport(bool video, uint32_t ssrc, uint64_t ntp) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!closed && cb.onSenderReport) cb.onSenderReport(video, ssrc, ntp);
    }
    void text(const std::string& ch, const std::string& t) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!closed && cb.onText) cb.onText(ch, t);
    }
    void binary(const std::string& ch, const uint8_t* d, size_t n) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!closed && cb.onBinary) cb.onBinary(ch, d, n);
    }
    void open(const std::string& ch) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!closed && cb.onChannelOpen) cb.onChannelOpen(ch);
    }
    void state(const std::string& s) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (!closed && cb.onState) cb.onState(s);
    }

    // Walk a (compound) RTCP packet and report every sender report.
    void parseRtcp(const uint8_t* p, size_t n) {
        size_t off = 0;
        while (off + 8 <= n) {
            const uint8_t* h = p + off;
            if ((h[0] >> 6) != 2) return;
            const size_t len = (static_cast<size_t>(be16(h + 2)) + 1) * 4;
            if (off + len > n) return;
            if (h[1] == 200 && len >= 28) {  // SR: sender SSRC, NTP (64), RTP ts, counts
                const uint32_t ssrc = be32(h + 4);
                const uint64_t ntp = (static_cast<uint64_t>(be32(h + 8)) << 32) | be32(h + 12);
                const Kind k = classifySsrc(ssrc);
                if (k != Kind::Unknown) senderReport(k == Kind::Video, ssrc, ntp);
            } else if (h[1] == 203) {
                XC_LOGI("webrtc: RTCP BYE received");
            }
            off += len;
        }
    }
};

// PeerConnection-wide handler: runs on every decrypted SRTP/SRTCP packet before track dispatch.
class GlobalReceiver final : public rtc::MediaHandler {
public:
    explicit GlobalReceiver(std::shared_ptr<Shared> s) : s_(std::move(s)) {}

    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        rtc::message_vector keep;
        keep.reserve(messages.size());
        for (auto& m : messages) {
            if (!m) continue;
            const auto* d = reinterpret_cast<const uint8_t*>(m->data());
            const size_t n = m->size();
            if (m->type == rtc::Message::Control) {
                // RTCP is consumed here; the tracks only carry RTP to our callbacks.
                s_->parseRtcp(d, n);
                continue;
            }
            if (m->type != rtc::Message::Binary || n < 12 || (d[0] >> 6) != 2) {
                keep.push_back(std::move(m));
                continue;
            }
            const uint8_t pt = d[1] & 0x7F;
            const uint32_t ssrc = be32(d + 8);
            const Shared::Kind k = s_->classifyPt(pt);
            if (k == Shared::Kind::Unknown) {
                keep.push_back(std::move(m));
                continue;
            }
            if (k == Shared::Kind::Video && s_->videoSsrc.load() != ssrc) s_->latch(k, ssrc);
            if (s_->isRoutable(ssrc)) {
                keep.push_back(std::move(m));  // libdatachannel routes it to the track -> onMessage
            } else if (k == Shared::Kind::Audio) {
                s_->routeAudio(d, n);  // unannounced SSRC: libdatachannel would drop it
            } else {
                s_->media(k, d, n);
            }
        }
        messages.swap(keep);
    }

private:
    std::shared_ptr<Shared> s_;
};

}  // namespace

struct WebRtc::Impl {
    mutable std::mutex mu;  // guards the pointers below
    std::shared_ptr<Shared> sh;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::map<std::string, std::shared_ptr<rtc::DataChannel>> channels;
    std::shared_ptr<GlobalReceiver> rx;
    uint32_t localSsrc = 0;
    std::string cname;
    bool offered = false;
    WebRtc::MicInfo mic;            // ssrc 0 = voice off
    std::string answerAudioDir;     // direction of mid "audio" in the last applied answer
    std::string initUfrag, initPwd, initFp;  // credentials of the first local offer
    bool renegPending = false;      // beginRenegotiation() set a local offer not yet answered

    // Microphone sender state (sendMicOpus), own lock: never held together with Shared::mu.
    mutable std::mutex micMu;
    uint16_t micSeq = 0;
    uint32_t micTs = 0;
    uint64_t micPackets = 0, micBytes = 0, micFailed = 0;
    uint64_t micPayloadBytes = 0;  // octet count for the SR (payload only, RFC 3550)
    bool micFirstSent = false;
    int64_t micLastSrMs = 0, micLastFailLogMs = -kMicLogIntervalMs, micLastStatsLogMs = 0;

    void resetMicState() {
        std::lock_guard<std::mutex> lk(micMu);
        micSeq = static_cast<uint16_t>(randomU32());
        micTs = randomU32();
        micPackets = micBytes = micFailed = micPayloadBytes = 0;
        micFirstSent = false;
        micLastSrMs = 0;
        micLastFailLogMs = -kMicLogIntervalMs;
        micLastStatsLogMs = nowMs();
    }

    std::shared_ptr<rtc::DataChannel> channel(const std::string& label) const {
        std::lock_guard<std::mutex> lk(mu);
        auto it = channels.find(label);
        return it == channels.end() ? nullptr : it->second;
    }

    bool sendRtcp(const std::vector<uint8_t>& pkt) {
        std::shared_ptr<rtc::Track> t;
        {
            std::lock_guard<std::mutex> lk(mu);
            t = video ? video : audio;
        }
        if (!t || pkt.size() < 8) return false;
        try {
            if (!t->isOpen()) return false;
            return t->send(reinterpret_cast<const std::byte*>(pkt.data()), pkt.size());
        } catch (const std::exception& e) {
            XC_LOGD("webrtc: RTCP send failed: %s", e.what());
            return false;
        }
    }
};

WebRtc::WebRtc() : d_(new Impl) {}
WebRtc::~WebRtc() { close(); }

bool WebRtc::init(const Callbacks& cb, bool voice) {
    close();
    initRtcLoggerOnce();

    auto sh = std::make_shared<Shared>();
    sh->cb = cb;
    const uint32_t localSsrc = randomSsrc();
    char cname[32];
    std::snprintf(cname, sizeof(cname), "xc%08x", localSsrc);
    MicInfo mic;
    if (voice) {
        do {
            mic.ssrc = randomSsrc();
        } while (mic.ssrc == localSsrc);
        mic.cname = cname;
        mic.msid = proto::newUuid();
        mic.trackId = proto::newUuid();
    }

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::map<std::string, std::shared_ptr<rtc::DataChannel>> channels;
    auto rx = std::make_shared<GlobalReceiver>(sh);
    try {
        rtc::Configuration cfg;
        cfg.iceServers.emplace_back(kStunServer);
        cfg.disableAutoNegotiation = true;  // one explicit setLocalDescription(Offer) in gatherLocal()
        pc = std::make_shared<rtc::PeerConnection>(cfg);
        pc->setMediaHandler(rx);

        std::weak_ptr<Shared> wsh = sh;
        pc->onStateChange([wsh](rtc::PeerConnection::State st) {
            XC_LOGI("webrtc: peer connection %s", stateName(st));
            if (auto s = wsh.lock()) s->state(stateName(st));
        });
        pc->onIceStateChange([](rtc::PeerConnection::IceState st) {
            XC_LOGD("webrtc: ICE state %d", static_cast<int>(st));
        });
        pc->onGatheringStateChange([wsh](rtc::PeerConnection::GatheringState st) {
            XC_LOGD("webrtc: gathering state %d", static_cast<int>(st));
            if (st != rtc::PeerConnection::GatheringState::Complete) return;
            if (auto s = wsh.lock()) {
                std::lock_guard<std::mutex> lk(s->gatherMu);
                s->gatherDone = true;
                s->gatherCv.notify_all();
            }
        });
        pc->onLocalCandidate([wsh](rtc::Candidate c) {
            if (auto s = wsh.lock()) {
                std::lock_guard<std::mutex> lk(s->gatherMu);
                s->trickled.push_back(c.candidate());
            }
        });
        pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
            XC_LOGW("webrtc: unexpected remote data channel '%s'", dc->label().c_str());
        });

        // 1) video (mid "video"), 2) audio (mid "audio") — order matters, see header.
        rtc::Description::Video vdesc("video", rtc::Description::Direction::RecvOnly);
        vdesc.addH264Codec(kDefaultVideoPt,
                           std::string("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"));
        video = pc->addTrack(vdesc);
        rtc::Description::Audio adesc(
            "audio", voice ? rtc::Description::Direction::SendRecv : rtc::Description::Direction::RecvOnly);
        adesc.addOpusCodec(kDefaultAudioPt, std::string("minptime=10;useinbandfec=1"));
        if (voice) adesc.addSSRC(mic.ssrc, mic.cname, mic.msid, mic.trackId);
        audio = pc->addTrack(adesc);  // never gets a media handler: mic RTP is sent raw

        auto onTrackRtp = [wsh](Shared::Kind k) {
            return [wsh, k](rtc::binary b) {
                const auto* d = reinterpret_cast<const uint8_t*>(b.data());
                if (b.size() < 12 || isRtcpPacket(d, b.size())) return;
                if (auto s = wsh.lock()) s->media(k, d, b.size());
            };
        };
        video->onMessage(onTrackRtp(Shared::Kind::Video), [](rtc::string) {});
        audio->onMessage(
            [wsh](rtc::binary b) {
                const auto* d = reinterpret_cast<const uint8_t*>(b.data());
                if (b.size() < 12 || isRtcpPacket(d, b.size())) return;
                if (auto s = wsh.lock()) s->routeAudio(d, b.size());
            },
            [](rtc::string) {});

        // 3) data channels, in-band (DCEP), reliable + ordered, before the offer -> m-line mid "0".
        for (const ChannelSpec& spec : kChannels) {
            rtc::DataChannelInit init;
            init.protocol = spec.protocol;
            auto dc = pc->createDataChannel(spec.label, init);
            const std::string label = spec.label;
            dc->onOpen([wsh, label]() {
                XC_LOGI("webrtc: channel '%s' open", label.c_str());
                if (auto s = wsh.lock()) s->open(label);
            });
            dc->onClosed([wsh, label]() {
                XC_LOGI("webrtc: channel '%s' closed", label.c_str());
                if (auto s = wsh.lock()) s->channelClosed(label);
            });
            dc->onError([label](std::string err) { XC_LOGW("webrtc: channel '%s' error: %s", label.c_str(), err.c_str()); });
            dc->onMessage(
                [wsh, label](rtc::binary b) {
                    if (auto s = wsh.lock()) s->binary(label, reinterpret_cast<const uint8_t*>(b.data()), b.size());
                },
                [wsh, label](rtc::string t) {
                    if (auto s = wsh.lock()) s->text(label, t);
                });
            channels[label] = dc;
        }
    } catch (const std::exception& e) {
        XC_LOGE("webrtc: init failed: %s", e.what());
        if (pc) {
            try {
                pc->close();
            } catch (...) {
            }
        }
        return false;
    }

    d_->resetMicState();
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->sh = sh;
        d_->pc = pc;
        d_->video = video;
        d_->audio = audio;
        d_->channels = std::move(channels);
        d_->rx = rx;
        d_->localSsrc = localSsrc;
        d_->cname = cname;
        d_->offered = false;
        d_->mic = mic;
        d_->answerAudioDir.clear();
        d_->initUfrag.clear();
        d_->initPwd.clear();
        d_->initFp.clear();
        d_->renegPending = false;
    }
    XC_LOGI("webrtc: voice %s (audio %s, mic ssrc %08x cname %s msid %s)", voice ? "enabled" : "disabled",
            voice ? "sendrecv" : "recvonly", mic.ssrc, voice ? mic.cname.c_str() : "-",
            voice ? mic.msid.c_str() : "-");
    return true;
}

bool WebRtc::gatherLocal(std::string& ufrag, std::string& pwd, std::string& fingerprint,
                         std::vector<std::string>& candidates, int timeoutMs, const std::atomic<bool>* abort) {
    ufrag.clear();
    pwd.clear();
    fingerprint.clear();
    candidates.clear();

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<Shared> sh;
    bool alreadyOffered;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        pc = d_->pc;
        sh = d_->sh;
        alreadyOffered = d_->offered;
        d_->offered = true;
    }
    if (!pc || !sh) {
        XC_LOGE("webrtc: gatherLocal before init");
        return false;
    }
    try {
        if (!alreadyOffered) pc->setLocalDescription(rtc::Description::Type::Offer);
    } catch (const std::exception& e) {
        XC_LOGE("webrtc: setLocalDescription(offer) failed: %s", e.what());
        return false;
    }

    bool complete = false;
    {
        // Wait in short slices so a cancelled session does not sit out the whole timeout.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));
        std::unique_lock<std::mutex> lk(sh->gatherMu);
        for (;;) {
            if (abort && abort->load()) {
                XC_LOGD("webrtc: ICE gathering aborted");
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (sh->gatherDone || now >= deadline) {
                complete = sh->gatherDone;
                break;
            }
            sh->gatherCv.wait_for(lk, std::min<std::chrono::steady_clock::duration>(deadline - now,
                                                                                       std::chrono::milliseconds(100)));
        }
    }
    if (!complete) XC_LOGW("webrtc: ICE gathering not complete after %d ms, using what we have", timeoutMs);

    std::optional<rtc::Description> desc;
    try {
        desc = pc->localDescription();
    } catch (const std::exception& e) {
        XC_LOGE("webrtc: localDescription failed: %s", e.what());
        return false;
    }
    if (!desc) {
        XC_LOGE("webrtc: no local description");
        return false;
    }
    ufrag = desc->iceUfrag().value_or("");
    pwd = desc->icePwd().value_or("");
    if (auto fp = desc->fingerprint()) {
        if (fp->algorithm == rtc::CertificateFingerprint::Algorithm::Sha256) fingerprint = fp->value;
        else XC_LOGE("webrtc: local fingerprint is not sha-256");
    }

    std::set<std::string> seen;
    auto add = [&](const std::string& c) {
        std::string s = trim(c);
        if (s.rfind("a=", 0) == 0) s = s.substr(2);
        if (s.rfind("candidate:", 0) != 0) return;
        if (seen.insert(s).second) candidates.push_back(s);
    };
    for (const rtc::Candidate& c : desc->candidates()) add(c.candidate());
    {
        std::lock_guard<std::mutex> lk(sh->gatherMu);
        for (const auto& c : sh->trickled) add(c);
    }

    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->pc == pc) {
            d_->initUfrag = ufrag;
            d_->initPwd = pwd;
            d_->initFp = fingerprint;
        }
    }

    XC_LOGI("webrtc: local ICE ufrag=%s, %zu candidate(s)%s", ufrag.c_str(), candidates.size(),
            complete ? "" : " (gathering incomplete)");
    for (const auto& c : candidates) XC_LOGD("webrtc: local %s", c.c_str());
    if (ufrag.empty() || pwd.empty() || fingerprint.empty()) {
        XC_LOGE("webrtc: local description lacks ICE credentials or sha-256 fingerprint");
        return false;
    }
    if (candidates.empty()) {
        XC_LOGE("webrtc: no local ICE candidates gathered");
        return false;
    }
    return true;
}

std::string WebRtc::localDescriptionSdp() const {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        pc = d_->pc;
    }
    if (!pc) return std::string();
    try {
        if (auto desc = pc->localDescription()) return std::string(*desc);
    } catch (const std::exception& e) {
        XC_LOGW("webrtc: localDescription failed: %s", e.what());
    }
    return std::string();
}

bool WebRtc::setRemote(const std::string& answerSdp, const std::vector<std::string>& remoteCandidates) {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<Shared> sh;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        pc = d_->pc;
        sh = d_->sh;
    }
    if (!pc || !sh) {
        XC_LOGE("webrtc: setRemote before init");
        return false;
    }

    const AnswerInfo info = parseAnswer(answerSdp);
    {
        std::lock_guard<std::mutex> lk(sh->infoMu);
        if (!info.videoPts.empty()) sh->videoPts = info.videoPts;
        if (!info.audioPts.empty()) sh->audioPts = info.audioPts;
        sh->answerVideoSsrcs = info.videoSsrcs;
        sh->answerAudioSsrcs = info.audioSsrcs;
        sh->routable.clear();
        sh->routable.insert(info.videoSsrcs.begin(), info.videoSsrcs.end());
        sh->routable.insert(info.audioSsrcs.begin(), info.audioSsrcs.end());
    }
    if (!info.videoSsrcs.empty() && sh->videoSsrc.load() == 0) sh->videoSsrc = *info.videoSsrcs.begin();
    sh->setGameAudioFromAnswer(info.firstAudioSsrc);
    XC_LOGI("webrtc: answer: video pt %d ssrcs %zu, audio pt %d ssrcs %zu",
            info.videoPts.empty() ? -1 : *info.videoPts.begin(), info.videoSsrcs.size(),
            info.audioPts.empty() ? -1 : *info.audioPts.begin(), info.audioSsrcs.size());

    try {
        pc->setRemoteDescription(rtc::Description(answerSdp, rtc::Description::Type::Answer));
    } catch (const std::exception& e) {
        XC_LOGE("webrtc: setRemoteDescription failed: %s", e.what());
        return false;
    }
    const std::string dir = proto::sdpMediaDirection(answerSdp, "audio");
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->pc == pc) d_->answerAudioDir = dir;
    }
    XC_LOGI("webrtc: answer audio direction %s", dir.empty() ? "(none)" : dir.c_str());

    size_t added = 0, tried = 0;
    for (const std::string& raw : remoteCandidates) {
        std::string c = trim(raw);
        if (c.rfind("a=", 0) == 0) c = c.substr(2);
        if (c.empty() || c.rfind("candidate:", 0) != 0) continue;  // also skips end-of-candidates
        ++tried;
        try {
            pc->addRemoteCandidate(rtc::Candidate(c, "0"));
            ++added;
            XC_LOGD("webrtc: remote %s", c.c_str());
        } catch (const std::exception& e) {
            XC_LOGW("webrtc: remote candidate rejected (%s): %s", e.what(), c.c_str());
        }
    }
    XC_LOGI("webrtc: added %zu/%zu remote candidate(s)", added, tried);
    return tried == 0 || added > 0;
}

bool WebRtc::sendText(const std::string& ch, const std::string& text) {
    auto dc = d_->channel(ch);
    if (!dc) return false;
    try {
        if (!dc->isOpen()) return false;
        dc->send(std::string(text));  // string message -> WebRTC DOMString PPID
        return true;
    } catch (const std::exception& e) {
        XC_LOGW("webrtc: send on '%s' failed: %s", ch.c_str(), e.what());
        return false;
    }
}

bool WebRtc::sendBinary(const std::string& ch, const std::vector<uint8_t>& data) {
    auto dc = d_->channel(ch);
    if (!dc) return false;
    try {
        if (!dc->isOpen()) return false;
        dc->send(reinterpret_cast<const std::byte*>(data.data()), data.size());
        return true;
    } catch (const std::exception& e) {
        XC_LOGW("webrtc: send on '%s' failed: %s", ch.c_str(), e.what());
        return false;
    }
}

bool WebRtc::isChannelOpen(const std::string& ch) const {
    auto dc = d_->channel(ch);
    if (!dc) return false;
    try {
        return dc->isOpen();
    } catch (...) {
        return false;
    }
}

void WebRtc::requestKeyframe() {
    const uint32_t media = videoSsrc();
    if (!media) return;
    // RFC 4585 §6.3.1 PLI: PSFB (206), FMT 1, length 2. Sender SSRC 0 like browsers — Xbox
    // ignores PLIs whose sender SSRC equals the media SSRC (GreenOvercast).
    std::vector<uint8_t> p;
    p.reserve(12);
    p.push_back(0x80 | 1);
    p.push_back(206);
    put16(p, 2);
    put32(p, 0);
    put32(p, media);
    d_->sendRtcp(p);
}

void WebRtc::sendNack(uint32_t ssrc, const std::vector<uint16_t>& seqs) {
    if (!ssrc) ssrc = videoSsrc();
    if (!ssrc || seqs.empty()) return;
    uint32_t sender;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        sender = d_->localSsrc;
    }
    // Order wrap-aware relative to the first entry, de-duplicate.
    std::vector<uint16_t> s(seqs);
    const uint16_t ref = static_cast<uint16_t>(s.front() - 0x4000);
    std::sort(s.begin(), s.end(), [ref](uint16_t a, uint16_t b) {
        return static_cast<uint16_t>(a - ref) < static_cast<uint16_t>(b - ref);
    });
    s.erase(std::unique(s.begin(), s.end()), s.end());

    // RFC 4585 §6.2.1 generic NACK: RTPFB (205), FMT 1, FCI = PID + BLP (16 following packets).
    constexpr size_t kMaxFciPerPacket = 64;
    std::vector<std::pair<uint16_t, uint16_t>> fci;
    for (size_t i = 0; i < s.size();) {
        const uint16_t pid = s[i];
        uint16_t blp = 0;
        size_t j = i + 1;
        while (j < s.size()) {
            const uint16_t d = static_cast<uint16_t>(s[j] - pid);
            if (d == 0 || d > 16) break;
            blp |= static_cast<uint16_t>(1u << (d - 1));
            ++j;
        }
        fci.emplace_back(pid, blp);
        i = j;
    }
    for (size_t off = 0; off < fci.size(); off += kMaxFciPerPacket) {
        const size_t cnt = std::min(kMaxFciPerPacket, fci.size() - off);
        std::vector<uint8_t> p;
        p.reserve(12 + 4 * cnt);
        p.push_back(0x80 | 1);
        p.push_back(205);
        put16(p, static_cast<uint16_t>(2 + cnt));
        put32(p, sender);
        put32(p, ssrc);
        for (size_t k = 0; k < cnt; ++k) {
            put16(p, fci[off + k].first);
            put16(p, fci[off + k].second);
        }
        d_->sendRtcp(p);
    }
}

void WebRtc::sendReceiverReport(const std::vector<RtpReceiveStats>& blocks) {
    uint32_t sender;
    std::string cname;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        sender = d_->localSsrc;
        cname = d_->cname;
    }
    if (!sender) return;
    std::vector<const RtpReceiveStats*> valid;
    for (const auto& b : blocks)
        if (b.ssrc && valid.size() < 31) valid.push_back(&b);

    // RFC 3550 §6.4.2 RR + §6.5 SDES (CNAME) in one compound packet (green-nx: senders ignore
    // bare RRs). DLSR/LSR come from RtpStatsTracker.
    std::vector<uint8_t> p;
    p.reserve(8 + 24 * valid.size() + 32);
    p.push_back(static_cast<uint8_t>(0x80 | valid.size()));
    p.push_back(201);
    put16(p, static_cast<uint16_t>(1 + 6 * valid.size()));
    put32(p, sender);
    for (const RtpReceiveStats* b : valid) {
        put32(p, b->ssrc);
        int32_t lost = std::max<int32_t>(-0x800000, std::min<int32_t>(0x7FFFFF, b->cumulativeLost));
        put32(p, (static_cast<uint32_t>(b->fractionLost) << 24) | (static_cast<uint32_t>(lost) & 0x00FFFFFFu));
        put32(p, b->extHighestSeq);
        put32(p, b->jitter);
        put32(p, b->lastSr);
        put32(p, b->lastSr ? b->delaySinceLastSr : 0);
    }
    const size_t sdes = p.size();
    p.push_back(0x80 | 1);
    p.push_back(202);
    put16(p, 0);  // patched below
    put32(p, sender);
    p.push_back(1);  // CNAME
    p.push_back(static_cast<uint8_t>(cname.size()));
    p.insert(p.end(), cname.begin(), cname.end());
    p.push_back(0);  // end of items
    while ((p.size() - sdes) % 4) p.push_back(0);
    set16(p, sdes + 2, static_cast<uint16_t>((p.size() - sdes) / 4 - 1));
    d_->sendRtcp(p);
}

void WebRtc::sendRemb(uint32_t bps) {
    const uint32_t v = videoSsrc(), a = audioSsrc();
    if (!v) return;
    uint32_t sender;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        sender = d_->localSsrc;
    }
    // draft-alvestrand-rmcat-remb: PSFB (206), FMT 15, media SSRC 0, "REMB", num SSRC,
    // 6-bit exponent + 18-bit mantissa, SSRC list.
    uint32_t mantissa = bps, exp = 0;
    while (mantissa > 0x3FFFF) {
        mantissa >>= 1;
        ++exp;
    }
    const uint8_t num = a ? 2 : 1;
    std::vector<uint8_t> p;
    p.reserve(28);
    p.push_back(0x80 | 15);
    p.push_back(206);
    put16(p, static_cast<uint16_t>(4 + num));
    put32(p, sender);
    put32(p, 0);
    p.push_back('R');
    p.push_back('E');
    p.push_back('M');
    p.push_back('B');
    p.push_back(num);
    p.push_back(static_cast<uint8_t>((exp << 2) | ((mantissa >> 16) & 0x03)));
    p.push_back(static_cast<uint8_t>(mantissa >> 8));
    p.push_back(static_cast<uint8_t>(mantissa));
    put32(p, v);
    if (a) put32(p, a);
    d_->sendRtcp(p);
}

uint32_t WebRtc::videoSsrc() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->sh ? d_->sh->videoSsrc.load() : 0;
}

uint32_t WebRtc::audioSsrc() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->sh ? d_->sh->audioSsrc.load() : 0;
}

uint32_t WebRtc::chatAudioSsrc() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->sh ? d_->sh->chatSsrc.load() : 0;
}

WebRtc::MicInfo WebRtc::micInfo() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->mic;
}

WebRtc::MicTxStats WebRtc::micTxStats() const {
    std::lock_guard<std::mutex> lk(d_->micMu);
    MicTxStats st;
    st.packets = d_->micPackets;
    st.bytes = d_->micBytes;
    st.failed = d_->micFailed;
    return st;
}

std::string WebRtc::answerAudioDirection() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->answerAudioDir;
}

bool WebRtc::sendMicOpus(const uint8_t* opus, size_t n, uint32_t samples48k, bool marker) {
    if (!opus || n == 0 || n > 1200) return false;
    std::shared_ptr<rtc::Track> audio;
    uint32_t ssrc;
    std::string cname;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        audio = d_->audio;
        ssrc = d_->mic.ssrc;
        cname = d_->mic.cname;
    }
    if (!audio || ssrc == 0) return false;

    const int64_t now = nowMs();
    bool sendSr = false;
    uint32_t srTs = 0, srPackets = 0, srOctets = 0;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(d_->micMu);
        const uint16_t seq = d_->micSeq;
        const uint32_t ts = d_->micTs;
        d_->micTs += samples48k;  // timestamps follow capture time even when a send fails
        std::string why;
        bool attempted = false;
        try {
            if (!audio->isOpen()) {
                why = "track not open";
            } else {
                const std::vector<uint8_t> pkt = buildOpusRtp(seq, ts, ssrc, marker, opus, n);
                attempted = true;
                ok = audio->send(reinterpret_cast<const std::byte*>(pkt.data()), pkt.size());
                if (!ok) why = "transport refused packet";
            }
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (attempted) ++d_->micSeq;  // a refused packet is a loss the receiver may see as a gap
        if (ok) {
            ++d_->micPackets;
            d_->micBytes += n + 12;
            d_->micPayloadBytes += n;
            if (!d_->micFirstSent) {
                d_->micFirstSent = true;
                XC_LOGI("voice: first mic RTP sent ssrc=%08x seq=%u ts=%u bytes=%zu track=%s", ssrc, unsigned(seq),
                        unsigned(ts), n + 12, directionName(audio->direction()));
            }
            if (now - d_->micLastSrMs >= kMicSrIntervalMs) {
                d_->micLastSrMs = now;
                sendSr = true;
                srTs = d_->micTs;
                srPackets = static_cast<uint32_t>(d_->micPackets);
                srOctets = static_cast<uint32_t>(d_->micPayloadBytes);
            }
        } else {
            ++d_->micFailed;
            if (now - d_->micLastFailLogMs >= kMicLogIntervalMs) {
                d_->micLastFailLogMs = now;
                XC_LOGW("voice: mic RTP send failed (%s)", why.c_str());
            }
        }
        if (now - d_->micLastStatsLogMs >= kMicLogIntervalMs) {
            d_->micLastStatsLogMs = now;
            XC_LOGI("voice: tx %llu pkts %llu bytes failed %llu", static_cast<unsigned long long>(d_->micPackets),
                    static_cast<unsigned long long>(d_->micBytes), static_cast<unsigned long long>(d_->micFailed));
        }
    }

    if (sendSr) {
        // RFC 3550 §6.4.1 SR (no report blocks) + §6.5 SDES CNAME for the mic SSRC, one compound
        // packet through the existing RTCP path (video track).
        const auto wall = std::chrono::system_clock::now().time_since_epoch();
        const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(wall).count());
        const uint32_t ntpSec = static_cast<uint32_t>(us / 1000000u + 2208988800u);
        const uint32_t ntpFrac = static_cast<uint32_t>(((us % 1000000u) << 32) / 1000000u);
        std::vector<uint8_t> p;
        p.reserve(64);
        p.push_back(0x80);
        p.push_back(200);
        put16(p, 6);
        put32(p, ssrc);
        put32(p, ntpSec);
        put32(p, ntpFrac);
        put32(p, srTs);
        put32(p, srPackets);
        put32(p, srOctets);
        const size_t sdes = p.size();
        p.push_back(0x80 | 1);
        p.push_back(202);
        put16(p, 0);  // patched below
        put32(p, ssrc);
        p.push_back(1);  // CNAME
        const size_t cl = std::min<size_t>(cname.size(), 255);
        p.push_back(static_cast<uint8_t>(cl));
        p.insert(p.end(), cname.begin(), cname.begin() + static_cast<std::ptrdiff_t>(cl));
        p.push_back(0);
        while ((p.size() - sdes) % 4) p.push_back(0);
        set16(p, sdes + 2, static_cast<uint16_t>((p.size() - sdes) / 4 - 1));
        d_->sendRtcp(p);
    }
    return ok;
}

bool WebRtc::beginRenegotiation(std::string& ufrag, std::string& pwd, std::string& fingerprint) {
    ufrag.clear();
    pwd.clear();
    fingerprint.clear();
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string iu, ip, ifp;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        pc = d_->pc;
        iu = d_->initUfrag;
        ip = d_->initPwd;
        ifp = d_->initFp;
    }
    if (!pc) {
        XC_LOGE("voice: renegotiation without peer connection");
        return false;
    }
    std::optional<rtc::Description> desc;
    try {
        if (!pc->remoteDescription()) {
            XC_LOGE("voice: renegotiation before the first answer was applied");
            return false;
        }
        if (pc->signalingState() != rtc::PeerConnection::SignalingState::Stable) {
            XC_LOGE("voice: renegotiation in signaling state %d", static_cast<int>(pc->signalingState()));
            return false;
        }
        pc->setLocalDescription(rtc::Description::Type::Offer);
        {
            std::lock_guard<std::mutex> lk(d_->mu);
            if (d_->pc == pc) d_->renegPending = true;
        }
        desc = pc->localDescription();
    } catch (const std::exception& e) {
        XC_LOGE("voice: renegotiation setLocalDescription(offer) failed: %s", e.what());
        return false;
    }
    if (!desc) {
        XC_LOGE("voice: renegotiation produced no local description");
        return false;
    }
    ufrag = desc->iceUfrag().value_or("");
    pwd = desc->icePwd().value_or("");
    if (auto fp = desc->fingerprint())
        if (fp->algorithm == rtc::CertificateFingerprint::Algorithm::Sha256) fingerprint = fp->value;
    const bool same = ufrag == iu && pwd == ip && fingerprint == ifp;
    XC_LOGI("voice: renegotiation local offer ready (creds %s)", same ? "unchanged" : "CHANGED");
    if (!same) XC_LOGW("voice: renegotiation ICE/DTLS credentials differ from the initial offer");
    if (ufrag.empty() || pwd.empty() || fingerprint.empty()) {
        XC_LOGE("voice: renegotiation local description lacks ICE credentials or sha-256 fingerprint");
        return false;
    }
    return true;
}

bool WebRtc::finishRenegotiation(const std::string& answerSdp) {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<Shared> sh;
    bool pending;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        pc = d_->pc;
        sh = d_->sh;
        pending = d_->renegPending;
    }
    if (!pc || !sh) {
        XC_LOGE("voice: renegotiation answer without peer connection");
        return false;
    }
    if (!pending) {
        XC_LOGE("voice: renegotiation answer without a pending renegotiation offer");
        return false;
    }
    try {
        if (pc->signalingState() != rtc::PeerConnection::SignalingState::HaveLocalOffer) {
            XC_LOGE("voice: renegotiation answer in signaling state %d (no pending offer)",
                    static_cast<int>(pc->signalingState()));
            return false;
        }
        pc->setRemoteDescription(rtc::Description(answerSdp, rtc::Description::Type::Answer));
    } catch (const std::exception& e) {
        XC_LOGE("voice: renegotiation setRemoteDescription failed: %s", e.what());
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->pc == pc) d_->renegPending = false;
    }

    const AnswerInfo info = parseAnswer(answerSdp);
    {
        std::lock_guard<std::mutex> lk(sh->infoMu);
        if (!info.videoPts.empty()) sh->videoPts = info.videoPts;
        if (!info.audioPts.empty()) sh->audioPts = info.audioPts;
        sh->answerVideoSsrcs = info.videoSsrcs;
        sh->answerAudioSsrcs = info.audioSsrcs;
        sh->routable.clear();
        sh->routable.insert(info.videoSsrcs.begin(), info.videoSsrcs.end());
        sh->routable.insert(info.audioSsrcs.begin(), info.audioSsrcs.end());
    }
    if (!info.videoSsrcs.empty() && sh->videoSsrc.load() == 0) sh->videoSsrc = *info.videoSsrcs.begin();
    sh->setGameAudioFromAnswer(info.firstAudioSsrc);  // no-op once the game SSRC is known

    const std::string dir = proto::sdpMediaDirection(answerSdp, "audio");
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->pc == pc) d_->answerAudioDir = dir;
    }
    XC_LOGI("webrtc: answer audio direction %s", dir.empty() ? "(none)" : dir.c_str());
    XC_LOGI("voice: renegotiation answer applied (audio %s)", dir.empty() ? "(none)" : dir.c_str());
    return true;
}

void WebRtc::abortRenegotiation() {
    std::shared_ptr<rtc::PeerConnection> pc;
    bool pending;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        pc = d_->pc;
        pending = d_->renegPending;
        d_->renegPending = false;
    }
    if (!pc || !pending) {
        XC_LOGD("voice: renegotiation rollback: no pending renegotiation offer");
        return;
    }
    try {
        const auto st = pc->signalingState();
        if (st != rtc::PeerConnection::SignalingState::HaveLocalOffer) {
            XC_LOGI("voice: renegotiation rollback done (nothing pending, state %d)", static_cast<int>(st));
            return;
        }
        pc->setLocalDescription(rtc::Description::Type::Rollback);
        if (pc->signalingState() == rtc::PeerConnection::SignalingState::Stable)
            XC_LOGI("voice: renegotiation rollback done");
        else
            XC_LOGW("voice: renegotiation rollback failed: still in signaling state %d",
                    static_cast<int>(pc->signalingState()));
    } catch (const std::exception& e) {
        XC_LOGW("voice: renegotiation rollback failed: %s", e.what());
    }
}

void WebRtc::close() {
    if (!d_) return;
    std::shared_ptr<Shared> sh;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::map<std::string, std::shared_ptr<rtc::DataChannel>> channels;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        sh = std::move(d_->sh);
        pc = std::move(d_->pc);
        video = std::move(d_->video);
        audio = std::move(d_->audio);
        channels.swap(d_->channels);
        d_->rx.reset();
        d_->offered = false;
        d_->mic = MicInfo{};
        d_->answerAudioDir.clear();
        d_->renegPending = false;
    }
    if (sh) {
        // Wait for a running callback to finish, then block all further ones. Must not be called
        // while holding a lock that a callback also takes.
        {
            std::lock_guard<std::recursive_mutex> lk(sh->mu);
            sh->closed = true;
        }
        {
            std::lock_guard<std::mutex> lk(sh->gatherMu);
            sh->gatherDone = true;
        }
        sh->gatherCv.notify_all();
    }
    try {
        for (auto& kv : channels) {
            kv.second->resetCallbacks();
            kv.second->close();
        }
        if (video) video->resetCallbacks();
        if (audio) audio->resetCallbacks();
        if (pc) {
            pc->setMediaHandler(nullptr);
            pc->resetCallbacks();
            pc->close();
        }
    } catch (const std::exception& e) {
        XC_LOGW("webrtc: close: %s", e.what());
    }
}

std::vector<uint8_t> buildOpusRtp(uint16_t seq, uint32_t ts, uint32_t ssrc, bool marker, const uint8_t* opus, size_t n,
                                  uint8_t pt) {
    std::vector<uint8_t> p;
    p.reserve(12 + n);
    p.push_back(0x80);  // V=2, P=0, X=0, CC=0
    p.push_back(static_cast<uint8_t>((marker ? 0x80 : 0x00) | (pt & 0x7F)));
    put16(p, seq);
    put32(p, ts);
    put32(p, ssrc);
    if (opus && n) p.insert(p.end(), opus, opus + n);
    return p;
}

}  // namespace xc
