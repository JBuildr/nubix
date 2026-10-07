// Nubix — Opus audio: RTP -> libopus (48 kHz stereo) -> ring buffer -> SDL audio device.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace xc {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Create the Opus decoder and open the SDL audio device (48 kHz, s16, stereo).
    // SDL_INIT_AUDIO must already be initialised.
    bool init();

    // Push one raw audio RTP packet (header included); decodes and queues PCM. Thread-safe.
    void pushRtp(const uint8_t* rtp, size_t n);

    // Buffered audio latency in milliseconds (stats overlay).
    int bufferedMs() const;

    // Mute/unmute output without closing the device.
    void setMuted(bool muted);

    // Close the audio device and decoder. Idempotent.
    void close();

    // ---- additive API (media module) ----

    // Push one Opus payload that was already de-packetised (e.g. by RtpOpusReceiver).
    // lostBefore = packets missing immediately before this one; they are concealed with
    // in-band FEC (last one) and PLC. Thread-safe. Do not mix with pushRtp() in one session.
    void pushOpus(const uint8_t* opus, size_t n, int lostBefore = 0);

    // Drop all buffered audio and decoder/sequence state (stream reconnect). Thread-safe.
    void reset();

    struct Stats {
        uint64_t packets = 0;      // Opus packets decoded
        uint64_t lost = 0;         // packets concealed (gap in sequence numbers)
        uint64_t late = 0;         // duplicate / out-of-order packets dropped
        uint64_t decodeErrors = 0; // opus_decode failures / malformed RTP
        uint64_t underruns = 0;    // callback ran dry after playback had started
        uint64_t droppedMs = 0;    // audio discarded to bound latency (overflow)
        int bufferedMs = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
