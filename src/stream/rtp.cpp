// Nubix — RTP receive side.
//
// H.264 reassembly and loss policy ported/adapted from green-nx
// (src/switch/stream/video_jitter.cpp, GPL-3.0, (c) rmrf404 and contributors):
//   * padding-only packets consume sequence numbers but are never "loss";
//   * a frame is emitted as soon as it is internally complete;
//   * an incomplete frame is held only while a NACK retransmission can still arrive;
//   * after unrecoverable loss only a real IDR (NAL type 5) restarts output — xCloud repeats
//     SPS/PPS without IDR and resuming on those produces self-sustaining garbage.
// Statistics follow RFC 3550 Appendix A.1 / A.3 / A.8.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "stream/rtp.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

#include "core/log.hpp"

namespace xc {

uint64_t rtpNowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

namespace {

uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Parsed view of one RTP packet (RFC 3550 §5.1).
struct RtpView {
    uint8_t pt = 0;
    bool marker = false;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    bool padded = false;  // had the P bit set (libwebrtc bandwidth probing)
};

bool parseRtp(const uint8_t* p, size_t n, RtpView& v) {
    if (!p || n < 12) return false;
    if ((p[0] >> 6) != 2) return false;
    const uint8_t pt = p[1] & 0x7F;
    if (pt >= 64 && pt <= 95) return false;  // RTCP (RFC 5761 §4)
    const size_t cc = p[0] & 0x0F;
    const bool ext = (p[0] & 0x10) != 0;
    const bool pad = (p[0] & 0x20) != 0;
    size_t hdr = 12 + 4 * cc;
    if (n < hdr) return false;
    if (ext) {
        if (n < hdr + 4) return false;
        const size_t words = (static_cast<size_t>(p[hdr + 2]) << 8) | p[hdr + 3];
        hdr += 4 + 4 * words;
        if (n < hdr) return false;
    }
    size_t plen = n - hdr;
    if (pad) {
        const size_t padLen = p[n - 1];
        plen = padLen <= plen ? plen - padLen : 0;
    }
    v.pt = pt;
    v.marker = (p[1] & 0x80) != 0;
    v.seq = static_cast<uint16_t>((p[2] << 8) | p[3]);
    v.ts = (static_cast<uint32_t>(p[4]) << 24) | (static_cast<uint32_t>(p[5]) << 16) |
           (static_cast<uint32_t>(p[6]) << 8) | p[7];
    v.ssrc = (static_cast<uint32_t>(p[8]) << 24) | (static_cast<uint32_t>(p[9]) << 16) |
             (static_cast<uint32_t>(p[10]) << 8) | p[11];
    v.payload = p + hdr;
    v.payloadLen = plen;
    v.padded = pad;
    return true;
}

// Sequence number unwrapping to a monotonic 64-bit value around the highest seen.
int64_t unwrapSeq(int64_t highestExt, uint16_t seq) {
    const int16_t diff = static_cast<int16_t>(static_cast<uint16_t>(seq - static_cast<uint16_t>(highestExt)));
    return highestExt + diff;
}

// Accepts an SSRC change only after a few consecutive packets of the new SSRC, so a stray packet
// cannot flush the whole receiver.
struct SsrcLatch {
    uint32_t ssrc = 0;
    bool have = false;
    uint32_t candidate = 0;
    int candidateCount = 0;

