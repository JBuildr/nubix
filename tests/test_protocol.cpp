// Nubix — protocol byte-level tests (input packets, vibration, teredo, JSON messages).
// Expected values derived from green-nx and xbox-xcloud-player.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>

#include <nlohmann/json.hpp>

#include "core/protocol.hpp"
#include "test.hpp"

using namespace xc;
using nlohmann::json;

namespace {
uint16_t u16(const std::vector<uint8_t>& b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t u32(const std::vector<uint8_t>& b, size_t o) {
    return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}
}  // namespace

XC_TEST(protocol, client_metadata_layout) {
    proto::InputSerializer s;
    auto p = s.clientMetadata(1);
    XC_REQUIRE(p.size() == 15u);
    XC_CHECK_EQ(u16(p, 0), uint16_t(proto::report::ClientMetadata));
    XC_CHECK_EQ(p[14], uint8_t(1));
}

XC_TEST(protocol, gamepad_layout) {
    proto::InputSerializer s;
    s.clientMetadata(1);
    GamepadState st;
    st.buttons = btn::A | btn::DUp | btn::RS;
    st.lx = 1000; st.ly = -2000; st.rx = 32767; st.ry = -32767;
    st.lt = 65535; st.rt = 1234;
    auto p = s.gamepad(st, 0);
    XC_REQUIRE(p.size() == 38u);
    XC_CHECK_EQ(u16(p, 0), uint16_t(proto::report::Gamepad));
    XC_CHECK_EQ(p[14], uint8_t(1));   // count
    XC_CHECK_EQ(p[15], uint8_t(0));   // gamepad index
    XC_CHECK_EQ(u16(p, 16), uint16_t(btn::A | btn::DUp | btn::RS));
    XC_CHECK_EQ(int16_t(u16(p, 18)), int16_t(1000));
    XC_CHECK_EQ(int16_t(u16(p, 20)), int16_t(-2000));
    XC_CHECK_EQ(int16_t(u16(p, 22)), int16_t(32767));
    XC_CHECK_EQ(int16_t(u16(p, 24)), int16_t(-32767));
    XC_CHECK_EQ(u16(p, 26), uint16_t(65535));
    XC_CHECK_EQ(u16(p, 28), uint16_t(1234));
    XC_CHECK_EQ(u32(p, 30), 1u);  // PhysicalPhysicality, LE 1
    // VirtualPhysicality: big-endian 1 -> bytes 00 00 00 01
    XC_CHECK_EQ(p[34], uint8_t(0));
    XC_CHECK_EQ(p[35], uint8_t(0));
    XC_CHECK_EQ(p[36], uint8_t(0));
    XC_CHECK_EQ(p[37], uint8_t(1));
}

XC_TEST(protocol, sequence_increments_and_rollback) {
    proto::InputSerializer s;
    auto a = s.clientMetadata(1);
    auto b = s.gamepad(GamepadState{}, 0);
    auto c = s.gamepad(GamepadState{}, 0);
    XC_REQUIRE(a.size() >= 6 && b.size() >= 6 && c.size() >= 6);
    XC_CHECK_EQ(u32(b, 2), u32(a, 2) + 1);
    XC_CHECK_EQ(u32(c, 2), u32(b, 2) + 1);
    s.rollback();
    auto d = s.gamepad(GamepadState{}, 0);
    XC_REQUIRE(d.size() >= 6);
    XC_CHECK_EQ(u32(d, 2), u32(c, 2));
}

XC_TEST(protocol, timestamp_is_monotonic_double) {
    proto::InputSerializer s;
    auto a = s.gamepad(GamepadState{}, 0);
    auto b = s.gamepad(GamepadState{}, 0);
    XC_REQUIRE(a.size() >= 14 && b.size() >= 14);
    double ta, tb;
    std::memcpy(&ta, &a[6], 8);
    std::memcpy(&tb, &b[6], 8);
    XC_CHECK(ta >= 0.0);
    XC_CHECK(tb >= ta);
}

XC_TEST(protocol, parse_vibration) {
    const uint8_t pkt[] = {0x80, 0x00, 0x00, 0x00, 100, 50, 10, 20, 0xE8, 0x03, 0x05, 0x00, 0x02};
    Vibration v;
    XC_REQUIRE(proto::parseVibration(pkt, sizeof(pkt), v));
    XC_CHECK_EQ(v.pad, uint8_t(0));
    XC_CHECK_EQ(v.left, uint8_t(100));
    XC_CHECK_EQ(v.right, uint8_t(50));
    XC_CHECK_EQ(v.lt, uint8_t(10));
    XC_CHECK_EQ(v.rt, uint8_t(20));
    XC_CHECK_EQ(v.durationMs, uint16_t(1000));
    XC_CHECK_EQ(v.delayMs, uint16_t(5));
    XC_CHECK_EQ(v.repeat, uint8_t(2));
}

XC_TEST(protocol, parse_vibration_rejects_other_reports) {
    const uint8_t pkt[] = {0x10, 0x00, 0x38, 0x04, 0x00, 0x00, 0x80, 0x07, 0x00, 0x00};
    Vibration v;
    XC_CHECK(!proto::parseVibration(pkt, sizeof(pkt), v));
    XC_CHECK(!proto::parseVibration(pkt, 1, v));
}

XC_TEST(protocol, parse_server_metadata) {
    // reportType 16, u32 height @2, u32 width @6
    const uint8_t pkt[] = {0x10, 0x00, 0x38, 0x04, 0x00, 0x00, 0x80, 0x07, 0x00, 0x00};
    uint32_t w = 0, h = 0;
    XC_REQUIRE(proto::parseServerMetadata(pkt, sizeof(pkt), w, h));
    XC_CHECK_EQ(w, 1920u);
    XC_CHECK_EQ(h, 1080u);
}

XC_TEST(protocol, teredo_rfc4380_example) {
    std::string ip;
    uint16_t port = 0;
    XC_REQUIRE(proto::teredoDecode("2001:0000:4136:e378:8000:63bf:3fff:fdd2", ip, port));
    XC_CHECK_EQ(ip, std::string("192.0.2.45"));
    XC_CHECK_EQ(port, uint16_t(40000));
    XC_REQUIRE(proto::teredoDecode("2001:0:4136:e378:8000:63bf:3fff:fdd2", ip, port));
    XC_CHECK_EQ(ip, std::string("192.0.2.45"));
    XC_CHECK_EQ(port, uint16_t(40000));
}

XC_TEST(protocol, teredo_rejects_non_teredo) {
    std::string ip;
    uint16_t port = 0;
    XC_CHECK(!proto::teredoDecode("2a01:4f8::1", ip, port));
    XC_CHECK(!proto::teredoDecode("192.168.1.1", ip, port));
    XC_CHECK(!proto::teredoDecode("garbage", ip, port));
}

XC_TEST(protocol, teredo_candidate_expansion) {
    std::vector<std::string> in = {
        "a=candidate:1 1 UDP 100 2001:0:4136:e378:8000:63bf:3fff:fdd2 1234 typ host",
        "a=candidate:2 1 UDP 50 20.1.2.3 1000 typ host"};
    auto out = proto::expandTeredoCandidates(in);
    bool sawOrig = false, saw40000 = false, saw9002 = false, sawPlain = false;
    for (const auto& c : out) {
        if (c.find("2001:0:4136:e378") != std::string::npos) sawOrig = true;
        if (c.find(" 192.0.2.45 40000 ") != std::string::npos) saw40000 = true;
        if (c.find(" 192.0.2.45 9002 ") != std::string::npos) saw9002 = true;
        if (c.find(" 20.1.2.3 1000 ") != std::string::npos) sawPlain = true;
    }
    XC_CHECK(sawOrig);
    XC_CHECK(saw40000);
    XC_CHECK(saw9002);
    XC_CHECK(sawPlain);
}

XC_TEST(protocol, control_messages_are_json) {
    auto kf = json::parse(proto::controlKeyframeRequest(), nullptr, false);
    XC_REQUIRE(!kf.is_discarded());
    XC_CHECK_EQ(kf.value("message", std::string()), std::string("videoKeyframeRequested"));
    XC_CHECK(kf.value("ifrRequested", false));
    XC_CHECK(!json::parse(proto::controlAuthorization(), nullptr, false).is_discarded());
    XC_CHECK(!json::parse(proto::controlGamepadChanged(0, true), nullptr, false).is_discarded());
    XC_CHECK(!json::parse(proto::controlResolution("1080"), nullptr, false).is_discarded());
    XC_CHECK(!json::parse(proto::messageHandshake(), nullptr, false).is_discarded());
}

XC_TEST(protocol, message_init_sequence) {
    Settings s;
    auto msgs = proto::messageInitSequence(s, "00000000-0000-4000-8000-000000000000", 1920, 1080);
    XC_REQUIRE(!msgs.empty());
    for (const auto& m : msgs) XC_CHECK(!json::parse(m, nullptr, false).is_discarded());
    auto tc = json::parse(proto::transactionComplete("abc", "{}"), nullptr, false);
    XC_CHECK(!tc.is_discarded());
}

XC_TEST(protocol, ice_post_body_green_nx_shape) {
    std::vector<std::string> cands = {"candidate:1 1 UDP 2122260223 192.168.1.2 50000 typ host"};
    auto body = json::parse(proto::icePostBody(cands, "UFRAG"), nullptr, false);
    XC_REQUIRE(!body.is_discarded());
    XC_CHECK_EQ(body.value("messageType", std::string()), std::string("iceCandidate"));
    XC_REQUIRE(body.contains("candidate") && body["candidate"].is_array());
    XC_REQUIRE(body["candidate"].size() == 2u);  // candidate + end-of-candidates
    auto first = json::parse(body["candidate"][0].get<std::string>(), nullptr, false);
    XC_REQUIRE(!first.is_discarded());
    XC_CHECK_EQ(first.value("candidate", std::string()), cands[0]);
    XC_CHECK_EQ(first.value("sdpMid", std::string()), std::string("0"));
    XC_CHECK_EQ(first.value("sdpMLineIndex", -1), 0);
    XC_CHECK_EQ(first.value("usernameFragment", std::string()), std::string("UFRAG"));
    auto last = json::parse(body["candidate"][1].get<std::string>(), nullptr, false);
    XC_CHECK_EQ(last.value("candidate", std::string()), std::string("a=end-of-candidates"));
}

// ---------------------------------------------------------------------------------------------
// Byte-exact vectors (derived from green-nx xcloud_protocol.cpp / xcloud-player packet.ts)
// ---------------------------------------------------------------------------------------------

namespace {
// Compare everything except the 8 timestamp bytes at [6, 14).
bool sameIgnoringTimestamp(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < got.size(); ++i) {
        if (i >= 6 && i < 14) continue;
        if (got[i] != want[i]) return false;
    }
    return true;
}
std::string hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) {
        s += d[c >> 4];
        s += d[c & 15];
        s += ' ';
    }
    return s;
}
}  // namespace

