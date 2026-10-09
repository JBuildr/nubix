// Nubix — voice chat capture/encoder and chat-voice mixing tests.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "platform/audio_in.hpp"
#include "stream/audio.hpp"
#include "stream/voice.hpp"
#include "test.hpp"

using namespace std::chrono;

namespace {

// Real-time paced sine source (like a device that blocks for one grain).
class FakeAudioIn final : public xc::platform::AudioIn {
public:
    FakeAudioIn(int rate, int grain, bool failOpen = false, int failReadsAfter = -1)
        : rate_(rate), grain_(grain), failOpen_(failOpen), failReadsAfter_(failReadsAfter) {}

    bool open(int& rate, int& grain) override {
        if (failOpen_) return false;
        rate = rate_;
        grain = grain_;
        next_ = steady_clock::now();
        open_ = true;
        return true;
    }
    int read(int16_t* buf, int) override {
        if (!open_) return -1;
        if (failReadsAfter_ >= 0 && reads_++ >= failReadsAfter_) return -1;
        std::this_thread::sleep_until(next_);
        next_ += microseconds(1000000LL * grain_ / rate_);
        for (int i = 0; i < grain_; ++i) {
            buf[i] = static_cast<int16_t>(8000.0 * std::sin(phase_));
            phase_ += 2.0 * M_PI * 440.0 / rate_;
        }
        return grain_;
    }
    uint32_t silentState() override { return 0; }
    void close() override { open_ = false; }
    const char* backend() const override { return "fake"; }

private:
    int rate_, grain_;
    bool failOpen_;
    int failReadsAfter_;
    int reads_ = 0;
    bool open_ = false;
    double phase_ = 0.0;
    steady_clock::time_point next_{};
};

struct Captured {
    std::mutex mu;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint32_t> samples;
    std::vector<bool> markers;

    xc::VoiceChat::FrameSink sink() {
        return [this](const uint8_t* p, size_t n, uint32_t s48, bool m) {
            std::lock_guard<std::mutex> lk(mu);
            frames.emplace_back(p, p + n);
            samples.push_back(s48);
            markers.push_back(m);
        };
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return frames.size();
    }
};

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto end = steady_clock::now() + milliseconds(ms);
    while (steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(milliseconds(5));
    }
    return pred();
}

// Decode every frame (48 kHz mono) and return the peak of frames [skip, end).
int decodedPeak(const std::vector<std::vector<uint8_t>>& frames, size_t skip, bool& allOk) {
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
    allOk = dec != nullptr;
    int peak = 0;
    std::vector<int16_t> pcm(5760);
    for (size_t i = 0; dec && i < frames.size(); ++i) {
        const int got = opus_decode(dec, frames[i].data(), static_cast<opus_int32>(frames[i].size()), pcm.data(), 5760, 0);
        if (got != 960) allOk = false;
        if (i >= skip)
            for (int k = 0; k < std::max(got, 0); ++k) peak = std::max(peak, std::abs(static_cast<int>(pcm[k])));
    }
    if (dec) opus_decoder_destroy(dec);
    return peak;
}

}  // namespace

XC_TEST(voice, frames_cadence_marker_and_decode) {
    Captured cap;
    xc::VoiceChat v(std::make_unique<FakeAudioIn>(16000, 256));
    XC_REQUIRE(v.start(cap.sink(), false));
    XC_CHECK(!v.start(cap.sink(), false));  // already running
    XC_CHECK(waitFor([&] { return v.state() == xc::VoiceChat::State::Live; }, 500));
    std::this_thread::sleep_for(milliseconds(1300));  // > one 1 s kbps window
    const float lvl = v.level();
    const auto st = v.stats();
    v.stop();
    XC_CHECK(v.state() == xc::VoiceChat::State::Off);

    std::lock_guard<std::mutex> lk(cap.mu);
    // ~50 frames per second over ~1.3 s.
    XC_CHECK(cap.frames.size() >= 58 && cap.frames.size() <= 75);
    XC_REQUIRE(!cap.frames.empty());
    XC_CHECK(cap.markers.front());
    XC_CHECK(std::count(cap.markers.begin(), cap.markers.end(), true) == 1);
    XC_CHECK(std::all_of(cap.samples.begin(), cap.samples.end(), [](uint32_t s) { return s == 960; }));
    bool ok = false;
    const int peak = decodedPeak(cap.frames, 5, ok);
    XC_CHECK(ok);
    XC_CHECK(peak > 2000);
    XC_CHECK(lvl > 0.1f && lvl <= 1.0f);
    XC_CHECK_EQ(st.backend, std::string("fake"));
    XC_CHECK_EQ(st.rate, 16000);
    XC_CHECK(st.kbps > 10 && st.kbps < 60);
}