    enum class Result { Same, First, Switched, Foreign };
    Result check(uint32_t s) {
        if (!have) {
            have = true;
            ssrc = s;
            candidateCount = 0;
            return Result::First;
        }
        if (s == ssrc) {
            candidateCount = 0;
            return Result::Same;
        }
        if (s == candidate) {
            if (++candidateCount >= 3) {
                ssrc = s;
                candidateCount = 0;
                return Result::Switched;
            }
        } else {
            candidate = s;
            candidateCount = 1;
        }
        return Result::Foreign;
    }
};

constexpr uint8_t kStartCode[4] = {0, 0, 0, 1};
constexpr size_t kMaxAccessUnitBytes = 4u * 1024u * 1024u;
constexpr int64_t kMaxSeqGap = 1000;          // larger forward jump = stream discontinuity
constexpr size_t kMaxBufferedPackets = 4096;  // reorder buffer safety cap
constexpr size_t kMaxQueuedFrames = 16;       // output queue cap (consumer stalled)
constexpr int kMaxNackRetries = 3;
constexpr size_t kMaxNacksPerCall = 256;
constexpr uint64_t kNackHistoryMs = 1000;     // keep NACK send times for late RTT samples

// H.264 helpers ---------------------------------------------------------------------------------

uint8_t nalType(uint8_t b) { return b & 0x1F; }

// First bit of a slice header after the NAL byte: first_mb_in_slice is ue(v); a leading '1' bit
// encodes 0, i.e. the slice starts a new picture.
bool sliceStartsPicture(const uint8_t* sliceAfterNalHeader, size_t n) {
    return n > 0 && (sliceAfterNalHeader[0] & 0x80) != 0;
}

// True if an RTP payload is the first packet of an access unit (as far as the payload tells).
bool payloadStartsAccessUnit(const uint8_t* p, size_t n) {
    if (n == 0) return false;
    const uint8_t t = nalType(p[0]);
    if (t == 1 || t == 5) return sliceStartsPicture(p + 1, n - 1);
    if (t >= 6 && t <= 9) return true;  // SEI, SPS, PPS, AUD precede the slices of a picture
    if (t == 24) {                      // STAP-A: look at the first aggregated NAL
        if (n < 4) return false;
        const size_t len = (static_cast<size_t>(p[1]) << 8) | p[2];
        if (len == 0 || 3 + len > n) return false;
        return payloadStartsAccessUnit(p + 3, len);
    }
    if (t == 28) {  // FU-A start fragment
        if (n < 3 || !(p[1] & 0x80)) return false;
        const uint8_t inner = nalType(p[1]);
        if (inner == 1 || inner == 5) return sliceStartsPicture(p + 2, n - 2);
        return inner >= 6 && inner <= 9;
    }
    return false;
}

// True if the payload can begin a frame's packet run (single NAL, STAP-A or FU-A start).
bool isPartitionHead(const uint8_t* p, size_t n) {
    if (n == 0) return false;
    const uint8_t t = nalType(p[0]);
    if (t == 28) return n > 1 && (p[1] & 0x80) != 0;
    return t >= 1 && t <= 24;
}

}  // namespace

// =================================================================================================
// RtpStatsTracker
// =================================================================================================

RtpStatsTracker::RtpStatsTracker(const RtpStatsTracker& o) { *this = o; }

RtpStatsTracker& RtpStatsTracker::operator=(const RtpStatsTracker& o) {
    if (this == &o) return *this;
    std::unique_lock<std::mutex> a(mu_, std::defer_lock), b(o.mu_, std::defer_lock);
    std::lock(a, b);
    st_ = o.st_;
    init_ = o.init_;
    maxSeq_ = o.maxSeq_;
    cycles_ = o.cycles_;
    baseSeq_ = o.baseSeq_;
    received_ = o.received_;
    expectedPrior_ = o.expectedPrior_;
    receivedPrior_ = o.receivedPrior_;
    badSeq_ = o.badSeq_;
    transit_ = o.transit_;
    jitter_ = o.jitter_;
    haveTransit_ = o.haveTransit_;
    lastSrLocalMs_ = o.lastSrLocalMs_;
    haveSr_ = o.haveSr_;
    return *this;
}

void RtpStatsTracker::initSeq(uint16_t seq) {
    baseSeq_ = seq;
    maxSeq_ = seq;
    badSeq_ = 0x10001;  // RTP_SEQ_MOD + 1: never equal to a 16-bit seq
    cycles_ = 0;
    received_ = 0;
    receivedPrior_ = 0;
    expectedPrior_ = 0;
}

void RtpStatsTracker::onPacket(uint32_t ssrc, uint16_t seq, uint32_t rtpTs, uint32_t clockRate, size_t bytes) {
    onPacketAt(ssrc, seq, rtpTs, clockRate, bytes, nowUs());
}

void RtpStatsTracker::onPacketAt(uint32_t ssrc, uint16_t seq, uint32_t rtpTs, uint32_t clockRate, size_t bytes,
                                 uint64_t arrivalUs) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!init_ || ssrc != st_.ssrc) {
        // First packet: keep a sender report that arrived before it (normal right after DTLS),
        // so the first RRs already carry LSR/DLSR. A new SSRC on an initialised tracker starts
        // from scratch: an SR of the previous source is meaningless now.
        const bool keepSr = !init_ && haveSr_;
        const uint32_t keptLsr = keepSr ? st_.lastSr : 0;
        st_ = RtpReceiveStats{};
        st_.ssrc = ssrc;
        st_.lastSr = keptLsr;
        if (!keepSr) {
            haveSr_ = false;
            lastSrLocalMs_ = 0;
        }
        initSeq(seq);
        init_ = true;
        haveTransit_ = false;
        jitter_ = 0;
    } else {
        // RFC 3550 A.1 update_seq (without the probation phase: xCloud is a trusted, single source).
        constexpr uint16_t kMaxDropout = 3000;
        constexpr uint16_t kMaxMisorder = 100;
        const uint16_t udelta = static_cast<uint16_t>(seq - maxSeq_);
        if (udelta < kMaxDropout) {
            if (seq < maxSeq_) cycles_ += 0x10000;  // wrapped
            maxSeq_ = seq;
        } else if (udelta <= 0x10000 - kMaxMisorder) {
            // Very large jump: resync only if the next packet continues from here.
            if (static_cast<uint32_t>(seq) == badSeq_) {
                initSeq(seq);
            } else {
                badSeq_ = (static_cast<uint32_t>(seq) + 1) & 0xFFFF;
                return;
            }
        }
        // else: duplicate or reordered packet — counted below, does not move maxSeq_.
    }
    ++received_;
    ++st_.packets;
    st_.bytes += bytes;

    // RFC 3550 A.8 interarrival jitter, in RTP timestamp units.
    if (clockRate > 0) {
        const uint32_t arrival = static_cast<uint32_t>((arrivalUs * clockRate) / 1000000u);
        const uint32_t transit = arrival - rtpTs;
        if (haveTransit_) {
            const int32_t d = static_cast<int32_t>(transit - static_cast<uint32_t>(transit_));
            const double ad = std::fabs(static_cast<double>(d));
            // Ignore absurd deltas (timestamp jumps on source restart).
            if (ad < static_cast<double>(clockRate) * 10.0) jitter_ += (ad - jitter_) / 16.0;
        }
        transit_ = static_cast<double>(transit);
        haveTransit_ = true;
    }
}

