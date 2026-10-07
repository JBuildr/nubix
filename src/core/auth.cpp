// Nubix — Microsoft account + Xbox Live + gssv authentication.
// Flow and request shapes mirror green-nx src/core/auth.cpp (GPL-3.0) and xal-node src/msal.ts:
// MSAL v2 device code (consumers tenant, xbox.com web client id) -> XBL user token (RPS "d=")
// -> XSTS (http://gssv.xboxlive.com/ and http://xboxlive.com) -> /v2/login/user per offering,
// and the login.live.com transfer token (LPT) for xCloud /connect.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/auth.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/http.hpp"
#include "core/http_internal.hpp"
#include "core/log.hpp"

using nlohmann::json;

namespace xc {
namespace {

// Public client id of the xbox.com web app (Azure AD v2, consumers), used by green-nx,
// greenlight/xal-node and XStreaming. The same id is used for the login.live.com LPT request.
constexpr const char* kClientId = "1f907974-e22b-4810-a9de-d9647380c97e";
constexpr const char* kScope = "xboxlive.signin openid profile offline_access";
constexpr const char* kDeviceCodeUrl = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr const char* kTokenUrl = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
constexpr const char* kUserAuthUrl = "https://user.auth.xboxlive.com/user/authenticate";
constexpr const char* kXstsUrl = "https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr const char* kLptUrl = "https://login.live.com/oauth20_token.srf";
constexpr const char* kLptScope = "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN";
constexpr const char* kProfileUrl =
    "https://profile.xboxlive.com/users/me/profile/settings?settings=Gamertag";
constexpr const char* kBrowserUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 "
    "Safari/537.36";

const std::vector<std::string> kFormHeaders = {
    "Content-Type: application/x-www-form-urlencoded",
    "Accept: application/json",
};

const std::vector<std::string> kFormNoCacheHeaders = {
    "Content-Type: application/x-www-form-urlencoded",
    "Accept: application/json",
    "Cache-Control: no-store, must-revalidate, no-cache",
};

// xal-node msal.ts doXstsAuthorization headers (also fine for user/authenticate).
const std::vector<std::string> kXblHeaders = {
    "Content-Type: application/json",
    "Accept: */*",
    "x-xbl-contract-version: 1",
    "Cache-Control: no-cache",
    "Origin: https://www.xbox.com",
    "Referer: https://www.xbox.com/",
    "ms-cv: 0",
    std::string("User-Agent: ") + kBrowserUa,
};

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

json parseJson(const std::string& body) {
    if (body.empty()) return json();
    return json::parse(body, nullptr, /*allow_exceptions=*/false);
}

std::string jStr(const json& j, const char* key) {
    if (!j.is_object()) return std::string();
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

int64_t jInt(const json& j, const char* key, int64_t def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end()) return def;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number()) return static_cast<int64_t>(it->get<double>());
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        char* end = nullptr;
        long long v = std::strtoll(s.c_str(), &end, 10);
        if (end && end != s.c_str()) return v;
    }
    return def;
}

std::string firstLine(std::string s, size_t maxLen = 200) {
    const size_t nl = s.find_first_of("\r\n");
    if (nl != std::string::npos) s.resize(nl);
    if (s.size() > maxLen) s = s.substr(0, maxLen) + "…";
    return s;
}

// Human-readable reason for a failed HTTP call; never includes request bodies (tokens).
std::string describe(const HttpResponse& r, const std::string& label) {
    if (!r.error.empty()) return label + ": network error: " + r.error;
    const json j = parseJson(r.body);
    std::string detail = jStr(j, "error_description");
    if (detail.empty()) detail = jStr(j, "message");
    if (detail.empty()) detail = jStr(j, "code");
    if (detail.empty()) detail = jStr(j, "error");
    if (detail.empty() && !r.body.empty() && !j.is_object()) detail = r.body;
    std::string out = label + " failed (HTTP " + std::to_string(r.status) + ")";
    if (!detail.empty()) out += ": " + firstLine(detail);
    return out;
}

