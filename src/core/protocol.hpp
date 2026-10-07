// Nubix — xCloud wire protocol helpers: input packets, control/message JSON,
// vibration parsing, Teredo decoding, SDP offer template, /sdp and /ice POST bodies.
// Layouts cross-checked against green-nx (xcloud_protocol.cpp) and xbox-xcloud-player
// (src/channel/input/packet.ts, src/channel/message.ts).
// Byte layouts and message shapes ported from green-nx (GPL-3.0,
// src/core/xcloud_protocol.cpp, src/core/session.cpp) and cross-checked against
// xbox-xcloud-player (src/channel/input/packet.ts, src/lib/teredo.ts).
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Portions derived from GreenOvercast (https://github.com/Producdevity/GreenOvercast), MPL-2.0, used here under GPL-3.0 (MPL-2.0 section 3.3).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/config.hpp"

namespace xc {

// Absolute pad state in Xbox conventions. ly/ry already Xbox convention (up = +).
struct GamepadState {
    uint16_t buttons = 0;  // btn:: bitmask
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint16_t lt = 0, rt = 0;  // 0..65535
    bool operator==(const GamepadState& o) const {
        return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry && lt == o.lt &&
               rt == o.rt;
    }
    bool operator!=(const GamepadState& o) const { return !(*this == o); }
};

namespace btn {
enum : uint16_t {
    Nexus = 2, Menu = 4, View = 8, A = 16, B = 32, X = 64, Y = 128,
    DUp = 256, DDown = 512, DLeft = 1024, DRight = 2048,
    LB = 4096, RB = 8192, LS = 16384, RS = 32768
};
}  // namespace btn

// Server -> client rumble request (input channel, reportType bit 128). Motor values 0..100 %.
struct Vibration {
    uint8_t pad = 0, left = 0, right = 0, lt = 0, rt = 0;
    uint16_t durationMs = 0, delayMs = 0;
    uint8_t repeat = 0;
};

namespace proto {

// input channel reportType bits.
namespace report {
enum : uint16_t {
    Metadata = 1, Gamepad = 2, Pointer = 4, ClientMetadata = 8, ServerMetadata = 16,
    Mouse = 32, Keyboard = 64, Vibration = 128
};
}  // namespace report

// Sizes of the fixed input packets (header 14 bytes).
constexpr size_t kInputHeaderSize = 14;
constexpr size_t kClientMetadataPacketSize = 15;  // header + u8 maxTouchpoints
constexpr size_t kGamepadFrameSize = 23;
constexpr size_t kGamepadPacketSize = 38;  // header + u8 count + one 23-byte frame

// Fixed accessKey of the control-channel authorizationRequest (same in every client).
constexpr const char* kControlAccessKey = "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E";

// Builds binary packets for the "input" data channel (protocol "1.0"). Header: u16 reportType,
// u32 sequence (+1 per packet, no gaps), f64 timestamp ms. All little-endian except
// VirtualPhysicality (big-endian 1, as xbox-xcloud-player writes it).
//
// Sequence numbering follows green-nx: pre-incremented, so the first packet (ClientMetadata)
// carries 1. Build each packet right before sending it and call rollback() if the send fails,
// otherwise the server sees a gap and stops applying input. Not thread-safe; guard externally.
class InputSerializer {
public:
    InputSerializer();
    // First packet after the channel opens: reportType 8 + u8 maxTouchpoints (15 bytes).
    std::vector<uint8_t> clientMetadata(uint8_t maxTouch = 1);
    // One gamepad frame (38 bytes).
    std::vector<uint8_t> gamepad(const GamepadState& st, uint8_t index = 0);
    // Undo the last sequence increment (call when a send failed so no gap is created).
    void rollback();
    // Sequence number the next packet will carry.
    uint32_t nextSequence() const { return seq_ + 1; }
    // Start over (new session): sequence back to 0, timestamp reference = now.
    void reset();

private:
    double elapsedMs() const;

    uint32_t seq_ = 0;  // sequence of the last packet built
    std::chrono::steady_clock::time_point start_;  // monotonic reference for the f64 timestamp
};

// "message" channel: {"type":"Handshake","version":"messageV1","id":<uuid>,"cv":"0"} sent when
// the channel opens.
std::string messageHandshake();
// "control" channel: authorizationRequest (accessKey 4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E).
std::string controlAuthorization();
// "control" channel: gamepadChanged {gamepadIndex, wasAdded}.
std::string controlGamepadChanged(int idx, bool added);
// "control" channel: {"message":"userRequestedResolutionUpdate","resolutionAlias":alias}
// (alias "720", "720HQ", "1080", "1080HQ", "1440", "Auto"; unknown values are sent as-is).
std::string controlResolution(const std::string& alias);
// "control" channel: {"message":"videoKeyframeRequested","ifrRequested":true}.
std::string controlKeyframeRequest();
// "message" channel startup messages after HandshakeAck, in send order (green-nx
// startup_messages): systemUi/configuration, clientappinstallidchanged, orientationchanged,
// touchinputenabledchanged, clientdevicecapabilities (w/h, maxBitrateKbps = s.bitrateKbps,
// 60 fps), dimensionschanged (w/h). Each is a "Message" envelope with a fresh uuid.
// An empty installId is replaced by a random uuid.
std::vector<std::string> messageInitSequence(const Settings& s, const std::string& installId, int w, int h);
// "message" channel reply to a TransactionStart with the given id. contentJson is the reply
// object serialized as JSON (embedded as a JSON *string*, e.g. "{\"Result\":0}"); cv "".
std::string transactionComplete(const std::string& id, const std::string& contentJson);

// ---- additional message-channel helpers (additive) --------------------------------------

// Random RFC 4122 v4 UUID, lower-case with dashes (independent of xc::generateUuid).
std::string newUuid();
// One-way "Message" envelope: {"type":"Message","content":contentJson,"id":<uuid>,"target":target,"cv":""}.
std::string messageEnvelope(const std::string& target, const std::string& contentJson);
// Reply for a Message/TransactionStart we do not handle: {"type":"Unhandled","id","target","cv"}.
std::string messageUnhandled(const std::string& id, const std::string& target, const std::string& cv = "");
// TransactionComplete with an explicit correlation vector.
std::string transactionComplete(const std::string& id, const std::string& contentJson, const std::string& cv);

// Parsed "message"-channel frame.
struct ChannelMessage {
    std::string type;     // Handshake, HandshakeAck, Message, TransactionStart, TransactionComplete, ...
    std::string id;
    std::string target;   // e.g. /streaming/sessionLifetimeManagement/serverInitiatedDisconnect
    std::string content;  // raw content (itself JSON text when the server sent a string)
    std::string cv;
};
// Parse a message-channel text frame. False if not a JSON object with a string "type".
bool parseChannelMessage(const std::string& text, ChannelMessage& out);
// True for {"type":"HandshakeAck",...}.
bool isHandshakeAck(const std::string& text);

namespace target {
constexpr const char* kServerInitiatedDisconnect = "/streaming/sessionLifetimeManagement/serverInitiatedDisconnect";
constexpr const char* kShowMessageDialog = "/streaming/systemUi/messages/ShowMessageDialog";
constexpr const char* kShowVirtualKeyboard = "/streaming/systemUi/messages/ShowVirtualKeyboard";
constexpr const char* kTitleInfo = "/streaming/properties/titleinfo";
}  // namespace target

// Parse a server->client input packet carrying a Vibration section. False if not vibration.
bool parseVibration(const uint8_t* d, size_t n, Vibration& out);
// Parse a ServerMetadata section (reportType 16): stream video size. False if absent.
bool parseServerMetadata(const uint8_t* d, size_t n, uint32_t& width, uint32_t& height);

// Teredo (2001:0::/32) address -> client IPv4 + port (bytes 10..15 XOR 0xFF). False if not Teredo.
bool teredoDecode(const std::string& ipv6, std::string& ipv4, uint16_t& port);
// Rewrite remote "a=candidate"/"candidate:" lines: for each Teredo candidate append an IPv4
// candidate with the decoded address and one with port 9002 (xHome). Other lines unchanged.
// Synthetic lines ("candidate:<20+n> <component> UDP 1 <ipv4> <port> typ host", same "a="
// prefix as the source line) come right before the original Teredo line, decoded port first.
std::vector<std::string> expandTeredoCandidates(const std::vector<std::string>& candidates);

// Hand-written browser-compatible offer (GreenOvercast buildCompatibleOffer / xCloud template):
// BUNDLE video audio 0, H.264 profiles, Opus, data channel m-line mid 0, with the given ICE
// credentials and sha-256 fingerprint ("AB:CD:..."). home selects xHome tweaks.
// Session header + application m-line from GreenOvercast (proven with libdatachannel); video and
// audio m-lines are green-nx's known-good template: PT 102 H.264 with
//   cloud 720:          profile-level-id=42e01f;max-fs=3600;max-mbps=108000
//   cloud 1080/1080HQ:  profile-level-id=42e01f;max-fs=8160;max-mbps=489600
//   home (any):         profile-level-id=42e020;max-fs=3600;max-mbps=108000
// and Opus PT 111 "minptime=10;useinbandfec=1;stereo=1". All lines CRLF. Returns "" if
// ufrag, pwd or fingerprint is empty (a leading "sha-256 " on the fingerprint is stripped).
std::string buildOffer(const std::string& ufrag, const std::string& pwd, const std::string& fingerprint, bool home,
                       const std::string& resolution);
// JSON body for POST /sdp: {"messageType":"offer","sdp":..., "requestId":"1","configuration":{...}}.
std::string sdpPostBody(const std::string& offer);
// JSON body for POST /ice (green-nx shape: stringified {candidate,sdpMid:"0",sdpMLineIndex:0,
// usernameFragment} entries + end-of-candidates). Candidate lines may carry an "a=" prefix
// and trailing CR/LF (both stripped); TCP candidates are skipped.
std::string icePostBody(const std::vector<std::string>& candidates, const std::string& ufrag);

}  // namespace proto
}  // namespace xc
