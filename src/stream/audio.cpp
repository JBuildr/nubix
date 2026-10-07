// Nubix — Opus audio playback.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// RTP (or pre-depacketised Opus) -> libopus 48 kHz stereo s16 -> ring buffer -> SDL audio callback.
//
// Jitter handling: playback starts once ~60 ms are buffered; on underrun the callback plays
// silence and re-primes; if the buffer grows past a high watermark the oldest audio is dropped
// back to the target. A small clock-skew servo (linear-interpolation stretch of at most 0.2 %)
// steers the smoothed depth towards the target so sender/receiver crystal drift never builds up.
// Ring, prebuffer, servo and resampler design are ported from green-nx
// (src/switch/stream/audio_player.cpp, GPL-3.0); the output side uses SDL instead of audout.
//
// Threads: pushRtp()/pushOpus()/reset() may be called from any thread (normally the
// libdatachannel track callback); the SDL audio thread only touches the ring under ringMu.
#include "stream/audio.hpp"

#include "core/log.hpp"

#include <SDL.h>
#include <opus.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace xc {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kMaxFrameSamples = 5760;                       // 120 ms per channel
constexpr int kInt16PerMs = kSampleRate * kChannels / 1000;  // 96 interleaved samples per ms
constexpr int kRingMs = 500;                                 // ring capacity
constexpr int kPrebufferMs = 60;                             // start playback at this depth
constexpr int kResumeMs = 40;                                // re-prime depth after an underrun
constexpr int kHighWatermarkMs = 150;                        // above this, shed audio...
constexpr int kShedTargetMs = 60;                            // ...down to here
constexpr int kMaxConcealFrames = 5;                         // PLC at most this many lost frames
constexpr Uint16 kDeviceSamples = 512;                       // ~10.7 ms; PS5 needs multiples of 256

// Clock-skew servo (see green-nx): ~1 s EMA of ring depth, 30 ppm per ms of error, clamp 0.2 %.
constexpr float kServoTargetMs = 60.0f;
constexpr float kServoEmaAlpha = 0.02f;
constexpr float kServoGain = 30e-6f;
constexpr float kServoMaxAdj = 2000e-6f;

}  // namespace

struct AudioPlayer::Impl {
    // ---- decoder side (guarded by decMu) ----
    std::mutex decMu;
    OpusDecoder* dec = nullptr;
    bool haveSeq = false;
    uint16_t expectedSeq = 0;
    int lastFrameSamples = kSampleRate / 50;  // 20 ms until the first packet tells us otherwise
    std::vector<int16_t> pcm;
    std::vector<int16_t> pcmOut;
    float depthEma = 0.0f;
    float servoAdj = 0.0f;
    float resamplePos = 0.0f;
    int16_t carry[2] = {0, 0};

    // ---- output side (guarded by ringMu) ----
    std::mutex ringMu;
    std::vector<int16_t> ring;
    int ringSize = 0;
    int readPos = 0, writePos = 0, count = 0;
    bool primed = false;
    int primeTarget = kPrebufferMs * kInt16PerMs;

    SDL_AudioDeviceID dev = 0;
    bool weInitedSdlAudio = false;
    std::atomic<bool> muted{false};

    // ---- stats ----
    std::atomic<uint64_t> packets{0}, lost{0}, late{0}, errors{0}, underruns{0}, droppedSamples{0};
    std::atomic<int> bufferedSamples{0};

    static void sdlCallback(void* user, Uint8* stream, int len) {
        static_cast<Impl*>(user)->fill(reinterpret_cast<int16_t*>(stream),
                                       len / static_cast<int>(sizeof(int16_t)));
    }

    void resetDecoderStateLocked() {
        if (dec) opus_decoder_ctl(dec, OPUS_RESET_STATE);
        haveSeq = false;
        expectedSeq = 0;
        lastFrameSamples = kSampleRate / 50;
        depthEma = 0.0f;
        servoAdj = 0.0f;
        resamplePos = 0.0f;
        carry[0] = carry[1] = 0;
    }

    void resetRing() {
        std::lock_guard<std::mutex> lk(ringMu);
        readPos = writePos = count = 0;
        primed = false;
        primeTarget = kPrebufferMs * kInt16PerMs;
        bufferedSamples.store(0, std::memory_order_relaxed);
    }

