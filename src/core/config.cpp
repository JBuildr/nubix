// Nubix — persisted settings and tokens (JSON file).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/config.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/log.hpp"

#if !defined(__APPLE__) && !defined(__FreeBSD__)
#include <openssl/rand.h>
#endif

using nlohmann::json;

namespace xc {
namespace {

constexpr int kConfigVersion = 1;

// Serialises the file write of save() across threads (the UI thread, the App worker and the
// streamer may all persist). Serialising happens inside it, so the last writer always writes
// the newest state.
std::mutex g_saveMutex;

// Type-checked readers: a hand-edited or older config with a wrong type must never throw.
void readStr(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) out = it->get<std::string>();
}

void readInt(const json& j, const char* key, int& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) out = it->get<int>();
}

void readI64(const json& j, const char* key, int64_t& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_number_integer()) out = it->get<int64_t>();
}

void readBool(const json& j, const char* key, bool& out) {
    auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) out = it->get<bool>();
}

json regionsToJson(const std::vector<Region>& regions) {
    json arr = json::array();
    for (const auto& r : regions) arr.push_back({{"name", r.name}, {"baseUri", r.baseUri}, {"isDefault", r.isDefault}});
    return arr;
}

void readRegions(const json& j, const char* key, std::vector<Region>& out) {
    out.clear();
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    for (const auto& e : *it) {
        if (!e.is_object()) continue;
        Region r;
        readStr(e, "name", r.name);
        readStr(e, "baseUri", r.baseUri);
        readBool(e, "isDefault", r.isDefault);
        if (!r.baseUri.empty()) out.push_back(std::move(r));
    }
}

void fillRandom(uint8_t* buf, size_t n) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    // macOS and the PS5 (FreeBSD-based libc) both provide arc4random_buf.
    arc4random_buf(buf, n);
#else
    if (RAND_bytes(buf, static_cast<int>(n)) == 1) return;
    std::random_device rd;
    std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ rd() ^
                        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(gen());
#endif
}

bool clampSettings(Settings& s) {
    bool changed = false;
    if (s.resolution != "720" && s.resolution != "1080" && s.resolution != "1080HQ") {
        s.resolution = "1080";
        changed = true;
    }
    if (s.bitrateKbps < 5000) { s.bitrateKbps = 5000; changed = true; }
    if (s.bitrateKbps > 20000) { s.bitrateKbps = 20000; changed = true; }
    if (s.stickDeadzone < 0) { s.stickDeadzone = 0; changed = true; }
    if (s.stickDeadzone > 50) { s.stickDeadzone = 50; changed = true; }
    if (s.locale.empty()) { s.locale = "en-US"; changed = true; }
    return changed;
}

}  // namespace

Config::Config(std::string path) : path_(std::move(path)) {}

bool Config::load() {
    Settings settings;
    Tokens tokens;
    std::string installId;

    bool ok = true;
    std::string text;
    if (!path_.empty()) {
        std::ifstream in(path_, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
    }

    if (!text.empty()) {
        json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (root.is_discarded() || !root.is_object()) {
            XC_LOGE("config: %s is not valid JSON; using defaults (kept as .bad)", path_.c_str());
            const std::string bad = path_ + ".bad";
            std::rename(path_.c_str(), bad.c_str());
            ok = false;
        } else {
            readStr(root, "installId", installId);
            auto sIt = root.find("settings");
            if (sIt != root.end() && sIt->is_object()) {
                const json& s = *sIt;
                readStr(s, "resolution", settings.resolution);
                readInt(s, "bitrateKbps", settings.bitrateKbps);
                readStr(s, "region", settings.region);
                readStr(s, "locale", settings.locale);
                readBool(s, "f2pFallback", settings.f2pFallback);
                readInt(s, "stickDeadzone", settings.stickDeadzone);
                if (clampSettings(settings)) XC_LOGW("config: out-of-range settings were reset");
            }
            auto tIt = root.find("tokens");
            if (tIt != root.end() && tIt->is_object()) {
                const json& t = *tIt;
                readStr(t, "msaRefresh", tokens.msaRefresh);
                readStr(t, "msaAccess", tokens.msaAccess);
                readI64(t, "msaExpiry", tokens.msaExpiry);
                readStr(t, "xhomeGs", tokens.xhomeGs);
                readStr(t, "xcloudGs", tokens.xcloudGs);
                readStr(t, "xcloudF2pGs", tokens.xcloudF2pGs);
                readStr(t, "xhomeBase", tokens.xhomeBase);
                readStr(t, "xcloudBase", tokens.xcloudBase);
                readStr(t, "xcloudF2pBase", tokens.xcloudF2pBase);
                readI64(t, "gsExpiry", tokens.gsExpiry);
                readStr(t, "gamertag", tokens.gamertag);
                readStr(t, "uhs", tokens.uhs);
                readStr(t, "xblToken", tokens.xblToken);
                readI64(t, "xblExpiry", tokens.xblExpiry);
                readRegions(t, "xhomeRegions", tokens.xhomeRegions);
                readRegions(t, "xcloudRegions", tokens.xcloudRegions);
                readRegions(t, "xcloudF2pRegions", tokens.xcloudF2pRegions);
                readStr(t, "market", tokens.market);
            }
        }
    }

    if (installId.size() != 36) installId = generateUuid();
    {
        std::lock_guard<std::mutex> lk(mu_);
        settings_ = std::move(settings);
        tokens_ = std::move(tokens);
        installId_ = std::move(installId);
    }
    ++revision_;
    return ok;
}

Settings Config::settings() const {
    std::lock_guard<std::mutex> lk(mu_);
    return settings_;
}

Tokens Config::tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tokens_;
}

