/* Nubix — resolver / interface-list fallbacks (see net_compat.h).
 * Copyright (C) 2026 Nubix contributors
 * SPDX-License-Identifier: GPL-3.0-only */
#define XC_NET_COMPAT_IMPL 1
#include "platform/net_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- getaddrinfo ------------------------------------------------------------------------- */

/* Each result node is one allocation: addrinfo followed by its sockaddr_storage. */
struct xc_ai_node {
    struct addrinfo ai;
    struct sockaddr_storage ss;
};

static struct addrinfo *xc_ai_new(const struct sockaddr *sa, socklen_t len, int socktype, int protocol) {
    struct xc_ai_node *n = (struct xc_ai_node *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    if (len > (socklen_t)sizeof(n->ss)) len = (socklen_t)sizeof(n->ss);
    memcpy(&n->ss, sa, (size_t)len);
    n->ai.ai_family = sa->sa_family;
    n->ai.ai_socktype = socktype;
    n->ai.ai_protocol = protocol;
    n->ai.ai_addrlen = len;
    n->ai.ai_addr = (struct sockaddr *)&n->ss;
    return &n->ai;
}

void xc_freeaddrinfo(struct addrinfo *ai) {
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_canonname);
        free(ai); /* ai is the first member of its xc_ai_node */
        ai = next;
    }
}

/* Numeric service (or NULL = 0). Returns -1 for a service name. */
static int xc_parse_port(const char *service) {
    if (!service || !*service) return 0;
    char *end = NULL;
    long v = strtol(service, &end, 10);
    if (*end != '\0' || v < 0 || v > 65535) return -1;
    return (int)v;
}

static void xc_hint_types(const struct addrinfo *hints, int *socktype, int *protocol) {
    *socktype = hints ? hints->ai_socktype : 0;
    *protocol = hints ? hints->ai_protocol : 0;
    if (*socktype == 0) *socktype = SOCK_DGRAM;
    if (*protocol == 0) *protocol = *socktype == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
}

/* Copy a system result into our own nodes so xc_freeaddrinfo can free every list uniformly. */
static int xc_copy_list(const struct addrinfo *src, struct addrinfo **res) {
    struct addrinfo *head = NULL, **tail = &head;
    for (const struct addrinfo *a = src; a; a = a->ai_next) {
        if (!a->ai_addr || (a->ai_family != AF_INET && a->ai_family != AF_INET6)) continue;
        struct addrinfo *n = xc_ai_new(a->ai_addr, a->ai_addrlen, a->ai_socktype, a->ai_protocol);
        if (!n) {
            xc_freeaddrinfo(head);
            return EAI_MEMORY;
        }
        n->ai_flags = a->ai_flags;
        if (a->ai_canonname) n->ai_canonname = strdup(a->ai_canonname);
        *tail = n;
        tail = &n->ai_next;
    }
    if (!head) return EAI_NONAME;
    *res = head;
    return 0;
}

