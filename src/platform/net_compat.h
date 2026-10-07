/* Nubix — resolver / interface-list fallbacks for the PS5 libc.
 *
 * The PS5 libc resolves real host names (curl works), but getaddrinfo() fails for the special
 * cases libjuice relies on: node == NULL with AI_PASSIVE ("any" address to bind a UDP socket) and
 * numeric hosts with AI_NUMERICHOST. xc_getaddrinfo() answers those itself (inet_pton) and only
 * hands real names to the system resolver. xc_getifaddrs() falls back to the address of the
 * default route when getifaddrs() fails or lists no usable IPv4 interface.
 *
 * The PS5 build of libdatachannel force-includes this header into its C sources (libjuice) with
 * XC_NET_COMPAT_REDIRECT defined (scripts/build-deps.sh), so their calls land here.
 * Copyright (C) 2026 Nubix contributors
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef XC_NET_COMPAT_H
#define XC_NET_COMPAT_H

#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netdb.h>

#ifdef __cplusplus
extern "C" {
#endif

int xc_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void xc_freeaddrinfo(struct addrinfo *ai);
int xc_getifaddrs(struct ifaddrs **ifap);
void xc_freeifaddrs(struct ifaddrs *ifa);
/* Numeric conversions (NI_NUMERICHOST/NI_NUMERICSERV) via inet_ntop; others go to the system. */
int xc_getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv,
                   socklen_t servlen, int flags);

#ifdef __cplusplus
}
#endif

#if defined(XC_NET_COMPAT_REDIRECT) && !defined(XC_NET_COMPAT_IMPL)
#define getaddrinfo xc_getaddrinfo
#define freeaddrinfo xc_freeaddrinfo
#define getifaddrs xc_getifaddrs
#define freeifaddrs xc_freeifaddrs
#define getnameinfo xc_getnameinfo
#endif

#endif /* XC_NET_COMPAT_H */
