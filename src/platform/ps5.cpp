// Nubix — PS5 payload platform implementation.
// Notification request layout from the ps5-payload-sdk samples (hello_world / notify_debug).
// Notification layout from the ps5-payload-sdk samples (John Toernblom, GPL-3.0-or-later) and ProsperoLight (GPL-3.0).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifdef XC_PS5

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.hpp"
#include "platform/platform.hpp"

// Notification request as used by the PS5 payload SDK samples / ProsperoLight.
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
extern "C" int sceSystemServiceHideSplashScreen(void);

namespace xc {
namespace platform {
namespace {

const char* kDataDir = "/data/nubix";
const char* kLegacyDataDir = "/data/ps5-xcloud";  // name before 0.3.0: migrated on first start
const char* kInstallDir = "/data/homebrew/nubix";
std::string g_exeDir = kInstallDir;
std::atomic<bool> g_splashHidden{false};

bool isDir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p
bool mkdirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/") continue;
            if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
        }
    }
    return isDir(path);
}

std::string dirName(const std::string& p) {
    auto pos = p.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

}  // namespace

bool init(int argc, char** argv) {
    // websrv launches eboot.elf with an absolute argv[0] and cwd = install dir; elfldr payloads
    // have no meaningful argv[0], so fall back to the cwd (if it has assets/) or the install dir.
    char cwd[1024] = {0};
    if (argc > 0 && argv && argv[0] && std::strchr(argv[0], '/') && isDir(dirName(argv[0]))) {
        g_exeDir = dirName(argv[0]);
    } else if (getcwd(cwd, sizeof(cwd)) && isDir(std::string(cwd) + "/assets")) {
        g_exeDir = cwd;
    } else {
        g_exeDir = kInstallDir;
    }
    // Keep the sign-in and settings of installs from before the rename.
    if (!isDir(kDataDir) && isDir(kLegacyDataDir)) std::rename(kLegacyDataDir, kDataDir);
    if (!mkdirs(kDataDir)) {
        const int e = errno;
        std::fprintf(stderr, "cannot create %s: %s\n", kDataDir, std::strerror(e));
        // stderr goes to the launcher pipe only: tell the user why the app closes right away.
        notify(std::string("nubix: cannot create ") + kDataDir + " (" + std::strerror(e) + ")");
        return false;
    }
    return true;
}

void shutdown() {}

std::string dataDir() { return kDataDir; }

std::string assetsDir() {
    std::string local = g_exeDir + "/assets";
    if (isDir(local)) return local;
    return std::string(kInstallDir) + "/assets";
}

std::string exeDir() { return g_exeDir; }

void notify(const std::string& msg) {
    XC_LOGI("[notify] %s", msg.c_str());
    notify_request_t req;
    std::memset(&req, 0, sizeof(req));
    std::snprintf(req.message, sizeof(req.message), "%s", msg.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void onFirstFrame() {
    if (g_splashHidden.exchange(true)) return;
    int rc = sceSystemServiceHideSplashScreen();
    if (rc != 0) XC_LOGD("sceSystemServiceHideSplashScreen: 0x%08x", static_cast<unsigned>(rc));
}

}  // namespace platform
}  // namespace xc

#endif  // XC_PS5