XC_TEST(protocol, client_metadata_exact_bytes_green_nx_sequence) {
    proto::InputSerializer s;
    XC_CHECK_EQ(s.nextSequence(), 1u);
    auto p = s.clientMetadata(0);
    // u16 8 | u32 seq=1 (green-nx pre-increments) | f64 ts | u8 maxTouchpoints
    const std::vector<uint8_t> want = {0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x00};
    XC_CHECK(sameIgnoringTimestamp(p, want));
    if (!sameIgnoringTimestamp(p, want)) std::printf("  got  %s\n  want %s\n", hex(p).c_str(), hex(want).c_str());
    XC_CHECK_EQ(s.nextSequence(), 2u);
}

XC_TEST(protocol, gamepad_exact_bytes) {
    proto::InputSerializer s;
    s.clientMetadata(1);  // seq 1
    GamepadState st;
    st.buttons = btn::Nexus | btn::A | btn::LB | btn::RS;  // 0x9012
    st.lx = 0x1234; st.ly = -1; st.rx = -32767; st.ry = 32767;
    st.lt = 0xABCD; st.rt = 0x0001;
    auto p = s.gamepad(st, 2);
    const std::vector<uint8_t> want = {
        0x02, 0x00,                    // reportType Gamepad
        0x02, 0x00, 0x00, 0x00,        // seq 2
        0, 0, 0, 0, 0, 0, 0, 0,        // timestamp (ignored)
        0x01,                          // count
        0x02,                          // +0  index
        0x12, 0x90,                    // +1  buttons
        0x34, 0x12,                    // +3  lx
        0xFF, 0xFF,                    // +5  ly = -1
        0x01, 0x80,                    // +7  rx = -32767
        0xFF, 0x7F,                    // +9  ry = 32767
        0xCD, 0xAB,                    // +11 lt
        0x01, 0x00,                    // +13 rt
        0x01, 0x00, 0x00, 0x00,        // +15 PhysicalPhysicality LE 1
        0x00, 0x00, 0x00, 0x01,        // +19 VirtualPhysicality BE 1
    };
    XC_REQUIRE(want.size() == proto::kGamepadPacketSize);
    XC_CHECK(sameIgnoringTimestamp(p, want));
    if (!sameIgnoringTimestamp(p, want)) std::printf("  got  %s\n  want %s\n", hex(p).c_str(), hex(want).c_str());
}