void RtpStatsTracker::onSenderReport(uint64_t ntpTimestamp) { onSenderReportAt(ntpTimestamp, rtpNowMs()); }

void RtpStatsTracker::onSenderReportAt(uint64_t ntpTimestamp, uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    st_.lastSr = static_cast<uint32_t>((ntpTimestamp >> 16) & 0xFFFFFFFFu);
    lastSrLocalMs_ = nowMs;
    haveSr_ = true;
}

RtpReceiveStats RtpStatsTracker::snapshotForReport() { return snapshotForReportAt(rtpNowMs()); }

RtpReceiveStats RtpStatsTracker::snapshotForReportAt(uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!init_) return st_;
    const uint32_t extendedMax = cycles_ + maxSeq_;
    const uint32_t expected = extendedMax - baseSeq_ + 1;
    int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);
    lost = std::max<int64_t>(-0x800000, std::min<int64_t>(0x7FFFFF, lost));  // 24-bit signed

    const uint32_t expectedInterval = expected - expectedPrior_;
    expectedPrior_ = expected;
    const uint32_t receivedInterval = received_ - receivedPrior_;
    receivedPrior_ = received_;
    const int64_t lostInterval = static_cast<int64_t>(expectedInterval) - static_cast<int64_t>(receivedInterval);
    uint8_t fraction = 0;
    if (expectedInterval != 0 && lostInterval > 0)
        fraction = static_cast<uint8_t>(std::min<int64_t>(255, (lostInterval << 8) / expectedInterval));

    st_.fractionLost = fraction;
    st_.cumulativeLost = static_cast<int32_t>(lost);
    st_.extHighestSeq = extendedMax;
    st_.jitter = static_cast<uint32_t>(jitter_);
    if (haveSr_) {
        // DLSR is in units of 1/65536 s (green-nx: getting this unit wrong pins xCloud's
        // encoder at its starvation bitrate).
        const uint64_t delta = nowMs >= lastSrLocalMs_ ? nowMs - lastSrLocalMs_ : 0;
        st_.delaySinceLastSr = static_cast<uint32_t>(std::min<uint64_t>((delta << 16) / 1000u, 0xFFFFFFFFu));
    } else {
        st_.lastSr = 0;
        st_.delaySinceLastSr = 0;
    }
    return st_;
}

bool RtpStatsTracker::hasData() const {
    std::lock_guard<std::mutex> lk(mu_);
    return init_;
}

void RtpStatsTracker::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    st_ = RtpReceiveStats{};
    init_ = false;
    maxSeq_ = 0;
    cycles_ = baseSeq_ = received_ = expectedPrior_ = receivedPrior_ = 0;
    badSeq_ = 0x10001;
    transit_ = jitter_ = 0;
    haveTransit_ = false;
    lastSrLocalMs_ = 0;
    haveSr_ = false;
}

// =================================================================================================
// RtpH264Depacketizer
// =================================================================================================

struct RtpH264Depacketizer::Impl {
    struct Packet {
        uint32_t ts = 0;
        bool marker = false;
        bool padding = false;  // no media payload (bandwidth probe); consumes a seq only
        std::vector<uint8_t> payload;
    };
    struct Missing {
        uint64_t detectedMs = 0;
        uint64_t lastNackMs = 0;
        int nacks = 0;
    };
    struct Frame {
        std::vector<uint8_t> au;
        uint32_t ts = 0;
        bool key = false;
    };

    mutable std::mutex mu;
    RtpStatsTracker stats;
    uint8_t ptFilter = 0;
    SsrcLatch latch;