int xc_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_FAIL;
    *res = NULL;
    const int family = hints ? hints->ai_family : AF_UNSPEC;
    const int flags = hints ? hints->ai_flags : 0;
    const int port = xc_parse_port(service);
    int socktype, protocol;
    xc_hint_types(hints, &socktype, &protocol);

    if (port >= 0) {
        /* node == NULL: wildcard (AI_PASSIVE) or loopback. IPv4 unless IPv6 was asked for:
         * the PS5 network stack is used IPv4-only here (Teredo candidates are decoded to IPv4). */
        if (!node) {
            if (family == AF_INET6) {
                struct sockaddr_in6 sin6;
                memset(&sin6, 0, sizeof(sin6));
                sin6.sin6_family = AF_INET6;
                sin6.sin6_port = htons((uint16_t)port);
                sin6.sin6_addr = (flags & AI_PASSIVE) ? in6addr_any : in6addr_loopback;
#ifdef SIN6_LEN
                sin6.sin6_len = sizeof(sin6);
#endif
                *res = xc_ai_new((const struct sockaddr *)&sin6, sizeof(sin6), socktype, protocol);
            } else {
                struct sockaddr_in sin;
                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
                sin.sin_port = htons((uint16_t)port);
                sin.sin_addr.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
#if defined(__FreeBSD__) || defined(__APPLE__)
                sin.sin_len = sizeof(sin);
#endif
                *res = xc_ai_new((const struct sockaddr *)&sin, sizeof(sin), socktype, protocol);
            }
            return *res ? 0 : EAI_MEMORY;
        }

        struct in_addr a4;
        if ((family == AF_UNSPEC || family == AF_INET) && inet_pton(AF_INET, node, &a4) == 1) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons((uint16_t)port);
            sin.sin_addr = a4;
#if defined(__FreeBSD__) || defined(__APPLE__)
            sin.sin_len = sizeof(sin);
#endif
            *res = xc_ai_new((const struct sockaddr *)&sin, sizeof(sin), socktype, protocol);
            return *res ? 0 : EAI_MEMORY;
        }
        struct in6_addr a6;
        const char *pct = strchr(node, '%'); /* scoped literal: let the system handle it */
        if (!pct && (family == AF_UNSPEC || family == AF_INET6) && inet_pton(AF_INET6, node, &a6) == 1) {
            struct sockaddr_in6 sin6;
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = htons((uint16_t)port);
            sin6.sin6_addr = a6;
#ifdef SIN6_LEN
            sin6.sin6_len = sizeof(sin6);
#endif
            *res = xc_ai_new((const struct sockaddr *)&sin6, sizeof(sin6), socktype, protocol);
            return *res ? 0 : EAI_MEMORY;
        }
        if (flags & AI_NUMERICHOST) return EAI_NONAME;
    }

    /* A real host name (or a service name): system resolver. Retry without AI_ADDRCONFIG,
     * which minimal resolvers may not support. */
    struct addrinfo h;
    memset(&h, 0, sizeof(h));
    if (hints) h = *hints;
    h.ai_addr = NULL;
    h.ai_canonname = NULL;
    h.ai_next = NULL;
    struct addrinfo *sys = NULL;
    int rc = getaddrinfo(node, service, &h, &sys);
#ifdef AI_ADDRCONFIG
    if (rc != 0 && (h.ai_flags & AI_ADDRCONFIG)) {
        h.ai_flags &= ~AI_ADDRCONFIG;
        rc = getaddrinfo(node, service, &h, &sys);
    }
#endif
    if (rc != 0) return rc;
    rc = xc_copy_list(sys, res);
    freeaddrinfo(sys);
    return rc;
}

/* ---- getnameinfo ------------------------------------------------------------------------- */

int xc_getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv,
                   socklen_t servlen, int flags) {
    if (!sa) return EAI_FAIL;
    const int wantNumericHost = !host || hostlen == 0 || (flags & NI_NUMERICHOST);
    const int wantNumericServ = !serv || servlen == 0 || (flags & NI_NUMERICSERV);
    if (!wantNumericHost || !wantNumericServ || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6))
        return getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
    unsigned port;
    if (sa->sa_family == AF_INET) {
        if (salen < (socklen_t)sizeof(struct sockaddr_in)) return EAI_FAIL;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        if (host && hostlen && !inet_ntop(AF_INET, &sin->sin_addr, host, hostlen)) return EAI_OVERFLOW;
        port = ntohs(sin->sin_port);
    } else {
        if (salen < (socklen_t)sizeof(struct sockaddr_in6)) return EAI_FAIL;
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        if (host && hostlen && !inet_ntop(AF_INET6, &sin6->sin6_addr, host, hostlen)) return EAI_OVERFLOW;
        port = ntohs(sin6->sin6_port);
    }
    if (serv && servlen) {
        int n = snprintf(serv, servlen, "%u", port);
        if (n < 0 || (socklen_t)n >= servlen) return EAI_OVERFLOW;
    }
    return 0;
}

