// Nubix — resolver fallbacks used by libjuice/libdatachannel on PS5 (net_compat.c).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cstring>
#include <string>

#include "platform/net_compat.h"
#include "test.hpp"

XC_TEST(net, passive_null_node_gives_ipv4_any) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    XC_CHECK_EQ(xc_getaddrinfo(nullptr, "0", &hints, &res), 0);
    XC_CHECK(res != nullptr);
    XC_CHECK_EQ(res->ai_family, AF_INET);
    XC_CHECK_EQ(res->ai_socktype, SOCK_DGRAM);
    auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    XC_CHECK_EQ(sin->sin_addr.s_addr, htonl(INADDR_ANY));
    XC_CHECK_EQ(sin->sin_port, htons(0));
    XC_CHECK(res->ai_next == nullptr);
    xc_freeaddrinfo(res);
}

XC_TEST(net, numeric_hosts) {
    addrinfo hints{};
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    XC_CHECK_EQ(xc_getaddrinfo("20.40.60.80", "9002", &hints, &res), 0);
    XC_CHECK_EQ(res->ai_family, AF_INET);
    XC_CHECK_EQ(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_port, htons(9002));
    char host[64], serv[16];
    XC_CHECK_EQ(xc_getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof host, serv, sizeof serv,
                               NI_NUMERICHOST | NI_NUMERICSERV), 0);
    XC_CHECK_EQ(std::string(host), std::string("20.40.60.80"));
    XC_CHECK_EQ(std::string(serv), std::string("9002"));
    xc_freeaddrinfo(res);

    res = nullptr;
    XC_CHECK_EQ(xc_getaddrinfo("2001:0:4137:9e76::1", "1", &hints, &res), 0);
    XC_CHECK_EQ(res->ai_family, AF_INET6);
    xc_freeaddrinfo(res);

    res = nullptr;
    XC_CHECK(xc_getaddrinfo("example.com", "1", &hints, &res) != 0);  // AI_NUMERICHOST: no lookup
    XC_CHECK(res == nullptr);
}

XC_TEST(net, family_filter) {
    addrinfo hints{};
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* res = nullptr;
    XC_CHECK(xc_getaddrinfo("1.2.3.4", "1", &hints, &res) != 0);
}

XC_TEST(net, ifaddrs_has_usable_ipv4) {
    ifaddrs* list = nullptr;
    XC_CHECK_EQ(xc_getifaddrs(&list), 0);
    bool found = false;
    for (ifaddrs* i = list; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK)) found = true;
    XC_CHECK(found);
    xc_freeifaddrs(list);
}