XC_TEST(protocol, rollback_never_underflows_and_reset) {
    proto::InputSerializer s;
    s.rollback();
    XC_CHECK_EQ(s.nextSequence(), 1u);
    s.gamepad(GamepadState{}, 0);
    s.gamepad(GamepadState{}, 0);
    XC_CHECK_EQ(s.nextSequence(), 3u);
    s.rollback();
    XC_CHECK_EQ(s.nextSequence(), 2u);
    auto p = s.gamepad(GamepadState{}, 0);
    XC_CHECK_EQ(u32(p, 2), 2u);
    s.reset();
    XC_CHECK_EQ(s.nextSequence(), 1u);
    XC_CHECK_EQ(u32(s.clientMetadata(), 2), 1u);
}

XC_TEST(protocol, control_messages_exact) {
    XC_CHECK_EQ(proto::controlAuthorization(),
                std::string(R"({"accessKey":"4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E","message":"authorizationRequest"})"));
    XC_CHECK_EQ(proto::controlGamepadChanged(0, true),
                std::string(R"({"gamepadIndex":0,"message":"gamepadChanged","wasAdded":true})"));
    XC_CHECK_EQ(proto::controlGamepadChanged(1, false),
                std::string(R"({"gamepadIndex":1,"message":"gamepadChanged","wasAdded":false})"));
    XC_CHECK_EQ(proto::controlResolution("1080HQ"),
                std::string(R"({"message":"userRequestedResolutionUpdate","resolutionAlias":"1080HQ"})"));
    XC_CHECK_EQ(proto::controlKeyframeRequest(),
                std::string(R"({"ifrRequested":true,"message":"videoKeyframeRequested"})"));
}

