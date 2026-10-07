// Nubix — RTP receive side: H.264 depacketizer (RFC 6184: single NAL, STAP-A, FU-A) with
// reorder buffer, loss tracking and NACK list; Opus RTP receiver; RFC 3550 receiver statistics.
//
// Loss handling follows the lessons documented in green-nx (src/switch/stream/video_jitter.cpp,
// GPL-3.0): complete frames are emitted immediately, incomplete ones are held only while a
// retransmission can still arrive, RTP padding packets are not loss, and after an unrecoverable
// loss nothing but a real IDR access unit is passed to the decoder.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace xc {

// Receiver-side statistics for one SSRC, as needed for an RTCP RR report block (RFC 3550 §6.4.1).
struct RtpReceiveStats {
    uint32_t ssrc = 0;
    uint8_t fractionLost = 0;      // since previous report, fixed point /256
    int32_t cumulativeLost = 0;    // 24-bit signed on the wire
    uint32_t extHighestSeq = 0;    // cycles << 16 | highest seq
    uint32_t jitter = 0;           // interarrival jitter in RTP timestamp units
    uint32_t lastSr = 0;           // middle 32 bits of the last SR NTP timestamp (LSR)
    uint32_t delaySinceLastSr = 0; // DLSR in 1/65536 s
    uint64_t packets = 0, bytes = 0;
};

// Monotonic milliseconds used by the RTP classes when no explicit time is passed.
uint64_t rtpNowMs();

// RFC 3550 Appendix A.1/A.8 sequence + jitter tracker, shared by video and audio receivers.
// Thread-safe (internally locked).
class RtpStatsTracker {
public:
    RtpStatsTracker() = default;
    // Copyable / assignable (state is copied under the source's lock; the mutex is not).
    RtpStatsTracker(const RtpStatsTracker& o);
    RtpStatsTracker& operator=(const RtpStatsTracker& o);

    // Account one received RTP packet (seq, rtp timestamp, clockRate Hz, payload bytes).
    void onPacket(uint32_t ssrc, uint16_t seq, uint32_t rtpTs, uint32_t clockRate, size_t bytes);
    // Same, with an explicit arrival time in microseconds (monotonic; for tests / replay).
    void onPacketAt(uint32_t ssrc, uint16_t seq, uint32_t rtpTs, uint32_t clockRate, size_t bytes,
                    uint64_t arrivalUs);
    // Record an incoming RTCP sender report (NTP timestamp) to fill LSR/DLSR.
    void onSenderReport(uint64_t ntpTimestamp);
    void onSenderReportAt(uint64_t ntpTimestamp, uint64_t nowMs);
    // Snapshot for an RR block; resets the "since last report" interval counters.
    RtpReceiveStats snapshotForReport();
    RtpReceiveStats snapshotForReportAt(uint64_t nowMs);
    // True once at least one packet was accounted.
    bool hasData() const;
    // Forget everything (new SSRC / reconnect).
    void reset();

private:
    void initSeq(uint16_t seq);

    mutable std::mutex mu_;
    RtpReceiveStats st_;
    bool init_ = false;
    uint16_t maxSeq_ = 0;
    uint32_t cycles_ = 0, baseSeq_ = 0, received_ = 0, expectedPrior_ = 0, receivedPrior_ = 0;
    uint32_t badSeq_ = 0x10001;
    double transit_ = 0, jitter_ = 0;
    bool haveTransit_ = false;
    uint64_t lastSrLocalMs_ = 0;
    bool haveSr_ = false;
};

// Counters of the H.264 depacketizer (monotonic since construction / reset()).
struct RtpVideoCounters {
    uint64_t packets = 0;          // RTP packets accepted (incl. padding)
    uint64_t framesEmitted = 0;    // complete access units handed out
    uint64_t framesDropped = 0;    // incomplete or non-decodable access units discarded
    uint64_t nacksSent = 0;        // sequence numbers returned by takeNackList (incl. retries)
    uint64_t recovered = 0;        // NACKed packets that arrived in time
    uint64_t lateOrDuplicate = 0;  // packets that arrived after their slot was consumed
    uint64_t keyframeRequests = 0; // times needKeyframe() returned true
    uint32_t rttMs = 0;            // NACK round-trip estimate (0 = none yet)
    bool waitingForKeyframe = true;
};

class RtpH264Depacketizer {
public:
    RtpH264Depacketizer();
    ~RtpH264Depacketizer();

    // Push one raw RTP packet (header included). Packets may arrive out of order; they are
    // reordered in a small window. Complete access units are emitted per marker bit / timestamp change.
    void push(const uint8_t* pkt, size_t n);
    // Same with explicit monotonic time in ms (tests / replay). Do not mix clocks.
    void pushAt(const uint8_t* pkt, size_t n, uint64_t nowMs);

    // Pop the next complete access unit as Annex-B (00 00 00 01 start codes). isKeyframe = contains IDR.
    bool popFrame(std::vector<uint8_t>& annexB, uint32_t& rtpTs, bool& isKeyframe);

    // Sequence numbers detected missing since the last call (for RTCP NACK). Clears the list.
    // Missing packets are re-requested a limited number of times while still useful.
    std::vector<uint16_t> takeNackList();
    std::vector<uint16_t> takeNackListAt(uint64_t nowMs);

    // True once after unrecoverable loss (frame dropped); caller should send PLI / keyframe request.
    // While still waiting for an IDR it becomes true again every keyframeRetryMs (default 1000).
    bool needKeyframe();
    bool needKeyframeAt(uint64_t nowMs);

    // Receiver statistics (for RTCP RR).
    RtpStatsTracker& stats();

    // Drop all buffered state (e.g. after reconnect).
    void reset();

    // Tuning: minimum time an incomplete frame is held for retransmission (default 40 ms; the
    // effective value grows with the measured NACK round trip, capped at maxLossWaitMs),
    // maximum hold (default 250 ms) and keyframe re-request interval (default 1000 ms).
    void setLossWait(uint32_t minLossWaitMs, uint32_t maxLossWaitMs);
    void setKeyframeRetryMs(uint32_t ms);
    // Payload type filter (0 = accept any, default 0). Packets with another PT are ignored.
    void setPayloadType(uint8_t pt);

    // Counters snapshot.
    RtpVideoCounters counters() const;
    // Latched media SSRC (0 = none yet).
    uint32_t ssrc() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Extracts Opus payloads from RTP, with a short reorder window and loss detection.
class RtpOpusReceiver {
public:
    RtpOpusReceiver();
    ~RtpOpusReceiver();

    // Push one raw RTP packet (header included).
    void push(const uint8_t* pkt, size_t n);
    void pushAt(const uint8_t* pkt, size_t n, uint64_t nowMs);

    // Pop the next Opus packet in sequence order. lostBefore = number of packets missing
    // immediately before this one (decoder should run PLC/FEC for them).
    bool pop(std::vector<uint8_t>& opus, uint32_t& rtpTs, int& lostBefore);
    bool popAt(std::vector<uint8_t>& opus, uint32_t& rtpTs, int& lostBefore, uint64_t nowMs);

    // Receiver statistics (for RTCP RR).
    RtpStatsTracker& stats();

    // Drop all buffered state.
    void reset();

    // Number of packets currently buffered (in order or ahead of a gap).
    size_t buffered() const;
    // Latched media SSRC (0 = none yet).
    uint32_t ssrc() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