/* ---- getifaddrs -------------------------------------------------------------------------- */

struct xc_ifa_node {
    struct ifaddrs ifa;
    struct sockaddr_storage addr, mask;
    char name[IFNAMSIZ];
};

static struct ifaddrs *xc_ifa_new(const char *name, unsigned flags, const struct sockaddr *addr,
                                  const struct sockaddr *mask) {
    struct xc_ifa_node *n = (struct xc_ifa_node *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    strncpy(n->name, name ? name : "eth0", sizeof(n->name) - 1);
    n->ifa.ifa_name = n->name;
    n->ifa.ifa_flags = flags;
    if (addr) {
        const size_t len = addr->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        memcpy(&n->addr, addr, len);
        n->ifa.ifa_addr = (struct sockaddr *)&n->addr;
    }
    if (mask) {
        const size_t len = mask->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        memcpy(&n->mask, mask, len);
        n->ifa.ifa_netmask = (struct sockaddr *)&n->mask;
    }
    return &n->ifa;
}

void xc_freeifaddrs(struct ifaddrs *ifa) {
    while (ifa) {
        struct ifaddrs *next = ifa->ifa_next;
        free(ifa); /* ifa is the first member of its xc_ifa_node */
        ifa = next;
    }
}

/* Address the kernel would use for the default route: connect() a UDP socket (sends nothing). */
static int xc_default_route_ipv4(struct sockaddr_in *out) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = htonl(0x08080808); /* 8.8.8.8 — never contacted */
#if defined(__FreeBSD__) || defined(__APPLE__)
    dst.sin_len = sizeof(dst);
#endif
    int rc = -1;
    if (connect(s, (const struct sockaddr *)&dst, sizeof(dst)) == 0) {
        socklen_t len = sizeof(*out);
        if (getsockname(s, (struct sockaddr *)out, &len) == 0 && out->sin_family == AF_INET &&
            out->sin_addr.s_addr != htonl(INADDR_ANY))
            rc = 0;
    }
    close(s);
    return rc;
}

static int xc_usable_ipv4(const struct sockaddr *sa, unsigned flags) {
    if (!sa || sa->sa_family != AF_INET) return 0;
    if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK)) return 0;
    const uint32_t a = ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
    return a != 0 && (a >> 24) != 127 && (a >> 16) != 0xA9FE; /* not 0/8-any, loopback, link-local */
}

int xc_getifaddrs(struct ifaddrs **ifap) {
    if (!ifap) {
        errno = EINVAL;
        return -1;
    }
    *ifap = NULL;
    struct ifaddrs *head = NULL, **tail = &head;
    int haveIpv4 = 0;

    struct ifaddrs *sys = NULL;
    if (getifaddrs(&sys) == 0) {
        for (struct ifaddrs *i = sys; i; i = i->ifa_next) {
            if (!i->ifa_addr || (i->ifa_addr->sa_family != AF_INET && i->ifa_addr->sa_family != AF_INET6)) continue;
            struct ifaddrs *n = xc_ifa_new(i->ifa_name, i->ifa_flags, i->ifa_addr, i->ifa_netmask);
            if (!n) break;
            if (xc_usable_ipv4(i->ifa_addr, i->ifa_flags)) haveIpv4 = 1;
            *tail = n;
            tail = &n->ifa_next;
        }
        freeifaddrs(sys);
    }

    if (!haveIpv4) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        if (xc_default_route_ipv4(&sin) == 0) {
            struct ifaddrs *n = xc_ifa_new("eth0", IFF_UP | IFF_RUNNING, (const struct sockaddr *)&sin, NULL);
            if (n) {
                ((struct sockaddr_in *)n->ifa_addr)->sin_port = 0;
                *tail = n;
                tail = &n->ifa_next;
            }
        }
    }

    if (!head) {
        errno = ENETDOWN;
        return -1;
    }
    *ifap = head;
    return 0;
}