    // Decode one packet (data == nullptr -> PLC, fec -> in-band FEC of the *previous* frame)
    // and queue the PCM. Called with decMu held.
    void decodeAndQueue(const uint8_t* data, size_t n, int frameSamples, bool fec) {
        if (!dec) return;
        const int maxSamples = (data && !fec) ? kMaxFrameSamples : frameSamples;
        const int got = opus_decode(dec, data, static_cast<opus_int32>(data ? n : 0), pcm.data(),
                                    maxSamples, fec ? 1 : 0);
        if (got <= 0) {
            errors.fetch_add(1, std::memory_order_relaxed);
            if (errors.load(std::memory_order_relaxed) <= 5)
                XC_LOGW("audio: opus_decode failed: %s", opus_strerror(got));
            return;
        }
        if (data && !fec) lastFrameSamples = got;
        queuePcm(got);
    }

    // Servo + resample `samples` frames from pcm into the ring. Called with decMu held.
    void queuePcm(int samples) {
        int depthSamples;
        {
            std::lock_guard<std::mutex> lk(ringMu);
            depthSamples = count;
        }
        const float depthMs = static_cast<float>(depthSamples) / kInt16PerMs;
        depthEma += kServoEmaAlpha * (depthMs - depthEma);
        servoAdj = std::clamp((kServoTargetMs - depthEma) * kServoGain, -kServoMaxAdj, kServoMaxAdj);

        const int outSamples = servoResample(pcm.data(), samples, pcmOut.data(),
                                             static_cast<int>(pcmOut.size()) / kChannels);
        pushRing(pcmOut.data(), outSamples * kChannels);
    }

    // Linear-interpolation stretch, phase-continuous across frames (green-nx servo_resample).
    // step < 1 stretches (more output), step > 1 shrinks.
    int servoResample(const int16_t* in, int inSamples, int16_t* out, int maxOut) {
        const float step = 1.0f - servoAdj;
        float pos = resamplePos;
        int outN = 0;
        while (outN < maxOut) {
            const int i = static_cast<int>(pos);
            if (i >= inSamples) break;
            const float frac = pos - static_cast<float>(i);
            const int16_t l0 = (i == 0) ? carry[0] : in[(i - 1) * kChannels];
            const int16_t r0 = (i == 0) ? carry[1] : in[(i - 1) * kChannels + 1];
            const int16_t l1 = in[i * kChannels];
            const int16_t r1 = in[i * kChannels + 1];
            out[outN * kChannels] = static_cast<int16_t>(l0 + static_cast<float>(l1 - l0) * frac);
            out[outN * kChannels + 1] = static_cast<int16_t>(r0 + static_cast<float>(r1 - r0) * frac);
            ++outN;
            pos += step;
        }
        // Keep the fractional phase relative to the next frame (pos is >= inSamples here unless
        // the output buffer filled up, which cannot happen with the +1 % headroom).
        resamplePos = std::max(0.0f, pos - static_cast<float>(inSamples));
        carry[0] = in[(inSamples - 1) * kChannels];
        carry[1] = in[(inSamples - 1) * kChannels + 1];
        return outN;
    }

    void pushRing(const int16_t* data, int n) {
        if (n <= 0) return;
        const int high = kHighWatermarkMs * kInt16PerMs;
        const int target = kShedTargetMs * kInt16PerMs;
        std::lock_guard<std::mutex> lk(ringMu);
        if (ringSize == 0) return;
        if (n > ringSize) {
            data += n - ringSize;
            n = ringSize;
        }
        // Overflow: drop the oldest audio down to the target to bound latency.
        if (count + n > high) {
            int shed = std::min(count + n - target, count);
            if (shed > 0) {
                shed -= shed % kChannels;  // keep L/R alignment
                readPos = (readPos + shed) % ringSize;
                count -= shed;
                droppedSamples.fetch_add(static_cast<uint64_t>(shed), std::memory_order_relaxed);
            }
        }
        if (count + n > ringSize) {
            const int over = count + n - ringSize;
            readPos = (readPos + over) % ringSize;
            count -= over;
            droppedSamples.fetch_add(static_cast<uint64_t>(over), std::memory_order_relaxed);
        }
        const int first = std::min(n, ringSize - writePos);
        std::memcpy(&ring[writePos], data, static_cast<size_t>(first) * sizeof(int16_t));
        if (n > first)
            std::memcpy(&ring[0], data + first, static_cast<size_t>(n - first) * sizeof(int16_t));
        writePos = (writePos + n) % ringSize;
        count += n;
        if (!primed && count >= primeTarget) primed = true;
        bufferedSamples.store(count, std::memory_order_relaxed);
    }

