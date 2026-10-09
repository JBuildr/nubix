// Nubix — voice chat signalling tests: mic offer, chatStream bodies, exchange fields, RTP builder.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <atomic>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/protocol.hpp"
#include "stream/webrtc.hpp"
#include "test.hpp"

using namespace xc;

namespace {

const char* const kAudioRecvonly =
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:audio\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n"
    "a=recvonly\r\n";

const char* const kAudioMic =
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:audio\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n"
    "a=sendrecv\r\n"
    "a=msid:stream-1 track-1\r\n"
    "a=ssrc:3735928559 cname:xcdeadbeef\r\n"
    "a=ssrc:3735928559 msid:stream-1 track-1\r\n";

proto::OfferMic testMic() {
    proto::OfferMic m;
    m.ssrc = 0xDEADBEEFu;
    m.cname = "xcdeadbeef";
    m.msid = "stream-1";
    m.trackId = "track-1";
    return m;
}

std::string replaceOnce(std::string s, const std::string& from, const std::string& to) {
    const size_t at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

// Lines of the m-section with a=mid:<mid> (CR stripped).
std::vector<std::string> section(const std::string& sdp, const std::string& mid) {
    std::vector<std::string> cur, found;
    bool match = false;
    std::istringstream in(sdp);
    std::string line;
    auto flush = [&] {
        if (match && found.empty()) found = cur;
        cur.clear();
        match = false;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("m=", 0) == 0) flush();
        if (line == "a=mid:" + mid) match = true;
        cur.push_back(line);
    }
    flush();
    return found;
}

bool has(const std::vector<std::string>& lines, const std::string& l) {
    for (const auto& x : lines)
        if (x == l) return true;
    return false;
}

}  // namespace

XC_TEST(voice_sdp, offer_without_mic_unchanged) {
    for (bool home : {false, true}) {
        const std::string base = proto::buildOffer("uFrAg", "pWd", "AB:CD", home, "1080");
        XC_CHECK_EQ(proto::buildOffer("uFrAg", "pWd", "AB:CD", home, "1080", nullptr, 2), base);
        XC_CHECK(base.find(kAudioRecvonly) != std::string::npos);
        proto::OfferMic zero;  // ssrc 0 = no mic
        XC_CHECK_EQ(proto::buildOffer("uFrAg", "pWd", "AB:CD", home, "1080", &zero), base);
    }
}

XC_TEST(voice_sdp, offer_with_mic_exact_audio_lines) {
    const proto::OfferMic mic = testMic();
    const std::string base = proto::buildOffer("uFrAg", "pWd", "AB:CD", false, "1080");
    const std::string withMic = proto::buildOffer("uFrAg", "pWd", "AB:CD", false, "1080", &mic);
    XC_CHECK_EQ(withMic, replaceOnce(base, kAudioRecvonly, kAudioMic));
    // video stays recvonly, data m-line unchanged
    XC_CHECK_EQ(proto::sdpMediaDirection(withMic, "video"), std::string("recvonly"));
    XC_CHECK_EQ(proto::sdpMediaDirection(withMic, "audio"), std::string("sendrecv"));

    // sessionVersion 3 changes only the o= line
    const std::string v3 = proto::buildOffer("uFrAg", "pWd", "AB:CD", false, "1080", &mic, 3);
    XC_CHECK_EQ(v3, replaceOnce(withMic, "o=- 4611731400430051 2 IN IP4", "o=- 4611731400430051 3 IN IP4"));
    XC_CHECK(v3 != withMic);
    XC_CHECK(v3.find("o=- 4611731400430051 3 IN IP4 127.0.0.1\r\n") != std::string::npos);
}

XC_TEST(voice_sdp, post_body_chat_stream) {
    const std::string offer = "v=0\r\n";
    const std::string plain = proto::sdpPostBody(offer);
    XC_CHECK_EQ(proto::sdpPostBody(offer, false), plain);
    XC_CHECK(plain.find("chatStream") == std::string::npos);

    auto j = nlohmann::json::parse(proto::sdpPostBody(offer, true), nullptr, false);
    XC_REQUIRE(!j.is_discarded());
    auto base = nlohmann::json::parse(plain);
    XC_REQUIRE(j.contains("configuration"));
    const auto& cfg = j["configuration"];
    XC_REQUIRE(cfg.contains("chatStream"));
    XC_CHECK_EQ(cfg["chatStream"].value("minVersion", 0), 1);
    XC_CHECK_EQ(cfg["chatStream"].value("maxVersion", 0), 1);
    XC_CHECK(cfg.contains("chatConfiguration"));
    // everything else identical
    j["configuration"].erase("chatStream");
    XC_CHECK(j == base);
}

XC_TEST(voice_sdp, renegotiation_body_shape) {
    const std::string body = proto::sdpChatRenegotiationBody("v=0\r\n");
    XC_CHECK_EQ(body, std::string(R"({"configuration":{"isMediaStreamsChatRenegotiation":true},)"
                                  R"("messageType":"offer","requestId":"2","sdp":"v=0\r\n"})"));
}

