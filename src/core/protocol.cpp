// Nubix — xCloud wire protocol helpers.
//
// Input packet layout, control/message JSON and the /sdp + /ice bodies are ported from
// green-nx (GPL-3.0): src/core/xcloud_protocol.cpp and
// src/core/session.cpp. The offer template combines GreenOvercast's buildCompatibleOffer
// (src/session/webrtc_session.zig, MPL-2.0; session header + data-channel m-line, proven with
// libdatachannel) with green-nx's media m-lines (deps/patches/libpeer-switch.patch,
// sdp_append_h264/sdp_append_opus + session.cpp sdp_scale_video_caps_1080).
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Portions derived from GreenOvercast (https://github.com/Producdevity/GreenOvercast), MPL-2.0, used here under GPL-3.0 (MPL-2.0 section 3.3).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace xc {
namespace proto {

using nlohmann::json;

// Serialize without ever throwing: invalid UTF-8 (e.g. from a hostile server payload echoed
// back) is replaced with U+FFFD instead of raising json::type_error.
constexpr json::error_handler_t kReplace = json::error_handler_t::replace;

namespace {

// ---- little-endian writers (explicit byte order, independent of host endianness) ----------

void putU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t(v >> 8));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}

void putU32BE(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}

void putF64(std::vector<uint8_t>& out, double v) {
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit");
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t((bits >> (8 * i)) & 0xFF));
}

uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }

uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// ---- string helpers -----------------------------------------------------------------------

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> splitWs(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> out;
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool iequals(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace

// ============================================================================================
// Input channel
// ============================================================================================

InputSerializer::InputSerializer() : start_(std::chrono::steady_clock::now()) {}

void InputSerializer::reset() {
    seq_ = 0;
    start_ = std::chrono::steady_clock::now();
}

double InputSerializer::elapsedMs() const {
    auto d = std::chrono::steady_clock::now() - start_;
    double ms = std::chrono::duration<double, std::milli>(d).count();
    return ms < 0.0 ? 0.0 : ms;
}

std::vector<uint8_t> InputSerializer::clientMetadata(uint8_t maxTouch) {
    std::vector<uint8_t> out;
    out.reserve(kClientMetadataPacketSize);
    putU16(out, report::ClientMetadata);
    putU32(out, ++seq_);
    putF64(out, elapsedMs());
    putU8(out, maxTouch);
    return out;
}

std::vector<uint8_t> InputSerializer::gamepad(const GamepadState& st, uint8_t index) {
    std::vector<uint8_t> out;
    out.reserve(kGamepadPacketSize);
    // 14-byte header
    putU16(out, report::Gamepad);
    putU32(out, ++seq_);
    putF64(out, elapsedMs());
    // Gamepad section: u8 count, then one 23-byte frame (offsets relative to the frame start).
    putU8(out, 1);
    putU8(out, index);          // +0  gamepadIndex
    putU16(out, st.buttons);    // +1  buttons
    putU16(out, uint16_t(st.lx));  // +3  LeftThumbX
    putU16(out, uint16_t(st.ly));  // +5  LeftThumbY (Xbox convention, up = +)
    putU16(out, uint16_t(st.rx));  // +7  RightThumbX
    putU16(out, uint16_t(st.ry));  // +9  RightThumbY
    putU16(out, st.lt);         // +11 LeftTrigger
    putU16(out, st.rt);         // +13 RightTrigger
    putU32(out, 1);             // +15 PhysicalPhysicality, little-endian 1
    putU32BE(out, 1);           // +19 VirtualPhysicality, big-endian 1 (packet.ts setUint32(.., 1, false))
    return out;
}

void InputSerializer::rollback() {
    if (seq_ > 0) --seq_;
}

// ============================================================================================
// Control / message channel JSON
// ============================================================================================

std::string newUuid() {
    static std::mutex m;
    static std::mt19937_64 rng = [] {
        std::vector<uint32_t> seed;
        try {
            std::random_device rd;  // arc4random on FreeBSD/PS5 libc++, /dev/urandom elsewhere
            for (int i = 0; i < 8; ++i) seed.push_back(rd());
        } catch (...) {
            // no entropy source: fall back to clocks + addresses (uniqueness, not secrecy, matters)
        }
        const auto now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto wall = uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
        const auto addr = uint64_t(reinterpret_cast<uintptr_t>(&seed));
        for (uint64_t v : {now, wall, addr}) {
            seed.push_back(uint32_t(v));
            seed.push_back(uint32_t(v >> 32));
        }
        std::seed_seq seq(seed.begin(), seed.end());
        return std::mt19937_64(seq);
    }();
    uint8_t b[16];
    {
        std::lock_guard<std::mutex> lock(m);
        uint64_t hi = rng(), lo = rng();
        for (int i = 0; i < 8; ++i) {
            b[i] = uint8_t(hi >> (8 * i));
            b[8 + i] = uint8_t(lo >> (8 * i));
        }
    }
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
    char s[37];
    std::snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(s);
}

std::string messageHandshake() {
    return json{{"type", "Handshake"}, {"version", "messageV1"}, {"id", newUuid()}, {"cv", "0"}}.dump(-1, ' ', false, kReplace);
}

std::string controlAuthorization() {
    return json{{"message", "authorizationRequest"}, {"accessKey", kControlAccessKey}}.dump(-1, ' ', false, kReplace);
}

std::string controlGamepadChanged(int idx, bool added) {
    return json{{"message", "gamepadChanged"}, {"gamepadIndex", idx}, {"wasAdded", added}}.dump(-1, ' ', false, kReplace);
}

std::string controlResolution(const std::string& alias) {
    return json{{"message", "userRequestedResolutionUpdate"}, {"resolutionAlias", alias}}.dump(-1, ' ', false, kReplace);
}

std::string controlKeyframeRequest() {
    return json{{"message", "videoKeyframeRequested"}, {"ifrRequested", true}}.dump(-1, ' ', false, kReplace);
}

std::string messageEnvelope(const std::string& target, const std::string& contentJson) {
    return json{{"type", "Message"}, {"content", contentJson}, {"id", newUuid()}, {"target", target}, {"cv", ""}}
        .dump(-1, ' ', false, kReplace);
}

std::vector<std::string> messageInitSequence(const Settings& s, const std::string& installId, int w, int h) {
    const int fps = 60;
    const int kbps = s.bitrateKbps > 0 ? s.bitrateKbps : 12000;
    const std::string install = installId.empty() ? newUuid() : installId;
    std::vector<std::string> out;
    out.push_back(messageEnvelope("/streaming/systemUi/configuration",
                                  json{{"version", {0, 2, 0}}, {"systemUis", json::array()}}.dump(-1, ' ', false, kReplace)));
    out.push_back(messageEnvelope("/streaming/properties/clientappinstallidchanged",
                                  json{{"clientAppInstallId", install}}.dump(-1, ' ', false, kReplace)));
    out.push_back(messageEnvelope("/streaming/characteristics/orientationchanged", json{{"orientation", 0}}.dump(-1, ' ', false, kReplace)));
    out.push_back(messageEnvelope("/streaming/characteristics/touchinputenabledchanged",
                                  json{{"touchInputEnabled", false}}.dump(-1, ' ', false, kReplace)));
    out.push_back(messageEnvelope("/streaming/characteristics/clientdevicecapabilities",
                                  json{{"supportsCustomResolution", true},
                                       {"supportsHevc", false},
                                       {"supportsHdr", false},
                                       {"supportsFps", fps},
                                       {"maxWidth", w},
                                       {"maxHeight", h},
                                       {"maxBitrateKbps", kbps},
                                       {"video",
                                        {{"width", w},
                                         {"height", h},
                                         {"maxWidth", w},
                                         {"maxHeight", h},
                                         {"maxBitrateKbps", kbps}}}}
                                      .dump(-1, ' ', false, kReplace)));
    out.push_back(messageEnvelope("/streaming/characteristics/dimensionschanged",
                                  json{{"horizontal", w},
                                       {"vertical", h},
                                       {"preferredWidth", w},
                                       {"preferredHeight", h},
                                       {"safeAreaLeft", 0},
                                       {"safeAreaTop", 0},
                                       {"safeAreaRight", w},
                                       {"safeAreaBottom", h},
                                       {"supportsCustomResolution", true}}
                                      .dump(-1, ' ', false, kReplace)));
    return out;
}

std::string transactionComplete(const std::string& id, const std::string& contentJson, const std::string& cv) {
    return json{{"type", "TransactionComplete"}, {"id", id}, {"content", contentJson}, {"cv", cv}}.dump(-1, ' ', false, kReplace);
}

std::string transactionComplete(const std::string& id, const std::string& contentJson) {
    return transactionComplete(id, contentJson, "");
}

std::string messageUnhandled(const std::string& id, const std::string& target, const std::string& cv) {
    return json{{"type", "Unhandled"}, {"id", id}, {"target", target}, {"cv", cv}}.dump(-1, ' ', false, kReplace);
}

bool parseChannelMessage(const std::string& text, ChannelMessage& out) {
    out = ChannelMessage{};
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto it = j.find("type");
    if (it == j.end() || !it->is_string()) return false;
    out.type = it->get<std::string>();
    auto str = [&](const char* key) -> std::string {
        auto f = j.find(key);
        if (f == j.end() || f->is_null()) return std::string();
        if (f->is_string()) return f->get<std::string>();
        return f->dump();
    };
    out.id = str("id");
    out.target = str("target");
    out.content = str("content");
    out.cv = str("cv");
    return true;
}

bool isHandshakeAck(const std::string& text) {
    ChannelMessage m;
    return parseChannelMessage(text, m) && m.type == "HandshakeAck";
}

// ============================================================================================
// Server -> client input packets
// ============================================================================================

bool parseVibration(const uint8_t* d, size_t n, Vibration& out) {
    if (!d || n < 2) return false;
    const uint16_t type = getU16(d);
    if (!(type & report::Vibration)) return false;
    size_t off = 2;
    if (type & report::ServerMetadata) off += 8;  // sections follow in bit order
    if (n < off + 11) return false;
    const uint8_t* p = d + off;
    // p[0] = rumbleType (0 = FourMotorRumble), ignored like every reference client.
    out.pad = p[1];
    out.left = p[2];
    out.right = p[3];
    out.lt = p[4];
    out.rt = p[5];
    out.durationMs = getU16(p + 6);
    out.delayMs = getU16(p + 8);
    out.repeat = p[10];
    return true;
}

bool parseServerMetadata(const uint8_t* d, size_t n, uint32_t& width, uint32_t& height) {
    width = height = 0;
    if (!d || n < 10) return false;
    if (!(getU16(d) & report::ServerMetadata)) return false;
    height = getU32(d + 2);  // height first
    width = getU32(d + 6);
    return true;
}

// ============================================================================================
// Teredo
// ============================================================================================

bool teredoDecode(const std::string& ipv6, std::string& ipv4, uint16_t& port) {
    ipv4.clear();
    port = 0;
    std::string addr = trimmed(ipv6);
    if (!addr.empty() && addr.front() == '[' && addr.back() == ']') addr = addr.substr(1, addr.size() - 2);
    size_t pct = addr.find('%');
    if (pct != std::string::npos) addr.resize(pct);
    if (addr.find(':') == std::string::npos) return false;
    struct in6_addr a;
    if (inet_pton(AF_INET6, addr.c_str(), &a) != 1) return false;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&a);
    // Teredo prefix 2001:0000::/32
    if (b[0] != 0x20 || b[1] != 0x01 || b[2] != 0x00 || b[3] != 0x00) return false;
    port = uint16_t(((b[10] ^ 0xFF) << 8) | (b[11] ^ 0xFF));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", unsigned(b[12] ^ 0xFF), unsigned(b[13] ^ 0xFF),
                  unsigned(b[14] ^ 0xFF), unsigned(b[15] ^ 0xFF));
    ipv4 = buf;
    return true;
}