    bool haveSeq = false;
    int64_t highestExt = 0;  // highest extended seq received
    int64_t nextExt = 0;     // first extended seq not yet consumed
    std::map<int64_t, Packet> buf;
    std::map<int64_t, Missing> missing;
    std::map<int64_t, uint64_t> nackHistory;  // given-up seqs that were NACKed -> last NACK time
    int backJumps = 0;

    std::deque<Frame> out;
    bool waitingKey = true;
    bool keyPending = false;
    uint64_t lastKeyRequestMs = 0;
    bool keyClockStarted = false;

    bool haveLastTs = false;
    uint32_t lastEmittedTs = 0;
    bool haveDropTs = false;  // timestamp of the last frame given up on: late packets of it are junk
    uint32_t dropTs = 0;
    uint32_t frameInterval = 0;  // smoothed RTP ts delta between consecutive frames

    double srttMs = 0;  // NACK round-trip estimate
    uint32_t minWaitMs = 40, maxWaitMs = 250, keyRetryMs = 1000;
    RtpVideoCounters c;

    uint64_t lossWaitMs() const {
        double w = minWaitMs;
        if (srttMs > 0) w = std::max(w, srttMs * 1.5 + 10.0);
        return static_cast<uint64_t>(std::min<double>(w, maxWaitMs));
    }
    uint64_t nackRetryMs() const {
        return srttMs > 0 ? static_cast<uint64_t>(std::max(15.0, srttMs * 1.2)) : 30;
    }

    void rttSample(uint64_t sampleMs) {
        if (sampleMs > 2000) return;
        const double s = static_cast<double>(sampleMs);
        srttMs = srttMs <= 0 ? s : srttMs * 0.875 + s * 0.125;
    }

    void requireKeyframe() {
        if (!waitingKey) XC_LOGD("rtp: video loss, waiting for IDR");
        waitingKey = true;
        keyPending = true;
    }

    void clearStream() {
        buf.clear();
        missing.clear();
        nackHistory.clear();
        haveSeq = false;
        backJumps = 0;
        haveLastTs = false;
        haveDropTs = false;
    }

    void resetAll() {
        clearStream();
        out.clear();
        stats.reset();
        latch = SsrcLatch{};
        waitingKey = true;
        keyPending = false;
        keyClockStarted = false;
        frameInterval = 0;
        srttMs = 0;
        c = RtpVideoCounters{};
    }

    void retireMissingBefore(int64_t ext) {
        while (!missing.empty() && missing.begin()->first < ext) {
            if (missing.begin()->second.nacks > 0) nackHistory[missing.begin()->first] = missing.begin()->second.lastNackMs;
            missing.erase(missing.begin());
        }
        while (!buf.empty() && buf.begin()->first < ext) buf.erase(buf.begin());
    }

    void pruneNackHistory(uint64_t nowMs) {
        for (auto it = nackHistory.begin(); it != nackHistory.end();) {
            if (nowMs - it->second > kNackHistoryMs || nackHistory.size() > 2048) it = nackHistory.erase(it);
            else ++it;
        }
    }

    void push(const uint8_t* pkt, size_t n, uint64_t nowMs) {
        RtpView v;
        if (!parseRtp(pkt, n, v)) return;
        if (ptFilter && v.pt != ptFilter) return;

        switch (latch.check(v.ssrc)) {
        case SsrcLatch::Result::Foreign:
            return;
        case SsrcLatch::Result::Switched:
            XC_LOGI("rtp: video SSRC changed to %08x, resyncing", v.ssrc);
            clearStream();
            out.clear();
            stats.reset();
            requireKeyframe();
            break;
        case SsrcLatch::Result::First:
            XC_LOGD("rtp: video SSRC %08x", v.ssrc);
            break;
        case SsrcLatch::Result::Same:
            break;
        }
        if (!keyClockStarted) {
            keyClockStarted = true;
            lastKeyRequestMs = nowMs;
        }
        ++c.packets;
        stats.onPacketAt(v.ssrc, v.seq, v.ts, 90000, v.payloadLen, nowMs * 1000);

        int64_t ext;
        if (!haveSeq) {
            haveSeq = true;
            ext = static_cast<int64_t>(v.seq) + (int64_t(1) << 20);
            highestExt = ext;
            nextExt = ext;
        } else {
            ext = unwrapSeq(highestExt, v.seq);
        }

        if (ext < nextExt) {
            // Late (its slot was already consumed or given up) or duplicate.
            ++c.lateOrDuplicate;
            auto h = nackHistory.find(ext);
            if (h != nackHistory.end()) {
                rttSample(nowMs - h->second);
                nackHistory.erase(h);
            }
            if (ext < nextExt - kMaxSeqGap) {
                // Sender restarted its sequence numbering backwards: resync after a few packets.
                if (++backJumps >= 3) {
                    XC_LOGI("rtp: video sequence jumped backwards, resyncing");
                    clearStream();
                    requireKeyframe();
                    push(pkt, n, nowMs);
                }
            }
            return;
        }
        backJumps = 0;
        if (buf.count(ext)) {
            ++c.lateOrDuplicate;
            return;
        }

        if (ext > highestExt) {
            const int64_t gap = ext - highestExt - 1;
            if (gap > kMaxSeqGap) {
                XC_LOGI("rtp: video sequence discontinuity (%lld packets), resyncing", static_cast<long long>(gap));
                clearStream();
                requireKeyframe();
                haveSeq = true;
                highestExt = nextExt = ext;
            } else {
                for (int64_t s = highestExt + 1; s < ext; ++s) missing[s] = Missing{nowMs, 0, 0};
                highestExt = ext;
            }
        } else {
            auto m = missing.find(ext);
            if (m != missing.end()) {
                if (m->second.nacks > 0) {
                    ++c.recovered;
                    rttSample(nowMs - m->second.lastNackMs);
                }
                missing.erase(m);
            }
        }

        Packet p;
        p.ts = v.ts;
        p.marker = v.marker;
        p.padding = v.payloadLen == 0;
        if (!p.padding) p.payload.assign(v.payload, v.payload + v.payloadLen);
        buf.emplace(ext, std::move(p));

        if (buf.size() > kMaxBufferedPackets) {
            XC_LOGW("rtp: video reorder buffer overflow, dropping");
            retireMissingBefore(highestExt + 1);
            nextExt = highestExt + 1;
            ++c.framesDropped;
            requireKeyframe();
        }
        drain(nowMs);
        pruneNackHistory(nowMs);
    }

