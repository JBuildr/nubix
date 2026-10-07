// Nubix — host CLI for live API testing without the UI.
//
//   xc-cli login                 device-code login (prints code + URL), then authorize streaming
//   xc-cli devicecode            only request + print a device code (live endpoint smoke test)
//   xc-cli token                 refresh + authorize streaming, print offerings and regions
//   xc-cli refresh               refresh MSA token + gs tokens
//   xc-cli status                show stored account/token state
//   xc-cli titles [filter]       list playable cloud titles (tools/cli_gssv.cpp)
//   xc-cli consoles              list xHome consoles (tools/cli_gssv.cpp)
//   xc-cli play <titleId>|--home <serverId> [--no-connect] [--timeout S]
//                                gssv session start/poll/connect/keepalive/stop (tools/cli_gssv.cpp)
//   xc-cli session cloud|home ID gssv only: start, poll until Provisioned (or fail), stop
//   xc-cli stream cloud|home ID [seconds]  full headless stream via Streamer (no video output)
//   xc-cli offer                 print a generated SDP offer (local WebRTC gathering only)
//   xc-cli logout
//
// Options: --verbose. Data dir: $XC_DATA_DIR or the platform default (shared with the app).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <SDL.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

#include "core/auth.hpp"
#include "core/config.hpp"
#include "core/gssv.hpp"
#include "core/http.hpp"
#include "core/log.hpp"
#include "cli_gssv.hpp"
#include "core/protocol.hpp"
#include "platform/platform.hpp"
#include "stream/streamer.hpp"
#include "stream/webrtc.hpp"

using namespace xc;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int usage() {
    std::fprintf(stderr,
                 "usage: xc-cli [--verbose] <command>\n"
                 "  login | devicecode | token | refresh | status | logout\n"
                 "  titles [filter] | consoles\n"
                 "  play <titleId> | play --home <serverId>  [--no-connect] [--timeout S]\n"
                 "  session <cloud|home> <titleId|serverId>\n"
                 "  stream <cloud|home> <titleId|serverId> [seconds]\n"
                 "  offer\n");
    return 2;
}

bool parseKind(const std::string& s, SessionKind& k) {
    if (s == "cloud") { k = SessionKind::Cloud; return true; }
    if (s == "home") { k = SessionKind::Home; return true; }
    return false;
}

int cmdLogin(Config& cfg, Auth& auth) {
    DeviceCode dc;
    std::string err;
    if (!auth.requestDeviceCode(dc, err)) {
        std::fprintf(stderr, "device code request failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("Open %s and enter code: %s\n(QR url: %s)\n", dc.verificationUri.c_str(), dc.userCode.c_str(),
                dc.qrUrl.c_str());
    std::fflush(stdout);
    int interval = dc.interval;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(dc.expiresIn);
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < interval * 10 && !g_stop; ++i) sleepMs(100);
        if (g_stop) break;
        PollResult r = auth.pollToken(dc, err);
        if (r == PollResult::Pending) {
            if (err == "slow_down") interval += 5;  // RFC 8628
            continue;
        }
        if (r != PollResult::Success) {
            std::fprintf(stderr, "login failed: %s\n", err.c_str());
            return 1;
        }
        if (!auth.authorizeStreaming(err)) {
            std::fprintf(stderr, "signed in, but authorizeStreaming failed: %s\n", err.c_str());
            return 1;
        }
        cfg.save();
        std::printf("Logged in as %s\n", cfg.gamertag().c_str());
        return 0;
    }
    std::fprintf(stderr, "login expired/cancelled\n");
    return 1;
}

