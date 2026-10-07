// Nubix — cloud title library and xHome console list.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.hpp"

namespace xc {

struct Title {
    std::string titleId;    // gssv titleId (used for /play and /waittime)
    std::string productId;  // Store product id (displaycatalog)
    std::string name, publisher;
    std::string imageUrl;   // box art (poster) URL
    bool hasEntitlement = false;  // owned / playable with current offering
    bool f2p = false;             // from the xgpuwebf2p offering
    bool gamePass = false;        // part of Game Pass
    bool f2pOnly = false;         // playable only via the xgpuwebf2p offering (not owned, not in plan):
                                  // start the session with xcloudF2pGs/xcloudF2pBase
    int maxSessionSeconds = 0;    // session cap (ad-supported runs); 0 = no cap
};

struct Console {
    std::string serverId;     // xHome serverId (used for /play)
    std::string name;         // user-given console name
    std::string powerState;   // "On", "ConnectedStandby", ...
    std::string consoleType;  // "XboxSeriesX", "XboxOne", ...
};

class Catalog {
public:
    explicit Catalog(Config& cfg);

    // GET {xcloudBase}/v2/titles (+ f2p offering), merged, names/art hydrated via displaycatalog
    // (batched 20 product ids per request). Sorted by name. authRejected (optional) is set when
    // the failure was the gsToken being refused (401/403): re-authorize and retry.
    // The calling thread's Http abort flag also cancels the helper threads.
    // Results are cached on disk (setCacheDir) for kTitleCacheTtlSec per account/offering: within
    // that time the library is served from the cache unless forceRefresh. When the network fetch
    // fails for a reason other than a rejected token, a stale cache is returned instead.
    bool fetchCloudTitles(std::vector<Title>& out, std::string& err, bool* authRejected = nullptr,
                          bool forceRefresh = false);

    // GET {xhomeBase}/v6/servers/home — consoles registered for remote play (android/720
    // device fingerprint, as for every xhome call). authRejected as above.
    bool fetchConsoles(std::vector<Console>& out, std::string& err, bool* authRejected = nullptr);

    // Download an image (box art). Appends size hints (?w=…) where the CDN supports it. Served
    // from / stored in <cacheDir>/img when a cache dir is set (box art URLs are immutable).
    static bool fetchImage(const std::string& url, std::vector<uint8_t>& bytes);

    // Directory for the title list and box-art caches (created on demand). Empty = no caching.
    static void setCacheDir(const std::string& dir);
    static constexpr long kTitleCacheTtlSec = 6 * 3600;

    // Fill name/publisher/imageUrl for titles that have a productId but no name yet
    // (displaycatalog, batched 20, no auth). Best effort; titles keep titleId as name on failure.
    void hydrate(std::vector<Title>& titles);

private:
    bool fetchCloudTitlesNetwork(const Tokens& tk, const Settings& st, std::vector<Title>& out, std::string& err,
                                 bool& authRejected);
    Config& cfg_;
};

}  // namespace xc