std::string Config::gamertag() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tokens_.gamertag;
}

std::string Config::installId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return installId_;
}

void Config::setSettings(const Settings& s) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        settings_ = s;
    }
    ++revision_;
}

void Config::updateSettings(const std::function<void(Settings&)>& fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn(settings_);
    }
    ++revision_;
}

void Config::setTokens(const Tokens& t) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tokens_ = t;
    }
    ++revision_;
}

void Config::updateTokens(const std::function<void(Tokens&)>& fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn(tokens_);
    }
    ++revision_;
}



bool Config::save() {
    if (path_.empty()) {
        XC_LOGE("config: save() without a path");
        return false;
    }

    std::lock_guard<std::mutex> saveLock(g_saveMutex);
    Settings settings;
    Tokens tokens;
    std::string installId;
    {
        std::lock_guard<std::mutex> lk(mu_);
        settings = settings_;
        tokens = tokens_;
        installId = installId_;
    }
    json root;
    root["version"] = kConfigVersion;
    root["installId"] = installId;
    root["settings"] = {
        {"resolution", settings.resolution}, {"bitrateKbps", settings.bitrateKbps},
        {"region", settings.region},         {"locale", settings.locale},
        {"f2pFallback", settings.f2pFallback}, {"stickDeadzone", settings.stickDeadzone},
    };
    root["tokens"] = {
        {"msaRefresh", tokens.msaRefresh},
        {"msaAccess", tokens.msaAccess},
        {"msaExpiry", tokens.msaExpiry},
        {"xhomeGs", tokens.xhomeGs},
        {"xcloudGs", tokens.xcloudGs},
        {"xcloudF2pGs", tokens.xcloudF2pGs},
        {"xhomeBase", tokens.xhomeBase},
        {"xcloudBase", tokens.xcloudBase},
        {"xcloudF2pBase", tokens.xcloudF2pBase},
        {"gsExpiry", tokens.gsExpiry},
        {"gamertag", tokens.gamertag},
        {"uhs", tokens.uhs},
        {"xblToken", tokens.xblToken},
        {"xblExpiry", tokens.xblExpiry},
        {"xhomeRegions", regionsToJson(tokens.xhomeRegions)},
        {"xcloudRegions", regionsToJson(tokens.xcloudRegions)},
        {"xcloudF2pRegions", regionsToJson(tokens.xcloudF2pRegions)},
        {"market", tokens.market},
    };
    // Replace invalid UTF-8 instead of throwing (values come from the network).
    const std::string text = root.dump(2, ' ', false, json::error_handler_t::replace) + "\n";

    const std::string tmp = path_ + ".tmp";
    // 0600: the file holds the MSA refresh token.
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        XC_LOGE("config: cannot create %s: %s", tmp.c_str(), std::strerror(errno));
        return false;
    }
    size_t off = 0;
    while (off < text.size()) {
        ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            XC_LOGE("config: write %s failed: %s", tmp.c_str(), std::strerror(errno));
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    ::fsync(fd);
    if (::close(fd) != 0) {
        XC_LOGE("config: close %s failed: %s", tmp.c_str(), std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        XC_LOGE("config: rename %s -> %s failed: %s", tmp.c_str(), path_.c_str(), std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string pickRegionBase(const std::vector<Region>& regions, const std::string& preferred) {
    if (regions.empty()) return std::string();
    if (!preferred.empty())
        for (const auto& r : regions)
            if (r.name == preferred) return r.baseUri;
    for (const auto& r : regions)
        if (r.isDefault) return r.baseUri;
    return regions.front().baseUri;
}

std::string generateUuid() {
    uint8_t b[16];
    fillRandom(b, sizeof(b));
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex[b[i] >> 4];
        out += hex[b[i] & 0x0F];
    }
    return out;
}

}  // namespace xc