    // Build an access unit from buf[first..last]. False if the payload sequence is malformed.
    bool assemble(int64_t first, int64_t last, Frame& f) {
        f.au.clear();
        f.key = false;
        bool fuOpen = false;
        bool sawHead = false;
        for (int64_t e = first; e <= last; ++e) {
            auto it = buf.find(e);
            if (it == buf.end()) return false;
            const Packet& pk = it->second;
            if (pk.padding) continue;
            const uint8_t* p = pk.payload.data();
            const size_t n = pk.payload.size();
            if (!sawHead) {
                if (!isPartitionHead(p, n)) return false;
                sawHead = true;
                f.ts = pk.ts;
            }
            const uint8_t t = nalType(p[0]);
            if (t >= 1 && t <= 23) {
                if (fuOpen) return false;  // FU-A without end fragment
                f.au.insert(f.au.end(), kStartCode, kStartCode + 4);
                f.au.insert(f.au.end(), p, p + n);
                if (t == 5) f.key = true;
            } else if (t == 24) {  // STAP-A
                if (fuOpen) return false;
                size_t i = 1;
                while (i + 2 <= n) {
                    const size_t len = (static_cast<size_t>(p[i]) << 8) | p[i + 1];
                    i += 2;
                    if (len == 0 || i + len > n) return false;
                    f.au.insert(f.au.end(), kStartCode, kStartCode + 4);
                    f.au.insert(f.au.end(), p + i, p + i + len);
                    if (nalType(p[i]) == 5) f.key = true;
                    i += len;
                }
            } else if (t == 28) {  // FU-A
                if (n < 2) return false;
                const bool start = (p[1] & 0x80) != 0, end = (p[1] & 0x40) != 0;
                if (start) {
                    if (fuOpen) return false;
                    fuOpen = true;
                    f.au.insert(f.au.end(), kStartCode, kStartCode + 4);
                    f.au.push_back(static_cast<uint8_t>((p[0] & 0xE0) | (p[1] & 0x1F)));
                    if (nalType(p[1]) == 5) f.key = true;
                } else if (!fuOpen) {
                    return false;
                }
                f.au.insert(f.au.end(), p + 2, p + n);
                if (end) fuOpen = false;
            } else {
                // STAP-B / MTAP / FU-B are not allowed in packetization-mode=1; skip reserved types.
                continue;
            }
            if (f.au.size() > kMaxAccessUnitBytes) return false;
        }
        if (fuOpen) return false;
        return sawHead && !f.au.empty();
    }

    void emit(Frame&& f) {
        if (haveLastTs) {
            const uint32_t d = f.ts - lastEmittedTs;
            if (d > 0 && d < 90000) frameInterval = frameInterval ? (frameInterval * 7 + d) / 8 : d;
        }
        haveLastTs = true;
        lastEmittedTs = f.ts;
        if (waitingKey) {
            if (!f.key) {
                ++c.framesDropped;
                return;
            }
            waitingKey = false;
            XC_LOGD("rtp: IDR received, video output resumes");
        }
        if (out.size() >= kMaxQueuedFrames) {
            // Consumer stalled: everything queued is now useless as reference chain.
            XC_LOGW("rtp: video frame queue overflow (%zu), dropping and waiting for IDR", out.size());
            c.framesDropped += out.size() + 1;
            out.clear();
            requireKeyframe();
            return;
        }
        ++c.framesEmitted;
        out.push_back(std::move(f));
    }

