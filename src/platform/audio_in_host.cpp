// Nubix — host microphone sources: SDL capture, synthetic tone, "none".
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// XC_MIC_SOURCE selects the source: "tone" (440 Hz sine at -20 dBFS, paced in real time),
// "none" (open() fails: simulates a console without a microphone), anything else / unset:
// the default SDL capture device at 48 kHz mono s16 with a 480-sample (10 ms) grain.
#ifndef XC_PS5

#include "platform/audio_in.hpp"

#include <SDL.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "core/log.hpp"

namespace xc {
namespace platform {
namespace {

constexpr int kRate = 48000;
constexpr int kGrain = 480;  // 10 ms

// ---- XC_MIC_SOURCE=none ----------------------------------------------------------------------
class NoneAudioIn final : public AudioIn {
public:
    bool open(int&, int&) override {
        XC_LOGI("voice: host mic source 'none' (XC_MIC_SOURCE=none)");
        return false;
    }
    int read(int16_t*, int) override { return -1; }
    uint32_t silentState() override { return kAudioInSilentNoDevice; }
    void close() override {}
    const char* backend() const override { return "none"; }
};

// ---- XC_MIC_SOURCE=tone ----------------------------------------------------------------------
class ToneAudioIn final : public AudioIn {
public:
    bool open(int& rate, int& grain) override {
        rate = kRate;
        grain = kGrain;
        phase_ = 0.0;
        next_ = Clock::now();
        open_ = true;
        XC_LOGI("voice: host mic source 'tone' (440 Hz, -20 dBFS, %d Hz, grain %d)", kRate, kGrain);
        return true;
    }

    int read(int16_t* buf, int timeoutMs) override {
        if (!open_) return -1;
        // Pace like a real device: one grain per 10 ms of wall clock.
        const auto now = Clock::now();
        if (next_ > now) {
            const auto wait = next_ - now;
            if (wait > std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs));
                return 0;
            }
            std::this_thread::sleep_until(next_);
        } else if (now - next_ > std::chrono::milliseconds(200)) {
            next_ = now;  // fell far behind (debugger, suspend): do not burst
        }
        next_ += std::chrono::microseconds(1000000LL * kGrain / kRate);
        const double amp = 32767.0 * 0.1;  // -20 dBFS
        const double step = 2.0 * M_PI * 440.0 / kRate;
        for (int i = 0; i < kGrain; ++i) {
            buf[i] = static_cast<int16_t>(std::lround(amp * std::sin(phase_)));
            phase_ += step;
            if (phase_ > 2.0 * M_PI) phase_ -= 2.0 * M_PI;
        }
        return kGrain;
    }

    uint32_t silentState() override { return 0; }
    void close() override { open_ = false; }
    const char* backend() const override { return "tone"; }

private:
    using Clock = std::chrono::steady_clock;
    bool open_ = false;
    double phase_ = 0.0;
    Clock::time_point next_{};
};

// ---- SDL capture -----------------------------------------------------------------------------
class SdlAudioIn final : public AudioIn {
public:
    ~SdlAudioIn() override { close(); }

    bool open(int& rate, int& grain) override {
        close();
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            XC_LOGW("voice: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return false;
        }
        sdlInited_ = true;
        const int nDev = SDL_GetNumAudioDevices(1);
        const char* name = nDev > 0 ? SDL_GetAudioDeviceName(0, 1) : nullptr;
        XC_LOGI("voice: host SDL capture devices: %d (default '%s')", nDev, name ? name : "?");

        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = kGrain;
        want.callback = nullptr;  // queue mode: SDL_DequeueAudio
        SDL_AudioSpec have;
        SDL_zero(have);
        // No ALLOW_* flags: SDL converts to exactly s16 / 48 kHz / mono for us.
        dev_ = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
        if (dev_ == 0) {
            XC_LOGW("voice: SDL_OpenAudioDevice(capture) failed: %s", SDL_GetError());
            close();
            return false;
        }
        SDL_PauseAudioDevice(dev_, 0);
        rate = kRate;
        grain = kGrain;
        XC_LOGI("voice: host SDL capture open (%s, %d Hz, %d ch, %u samples)",
                SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", have.freq,
                have.channels, static_cast<unsigned>(have.samples));
        return true;
    }

    int read(int16_t* buf, int timeoutMs) override {
        if (dev_ == 0) return -1;
        const Uint32 need = static_cast<Uint32>(kGrain * sizeof(int16_t));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
        while (SDL_GetQueuedAudioSize(dev_) < need) {
            if (SDL_GetAudioDeviceStatus(dev_) == SDL_AUDIO_STOPPED) return -1;  // unplugged
            if (std::chrono::steady_clock::now() >= deadline) return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const Uint32 got = SDL_DequeueAudio(dev_, buf, need);
        if (got < need) std::memset(reinterpret_cast<uint8_t*>(buf) + got, 0, need - got);
        return kGrain;
    }

    uint32_t silentState() override { return 0; }

    void close() override {
        if (dev_ != 0) {
            SDL_CloseAudioDevice(dev_);
            dev_ = 0;
        }
        if (sdlInited_) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);  // reference counted by SDL
            sdlInited_ = false;
        }
    }

    const char* backend() const override { return "sdl"; }

private:
    SDL_AudioDeviceID dev_ = 0;
    bool sdlInited_ = false;
};

}  // namespace

std::unique_ptr<AudioIn> createAudioIn() {
    const char* env = std::getenv("XC_MIC_SOURCE");
    const std::string src = env ? env : "";
    if (src == "tone") return std::make_unique<ToneAudioIn>();
    if (src == "none") return std::make_unique<NoneAudioIn>();
    return std::make_unique<SdlAudioIn>();
}

}  // namespace platform
}  // namespace xc

#endif  // !XC_PS5