namespace {
bool isUuidV4(const std::string& u) {
    if (u.size() != 36) return false;
    for (size_t i = 0; i < u.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (u[i] != '-') return false;
        } else if (!((u[i] >= '0' && u[i] <= '9') || (u[i] >= 'a' && u[i] <= 'f'))) {
            return false;
        }
    }
    return u[14] == '4' && (u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
}
}  // namespace

XC_TEST(protocol, uuid_and_handshake) {
    auto a = proto::newUuid(), b = proto::newUuid();
    XC_CHECK(isUuidV4(a));
    XC_CHECK(isUuidV4(b));
    XC_CHECK(a != b);
    auto h = json::parse(proto::messageHandshake());
    XC_CHECK_EQ(h["type"].get<std::string>(), std::string("Handshake"));
    XC_CHECK_EQ(h["version"].get<std::string>(), std::string("messageV1"));
    XC_CHECK_EQ(h["cv"].get<std::string>(), std::string("0"));
    XC_CHECK(isUuidV4(h["id"].get<std::string>()));
    XC_CHECK_EQ(h.size(), size_t(4));
}

XC_TEST(protocol, message_init_sequence_exact_content) {
    Settings s;
    s.bitrateKbps = 15000;
    const std::string install = "c97d7ee0-73b2-4239-bf1d-9d805a338429";
    auto msgs = proto::messageInitSequence(s, install, 1280, 720);
    XC_REQUIRE(msgs.size() == 6u);
    const char* targets[] = {"/streaming/systemUi/configuration",
                             "/streaming/properties/clientappinstallidchanged",
                             "/streaming/characteristics/orientationchanged",
                             "/streaming/characteristics/touchinputenabledchanged",
                             "/streaming/characteristics/clientdevicecapabilities",
                             "/streaming/characteristics/dimensionschanged"};
    const char* contents[] = {
        R"({"systemUis":[],"version":[0,2,0]})",
        R"({"clientAppInstallId":"c97d7ee0-73b2-4239-bf1d-9d805a338429"})",
        R"({"orientation":0})",
        R"({"touchInputEnabled":false})",
        R"({"maxBitrateKbps":15000,"maxHeight":720,"maxWidth":1280,"supportsCustomResolution":true,"supportsFps":60,"supportsHdr":false,"supportsHevc":false,"video":{"height":720,"maxBitrateKbps":15000,"maxHeight":720,"maxWidth":1280,"width":1280}})",
        R"({"horizontal":1280,"preferredHeight":720,"preferredWidth":1280,"safeAreaBottom":720,"safeAreaLeft":0,"safeAreaRight":1280,"safeAreaTop":0,"supportsCustomResolution":true,"vertical":720})",
    };
    std::vector<std::string> ids;
    for (size_t i = 0; i < msgs.size(); ++i) {
        auto j = json::parse(msgs[i]);
        XC_CHECK_EQ(j.size(), size_t(5));
        XC_CHECK_EQ(j["type"].get<std::string>(), std::string("Message"));
        XC_CHECK_EQ(j["target"].get<std::string>(), std::string(targets[i]));
        XC_CHECK_EQ(j["cv"].get<std::string>(), std::string(""));
        XC_REQUIRE(j["content"].is_string());  // content is a JSON *string*
        XC_CHECK_EQ(j["content"].get<std::string>(), std::string(contents[i]));
        XC_CHECK(isUuidV4(j["id"].get<std::string>()));
        ids.push_back(j["id"].get<std::string>());
    }
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t k = i + 1; k < ids.size(); ++k) XC_CHECK(ids[i] != ids[k]);

    // empty install id -> generated uuid
    auto gen = proto::messageInitSequence(s, "", 1920, 1080);
    auto c = json::parse(json::parse(gen[1])["content"].get<std::string>());
    XC_CHECK(isUuidV4(c["clientAppInstallId"].get<std::string>()));
}

