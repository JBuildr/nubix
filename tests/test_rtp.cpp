// Nubix — RTP H.264 depacketizer / Opus receiver tests.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>

#include "stream/rtp.hpp"
#include "test.hpp"

using namespace xc;

namespace {

std::vector<uint8_t> rtp(uint16_t seq, uint32_t ts, bool marker, const std::vector<uint8_t>& payload,
                         uint8_t pt = 102, uint32_t ssrc = 0x11223344) {
    std::vector<uint8_t> p(12);
    p[0] = 0x80;
    p[1] = uint8_t((marker ? 0x80 : 0) | pt);
    p[2] = uint8_t(seq >> 8); p[3] = uint8_t(seq);
    p[4] = uint8_t(ts >> 24); p[5] = uint8_t(ts >> 16); p[6] = uint8_t(ts >> 8); p[7] = uint8_t(ts);
    p[8] = uint8_t(ssrc >> 24); p[9] = uint8_t(ssrc >> 16); p[10] = uint8_t(ssrc >> 8); p[11] = uint8_t(ssrc);
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

void push(RtpH264Depacketizer& d, const std::vector<uint8_t>& p) { d.push(p.data(), p.size()); }

const std::vector<uint8_t> kSps = {0x67, 0x42, 0xE0, 0x1F, 0xAA};
const std::vector<uint8_t> kPps = {0x68, 0xCE, 0x3C, 0x80};

bool contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::memcmp(&hay[i], needle.data(), needle.size()) == 0) return true;
    return false;
}

std::vector<uint8_t> withStart(const std::vector<uint8_t>& nal) {
    std::vector<uint8_t> v = {0, 0, 0, 1};
    v.insert(v.end(), nal.begin(), nal.end());
    return v;
}

}  // namespace

XC_TEST(rtp, single_nal_frame) {
    RtpH264Depacketizer d;
    const std::vector<uint8_t> idr = {0x65, 0x88, 0x01, 0x02};     // IDR slice (first_mb = 0)
    const std::vector<uint8_t> nal = {0x41, 0x9A, 0x01, 0x02, 0x03};  // non-IDR slice
    push(d, rtp(9, 6000, true, idr));
    push(d, rtp(10, 9000, true, nal));
    std::vector<uint8_t> au;
    uint32_t ts = 0;
    bool key = false;
    XC_REQUIRE(d.popFrame(au, ts, key));
    XC_CHECK_EQ(ts, 6000u);
    XC_CHECK(key);
    XC_CHECK(au == withStart(idr));
    XC_REQUIRE(d.popFrame(au, ts, key));
    XC_CHECK_EQ(ts, 9000u);
    XC_CHECK(!key);
    XC_CHECK(au == withStart(nal));
    XC_CHECK(!d.popFrame(au, ts, key));
}

