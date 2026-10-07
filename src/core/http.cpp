// Nubix — minimal blocking HTTPS client (libcurl + OpenSSL).
// Request setup follows green-nx src/core/http.cpp (GPL-3.0, error-buffer diagnostics, one TLS
// connection per transfer) adapted to a stateless, thread-safe API.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/http.hpp"

#include <curl/curl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include "core/http_internal.hpp"
#include "core/log.hpp"
#include "core/ratelimit.hpp"

namespace xc {
namespace {

std::mutex g_mutex;           // guards g_caBundle, g_forwardedFor, g_inited
std::string g_caBundle;
std::string g_forwardedFor;
bool g_inited = false;

thread_local bool t_quiet = false;
thread_local const std::atomic<bool>* t_abort = nullptr;

// CURLOPT_XFERINFOFUNCTION: non-zero aborts the transfer (CURLE_ABORTED_BY_CALLBACK).
int xferInfoCb(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* flag = static_cast<const std::atomic<bool>*>(clientp);
    return (flag && flag->load()) ? 1 : 0;
}

constexpr size_t kMaxBody = 64u * 1024u * 1024u;  // refuse absurd responses (memory on PS5)

struct Transfer {
    HttpResponse* resp;
    bool overflow = false;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* t = static_cast<Transfer*>(userdata);
    const size_t n = size * nmemb;
    if (t->resp->body.size() + n > kMaxBody) {
        t->overflow = true;
        return 0;  // aborts with CURLE_WRITE_ERROR
    }
    t->resp->body.append(ptr, n);
    return n;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

size_t headerCb(char* buf, size_t size, size_t nitems, void* userdata) {
    auto* t = static_cast<Transfer*>(userdata);
    const size_t n = size * nitems;
    std::string line(buf, n);
    // A new status line starts a new response (redirect / 100-continue): drop older headers.
    if (line.compare(0, 5, "HTTP/") == 0) {
        t->resp->headers.clear();
        return n;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return n;
    std::string key = trim(line.substr(0, colon));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string value = trim(line.substr(colon + 1));
    auto it = t->resp->headers.find(key);
    if (it == t->resp->headers.end()) t->resp->headers.emplace(std::move(key), std::move(value));
    else it->second += ", " + value;
    return n;
}

// URL without query string, for logs (queries may carry ids we do not need in log files).
std::string logUrl(const std::string& url) {
    const size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q) + "?…";
}

std::string snippet(const std::string& body) {
    std::string s = body.substr(0, 300);
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (body.size() > 300) s += "…";
    return s;
}

bool hasHeader(const std::vector<std::string>& headers, const char* name) {
    const size_t len = std::char_traits<char>::length(name);
    for (const auto& h : headers) {
        if (h.size() <= len || h[len] != ':') continue;
        bool eq = true;
        for (size_t i = 0; i < len && eq; ++i)
            eq = std::tolower(static_cast<unsigned char>(h[i])) == std::tolower(static_cast<unsigned char>(name[i]));
        if (eq) return true;
    }
    return false;
}

}  // namespace

namespace httpdetail {
QuietErrors::QuietErrors() : prev_(t_quiet) { t_quiet = true; }
QuietErrors::~QuietErrors() { t_quiet = prev_; }
}  // namespace httpdetail

void Http::globalInit(const std::string& caBundlePath) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_inited) {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK) XC_LOGE("http: curl_global_init failed: %s", curl_easy_strerror(rc));
        g_inited = true;
        const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
        if (v) XC_LOGI("http: libcurl %s (%s)", v->version, v->ssl_version ? v->ssl_version : "no TLS");
    }
    g_caBundle.clear();
    if (!caBundlePath.empty()) {
        struct stat st {};
        if (::stat(caBundlePath.c_str(), &st) == 0 && st.st_size > 0) {
            g_caBundle = caBundlePath;
            XC_LOGD("http: CA bundle %s (%lld bytes)", caBundlePath.c_str(), (long long)st.st_size);
        } else {
            XC_LOGW("http: CA bundle %s not found; using libcurl's built-in CA path", caBundlePath.c_str());
        }
    }
}

