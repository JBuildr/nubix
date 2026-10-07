// Nubix — config persistence tests.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/gssv.hpp"
#include "test.hpp"

using namespace xc;

namespace {
std::string tempPath() {
    char tmpl[] = "/tmp/xc-config-test-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    std::remove(tmpl);
    return tmpl;
}
}  // namespace

XC_TEST(config, missing_file_gives_defaults_and_install_id) {
    Config c(tempPath());
    XC_CHECK(c.load());
    XC_CHECK_EQ(c.settings().resolution, std::string("1080"));
    XC_CHECK_EQ(c.installId().size(), size_t(36));
}

XC_TEST(config, roundtrip) {
    const std::string path = tempPath();
    {
        Config c(path);
        c.load();
        c.updateSettings([](Settings& s) {
            s.resolution = "720";
            s.bitrateKbps = 8000;
            s.f2pFallback = false;
        });
        c.updateTokens([](Tokens& t) {
            t.msaRefresh = "refresh-token";
            t.gsExpiry = 1234567890123LL;
            t.gamertag = "Tester";
        });
        XC_REQUIRE(c.save());
    }
    Config d(path);
    XC_REQUIRE(d.load());
    XC_CHECK_EQ(d.settings().resolution, std::string("720"));
    XC_CHECK_EQ(d.settings().bitrateKbps, 8000);
    XC_CHECK(!d.settings().f2pFallback);
    XC_CHECK_EQ(d.tokens().msaRefresh, std::string("refresh-token"));
    XC_CHECK_EQ(d.tokens().gsExpiry, int64_t(1234567890123LL));
    XC_CHECK_EQ(d.tokens().gamertag, std::string("Tester"));
    std::remove(path.c_str());
}

XC_TEST(config, uuid_format) {
    const std::string a = generateUuid(), b = generateUuid();
    XC_REQUIRE(a.size() == 36u);
    XC_CHECK(a != b);
    XC_CHECK_EQ(a[8], '-');
    XC_CHECK_EQ(a[14], '4');
}

// Readers take snapshots while another thread replaces the region lists and saves: no torn
// state, every snapshot is one of the published versions.
XC_TEST(config, concurrent_snapshots_and_updates) {
    const std::string path = tempPath();
    Config c(path);
    XC_REQUIRE(c.load());
    const uint64_t rev0 = c.revision();
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread writer([&] {
        for (int i = 0; i < 300; ++i) {
            c.updateTokens([i](Tokens& t) {
                t.xcloudRegions.assign(static_cast<size_t>(1 + i % 7), Region{"R" + std::to_string(i), "https://x", false});
                t.gamertag = "G" + std::to_string(i);
            });
            c.updateSettings([i](Settings& s) { s.region = "R" + std::to_string(i); });
            if (i % 50 == 0) c.save();
        }
        stop = true;
    });
    while (!stop) {
        const Tokens t = c.tokens();
        for (const auto& r : t.xcloudRegions)
            if (r.name != t.xcloudRegions.front().name) ++bad;
        if (!t.xcloudRegions.empty() && "G" + t.xcloudRegions.front().name.substr(1) != t.gamertag) ++bad;
        (void)c.settings().region.size();
    }
    writer.join();
    XC_CHECK_EQ(bad.load(), 0);
    XC_CHECK(c.revision() >= rev0 + 600);
    XC_REQUIRE(c.save());
    Config d(path);
    XC_REQUIRE(d.load());
    XC_CHECK_EQ(d.tokens().gamertag, std::string("G299"));
    XC_CHECK_EQ(d.settings().region, std::string("R299"));
    std::remove(path.c_str());
}

XC_TEST(config, gssv_device_tier_and_auth_rejection) {
    // xhome: always the android (720) fingerprint, cloud: the user's tier
    XC_CHECK_EQ(gssv::deviceTierFor(true, "1080"), std::string("720"));
    XC_CHECK_EQ(gssv::deviceTierFor(true, "1080HQ"), std::string("720"));
    XC_CHECK_EQ(gssv::deviceTierFor(false, "1080HQ"), std::string("1080HQ"));
    XC_CHECK_EQ(std::string(gssv::osNameForResolution(gssv::deviceTierFor(true, "1080"))), std::string("android"));
    XC_CHECK(gssv::deviceInfoJson(gssv::deviceTierFor(true, "1080")).find("\"android\"") != std::string::npos);
    XC_CHECK(gssv::isAuthRejection(401, ""));
    XC_CHECK(gssv::isAuthRejection(403, ""));
    XC_CHECK(gssv::isAuthRejection(403, "<html>Forbidden</html>"));
    XC_CHECK(!gssv::isAuthRejection(403, "{\"code\":\"OfferingAccessDenied\",\"message\":\"no\"}"));
    XC_CHECK(!gssv::isAuthRejection(400, ""));
    XC_CHECK(!gssv::isAuthRejection(200, ""));
}
