#!/usr/bin/env python3
# Nubix — protective DNS server for a jailbroken PS5: allow only what Nubix needs, block the rest.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
"""
Run this on a PC in the same network as the PS5, then set the PS5's Primary AND Secondary DNS to
the address this script prints.

Allowlist mode: every DNS query is BLOCKED (answered with 0.0.0.0 / ::) unless the name belongs to
  1. the domains Nubix needs for Xbox Cloud Gaming (NUBIX_DOMAINS below), or
  2. a domain you add yourself in allowlist.txt (next to this script) or with --allow.
Sony / PlayStation domains are ALWAYS blocked, even if they appear in your allowlist, so the
console cannot reach PSN, download a system update or send telemetry.
Allowed names are forwarded unchanged to an upstream DNS server (default 1.1.1.1).

You are responsible for every domain you add to the allowlist.

Only the Python standard library is used (Python 3.8+).

  Windows (terminal "Run as administrator"):   python tools\\sony_dns.py
  macOS / Linux:                               sudo python3 tools/sony_dns.py

Options:
  --allow example.com      allow one more domain (with its subdomains; can be repeated)
  --allowlist FILE         allowlist file (default: allowlist.txt next to this script)
  --map host=IP            answer host with a fixed IPv4 address, e.g. for a jailbreak page
                           (works even for Sony names; can be repeated)
  --upstream 9.9.9.9       upstream DNS server (default 1.1.1.1)
  --log-all                print every query (ALLOW / BLOCK / MAP) to see what the PS5 asks for
  --quiet                  print nothing per query
  --port 53                listening port (the PS5 always uses 53)
The PC has to stay on while the PS5 is online; give it a fixed IP in your router.
"""

import argparse
import ipaddress
import os
import socket
import socketserver
import struct
import sys
import threading
import time

# What Nubix itself contacts (see src/core/auth.cpp, gssv.cpp, catalog.cpp, stream/webrtc.cpp).
# Each entry also covers its subdomains.
NUBIX_DOMAINS = [
    "login.microsoftonline.com",        # Microsoft sign-in (device code, tokens)
    "login.live.com",                   # transfer token for starting cloud sessions
    "xboxlive.com",                     # Xbox Live tokens, profile, cloud gaming API and regions
    "displaycatalog.mp.microsoft.com",  # game names
    "store-images.s-microsoft.com",     # box art
    "stun.l.google.com",                # WebRTC: discovers the public address for the stream
]

# Always blocked, whatever the allowlist says (only --map can answer one of these names).
SONY_DOMAINS = [
    "playstation.com",
    "playstation.net",
    "playstation.org",
    "sonyentertainmentnetwork.com",
    "scea.com",
    "sbdnpd.com",
    "ribob01.net",
]

TYPE_A, TYPE_AAAA = 1, 28
ANSWER_TTL = 300
UPSTREAM_TIMEOUT = 4.0

_print_lock = threading.Lock()


def log(msg):
    with _print_lock:
        print(time.strftime("%H:%M:%S"), msg, flush=True)


def normalize(domain):
    return domain.strip().lower().strip(".")


def matches(name, domains):
    return any(name == d or name.endswith("." + d) for d in domains)


def load_allowlist(path):
    """One domain per line; '#' starts a comment. Missing file = empty list."""
    domains = []
    if not path or not os.path.isfile(path):
        return domains
    with open(path, encoding="utf-8") as f:
        for line in f:
            entry = normalize(line.split("#", 1)[0])
            if entry:
                domains.append(entry)
    return domains


def parse_question(packet):
    """Return (qname, qtype, end_offset_of_question) of the first question, or None."""
    if len(packet) < 12 or struct.unpack(">H", packet[4:6])[0] < 1:
        return None
    labels, pos = [], 12
    while True:
        if pos >= len(packet):
            return None
        length = packet[pos]
        if length == 0:
            pos += 1
            break
        if length & 0xC0:  # compression pointers do not occur in a query's question
            return None
        pos += 1
        labels.append(packet[pos:pos + length].decode("ascii", "replace"))
        pos += length
    if pos + 4 > len(packet):
        return None
    qtype = struct.unpack(">H", packet[pos:pos + 2])[0]
    return ".".join(labels).lower().rstrip("."), qtype, pos + 4


