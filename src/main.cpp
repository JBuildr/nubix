// Nubix — native Xbox Cloud Gaming / Remote Play client for PS5 (and host builds).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.hpp"
#include "platform/platform.hpp"
#include "ui/app.hpp"

#ifndef XC_VERSION
#define XC_VERSION "dev"
#endif

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0 || std::strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("nubix %s\n", XC_VERSION);
            return 0;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("usage: %s [--verbose|-v] [--version] [--selftest]\n"
                        "  --verbose  debug logging (log.txt in the data directory)\n"
                        "  --selftest init all subsystems, render a few Login frames, exit 0 on success\n"
                        "             (headless: SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy)\n"
                        "Host environment: XC_DATA_DIR, XC_ASSETS_DIR, XC_FULLSCREEN=1\n"
                        "Keys: arrows/Enter/Esc navigate, Q/E switch tabs, Y filter/refresh, F1 stream menu,\n"
                        "      F12 screenshot (saved to the data directory)\n",
                        argc > 0 ? argv[0] : "nubix");
            return 0;
        }
    }

    if (!xc::platform::init(argc, argv)) {
        std::fprintf(stderr, "platform init failed\n");
        return 1;
    }
    xc::logInit(xc::platform::dataDir() + "/log.txt", verbose ? xc::LogLevel::Debug : xc::LogLevel::Info);
    XC_LOGI("nubix %s starting (%s), data=%s assets=%s", XC_VERSION, xc::platform::isPs5() ? "PS5" : "host",
            xc::platform::dataDir().c_str(), xc::platform::assetsDir().c_str());

    int rc = 0;
    {
        xc::App app;
        rc = app.run(argc, argv);
    }

    XC_LOGI("exit code %d", rc);
    xc::logShutdown();
    xc::platform::shutdown();
    return rc;
}
