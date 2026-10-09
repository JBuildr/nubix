// Nubix — voice chat capture: microphone -> Opus 20 ms frames for the in-stream chat RTP.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// One capture thread per start(): open the AudioIn source, (optionally) resample to 48 kHz,
// cut 20 ms frames, encode them with libopus (mono, VOIP, 32 kbps, FEC) and hand them to the
// sink, which sends them as RTP. Muting, and a source that reports "no device", keep the frame
// cadence and send encoded digital silence, so the console's chat endpoint stays alive and no
// renegotiation is needed when the user toggles mute.
#include "stream/voice.hpp"

#include <opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.hpp"
#include "platform/audio_in.hpp"

namespace xc {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRtpClock = 48000;
constexpr uint32_t kSamples48kPerFrame = kRtpClock / 1000 * VoiceChat::kFrameMs;  // 960
constexpr int kMaxPacket = 400;          // bytes per encoded frame (32 kbps * 20 ms = 80 typ.)
constexpr int kReadTimeoutMs = 40;       // bounds stop() latency on hosts with a timeout
constexpr int kMaxGrain = 8192;          // sanity bound for a source's grain
constexpr int kFatalReadErrors = 5;      // consecutive read() < 0 -> give up
constexpr int kSilentPollMs = 500;
constexpr int kLevelFrames = 5;          // 5 * 20 ms = level window
constexpr auto kEncoderWarnEvery = std::chrono::seconds(10);
constexpr int kPaceSlackMs = 100;        // a source may run this far ahead of real time
constexpr auto kHealthFirst = std::chrono::seconds(5);
constexpr auto kHealthEvery = std::chrono::seconds(30);

bool opusRate(int r) { return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000; }

// Streaming linear-interpolation resampler (mono s16), phase-continuous across blocks.
class LinearResampler {
public:
    void setup(int inRate, int outRate) {
        step_ = static_cast<double>(inRate) / static_cast<double>(outRate);
        pos_ = 0.0;
        prev_ = 0;
    }
    // Appends resampled samples to out.
    void process(const int16_t* in, int n, std::vector<int16_t>& out) {
        if (n <= 0) return;
        // Index -1 is the previous block's last sample, so output lags input by one sample.
        while (true) {
            const int i = static_cast<int>(pos_);
            if (i >= n) break;
            const double frac = pos_ - i;
            const int s0 = (i == 0) ? prev_ : in[i - 1];
            const int s1 = in[i];
            out.push_back(static_cast<int16_t>(s0 + (s1 - s0) * frac));
            pos_ += step_;
        }
        pos_ -= n;
        prev_ = in[n - 1];
    }

private:
    double step_ = 1.0;
    double pos_ = 0.0;
    int prev_ = 0;
};

}  // namespace

const char* voiceStateName(VoiceChat::State s) {
    switch (s) {
        case VoiceChat::State::Off: return "off";
        case VoiceChat::State::Starting: return "starting";
        case VoiceChat::State::Unavailable: return "unavailable";
        case VoiceChat::State::Muted: return "muted";
        case VoiceChat::State::Live: return "live";
    }
    return "?";
}

struct VoiceChat::Impl {
    std::unique_ptr<platform::AudioIn> src;
    std::thread th;
    std::mutex ctlMu;  // start()/stop() serialisation
    std::atomic<bool> stopReq{false};
    std::atomic<bool> running{false};  // capture thread alive (cleared as it exits)
    std::atomic<bool> muted{false};
    std::atomic<int> state{static_cast<int>(State::Off)};
    std::atomic<float> level{0.0f};
    FrameSink sink;

    mutable std::mutex statsMu;
    Stats stats;

    void setState(State s) {
        const State old = static_cast<State>(state.exchange(static_cast<int>(s)));
        if (old != s) XC_LOGI("voice: state %s -> %s", voiceStateName(old), voiceStateName(s));
    }

    void run();
    void runCapture();
};

void VoiceChat::Impl::run() {
    runCapture();
    running.store(false);
}