// ISO 8601 UTC ("2026-10-05T10:00:00.1234567Z") -> unix seconds; 0 if unparsable.
int64_t parseIso8601(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    // days_from_civil (Howard Hinnant), avoids timegm() availability questions on the PS5 libc.
    y -= mo <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

// XSTS 401 "XErr" codes (xbox-webapi / xal docs) -> actionable text.
std::string xstsErrorText(const HttpResponse& r, const std::string& label) {
    const json j = parseJson(r.body);
    const int64_t xerr = jInt(j, "XErr", 0);
    switch (xerr) {
        case 2148916227LL: return label + ": this account is banned from Xbox Live";
        case 2148916229LL: return label + ": account restricted by parental controls (online play not allowed)";
        case 2148916233LL: return label + ": this Microsoft account has no Xbox profile yet - sign in once at xbox.com";
        case 2148916234LL: return label + ": the account must accept the Xbox terms of use at xbox.com";
        case 2148916235LL: return label + ": Xbox Live is not available in this account's country";
        case 2148916236LL:
        case 2148916237LL: return label + ": the account requires age verification at login.live.com";
        case 2148916238LL: return label + ": child account - an adult must add it to a Microsoft family";
        default: break;
    }
    std::string out = describe(r, label);
    if (xerr) out += " (XErr " + std::to_string(xerr) + ")";
    return out;
}

struct XboxToken {
    std::string token, uhs, gamertag;
    int64_t notAfter = 0;
};

bool parseXboxToken(const HttpResponse& r, XboxToken& out) {
    const json j = parseJson(r.body);
    out.token = jStr(j, "Token");
    if (out.token.empty()) return false;
    out.notAfter = parseIso8601(jStr(j, "NotAfter"));
    auto dc = j.find("DisplayClaims");
    if (dc != j.end() && dc->is_object()) {
        auto xui = dc->find("xui");
        if (xui != dc->end() && xui->is_array() && !xui->empty()) {
            out.uhs = jStr(xui->front(), "uhs");
            out.gamertag = jStr(xui->front(), "gtg");
        }
    }
    return true;
}

// Outcome of an MSA token-endpoint call.
enum class MsaResult { Ok, Dead, Failed };

void clearXboxTokens(Tokens& t) {
    t.xhomeGs.clear();
    t.xcloudGs.clear();
    t.xcloudF2pGs.clear();
    t.xhomeBase.clear();
    t.xcloudBase.clear();
    t.xcloudF2pBase.clear();
    t.xhomeRegions.clear();
    t.xcloudRegions.clear();
    t.xcloudF2pRegions.clear();
    t.gsExpiry = 0;
    t.uhs.clear();
    t.xblToken.clear();
    t.xblExpiry = 0;
    t.market.clear();
}

// Store an MSA token-endpoint response (device code success or refresh). Keeps the old
// refresh token if the response did not rotate it.
bool storeMsaTokens(Tokens& t, const json& j) {
    const std::string access = jStr(j, "access_token");
    if (access.empty()) return false;
    t.msaAccess = access;
    const std::string rt = jStr(j, "refresh_token");
    if (!rt.empty()) t.msaRefresh = rt;
    t.msaExpiry = now() + jInt(j, "expires_in", 3600);
    return true;
}

// t is the caller's working copy of the tokens; it is updated and the MSA fields are committed
// to cfg (and persisted). Callers hold Auth::mu_, the only writer of tokens.
MsaResult refreshMsa(Config& cfg, Tokens& t, std::string& err) {
    if (t.msaRefresh.empty()) {
        err = "not signed in";
        return MsaResult::Dead;
    }
    // xal-node msal.ts refreshUserToken / green-nx refresh_user_token.
    const std::string body = std::string("client_id=") + kClientId + "&grant_type=refresh_token&refresh_token=" +
                             Http::urlEncode(t.msaRefresh) + "&scope=" + Http::urlEncode(kScope);
    HttpResponse r = Http::request("POST", kTokenUrl, kFormNoCacheHeaders, body);
    if (r.ok()) {
        if (!storeMsaTokens(t, parseJson(r.body))) {
            err = "token refresh: response without access_token";
            return MsaResult::Failed;
        }
        cfg.updateTokens([&t](Tokens& c) {
            c.msaAccess = t.msaAccess;
            c.msaRefresh = t.msaRefresh;
            c.msaExpiry = t.msaExpiry;
        });
        cfg.save();
        XC_LOGI("auth: MSA token refreshed (valid %lld s)", (long long)(t.msaExpiry - now()));
        return MsaResult::Ok;
    }
    if (r.error.empty() && r.status >= 400 && r.status < 500) {
        const json j = parseJson(r.body);
        const std::string kind = jStr(j, "error");
        // invalid_grant: revoked/expired/rotated-away refresh token. interaction_required:
        // consent/password change. Either way the stored login is unusable (msal.ts
        // TokenRefreshError); forget it so the UI shows the device-code screen again.
        if (kind == "invalid_grant" || kind == "interaction_required" || kind == "invalid_client" ||
            kind == "unauthorized_client") {
            XC_LOGW("auth: refresh token rejected (%s); signing out", kind.c_str());
            const std::string gamertag = t.gamertag;
            t = Tokens();
            cfg.setTokens(t);
            cfg.save();
            err = "saved login expired; please sign in again" + (gamertag.empty() ? "" : " (" + gamertag + ")");
            return MsaResult::Dead;
        }
    }
    err = describe(r, "token refresh");
    return MsaResult::Failed;
}

struct OfferingLogin {
    std::string gsToken, market;
    std::vector<Region> regions;
    int64_t duration = 0;
};

// POST https://{offering}.gssv-play-prod.xboxlive.com/v2/login/user (green-nx streaming_login).
bool gssvLogin(const std::string& xsts, const std::string& offering, OfferingLogin& out, std::string& err) {
    const json body = {{"token", xsts}, {"offeringId", offering}};
    const std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Accept: application/json",
        "Cache-Control: no-store, must-revalidate, no-cache",
        "x-gssv-client: XboxComBrowser",
    };
    HttpResponse r;
    {
        // 403 OfferingAccessDenied is a normal outcome (no Game Pass / no console).
        httpdetail::QuietErrors quiet;
        r = Http::request("POST", "https://" + offering + ".gssv-play-prod.xboxlive.com/v2/login/user", headers,
                          body.dump());
    }
    if (!r.ok()) {
        err = describe(r, offering);
        return false;
    }
    const json j = parseJson(r.body);
    out.gsToken = jStr(j, "gsToken");
    out.market = jStr(j, "market");
    out.duration = jInt(j, "durationInSeconds", 14400);
    out.regions.clear();
    auto os = j.find("offeringSettings");
    if (j.is_object() && os != j.end() && os->is_object()) {
        auto regs = os->find("regions");
        if (regs != os->end() && regs->is_array()) {
            for (const auto& e : *regs) {
                if (!e.is_object()) continue;
                Region reg;
                reg.name = jStr(e, "name");
                reg.baseUri = jStr(e, "baseUri");
                auto def = e.find("isDefault");
                reg.isDefault = def != e.end() && def->is_boolean() && def->get<bool>();
                while (!reg.baseUri.empty() && reg.baseUri.back() == '/') reg.baseUri.pop_back();
                if (!reg.baseUri.empty()) out.regions.push_back(std::move(reg));
            }
        }
    }
    if (out.gsToken.empty()) {
        err = offering + ": response without gsToken";
        return false;
    }
    if (out.regions.empty()) {
        err = offering + ": no regions in response";
        return false;
    }
    return true;
}

}  // namespace

