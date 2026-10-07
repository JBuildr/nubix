// Nubix — xc-cli subcommands for the catalog + gssv session API (live testing).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "cli_gssv.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/auth.hpp"
#include "core/catalog.hpp"
#include "core/config.hpp"
#include "core/gssv.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

using namespace xc;

namespace {

std::atomic<bool> g_cancel{false};

void onCancel(int) { g_cancel = true; }

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int gssvUsage() {
    std::fprintf(stderr,
                 "usage: xc-cli titles [filter]\n"
                 "       xc-cli consoles\n"
                 "       xc-cli play <titleId> [--no-connect] [--timeout S]\n"
                 "       xc-cli play --home <serverId> [--timeout S]\n");
    return 2;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ensureAuth(Config& cfg, Auth& auth) {
    if (!auth.isLoggedIn()) {
        std::fprintf(stderr, "not logged in (run: xc-cli login)\n");
        return false;
    }
    std::string err;
    if (!auth.ensureFresh(err)) {
        std::fprintf(stderr, "auth: %s\n", err.c_str());
        return false;
    }
    cfg.save();
    return true;
}

int cmdTitles(Config& cfg, Auth& auth, const std::string& filter) {
    if (!ensureAuth(cfg, auth)) return 1;
    Catalog cat(cfg);
    std::vector<Title> titles;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!cat.fetchCloudTitles(titles, err)) {
        std::fprintf(stderr, "titles: %s\n", err.c_str());
        return 1;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::string f = lower(filter);
    size_t shown = 0;
    for (const auto& t : titles) {
        if (!f.empty() && lower(t.name).find(f) == std::string::npos && lower(t.titleId).find(f) == std::string::npos)
            continue;
        std::printf("%-28s %-14s %-3s %-4s %-3s %-4s %s%s%s\n", t.titleId.c_str(), t.productId.c_str(),
                    t.gamePass ? "GP" : "", t.f2p ? "F2P" : "", t.hasEntitlement ? "OWN" : "",
                    t.f2pOnly ? "ADS" : "", t.name.c_str(), t.publisher.empty() ? "" : "  / ",
                    t.publisher.c_str());
        if (!filter.empty() && !t.imageUrl.empty()) std::printf("    art: %s\n", t.imageUrl.c_str());
        ++shown;
    }
    std::printf("%zu/%zu playable titles (%.1f s)\n", shown, titles.size(), secs);
    return 0;
}

int cmdConsoles(Config& cfg, Auth& auth) {
    if (!ensureAuth(cfg, auth)) return 1;
    Catalog cat(cfg);
    std::vector<Console> consoles;
    std::string err;
    if (!cat.fetchConsoles(consoles, err)) {
        std::fprintf(stderr, "consoles: %s\n", err.c_str());
        return 1;
    }
    for (const auto& c : consoles)
        std::printf("%-20s %-16s %-18s %s\n", c.serverId.c_str(), c.consoleType.c_str(), c.powerState.c_str(),
                    c.name.c_str());
    std::printf("%zu consoles\n", consoles.size());
    return 0;
}

// Start a session, drive it to Provisioned (or ReadyToConnect with --no-connect), then DELETE it.
int cmdPlay(Config& cfg, Auth& auth, SessionKind kind, const std::string& id, bool doConnect, int timeoutSec) {
    if (!ensureAuth(cfg, auth)) return 1;
    const Tokens tk = cfg.tokens();
    // xhome: the console agent only accepts the android/720 fingerprint (see gssv::deviceTierFor).
    Settings gs = cfg.settings();
    gs.resolution = gssv::deviceTierFor(kind == SessionKind::Home, gs.resolution);

    struct Offering {
        const char* name;
        std::string base, token;
    };
    std::vector<Offering> offerings;
    if (kind == SessionKind::Home) {
        offerings.push_back({"xhome", tk.xhomeBase, tk.xhomeGs});
    } else {
        if (!tk.xcloudGs.empty()) offerings.push_back({"xgpuweb", tk.xcloudBase, tk.xcloudGs});
        if (!tk.xcloudF2pGs.empty() && (offerings.empty() || gs.f2pFallback))
            offerings.push_back({"xgpuwebf2p", tk.xcloudF2pBase, tk.xcloudF2pGs});
    }
    if (offerings.empty() || offerings.front().token.empty()) {
        std::fprintf(stderr, "no %s offering token for this account\n", kind == SessionKind::Home ? "xhome" : "xCloud");
        return 1;
    }

    // Our own handler so Ctrl-C still DELETEs the session.
    g_cancel = false;
    auto prevInt = std::signal(SIGINT, onCancel);
    auto prevTerm = std::signal(SIGTERM, onCancel);

    int rc = 1;
    std::string err;
    SessionInfo s;
    const Offering* used = nullptr;
    std::unique_ptr<GssvClient> client;
    for (const Offering& off : offerings) {
        client.reset(new GssvClient(off.base, off.token, gs));
        std::printf("offering %s (%s), osName %s\n", off.name, off.base.c_str(),
                    gssv::osNameForResolution(gs.resolution));
        if (!client->cleanupActive(kind)) std::printf("  (active-session cleanup reported a problem, continuing)\n");
        if (client->play(kind, id, s, err)) {
            used = &off;
            break;
        }
        std::fprintf(stderr, "play: %s\n", err.c_str());
        if (!s.errorCode.empty()) std::fprintf(stderr, "  code %s %s\n", s.errorCode.c_str(), s.errorMessage.c_str());
        if (s.errorCode != kErrOfferingDoesNotContainTitle) break;
    }
    if (!used) {
        std::signal(SIGINT, prevInt);
        std::signal(SIGTERM, prevTerm);
        return 1;
    }
    std::printf("session %s (%s) state %s\n", s.sessionId.c_str(), s.sessionPath.c_str(), s.state.c_str());

    bool connected = false;
    bool waitShown = false;
    std::string lastState;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    while (!g_cancel) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "timeout after %d s in state %s\n", timeoutSec, s.state.c_str());
            break;
        }
        if (!client->pollState(s, err)) {
            std::fprintf(stderr, "state: %s\n", err.c_str());
            break;
        }
        if (s.state != lastState) {
            std::printf("state: %s\n", s.state.c_str());
            lastState = s.state;
        }
        if (s.state == "WaitingForResources" && kind == SessionKind::Cloud && !waitShown) {
            waitShown = true;
            int w = client->waitTimeSeconds(id);
            if (w >= 0) std::printf("  estimated wait: %d s\n", w);
        } else if (s.state == "ReadyToConnect" && !connected) {
            if (!doConnect) {
                std::printf("ReadyToConnect reached (--no-connect)\n");
                rc = 0;
                break;
            }
            std::string lpt;
            if (!auth.fetchLpt(lpt, err)) {
                std::fprintf(stderr, "lpt: %s\n", err.c_str());
                break;
            }
            if (!client->connect(s, lpt, err)) {
                std::fprintf(stderr, "connect: %s\n", err.c_str());
                break;
            }
            std::printf("connect sent\n");
            connected = true;
        } else if (s.state == "Provisioned") {
            const int ka = client->keepAliveSeconds(s);
            std::printf("provisioned; keepalive interval %d s\n", ka);
            std::printf("keepalive: %s\n", client->keepAlive(s) ? "ok" : "failed");
            rc = 0;
            break;
        } else if (s.state == "Failed") {
            std::fprintf(stderr, "failed: %s %s\n", s.errorCode.c_str(), s.errorMessage.c_str());
            break;
        }
        sleepMs(1000);
    }
    if (g_cancel) std::fprintf(stderr, "cancelled\n");
    std::printf("stopping session: %s\n", client->stop(s) ? "ok" : "failed");

    std::signal(SIGINT, prevInt);
    std::signal(SIGTERM, prevTerm);
    return rc;
}

}  // namespace

