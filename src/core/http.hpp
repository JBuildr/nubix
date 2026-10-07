// Nubix — minimal blocking HTTPS client (libcurl + OpenSSL).
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace xc {

struct HttpResponse {
    long status = 0;                             // HTTP status code (0 on transport error)
    std::string body;                            // response body
    std::map<std::string, std::string> headers;  // response headers, keys lower-cased
    std::string error;                           // transport error (curl), empty on success
    // True when the transfer succeeded and the status is 2xx.
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

class Http {
public:
    // Call once at startup (curl_global_init). caBundlePath is used as CURLOPT_CAINFO for
    // every request (assets/cacert.pem); empty = curl's built-in default.
    static void globalInit(const std::string& caBundlePath);

    // Call once at shutdown (curl_global_cleanup).
    static void globalCleanup();

    // Perform a blocking request. method: GET/POST/PUT/DELETE. headers: "Name: value" lines.
    // Follows redirects, accepts gzip, thread-safe (one easy handle per call).
    static HttpResponse request(const std::string& method, const std::string& url,
                                const std::vector<std::string>& headers, const std::string& body = "",
                                long timeoutSec = 20);

    // Optional "X-Forwarded-For: <ip>" added to every request (geo/region override used by
    // greenlight/XStreaming for unsupported countries). Empty = off. Thread-safe.
    static void setForwardedFor(const std::string& ip);

    // Per-thread cancellation (green-nx style): while a flag is installed on the calling thread,
    // requests made on that thread fail fast with error "aborted" once *flag becomes true
    // (checked before the transfer and from curl's progress callback, i.e. within ~1 s).
    // nullptr removes it. Returns the previously installed flag. The flag must outlive its use.
    static const std::atomic<bool>* setThreadAbortFlag(const std::atomic<bool>* flag);
    // Flag currently installed on the calling thread (nullptr = none). Lets helper threads
    // spawned for one job inherit its cancellation.
    static const std::atomic<bool>* threadAbortFlag();

    // RAII: install `flag` (may be nullptr to temporarily disable cancellation, e.g. for a
    // mandatory cleanup DELETE) for the current scope, restoring the previous one afterwards.
    class AbortScope {
    public:
        explicit AbortScope(const std::atomic<bool>* flag) : prev_(setThreadAbortFlag(flag)) {}
        ~AbortScope() { setThreadAbortFlag(prev_); }
        AbortScope(const AbortScope&) = delete;
        AbortScope& operator=(const AbortScope&) = delete;

    private:
        const std::atomic<bool>* prev_;
    };

    // Percent-encode a string for application/x-www-form-urlencoded bodies / query strings.
    static std::string urlEncode(const std::string& s);
};

}  // namespace xc
