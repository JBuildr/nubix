// Nubix — platform functions shared by PS5 and host builds (POSIX).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "platform/platform.hpp"
#include "platform/net_compat.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>

namespace xc {
namespace platform {

std::string assetPath(const std::string& rel) { return assetsDir() + "/" + rel; }

// Usable unicast address: not INADDR_ANY (PS5 lists idle interfaces as UP with 0.0.0.0, which
// websrv/elfldr skip for the same reason), not loopback, not link-local 169.254/16 (no DHCP lease).
bool usableIpv4(uint32_t hostOrder) {
    if ((hostOrder >> 24) == 0) return false;            // 0.0.0.0/8
    if ((hostOrder >> 24) == 127) return false;          // loopback
    if ((hostOrder >> 16) == 0xA9FE) return false;       // 169.254/16
    return true;
}

std::string localIpv4() {
    struct ifaddrs* list = nullptr;
    if (xc_getifaddrs(&list) != 0) return std::string();  // with default-route fallback (PS5)
    std::string result;
    for (struct ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
        if (it->ifa_name && std::strncmp(it->ifa_name, "lo", 2) == 0) continue;
        auto* sin = reinterpret_cast<struct sockaddr_in*>(it->ifa_addr);
        if (!usableIpv4(ntohl(sin->sin_addr.s_addr))) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            result = buf;
            break;
        }
    }
    xc_freeifaddrs(list);
    return result;
}

bool networkAvailable() { return !localIpv4().empty(); }

uint64_t monotonicMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int64_t unixTime() { return static_cast<int64_t>(std::time(nullptr)); }

}  // namespace platform
}  // namespace xc