bool cli_gssv_handles(const char* cmd) {
    return cmd && (!std::strcmp(cmd, "titles") || !std::strcmp(cmd, "consoles") || !std::strcmp(cmd, "play"));
}

int cli_gssv(int argc, char** argv) {
    if (argc < 1 || !cli_gssv_handles(argv[0])) return gssvUsage();
    const std::string cmd = argv[0];

    Config cfg(platform::dataDir() + "/config.json");
    if (!cfg.load()) XC_LOGW("config %s could not be parsed, using defaults", cfg.path().c_str());
    Auth auth(cfg);

    if (cmd == "titles") return cmdTitles(cfg, auth, argc > 1 ? argv[1] : "");
    if (cmd == "consoles") return cmdConsoles(cfg, auth);

    // play
    SessionKind kind = SessionKind::Cloud;
    bool doConnect = true;
    int timeoutSec = 300;
    std::string id;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--home") kind = SessionKind::Home;
        else if (a == "--no-connect") doConnect = false;
        else if (a == "--timeout" && i + 1 < argc) timeoutSec = std::max(10, std::atoi(argv[++i]));
        else if (!a.empty() && a[0] != '-' && id.empty()) id = a;
        else return gssvUsage();
    }
    if (id.empty()) return gssvUsage();
    return cmdPlay(cfg, auth, kind, id, doConnect, timeoutSec);
}
