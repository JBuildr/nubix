// Nubix — H.264 decoder (FFmpeg libavcodec, software, slice threads, low delay).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

struct AVFrame;

namespace xc {

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Open the H.264 decoder (thread_type = FF_THREAD_SLICE, AV_CODEC_FLAG_LOW_DELAY, FLAG2_FAST).
    bool init(int threads = 4);

    // Decode one Annex-B access unit. False on decode error (caller requests a keyframe).
    // Called from the stream worker thread.
    bool decode(const uint8_t* au, size_t n, int64_t pts);

    // Thread-safe frame handoff: returns a NEW reference to the most recent decoded frame that
    // has not been returned before, or nullptr if none. Caller owns it (av_frame_free).
    AVFrame* latest();

    // Average decode time in milliseconds over the last second (stats overlay).
    double avgDecodeMs() const;

    // Decoded frames since init (stats overlay).
    uint64_t framesDecoded() const;

    // Free decoder and buffered frames. Idempotent.
    void close();

    // ---- additive API (media module) ----

    // True if a decode error / corrupt frame happened since the last call (flag is cleared).
    // The caller should send PLI + control keyframe request (rate-limited).
    bool needKeyframe();

    // Number of decode errors (send/receive failures + frames flagged corrupt) since init.
    uint64_t decodeErrors() const;

    // Dimensions of the most recently decoded frame (0 before the first frame).
    int width() const;
    int height() const;

    // Drop decoder state and any pending frame (e.g. after a stream reconnect). Keeps the
    // decoder open; the next frame must be a keyframe.
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