Auth::Auth(Config& cfg) : cfg_(cfg) {}

bool Auth::requestDeviceCode(DeviceCode& out, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    const std::string body = std::string("client_id=") + kClientId + "&scope=" + Http::urlEncode(kScope);
    HttpResponse r = Http::request("POST", kDeviceCodeUrl, kFormHeaders, body);
    if (!r.ok()) {
        err = describe(r, "device code request");
        return false;
    }
    const json j = parseJson(r.body);
    out = DeviceCode();
    out.userCode = jStr(j, "user_code");
    out.deviceCode = jStr(j, "device_code");
    out.verificationUri = jStr(j, "verification_uri");
    out.message = jStr(j, "message");
    out.interval = static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(60, jInt(j, "interval", 5))));
    out.expiresIn = static_cast<int>(std::max<int64_t>(30, std::min<int64_t>(3600, jInt(j, "expires_in", 900))));
    if (out.userCode.empty() || out.deviceCode.empty()) {
        err = "device code request: malformed response";
        return false;
    }
    if (out.verificationUri.empty()) out.verificationUri = "https://www.microsoft.com/link";
    // The consumers endpoint has no verification_uri_complete; build the documented ?otc= form
    // so a phone scanning the QR lands on the code screen pre-filled (green-nx).
    out.qrUrl = jStr(j, "verification_uri_complete");
    if (out.qrUrl.empty()) {
        std::string bare;
        for (char c : out.userCode)
            if (c != '-' && c != ' ') bare += c;
        out.qrUrl = out.verificationUri + (out.verificationUri.find('?') == std::string::npos ? "?" : "&") +
                    "otc=" + Http::urlEncode(bare);
    }
    XC_LOGI("auth: device code %s at %s (expires in %d s, poll every %d s)", out.userCode.c_str(),
            out.verificationUri.c_str(), out.expiresIn, out.interval);
    return true;
}

