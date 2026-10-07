// Nubix — persisted settings and tokens (JSON file).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace xc {

struct Settings {
    std::string resolution = "1080";  // "720" | "1080" | "1080HQ"
    int bitrateKbps = 12000;          // requested max bitrate (REMB / clientdevicecapabilities), 5000..20000
    std::string region = "";          // gssv region name; empty = server default
    std::string locale = "en-US";     // used for catalog + session start
    bool f2pFallback = true;          // retry cloud start with the F2P offering on OfferingDoesNotContainTitle
    int stickDeadzone = 0;            // extra radial deadzone in percent (0 = rely on server)
};

// One gssv region of an offering, from /v2/login/user offeringSettings.regions[].
struct Region {
    std::string name;     // e.g. "WestEurope"
    std::string baseUri;  // e.g. https://weu.core.gssv-play-prod.xboxlive.com
    bool isDefault = false;
};

struct Tokens {
    std::string msaRefresh, msaAccess;         // MSA (login.live.com) refresh + access token
    int64_t msaExpiry = 0;                     // unix seconds when msaAccess expires
    std::string xhomeGs, xcloudGs, xcloudF2pGs;        // gssv gsToken per offering (empty = not entitled)
    std::string xhomeBase, xcloudBase, xcloudF2pBase;  // base URI per offering, e.g. https://weu.core.gssv-play-prod.xboxlive.com
    int64_t gsExpiry = 0;                      // unix seconds when the gs tokens expire
    std::string gamertag, uhs, xblToken;       // profile + XBL user token (for XSTS / displaycatalog)
    // xblToken is the XSTS token for RelyingParty http://xboxlive.com and uhs its user hash:
    // use as "Authorization: XBL3.0 x=<uhs>;<xblToken>" (profile, smartglass, titlehub, ...).
    int64_t xblExpiry = 0;                     // unix seconds when xblToken expires (XSTS NotAfter)
    // Regions per offering (empty = offering not entitled). The *Base fields above hold the
    // selected region's baseUri (Settings::region for cloud offerings, else isDefault).
    std::vector<Region> xhomeRegions, xcloudRegions, xcloudF2pRegions;
    std::string market;                        // gssv "market" (country), e.g. "DE"
};

// Thread-safe: settings and tokens are shared by the UI thread, the App worker (Auth, Catalog)
// and the Streamer worker, so they are only reachable through copies (snapshots) and locked
// updates. Never hold a reference into them across threads.
class Config {
public:
    // path: JSON file location, normally platform::dataDir() + "/config.json".
    explicit Config(std::string path = "");

    // Load from path. Missing file = defaults (returns true); malformed JSON = defaults + false.
    // Generates installId if absent.
    bool load();

    // Atomically write settings + tokens to path (write temp file, then rename). The state is
    // serialised under the lock; the file I/O runs outside it (serialised among savers).
    bool save();

    // File location set at construction / via setPath() (set before threads start).
    const std::string& path() const { return path_; }
    void setPath(const std::string& p) { path_ = p; }

    // Snapshots (copies taken under the lock).
    Settings settings() const;
    Tokens tokens() const;
    std::string gamertag() const;
    // Random UUID v4, generated once and persisted.
    std::string installId() const;

    // Locked writes. The callback runs with the lock held: it must not call back into Config.
    // None of these persist; call save() afterwards.
    void setSettings(const Settings& s);
    void updateSettings(const std::function<void(Settings&)>& fn);
    void setTokens(const Tokens& t);
    void updateTokens(const std::function<void(Tokens&)>& fn);

    // Incremented on every change of settings or tokens (load included): lets per-frame UI code
    // refresh its cached snapshot only when something changed.
    uint64_t revision() const { return revision_.load(); }

private:
    std::string path_;
    mutable std::mutex mu_;
    Settings settings_;
    Tokens tokens_;
    std::string installId_;
    std::atomic<uint64_t> revision_{0};
};

// Pick a region baseUri: the region named `preferred` if present, else the isDefault one,
// else the first; "" if regions is empty.
std::string pickRegionBase(const std::vector<Region>& regions, const std::string& preferred);

// Generate a random RFC 4122 v4 UUID string (lower-case, with dashes).
std::string generateUuid();

}  // namespace xc