XC_TEST(voice_sdp, parse_exchange_fields) {
    // string-wrapped object (what gssv returns)
    nlohmann::json inner = {{"sdp", "v=0\r\na=x\r\n"}, {"sdpType", "answer"}, {"status", "success"},
                            {"chat", 1}, {"chatStream", 1}};
    const std::string wrapped = nlohmann::json(inner.dump()).dump();
    proto::SdpExchangeFields f;
    XC_REQUIRE(proto::parseExchangeFields(wrapped, f));
    XC_CHECK_EQ(f.sdp, std::string("v=0\r\na=x\r\n"));
    XC_CHECK_EQ(f.status, std::string("success"));
    XC_CHECK_EQ(f.chat, 1);
    XC_CHECK_EQ(f.chatStream, 1);
    XC_CHECK(f.summary.find("sdpType") != std::string::npos);
    XC_CHECK(f.summary.find("v=0") == std::string::npos);
    XC_CHECK(f.summary.find("\"sdp\"") == std::string::npos);

    // plain object without chatStream
    proto::SdpExchangeFields g;
    XC_REQUIRE(proto::parseExchangeFields(R"({"sdp":"v=0","chat":0,"sdpType":"answer"})", g));
    XC_CHECK_EQ(g.chat, 0);
    XC_CHECK_EQ(g.chatStream, -1);
    XC_CHECK(g.status.empty());

    // numeric string / bool tolerance, long summary truncated
    proto::SdpExchangeFields h;
    nlohmann::json big = {{"sdp", "v=0"}, {"chatStream", "2"}, {"chat", true}, {"pad", std::string(1000, 'x')}};
    XC_REQUIRE(proto::parseExchangeFields(big.dump(), h));
    XC_CHECK_EQ(h.chatStream, 2);
    XC_CHECK_EQ(h.chat, 1);
    XC_CHECK(h.summary.size() <= 400);

    proto::SdpExchangeFields bad;
    XC_CHECK(!proto::parseExchangeFields("not json", bad));
    XC_CHECK(!proto::parseExchangeFields("[1,2]", bad));
    XC_CHECK(!proto::parseExchangeFields(R"("not an object")", bad));
}

XC_TEST(voice_sdp, media_direction) {
    const std::string base = "v=0\r\nm=video 9 X 102\r\na=mid:video\r\na=recvonly\r\nm=audio 9 X 111\r\n"
                             "a=mid:audio\r\na=%s\r\nm=application 9 X\r\na=mid:0\r\n";
    for (const char* d : {"sendrecv", "sendonly", "recvonly", "inactive"}) {
        const std::string sdp = replaceOnce(base, "%s", d);
        XC_CHECK_EQ(proto::sdpMediaDirection(sdp, "audio"), std::string(d));
        XC_CHECK_EQ(proto::sdpMediaDirection(sdp, "video"), std::string("recvonly"));
    }
    // direction before a=mid, LF-only line endings
    XC_CHECK_EQ(proto::sdpMediaDirection("v=0\nm=audio 9 X 111\na=sendonly\na=mid:audio\n", "audio"),
                std::string("sendonly"));
    // section without direction attribute
    XC_CHECK_EQ(proto::sdpMediaDirection("v=0\r\nm=audio 9 X 111\r\na=mid:audio\r\n", "audio"),
                std::string("sendrecv"));
    // session-level direction is not a section's
    XC_CHECK_EQ(proto::sdpMediaDirection("v=0\r\na=inactive\r\nm=audio 9 X 111\r\na=mid:audio\r\n", "audio"),
                std::string("sendrecv"));
    // absent mid
    XC_CHECK_EQ(proto::sdpMediaDirection(replaceOnce(base, "%s", "sendrecv"), "nope"), std::string());
    XC_CHECK_EQ(proto::sdpMediaDirection("", "audio"), std::string());
}

XC_TEST(voice_sdp, party_chat_active_message) {
    proto::ChannelMessage m;
    XC_REQUIRE(proto::parseChannelMessage(proto::messageSetPartyChatActive(true, "cv1"), m));
    XC_CHECK_EQ(m.type, std::string("TransactionStart"));
    XC_CHECK_EQ(m.target, std::string("/streaming/social/partyChatAudioCoordination/setPartyChatActive"));
    XC_CHECK_EQ(m.content, std::string(R"({"partyChatActive":true})"));
    XC_CHECK_EQ(m.cv, std::string("cv1"));
    XC_CHECK_EQ(m.id.size(), size_t(36));
    XC_REQUIRE(proto::parseChannelMessage(proto::messageSetPartyChatActive(false), m));
    XC_CHECK_EQ(m.content, std::string(R"({"partyChatActive":false})"));
    XC_CHECK_EQ(m.cv, std::string());
}

