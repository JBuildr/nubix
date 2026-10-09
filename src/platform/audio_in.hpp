// Nubix — microphone capture abstraction (PS5 sceAudioIn / host SDL capture).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace xc {
namespace platform {

// Mono s16 microphone source. One instance per stream; open/read/close from ONE thread.
// An instance may be re-opened after close().
class AudioIn {
public:
    virtual ~AudioIn() = default;
    // Open the device. On success rate (Hz) and grain (samples per read) are set. Logs every attempt.
    virtual bool open(int& rate, int& grain) = 0;
    // Read exactly one grain into buf (capacity >= grain). Blocks at most ~timeoutMs
    // (PS5: blocks one hardware grain, timeout ignored). Returns samples (>0), 0 on timeout,
    // <0 on fatal error (device gone).
    virtual int read(int16_t* buf, int timeoutMs) = 0;
    // 0 = live. PS5: sceAudioInGetSilentState bits (0x1 no device, 0x2 low priority,
    // 0x8 unable format, others = user mute). Host: 0. Cheap enough to call every 500 ms.
    virtual uint32_t silentState() = 0;
    virtual void close() = 0;                 // idempotent
    virtual const char* backend() const = 0;  // "ps5-audioin", "sdl", "tone", "none"
    // Raw backend code of the last failed read() (PS5: sceAudioInInput rc), 0 if none/unknown.
    virtual int32_t lastError() const { return 0; }
};

// Silent-state bit: no input device for the user (headset / controller mic absent).
constexpr uint32_t kAudioInSilentNoDevice = 0x1;

// PS5: sceAudioIn backend. Host: XC_MIC_SOURCE=tone -> 440 Hz -20 dBFS synthetic,
// =none -> open() fails, else SDL capture (default device, 48000 Hz, mono, s16, 480-sample grain).
std::unique_ptr<AudioIn> createAudioIn();

}  // namespace platform
}  // namespace xc
