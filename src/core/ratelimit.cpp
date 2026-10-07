// Nubix — client-side request pacing (see ratelimit.hpp).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/ratelimit.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace xc {

namespace {

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Days since 1970-01-01 for a proleptic Gregorian date (timegm is not portable to the PS5 libc).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// "Sun, 06 Nov 1994 08:49:37 GMT" (IMF-fixdate, the only form servers send today).
bool parseHttpDate(const std::string& v, int64_t& unixSec) {
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const size_t comma = v.find(',');
    if (comma == std::string::npos) return false;
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0};
    if (std::sscanf(v.c_str() + comma + 1, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return false;
    int month = -1;
    for (int i = 0; i < 12; ++i)
        if (std::strcmp(mon, kMonths[i]) == 0) month = i + 1;
    if (month < 0 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return false;
    unixSec = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hh * 3600 +
              mm * 60 + ss;
    return true;
}

}  // namespace

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string urlHost(const std::string& url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t at = host.rfind('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    if (!host.empty() && host[0] != '[') {
        const size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
    }
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

int parseRetryAfter(const std::string& value, int64_t nowUnixSec) {
    size_t b = 0, e = value.size();
    while (b < e && std::isspace(static_cast<unsigned char>(value[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(value[e - 1]))) --e;
    if (b == e) return -1;
    const std::string v = value.substr(b, e - b);
    if (std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); })) {
        if (v.size() > 6) return 3600;
        return std::min(3600, std::atoi(v.c_str()));
    }
    int64_t at = 0;
    if (!parseHttpDate(v, at)) return -1;
    return static_cast<int>(std::clamp<int64_t>(at - nowUnixSec, 0, 3600));
}

// ---------------------------------------------------------------------------------------------
// RateLimiter

RateLimiter::Policy RateLimiter::policyFor(const std::string& host) {
    // Box art / store images and the public, unauthenticated store catalog (title names, batched
    // 20 per request): many small requests on a cold library load, CDN-backed. Cached on disk.
    if (endsWith(host, "store-images.s-microsoft.com") || endsWith(host, ".akamaized.net") ||
        host.find("images") != std::string::npos || host == "displaycatalog.mp.microsoft.com")
        return {10.0, 30.0};
    // Microsoft APIs (gssv, auth, catalog). Session signalling peaks at ~3 req/s (state + sdp/ice
    // polls); the catalog hydration bursts ~10 requests once.
    return {5.0, 12.0};
}

int64_t RateLimiter::reserve(const std::string& host, int64_t nowMs, bool* inCooldown) {
    if (inCooldown) *inCooldown = false;
    std::lock_guard<std::mutex> lk(mu_);
    HostState& h = hosts_[host];
    if (nowMs < h.cooldownUntilMs) {
        if (inCooldown) *inCooldown = true;
        return h.cooldownUntilMs - nowMs;
    }
    const Policy p = policyFor(host);
    if (h.tokens < 0) {
        h.tokens = p.burst;
        h.lastRefillMs = nowMs;
    } else if (nowMs > h.lastRefillMs) {
        h.tokens = std::min(p.burst, h.tokens + (nowMs - h.lastRefillMs) * p.ratePerSec / 1000.0);
        h.lastRefillMs = nowMs;
    }
    if (h.tokens >= 1.0) {
        h.tokens -= 1.0;
        return 0;
    }
    const double missing = 1.0 - h.tokens;
    return std::max<int64_t>(1, static_cast<int64_t>(missing * 1000.0 / p.ratePerSec + 0.999));
}

void RateLimiter::onResponse(const std::string& host, long status, int retryAfterSec, int64_t nowMs) {
    if (status <= 0) return;  // transport error: says nothing about server-side limits
    std::lock_guard<std::mutex> lk(mu_);
    HostState& h = hosts_[host];
    const bool limited = status == 429 || (status == 503 && retryAfterSec >= 0);
    if (!limited) {
        h.consecutive429 = 0;
        return;
    }
    ++h.consecutive429;
    int64_t backoff;
    if (retryAfterSec >= 0) {
        backoff = static_cast<int64_t>(retryAfterSec) * 1000;
    } else {
        const int shift = std::min(h.consecutive429 - 1, 6);
        backoff = std::min<int64_t>(kMaxBackoffMs, int64_t(2000) << shift);
    }
    h.cooldownUntilMs = std::max(h.cooldownUntilMs, nowMs + backoff);
    h.tokens = 0;  // restart slowly after the cooldown
    h.lastRefillMs = h.cooldownUntilMs;
}

int64_t RateLimiter::cooldownRemaining(const std::string& host, int64_t nowMs) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = hosts_.find(host);
    if (it == hosts_.end() || nowMs >= it->second.cooldownUntilMs) return 0;
    return it->second.cooldownUntilMs - nowMs;
}

void RateLimiter::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    hosts_.clear();
}

RateLimiter& RateLimiter::global() {
    static RateLimiter instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------
// SessionStartLimiter

bool SessionStartLimiter::allow(bool home, int64_t nowMs, int& waitSec) {
    std::lock_guard<std::mutex> lk(mu_);
    std::deque<int64_t>& q = home ? home_ : cloud_;
    const int maxStarts = home ? kMaxHome : kMaxCloud;
    while (!q.empty() && nowMs - q.front() >= kWindowMs) q.pop_front();
    int64_t waitMs = 0;
    if (static_cast<int>(q.size()) >= maxStarts) waitMs = q.front() + kWindowMs - nowMs;
    if (last_ >= 0 && nowMs - last_ < kMinSpacingMs) waitMs = std::max(waitMs, last_ + kMinSpacingMs - nowMs);
    if (waitMs > 0) {
        waitSec = static_cast<int>(std::max<int64_t>(1, (waitMs + 999) / 1000));
        return false;
    }
    q.push_back(nowMs);
    last_ = nowMs;
    waitSec = 0;
    return true;
}

void SessionStartLimiter::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    cloud_.clear();
    home_.clear();
    last_ = -1;
}

SessionStartLimiter& SessionStartLimiter::global() {
    static SessionStartLimiter instance;
    return instance;
}

}  // namespace xc