    // SDL audio thread.
    void fill(int16_t* out, int need) {
        int given = 0;
        {
            std::lock_guard<std::mutex> lk(ringMu);
            if (primed) {
                const int give = std::min(need, count);
                const int first = std::min(give, ringSize - readPos);
                std::memcpy(out, &ring[readPos], static_cast<size_t>(first) * sizeof(int16_t));
                if (give > first)
                    std::memcpy(out + first, &ring[0], static_cast<size_t>(give - first) * sizeof(int16_t));
                readPos = (readPos + give) % ringSize;
                count -= give;
                given = give;
                if (given < need) {
                    // Ran dry: play silence and wait for a shallower cushion before resuming.
                    primed = false;
                    primeTarget = kResumeMs * kInt16PerMs;
                    underruns.fetch_add(1, std::memory_order_relaxed);
                }
            }
            bufferedSamples.store(count, std::memory_order_relaxed);
        }
        if (given < need)
            std::memset(out + given, 0, static_cast<size_t>(need - given) * sizeof(int16_t));
        if (muted.load(std::memory_order_relaxed))
            std::memset(out, 0, static_cast<size_t>(need) * sizeof(int16_t));
    }

    // Handle one Opus payload with `lostBefore` missing packets in front of it.
    void handleOpus(const uint8_t* data, size_t n, int lostBefore) {
        if (!dec) return;
        int frameSamples = lastFrameSamples;
        const int ns = opus_packet_get_nb_samples(data, static_cast<opus_int32>(n), kSampleRate);
        if (ns > 0 && ns <= kMaxFrameSamples) frameSamples = ns;

        if (lostBefore > 0) {
            lost.fetch_add(static_cast<uint64_t>(lostBefore), std::memory_order_relaxed);
            const int conceal = std::min(lostBefore, kMaxConcealFrames);
            // PLC for all but the last missing frame; the last one is recovered from this
            // packet's in-band FEC (LBRR) when present (libopus falls back to PLC otherwise).
            for (int i = 0; i < conceal - 1; ++i) decodeAndQueue(nullptr, 0, lastFrameSamples, false);
            decodeAndQueue(data, n, frameSamples, true);
        }
        decodeAndQueue(data, n, frameSamples, false);
        packets.fetch_add(1, std::memory_order_relaxed);
    }
};

AudioPlayer::AudioPlayer() : d_(new Impl) {}
AudioPlayer::~AudioPlayer() { close(); }