XC_TEST(protocol, transaction_complete_and_unhandled_exact) {
    XC_CHECK_EQ(proto::transactionComplete("abc", R"({"Result":0})"),
                std::string(R"({"content":"{\"Result\":0}","cv":"","id":"abc","type":"TransactionComplete"})"));
    XC_CHECK_EQ(proto::transactionComplete("x", "{}", "cv.1"),
                std::string(R"({"content":"{}","cv":"cv.1","id":"x","type":"TransactionComplete"})"));
    XC_CHECK_EQ(proto::messageUnhandled("i", "/t", "c"),
                std::string(R"({"cv":"c","id":"i","target":"/t","type":"Unhandled"})"));
    auto env = json::parse(proto::messageEnvelope("/a/b", R"({"k":1})"));
    XC_CHECK_EQ(env["content"].get<std::string>(), std::string(R"({"k":1})"));
    XC_CHECK_EQ(env["target"].get<std::string>(), std::string("/a/b"));
}

XC_TEST(protocol, parse_channel_message) {
    proto::ChannelMessage m;
    XC_REQUIRE(proto::parseChannelMessage(
        R"({"type":"TransactionStart","id":"42","target":"/streaming/systemUi/messages/ShowMessageDialog","content":"{\"TitleText\":\"Hi\"}","cv":"abc.1"})",
        m));
    XC_CHECK_EQ(m.type, std::string("TransactionStart"));
    XC_CHECK_EQ(m.id, std::string("42"));
    XC_CHECK_EQ(m.target, std::string(proto::target::kShowMessageDialog));
    XC_CHECK_EQ(m.content, std::string(R"({"TitleText":"Hi"})"));
    XC_CHECK_EQ(m.cv, std::string("abc.1"));
    XC_REQUIRE(proto::parseChannelMessage(R"({"type":"Message","content":{"reason":"x"}})", m));
    XC_CHECK_EQ(m.content, std::string(R"({"reason":"x"})"));
    XC_CHECK(m.id.empty());
    XC_CHECK(proto::isHandshakeAck(R"({"type":"HandshakeAck","version":"messageV1","id":"x","cv":"0"})"));
    XC_CHECK(!proto::isHandshakeAck(R"({"type":"Handshake"})"));
    XC_CHECK(!proto::isHandshakeAck("not json"));
    XC_CHECK(!proto::parseChannelMessage("[1,2]", m));
    XC_CHECK(!proto::parseChannelMessage(R"({"type":5})", m));
}