    // Consume packets [nextExt..last] and continue after them.
    void consumeThrough(int64_t last) {
        retireMissingBefore(last + 1);
        nextExt = last + 1;
    }

    // Give up on the loss blocking the head of the buffer. gapExt = first missing seq.
    // brokenFrameStart = ext of the incomplete frame's first packet, or -1 if the gap is at the head.
    void giveUp(int64_t gapExt, bool frameStartedBeforeGap) {
        // First present packet after the gap.
        auto p = buf.upper_bound(gapExt);
        if (p == buf.end()) {
            // Cannot happen (highestExt is always present), but stay safe.
            consumeThrough(highestExt);
            requireKeyframe();
            return;
        }
        // Skip leading padding to find the real next media packet.
        auto media = p;
        while (media != buf.end() && media->second.padding) ++media;

        if (frameStartedBeforeGap) {
            // The frame at the head is broken: drop all its packets (same timestamp), plus anything
            // up to the next frame start.
            const uint32_t brokenTs = buf.begin()->second.ts;
            haveDropTs = true;
            dropTs = brokenTs;
            int64_t last = gapExt;
            for (auto it = buf.begin(); it != buf.end(); ++it) {
                if (it->first > gapExt && !it->second.padding && it->second.ts != brokenTs) break;
                last = it->first;
            }
            ++c.framesDropped;
            consumeThrough(last);
            requireKeyframe();
            return;
        }

        // Gap at the head: lost packets were padding, a whole frame, or the start of the next frame.
        if (media == buf.end()) {
            // Only padding follows; nothing decodable lost so far.
            consumeThrough(std::prev(buf.end())->first);
            return;
        }
        const Packet& mp = media->second;
        const bool clean = payloadStartsAccessUnit(mp.payload.data(), mp.payload.size());
        if (clean) {
            // Was a whole frame lost? Compare the timestamp step with the usual frame interval.
            bool frameLost = false;
            if (haveLastTs && frameInterval > 0) {
                const uint32_t step = mp.ts - lastEmittedTs;
                frameLost = step > frameInterval + frameInterval / 2;
            }
            nextExt = media->first;
            retireMissingBefore(nextExt);
            if (frameLost) {
                ++c.framesDropped;
                requireKeyframe();
            }
            return;
        }
        // The next frame lost its beginning: drop it entirely.
        const uint32_t brokenTs = mp.ts;
        haveDropTs = true;
        dropTs = brokenTs;
        int64_t last = media->first;
        for (auto it = media; it != buf.end(); ++it) {
            if (!it->second.padding && it->second.ts != brokenTs) break;
            last = it->first;
        }
        ++c.framesDropped;
        consumeThrough(last);
        requireKeyframe();
    }

    void drain(uint64_t nowMs) {
        for (int guard = 0; guard < 100000; ++guard) {
            // Leading padding packets are consumed without further ado.
            auto head = buf.find(nextExt);
            while (head != buf.end() && head->second.padding) {
                consumeThrough(nextExt);
                head = buf.find(nextExt);
            }
            if (nextExt > highestExt) return;  // all consumed
            if (head == buf.end()) {
                // Head missing.
                auto m = missing.find(nextExt);
                const uint64_t since = m != missing.end() ? m->second.detectedMs : nowMs;
                if (nowMs - since >= lossWaitMs()) {
                    giveUp(nextExt, false);
                    continue;
                }
                return;
            }
            // Remaining packets of a frame that was already given up on.
            if (haveDropTs && head->second.ts == dropTs) {
                consumeThrough(nextExt);
                continue;
            }
            // Scan the frame starting at nextExt.
            const uint32_t ts = head->second.ts;
            int64_t e = nextExt;
            enum { Complete, Gap, Pending } state = Pending;
            int64_t gapAt = 0;
            for (;;) {
                auto it = buf.find(e);
                const Packet& pk = it->second;
                if (!pk.padding && pk.marker) {
                    state = Complete;
                    break;
                }
                const int64_t nx = e + 1;
                if (nx > highestExt) {
                    state = Pending;
                    break;
                }
                auto nit = buf.find(nx);
                if (nit == buf.end()) {
                    state = Gap;
                    gapAt = nx;
                    break;
                }
                if (!nit->second.padding && nit->second.ts != ts) {
                    state = Complete;  // timestamp changed without marker: frame ended at e
                    break;
                }
                e = nx;
            }
            if (state == Pending) return;
            if (state == Gap) {
                auto m = missing.find(gapAt);
                const uint64_t since = m != missing.end() ? m->second.detectedMs : nowMs;
                if (nowMs - since >= lossWaitMs()) {
                    giveUp(gapAt, true);
                    continue;
                }
                return;
            }
            // Complete frame [nextExt..e]; trailing padding with the same ts is consumed later.
            Frame f;
            const bool ok = assemble(nextExt, e, f);
            consumeThrough(e);
            if (!ok) {
                ++c.framesDropped;
                requireKeyframe();
                continue;
            }
            emit(std::move(f));
        }
    }