bool AudioPlayer::init() {
    close();
    Impl& d = *d_;

    {
        std::lock_guard<std::mutex> lk(d.decMu);
        int err = OPUS_OK;
        d.dec = opus_decoder_create(kSampleRate, kChannels, &err);
        if (err != OPUS_OK || !d.dec) {
            XC_LOGE("audio: opus_decoder_create failed: %s", opus_strerror(err));
            d.dec = nullptr;
            return false;
        }
        d.pcm.assign(static_cast<size_t>(kMaxFrameSamples) * kChannels, 0);
        // +1 % headroom: the servo stretches a frame by at most 0.2 %.
        d.pcmOut.assign(static_cast<size_t>(kMaxFrameSamples + kMaxFrameSamples / 100 + 2) * kChannels, 0);
        d.resetDecoderStateLocked();
    }
    {
        std::lock_guard<std::mutex> lk(d.ringMu);
        d.ringSize = kRingMs * kInt16PerMs;
        d.ring.assign(static_cast<size_t>(d.ringSize), 0);
    }
    d.resetRing();
    d.packets = 0;
    d.lost = 0;
    d.late = 0;
    d.errors = 0;
    d.underruns = 0;
    d.droppedSamples = 0;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            XC_LOGE("audio: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            close();
            return false;
        }
        d.weInitedSdlAudio = true;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples = kDeviceSamples;
    want.callback = &Impl::sdlCallback;
    want.userdata = &d;
    SDL_AudioSpec have;
    SDL_zero(have);
    // Only the buffer size may change; SDL converts anything else to our s16/48k/stereo.
    d.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (d.dev == 0) {
        XC_LOGE("audio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        close();
        return false;
    }
    XC_LOGI("audio: device open (%s, %d Hz, %d ch, %u samples/buffer), prebuffer %d ms",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", have.freq,
            have.channels, static_cast<unsigned>(have.samples), kPrebufferMs);
    SDL_PauseAudioDevice(d.dev, 0);
    return true;
}

void AudioPlayer::pushRtp(const uint8_t* rtp, size_t n) {
    Impl& d = *d_;
    // RFC 3550 fixed header (12 bytes) + CSRC list + optional header extension + padding.
    if (!rtp || n < 12 || (rtp[0] >> 6) != 2) {
        d.errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool padding = (rtp[0] & 0x20) != 0;
    const bool extension = (rtp[0] & 0x10) != 0;
    const size_t csrc = rtp[0] & 0x0f;
    const uint8_t pt = rtp[1] & 0x7f;
    if (pt >= 72 && pt <= 76) return;  // RTCP muxed on the same port (SR/RR/...), not media
    size_t off = 12 + 4 * csrc;
    size_t end = n;
    if (off > end) {
        d.errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (extension) {
        if (off + 4 > end) {
            d.errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const size_t extWords = (static_cast<size_t>(rtp[off + 2]) << 8) | rtp[off + 3];
        off += 4 + extWords * 4;
        if (off > end) {
            d.errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (padding) {
        const size_t pad = rtp[n - 1];
        if (pad == 0 || pad > end - off) {
            d.errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        end -= pad;
    }
    if (end == off) return;  // no payload (keep-alive)
    const uint16_t seq = static_cast<uint16_t>((rtp[2] << 8) | rtp[3]);

    std::lock_guard<std::mutex> lk(d.decMu);
    if (!d.dec) return;
    int lostBefore = 0;
    if (d.haveSeq) {
        const int16_t diff = static_cast<int16_t>(seq - d.expectedSeq);
        if (diff < 0) {
            // Duplicate or arrived after we already concealed it: too late to play.
            d.late.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        lostBefore = diff;
    }
    d.haveSeq = true;
    d.expectedSeq = static_cast<uint16_t>(seq + 1);
    d.handleOpus(rtp + off, end - off, lostBefore);
}

void AudioPlayer::pushOpus(const uint8_t* opus, size_t n, int lostBefore) {
    if (!opus || n == 0) return;
    std::lock_guard<std::mutex> lk(d_->decMu);
    if (!d_->dec) return;
    d_->handleOpus(opus, n, lostBefore < 0 ? 0 : lostBefore);
}

void AudioPlayer::reset() {
    {
        std::lock_guard<std::mutex> lk(d_->decMu);
        d_->resetDecoderStateLocked();
    }
    d_->resetRing();
}

int AudioPlayer::bufferedMs() const {
    return d_->bufferedSamples.load(std::memory_order_relaxed) / kInt16PerMs;
}

void AudioPlayer::setMuted(bool muted) { d_->muted.store(muted, std::memory_order_relaxed); }

AudioPlayer::Stats AudioPlayer::stats() const {
    Stats s;
    s.packets = d_->packets.load(std::memory_order_relaxed);
    s.lost = d_->lost.load(std::memory_order_relaxed);
    s.late = d_->late.load(std::memory_order_relaxed);
    s.decodeErrors = d_->errors.load(std::memory_order_relaxed);
    s.underruns = d_->underruns.load(std::memory_order_relaxed);
    s.droppedMs = d_->droppedSamples.load(std::memory_order_relaxed) / kInt16PerMs;
    s.bufferedMs = bufferedMs();
    return s;
}

void AudioPlayer::close() {
    if (!d_) return;
    Impl& d = *d_;
    // Close the device first: SDL joins its audio thread, so fill() is no longer running.
    if (d.dev) {
        SDL_CloseAudioDevice(d.dev);
        d.dev = 0;
        XC_LOGI("audio: closed (%llu packets, %llu lost, %llu late, %llu underruns, %llu ms dropped)",
                static_cast<unsigned long long>(d.packets.load()),
                static_cast<unsigned long long>(d.lost.load()),
                static_cast<unsigned long long>(d.late.load()),
                static_cast<unsigned long long>(d.underruns.load()),
                static_cast<unsigned long long>(d.droppedSamples.load() / kInt16PerMs));
    }
    if (d.weInitedSdlAudio) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        d.weInitedSdlAudio = false;
    }
    {
        std::lock_guard<std::mutex> lk(d.decMu);
        if (d.dec) {
            opus_decoder_destroy(d.dec);
            d.dec = nullptr;
        }
        d.resetDecoderStateLocked();
    }
    d.resetRing();
}

}  // namespace xc