XC_TEST(protocol, parse_vibration_after_server_metadata) {
    // reportType 0x90 = ServerMetadata | Vibration: 8 bytes of size, then the vibration section
    const uint8_t pkt[] = {0x90, 0x00, 0xD0, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
                           0x00, 0x01, 7, 8, 9, 10, 0x10, 0x00, 0x20, 0x00, 3};
    Vibration v;
    XC_REQUIRE(proto::parseVibration(pkt, sizeof(pkt), v));
    XC_CHECK_EQ(v.pad, uint8_t(1));
    XC_CHECK_EQ(v.left, uint8_t(7));
    XC_CHECK_EQ(v.right, uint8_t(8));
    XC_CHECK_EQ(v.lt, uint8_t(9));
    XC_CHECK_EQ(v.rt, uint8_t(10));
    XC_CHECK_EQ(v.durationMs, uint16_t(16));
    XC_CHECK_EQ(v.delayMs, uint16_t(32));
    XC_CHECK_EQ(v.repeat, uint8_t(3));
    uint32_t w = 0, h = 0;
    XC_REQUIRE(proto::parseServerMetadata(pkt, sizeof(pkt), w, h));
    XC_CHECK_EQ(w, 1280u);
    XC_CHECK_EQ(h, 720u);
    XC_CHECK(!proto::parseVibration(pkt, sizeof(pkt) - 1, v));  // truncated
    const uint8_t only[] = {0x80, 0x00, 0, 0, 1, 2, 3, 4, 0, 0, 0, 0};  // 12 bytes, one short
    XC_CHECK(!proto::parseVibration(only, sizeof(only), v));
    XC_CHECK(!proto::parseServerMetadata(only, sizeof(only), w, h));
    XC_CHECK(!proto::parseVibration(nullptr, 0, v));
}

