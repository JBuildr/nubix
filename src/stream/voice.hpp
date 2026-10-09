// Nubix — voice chat capture: microphone -> Opus 20 ms frames for the in-stream chat RTP.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xc {
namespace platform {
class AudioIn;
}

// Microphone -> Opus 20 ms frames, on its own capture thread.
class VoiceChat {
public:
    // opus frame, samples in 48 kHz units (always 960 per 20 ms frame), marker = first frame
    // after start(). Called on the capture thread; must not block for long (just sends RTP).
    using FrameSink = std::function<void(const uint8_t* opus, size_t n, uint32_t samples48k, bool marker)>;

    enum class State { Off, Starting, Unavailable, Muted, Live };

    // source == nullptr -> platform::createAudioIn(). Tests inject a fake source.
    explicit VoiceChat(std::unique_ptr<platform::AudioIn> source = nullptr);
    ~VoiceChat();  // stop()
    VoiceChat(const VoiceChat&) = delete;
    VoiceChat& operator=(const VoiceChat&) = delete;

    // Spawn the capture thread (non-blocking; device open happens on it -> State Starting).
    // False if already running. With no usable device the thread ends with State Unavailable.
    bool start(FrameSink sink, bool startMuted);
    // Stop and join (bounded by one grain, <= ~50 ms). Idempotent. State -> Off.
    void stop();

    void setMuted(bool muted);  // thread-safe; muted = encode+send digital silence
    bool muted() const;
    State state() const;  // thread-safe
    float level() const;  // 0..1 peak of the last ~100 ms, 0 when muted/unavailable

    struct Stats {
        std::string backend;  // AudioIn::backend()
        int rate = 0;         // capture/encoder rate
        uint64_t frames = 0;  // frames handed to the sink
        uint64_t readErrors = 0;
        int32_t lastError = 0;  // AudioIn::lastError() of the last failed read
        int kbps = 0;  // encoded payload kbps over the last second
    };
    Stats stats() const;

    // Opus encoder settings applied by the capture thread.
    static constexpr int kBitrate = 32000, kComplexity = 5, kLossPerc = 10, kFrameMs = 20;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

const char* voiceStateName(VoiceChat::State s);  // "off","starting","unavailable","muted","live"

}  // namespace xc
