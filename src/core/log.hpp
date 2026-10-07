// Nubix — logging.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdarg>
#include <string>

namespace xc {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Initialise logging. Messages >= minLevel go to stdout; if filePath is non-empty they are
// also appended to that file (used on PS5: <dataDir>/log.txt). Safe to call more than once.
void logInit(const std::string& filePath, LogLevel minLevel = LogLevel::Info);

// Change the minimum level at runtime (e.g. --verbose).
void logSetLevel(LogLevel minLevel);

// Current minimum level.
LogLevel logLevel();

// printf-style log line, thread-safe; a timestamp, level tag and newline are added.
void log(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// va_list variant of log().
void logv(LogLevel level, const char* fmt, va_list ap);

// Flush and close the log file.
void logShutdown();

}  // namespace xc

#define XC_LOGD(...) ::xc::log(::xc::LogLevel::Debug, __VA_ARGS__)
#define XC_LOGI(...) ::xc::log(::xc::LogLevel::Info, __VA_ARGS__)
#define XC_LOGW(...) ::xc::log(::xc::LogLevel::Warn, __VA_ARGS__)
#define XC_LOGE(...) ::xc::log(::xc::LogLevel::Error, __VA_ARGS__)