XC_TEST(protocol, teredo_forms) {
    std::string ip;
    uint16_t port = 0;
    // compressed zero run + upper-case + zone id
    XC_REQUIRE(proto::teredoDecode("2001::4136:E378:8000:63BF:3FFF:FDD2", ip, port));
    XC_CHECK_EQ(ip, std::string("192.0.2.45"));
    XC_CHECK_EQ(port, uint16_t(40000));
    XC_REQUIRE(proto::teredoDecode(" 2001:0:4136:e378:8000:63bf:3fff:fdd2%en0 ", ip, port));
    XC_CHECK_EQ(ip, std::string("192.0.2.45"));
    // all-ones fields invert to zero
    XC_REQUIRE(proto::teredoDecode("2001:0:0:0:0:ffff:ffff:ffff", ip, port));
    XC_CHECK_EQ(ip, std::string("0.0.0.0"));
    XC_CHECK_EQ(port, uint16_t(0));
    // 2001:db8::/32 is documentation space, not Teredo
    XC_CHECK(!proto::teredoDecode("2001:db8::1", ip, port));
    XC_CHECK(ip.empty());
    XC_CHECK(!proto::teredoDecode("", ip, port));
}

XC_TEST(protocol, teredo_expansion_exact) {
    std::vector<std::string> in = {
        "a=candidate:1 1 UDP 100 13.104.1.2 1234 typ host",
        "a=candidate:2 1 UDP 2130706431 2001:0:4136:e378:8000:63bf:3fff:fdd2 9002 typ host",
        "candidate:3 1 udp 50 2001:0:4136:e378:8000:63bf:3fff:fdd2 9003 typ srflx raddr 0.0.0.0 rport 0",
        "a=end-of-candidates",
    };
    auto out = proto::expandTeredoCandidates(in);
    const std::vector<std::string> want = {
        "a=candidate:1 1 UDP 100 13.104.1.2 1234 typ host",
        "a=candidate:20 1 UDP 1 192.0.2.45 40000 typ host",
        "a=candidate:21 1 UDP 1 192.0.2.45 9002 typ host",
        "a=candidate:2 1 UDP 2130706431 2001:0:4136:e378:8000:63bf:3fff:fdd2 9002 typ host",
        "candidate:22 1 UDP 1 192.0.2.45 40000 typ host",
        "candidate:23 1 UDP 1 192.0.2.45 9002 typ host",
        "candidate:3 1 udp 50 2001:0:4136:e378:8000:63bf:3fff:fdd2 9003 typ srflx raddr 0.0.0.0 rport 0",
        "a=end-of-candidates",
    };
    XC_REQUIRE(out.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) XC_CHECK_EQ(out[i], want[i]);
}

XC_TEST(protocol, ice_post_body_exact) {
    std::vector<std::string> cands = {
        "a=candidate:1 1 UDP 2122260223 192.168.1.2 50000 typ host\r\n",
        "candidate:2 1 TCP 1518280447 192.168.1.2 9 typ host tcptype active",
        "garbage",
    };
    const std::string want =
        R"({"candidate":[)"
        R"("{\"candidate\":\"candidate:1 1 UDP 2122260223 192.168.1.2 50000 typ host\",\"sdpMLineIndex\":0,\"sdpMid\":\"0\",\"usernameFragment\":\"Uf\"}",)"
        R"("{\"candidate\":\"a=end-of-candidates\",\"sdpMLineIndex\":0,\"sdpMid\":\"0\",\"usernameFragment\":\"Uf\"}"],)"
        R"("messageType":"iceCandidate"})";
    XC_CHECK_EQ(proto::icePostBody(cands, "Uf"), want);
}

XC_TEST(protocol, sdp_post_body_exact) {
    const std::string body = proto::sdpPostBody("v=0\r\n");
    const std::string want =
        R"({"configuration":{"chat":{"maxVersion":1,"minVersion":1},)"
        R"("chatConfiguration":{"bytesPerSample":2,"expectedClipDurationMs":20,"format":{"codec":"opus","container":"webm"},"numChannels":1,"sampleFrequencyHz":24000},)"
        R"("control":{"maxVersion":3,"minVersion":1},"input":{"maxVersion":9,"minVersion":1},"message":{"maxVersion":1,"minVersion":1},)"
        R"("reliableinput":{"maxVersion":9,"minVersion":9},"unreliableinput":{"maxVersion":9,"minVersion":9}},)"
        R"("messageType":"offer","requestId":"1","sdp":"v=0\r\n"})";
    XC_CHECK_EQ(body, want);
}