// Only request a device code and print it (live smoke test of the MSAL endpoint; no login).
int cmdDeviceCode(Auth& auth) {
    DeviceCode dc;
    std::string err;
    if (!auth.requestDeviceCode(dc, err)) {
        std::fprintf(stderr, "device code request failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("user code   : %s\n", dc.userCode.c_str());
    std::printf("verify url  : %s\n", dc.verificationUri.c_str());
    std::printf("qr url      : %s\n", dc.qrUrl.c_str());
    std::printf("interval    : %d s\n", dc.interval);
    std::printf("expires in  : %d s\n", dc.expiresIn);
    if (!dc.message.empty()) std::printf("message     : %s\n", dc.message.c_str());
    return 0;
}

void printRegions(const char* name, const std::string& base, const std::vector<Region>& regions) {
    if (regions.empty()) {
        std::printf("%-11s : not available\n", name);
        return;
    }
    std::printf("%-11s : %s\n", name, base.c_str());
    for (const auto& r : regions)
        std::printf("              %c %-22s %s%s\n", r.baseUri == base ? '*' : ' ', r.name.c_str(), r.baseUri.c_str(),
                    r.isDefault ? "  (default)" : "");
}

// Refresh the MSA token + authorize streaming, then print offerings and regions.
int cmdToken(Config& cfg, Auth& auth) {
    std::string err;
    if (!auth.isLoggedIn()) {
        std::fprintf(stderr, "not signed in - run: xc-cli login\n");
        return 1;
    }
    if (!auth.refresh(err) || !auth.authorizeStreaming(err)) {
        std::fprintf(stderr, "token: %s\n", err.c_str());
        return 1;
    }
    const Tokens t = cfg.tokens();
    std::printf("gamertag    : %s\n", t.gamertag.c_str());
    std::printf("market      : %s\n", t.market.c_str());
    std::printf("msa valid   : %lld s\n", (long long)(t.msaExpiry - platform::unixTime()));
    std::printf("gs valid    : %lld s\n", (long long)(t.gsExpiry - platform::unixTime()));
    std::printf("xbl web     : %s\n", t.xblToken.empty() ? "-" : "token");
    printRegions("xhome", t.xhomeBase, t.xhomeRegions);
    printRegions("xgpuweb", t.xcloudBase, t.xcloudRegions);
    printRegions("xgpuwebf2p", t.xcloudF2pBase, t.xcloudF2pRegions);
    return 0;
}

int cmdStatus(const Config& cfg, const Auth& auth) {
    const Tokens t = cfg.tokens();
    std::printf("config      : %s\n", cfg.path().c_str());
    std::printf("installId   : %s\n", cfg.installId().c_str());
    std::printf("logged in   : %s\n", auth.isLoggedIn() ? "yes" : "no");
    std::printf("gamertag    : %s\n", t.gamertag.c_str());
    std::printf("msa expiry  : %lld (now %lld)\n", (long long)t.msaExpiry, (long long)platform::unixTime());
    std::printf("gs expiry   : %lld\n", (long long)t.gsExpiry);
    std::printf("xhome       : %s %s\n", t.xhomeGs.empty() ? "-" : "token", t.xhomeBase.c_str());
    std::printf("xcloud      : %s %s\n", t.xcloudGs.empty() ? "-" : "token", t.xcloudBase.c_str());
    std::printf("xcloud f2p  : %s %s\n", t.xcloudF2pGs.empty() ? "-" : "token", t.xcloudF2pBase.c_str());
    return 0;
}

int cmdSession(Config& cfg, Auth& auth, SessionKind kind, const std::string& id) {
    std::string err;
    if (!auth.ensureFresh(err)) {
        std::fprintf(stderr, "auth: %s\n", err.c_str());
        return 1;
    }
    const Tokens t = cfg.tokens();
    Settings gs = cfg.settings();
    gs.resolution = gssv::deviceTierFor(kind == SessionKind::Home, gs.resolution);  // xhome: android only
    GssvClient gssv(kind == SessionKind::Home ? t.xhomeBase : t.xcloudBase,
                    kind == SessionKind::Home ? t.xhomeGs : t.xcloudGs, gs);
    gssv.cleanupActive(kind);
    SessionInfo s;
    if (!gssv.play(kind, id, s, err)) {
        std::fprintf(stderr, "play: %s\n", err.c_str());
        return 1;
    }
    std::printf("session %s (%s)\n", s.sessionId.c_str(), s.sessionPath.c_str());
    bool connected = false;
    int rc = 1;
    while (!g_stop) {
        if (!gssv.pollState(s, err)) {
            std::fprintf(stderr, "state: %s\n", err.c_str());
            break;
        }
        std::printf("state: %s\n", s.state.c_str());
        if (s.state == "ReadyToConnect" && !connected) {
            std::string lpt;
            if (!auth.fetchLpt(lpt, err) || !gssv.connect(s, lpt, err)) {
                std::fprintf(stderr, "connect: %s\n", err.c_str());
                break;
            }
            connected = true;
        } else if (s.state == "Provisioned") {
            std::printf("provisioned; keepalive interval %d s\n", gssv.keepAliveSeconds(s));
            rc = 0;
            break;
        } else if (s.state == "Failed") {
            std::fprintf(stderr, "failed: %s %s\n", s.errorCode.c_str(), s.errorMessage.c_str());
            break;
        }
        sleepMs(1000);
    }
    gssv.stop(s);
    return rc;
}

int cmdStream(Config& cfg, Auth& auth, SessionKind kind, const std::string& id, int seconds) {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) std::fprintf(stderr, "SDL audio unavailable: %s\n", SDL_GetError());
    int rc = 1;
    {
        Streamer st(cfg, auth);
        if (!st.start(kind, id)) {
            std::fprintf(stderr, "stream start failed\n");
            SDL_Quit();
            return 1;
        }
        const auto t0 = std::chrono::steady_clock::now();
        StreamState last = StreamState::Idle;
        uint64_t frames = 0;
        while (!g_stop) {
            StreamState s = st.state();
            if (s != last) {
                std::printf("state: %s  %s\n", streamStateName(s), st.statusText().c_str());
                last = s;
            }
            if (AVFrame* f = st.currentFrame()) {
                ++frames;
                av_frame_free(&f);
            }
            if (s == StreamState::Ended) { rc = 0; break; }
            if (s == StreamState::Failed) break;
            if (s == StreamState::Streaming) {
                rc = 0;
                const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                static int lastPrint = -1;
                if (static_cast<int>(el) != lastPrint) {
                    lastPrint = static_cast<int>(el);
                    StreamStats ss = st.stats();
                    std::printf("%dx%d %.1f fps %.0f kbps decode %.1f ms loss %.1f%% frames %llu\n", ss.width,
                                ss.height, ss.fps, ss.bitrateKbps, ss.decodeMs, ss.lossPercent,
                                (unsigned long long)frames);
                }
                if (seconds > 0 && el >= seconds) break;
            }
            sleepMs(5);
        }
        st.stop();
    }
    SDL_Quit();
    return rc;
}

int cmdOffer() {
    WebRtc rtc;
    WebRtc::Callbacks cb;
    if (!rtc.init(cb)) {
        std::fprintf(stderr, "webrtc init failed\n");
        return 1;
    }
    std::string ufrag, pwd, fp;
    std::vector<std::string> cands;
    if (!rtc.gatherLocal(ufrag, pwd, fp, cands, 5000)) {
        std::fprintf(stderr, "gathering failed\n");
        return 1;
    }
    std::printf("%s\n", proto::buildOffer(ufrag, pwd, fp, false, "1080").c_str());
    for (const auto& c : cands) std::printf("%s\n", c.c_str());
    std::printf("ice body: %s\n", proto::icePostBody(cands, ufrag).c_str());
    rtc.close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--verbose") || !std::strcmp(argv[i], "-v")) verbose = true;
        else args.emplace_back(argv[i]);
    }
    if (args.empty()) return usage();

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!platform::init(argc, argv)) {
        std::fprintf(stderr, "cannot create data dir %s\n", platform::dataDir().c_str());
        return 1;
    }
    logInit("", verbose ? LogLevel::Debug : LogLevel::Info);
    Http::globalInit(platform::assetPath("cacert.pem"));

    // catalog + gssv commands (titles, consoles, play) live in cli_gssv.cpp; they build their own
    // Config/Auth from the same data dir.
    if (cli_gssv_handles(args[0].c_str())) {
        std::vector<char*> av;
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        const int grc = cli_gssv(static_cast<int>(av.size()), av.data());
        Http::globalCleanup();
        logShutdown();
        return grc;
    }

    Config cfg(platform::dataDir() + "/config.json");
    if (!cfg.load()) XC_LOGW("config %s could not be parsed, using defaults", cfg.path().c_str());
    Auth auth(cfg);

    const std::string& cmd = args[0];
    int rc = 2;
    SessionKind kind = SessionKind::Cloud;
    if (cmd == "login") rc = cmdLogin(cfg, auth);
    else if (cmd == "devicecode") rc = cmdDeviceCode(auth);
    else if (cmd == "token") rc = cmdToken(cfg, auth);
    else if (cmd == "refresh") {
        std::string err;
        rc = (auth.refresh(err) && auth.authorizeStreaming(err)) ? 0 : 1;
        if (rc) std::fprintf(stderr, "refresh failed: %s\n", err.c_str());
        else cfg.save();
    } else if (cmd == "status") rc = cmdStatus(cfg, auth);
    else if (cmd == "logout") { auth.logout(); rc = 0; }
    else if (cmd == "session" && args.size() >= 3 && parseKind(args[1], kind)) rc = cmdSession(cfg, auth, kind, args[2]);
    else if (cmd == "stream" && args.size() >= 3 && parseKind(args[1], kind))
        rc = cmdStream(cfg, auth, kind, args[2], args.size() > 3 ? std::atoi(args[3].c_str()) : 0);
    else if (cmd == "offer") rc = cmdOffer();
    else rc = usage();

    Http::globalCleanup();
    logShutdown();
    return rc;
}