XC_TEST(voice_sdp, opus_rtp_bytes) {
    const uint8_t payload[] = {0xF8, 0xFF, 0xFE};
    const std::vector<uint8_t> p = buildOpusRtp(0xABCD, 0x01020304u, 0xDEADBEEFu, true, payload, sizeof(payload));
    const std::vector<uint8_t> want = {0x80, 0xEF, 0xAB, 0xCD, 0x01, 0x02, 0x03, 0x04,
                                       0xDE, 0xAD, 0xBE, 0xEF, 0xF8, 0xFF, 0xFE};
    XC_CHECK(p == want);

    const std::vector<uint8_t> q = buildOpusRtp(0xFFFF, 0xFFFFFC40u, 1, false, payload, 1, 96);
    XC_REQUIRE(q.size() == 13);
    XC_CHECK_EQ(q[0], uint8_t(0x80));
    XC_CHECK_EQ(q[1], uint8_t(96));  // no marker
    XC_CHECK_EQ(q[2], uint8_t(0xFF));
    XC_CHECK_EQ(q[3], uint8_t(0xFF));
    XC_CHECK_EQ(q[4], uint8_t(0xFF));
    XC_CHECK_EQ(q[7], uint8_t(0x40));
    XC_CHECK_EQ(q[11], uint8_t(1));
    XC_CHECK_EQ(q[12], uint8_t(0xF8));
    XC_CHECK_EQ(buildOpusRtp(1, 2, 3, false, nullptr, 0).size(), size_t(12));
}

XC_TEST(voice_sdp, webrtc_voice_off_has_no_mic) {
    WebRtc rtc;
    XC_REQUIRE(rtc.init(WebRtc::Callbacks{}));
    XC_CHECK_EQ(rtc.micInfo().ssrc, 0u);
    const uint8_t b[3] = {1, 2, 3};
    XC_CHECK(!rtc.sendMicOpus(b, sizeof(b), 960, true));
    XC_CHECK(rtc.answerAudioDirection().empty());
    XC_CHECK_EQ(rtc.chatAudioSsrc(), 0u);
    rtc.close();
}

// The hand-written offer must agree with libdatachannel's own local description for the
// audio m-line: direction, SSRC, cname, msid / track id.
XC_TEST(voice_sdp, offer_matches_local_description) {
    WebRtc rtc;
    XC_REQUIRE(rtc.init(WebRtc::Callbacks{}, true));
    const WebRtc::MicInfo mi = rtc.micInfo();
    XC_REQUIRE(mi.ssrc != 0);
    XC_CHECK(!mi.cname.empty());
    XC_CHECK_EQ(mi.msid.size(), size_t(36));
    XC_CHECK_EQ(mi.trackId.size(), size_t(36));
    XC_CHECK(mi.msid != mi.trackId);

    std::string u, p, fp;
    std::vector<std::string> cands;
    std::atomic<bool> abort{false};
    rtc.gatherLocal(u, p, fp, cands, 1500, &abort);  // result irrelevant: only the description matters
    const std::string local = rtc.localDescriptionSdp();
    XC_REQUIRE(!local.empty());

    proto::OfferMic om;
    om.ssrc = mi.ssrc;
    om.cname = mi.cname;
    om.msid = mi.msid;
    om.trackId = mi.trackId;
    const std::string offer = proto::buildOffer("u", "p", "AB:CD", false, "1080", &om);

    const auto mine = section(offer, "audio");
    const auto theirs = section(local, "audio");
    XC_REQUIRE(!mine.empty());
    XC_REQUIRE(!theirs.empty());
    XC_CHECK_EQ(proto::sdpMediaDirection(local, "audio"), std::string("sendrecv"));
    XC_CHECK_EQ(proto::sdpMediaDirection(offer, "audio"), std::string("sendrecv"));
    const std::string ssrc = std::to_string(mi.ssrc);
    for (const std::string& l : {"a=ssrc:" + ssrc + " cname:" + mi.cname,
                                 "a=ssrc:" + ssrc + " msid:" + mi.msid + " " + mi.trackId,
                                 "a=msid:" + mi.msid + " " + mi.trackId}) {
        XC_CHECK(has(mine, l));
        XC_CHECK(has(theirs, l));
    }
    // video stays receive-only in both
    XC_CHECK_EQ(proto::sdpMediaDirection(local, "video"), std::string("recvonly"));

    // Not connected: sending fails cleanly and is counted.
    const uint8_t b[3] = {1, 2, 3};
    XC_CHECK(!rtc.sendMicOpus(b, sizeof(b), 960, true));
    XC_CHECK_EQ(rtc.micTxStats().packets, uint64_t(0));
    XC_CHECK_EQ(rtc.micTxStats().failed, uint64_t(1));
    // No answer yet: renegotiation refuses, rollback is a no-op.
    std::string ru, rp, rfp;
    XC_CHECK(!rtc.beginRenegotiation(ru, rp, rfp));
    XC_CHECK(!rtc.finishRenegotiation("v=0\r\n"));
    rtc.abortRenegotiation();
    rtc.close();
    XC_CHECK_EQ(rtc.micInfo().ssrc, 0u);
}