XC_TEST(voice, muted_sends_silence) {
    Captured cap;
    xc::VoiceChat v(std::make_unique<FakeAudioIn>(16000, 256));
    XC_REQUIRE(v.start(cap.sink(), true));
    XC_CHECK(v.muted());
    XC_CHECK(waitFor([&] { return v.state() == xc::VoiceChat::State::Muted; }, 500));
    std::this_thread::sleep_for(milliseconds(400));
    XC_CHECK(v.level() == 0.0f);
    v.stop();
    std::lock_guard<std::mutex> lk(cap.mu);
    XC_CHECK(cap.frames.size() >= 15);
    bool ok = false;
    const int peak = decodedPeak(cap.frames, 0, ok);
    XC_CHECK(ok);
    XC_CHECK(peak < 64);
}

XC_TEST(voice, mute_toggle_while_live) {
    Captured cap;
    xc::VoiceChat v(std::make_unique<FakeAudioIn>(16000, 256));
    XC_REQUIRE(v.start(cap.sink(), false));
    XC_CHECK(waitFor([&] { return v.state() == xc::VoiceChat::State::Live; }, 500));
    v.setMuted(true);
    XC_CHECK(waitFor([&] { return v.state() == xc::VoiceChat::State::Muted; }, 200));
    XC_CHECK(v.level() == 0.0f);
    const size_t before = cap.count();
    std::this_thread::sleep_for(milliseconds(200));
    XC_CHECK(cap.count() > before + 5);  // cadence continues while muted
    v.setMuted(false);
    XC_CHECK(waitFor([&] { return v.state() == xc::VoiceChat::State::Live; }, 200));
    v.stop();
}

XC_TEST(voice, open_failure_is_unavailable) {
    Captured cap;
    xc::VoiceChat v(std::make_unique<FakeAudioIn>(16000, 256, true));
    XC_REQUIRE(v.start(cap.sink(), false));
    XC_CHECK(waitFor([&] { return v.state() == xc::VoiceChat::State::Unavailable; }, 500));
    std::this_thread::sleep_for(milliseconds(50));
    XC_CHECK_EQ(cap.count(), static_cast<size_t>(0));
    XC_CHECK(v.level() == 0.0f);
    v.stop();
    XC_CHECK(v.state() == xc::VoiceChat::State::Off);
}

XC_TEST(voice, read_errors_end_unavailable) {
    Captured cap;
    xc::VoiceChat v(std::make_unique<FakeAudioIn>(16000, 256, false, 10));
    XC_REQUIRE(v.start(cap.sink(), false));
    XC_CHECK(waitFor([&] { return v.state() == xc::VoiceChat::State::Unavailable; }, 1000));
    XC_CHECK(v.stats().readErrors >= 5);
    // The ended thread can be restarted (fake keeps failing -> Unavailable again).
    XC_CHECK(v.start(cap.sink(), false));
    v.stop();
}

XC_TEST(voice, stop_is_fast_and_idempotent) {
    Captured cap;
    xc::VoiceChat v(std::make_unique<FakeAudioIn>(16000, 256));
    XC_REQUIRE(v.start(cap.sink(), false));
    std::this_thread::sleep_for(milliseconds(150));
    const auto t0 = steady_clock::now();
    v.stop();
    const auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    XC_CHECK(ms < 200);
    v.stop();
    XC_CHECK(v.state() == xc::VoiceChat::State::Off);
    // Restart after stop.
    XC_CHECK(v.start(cap.sink(), false));
    v.stop();
}

XC_TEST(voice, non_opus_rate_is_resampled) {
    Captured cap;
    xc::VoiceChat v(std::make_unique<FakeAudioIn>(44100, 441));
    XC_REQUIRE(v.start(cap.sink(), false));
    std::this_thread::sleep_for(milliseconds(500));
    const auto st = v.stats();
    v.stop();
    XC_CHECK_EQ(st.rate, 48000);
    std::lock_guard<std::mutex> lk(cap.mu);
    XC_CHECK(cap.frames.size() >= 20 && cap.frames.size() <= 32);
    bool ok = false;
    XC_CHECK(decodedPeak(cap.frames, 5, ok) > 2000);
    XC_CHECK(ok);
}