std::vector<std::string> expandTeredoCandidates(const std::vector<std::string>& candidates) {
    std::vector<std::string> out;
    out.reserve(candidates.size() * 3);
    int foundation = 20;  // synthetic foundations, as in green-nx receive_ice_candidates
    for (const std::string& line : candidates) {
        std::string body = trimmed(line);
        const bool hasA = startsWith(body, "a=");
        if (hasA) body = body.substr(2);
        if (!startsWith(body, "candidate:")) {
            out.push_back(line);
            continue;
        }
        // candidate:<foundation> <component> <transport> <priority> <address> <port> typ <type> ...
        std::vector<std::string> f = splitWs(body);
        std::string ipv4;
        uint16_t port = 0;
        if (f.size() >= 8 && iequals(f[2], "udp") && teredoDecode(f[4], ipv4, port)) {
            const std::string prefix = hasA ? "a=candidate:" : "candidate:";
            for (uint16_t p : {port, uint16_t(9002)}) {
                out.push_back(prefix + std::to_string(foundation++) + " " + f[1] + " UDP 1 " + ipv4 + " " +
                              std::to_string(p) + " typ host");
            }
        }
        out.push_back(line);
    }
    return out;
}

// ============================================================================================
// SDP offer + signalling bodies
// ============================================================================================

