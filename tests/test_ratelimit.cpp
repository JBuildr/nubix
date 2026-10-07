// Nubix — request pacing tests (token bucket, 429 cooldown/backoff, session start cap).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <string>

#include "core/ratelimit.hpp"
#include "test.hpp"

using namespace xc;

XC_TEST(ratelimit, url_host) {
    XC_CHECK_EQ(urlHost("https://WEU.core.gssv-play-prod.xboxlive.com/v5/sessions?x=1"),
                std::string("weu.core.gssv-play-prod.xboxlive.com"));
    XC_CHECK_EQ(urlHost("https://login.live.com:443/oauth20_token.srf"), std::string("login.live.com"));
    XC_CHECK_EQ(urlHost("http://u:p@host.example#frag"), std::string("host.example"));
}

XC_TEST(ratelimit, retry_after_parsing) {
    XC_CHECK_EQ(parseRetryAfter("", 0), -1);
    XC_CHECK_EQ(parseRetryAfter(" 7 ", 0), 7);
    XC_CHECK_EQ(parseRetryAfter("999999999", 0), 3600);
    XC_CHECK_EQ(parseRetryAfter("soon", 0), -1);
    // Sun, 06 Nov 1994 08:49:37 GMT = 784111777
    XC_CHECK_EQ(parseRetryAfter("Sun, 06 Nov 1994 08:49:37 GMT", 784111777 - 30), 30);
    XC_CHECK_EQ(parseRetryAfter("Sun, 06 Nov 1994 08:49:37 GMT", 784111777 + 30), 0);
}

XC_TEST(ratelimit, bucket_allows_burst_then_paces) {
    RateLimiter rl;
    const std::string h = "api.example";
    const auto p = RateLimiter::policyFor(h);
    int granted = 0;
    for (int i = 0; i < 100; ++i)
        if (rl.reserve(h, 1000) == 0) ++granted;
    XC_CHECK_EQ(granted, static_cast<int>(p.burst));
    const int64_t wait = rl.reserve(h, 1000);
    XC_CHECK(wait > 0 && wait <= static_cast<int64_t>(1000 / p.ratePerSec) + 1);
    XC_CHECK_EQ(rl.reserve(h, 1000 + wait), int64_t(0));
    // Other hosts are independent.
    XC_CHECK_EQ(rl.reserve("other.example", 1000), int64_t(0));
}

XC_TEST(ratelimit, cooldown_honours_retry_after) {
    RateLimiter rl;
    const std::string h = "api.example";
    rl.onResponse(h, 429, 10, 5000);
    bool cooldown = false;
    XC_CHECK_EQ(rl.reserve(h, 5000, &cooldown), int64_t(10000));
    XC_CHECK(cooldown);
    XC_CHECK_EQ(rl.cooldownRemaining(h, 14000), int64_t(1000));
    XC_CHECK(rl.reserve(h, 15000, &cooldown) > 0);  // bucket restarts empty after a 429
    XC_CHECK(!cooldown);
}

XC_TEST(ratelimit, exponential_backoff_without_header) {
    RateLimiter rl;
    const std::string h = "api.example";
    rl.onResponse(h, 429, -1, 0);
    XC_CHECK_EQ(rl.cooldownRemaining(h, 0), int64_t(2000));
    rl.onResponse(h, 429, -1, 2000);
    XC_CHECK_EQ(rl.cooldownRemaining(h, 2000), int64_t(4000));
    rl.onResponse(h, 429, -1, 6000);
    XC_CHECK_EQ(rl.cooldownRemaining(h, 6000), int64_t(8000));
    for (int i = 0; i < 20; ++i) rl.onResponse(h, 429, -1, 100000);
    XC_CHECK_EQ(rl.cooldownRemaining(h, 100000), RateLimiter::kMaxBackoffMs);
    // A normal answer resets the backoff ladder.
    rl.onResponse(h, 200, -1, 300000);
    rl.onResponse(h, 429, -1, 300000);
    XC_CHECK_EQ(rl.cooldownRemaining(h, 300000), int64_t(2000));
}

XC_TEST(ratelimit, plain_503_and_transport_errors_do_not_cool_down) {
    RateLimiter rl;
    rl.onResponse("a", 503, -1, 0);
    rl.onResponse("a", 0, -1, 0);
    XC_CHECK_EQ(rl.cooldownRemaining("a", 0), int64_t(0));
    rl.onResponse("a", 503, 5, 0);
    XC_CHECK_EQ(rl.cooldownRemaining("a", 0), int64_t(5000));
}

XC_TEST(ratelimit, session_start_cap) {
    SessionStartLimiter sl;
    int wait = 0;
    int64_t t = 0;
    for (int i = 0; i < SessionStartLimiter::kMaxCloud; ++i) {
        XC_CHECK(sl.allow(false, t, wait));
        t += 3000;
    }
    XC_CHECK(!sl.allow(false, t, wait));
    XC_CHECK(wait > 0 && wait <= 300);
    // Spacing applies across kinds.
    XC_CHECK(!sl.allow(true, t - 3000 + 500, wait));
    XC_CHECK(sl.allow(true, t, wait));
    // After the window the oldest start expires.
    XC_CHECK(sl.allow(false, SessionStartLimiter::kWindowMs + 1, wait));
}