    std::vector<uint16_t> takeNacks(uint64_t nowMs) {
        std::vector<uint16_t> outSeqs;
        const uint64_t retry = nackRetryMs();
        const uint64_t useful = std::max<uint64_t>(lossWaitMs(), 1);
        for (auto& kv : missing) {
            if (outSeqs.size() >= kMaxNacksPerCall) break;
            Missing& m = kv.second;
            if (nowMs - m.detectedMs >= useful + retry) continue;  // too late to help
            const bool due = m.nacks == 0 || (m.nacks < kMaxNackRetries && nowMs - m.lastNackMs >= retry);
            if (!due) continue;
            ++m.nacks;
            m.lastNackMs = nowMs;
            outSeqs.push_back(static_cast<uint16_t>(kv.first & 0xFFFF));
        }
        c.nacksSent += outSeqs.size();
        return outSeqs;
    }
};

RtpH264Depacketizer::RtpH264Depacketizer() : d_(new Impl) {}
RtpH264Depacketizer::~RtpH264Depacketizer() = default;

void RtpH264Depacketizer::push(const uint8_t* pkt, size_t n) { pushAt(pkt, n, rtpNowMs()); }

void RtpH264Depacketizer::pushAt(const uint8_t* pkt, size_t n, uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->push(pkt, n, nowMs);
}

bool RtpH264Depacketizer::popFrame(std::vector<uint8_t>& annexB, uint32_t& rtpTs, bool& isKeyframe) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->out.empty()) {
        annexB.clear();
        rtpTs = 0;
        isKeyframe = false;
        return false;
    }
    Impl::Frame& f = d_->out.front();
    annexB.swap(f.au);
    rtpTs = f.ts;
    isKeyframe = f.key;
    d_->out.pop_front();
    return true;
}

std::vector<uint16_t> RtpH264Depacketizer::takeNackList() { return takeNackListAt(rtpNowMs()); }

std::vector<uint16_t> RtpH264Depacketizer::takeNackListAt(uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(d_->mu);
    // Time also advances without packets: give up on stale losses so needKeyframe() fires.
    if (d_->haveSeq) d_->drain(nowMs);
    return d_->takeNacks(nowMs);
}

bool RtpH264Depacketizer::needKeyframe() { return needKeyframeAt(rtpNowMs()); }

bool RtpH264Depacketizer::needKeyframeAt(uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->haveSeq) d_->drain(nowMs);
    bool need = false;
    if (d_->keyPending) {
        need = true;
    } else if (d_->waitingKey && d_->keyClockStarted && nowMs - d_->lastKeyRequestMs >= d_->keyRetryMs) {
        need = true;  // PLI lost or ignored: ask again
    }
    if (need) {
        d_->keyPending = false;
        d_->lastKeyRequestMs = nowMs;
        ++d_->c.keyframeRequests;
    }
    return need;
}

RtpStatsTracker& RtpH264Depacketizer::stats() { return d_->stats; }

void RtpH264Depacketizer::reset() {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->resetAll();
}

void RtpH264Depacketizer::setLossWait(uint32_t minLossWaitMs, uint32_t maxLossWaitMs) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->minWaitMs = minLossWaitMs;
    d_->maxWaitMs = std::max(minLossWaitMs, maxLossWaitMs);
}

void RtpH264Depacketizer::setKeyframeRetryMs(uint32_t ms) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->keyRetryMs = std::max<uint32_t>(ms, 50);
}

void RtpH264Depacketizer::setPayloadType(uint8_t pt) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->ptFilter = pt & 0x7F;
}

RtpVideoCounters RtpH264Depacketizer::counters() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    RtpVideoCounters c = d_->c;
    c.rttMs = static_cast<uint32_t>(d_->srttMs + 0.5);
    c.waitingForKeyframe = d_->waitingKey;
    return c;
}

uint32_t RtpH264Depacketizer::ssrc() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->latch.have ? d_->latch.ssrc : 0;
}

// =================================================================================================
// RtpOpusReceiver
// =================================================================================================

struct RtpOpusReceiver::Impl {
    struct Packet {
        uint32_t ts = 0;
        uint64_t arrivalMs = 0;
        std::vector<uint8_t> payload;
    };

    // Small on purpose: the player's ~60 ms prebuffer absorbs it, so a swapped pair is played in
    // order without adding steady-state latency.
    static constexpr size_t kReorderPackets = 2;  // packets buffered past a gap before skipping it
    static constexpr uint64_t kReorderMs = 20;    // or this long since the first packet past the gap
    static constexpr size_t kMaxBuffered = 256;
    static constexpr int64_t kMaxGap = 500;

