// Nubix — client-side request pacing: per-host token buckets, 429/Retry-After cooldowns with
// exponential backoff, and a sliding-window cap on session starts. Keeps the client's traffic
// pattern at or below what the official browser client produces.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace xc {

class RateLimiter {
public:
    struct Policy {
        double ratePerSec;  // sustained requests per second
        double burst;       // bucket size
    };

    // Policy for a host: image CDNs (box art) get a larger budget than API hosts.
    static Policy policyFor(const std::string& host);

    // Try to start a request to `host` at time nowMs. Returns 0 when the request may go now (a
    // token is consumed), otherwise the milliseconds to wait before asking again. When the host
    // is in a 429 cooldown, *inCooldown is set and the remaining cooldown is returned.
    int64_t reserve(const std::string& host, int64_t nowMs, bool* inCooldown = nullptr);

    // Feed a response back. HTTP 429 (and 503 carrying Retry-After) puts the host into cooldown
    // for Retry-After seconds, or an exponential backoff (2, 4, 8 … 120 s) when the header is
    // missing. Any other completed response resets the backoff. retryAfterSec < 0 = no header.
    void onResponse(const std::string& host, long status, int retryAfterSec, int64_t nowMs);

    // Remaining cooldown for host (0 = none).
    int64_t cooldownRemaining(const std::string& host, int64_t nowMs);

    void reset();

    // Process-wide instance used by Http::request.
    static RateLimiter& global();

    static constexpr int64_t kMaxBackoffMs = 120000;

private:
    struct HostState {
        double tokens = -1;  // < 0 = not initialised (full bucket)
        int64_t lastRefillMs = 0;
        int64_t cooldownUntilMs = 0;
        int consecutive429 = 0;
    };
    std::mutex mu_;
    std::map<std::string, HostState> hosts_;
};

// Caps how often game sessions are requested (POST …/play), independent of HTTP 429s:
// cloud at most 4 starts per 5 minutes, home (console wake-up retries) at most 8, and at least
// 2 s between any two starts.
class SessionStartLimiter {
public:
    static constexpr int64_t kWindowMs = 5 * 60 * 1000;
    static constexpr int kMaxCloud = 4;
    static constexpr int kMaxHome = 8;
    static constexpr int64_t kMinSpacingMs = 2000;

    // Records the start and returns true when allowed; otherwise sets waitSec (>= 1).
    bool allow(bool home, int64_t nowMs, int& waitSec);
    void reset();

    static SessionStartLimiter& global();

private:
    std::mutex mu_;
    std::deque<int64_t> cloud_, home_;
    int64_t last_ = -1;
};

// Parse a Retry-After header value: delta-seconds or an HTTP-date. Returns -1 when absent or
// unparseable; clamps to [0, 3600].
int parseRetryAfter(const std::string& value, int64_t nowUnixSec);

// Lower-cased host of an http(s) URL ("" when none).
std::string urlHost(const std::string& url);

// Monotonic milliseconds (steady clock).
int64_t steadyNowMs();

}  // namespace xc
