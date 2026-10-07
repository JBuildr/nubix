// Nubix — platform abstraction (PS5 payload vs. host build).
// Implemented in platform/ps5.cpp (XC_PS5) or platform/host.cpp.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>

namespace xc {
namespace platform {

// Early process init: detect the install / executable dir (PS5: absolute argv[0], else the cwd
// when it holds assets/, else /data/homebrew/nubix) and create dataDir(). Returns false
// on fatal error (PS5: also shows a system notification, since stderr is not visible).
// System modules (libScePad, AudioOut, VideoOut, ...) are not loaded here: the payload ELF's
// DT_NEEDED entries make the loader map them before main().
bool init(int argc, char** argv);

// Undo init().
void shutdown();

// Writable data dir, created on demand. PS5: /data/nubix; host: $XC_DATA_DIR or
// ~/.config/nubix (macOS: ~/Library/Application Support/nubix).
std::string dataDir();

// Read-only assets dir: <exe dir>/assets, fallback /data/homebrew/nubix/assets (PS5)
// or the source tree's assets/ (host, XC_SOURCE_ASSETS_DIR).
std::string assetsDir();

// assetsDir() + "/" + rel.
std::string assetPath(const std::string& rel);

// Directory containing the running executable.
std::string exeDir();

// Show a system notification (PS5: sceKernelSendNotificationRequest), host: log only.
void notify(const std::string& msg);

// True if an UP interface has a usable IPv4 address (see localIpv4()).
bool networkAvailable();

// First usable IPv4 address of an UP interface ("" if none): skips loopback, 0.0.0.0 (PS5
// lists idle interfaces that way) and link-local 169.254/16. Calls getifaddrs(): cache the
// result in per-frame code.
std::string localIpv4();

// Called by the UI once the first frame has been presented. PS5: hides the system splash
// screen shown while a (fake) app starts; host: no-op.
void onFirstFrame();

// Milliseconds from a monotonic clock.
uint64_t monotonicMs();

// Seconds since the Unix epoch (wall clock).
int64_t unixTime();

// True when built for / running on PS5.
constexpr bool isPs5() {
#ifdef XC_PS5
    return true;
#else
    return false;
#endif
}

}  // namespace platform
}  // namespace xc