PollResult Auth::pollToken(const DeviceCode& dc, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    const std::string body = std::string("grant_type=") + Http::urlEncode("urn:ietf:params:oauth:grant-type:device_code") +
                             "&client_id=" + kClientId + "&device_code=" + Http::urlEncode(dc.deviceCode);
    HttpResponse r;
    {
        httpdetail::QuietErrors quiet;  // 400 authorization_pending every interval is expected
        r = Http::request("POST", kTokenUrl, kFormHeaders, body);
    }
    if (!r.error.empty() || r.status >= 500 || r.status == 429) {
        // Transient (network blip, server hiccup): keep polling until the code expires.
        err = describe(r, "device code poll");
        XC_LOGW("auth: %s (will retry)", err.c_str());
        return PollResult::Pending;
    }
    const json j = parseJson(r.body);
    if (r.ok()) {
        Tokens t = cfg_.tokens();
        if (!storeMsaTokens(t, j) || jStr(j, "refresh_token").empty()) {
            err = "device code poll: response without access/refresh token";
            return PollResult::Error;
        }
        // A new login may be a different account: drop all derived tokens and the profile.
        clearXboxTokens(t);
        t.gamertag.clear();
        cfg_.setTokens(t);
        cfg_.save();
        XC_LOGI("auth: device code login complete");
        err.clear();
        return PollResult::Success;
    }
    const std::string kind = jStr(j, "error");
    if (kind == "authorization_pending") return PollResult::Pending;
    if (kind == "slow_down") {
        // RFC 8628: the caller should add 5 s to its interval.
        err = "slow_down";
        return PollResult::Pending;
    }
    if (kind == "expired_token" || kind == "code_expired" || kind == "bad_verification_code") {
        err = "the sign-in code expired";
        return PollResult::Expired;
    }
    if (kind == "authorization_declined" || kind == "access_denied") {
        err = "sign-in was declined";
        return PollResult::Denied;
    }
    err = describe(r, "device code poll");
    return PollResult::Error;
}

bool Auth::refresh(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Tokens t = cfg_.tokens();
    return refreshMsa(cfg_, t, err) == MsaResult::Ok;
}