def local_response(query, qtype, question_end, ipv4=b"\x00\x00\x00\x00", ipv6=b"\x00" * 16):
    """Answer the question locally: A -> ipv4, AAAA -> ipv6 (None = no AAAA record),
    any other type -> empty NOERROR answer."""
    qid = query[0:2]
    flags = struct.unpack(">H", query[2:4])[0]
    resp_flags = 0x8000 | (flags & 0x7800) | (flags & 0x0100) | 0x0080  # QR, opcode, RD, RA
    if qtype == TYPE_A:
        answer = struct.pack(">HHHIH", 0xC00C, TYPE_A, 1, ANSWER_TTL, 4) + ipv4
    elif qtype == TYPE_AAAA and ipv6 is not None:
        answer = struct.pack(">HHHIH", 0xC00C, TYPE_AAAA, 1, ANSWER_TTL, 16) + ipv6
    else:
        answer = b""
    header = qid + struct.pack(">HHHHH", resp_flags, 1, 1 if answer else 0, 0, 0)
    return header + query[12:question_end] + answer


def servfail(query):
    if len(query) < 12:
        return b""
    flags = struct.unpack(">H", query[2:4])[0]
    resp_flags = 0x8000 | (flags & 0x7900) | 0x0080 | 2  # SERVFAIL
    return query[0:2] + struct.pack(">HHHHH", resp_flags, 0, 0, 0, 0)


class Resolver:
    def __init__(self, upstream, allowed, mapped, log_all, quiet):
        self.upstream = upstream
        self.allowed = allowed
        self.mapped = mapped
        self.log_all = log_all
        self.quiet = quiet
        self.counts = {"ALLOW": 0, "BLOCK": 0, "MAP": 0}

    def _note(self, verdict, name):
        self.counts[verdict] += 1
        if self.quiet:
            return
        if self.log_all or verdict == "BLOCK":
            log("%-5s %s" % (verdict, name))

    def handle(self, query, tcp=False):
        q = parse_question(query)
        if not q:
            return servfail(query)
        name, qtype, end = q
        if name in self.mapped:
            self._note("MAP", name)
            return local_response(query, qtype, end, ipv4=self.mapped[name], ipv6=None)
        if matches(name, SONY_DOMAINS) or not matches(name, self.allowed):
            self._note("BLOCK", name)
            return local_response(query, qtype, end)
        self._note("ALLOW", name)
        try:
            return self.forward_tcp(query) if tcp else self.forward_udp(query)
        except OSError as e:
            log("upstream error for %s: %s" % (name, e))
            return servfail(query)

    def forward_udp(self, query):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(UPSTREAM_TIMEOUT)
            s.sendto(query, (self.upstream, 53))
            while True:
                data, _ = s.recvfrom(65535)
                if data[:2] == query[:2]:
                    return data

    def forward_tcp(self, query):
        with socket.create_connection((self.upstream, 53), timeout=UPSTREAM_TIMEOUT) as s:
            s.sendall(struct.pack(">H", len(query)) + query)
            size = struct.unpack(">H", recv_exact(s, 2))[0]
            return recv_exact(s, size)


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise OSError("connection closed")
        buf += chunk
    return buf


class UDPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        resp = self.server.resolver.handle(data)
        if resp:
            sock.sendto(resp, self.client_address)


class TCPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            while True:
                size = struct.unpack(">H", recv_exact(self.request, 2))[0]
                resp = self.server.resolver.handle(recv_exact(self.request, size), tcp=True)
                self.request.sendall(struct.pack(">H", len(resp)) + resp)
        except OSError:
            pass