void VoiceChat::Impl::runCapture() {
    int rate = 0, grain = 0;
    const char* backend = src->backend();
    {
        std::lock_guard<std::mutex> lk(statsMu);
        stats = Stats{};
        stats.backend = backend;
    }
    if (!src->open(rate, grain)) {
        XC_LOGI("voice: no microphone (%s), voice chat disabled", backend);
        src->close();
        level.store(0.0f);
        setState(State::Unavailable);
        return;
    }
    if (rate <= 0 || grain <= 0 || grain > kMaxGrain) {
        XC_LOGW("voice: no microphone (%s: bad format rate=%d grain=%d), voice chat disabled", backend, rate, grain);
        src->close();
        setState(State::Unavailable);
        return;
    }

    const bool resample = !opusRate(rate);
    const int encRate = resample ? kRtpClock : rate;
    LinearResampler rs;
    if (resample) {
        rs.setup(rate, encRate);
        XC_LOGI("voice: capture rate %d Hz is not an Opus rate, resampling to %d Hz", rate, encRate);
    }

    int err = OPUS_OK;
    OpusEncoder* enc = opus_encoder_create(encRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !enc) {
        XC_LOGW("voice: opus_encoder_create(%d) failed: %s, voice chat disabled", encRate, opus_strerror(err));
        src->close();
        setState(State::Unavailable);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(kLossPerc));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(kComplexity));
    opus_encoder_ctl(enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    {
        std::lock_guard<std::mutex> lk(statsMu);
        stats.rate = encRate;
    }
    XC_LOGI("voice: mic started backend=%s rate=%d grain=%d", backend, rate, grain);

    const int frameN = encRate * kFrameMs / 1000;
    // Headroom: a backend may write a whole hardware grain larger than the one it reported
    // (PS5 HqOpen is unverified); only min(n, grain) samples are consumed.
    std::vector<int16_t> grainBuf(static_cast<size_t>(std::max(grain, kMaxGrain)), 0);
    std::vector<int16_t> acc;
    acc.reserve(static_cast<size_t>(frameN + grain * 2 + 64));
    std::vector<int16_t> zeros(static_cast<size_t>(frameN), 0);
    uint8_t packet[kMaxPacket];

    bool marker = true;
    bool noDevice = (src->silentState() & platform::kAudioInSilentNoDevice) != 0;
    auto nextSilentPoll = Clock::now() + std::chrono::milliseconds(kSilentPollMs);
    int consecutiveErrors = 0;
    int peaks[kLevelFrames] = {0};
    int peakIdx = 0;
    uint64_t windowBytes = 0;
    auto windowStart = Clock::now();
    Clock::time_point lastEncWarn{};
    bool fatal = false;
    bool reopened = false;
    // Real-time pacing + health log: bounds a source whose read() does not block.
    auto paceStart = Clock::now();
    uint64_t samplesIn = 0;
    auto nextHealth = paceStart + kHealthFirst;
    auto healthStart = paceStart;
    uint64_t healthGrains = 0;
    int healthPeak = 0;
    uint32_t lastSilentBits = src->silentState();

    auto updateState = [&] {
        if (noDevice)
            setState(State::Unavailable);
        else
            setState(muted.load() ? State::Muted : State::Live);
    };
    updateState();

    while (!stopReq.load()) {
        const int n = src->read(grainBuf.data(), kReadTimeoutMs);
        if (n < 0) {
            {
                std::lock_guard<std::mutex> lk(statsMu);
                ++stats.readErrors;
            }
            const int32_t code = src->lastError();
            {
                std::lock_guard<std::mutex> lk(statsMu);
                stats.lastError = code;
            }
            if (++consecutiveErrors >= kFatalReadErrors) {
                // One close + reopen (headset re-plugged, controller woke up) before giving up.
                int r2 = 0, g2 = 0;
                if (!reopened && !stopReq.load()) {
                    reopened = true;
                    src->close();
                    if (src->open(r2, g2) && r2 == rate && g2 == grain) {
                        XC_LOGW("voice: capture error (%s 0x%08x), microphone reopened", backend,
                                static_cast<unsigned>(code));
                        consecutiveErrors = 0;
                        paceStart = Clock::now();
                        samplesIn = 0;
                        continue;
                    }
                }
                XC_LOGW("voice: capture error (%s 0x%08x), stopping", backend, static_cast<unsigned>(code));
                fatal = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        consecutiveErrors = 0;
        if (n == 0) continue;
        const int got = std::min(n, grain);

        auto now = Clock::now();
        samplesIn += static_cast<uint64_t>(got);
        {
            const int64_t aheadMs = static_cast<int64_t>(samplesIn * 1000 / static_cast<uint64_t>(rate)) -
                                    std::chrono::duration_cast<std::chrono::milliseconds>(now - paceStart).count();
            if (aheadMs > kPaceSlackMs) {
                std::this_thread::sleep_for(std::chrono::milliseconds(aheadMs - kPaceSlackMs));
                now = Clock::now();
            }
        }
        if (now >= nextSilentPoll) {
            nextSilentPoll = now + std::chrono::milliseconds(kSilentPollMs);
            lastSilentBits = src->silentState();
            noDevice = (lastSilentBits & platform::kAudioInSilentNoDevice) != 0;
        }
        ++healthGrains;
        for (int i = 0; i < got; ++i) healthPeak = std::max(healthPeak, std::abs(static_cast<int>(grainBuf[i])));
        if (now >= nextHealth) {
            const double secs = std::chrono::duration<double>(now - healthStart).count();
            uint64_t framesNow;
            {
                std::lock_guard<std::mutex> lk(statsMu);
                framesNow = stats.frames;
            }
            XC_LOGI("voice: capture %d grains/s (expected %d), frames %llu, peak %.2f, silent 0x%x",
                    secs > 0 ? static_cast<int>(healthGrains / secs + 0.5) : 0, (rate + grain / 2) / grain,
                    static_cast<unsigned long long>(framesNow), healthPeak / 32767.0, lastSilentBits);
            nextHealth = now + kHealthEvery;
            healthStart = now;
            healthGrains = 0;
            healthPeak = 0;
        }
        updateState();

        if (resample)
            rs.process(grainBuf.data(), got, acc);
        else
            acc.insert(acc.end(), grainBuf.begin(), grainBuf.begin() + got);

        size_t off = 0;
        while (acc.size() - off >= static_cast<size_t>(frameN)) {
            const int16_t* frame = acc.data() + off;
            off += static_cast<size_t>(frameN);
            const bool silent = muted.load() || noDevice;
            int peak = 0;
            if (silent) {
                frame = zeros.data();
            } else {
                for (int i = 0; i < frameN; ++i) peak = std::max(peak, std::abs(static_cast<int>(frame[i])));
            }
            peaks[peakIdx] = peak;
            peakIdx = (peakIdx + 1) % kLevelFrames;
            if (silent) {
                level.store(0.0f);
            } else {
                int m = 0;
                for (int p : peaks) m = std::max(m, p);
                level.store(std::min(1.0f, static_cast<float>(m) / 32767.0f));
            }

            const opus_int32 len = opus_encode(enc, frame, frameN, packet, kMaxPacket);
            if (len < 0) {
                if (lastEncWarn == Clock::time_point{} || now - lastEncWarn >= kEncoderWarnEvery) {
                    lastEncWarn = now;
                    XC_LOGW("voice: encoder error %d", static_cast<int>(len));
                }
                continue;
            }
            if (sink) sink(packet, static_cast<size_t>(len), kSamples48kPerFrame, marker);
            marker = false;
            windowBytes += static_cast<uint64_t>(len);
            {
                std::lock_guard<std::mutex> lk(statsMu);
                ++stats.frames;
            }
        }
        if (off) acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(off));

        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart).count();
        if (el >= 1000) {
            std::lock_guard<std::mutex> lk(statsMu);
            stats.kbps = static_cast<int>(windowBytes * 8 / static_cast<uint64_t>(el));
            windowBytes = 0;
            windowStart = now;
        }
    }

    opus_encoder_destroy(enc);
    src->close();
    level.store(0.0f);
    {
        std::lock_guard<std::mutex> lk(statsMu);
        stats.kbps = 0;
        XC_LOGI("voice: mic stopped (%llu frames, %llu read errors)",
                static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.readErrors));
    }
    if (fatal) setState(State::Unavailable);
}

