// Nubix — host (macOS/Linux) platform implementation.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef XC_PS5

#include <sys/stat.h>

#include <cstdio>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "core/log.hpp"
#include "platform/platform.hpp"

namespace xc {
namespace platform {
namespace {

std::string g_exeDir = ".";

bool mkdirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "/") continue;
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

bool isDir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string dirName(const std::string& p) {
    auto pos = p.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

std::string resolveExePath(const char* argv0) {
    char buf[PATH_MAX] = {0};
#if defined(__APPLE__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[PATH_MAX] = {0};
        if (realpath(buf, real)) return real;
        return buf;
    }
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        return buf;
    }
#endif
    if (argv0) {
        char real[PATH_MAX] = {0};
        if (realpath(argv0, real)) return real;
        return argv0;
    }
    return "./nubix";
}

}  // namespace

bool init(int argc, char** argv) {
    g_exeDir = dirName(resolveExePath(argc > 0 ? argv[0] : nullptr));
    // Keep the sign-in and settings of installs from before the rename (0.3.0).
    const std::string dir = dataDir();
    const size_t slash = dir.rfind('/');
    if (slash != std::string::npos && dir.compare(slash + 1, std::string::npos, "nubix") == 0) {
        const std::string legacy = dir.substr(0, slash + 1) + "ps5-xcloud";
        struct stat st {};
        if (::stat(dir.c_str(), &st) != 0 && ::stat(legacy.c_str(), &st) == 0) std::rename(legacy.c_str(), dir.c_str());
    }
    return mkdirs(dir);
}

void shutdown() {}

std::string dataDir() {
    if (const char* env = std::getenv("XC_DATA_DIR"); env && *env) return env;
    const char* home = std::getenv("HOME");
    std::string base = home && *home ? home : ".";
#if defined(__APPLE__)
    return base + "/Library/Application Support/nubix";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return std::string(xdg) + "/nubix";
    return base + "/.config/nubix";
#endif
}

std::string assetsDir() {
    if (const char* env = std::getenv("XC_ASSETS_DIR"); env && *env) return env;
    std::string local = g_exeDir + "/assets";
    if (isDir(local)) return local;
#ifdef XC_SOURCE_ASSETS_DIR
    return XC_SOURCE_ASSETS_DIR;
#else
    return local;
#endif
}

std::string exeDir() { return g_exeDir; }

void notify(const std::string& msg) { XC_LOGI("[notify] %s", msg.c_str()); }

void onFirstFrame() {}

}  // namespace platform
}  // namespace xc

#endif  // !XC_PS5
