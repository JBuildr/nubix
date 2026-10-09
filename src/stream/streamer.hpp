// Nubix — orchestrates one streaming session: auth refresh, gssv start/queue/provision,
// SDP/ICE exchange, WebRTC, control/message/input channels, decode, audio, RTCP, keepalive.
// Runs on its own worker thread; the main thread polls state and pulls frames.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/auth.hpp"
#include "core/config.hpp"
#include "core/gssv.hpp"
#include "core/protocol.hpp"

struct AVFrame;

namespace xc {

enum class StreamState { Idle, Starting, Queued, Provisioning, Connecting, Streaming, Ended, Failed };

// Human readable name for a state (logs / UI).
const char* streamStateName(StreamState s);

// Microphone / voice chat state of the running stream, for the stream menu and indicator.
// Off = voice chat disabled in Settings, not streaming, or the mic is still being opened.
enum class MicState { Off, Unavailable, Muted, Live };

// Live statistics for the stream overlay.
struct StreamStats {
    int width = 0, height = 0;
    double fps = 0;            // decoded frames per second
    double bitrateKbps = 0;    // received video bitrate
    double decodeMs = 0;       // average decode time
    double lossPercent = 0;    // video packet loss over the last second
    int rttMs = -1;            // unknown = -1
    int audioBufferMs = 0;
    bool voiceNegotiated = false;    // the SDP answer had chatStream >= 1
    bool voiceRenegotiated = false;  // the chat renegotiation (second offer) was applied
    int micTxKbps = 0;               // microphone RTP sent, kbps over the last second
    uint64_t chatRxPackets = 0;      // separate chat-SSRC voice packets played (this session)
};

class Streamer {
public:
    Streamer(Config& cfg, Auth& auth);
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    // Start a session. Non-blocking: the work (releasing a previous session that is still
    // winding down, decoder/audio init, the worker threads) runs on the Streamer's own lifecycle
    // thread; state() reads Starting right away. False if a session is already active (started
    // and neither stopped nor finished on its own).
    // f2pOnly (cloud): the title is only playable through the free-to-play offering
    // (Title::f2pOnly) - start with xgpuwebf2p instead of xgpuweb, regardless of f2pFallback.
    bool start(SessionKind kind, const std::string& titleOrServerId, bool f2pOnly = false);

    // Current state, status line and queue position (-1 when not queued). Thread-safe.
    StreamState state() const;
    std::string statusText() const;
    int queuePosition() const;

    // Estimated queue wait in seconds while state() == Queued (gssv /v1/waittime, cloud only),
    // -1 when unknown or not queued. Thread-safe.
    int queueWaitSeconds() const;

    // Latest gamepad state; called each frame from the main thread.
    void setGamepad(const GamepadState& st);

    // Pop a pending rumble command from the server. Thread-safe.
    bool takeVibration(Vibration& out);

    // Newest decoded frame not yet returned (new reference, caller av_frame_free's it), or nullptr.
    AVFrame* currentFrame();

    // Snapshot of stream statistics.
    StreamStats stats() const;

    // Send a keyframe request (PLI + control message), rate-limited.
    void requestKeyframe();

    // Microphone mute (voice chat). Thread-safe. The flag survives reconnects and later
    // sessions of this process (not saved to disk); a muted mic keeps sending silence.
    void setMicMuted(bool muted);
    bool micMuted() const;
    // Current microphone state (Off while voice chat is off, the mic is still opening, or no
    // stream runs). Thread-safe, cheap enough to call every frame.
    MicState micState() const;
    // Peak input level of the last ~100 ms, 0..1 (0 when muted / unavailable / off).
    float micLevel() const;

    // Request the session to stop without blocking: signals the worker (in-flight requests
    // abort within ~1 s) and queues the teardown (DELETE on gssv, close PC, release
    // decoder/audio) on the lifecycle thread. A later start() runs after it. Idempotent.
    void stopAsync();

    // Stop the session synchronously and shut the lifecycle thread down (app exit / destructor):
    // cancels queued starts, joins every thread. The final gssv DELETE is bounded to ~5 s.
    // Safe to call in any state; idempotent. start() works again afterwards.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
