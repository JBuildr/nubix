// Nubix — logging.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#include <sys/stat.h>

namespace xc {
namespace {

// log.txt lives on the console's /data partition: once it grows past this it is moved to
// log.prev.txt at the next start (one generation kept), so the file never grows unbounded.
constexpr long long kMaxLogBytes = 2LL * 1024 * 1024;

std::string previousLogPath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return path.substr(0, dot) + ".prev" + path.substr(dot);
    return path + ".prev";
}

std::mutex g_mutex;
FILE* g_file = nullptr;
LogLevel g_level = LogLevel::Info;
const auto g_start = std::chrono::steady_clock::now();

const char* tag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "D";
        case LogLevel::Info: return "I";
        case LogLevel::Warn: return "W";
        case LogLevel::Error: return "E";
    }
    return "?";
}

}  // namespace

void logInit(const std::string& filePath, LogLevel minLevel) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_level = minLevel;
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (!filePath.empty()) {
        struct stat st{};
        if (::stat(filePath.c_str(), &st) == 0 && static_cast<long long>(st.st_size) > kMaxLogBytes) {
            const std::string prev = previousLogPath(filePath);
            std::remove(prev.c_str());
            if (std::rename(filePath.c_str(), prev.c_str()) != 0) std::remove(filePath.c_str());
        }
        g_file = std::fopen(filePath.c_str(), "a");
        if (g_file) std::setvbuf(g_file, nullptr, _IOLBF, 0);
    }
}

void logSetLevel(LogLevel minLevel) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_level = minLevel;
}

LogLevel logLevel() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_level;
}

void logv(LogLevel level, const char* fmt, va_list ap) {
    char msg[2048];
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();

    std::lock_guard<std::mutex> lk(g_mutex);
    if (level < g_level) return;
    std::fprintf(stdout, "[%9.3f] %s %s\n", t, tag(level), msg);
    std::fflush(stdout);
    if (g_file) {
        std::time_t now = std::time(nullptr);
        char ts[32];
        std::tm tmv{};
        localtime_r(&now, &tmv);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        std::fprintf(g_file, "%s [%9.3f] %s %s\n", ts, t, tag(level), msg);
    }
}

void log(LogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(level, fmt, ap);
    va_end(ap);
}

void logShutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace xc