std::string buildOffer(const std::string& ufrag, const std::string& pwd, const std::string& fingerprint, bool home,
                       const std::string& resolution, const OfferMic* mic, int sessionVersion) {
    const std::string u = trimmed(ufrag), p = trimmed(pwd);
    std::string fp = trimmed(fingerprint);
    if (startsWith(fp, "sha-256 ")) fp = trimmed(fp.substr(8));
    if (u.empty() || p.empty() || fp.empty()) return std::string();

    // Video caps: green-nx ships the 720p template verbatim for the 720 tier and for xHome
    // (where the console agent also wants H.264 level 3.2, 42e020); 1080 tiers declare
    // 1920x1080@60 decode capability (8160 MBs, 489600 MB/s).
    const bool is720 = startsWith(resolution, "720");
    const char* profile = home ? "42e020" : "42e01f";
    const char* caps = (home || is720) ? "max-fs=3600;max-mbps=108000" : "max-fs=8160;max-mbps=489600";

    std::string s;
    s.reserve(1600);
    auto L = [&s](const std::string& line) {
        s += line;
        s += "\r\n";
    };
    L("v=0");
    L("o=- 4611731400430051 " + std::to_string(sessionVersion) + " IN IP4 127.0.0.1");
    L("s=-");
    L("t=0 0");
    L("a=group:BUNDLE video audio 0");
    L("a=ice-ufrag:" + u);
    L("a=ice-pwd:" + p);
    L("a=fingerprint:sha-256 " + fp);
    L("a=setup:actpass");
    // video (green-nx sdp_append_h264)
    L("m=video 9 UDP/TLS/RTP/SAVPF 102");
    L("c=IN IP4 0.0.0.0");
    L("a=mid:video");
    L("a=rtcp-mux");
    L("a=rtcp-rsize");
    L("a=rtpmap:102 H264/90000");
    L(std::string("a=fmtp:102 level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=") + profile + ";" +
      caps);
    L("a=rtcp-fb:102 goog-remb");
    L("a=rtcp-fb:102 ccm fir");
    L("a=rtcp-fb:102 nack");
    L("a=rtcp-fb:102 nack pli");
    L("a=recvonly");
    // audio (green-nx sdp_append_opus)
    L("m=audio 9 UDP/TLS/RTP/SAVPF 111");
    L("c=IN IP4 0.0.0.0");
    L("a=mid:audio");
    L("a=rtcp-mux");
    L("a=rtcp-rsize");
    L("a=rtpmap:111 opus/48000/2");
    L("a=fmtp:111 minptime=10;useinbandfec=1;stereo=1");
    if (mic && mic->ssrc != 0) {
        // Voice chat: the microphone goes up on this m-line (xbox.com adds its mic track to the
        // audio transceiver the same way). Attribute order matches libdatachannel's addSSRC().
        const std::string ssrc = std::to_string(mic->ssrc);
        const std::string msid = trimmed(mic->msid);
        const std::string track = trimmed(mic->trackId).empty() ? msid : trimmed(mic->trackId);
        const std::string cname = trimmed(mic->cname).empty() ? std::string("xc") + ssrc : trimmed(mic->cname);
        L("a=sendrecv");
        if (!msid.empty()) L("a=msid:" + msid + " " + track);
        L("a=ssrc:" + ssrc + " cname:" + cname);
        if (!msid.empty()) L("a=ssrc:" + ssrc + " msid:" + msid + " " + track);
    } else {
        L("a=recvonly");
    }
    // data channels (GreenOvercast; libdatachannel assigns the SCTP m-line mid "0")
    L("m=application 9 UDP/DTLS/SCTP webrtc-datachannel");
    L("c=IN IP4 0.0.0.0");
    L("a=mid:0");
    L("a=sctp-port:5000");
    L("a=max-message-size:262144");
    return s;
}

std::string sdpPostBody(const std::string& offer, bool chatStream) {
    json body = {
        {"messageType", "offer"},
        {"sdp", offer},
        {"requestId", "1"},
        {"configuration",
         {{"chatConfiguration",
           {{"bytesPerSample", 2},
            {"expectedClipDurationMs", 20},
            {"format", {{"codec", "opus"}, {"container", "webm"}}},
            {"numChannels", 1},
            {"sampleFrequencyHz", 24000}}},
          {"chat", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"control", {{"minVersion", 1}, {"maxVersion", 3}}},
          {"input", {{"minVersion", 1}, {"maxVersion", 9}}},
          {"message", {{"minVersion", 1}, {"maxVersion", 1}}},
          {"reliableinput", {{"minVersion", 9}, {"maxVersion", 9}}},
          {"unreliableinput", {{"minVersion", 9}, {"maxVersion", 9}}}}},
    };
    // Media-stream chat (xbox.com ChatStreamManager): mic as RTP on the audio m-line.
    if (chatStream) body["configuration"]["chatStream"] = {{"minVersion", 1}, {"maxVersion", 1}};
    return body.dump(-1, ' ', false, kReplace);
}