void Http::globalCleanup() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_inited) {
        curl_global_cleanup();
        g_inited = false;
    }
}

const std::atomic<bool>* Http::setThreadAbortFlag(const std::atomic<bool>* flag) {
    const std::atomic<bool>* prev = t_abort;
    t_abort = flag;
    return prev;
}

const std::atomic<bool>* Http::threadAbortFlag() { return t_abort; }

void Http::setForwardedFor(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_forwardedFor = ip;
}

namespace {
HttpResponse performOnce(const std::string& method, const std::string& url,
                         const std::vector<std::string>& headers, const std::string& body, long timeoutSec) {
    HttpResponse resp;
    std::string caBundle, forwardedFor;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_inited) {
            // Be forgiving if a caller forgot globalInit(); curl_global_init is not thread-safe
            // on old libcurl, hence under the mutex.
            curl_global_init(CURL_GLOBAL_DEFAULT);
            g_inited = true;
        }
        caBundle = g_caBundle;
        forwardedFor = g_forwardedFor;
    }

    const std::atomic<bool>* abortFlag = t_abort;
    if (abortFlag && abortFlag->load()) {
        resp.error = "aborted";
        XC_LOGD("http: %s %s skipped: aborted", method.c_str(), logUrl(url).c_str());
        return resp;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "curl_easy_init failed";
        XC_LOGE("http: %s %s: %s", method.c_str(), logUrl(url).c_str(), resp.error.c_str());
        return resp;
    }

    // curl_easy_strerror() only names the error class; the TLS backend's actual reason goes to
    // the error buffer (green-nx).
    char errBuf[CURL_ERROR_SIZE] = {0};
    Transfer xfer{&resp};

    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errBuf);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // required for multi-threaded use
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &xfer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &xfer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // all built-in encodings (gzip, deflate, ...)
    const long total = timeoutSec > 0 ? timeoutSec : 20L;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, total);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(total, 10L));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    // One TLS connection per transfer, closed afterwards (green-nx: consoles have few TLS sessions).
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    if (!caBundle.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caBundle.c_str());
    if (abortFlag) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferInfoCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(abortFlag));
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "POST") {
        // Always send a body (possibly empty, Content-Length: 0) — gssv keepalive posts nothing.
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty() || method == "PUT" || method == "PATCH") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
    }

    curl_slist* list = nullptr;
    if (!forwardedFor.empty()) list = curl_slist_append(list, ("X-Forwarded-For: " + forwardedFor).c_str());
    if (!hasHeader(headers, "User-Agent"))
        list = curl_slist_append(list, "User-Agent: nubix/" XC_VERSION);
    if (method == "POST" && !hasHeader(headers, "Expect"))
        list = curl_slist_append(list, "Expect:");  // no 100-continue round trip
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

    const auto t0 = std::chrono::steady_clock::now();
    const CURLcode code = curl_easy_perform(curl);
    const long ms = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

    if (code == CURLE_ABORTED_BY_CALLBACK && abortFlag && abortFlag->load()) {
        resp.error = "aborted";
        resp.status = 0;
        XC_LOGD("http: %s %s aborted after %ld ms", method.c_str(), logUrl(url).c_str(), ms);
    } else if (code != CURLE_OK) {
        std::string msg = curl_easy_strerror(code);
        if (xfer.overflow) msg += " [response too large]";
        else if (errBuf[0]) msg += std::string(" [") + errBuf + "]";
        if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR ||
            code == CURLE_SSL_CACERT_BADFILE) {
            // Name the address actually reached: a DNS block presenting its own certificate reads
            // as "via 127.0.0.1" instead of a guessing game (green-nx #58).
            char* ip = nullptr;
            curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
            if (ip && *ip) msg += std::string(" via ") + ip;
        }
        long osErrno = 0;
        curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &osErrno);
        if (osErrno) msg += " errno=" + std::to_string(osErrno);
        resp.error = msg;
        resp.status = 0;
        XC_LOGW("http: %s %s failed after %ld ms: %s", method.c_str(), logUrl(url).c_str(), ms, msg.c_str());
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
        if (resp.status >= 200 && resp.status < 300) {
            XC_LOGD("http: %s %s -> %ld (%zu B, %ld ms)", method.c_str(), logUrl(url).c_str(), resp.status,
                    resp.body.size(), ms);
        } else if (t_quiet) {
            XC_LOGD("http: %s %s -> %ld (%ld ms): %s", method.c_str(), logUrl(url).c_str(), resp.status, ms,
                    snippet(resp.body).c_str());
        } else {
            XC_LOGW("http: %s %s -> HTTP %ld (%ld ms): %s", method.c_str(), logUrl(url).c_str(), resp.status, ms,
                    snippet(resp.body).c_str());
        }
    }

    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return resp;
}