class ThreadingUDP(socketserver.ThreadingMixIn, socketserver.UDPServer):
    daemon_threads = True
    allow_reuse_address = True


class ThreadingTCP(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def lan_ip():
    """Address of this PC on the local network (no packet is sent)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 53))
            return s.getsockname()[0]
    except OSError:
        return "this PC's IP address"


def parse_maps(values):
    mapped = {}
    for v in values:
        host, sep, ip = v.partition("=")
        try:
            if not sep:
                raise ValueError
            mapped[normalize(host)] = ipaddress.IPv4Address(ip.strip()).packed
        except ValueError:
            sys.exit("--map expects host=IPv4, e.g. --map manuals.playstation.net=192.168.1.20 (got %r)" % v)
    return mapped


def main():
    default_list = os.path.join(os.path.dirname(os.path.abspath(__file__)), "allowlist.txt")
    ap = argparse.ArgumentParser(description="Protective DNS for a jailbroken PS5: allows what Nubix needs, "
                                             "blocks everything else and always blocks Sony.")
    ap.add_argument("--allow", action="append", default=[], metavar="DOMAIN", help="allow one more domain")
    ap.add_argument("--allowlist", default=default_list, metavar="FILE", help="allowlist file (default: %(default)s)")
    ap.add_argument("--map", action="append", default=[], metavar="HOST=IP", help="answer HOST with a fixed IPv4")
    ap.add_argument("--upstream", default="1.1.1.1", help="upstream DNS server (default 1.1.1.1)")
    ap.add_argument("--port", type=int, default=53, help="listening port (default 53)")
    ap.add_argument("--bind", default="0.0.0.0", help="listening address (default all interfaces)")
    ap.add_argument("--log-all", action="store_true", help="print every query")
    ap.add_argument("--quiet", action="store_true", help="print nothing per query")
    args = ap.parse_args()

    user = load_allowlist(args.allowlist) + [normalize(d) for d in args.allow if normalize(d)]
    ignored = [d for d in user if matches(d, SONY_DOMAINS)]
    allowed = NUBIX_DOMAINS + [d for d in user if d not in ignored]
    mapped = parse_maps(args.map)
    resolver = Resolver(args.upstream, allowed, mapped, args.log_all, args.quiet)
    try:
        udp = ThreadingUDP((args.bind, args.port), UDPHandler)
        tcp = ThreadingTCP((args.bind, args.port), TCPHandler)
    except PermissionError:
        sys.exit("Permission denied for port %d: run as administrator (Windows) or with sudo (macOS/Linux)." % args.port)
    except OSError as e:
        sys.exit("Cannot listen on port %d: %s\nIs another DNS server already running on this PC?" % (args.port, e))
    udp.resolver = tcp.resolver = resolver
    for srv in (udp, tcp):
        threading.Thread(target=srv.serve_forever, daemon=True).start()

    print("Nubix protective DNS is running (allowlist mode: everything not listed is blocked).")
    print("  On the PS5 set Primary DNS AND Secondary DNS to:  %s" % lan_ip())
    print("  Allowed for Nubix: %s" % ", ".join(NUBIX_DOMAINS))
    extra = [d for d in user if d not in ignored]
    print("  Allowed by you:    %s" % (", ".join(extra) if extra else "(none; add domains to %s)" % args.allowlist))
    if ignored:
        print("  Ignored (Sony domains are always blocked): %s" % ", ".join(ignored))
    if mapped:
        print("  Fixed answers:     %s" % ", ".join("%s=%s" % (h, socket.inet_ntoa(ip)) for h, ip in mapped.items()))
    print("  Allowed queries go to %s. Press Ctrl+C to stop." % args.upstream)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        c = resolver.counts
        print("\nStopped. %d allowed, %d blocked, %d mapped." % (c["ALLOW"], c["BLOCK"], c["MAP"]))
        print("Without this PC the PS5 has no DNS. Do not switch it back to automatic DNS while jailbroken.")


if __name__ == "__main__":
    main()