std::string sdpChatRenegotiationBody(const std::string& offer) {
    json body = {
        {"messageType", "offer"},
        {"requestId", "2"},
        {"sdp", offer},
        {"configuration", {{"isMediaStreamsChatRenegotiation", true}}},
    };
    return body.dump(-1, ' ', false, kReplace);
}

namespace {

// Integer value of an exchange field: numbers, bools and numeric strings; -1 otherwise.
int exchangeInt(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) return -1;
    if (it->is_number_integer() || it->is_number_unsigned()) {
        const long long v = it->get<long long>();
        return v < -1 ? -1 : v > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(v);
    }
    if (it->is_number_float()) {
        const double v = it->get<double>();
        return v != v || v < -1.0 ? -1 : v > 2147483647.0 ? 0x7FFFFFFF : static_cast<int>(v);
    }
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (it->is_string()) {
        const std::string t = trimmed(it->get<std::string>());
        if (t.empty() || t.size() > 9) return -1;
        for (char c : t)
            if (c < '0' || c > '9') return -1;
        return std::atoi(t.c_str());
    }
    return -1;
}

}  // namespace

bool parseExchangeFields(const std::string& exchangeJson, SdpExchangeFields& out) {
    out = SdpExchangeFields{};
    json j = json::parse(exchangeJson, nullptr, false);
    // exchangeResponse is normally a JSON *string* holding the object.
    if (!j.is_discarded() && j.is_string()) j = json::parse(j.get<std::string>(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto sdpIt = j.find("sdp");
    if (sdpIt != j.end() && sdpIt->is_string()) out.sdp = sdpIt->get<std::string>();
    auto stIt = j.find("status");
    if (stIt != j.end() && stIt->is_string()) out.status = stIt->get<std::string>();
    out.chat = exchangeInt(j, "chat");
    out.chatStream = exchangeInt(j, "chatStream");
    json rest = j;
    rest.erase("sdp");
    out.summary = rest.dump(-1, ' ', false, kReplace);
    if (out.summary.size() > 400) out.summary.resize(400);
    return true;
}

std::string sdpMediaDirection(const std::string& sdp, const std::string& mid) {
    std::istringstream in(sdp);
    std::string line;
    bool inMedia = false, match = false;
    std::string dir;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (startsWith(line, "m=")) {
            if (match) break;
            inMedia = true;
            dir.clear();
            continue;
        }
        if (!inMedia) continue;
        if (startsWith(line, "a=mid:")) {
            if (trimmed(line.substr(6)) == mid) match = true;
        } else if (line == "a=sendrecv" || line == "a=sendonly" || line == "a=recvonly" || line == "a=inactive") {
            dir = line.substr(2);
        }
    }
    if (!match) return std::string();
    return dir.empty() ? std::string("sendrecv") : dir;
}

std::string messageSetPartyChatActive(bool partyChatActive, const std::string& cv) {
    return json{{"type", "TransactionStart"},
                {"content", json{{"partyChatActive", partyChatActive}}.dump(-1, ' ', false, kReplace)},
                {"id", newUuid()},
                {"target", target::kSetPartyChatActive},
                {"cv", cv}}
        .dump(-1, ' ', false, kReplace);
}

std::string icePostBody(const std::vector<std::string>& candidates, const std::string& ufrag) {
    auto entry = [&ufrag](const std::string& cand) {
        return json{{"candidate", cand}, {"sdpMid", "0"}, {"sdpMLineIndex", 0}, {"usernameFragment", ufrag}}.dump(-1, ' ', false, kReplace);
    };
    json list = json::array();
    for (const std::string& raw : candidates) {
        std::string c = trimmed(raw);
        if (startsWith(c, "a=")) c = c.substr(2);
        if (!startsWith(c, "candidate:")) continue;
        std::vector<std::string> f = splitWs(c);
        if (f.size() < 8) continue;
        if (iequals(f[2], "tcp")) continue;  // xCloud only does UDP (nano-rs filters TCP too)
        list.push_back(entry(c));
    }
    list.push_back(entry("a=end-of-candidates"));
    return json{{"messageType", "iceCandidate"}, {"candidate", list}}.dump(-1, ' ', false, kReplace);
}

}  // namespace proto
}  // namespace xc