namespace {
std::string expectedOffer(const char* profile, const char* caps) {
    return std::string("v=0\r\n"
                       "o=- 4611731400430051 2 IN IP4 127.0.0.1\r\n"
                       "s=-\r\n"
                       "t=0 0\r\n"
                       "a=group:BUNDLE video audio 0\r\n"
                       "a=ice-ufrag:uFrAg\r\n"
                       "a=ice-pwd:pWd\r\n"
                       "a=fingerprint:sha-256 AB:CD\r\n"
                       "a=setup:actpass\r\n"
                       "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
                       "c=IN IP4 0.0.0.0\r\n"
                       "a=mid:video\r\n"
                       "a=rtcp-mux\r\n"
                       "a=rtcp-rsize\r\n"
                       "a=rtpmap:102 H264/90000\r\n"
                       "a=fmtp:102 level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=") +
           profile + ";" + caps +
           "\r\n"
           "a=rtcp-fb:102 goog-remb\r\n"
           "a=rtcp-fb:102 ccm fir\r\n"
           "a=rtcp-fb:102 nack\r\n"
           "a=rtcp-fb:102 nack pli\r\n"
           "a=recvonly\r\n"
           "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
           "c=IN IP4 0.0.0.0\r\n"
           "a=mid:audio\r\n"
           "a=rtcp-mux\r\n"
           "a=rtcp-rsize\r\n"
           "a=rtpmap:111 opus/48000/2\r\n"
           "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1\r\n"
           "a=recvonly\r\n"
           "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
           "c=IN IP4 0.0.0.0\r\n"
           "a=mid:0\r\n"
           "a=sctp-port:5000\r\n"
           "a=max-message-size:262144\r\n";
}
}  // namespace

XC_TEST(protocol, offer_exact_template) {
    XC_CHECK_EQ(proto::buildOffer("uFrAg", "pWd", "AB:CD", false, "1080"),
                expectedOffer("42e01f", "max-fs=8160;max-mbps=489600"));
    XC_CHECK_EQ(proto::buildOffer("uFrAg", "pWd", "AB:CD", false, "1080HQ"),
                expectedOffer("42e01f", "max-fs=8160;max-mbps=489600"));
    XC_CHECK_EQ(proto::buildOffer("uFrAg", "pWd", "AB:CD", false, "720"),
                expectedOffer("42e01f", "max-fs=3600;max-mbps=108000"));
    XC_CHECK_EQ(proto::buildOffer("uFrAg", "pWd", "AB:CD", true, "1080"),
                expectedOffer("42e020", "max-fs=3600;max-mbps=108000"));
    XC_CHECK_EQ(proto::buildOffer("uFrAg\r\n", " pWd", "sha-256 AB:CD", true, "720"),
                expectedOffer("42e020", "max-fs=3600;max-mbps=108000"));
    XC_CHECK(proto::buildOffer("", "p", "AB", false, "720").empty());
    XC_CHECK(proto::buildOffer("u", "", "AB", false, "720").empty());
    XC_CHECK(proto::buildOffer("u", "p", "", false, "720").empty());
}

XC_TEST(protocol, invalid_utf8_never_throws) {
    bool threw = false;
    std::string out;
    try {
        out = proto::transactionComplete(std::string("id\xff"), std::string("{\"x\":\"\xc3\"}"));
        out += proto::messageUnhandled(std::string("\xfe"), "/t");
        out += proto::controlResolution(std::string("\xff"));
        out += proto::icePostBody({std::string("candidate:1 1 UDP 1 1.2.3.4 5 typ host \xff")}, std::string("\xff"));
    } catch (...) {
        threw = true;
    }
    XC_CHECK(!threw);
    XC_CHECK(!out.empty());
}