XC_TEST(voice, state_names) {
    XC_CHECK_EQ(std::string(xc::voiceStateName(xc::VoiceChat::State::Off)), std::string("off"));
    XC_CHECK_EQ(std::string(xc::voiceStateName(xc::VoiceChat::State::Starting)), std::string("starting"));
    XC_CHECK_EQ(std::string(xc::voiceStateName(xc::VoiceChat::State::Unavailable)), std::string("unavailable"));
    XC_CHECK_EQ(std::string(xc::voiceStateName(xc::VoiceChat::State::Muted)), std::string("muted"));
    XC_CHECK_EQ(std::string(xc::voiceStateName(xc::VoiceChat::State::Live)), std::string("live"));
}

XC_TEST(voice, mix_saturates) {
    int16_t dst[6] = {1000, 30000, -30000, INT16_MAX, INT16_MIN, -5};
    const int16_t src[6] = {-500, 5000, -5000, 1, -1, 5};
    xc::AudioPlayer::mixSaturate(dst, src, 6);
    XC_CHECK_EQ(dst[0], static_cast<int16_t>(500));
    XC_CHECK_EQ(dst[1], static_cast<int16_t>(INT16_MAX));
    XC_CHECK_EQ(dst[2], static_cast<int16_t>(INT16_MIN));
    XC_CHECK_EQ(dst[3], static_cast<int16_t>(INT16_MAX));
    XC_CHECK_EQ(dst[4], static_cast<int16_t>(INT16_MIN));
    XC_CHECK_EQ(dst[5], static_cast<int16_t>(0));
}

XC_TEST(voice, player_voice_api_without_device) {
    // Not initialised: pushes are ignored, stats stay zero, resets are safe.
    xc::AudioPlayer p;
    const uint8_t junk[3] = {0xf8, 0xff, 0xfe};
    p.pushVoiceOpus(junk, sizeof(junk), 2);
    p.resetVoice();
    p.reset();
    const auto vs = p.voiceStats();
    XC_CHECK_EQ(vs.packets, static_cast<uint64_t>(0));
    XC_CHECK_EQ(vs.bufferedMs, 0);
}

XC_TEST(voice, host_source_env) {
#ifndef XC_PS5
    setenv("XC_MIC_SOURCE", "none", 1);
    {
        auto s = xc::platform::createAudioIn();
        int r = 0, g = 0;
        XC_CHECK_EQ(std::string(s->backend()), std::string("none"));
        XC_CHECK(!s->open(r, g));
    }
    setenv("XC_MIC_SOURCE", "tone", 1);
    {
        auto s = xc::platform::createAudioIn();
        int r = 0, g = 0;
        XC_REQUIRE(s->open(r, g));
        XC_CHECK_EQ(r, 48000);
        XC_CHECK_EQ(g, 480);
        std::vector<int16_t> buf(static_cast<size_t>(g));
        XC_CHECK_EQ(s->read(buf.data(), 50), g);
        int peak = 0;
        for (int16_t x : buf) peak = std::max(peak, std::abs(static_cast<int>(x)));
        XC_CHECK(peak > 3000 && peak < 3400);  // -20 dBFS
        s->close();
    }
    unsetenv("XC_MIC_SOURCE");
#endif
}

XC_TEST(voice, player_voice_ring_cap) {
#ifndef XC_PS5
    // SDL's dummy driver drains the device in real time without touching hardware.
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    xc::AudioPlayer p;
    const bool ok = p.init();
    unsetenv("SDL_AUDIODRIVER");
    if (!ok) return;  // no SDL audio at all in this environment
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    XC_REQUIRE(enc != nullptr);
    std::vector<int16_t> pcm(960);
    for (int i = 0; i < 960; ++i) pcm[static_cast<size_t>(i)] = static_cast<int16_t>(6000.0 * std::sin(i * 0.057));
    uint8_t pkt[400];
    const int n = opus_encode(enc, pcm.data(), 960, pkt, sizeof(pkt));
    XC_REQUIRE(n > 0);
    // 400 ms burst: the cap (120 ms) trims back to 60 ms, so latency stays bounded.
    for (int i = 0; i < 20; ++i) p.pushVoiceOpus(pkt, static_cast<size_t>(n), i == 10 ? 1 : 0);
    const auto vs = p.voiceStats();
    XC_CHECK_EQ(vs.packets, static_cast<uint64_t>(20));
    XC_CHECK_EQ(vs.lost, static_cast<uint64_t>(1));
    XC_CHECK(vs.bufferedMs <= 120);
    XC_CHECK(vs.droppedMs >= 200);
    // Game stream untouched by voice pushes.
    XC_CHECK_EQ(p.stats().packets, static_cast<uint64_t>(0));
    p.reset();
    XC_CHECK_EQ(p.voiceStats().bufferedMs, 0);
    opus_encoder_destroy(enc);
    p.close();
#endif
}
