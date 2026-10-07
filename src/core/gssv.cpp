// Nubix — gssv session REST API.
// Ported from green-nx src/core/session.cpp (GPL-3.0, (c) green-nx authors). Protocol behaviour
// (/configuration, transferUri, error codes) cross-checked against XStreaming and green-vita.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/gssv.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <functional>

#include <nlohmann/json.hpp>

#include "core/http.hpp"
#include "core/log.hpp"
#include "core/ratelimit.hpp"
#include "core/protocol.hpp"

using nlohmann::json;

namespace xc {

namespace {

constexpr long kRequestTimeoutSec = 20;
// keepalive can block server-side for a while (reports §4: up to 15 s).
constexpr long kKeepAliveTimeoutSec = 25;

std::string trimSlashes(std::string s, bool leading, bool trailing) {
    if (trailing)
        while (!s.empty() && s.back() == '/') s.pop_back();
    if (leading) {
        size_t i = 0;
        while (i < s.size() && s[i] == '/') ++i;
        s.erase(0, i);
    }
    return s;
}

std::string snippet(const std::string& body, size_t max = 300) {
    std::string out = body.substr(0, max);
    for (char& c : out)
        if (c == '\r' || c == '\n') c = ' ';
    return out;
}

// "HTTP 403: {...}" / "transport: <curl error>".
std::string describeFailure(const char* label, const HttpResponse& r) {
    if (!r.error.empty()) return std::string(label) + ": " + r.error;
    std::string s = std::string(label) + ": HTTP " + std::to_string(r.status);
    if (!r.body.empty()) s += ": " + snippet(r.body);
    return s;
}

std::string jsonString(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// Pull {code, message} out of a gssv error body. Shapes seen: {"code":..,"message":..},
// {"errorCode":..,"errorMessage":..}, {"errorDetails":{"code":..,"message":..}}. green-vita
// simply substring-matches the body, so fall back to that for the codes we act on.
void extractError(const std::string& body, std::string& code, std::string& message) {
    json j = json::parse(body, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        const json* src = &j;
        auto ed = j.find("errorDetails");
        if (ed != j.end() && ed->is_object()) src = &*ed;
        code = jsonString(*src, "code");
        if (code.empty()) code = jsonString(*src, "errorCode");
        message = jsonString(*src, "message");
        if (message.empty()) message = jsonString(*src, "errorMessage");
        if (src != &j) {
            if (code.empty()) code = jsonString(j, "code");
            if (message.empty()) message = jsonString(j, "message");
        }
    }
    if (code.empty() && body.find(kErrOfferingDoesNotContainTitle) != std::string::npos)
        code = kErrOfferingDoesNotContainTitle;
}

// Home consoles attach an errorDetails object with all-null fields to perfectly good
// responses (the answer rides right alongside it); only a non-null field is a real failure.
// (green-nx is_real_exchange_error)
bool isRealExchangeError(const json& value) {
    if (!value.is_object()) return false;
    auto it = value.find("errorDetails");
    if (it == value.end() || it->is_null()) return false;
    if (!it->is_object()) return true;
    for (const auto& item : it->items())
        if (!item.value().is_null()) return true;
    return false;
}

// Body is "pending" when the server answers 204 or with nothing at all.
bool isPending(const HttpResponse& r) {
    if (r.status == 204) return true;
    for (char c : r.body)
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') return false;
    return true;
}

// Local UTC offset in minutes (east positive), as the browser's -getTimezoneOffset().
int timezoneOffsetMinutes() {
    std::time_t now = std::time(nullptr);
    std::tm local{}, utc{};
#if defined(_WIN32)
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
#else
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
#endif
    int diff = (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min);
    int dayDiff = local.tm_yday - utc.tm_yday;
    if (local.tm_year != utc.tm_year) dayDiff = local.tm_year > utc.tm_year ? 1 : -1;
    diff += dayDiff * 24 * 60;
    if (diff < -12 * 60 || diff > 14 * 60) diff = 0;
    return diff;
}

}  // namespace

// ---- headers ---------------------------------------------------------------------------------

namespace gssv {

const char* osNameForResolution(const std::string& resolution) {
    if (resolution == "720") return "android";
    if (resolution == "1080HQ") return "tizen";
    return "windows";
}

std::string deviceInfoJson(const std::string& resolution) {
    const bool p720 = resolution == "720";
    // green-nx: report the tier's real display size; a fabricated 4K size risks landing in an
    // unknown-device profile server-side.
    json info = {
        {"appInfo",
         {{"env",
           {{"clientAppId", "www.xbox.com"},
            {"clientAppType", "browser"},
            {"clientAppVersion", "26.1.97"},
            {"clientSdkVersion", "10.3.7"},
            {"httpEnvironment", "prod"},
            {"sdkInstallId", ""}}}}},
        {"dev",
         {{"hw", {{"make", "Microsoft"}, {"model", "unknown"}, {"sdktype", "web"}}},
          {"os", {{"name", osNameForResolution(resolution)}, {"ver", "22631.2715"}, {"platform", "desktop"}}},
          {"displayInfo",
           {{"dimensions", {{"widthInPixels", p720 ? 1280 : 1920}, {"heightInPixels", p720 ? 720 : 1080}}},
            {"pixelDensity", {{"dpiX", 1}, {"dpiY", 1}}}}},
          {"browser", {{"browserName", "chrome"}, {"browserVersion", "140.0.3485.54"}}}}},
    };
    return info.dump();
}

std::string deviceTierFor(bool home, const std::string& resolution) { return home ? std::string("720") : resolution; }

bool isAuthRejection(long status, const std::string& body) {
    if (status == 401) return true;
    if (status != 403) return false;
    std::string code, message;
    extractError(body, code, message);
    return code.empty();
}

std::vector<std::string> requestHeaders(const std::string& gsToken, const std::string& resolution) {
    return {
        "Accept: application/json",
        "Content-Type: application/json",
        "X-Gssv-Client: XboxComBrowser",
        "X-MS-Device-Info: " + deviceInfoJson(resolution),
        "Authorization: Bearer " + gsToken,
    };
}

}  // namespace gssv

// ---- GssvClient ------------------------------------------------------------------------------

GssvClient::GssvClient(const std::string& baseUri, const std::string& gsToken, const Settings& settings)
    : base_(trimSlashes(baseUri, false, true)), token_(gsToken), settings_(settings) {}

std::vector<std::string> GssvClient::headers() const { return gssv::requestHeaders(token_, settings_.resolution); }

void GssvClient::noteResponse(const HttpResponse& r) {
    if (!r.error.empty() || !gssv::isAuthRejection(r.status, r.body)) return;
    if (!authRejected_.exchange(true)) XC_LOGW("gssv: request rejected as unauthorized (HTTP %ld)", r.status);
}

std::string GssvClient::apiUrl(const std::string& path) const {
    return base_ + "/" + trimSlashes(path, true, false);
}

std::string GssvClient::sessionUrl(const SessionInfo& s, const std::string& suffix) const {
    const std::string host = s.host.empty() ? base_ : trimSlashes(s.host, false, true);
    std::string url = host + "/" + trimSlashes(s.sessionPath, true, true);
    if (!suffix.empty()) url += "/" + suffix;
    return url;
}

bool GssvClient::cleanupActive(SessionKind kind) {
    if (base_.empty() || token_.empty()) return false;
    const char* platform = kind == SessionKind::Home ? "home" : "cloud";
    HttpResponse r = Http::request("GET", apiUrl(std::string("v5/sessions/") + platform + "/active"), headers(), "",
                                   kRequestTimeoutSec);
    noteResponse(r);
    if (r.status == 404 || isPending(r)) {
        XC_LOGD("gssv: no active %s sessions (HTTP %ld)", platform, r.status);
        return r.error.empty();
    }
    if (!r.ok()) {
        XC_LOGW("%s", describeFailure("gssv active sessions", r).c_str());
        return false;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded()) {
        XC_LOGW("gssv active sessions: invalid JSON");
        return false;
    }
    // Collect any "sessionPath"/"path" strings anywhere in the response (green-nx).
    std::vector<std::string> paths;
    std::function<void(const json&)> walk = [&](const json& node) {
        if (node.is_array()) {
            for (const json& item : node) walk(item);
        } else if (node.is_object()) {
            for (const auto& item : node.items()) {
                if ((item.key() == "sessionPath" || item.key() == "path") && item.value().is_string())
                    paths.push_back(item.value().get<std::string>());
                walk(item.value());
            }
        }
    };
    walk(parsed);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    bool allOk = true;
    for (const std::string& p : paths) {
        if (p.empty()) continue;
        SessionInfo s;
        s.sessionPath = p;
        XC_LOGI("gssv: stopping stale session %s", p.c_str());
        if (!stop(s)) allOk = false;
    }
    return allOk;
}

bool GssvClient::play(SessionKind kind, const std::string& titleOrServerId, SessionInfo& out, std::string& err) {
    out = SessionInfo{};
    if (base_.empty() || token_.empty()) {
        err = kind == SessionKind::Home ? "no xHome token (remote play not available for this account)"
                                        : "no xCloud token for this offering";
        return false;
    }
    const bool home = kind == SessionKind::Home;
    // Client-side cap on session starts (ratelimit.hpp): a retry loop or impatient re-starts must
    // never turn into a burst of /play requests.
    int waitSec = 0;
    if (!SessionStartLimiter::global().allow(home, steadyNowMs(), waitSec)) {
        out.state = "Failed";
        out.errorCode = "ClientRateLimited";
        err = "Too many session starts in a short time. Please wait " + std::to_string(waitSec) +
              " s and try again.";
        XC_LOGW("gssv: play blocked by the session start limiter (%d s)", waitSec);
        return false;
    }
    // Field values mirror green-nx/green-vita exactly; useIceConnection must stay false (true makes
    // the console agent reject the start with AgentCommandError).
    json body = {
        {"clientSessionId", ""},
        {"titleId", home ? "" : titleOrServerId},
        {"systemUpdateGroup", ""},
        {"settings",
         {{"nanoVersion", "V3;WebrtcTransport.dll"},
          {"enableOptionalDataCollection", false},
          {"enableTextToSpeech", false},
          {"highContrast", 0},
          {"locale", settings_.locale.empty() ? std::string("en-US") : settings_.locale},
          {"useIceConnection", false},
          {"timezoneOffsetMinutes", timezoneOffsetMinutes()},
          {"sdkType", "web"},
          {"osName", gssv::osNameForResolution(settings_.resolution)}}},
        {"serverId", home ? titleOrServerId : ""},
        {"fallbackRegionNames", json::array()},
    };
    const std::string url = apiUrl(home ? "v5/sessions/home/play" : "v5/sessions/cloud/play");
    XC_LOGI("gssv: play %s %s (osName %s)", home ? "home" : "cloud", titleOrServerId.c_str(),
            gssv::osNameForResolution(settings_.resolution));
    HttpResponse r = Http::request("POST", url, headers(), body.dump(), kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        extractError(r.body, out.errorCode, out.errorMessage);
        out.state = "Failed";
        err = describeFailure("session start", r);
        XC_LOGE("%s", err.c_str());
        return false;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        err = "session start: invalid JSON response";
        return false;
    }
    out.sessionPath = trimSlashes(jsonString(parsed, "sessionPath"), true, true);
    if (out.sessionPath.empty()) {
        err = "session start: response has no sessionPath: " + snippet(r.body);
        return false;
    }
    out.sessionId = jsonString(parsed, "sessionId");
    if (out.sessionId.empty()) {
        size_t slash = out.sessionPath.rfind('/');
        out.sessionId = slash == std::string::npos ? out.sessionPath : out.sessionPath.substr(slash + 1);
    }
    out.state = jsonString(parsed, "state");
    if (out.state.empty()) out.state = "New";
    XC_LOGI("gssv: session %s started (%s)", out.sessionId.c_str(), out.state.c_str());
    return true;
}

bool GssvClient::pollState(SessionInfo& io, std::string& err) {
    if (io.sessionPath.empty()) {
        err = "no session";
        return false;
    }
    HttpResponse r = Http::request("GET", sessionUrl(io, "state"), headers(), "", kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        err = describeFailure("session state", r);
        return false;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        err = "session state: invalid JSON response";
        return false;
    }
    const std::string transfer = jsonString(parsed, "transferUri");
    if (!transfer.empty() && trimSlashes(transfer, false, true) != (io.host.empty() ? base_ : io.host)) {
        XC_LOGI("gssv: session transferred to %s", transfer.c_str());
        io.host = trimSlashes(transfer, false, true);
    }
    std::string state = jsonString(parsed, "state");
    if (state == "Error") state = "Failed";
    if (state != io.state) XC_LOGD("gssv: state %s -> %s", io.state.c_str(), state.c_str());
    io.state = state;
    if (state == "Failed") {
        extractError(r.body, io.errorCode, io.errorMessage);
        if (io.errorCode.empty() && io.errorMessage.empty()) io.errorMessage = snippet(r.body);
        XC_LOGE("gssv: session failed: %s %s", io.errorCode.c_str(), io.errorMessage.c_str());
    }
    return true;
}

bool GssvClient::connect(const SessionInfo& s, const std::string& lpt, std::string& err) {
    if (lpt.empty()) {
        err = "connect: empty user token";
        return false;
    }
    json body = {{"userToken", lpt}};
    HttpResponse r = Http::request("POST", sessionUrl(s, "connect"), headers(), body.dump(), kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        err = describeFailure("session connect", r);
        return false;
    }
    XC_LOGI("gssv: connect accepted (HTTP %ld)", r.status);
    return true;
}

int GssvClient::keepAliveSeconds(const SessionInfo& s) {
    constexpr int kDefault = 20;
    HttpResponse r = Http::request("GET", sessionUrl(s, "configuration"), headers(), "", kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        XC_LOGW("%s", describeFailure("session configuration", r).c_str());
        return kDefault;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return kDefault;
    XC_LOGD("gssv: configuration %s", snippet(r.body, 600).c_str());
    auto it = parsed.find("keepAlivePulseInSeconds");
    if (it == parsed.end()) return kDefault;
    double v = 0;
    if (it->is_number()) v = it->get<double>();
    else if (it->is_string()) v = std::atof(it->get<std::string>().c_str());
    if (!(v > 0)) return kDefault;
    // XStreaming: max(pulse, 5).
    return std::max(5, static_cast<int>(v));
}

int GssvClient::waitTimeSeconds(const std::string& titleId) {
    if (titleId.empty()) return -1;
    HttpResponse r = Http::request("GET", apiUrl("v1/waittime/" + Http::urlEncode(titleId)), headers(), "",
                                   kRequestTimeoutSec);
    if (!r.ok()) {
        XC_LOGD("%s", describeFailure("waittime", r).c_str());
        return -1;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return -1;
    for (const char* key : {"estimatedTotalWaitTimeInSeconds", "estimatedProvisioningTimeInSeconds"}) {
        auto it = parsed.find(key);
        if (it != parsed.end() && it->is_number()) return std::max(0, static_cast<int>(it->get<double>()));
    }
    return -1;
}

bool GssvClient::sendSdpOffer(const SessionInfo& s, const std::string& sdp, std::string& err) {
    HttpResponse r =
        Http::request("POST", sessionUrl(s, "sdp"), headers(), proto::sdpPostBody(sdp), kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        err = describeFailure("sdp offer", r);
        return false;
    }
    return true;
}

bool GssvClient::pollSdpAnswer(const SessionInfo& s, std::string& answerSdp, bool& ready, std::string& err) {
    answerSdp.clear();
    ready = false;
    HttpResponse r = Http::request("GET", sessionUrl(s, "sdp"), headers(), "", kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        err = describeFailure("sdp poll", r);
        return false;
    }
    if (isPending(r)) return true;
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        err = "sdp poll: invalid JSON response";
        return false;
    }
    if (isRealExchangeError(parsed)) {
        err = "sdp exchange: " + snippet(parsed.dump(), 400);
        return false;
    }
    // exchangeResponse is a JSON *string* holding {"sdp":"v=0...","sdpType":"answer",...}.
    json exchange;
    auto it = parsed.find("exchangeResponse");
    if (it == parsed.end() || it->is_null()) return true;  // not answered yet
    if (it->is_string()) exchange = json::parse(it->get<std::string>(), nullptr, false);
    else exchange = *it;
    if (exchange.is_discarded() || !exchange.is_object()) {
        err = "sdp exchange: unparsable exchangeResponse";
        return false;
    }
    const std::string status = jsonString(exchange, "status");
    auto sdpIt = exchange.find("sdp");
    if (sdpIt == exchange.end() || !sdpIt->is_string() || sdpIt->get<std::string>().empty()) {
        err = "sdp exchange: answer has no sdp" + (status.empty() ? std::string() : " (status " + status + ")") +
              ": " + snippet(exchange.dump(), 400);
        return false;
    }
    // Keep the answer verbatim (CRLF intact) — re-serialising corrupted ice-ufrag/pwd (green-nx).
    answerSdp = sdpIt->get<std::string>();
    ready = true;
    return true;
}

bool GssvClient::sendIce(const SessionInfo& s, const std::vector<std::string>& candidateLines,
                         const std::string& ufrag, std::string& err) {
    HttpResponse r = Http::request("POST", sessionUrl(s, "ice"), headers(), proto::icePostBody(candidateLines, ufrag),
                                   kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        err = describeFailure("ice send", r);
        return false;
    }
    if (!isPending(r)) {
        json parsed = json::parse(r.body, nullptr, false);
        if (!parsed.is_discarded() && isRealExchangeError(parsed)) {
            err = "ice send: " + snippet(parsed.dump(), 400);
            return false;
        }
    }
    return true;
}

bool GssvClient::pollIce(const SessionInfo& s, std::vector<std::string>& remoteCandidates, bool& ready,
                         std::string& err) {
    remoteCandidates.clear();
    ready = false;
    HttpResponse r = Http::request("GET", sessionUrl(s, "ice"), headers(), "", kRequestTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        err = describeFailure("ice poll", r);
        return false;
    }
    if (isPending(r)) return true;
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        err = "ice poll: invalid JSON response";
        return false;
    }
    if (isRealExchangeError(parsed)) {
        err = "ice exchange: " + snippet(parsed.dump(), 400);
        return false;
    }
    auto it = parsed.find("exchangeResponse");
    if (it == parsed.end() || it->is_null()) return true;
    json exchange = it->is_string() ? json::parse(it->get<std::string>(), nullptr, false) : *it;
    if (exchange.is_discarded()) {
        err = "ice exchange: unparsable exchangeResponse";
        return false;
    }
    const json* items = nullptr;
    if (exchange.is_array()) items = &exchange;
    else if (exchange.is_object() && exchange.contains("candidates") && exchange["candidates"].is_array())
        items = &exchange["candidates"];
    else if (exchange.is_object() && exchange.contains("candidate") && exchange["candidate"].is_array())
        items = &exchange["candidate"];
    ready = true;
    if (!items) return true;

    for (const json& item : *items) {
        json entry = item;
        if (entry.is_string()) entry = json::parse(entry.get<std::string>(), nullptr, false);
        if (entry.is_discarded() || !entry.is_object()) continue;
        std::string c = jsonString(entry, "candidate");
        if (c.rfind("a=", 0) == 0) c.erase(0, 2);
        if (c.find("end-of-candidates") != std::string::npos) continue;
        if (c.rfind("candidate:", 0) != 0) continue;
        while (!c.empty() && (c.back() == ' ' || c.back() == '\r' || c.back() == '\n')) c.pop_back();
        remoteCandidates.push_back(c);
    }
    XC_LOGD("gssv: %zu remote candidates", remoteCandidates.size());
    return true;
}

bool GssvClient::keepAlive(const SessionInfo& s) {
    if (s.sessionPath.empty()) return false;
    HttpResponse r = Http::request("POST", sessionUrl(s, "keepalive"), headers(), "", kKeepAliveTimeoutSec);
    noteResponse(r);
    if (!r.ok()) {
        XC_LOGW("%s", describeFailure("keepalive", r).c_str());
        return false;
    }
    if (!isPending(r)) {
        json parsed = json::parse(r.body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            const std::string code = jsonString(parsed, "code");
            if (code == "SessionNotActive" || code == "SessionNotFound") {
                XC_LOGW("gssv: keepalive: %s", code.c_str());
                return false;
            }
        }
    }
    return true;
}

bool GssvClient::stop(const SessionInfo& s, long timeoutSec) {
    if (s.sessionPath.empty()) return false;
    HttpResponse r = Http::request("DELETE", sessionUrl(s, ""), headers(), "", timeoutSec > 0 ? timeoutSec : kRequestTimeoutSec);
    if (r.ok() || r.status == 404 || r.status == 410) {
        XC_LOGI("gssv: session %s stopped (HTTP %ld)", s.sessionPath.c_str(), r.status);
        return true;
    }
    XC_LOGW("%s", describeFailure("session stop", r).c_str());
    return false;
}

}  // namespace xc
