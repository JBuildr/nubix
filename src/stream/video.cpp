// Nubix — H.264 decoder (FFmpeg).
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Software H.264 decode with libavcodec: slice threads, AV_CODEC_FLAG_LOW_DELAY + FLAG2_FAST,
// default error concealment (guess MVs + deblock). One access unit in, at most one frame out.
// Decoder setup and the corrupt-frame detection follow green-nx
// (src/switch/stream/video_decoder.cpp, GPL-3.0).
//
// Threads: decode()/flush() run on the stream worker thread; latest() is called by the render
// thread. Frame handoff is double-buffered: the decoder writes into its private `work` frame and
// then moves the reference into the shared `pending` slot (replacing an unconsumed older frame);
// latest() moves `pending` out into a fresh AVFrame for the caller. Only refcounted buffers move,
// no pixel data is copied.
#include "stream/video.hpp"

#include "core/log.hpp"

#include <atomic>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

namespace xc {

namespace {

using Clock = std::chrono::steady_clock;

// After this many consecutive access units that produced no picture (without an explicit
// error), assume the decoder lost its references and ask for a keyframe.
constexpr int kMaxSilentAus = 5;

// FFmpeg log forwarding. The h264 decoder prints one line per damaged slice when packets are
// lost, so everything goes to Debug and is rate-limited.
std::atomic<int64_t> g_avLogWindowStart{0};
std::atomic<int> g_avLogCount{0};
constexpr int kAvLogMaxPerSecond = 20;

void avLogCallback(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    if (level > AV_LOG_ERROR && logLevel() > LogLevel::Debug) return;

    const int64_t nowS =
        std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    int64_t start = g_avLogWindowStart.load(std::memory_order_relaxed);
    if (nowS != start && g_avLogWindowStart.compare_exchange_strong(start, nowS)) {
        const int suppressed = g_avLogCount.exchange(0) - kAvLogMaxPerSecond;
        if (suppressed > 0) XC_LOGD("ffmpeg: %d log lines suppressed", suppressed);
    }
    if (g_avLogCount.fetch_add(1, std::memory_order_relaxed) >= kAvLogMaxPerSecond) return;

    char line[512];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (len == 0) return;
    if (level <= AV_LOG_FATAL)
        XC_LOGE("ffmpeg: %s", line);
    else
        XC_LOGD("ffmpeg: %s", line);
}

void installAvLog() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(avLogCallback);
    });
}

std::string avErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

struct VideoDecoder::Impl {
    // ---- decoder (guarded by decodeMu) ----
    std::mutex decodeMu;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* work = nullptr;
    int silentAus = 0;
    bool gotFrame = false;
    Clock::time_point windowStart{};
    double windowSumMs = 0;
    uint64_t windowCount = 0;
    bool windowPublished = false;

    // ---- handoff (guarded by handoffMu) ----
    std::mutex handoffMu;
    AVFrame* pending = nullptr;
    bool hasPending = false;

    // ---- stats (lock-free reads) ----
    std::atomic<double> avgMs{0.0};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> replaced{0};
    std::atomic<bool> needKey{false};
    std::atomic<int> width{0};
    std::atomic<int> height{0};

    void markError() {
        errors.fetch_add(1, std::memory_order_relaxed);
        needKey.store(true, std::memory_order_relaxed);
    }

    // Move `work` into the handoff slot. Called with decodeMu held.
    void publish() {
        width.store(work->width, std::memory_order_relaxed);
        height.store(work->height, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(handoffMu);
        if (hasPending) {
            replaced.fetch_add(1, std::memory_order_relaxed);
            av_frame_unref(pending);
        }
        av_frame_move_ref(pending, work);
        hasPending = true;
    }

    void recordTiming(double ms) {
        const auto now = Clock::now();
        if (windowCount == 0 && windowSumMs == 0) windowStart = now;
        windowSumMs += ms;
        ++windowCount;
        if (now - windowStart >= std::chrono::seconds(1)) {
            avgMs.store(windowSumMs / static_cast<double>(windowCount), std::memory_order_relaxed);
            windowPublished = true;
            windowSumMs = 0;
            windowCount = 0;
        } else if (!windowPublished) {
            // First second of the stream: report the running average.
            avgMs.store(windowSumMs / static_cast<double>(windowCount), std::memory_order_relaxed);
        }
    }

    void resetCounters() {
        silentAus = 0;
        gotFrame = false;
        windowSumMs = 0;
        windowCount = 0;
        windowPublished = false;
        avgMs.store(0.0);
        frames.store(0);
        errors.store(0);
        replaced.store(0);
        needKey.store(false);
        width.store(0);
        height.store(0);
    }

    void dropPending() {
        std::lock_guard<std::mutex> lk(handoffMu);
        if (pending) av_frame_unref(pending);
        hasPending = false;
    }

    void freeAll() {
        if (ctx) avcodec_free_context(&ctx);
        if (pkt) av_packet_free(&pkt);
        if (work) av_frame_free(&work);
        std::lock_guard<std::mutex> lk(handoffMu);
        if (pending) av_frame_free(&pending);
        hasPending = false;
    }
};

VideoDecoder::VideoDecoder() : d_(new Impl) {}
VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::init(int threads) {
    installAvLog();
    std::lock_guard<std::mutex> lk(d_->decodeMu);
    d_->freeAll();
    d_->resetCounters();

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        XC_LOGE("video: FFmpeg has no H.264 decoder");
        return false;
    }
    d_->ctx = avcodec_alloc_context3(codec);
    d_->pkt = av_packet_alloc();
    d_->work = av_frame_alloc();
    {
        std::lock_guard<std::mutex> hl(d_->handoffMu);
        d_->pending = av_frame_alloc();
    }
    if (!d_->ctx || !d_->pkt || !d_->work || !d_->pending) {
        XC_LOGE("video: out of memory allocating decoder");
        d_->freeAll();
        return false;
    }