bool Auth::authorizeStreaming(std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    // Work on a copy and publish it in one step on every exit (the UI thread reads the token
    // lists while this runs). Auth::mu_ makes this the only writer meanwhile.
    Tokens t = cfg_.tokens();
    const Settings settings = cfg_.settings();
    struct Commit {
        Config& cfg;
        const Tokens& t;
        ~Commit() { cfg.setTokens(t); }
    } commit{cfg_, t};
    if (t.msaRefresh.empty()) {
        err = "not signed in";
        return false;
    }
    bool refreshed = false;
    if (t.msaAccess.empty() || t.msaExpiry - now() < 120) {
        if (refreshMsa(cfg_, t, err) != MsaResult::Ok) return false;
        refreshed = true;
    }

    // 1. XBL user token (XASU). "d=" prefix for MSAL v2 access tokens.
    XboxToken user;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const json body = {
            {"Properties",
             {{"AuthMethod", "RPS"}, {"RpsTicket", "d=" + t.msaAccess}, {"SiteName", "user.auth.xboxlive.com"}}},
            {"RelyingParty", "http://auth.xboxlive.com"},
            {"TokenType", "JWT"},
        };
        HttpResponse r = Http::request("POST", kUserAuthUrl, kXblHeaders, body.dump());
        if (r.ok() && parseXboxToken(r, user)) break;
        // A rejected RPS ticket usually means the cached MSA access token went stale: refresh once.
        if (!refreshed && r.error.empty() && (r.status == 400 || r.status == 401 || r.status == 403)) {
            XC_LOGW("auth: XBL user token rejected (HTTP %ld); refreshing MSA token", r.status);
            if (refreshMsa(cfg_, t, err) != MsaResult::Ok) return false;
            refreshed = true;
            continue;
        }
        err = r.ok() ? "Xbox Live user authentication: response without Token" : describe(r, "Xbox Live user authentication");
        return false;
    }

    // 2. XSTS for gssv (trailing slash matters).
    auto xsts = [&](const char* rp, XboxToken& out, std::string& e) {
        const json body = {
            {"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", {user.token}}}},
            {"RelyingParty", rp},
            {"TokenType", "JWT"},
        };
        HttpResponse r = Http::request("POST", kXstsUrl, kXblHeaders, body.dump());
        if (r.ok() && parseXboxToken(r, out)) return true;
        e = r.ok() ? std::string("XSTS ") + rp + ": response without Token" : xstsErrorText(r, std::string("XSTS ") + rp);
        return false;
    };
    XboxToken gssvXsts;
    if (!xsts("http://gssv.xboxlive.com/", gssvXsts, err)) return false;

    // 3. XSTS for http://xboxlive.com: web token for profile/smartglass/titlehub (optional).
    XboxToken web;
    std::string webErr;
    if (xsts("http://xboxlive.com", web, webErr)) {
        t.xblToken = web.token;
        t.uhs = !web.uhs.empty() ? web.uhs : gssvXsts.uhs;
        t.xblExpiry = web.notAfter;
    } else {
        XC_LOGW("auth: %s (profile/console features may be limited)", webErr.c_str());
        t.xblToken.clear();
        t.xblExpiry = 0;
        t.uhs = gssvXsts.uhs;
    }

    // 4. Gamertag via profile (optional; green-nx fetch_profile).
    if (!t.xblToken.empty() && !t.uhs.empty()) {
        HttpResponse r = Http::request(
            "GET", kProfileUrl,
            {"x-xbl-contract-version: 3", "Accept: application/json", "Accept-Language: " + settings.locale,
             "Authorization: XBL3.0 x=" + t.uhs + ";" + t.xblToken});
        std::string gamertag;
        if (r.ok()) {
            const json j = parseJson(r.body);
            auto users = j.is_object() ? j.find("profileUsers") : j.end();
            if (j.is_object() && users != j.end() && users->is_array()) {
                for (const auto& u : *users) {
                    auto settings = u.is_object() ? u.find("settings") : u.end();
                    if (!u.is_object() || settings == u.end() || !settings->is_array()) continue;
                    for (const auto& s : *settings)
                        if (jStr(s, "id") == "Gamertag") gamertag = jStr(s, "value");
                }
            }
        }
        if (gamertag.empty()) gamertag = !web.gamertag.empty() ? web.gamertag : gssvXsts.gamertag;
        if (!gamertag.empty()) t.gamertag = gamertag;
        else XC_LOGW("auth: could not read gamertag");
    }

    // 5. gsToken per offering, each optional (green-nx fetch_streaming_credentials; xgpuweb is
    //    optional here because F2P-only and xHome-only accounts are valid users).
    struct Offering {
        const char* id;
        std::string* gs;
        std::string* base;
        std::vector<Region>* regions;
        bool cloud;
    };
    const Offering offerings[] = {
        {"xhome", &t.xhomeGs, &t.xhomeBase, &t.xhomeRegions, false},
        {"xgpuweb", &t.xcloudGs, &t.xcloudBase, &t.xcloudRegions, true},
        {"xgpuwebf2p", &t.xcloudF2pGs, &t.xcloudF2pBase, &t.xcloudF2pRegions, true},
    };
    int64_t minDuration = INT64_MAX;
    int got = 0;
    std::string reasons;
    for (const auto& o : offerings) {
        OfferingLogin login;
        std::string e;
        if (gssvLogin(gssvXsts.token, o.id, login, e)) {
            *o.gs = login.gsToken;
            *o.regions = login.regions;
            *o.base = pickRegionBase(login.regions, o.cloud ? settings.region : std::string());
            if (!login.market.empty()) t.market = login.market;
            minDuration = std::min(minDuration, login.duration);
            ++got;
            XC_LOGI("auth: %s ok, %zu regions, using %s", o.id, login.regions.size(), o.base->c_str());
        } else {
            o.gs->clear();
            o.base->clear();
            o.regions->clear();
            XC_LOGI("auth: %s unavailable: %s", o.id, e.c_str());
            if (!reasons.empty()) reasons += "; ";
            reasons += e;
        }
    }
    if (got == 0) {
        t.gsExpiry = 0;
        cfg_.setTokens(t);
        cfg_.save();
        err = "no streaming access (xhome, xgpuweb, xgpuwebf2p all refused): " + reasons;
        return false;
    }
    t.gsExpiry = now() + minDuration;
    cfg_.setTokens(t);
    cfg_.save();
    XC_LOGI("auth: streaming authorized for %s (market %s, gs valid %lld s)", t.gamertag.c_str(), t.market.c_str(),
            (long long)minDuration);
    err.clear();
    return true;
}

