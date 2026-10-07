// Nubix — smoke tests for vendored / scaffold pieces (always expected to pass).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <nlohmann/json.hpp>

#include "core/log.hpp"
#include "platform/platform.hpp"
#include "test.hpp"
#include "ui/qr.hpp"

XC_TEST(smoke, qr_encodes_microsoft_link) {
    std::vector<bool> modules;
    int size = 0;
    XC_REQUIRE(xc::qr::encode("https://www.microsoft.com/link?otc=ABCD1234", modules, size));
    XC_CHECK(size >= 21);
    XC_CHECK_EQ(modules.size(), static_cast<size_t>(size) * static_cast<size_t>(size));
    // finder pattern: top-left 7x7 has dark corners
    XC_CHECK(modules[0]);
    XC_CHECK(modules[6]);
    XC_CHECK(modules[static_cast<size_t>(6) * size]);
}

XC_TEST(smoke, qr_rejects_oversized_input) {
    std::vector<bool> modules;
    int size = 0;
    XC_CHECK(!xc::qr::encode(std::string(8000, 'x'), modules, size));
    XC_CHECK_EQ(size, 0);
}

XC_TEST(smoke, json_roundtrip) {
    auto j = nlohmann::json::parse(R"({"a":1,"b":"x"})");
    XC_CHECK_EQ(j["a"].get<int>(), 1);
    XC_CHECK_EQ(j["b"].get<std::string>(), std::string("x"));
}

XC_TEST(smoke, log_does_not_crash) {
    xc::logInit("", xc::LogLevel::Debug);
    XC_LOGD("debug %d", 1);
    XC_LOGE("error %s", "x");
    xc::logSetLevel(xc::LogLevel::Info);
    XC_CHECK(xc::logLevel() == xc::LogLevel::Info);
}

XC_TEST(smoke, platform_clock) {
    XC_CHECK(xc::platform::unixTime() > 1700000000);
    const uint64_t a = xc::platform::monotonicMs();
    const uint64_t b = xc::platform::monotonicMs();
    XC_CHECK(b >= a);
}
