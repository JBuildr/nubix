// Nubix — gssv session REST API (xCloud "xgpuweb"/"xgpuwebf2p" and xHome "xhome").
// Endpoints and bodies as used by green-nx, Greenlight and xbox-xcloud-player.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/config.hpp"

namespace xc {

struct HttpResponse;

enum class SessionKind { Cloud, Home };

struct SessionInfo {
    std::string sessionPath;   // e.g. "v5/sessions/cloud/<id>" (relative to base URI)
    std::string sessionId;
    std::string state;         // WaitingForResources | ReadyToConnect | Provisioning | Provisioned | Failed
    std::string errorCode, errorMessage;  // set when state == Failed or the start request failed
    // Host the session lives on. Empty = the client's base URI. Set when /state returns a
    // transferUri (session migrated, XStreaming honours it); all later calls follow it.
    std::string host;
};

// GET {sessionPath}/sdp answer with the exchangeResponse fields voice chat needs.
struct SdpAnswerInfo {
    std::string sdp;          // verbatim answer SDP (CRLF intact)
    int chat = -1;            // exchangeResponse.chat (-1 absent)
    int chatStream = -1;      // exchangeResponse.chatStream (-1 absent)
    std::string summary;      // proto::SdpExchangeFields::summary
    // Set when pollSdpAnswer failed because the server refused the exchange itself (errorDetails,
    // exchangeResponse.status other than "success", answer without sdp) rather than a transport
    // or HTTP error. Polling again will not change the outcome.
    bool rejected = false;
};

// Well-known gssv error codes (SessionInfo::errorCode).
constexpr const char* kErrOfferingDoesNotContainTitle = "OfferingDoesNotContainTitle";

namespace gssv {
// Quality tier -> X-MS-Device-Info os.name and play settings.osName:
// "720" -> "android", "1080" -> "windows", "1080HQ" -> "tizen" (anything else -> "windows").
const char* osNameForResolution(const std::string& resolution);
// X-MS-Device-Info JSON (green-nx 2026 fingerprint) for the given quality tier.
std::string deviceInfoJson(const std::string& resolution);
// Standard gssv request headers (Accept, Content-Type, X-Gssv-Client, X-MS-Device-Info, Bearer).
std::vector<std::string> requestHeaders(const std::string& gsToken, const std::string& resolution);
// Quality tier for gssv calls of a session kind. The xHome console agent only accepts the
// android (720) fingerprint - windows/tizen get AgentCommandError (green-nx engine.cpp, green-vita)
// - so every xhome request uses "720" whatever the user picked; cloud uses the setting.
std::string deviceTierFor(bool home, const std::string& resolution);
// True when an HTTP answer means the gsToken itself was refused: 401, or 403 whose body carries
// no gssv error code (a coded 403 such as OfferingAccessDenied is a real answer).
bool isAuthRejection(long status, const std::string& body);
}  // namespace gssv

// All methods are const-safe w.r.t. internal state and may be called from different threads
// concurrently (e.g. keepAlive() from a keepalive thread while the worker polls /ice).
class GssvClient {
public:
    // baseUri: offering base (Tokens::xcloudBase etc.), gsToken: matching Bearer token.
    GssvClient(const std::string& baseUri, const std::string& gsToken, const Settings& settings);

    // Stop any session still running for this account (GET /v5/sessions/{kind}/active + DELETE).
    bool cleanupActive(SessionKind kind);

    // POST /v5/sessions/{cloud|home}/play. titleOrServerId = titleId (Cloud) or serverId (Home).
    bool play(SessionKind kind, const std::string& titleOrServerId, SessionInfo& out, std::string& err);

    // GET {sessionPath}/state; updates io.state (+ error fields).
    bool pollState(SessionInfo& io, std::string& err);

    // POST {sessionPath}/connect with the user LPT (xCloud, at ReadyToConnect).
    bool connect(const SessionInfo& s, const std::string& lpt, std::string& err);

    // Seconds between keepalives (from /configuration keepAlivePulseInSeconds; default 20).
    int keepAliveSeconds(const SessionInfo& s);

    // Queue wait time in seconds for a cloud title (GET /v1/waittime/{titleId}); -1 if unknown.
    int waitTimeSeconds(const std::string& titleId);

    // POST {sessionPath}/sdp with proto::sdpPostBody(offer, chatStream).
    bool sendSdpOffer(const SessionInfo& s, const std::string& sdp, std::string& err, bool chatStream = false);

    // POST {sessionPath}/sdp with proto::sdpChatRenegotiationBody(sdp) (voice chat second offer).
    bool sendSdpRenegotiation(const SessionInfo& s, const std::string& sdp, std::string& err);

    // GET {sessionPath}/sdp. ready=false while the server has not answered yet (HTTP 204).
    // Forwards to the SdpAnswerInfo overload.
    bool pollSdpAnswer(const SessionInfo& s, std::string& answerSdp, bool& ready, std::string& err);
    // Same, returning every exchange field (logs "sdp exchange fields: ..." once per answer).
    bool pollSdpAnswer(const SessionInfo& s, SdpAnswerInfo& out, bool& ready, std::string& err);

    // POST {sessionPath}/ice with proto::icePostBody(candidateLines, ufrag).
    bool sendIce(const SessionInfo& s, const std::vector<std::string>& candidateLines, const std::string& ufrag,
                 std::string& err);

    // GET {sessionPath}/ice. remoteCandidates = "candidate:..." lines (Teredo not yet expanded).
    bool pollIce(const SessionInfo& s, std::vector<std::string>& remoteCandidates, bool& ready, std::string& err);

    // POST {sessionPath}/keepalive.
    bool keepAlive(const SessionInfo& s);

    // DELETE {sessionPath}. timeoutSec bounds the whole request (shutdown uses a short one).
    bool stop(const SessionInfo& s, long timeoutSec = 20);

    // True once any request of this client was rejected as unauthorized (isAuthRejection):
    // the gsToken is no longer accepted and must be re-authorized (Auth::invalidateStreaming).
    bool authRejected() const { return authRejected_.load(); }

private:
    // Absolute URL for a session sub-resource ("" = the session itself, "state", "sdp", ...).
    std::string sessionUrl(const SessionInfo& s, const std::string& suffix) const;
    // Absolute URL on the base host for an API path ("v5/sessions/cloud/play").
    std::string apiUrl(const std::string& path) const;
    std::vector<std::string> headers() const;
    // Records an auth rejection (see authRejected()).
    void noteResponse(const HttpResponse& r);

    std::string base_, token_;
    Settings settings_;
    std::atomic<bool> authRejected_{false};
};

}  // namespace xc