    mutable std::mutex mu;
    RtpStatsTracker stats;
    SsrcLatch latch;
    bool haveSeq = false;
    int64_t highestExt = 0, nextExt = 0;
    std::map<int64_t, Packet> buf;
    int pendingLost = 0;  // lost count carried over an empty (DTX/padding) packet
    int backJumps = 0;

    void clearStream() {
        buf.clear();
        haveSeq = false;
        pendingLost = 0;
        backJumps = 0;
    }

    void push(const uint8_t* pkt, size_t n, uint64_t nowMs) {
        RtpView v;
        if (!parseRtp(pkt, n, v)) return;
        switch (latch.check(v.ssrc)) {
        case SsrcLatch::Result::Foreign:
            return;
        case SsrcLatch::Result::Switched:
            XC_LOGI("rtp: audio SSRC changed to %08x", v.ssrc);
            clearStream();
            stats.reset();
            break;
        default:
            break;
        }
        stats.onPacketAt(v.ssrc, v.seq, v.ts, 48000, v.payloadLen, nowMs * 1000);

        int64_t ext;
        if (!haveSeq) {
            haveSeq = true;
            ext = static_cast<int64_t>(v.seq) + (int64_t(1) << 20);
            highestExt = nextExt = ext;
        } else {
            ext = unwrapSeq(highestExt, v.seq);
        }
        if (ext < nextExt) {
            if (ext < nextExt - kMaxGap && ++backJumps >= 3) {
                clearStream();
                push(pkt, n, nowMs);
            }
            return;  // late or duplicate
        }
        backJumps = 0;
        if (ext > highestExt) {
            if (ext - highestExt > kMaxGap) {
                // Discontinuity: deliver what we have as-is, restart after it.
                buf.clear();
                pendingLost = 0;
                nextExt = ext;
            }
            highestExt = ext;
        }
        if (buf.count(ext)) return;
        Packet p;
        p.ts = v.ts;
        p.arrivalMs = nowMs;
        p.payload.assign(v.payload, v.payload + v.payloadLen);
        buf.emplace(ext, std::move(p));
        while (buf.size() > kMaxBuffered) {
            // Consumer is not popping: drop the oldest.
            nextExt = buf.begin()->first + 1;
            buf.erase(buf.begin());
            pendingLost = 0;
        }
    }

    bool pop(std::vector<uint8_t>& opus, uint32_t& rtpTs, int& lostBefore, uint64_t nowMs) {
        while (!buf.empty()) {
            auto it = buf.begin();
            int lost = 0;
            if (it->first != nextExt) {
                const bool skip = buf.size() >= kReorderPackets || nowMs - it->second.arrivalMs >= kReorderMs;
                if (!skip) return false;
                lost = static_cast<int>(std::min<int64_t>(it->first - nextExt, INT_MAX / 2));
            }
            nextExt = it->first + 1;
            Packet p = std::move(it->second);
            buf.erase(it);
            const int total = lost + pendingLost;
            if (p.payload.empty()) {
                pendingLost = total;  // DTX/padding: nothing to decode, keep loss count for the next
                continue;
            }
            pendingLost = 0;
            opus.swap(p.payload);
            rtpTs = p.ts;
            lostBefore = total;
            return true;
        }
        return false;
    }
};

RtpOpusReceiver::RtpOpusReceiver() : d_(new Impl) {}
RtpOpusReceiver::~RtpOpusReceiver() = default;

void RtpOpusReceiver::push(const uint8_t* pkt, size_t n) { pushAt(pkt, n, rtpNowMs()); }

void RtpOpusReceiver::pushAt(const uint8_t* pkt, size_t n, uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->push(pkt, n, nowMs);
}

bool RtpOpusReceiver::pop(std::vector<uint8_t>& opus, uint32_t& rtpTs, int& lostBefore) {
    return popAt(opus, rtpTs, lostBefore, rtpNowMs());
}

bool RtpOpusReceiver::popAt(std::vector<uint8_t>& opus, uint32_t& rtpTs, int& lostBefore, uint64_t nowMs) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->pop(opus, rtpTs, lostBefore, nowMs)) return true;
    opus.clear();
    rtpTs = 0;
    lostBefore = 0;
    return false;
}

RtpStatsTracker& RtpOpusReceiver::stats() { return d_->stats; }

void RtpOpusReceiver::reset() {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->clearStream();
    d_->stats.reset();
    d_->latch = SsrcLatch{};
}

size_t RtpOpusReceiver::buffered() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->buf.size();
}

uint32_t RtpOpusReceiver::ssrc() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->latch.have ? d_->latch.ssrc : 0;
}

}  // namespace xc