XC_TEST(rtp, stap_a_and_fu_a_keyframe) {
    RtpH264Depacketizer d;
    // STAP-A (type 24) with SPS + PPS
    std::vector<uint8_t> stap = {0x18};
    stap.push_back(0); stap.push_back(uint8_t(kSps.size())); stap.insert(stap.end(), kSps.begin(), kSps.end());
    stap.push_back(0); stap.push_back(uint8_t(kPps.size())); stap.insert(stap.end(), kPps.begin(), kPps.end());
    push(d, rtp(100, 3000, false, stap));
    // IDR (type 5, nri 3) split into 3 FU-A fragments
    const std::vector<uint8_t> idr = {0x65, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const uint8_t fuInd = (idr[0] & 0xE0) | 28;
    push(d, rtp(101, 3000, false, {fuInd, uint8_t(0x80 | 5), 1, 2, 3}));
    push(d, rtp(102, 3000, false, {fuInd, uint8_t(5), 4, 5, 6}));
    push(d, rtp(103, 3000, true, {fuInd, uint8_t(0x40 | 5), 7, 8, 9}));
    std::vector<uint8_t> au;
    uint32_t ts = 0;
    bool key = false;
    XC_REQUIRE(d.popFrame(au, ts, key));
    XC_CHECK(key);
    XC_CHECK_EQ(ts, 3000u);
    XC_CHECK(contains(au, withStart(kSps)));
    XC_CHECK(contains(au, withStart(kPps)));
    XC_CHECK(contains(au, withStart(idr)));
}

XC_TEST(rtp, reorders_within_window) {
    RtpH264Depacketizer d;
    const std::vector<uint8_t> idr = {0x65, 1, 2, 3, 4, 5, 6};
    const uint8_t fuInd = (idr[0] & 0xE0) | 28;
    push(d, rtp(201, 6000, false, {fuInd, uint8_t(0x80 | 5), 1, 2}));
    push(d, rtp(203, 6000, true, {fuInd, uint8_t(0x40 | 5), 5, 6}));
    push(d, rtp(202, 6000, false, {fuInd, uint8_t(5), 3, 4}));
    std::vector<uint8_t> au;
    uint32_t ts = 0;
    bool key = false;
    XC_REQUIRE(d.popFrame(au, ts, key));
    XC_CHECK(contains(au, withStart(idr)));
    XC_CHECK(d.takeNackList().empty());
}

XC_TEST(rtp, gap_produces_nack) {
    RtpH264Depacketizer d;
    push(d, rtp(65534, 1000, true, {0x41, 1}));
    // 65535 and 0 missing (wraparound)
    push(d, rtp(1, 2000, true, {0x41, 2}));
    push(d, rtp(2, 3000, true, {0x41, 3}));
    auto nacks = d.takeNackList();
    bool s65535 = false, s0 = false;
    for (auto s : nacks) {
        if (s == 65535) s65535 = true;
        if (s == 0) s0 = true;
    }
    XC_CHECK(s65535);
    XC_CHECK(s0);
    XC_CHECK(d.takeNackList().empty());
}

XC_TEST(rtp, opus_in_order_and_loss) {
    RtpOpusReceiver r;
    auto a = rtp(500, 960, false, {0xF8, 0x01}, 111);
    auto b = rtp(501, 1920, false, {0xF8, 0x02}, 111);
    auto c = rtp(503, 3840, false, {0xF8, 0x04}, 111);
    r.push(a.data(), a.size());
    r.push(b.data(), b.size());
    r.push(c.data(), c.size());
    std::vector<uint8_t> opus;
    uint32_t ts = 0;
    int lost = -1;
    XC_REQUIRE(r.pop(opus, ts, lost));
    XC_CHECK_EQ(ts, 960u);
    XC_CHECK_EQ(lost, 0);
    XC_CHECK_EQ(opus.size(), size_t(2));
    XC_REQUIRE(r.pop(opus, ts, lost));
    XC_CHECK_EQ(ts, 1920u);
    // 502 missing: depending on the reorder window it may be reported now or after more packets
    for (int i = 0; i < 10; ++i) {
        auto p = rtp(uint16_t(504 + i), 4800u + 960u * i, false, {0xF8, 0x05}, 111);
        r.push(p.data(), p.size());
    }
    XC_REQUIRE(r.pop(opus, ts, lost));
    XC_CHECK_EQ(ts, 3840u);
    XC_CHECK_EQ(lost, 1);
}

XC_TEST(rtp, stats_tracker_counts_loss) {
    RtpStatsTracker t;
    for (uint16_t s = 0; s < 100; ++s)
        if (s % 10 != 5) t.onPacket(0xABCD, s, s * 3000u, 90000, 1000);
    RtpReceiveStats st = t.snapshotForReport();
    XC_CHECK_EQ(st.ssrc, 0xABCDu);
    XC_CHECK_EQ(st.cumulativeLost, 10);
    XC_CHECK_EQ(st.extHighestSeq & 0xFFFFu, 99u);
    XC_CHECK(st.fractionLost > 0);
    XC_CHECK_EQ(st.packets, uint64_t(90));
}

// ---- time-controlled scenarios (pushAt / takeNackListAt / needKeyframeAt) -----------------------

namespace {

void pushAt(RtpH264Depacketizer& d, const std::vector<uint8_t>& p, uint64_t t) { d.pushAt(p.data(), p.size(), t); }

const std::vector<uint8_t> kIdr = {0x65, 0x88, 0x10, 0x20};
const std::vector<uint8_t> kP = {0x41, 0x9A, 0x30, 0x40};

int drainFrames(RtpH264Depacketizer& d, std::vector<uint32_t>* tss = nullptr, int* keys = nullptr) {
    std::vector<uint8_t> au;
    uint32_t ts = 0;
    bool key = false;
    int n = 0;
    while (d.popFrame(au, ts, key)) {
        ++n;
        if (tss) tss->push_back(ts);
        if (keys && key) ++*keys;
    }
    return n;
}

bool has(const std::vector<uint16_t>& v, uint16_t s) {
    for (auto x : v)
        if (x == s) return true;
    return false;
}

}  // namespace

XC_TEST(rtp, drops_p_frames_until_first_idr) {
    RtpH264Depacketizer d;
    pushAt(d, rtp(1, 1000, true, kP), 0);
    pushAt(d, rtp(2, 2500, true, kP), 16);
    XC_CHECK_EQ(drainFrames(d), 0);
    pushAt(d, rtp(3, 4000, true, kIdr), 33);
    pushAt(d, rtp(4, 5500, true, kP), 50);
    int keys = 0;
    XC_CHECK_EQ(drainFrames(d, nullptr, &keys), 2);
    XC_CHECK_EQ(keys, 1);
    XC_CHECK(!d.counters().waitingForKeyframe);
}

XC_TEST(rtp, fu_a_across_sequence_wrap) {
    RtpH264Depacketizer d;
    const uint8_t ind = 0x60 | 28;
    pushAt(d, rtp(65534, 7000, false, {ind, uint8_t(0x80 | 5), 0x88, 1}), 0);
    pushAt(d, rtp(65535, 7000, false, {ind, 5, 2, 3}), 0);
    pushAt(d, rtp(0, 7000, false, {ind, 5, 4, 5}), 0);
    pushAt(d, rtp(1, 7000, true, {ind, uint8_t(0x40 | 5), 6}), 0);
    std::vector<uint8_t> au;
    uint32_t ts = 0;
    bool key = false;
    XC_REQUIRE(d.popFrame(au, ts, key));
    XC_CHECK(key);
    XC_CHECK(au == withStart({0x65, 0x88, 1, 2, 3, 4, 5, 6}));
    XC_CHECK(d.takeNackListAt(1).empty());
}

XC_TEST(rtp, retransmission_completes_frame) {
    RtpH264Depacketizer d;
    pushAt(d, rtp(10, 1000, true, kIdr), 0);
    const uint8_t ind = 0x40 | 28;
    pushAt(d, rtp(11, 2500, false, {ind, uint8_t(0x80 | 1), 0x9A, 1}), 16);
    // 12 lost
    pushAt(d, rtp(13, 2500, true, {ind, uint8_t(0x40 | 1), 3}), 17);
    XC_CHECK_EQ(drainFrames(d), 1);  // only the IDR so far
    auto n = d.takeNackListAt(18);
    XC_CHECK(has(n, 12));
    pushAt(d, rtp(12, 2500, false, {ind, 1, 2}), 30);  // retransmission, 12 ms RTT
    std::vector<uint8_t> au;
    uint32_t ts = 0;
    bool key = true;
    XC_REQUIRE(d.popFrame(au, ts, key));
    XC_CHECK_EQ(ts, 2500u);
    XC_CHECK(!key);
    XC_CHECK(au == withStart({0x41, 0x9A, 1, 2, 3}));
    XC_CHECK(!d.needKeyframeAt(31));
    XC_CHECK_EQ(d.counters().recovered, uint64_t(1));
    XC_CHECK(d.counters().rttMs > 0);
}

XC_TEST(rtp, unrecoverable_loss_requests_keyframe_and_drops_until_idr) {
    RtpH264Depacketizer d;
    pushAt(d, rtp(100, 1000, true, kIdr), 0);
    XC_CHECK_EQ(drainFrames(d), 1);
    const uint8_t ind = 0x40 | 28;
    pushAt(d, rtp(101, 2500, false, {ind, uint8_t(0x80 | 1), 0x9A}), 16);
    // 102 (middle fragment) never arrives
    pushAt(d, rtp(103, 2500, true, {ind, uint8_t(0x40 | 1), 9}), 16);
    pushAt(d, rtp(104, 4000, true, kP), 33);
    XC_CHECK(has(d.takeNackListAt(17), 102));
    // NACK retries are limited
    int retries = 0;
    for (uint64_t t = 20; t < 50; t += 5) retries += static_cast<int>(d.takeNackListAt(t).size());
    XC_CHECK(retries <= 3);
    XC_CHECK_EQ(drainFrames(d), 0);  // held while a retransmission may still arrive
    XC_CHECK(!d.needKeyframeAt(30));
    // after the loss wait (40 ms) the frame is given up
    XC_CHECK(d.needKeyframeAt(60));
    XC_CHECK(!d.needKeyframeAt(61));  // once
    XC_CHECK_EQ(drainFrames(d), 0);   // the following P-frame is not decodable either
    pushAt(d, rtp(105, 5500, true, kP), 70);
    XC_CHECK_EQ(drainFrames(d), 0);
    // still waiting: request is repeated after the retry interval
    XC_CHECK(d.needKeyframeAt(1100));
    pushAt(d, rtp(106, 7000, true, kIdr), 1110);
    pushAt(d, rtp(107, 8500, true, kP), 1120);
    std::vector<uint32_t> tss;
    XC_CHECK_EQ(drainFrames(d, &tss), 2);
    XC_REQUIRE(tss.size() == 2);
    XC_CHECK_EQ(tss[0], 7000u);
    XC_CHECK(!d.needKeyframeAt(5000));
    // late retransmission of the given-up packet is ignored
    pushAt(d, rtp(102, 2500, false, {ind, 1, 5}), 1200);
    XC_CHECK_EQ(drainFrames(d), 0);
    XC_CHECK(d.counters().framesDropped >= 2);
}

XC_TEST(rtp, padding_packets_are_not_loss) {
    RtpH264Depacketizer d;
    pushAt(d, rtp(1, 1000, true, kIdr), 0);
    // padding-only packet (P bit, all payload is padding), then next frame
    std::vector<uint8_t> pad = rtp(2, 1000, false, {0, 0, 0, 4});
    pad[0] |= 0x20;
    pushAt(d, pad, 5);
    pushAt(d, rtp(3, 2500, true, kP), 16);
    XC_CHECK_EQ(drainFrames(d), 2);
    // a lost padding packet between frames: frame after it is a clean picture start -> no keyframe
    pushAt(d, rtp(5, 4000, true, kP), 33);  // 4 lost
    XC_CHECK(d.takeNackListAt(34).size() == 1);
    XC_CHECK_EQ(drainFrames(d), 0);
    XC_CHECK(!d.needKeyframeAt(80));  // gap given up after 40 ms, picture after it is clean
    XC_CHECK_EQ(drainFrames(d), 1);
    XC_CHECK(!d.counters().waitingForKeyframe);
}

XC_TEST(rtp, header_extension_csrc_and_padding_stripped) {
    RtpH264Depacketizer d;
    std::vector<uint8_t> p = rtp(50, 3000, true, {});
    p[0] = 0x80 | 0x20 | 0x10 | 0x01;  // P, X, CC=1
    std::vector<uint8_t> tail = {0xDE, 0xAD, 0xBE, 0xEF,             // CSRC
                                 0xBE, 0xDE, 0x00, 0x01, 1, 2, 3, 4};  // extension, 1 word
    tail.insert(tail.end(), kIdr.begin(), kIdr.end());
    tail.insert(tail.end(), {0, 0, 3});  // 3 bytes padding
    p.insert(p.end(), tail.begin(), tail.end());
    pushAt(d, p, 0);
    std::vector<uint8_t> au;
    uint32_t ts = 0;
    bool key = false;
    XC_REQUIRE(d.popFrame(au, ts, key));
    XC_CHECK(au == withStart(kIdr));
}

XC_TEST(rtp, marker_less_frame_ends_on_timestamp_change) {
    RtpH264Depacketizer d;
    pushAt(d, rtp(1, 1000, false, kIdr), 0);
    XC_CHECK_EQ(drainFrames(d), 0);
    pushAt(d, rtp(2, 2500, true, kP), 16);
    XC_CHECK_EQ(drainFrames(d), 2);
}

XC_TEST(rtp, ssrc_latch_ignores_stray_and_follows_switch) {
    RtpH264Depacketizer d;
    pushAt(d, rtp(1, 1000, true, kIdr, 102, 0xAAAA), 0);
    XC_CHECK_EQ(d.ssrc(), 0xAAAAu);
    pushAt(d, rtp(500, 9999, true, kIdr, 102, 0xBBBB), 1);  // stray packet of another source
    XC_CHECK_EQ(d.ssrc(), 0xAAAAu);
    XC_CHECK_EQ(drainFrames(d), 1);
    for (uint16_t s = 0; s < 3; ++s) pushAt(d, rtp(uint16_t(900 + s), 20000u + s * 1500u, true, kIdr, 102, 0xCCCC), 10 + s);
    XC_CHECK_EQ(d.ssrc(), 0xCCCCu);
    XC_CHECK(drainFrames(d) >= 1);
}

XC_TEST(rtp, stats_lsr_dlsr_and_jitter) {
    RtpStatsTracker t;
    // perfectly paced 90 kHz stream: zero jitter
    for (uint32_t i = 0; i < 50; ++i) t.onPacketAt(7, uint16_t(i), i * 1500u, 90000, 100, i * 16667ull);
    t.onSenderReportAt(0x0123456789ABCDEFull, 1000);
    RtpReceiveStats st = t.snapshotForReportAt(1500);  // 0.5 s after SR
    XC_CHECK_EQ(st.lastSr, 0x456789ABu);
    XC_CHECK_EQ(st.delaySinceLastSr, 32768u);  // 0.5 * 65536
    XC_CHECK(st.jitter <= 1u);
    XC_CHECK_EQ(st.cumulativeLost, 0);
    XC_CHECK_EQ(st.fractionLost, uint8_t(0));
    // wraparound counts a cycle
    RtpStatsTracker w;
    for (uint32_t i = 0; i < 20; ++i) w.onPacketAt(9, uint16_t(65530 + i), i * 960u, 48000, 10, i * 20000ull);
    RtpReceiveStats ws = w.snapshotForReport();
    XC_CHECK_EQ(ws.extHighestSeq, 0x10000u + 13u);
    XC_CHECK_EQ(ws.cumulativeLost, 0);
    // copyable
    RtpStatsTracker c = w;
    XC_CHECK(c.hasData());
}

XC_TEST(rtp, opus_gap_skipped_after_timeout_and_dtx) {
    RtpOpusReceiver r;
    auto a = rtp(10, 0, false, {0xF8, 1}, 111);
    auto c = rtp(12, 1920, false, {0xF8, 3}, 111);
    r.pushAt(a.data(), a.size(), 0);
    r.pushAt(c.data(), c.size(), 20);
    std::vector<uint8_t> opus;
    uint32_t ts = 0;
    int lost = -1;
    XC_REQUIRE(r.popAt(opus, ts, lost, 20));
    XC_CHECK_EQ(lost, 0);
    XC_CHECK(!r.popAt(opus, ts, lost, 30));  // 11 may still arrive
    XC_REQUIRE(r.popAt(opus, ts, lost, 61)); // reorder window (20 ms) has passed
    XC_CHECK_EQ(ts, 1920u);
    XC_CHECK_EQ(lost, 1);
    // late packet is dropped
    auto b = rtp(11, 960, false, {0xF8, 2}, 111);
    r.pushAt(b.data(), b.size(), 62);
    XC_CHECK(!r.popAt(opus, ts, lost, 200));
    // empty (DTX) packet carries loss over to the next real packet
    auto e = rtp(14, 3840, false, {}, 111);
    auto f = rtp(15, 4800, false, {0xF8, 5}, 111);
    r.pushAt(e.data(), e.size(), 300);
    r.pushAt(f.data(), f.size(), 300);
    XC_REQUIRE(r.popAt(opus, ts, lost, 400));
    XC_CHECK_EQ(ts, 4800u);
    XC_CHECK_EQ(lost, 1);  // 13 missing
}

XC_TEST(rtp, opus_swapped_pair_played_in_order) {
    RtpOpusReceiver r;
    auto a = rtp(100, 0, false, {0xF8, 1}, 111);
    auto b = rtp(101, 960, false, {0xF8, 2}, 111);
    auto c = rtp(102, 1920, false, {0xF8, 3}, 111);
    std::vector<uint8_t> opus;
    uint32_t ts = 0;
    int lost = -1;
    r.pushAt(a.data(), a.size(), 0);
    XC_REQUIRE(r.popAt(opus, ts, lost, 0));
    // 102 overtakes 101 by one slot: nothing is concealed, both play in order
    r.pushAt(c.data(), c.size(), 10);
    XC_CHECK(!r.popAt(opus, ts, lost, 10));
    r.pushAt(b.data(), b.size(), 15);
    XC_REQUIRE(r.popAt(opus, ts, lost, 15));
    XC_CHECK_EQ(ts, 960u);
    XC_CHECK_EQ(lost, 0);
    XC_REQUIRE(r.popAt(opus, ts, lost, 15));
    XC_CHECK_EQ(ts, 1920u);
    XC_CHECK_EQ(lost, 0);
}

XC_TEST(rtp, stats_sr_before_first_packet_is_kept) {
    RtpStatsTracker t;
    t.onSenderReportAt(0x0123456789ABCDEFull, 1000);  // SR right after DTLS, before any RTP
    for (uint32_t i = 0; i < 10; ++i) t.onPacketAt(5, uint16_t(i), i * 1500u, 90000, 100, 1000000ull + i * 16667ull);
    RtpReceiveStats st = t.snapshotForReportAt(1250);
    XC_CHECK_EQ(st.lastSr, 0x456789ABu);
    XC_CHECK_EQ(st.delaySinceLastSr, 16384u);  // 0.25 s
    // a different SSRC later drops it again
    t.onPacketAt(6, 0, 0, 90000, 100, 2000000ull);
    RtpReceiveStats st2 = t.snapshotForReportAt(1300);
    XC_CHECK_EQ(st2.lastSr, 0u);
    XC_CHECK_EQ(st2.delaySinceLastSr, 0u);
}