// Abortable sleep in 100 ms slices. False when the thread's abort flag was raised.
bool pacedSleep(int64_t ms, const std::atomic<bool>* abortFlag) {
    const int64_t until = steadyNowMs() + ms;
    for (;;) {
        if (abortFlag && abortFlag->load()) return false;
        const int64_t left = until - steadyNowMs();
        if (left <= 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(left, 100)));
    }
}
}  // namespace

// Pacing wrapper around performOnce (see ratelimit.hpp):
//  * every request takes a token from its host's bucket (waits briefly when the bucket is empty);
//  * a host answering 429 (or 503 + Retry-After) is put into cooldown; requests to it wait the
//    cooldown out when it is short, or fail fast ("rate limited") when it is long, instead of
//    hammering the server;
//  * idempotent requests (GET/DELETE) are retried after a short Retry-After, at most twice.
//    POSTs (session start, SDP, ICE, keepalive, token polls) are never repeated here.
HttpResponse Http::request(const std::string& method, const std::string& url,
                           const std::vector<std::string>& headers, const std::string& body,
                           long timeoutSec) {
    constexpr int64_t kMaxCooldownWaitMs = 15000;  // wait out shorter cooldowns, fail on longer ones
    constexpr int kMaxRetries = 2;
    const std::atomic<bool>* abortFlag = t_abort;
    const std::string host = urlHost(url);
    const bool idempotent = method == "GET" || method == "DELETE" || method == "HEAD";
    RateLimiter& limiter = RateLimiter::global();

    for (int attempt = 0;; ++attempt) {
        for (;;) {
            bool cooldown = false;
            const int64_t wait = limiter.reserve(host, steadyNowMs(), &cooldown);
            if (wait <= 0) break;
            if (cooldown && wait > kMaxCooldownWaitMs) {
                HttpResponse r;
                const int sec = static_cast<int>((wait + 999) / 1000);
                r.status = 429;
                r.headers["retry-after"] = std::to_string(sec);
                r.error = "rate limited by the server, retry in " + std::to_string(sec) + " s";
                XC_LOGW("http: %s %s not sent: %s is cooling down for %d s", method.c_str(), logUrl(url).c_str(),
                        host.c_str(), sec);
                return r;
            }
            if (cooldown)
                XC_LOGI("http: waiting %lld ms for %s (rate limit cooldown)", static_cast<long long>(wait), host.c_str());
            if (!pacedSleep(wait, abortFlag)) {
                HttpResponse r;
                r.error = "aborted";
                return r;
            }
        }

        HttpResponse resp = performOnce(method, url, headers, body, timeoutSec);
        int retryAfter = -1;
        auto it = resp.headers.find("retry-after");
        if (it != resp.headers.end()) retryAfter = parseRetryAfter(it->second, static_cast<int64_t>(std::time(nullptr)));
        limiter.onResponse(host, resp.status, retryAfter, steadyNowMs());

        const bool limited = resp.status == 429 || (resp.status == 503 && retryAfter >= 0);
        if (!limited) return resp;
        const int64_t cool = limiter.cooldownRemaining(host, steadyNowMs());
        XC_LOGW("http: %s rate limited (HTTP %ld), backing off %lld ms", host.c_str(), resp.status,
                static_cast<long long>(cool));
        if (!idempotent || attempt >= kMaxRetries || cool > kMaxCooldownWaitMs) return resp;
        // The cooldown loop above waits before the retry.
    }
}

std::string Http::urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

}  // namespace xc