bool Auth::fetchLpt(std::string& outLpt, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    outLpt.clear();
    // green-nx fetch_passport_token refreshes first, so the freshest refresh token is used (and the
    // rotated one persisted). A transient refresh failure is not fatal: the stored one still works.
    std::string rerr;
    Tokens t = cfg_.tokens();
    if (refreshMsa(cfg_, t, rerr) == MsaResult::Dead) {
        err = rerr;
        return false;
    }
    if (t.msaRefresh.empty()) {
        err = "not signed in";
        return false;
    }
    const std::string body = std::string("client_id=") + kClientId + "&scope=" + Http::urlEncode(kLptScope) +
                             "&grant_type=refresh_token&refresh_token=" + Http::urlEncode(t.msaRefresh);
    HttpResponse r = Http::request("POST", kLptUrl, kFormHeaders, body);
    if (!r.ok()) {
        err = describe(r, "transfer token (LPT)");
        return false;
    }
    outLpt = jStr(parseJson(r.body), "access_token");
    if (outLpt.empty()) {
        err = "transfer token (LPT): response without access_token";
        return false;
    }
    // The response also carries a login.live.com refresh token for this purpose scope; it is not a
    // replacement for the MSAL v2 refresh token, so it is deliberately not stored (xal-node, green-nx).
    XC_LOGI("auth: transfer token obtained");
    return true;
}

bool Auth::isLoggedIn() const {
    // No Auth::mu_ here: the UI asks this while a long refresh may hold it.
    return !cfg_.tokens().msaRefresh.empty();
}

void Auth::logout() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    cfg_.setTokens(Tokens());
    cfg_.save();
    XC_LOGI("auth: signed out");
}

void Auth::invalidateStreaming() {
    // Deliberately no Auth::mu_: a session thread reports the rejection while another thread may
    // be inside a long re-authorization (which then publishes a fresh expiry anyway).
    cfg_.updateTokens([](Tokens& t) { t.gsExpiry = 0; });
    XC_LOGW("auth: streaming token rejected by the server; it will be re-authorized");
}

bool Auth::ensureFresh(std::string& err, int marginSec) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Tokens t = cfg_.tokens();
    if (t.msaRefresh.empty()) {
        err = "not signed in";
        return false;
    }
    const int64_t n = now();
    const bool noGs = t.xhomeGs.empty() && t.xcloudGs.empty() && t.xcloudF2pGs.empty();
    // gsExpiry <= 0: never authorized, or invalidateStreaming() after a server rejection.
    if (noGs || t.gsExpiry <= 0 || t.gsExpiry - n < marginSec) {
        // authorizeStreaming refreshes the MSA token itself when needed.
        if (t.msaExpiry - n < marginSec) {
            if (refreshMsa(cfg_, t, err) != MsaResult::Ok) return false;
        }
        return authorizeStreaming(err);
    }
    if (t.msaAccess.empty() || t.msaExpiry - n < marginSec) {
        if (refreshMsa(cfg_, t, err) != MsaResult::Ok) return false;
    }
    err.clear();
    return true;
}

void Auth::applyRegion() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    const std::string region = cfg_.settings().region;
    std::string cloudBase;
    cfg_.updateTokens([&](Tokens& t) {
        t.xcloudBase = pickRegionBase(t.xcloudRegions, region);
        t.xcloudF2pBase = pickRegionBase(t.xcloudF2pRegions, region);
        t.xhomeBase = pickRegionBase(t.xhomeRegions, std::string());
        cloudBase = t.xcloudBase;
    });
    cfg_.save();
    XC_LOGI("auth: region '%s' -> cloud %s", region.c_str(), cloudBase.c_str());
}

}  // namespace xc