VoiceChat::VoiceChat(std::unique_ptr<platform::AudioIn> source) : d_(new Impl) {
    d_->src = source ? std::move(source) : platform::createAudioIn();
}

VoiceChat::~VoiceChat() { stop(); }

bool VoiceChat::start(FrameSink sink, bool startMuted) {
    std::lock_guard<std::mutex> lk(d_->ctlMu);
    if (d_->th.joinable()) {
        // A thread that already ended (no device / capture error) may be restarted.
        if (d_->running.load()) return false;
        d_->th.join();
    }
    if (!d_->src) {
        d_->setState(State::Unavailable);
        return false;
    }
    d_->stopReq.store(false);
    d_->muted.store(startMuted);
    d_->level.store(0.0f);
    d_->sink = std::move(sink);
    d_->setState(State::Starting);
    d_->running.store(true);
    try {
        d_->th = std::thread([this] { d_->run(); });
    } catch (const std::exception& e) {
        XC_LOGW("voice: cannot start capture thread: %s", e.what());
        d_->running.store(false);
        d_->setState(State::Unavailable);
        return false;
    }
    return true;
}

void VoiceChat::stop() {
    if (!d_) return;
    std::lock_guard<std::mutex> lk(d_->ctlMu);
    d_->stopReq.store(true);
    if (d_->th.joinable()) d_->th.join();
    d_->sink = nullptr;
    d_->level.store(0.0f);
    d_->setState(State::Off);
}

void VoiceChat::setMuted(bool m) {
    if (d_->muted.exchange(m) == m) return;
    XC_LOGI("voice: mic %s", m ? "muted" : "unmuted");
    // The capture thread applies it to the next frame and to state() (within one grain).
    if (m) d_->level.store(0.0f);
}

bool VoiceChat::muted() const { return d_->muted.load(); }

VoiceChat::State VoiceChat::state() const { return static_cast<State>(d_->state.load()); }

float VoiceChat::level() const {
    const State s = state();
    if (s != State::Live) return 0.0f;
    return d_->level.load();
}

VoiceChat::Stats VoiceChat::stats() const {
    std::lock_guard<std::mutex> lk(d_->statsMu);
    return d_->stats;
}

}  // namespace xc
