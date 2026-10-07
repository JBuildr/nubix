// Nubix — SDP offer template tests.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <nlohmann/json.hpp>

#include "core/protocol.hpp"
#include "test.hpp"

using namespace xc;

namespace {
const char* kFp = "AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:01:23:45:67:89";
}

XC_TEST(sdp, offer_contains_credentials_and_bundle) {
    for (bool home : {false, true}) {
        const std::string o = proto::buildOffer("uFrAg", "pAsSwOrDpAsSwOrDpAsSwOrD", kFp, home, "1080");
        XC_REQUIRE(!o.empty());
        XC_CHECK(o.rfind("v=0\r\n", 0) == 0);
        XC_CHECK(o.find("a=group:BUNDLE video audio 0") != std::string::npos);
        XC_CHECK(o.find("a=ice-ufrag:uFrAg\r\n") != std::string::npos);
        XC_CHECK(o.find("a=ice-pwd:pAsSwOrDpAsSwOrDpAsSwOrD\r\n") != std::string::npos);
        XC_CHECK(o.find(std::string("a=fingerprint:sha-256 ") + kFp) != std::string::npos);
        const size_t v = o.find("m=video"), a = o.find("m=audio"), d = o.find("m=application");
        XC_REQUIRE(v != std::string::npos && a != std::string::npos && d != std::string::npos);
        XC_CHECK(v < a && a < d);
        XC_CHECK(o.find("a=mid:video") != std::string::npos);
        XC_CHECK(o.find("a=mid:audio") != std::string::npos);
        XC_CHECK(o.find("a=mid:0") != std::string::npos);
        XC_CHECK(o.find("H264") != std::string::npos);
        XC_CHECK(o.find("opus/48000/2") != std::string::npos);
        XC_CHECK(o.find("a=recvonly") != std::string::npos);
        // every line CRLF terminated
        XC_CHECK(o.size() >= 2 && o.substr(o.size() - 2) == "\r\n");
        XC_CHECK(o.find("\n") == o.find("\r\n") + 1);
    }
}

XC_TEST(sdp, post_body) {
    const std::string o = proto::buildOffer("u", "p", kFp, false, "720");
    auto j = nlohmann::json::parse(proto::sdpPostBody(o), nullptr, false);
    XC_REQUIRE(!j.is_discarded());
    XC_CHECK_EQ(j.value("messageType", std::string()), std::string("offer"));
    XC_CHECK_EQ(j.value("sdp", std::string()), o);
    XC_CHECK(j.contains("configuration"));
}