    AVCodecContext* c = d_->ctx;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    c->flags2 |= AV_CODEC_FLAG2_FAST;
    // Slice threading adds no latency (frame threading would add threads-1 frames of delay).
    c->thread_type = FF_THREAD_SLICE;
    c->thread_count = threads > 0 ? threads : 1;
    c->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    c->workaround_bugs = FF_BUG_AUTODETECT;

    const int rc = avcodec_open2(c, codec, nullptr);
    if (rc < 0) {
        XC_LOGE("video: avcodec_open2 failed: %s", avErr(rc).c_str());
        d_->freeAll();
        return false;
    }
    XC_LOGI("video: %s decoder ready (slice threads=%d, low delay)", codec->name, c->thread_count);
    return true;
}

bool VideoDecoder::decode(const uint8_t* au, size_t n, int64_t pts) {
    std::lock_guard<std::mutex> lk(d_->decodeMu);
    Impl& d = *d_;
    if (!d.ctx) return false;
    if (!au || n == 0) return true;
    if (n > static_cast<size_t>(INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE)) {
        d.markError();
        return false;
    }

    const auto t0 = Clock::now();
    bool ok = true;

    av_packet_unref(d.pkt);
    if (av_new_packet(d.pkt, static_cast<int>(n)) < 0) {  // zero-fills the input padding
        XC_LOGE("video: av_new_packet(%zu) failed", n);
        return false;
    }
    std::memcpy(d.pkt->data, au, n);
    d.pkt->pts = pts;
    d.pkt->dts = pts;

    int produced = 0;
    auto drain = [&]() {
        for (;;) {
            const int r = avcodec_receive_frame(d.ctx, d.work);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) {
                XC_LOGD("video: receive_frame: %s", avErr(r).c_str());
                d.markError();
                ok = false;
                break;
            }
            if (d.work->decode_error_flags != 0 || (d.work->flags & AV_FRAME_FLAG_CORRUPT)) {
                // Concealed frame: still shown (better than freezing), but ask for a keyframe.
                d.markError();
                ok = false;
            }
            ++produced;
            d.frames.fetch_add(1, std::memory_order_relaxed);
            d.publish();  // leaves `work` empty
        }
    };

    int r = avcodec_send_packet(d.ctx, d.pkt);
    if (r == AVERROR(EAGAIN)) {
        // Should not happen with low delay since we always drain, but be safe.
        drain();
        r = avcodec_send_packet(d.ctx, d.pkt);
    }
    if (r < 0) {
        XC_LOGD("video: send_packet (%zu bytes): %s", n, avErr(r).c_str());
        d.markError();
        ok = false;
    }
    drain();
    av_packet_unref(d.pkt);

    if (produced > 0) {
        d.gotFrame = true;
        d.silentAus = 0;
    } else if (ok && ++d.silentAus >= kMaxSilentAus) {
        // SPS/PPS-only AUs legitimately produce nothing, but a run of them means the decoder
        // is discarding pictures (missing references / no IDR yet).
        d.silentAus = 0;
        d.needKey.store(true, std::memory_order_relaxed);
        if (d.gotFrame) ok = false;
    }

    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    d.recordTiming(ms);
    return ok;
}

AVFrame* VideoDecoder::latest() {
    std::lock_guard<std::mutex> lk(d_->handoffMu);
    if (!d_->hasPending || !d_->pending) return nullptr;
    AVFrame* out = av_frame_alloc();
    if (!out) return nullptr;
    av_frame_move_ref(out, d_->pending);
    d_->hasPending = false;
    return out;
}

double VideoDecoder::avgDecodeMs() const { return d_->avgMs.load(std::memory_order_relaxed); }

uint64_t VideoDecoder::framesDecoded() const { return d_->frames.load(std::memory_order_relaxed); }

bool VideoDecoder::needKeyframe() { return d_->needKey.exchange(false, std::memory_order_relaxed); }

uint64_t VideoDecoder::decodeErrors() const { return d_->errors.load(std::memory_order_relaxed); }

int VideoDecoder::width() const { return d_->width.load(std::memory_order_relaxed); }

int VideoDecoder::height() const { return d_->height.load(std::memory_order_relaxed); }

void VideoDecoder::flush() {
    std::lock_guard<std::mutex> lk(d_->decodeMu);
    if (d_->ctx) avcodec_flush_buffers(d_->ctx);
    if (d_->work) av_frame_unref(d_->work);
    d_->silentAus = 0;
    d_->gotFrame = false;
    d_->dropPending();
}

void VideoDecoder::close() {
    if (!d_) return;
    std::lock_guard<std::mutex> lk(d_->decodeMu);
    if (d_->ctx) {
        XC_LOGI("video: closing decoder (%llu frames, %llu errors, %llu unconsumed)",
                static_cast<unsigned long long>(d_->frames.load()),
                static_cast<unsigned long long>(d_->errors.load()),
                static_cast<unsigned long long>(d_->replaced.load()));
    }
    d_->freeAll();
}

}  // namespace xc
