// Nubix — cloud title library and xHome console list.
// Ported from green-nx src/core/catalog.cpp (GPL-3.0, (c) green-nx authors).
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "core/catalog.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <cctype>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/gssv.hpp"
#include "core/http.hpp"
#include "core/log.hpp"

using nlohmann::json;

namespace xc {

namespace {

// displaycatalog accepts comma-separated bigIds; the Details template runs to ~53 KB per
// product, so 20 already means a ~1 MB response to hold in memory (green-nx).
constexpr size_t kHydrateBatch = 20;
constexpr int kHydrateWorkers = 4;

// One /v2/titles entry with the full green-nx access breakdown.
struct Entry {
    Title t;
    bool subscription = false;     // offered under some Game Pass tier
    bool inSubscription = false;   // ... a tier this account actually holds
    bool adSupported = false;      // F2P program, no subscription needed
    bool adGranted = false;        // userPrograms holds F2P for this title
    bool adPlayable = false;       // the xgpuwebf2p offering will stream it
};

std::string fold(std::string id) {
    for (char& c : id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return id;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string str(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string snippet(const std::string& body) {
    std::string s = body.substr(0, 300);
    for (char& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return s;
}

std::string httpFailure(const char* label, const HttpResponse& r) {
    if (!r.error.empty()) return std::string(label) + ": " + r.error;
    return std::string(label) + ": HTTP " + std::to_string(r.status) + (r.body.empty() ? "" : ": " + snippet(r.body));
}

// GET {base}/v2/titles with the given offering token (green-nx fetch_catalog).
bool fetchOffering(const std::string& base, const std::string& token, const Settings& settings,
                   std::vector<Entry>& out, std::string& err, bool& authRejected) {
    out.clear();
    std::string url = base;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/v2/titles";
    HttpResponse r = Http::request("GET", url, gssv::requestHeaders(token, settings.resolution), "", 30);
    if (!r.ok()) {
        authRejected = r.error.empty() && gssv::isAuthRejection(r.status, r.body);
        err = httpFailure("titles", r);
        return false;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded()) {
        err = "titles: invalid JSON";
        return false;
    }
    const json* results = nullptr;
    if (parsed.is_array()) results = &parsed;
    else if (parsed.is_object() && parsed.contains("results") && parsed["results"].is_array())
        results = &parsed["results"];
    if (!results) {
        err = "titles: unexpected response shape";
        return false;
    }

    // Diagnostics: which program / subscription codenames this account and catalog use, so
    // filter problems ("Game Pass shows nothing") can be read straight from the log.
    std::map<std::string, int> programCounts, subCounts, userProgramCounts;
    int entitled = 0, entitledWithSubProgram = 0;
    for (const json& item : *results) {
        Entry e;
        e.t.titleId = str(item, "titleId");
        if (e.t.titleId.empty()) continue;
        const json details = item.is_object() ? item.value("details", json::object()) : json::object();
        if (!details.is_object()) continue;
        e.t.productId = str(details, "productId");
        auto ent = details.find("hasEntitlement");
        e.t.hasEntitlement = ent != details.end() && ent->is_boolean() && ent->get<bool>();
        for (const char* key : {"maxSessionLengthInSeconds", "maxGameplayTimeInSeconds"}) {
            auto it = details.find(key);
            if (it != details.end() && it->is_number()) {
                e.t.maxSessionSeconds = static_cast<int>(it->get<double>());
                break;
            }
        }

        // "programs" lists what the title is offered under, "userSubscriptions" what the account
        // holds; their intersection is Game Pass access. Subscription programs are backend
        // codenames that change over time, so anything not a known non-subscription program
        // counts as one (green-nx).
        const json programs = details.value("programs", json::array());
        const json subs = details.value("userSubscriptions", json::array());
        if (programs.is_array()) {
            for (const json& program : programs) {
                if (!program.is_string()) continue;
                const std::string name = program.get<std::string>();
                if (name == "F2P") e.adSupported = true;
                else if (name != "BYOG" && name != "EARLYACCESS") e.subscription = true;
                if (subs.is_array())
                    for (const json& sub : subs)
                        if (program == sub) e.inSubscription = true;
            }
        }
        // "userPrograms" = the program(s) through which THIS account may play the title. F2P means
        // ad-supported; any other non-purchase program means the subscription covers it. This is
        // the reliable signal: the account's own subscription code (e.g. XGPCORE for Game Pass
        // Essential) need not appear in "programs" at all (observed on an Essential account:
        // userSubscriptions {XGPCORE}, userPrograms {EUROPA, F2P}).
        const json userPrograms = details.value("userPrograms", json::array());
        if (userPrograms.is_array())
            for (const json& p : userPrograms) {
                if (!p.is_string()) continue;
                const std::string name = p.get<std::string>();
                if (name == "F2P") e.adGranted = true;
                else if (name != "BYOG" && name != "EARLYACCESS") e.inSubscription = true;
            }

        for (const json* arr : {&programs, &subs, &userPrograms}) {
            if (!arr->is_array()) continue;
            auto& counts = arr == &programs ? programCounts : arr == &subs ? subCounts : userProgramCounts;
            for (const json& v : *arr)
                if (v.is_string()) ++counts[v.get<std::string>()];
        }
        if (e.t.hasEntitlement) {
            ++entitled;
            if (e.subscription) ++entitledWithSubProgram;
        }
        out.push_back(std::move(e));
    }
    auto fmt = [](const std::map<std::string, int>& m) {
        std::string r;
        for (const auto& kv : m) r += (r.empty() ? "" : ", ") + kv.first + "=" + std::to_string(kv.second);
        return r.empty() ? std::string("(none)") : r;
    };
    XC_LOGI("catalog: %s: %zu titles, programs {%s}", base.c_str(), out.size(), fmt(programCounts).c_str());
    XC_LOGI("catalog: %s: userSubscriptions {%s}, userPrograms {%s}", base.c_str(), fmt(subCounts).c_str(),
            fmt(userProgramCounts).c_str());
    XC_LOGI("catalog: %s: %d entitled (%d of them also in a subscription program)", base.c_str(), entitled,
            entitledWithSubProgram);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.t.titleId < b.t.titleId; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Entry& a, const Entry& b) { return a.t.titleId == b.t.titleId; }),
              out.end());
    return true;
}

// Fold the ad-supported offering's catalog into the cloud one (green-nx merge_ad_supported).
void mergeAdSupported(std::vector<Entry>& catalog, const std::vector<Entry>& ads) {
    std::unordered_map<std::string, size_t> byTitle;
    for (size_t i = 0; i < catalog.size(); ++i) byTitle[catalog[i].t.titleId] = i;

    std::vector<Entry> extras;
    for (const Entry& ad : ads) {
        // Everything the offering returns is ad-supported by definition, but only what the
        // account is entitled to there will actually start.
        if (!ad.t.hasEntitlement && !ad.inSubscription && !ad.adGranted) continue;
        auto found = byTitle.find(ad.t.titleId);
        if (found == byTitle.end()) {
            Entry e = ad;
            e.adSupported = true;
            e.adPlayable = true;
            // The offering only answers for itself; ownership / plan coverage is the cloud
            // catalog's business, and it does not list this title at all.
            e.t.hasEntitlement = false;
            e.inSubscription = false;
            extras.push_back(std::move(e));
            continue;
        }
        Entry& e = catalog[found->second];
        e.adSupported = true;
        e.adPlayable = true;
        e.adGranted = ad.adGranted;
        if (ad.t.maxSessionSeconds > 0) e.t.maxSessionSeconds = ad.t.maxSessionSeconds;
        if (e.t.productId.empty()) e.t.productId = ad.t.productId;
    }
    catalog.insert(catalog.end(), std::make_move_iterator(extras.begin()), std::make_move_iterator(extras.end()));
}

std::string marketFromLocale(const std::string& locale) {
    size_t dash = locale.find_first_of("-_");
    if (dash == std::string::npos || dash + 1 >= locale.size()) return "US";
    return fold(locale.substr(dash + 1));
}

std::string absoluteImageUrl(const std::string& uri) {
    if (uri.empty()) return uri;
    if (uri.rfind("//", 0) == 0) return "https:" + uri;
    if (uri.rfind("http://", 0) == 0) return "https://" + uri.substr(7);
    return uri;
}


// ---- disk caches (title list, box art) ---------------------------------------------------------
// Bump to invalidate caches written by older builds (2: catalog diagnostics in the log,
// 3: Game Pass flag from userPrograms).
constexpr int kTitleCacheVersion = 3;
std::mutex g_cacheMu;
std::string g_cacheDir;

std::string cacheDir() {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    return g_cacheDir;
}

std::string fnv1aHex(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) mkdirs(path.substr(0, slash));
    const std::string tmp = path + ".tmp" + std::to_string(reinterpret_cast<uintptr_t>(&data) & 0xffff);
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// Keep the box-art cache bounded (~600 posters ≈ 30 MB): wipe it when it grows past that.
void pruneImageCache(const std::string& dir) {
    static std::atomic<int> sinceCheck{1000};
    if (++sinceCheck < 50) return;
    sinceCheck = 0;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    while (dirent* e = ::readdir(d))
        if (e->d_name[0] != '.') files.push_back(dir + "/" + e->d_name);
    ::closedir(d);
    if (files.size() <= 600) return;
    XC_LOGI("catalog: pruning box-art cache (%zu files)", files.size());
    for (const auto& f : files) std::remove(f.c_str());
}

std::string titleCachePath(const Tokens& tk, const Settings& st) {
    const std::string dir = cacheDir();
    if (dir.empty()) return "";
    const std::string account = !tk.uhs.empty() ? tk.uhs : tk.gamertag;
    if (account.empty()) return "";
    return dir + "/titles-" +
           fnv1aHex(account + "|" + tk.xcloudBase + "|" + tk.xcloudF2pBase + "|" + st.locale) + ".json";
}

bool loadTitleCache(const std::string& path, long maxAgeSec, std::vector<Title>& out) {
    std::vector<uint8_t> raw;
    if (!readFile(path, raw) || raw.empty()) return false;
    json j = json::parse(raw.begin(), raw.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("titles") || !j["titles"].is_array()) return false;
    if (j.value("version", 0) != kTitleCacheVersion) return false;  // older format: refetch
    const int64_t savedAt = j.value("savedAt", int64_t(0));
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (maxAgeSec >= 0 && (savedAt > now || now - savedAt > maxAgeSec)) return false;
    std::vector<Title> list;
    for (const json& e : j["titles"]) {
        if (!e.is_object()) continue;
        Title t;
        t.titleId = str(e, "titleId");
        if (t.titleId.empty()) continue;
        t.productId = str(e, "productId");
        t.name = str(e, "name");
        t.publisher = str(e, "publisher");
        t.imageUrl = str(e, "imageUrl");
        auto flag = [&](const char* k) { return e.contains(k) && e[k].is_boolean() && e[k].get<bool>(); };
        t.hasEntitlement = flag("hasEntitlement");
        t.f2p = flag("f2p");
        t.gamePass = flag("gamePass");
        t.f2pOnly = flag("f2pOnly");
        if (e.contains("maxSessionSeconds") && e["maxSessionSeconds"].is_number_integer())
            t.maxSessionSeconds = e["maxSessionSeconds"].get<int>();
        list.push_back(std::move(t));
    }
    if (list.empty()) return false;
    out = std::move(list);
    return true;
}

void saveTitleCache(const std::string& path, const std::vector<Title>& titles) {
    json arr = json::array();
    for (const Title& t : titles)
        arr.push_back({{"titleId", t.titleId},
                       {"productId", t.productId},
                       {"name", t.name},
                       {"publisher", t.publisher},
                       {"imageUrl", t.imageUrl},
                       {"hasEntitlement", t.hasEntitlement},
                       {"f2p", t.f2p},
                       {"gamePass", t.gamePass},
                       {"f2pOnly", t.f2pOnly},
                       {"maxSessionSeconds", t.maxSessionSeconds}});
    json j = {{"version", kTitleCacheVersion}, {"savedAt", static_cast<int64_t>(std::time(nullptr))}, {"titles", std::move(arr)}};
    if (!writeFileAtomic(path, j.dump())) XC_LOGW("catalog: could not write %s", path.c_str());
}

}  // namespace

Catalog::Catalog(Config& cfg) : cfg_(cfg) {}

bool Catalog::fetchCloudTitles(std::vector<Title>& out, std::string& err, bool* authRejected, bool forceRefresh) {
    out.clear();
    if (authRejected) *authRejected = false;
    // Snapshots: the UI thread may change settings / Auth may publish new tokens meanwhile.
    const Tokens tk = cfg_.tokens();
    const Settings st = cfg_.settings();
    const std::string cacheFile = titleCachePath(tk, st);
    if (!forceRefresh && !cacheFile.empty() && loadTitleCache(cacheFile, kTitleCacheTtlSec, out)) {
        XC_LOGI("catalog: %zu titles from cache", out.size());
        return true;
    }
    bool rejected = false;
    if (fetchCloudTitlesNetwork(tk, st, out, err, rejected)) {
        if (!cacheFile.empty() && !rejected) saveTitleCache(cacheFile, out);
        if (authRejected && rejected) *authRejected = true;
        return true;
    }
    if (authRejected) *authRejected = rejected;
    if (!rejected && !cacheFile.empty() && loadTitleCache(cacheFile, -1, out)) {
        XC_LOGW("catalog: network fetch failed (%s); showing the cached library", err.c_str());
        err.clear();
        return true;
    }
    return false;
}

bool Catalog::fetchCloudTitlesNetwork(const Tokens& tk, const Settings& st, std::vector<Title>& out,
                                      std::string& err, bool& authRejectedOut) {
    out.clear();
    authRejectedOut = false;
    bool rejectedLocal = false;
    bool* authRejected = &rejectedLocal;
    const bool haveCloud = !tk.xcloudGs.empty() && !tk.xcloudBase.empty();
    const bool haveF2p = !tk.xcloudF2pGs.empty() && !tk.xcloudF2pBase.empty();
    if (!haveCloud && !haveF2p) {
        err = "no xCloud offering available for this account";
        return false;
    }

    std::vector<Entry> cloud, ads;
    std::string cloudErr, f2pErr;
    bool cloudOk = false, f2pOk = false, cloudAuth = false, f2pAuth = false;
    // Both offerings in parallel: each /v2/titles is ~2400 entries.
    const std::atomic<bool>* abortFlag = Http::threadAbortFlag();
    std::thread f2pThread;
    if (haveF2p) {
        try {
            f2pThread = std::thread([&] {
                Http::AbortScope abort(abortFlag);
                f2pOk = fetchOffering(tk.xcloudF2pBase, tk.xcloudF2pGs, st, ads, f2pErr, f2pAuth);
            });
        } catch (const std::exception& e) {  // thread limit: fetch it on this thread instead
            XC_LOGW("catalog: cannot start a fetch thread (%s)", e.what());
            f2pOk = fetchOffering(tk.xcloudF2pBase, tk.xcloudF2pGs, st, ads, f2pErr, f2pAuth);
        }
    }
    if (haveCloud) cloudOk = fetchOffering(tk.xcloudBase, tk.xcloudGs, st, cloud, cloudErr, cloudAuth);
    if (f2pThread.joinable()) f2pThread.join();

    if (haveCloud && !cloudOk) XC_LOGW("catalog: xgpuweb %s", cloudErr.c_str());
    if (haveF2p && !f2pOk) XC_LOGW("catalog: xgpuwebf2p %s", f2pErr.c_str());
    if (!cloudOk && !f2pOk) {
        err = !cloudErr.empty() ? cloudErr : f2pErr;
        authRejectedOut = cloudAuth || f2pAuth;
        return false;
    }
    // One offering refused the token while the other answered: still a stale token.
    if (authRejected && (cloudAuth || f2pAuth)) *authRejected = true;
    XC_LOGI("catalog: %zu cloud + %zu f2p titles", cloud.size(), ads.size());
    if (f2pOk) mergeAdSupported(cloud, ads);

    // The library lists what this account can actually start (green-nx Game::playable()).
    for (Entry& e : cloud) {
        const bool playable = e.t.hasEntitlement || e.inSubscription || e.adPlayable;
        if (!playable) continue;
        e.t.gamePass = e.inSubscription;
        e.t.f2p = e.adPlayable;
        e.t.f2pOnly = e.adPlayable && !e.t.hasEntitlement && !e.inSubscription;
        out.push_back(std::move(e.t));
    }

    hydrate(out);
    for (Title& t : out)
        if (t.name.empty()) t.name = t.titleId;
    std::sort(out.begin(), out.end(), [](const Title& a, const Title& b) {
        const std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.titleId < b.titleId;
    });
    XC_LOGI("catalog: %zu playable titles", out.size());
    authRejectedOut = rejectedLocal;
    return true;
}

void Catalog::hydrate(std::vector<Title>& titles) {
    // displaycatalog echoes ProductId upper-cased whatever case the bigId was sent in, while a
    // few titles carry a lower-case productId; fold case on both sides (green-nx).
    std::unordered_map<std::string, std::vector<Title*>> byProduct;
    for (Title& t : titles)
        if (!t.productId.empty() && t.name.empty()) byProduct[fold(t.productId)].push_back(&t);
    if (byProduct.empty()) return;

    std::vector<std::string> ids;
    ids.reserve(byProduct.size());
    for (const auto& kv : byProduct) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());

    const Settings settings = cfg_.settings();
    const std::string locale = settings.locale.empty() ? std::string("en-US") : settings.locale;
    const std::string market = marketFromLocale(locale);
    const size_t batches = (ids.size() + kHydrateBatch - 1) / kHydrateBatch;
    std::atomic<size_t> next{0};
    std::atomic<size_t> failed{0};
    std::mutex mu;
    // Helper threads inherit the caller's cancellation (App shutdown).
    const std::atomic<bool>* abortFlag = Http::threadAbortFlag();

    auto worker = [&] {
        Http::AbortScope abort(abortFlag);
        for (size_t b = next++; b < batches; b = next++) {
            std::string bigIds;
            for (size_t i = b * kHydrateBatch; i < std::min((b + 1) * kHydrateBatch, ids.size()); ++i) {
                if (!bigIds.empty()) bigIds += ",";
                bigIds += ids[i];
            }
            const std::string url = "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=" +
                                    bigIds + "&market=" + Http::urlEncode(market) +
                                    "&languages=" + Http::urlEncode(locale) + "&fieldsTemplate=Details";
            HttpResponse r = Http::request("GET", url, {"Accept: application/json"}, "", 30);
            if (!r.ok()) {  // metadata is best-effort
                XC_LOGW("%s", httpFailure("displaycatalog", r).c_str());
                ++failed;
                continue;
            }
            json parsed = json::parse(r.body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                ++failed;
                continue;
            }
            const json products = parsed.value("Products", json::array());
            if (!products.is_array()) continue;
            for (const json& product : products) {
                const std::string pid = fold(str(product, "ProductId"));
                auto found = byProduct.find(pid);
                if (found == byProduct.end()) continue;
                const json localized = product.value("LocalizedProperties", json::array());
                if (!localized.is_array() || localized.empty()) continue;
                const json& props = localized.front();
                const std::string name = str(props, "ProductTitle");
                std::string publisher = str(props, "PublisherName");
                if (publisher.empty()) publisher = str(props, "DeveloperName");

                std::string poster, boxArt, tile;
                const json images = props.value("Images", json::array());
                if (images.is_array()) {
                    for (const json& image : images) {
                        const std::string purpose = str(image, "ImagePurpose");
                        if (purpose == "Poster" && poster.empty()) poster = str(image, "Uri");
                        else if (purpose == "BoxArt" && boxArt.empty()) boxArt = str(image, "Uri");
                        else if (purpose == "Tile" && tile.empty()) tile = str(image, "Uri");
                    }
                }
                const std::string& uri = !poster.empty() ? poster : (!boxArt.empty() ? boxArt : tile);

                std::lock_guard<std::mutex> lock(mu);
                for (Title* t : found->second) {
                    if (!name.empty()) t->name = name;
                    if (!publisher.empty()) t->publisher = publisher;
                    if (!uri.empty()) t->imageUrl = absoluteImageUrl(uri);
                }
            }
        }
    };

    const int n = static_cast<int>(std::min<size_t>(kHydrateWorkers, batches));
    std::vector<std::thread> pool;
    for (int i = 1; i < n; ++i) {
        try {
            pool.emplace_back(worker);
        } catch (const std::exception& e) {  // thread limit: the remaining workers share the load
            XC_LOGW("catalog: hydrate thread %d not started (%s)", i, e.what());
            break;
        }
    }
    worker();
    for (auto& th : pool) th.join();
    XC_LOGI("catalog: hydrated %zu products in %zu batches (%zu failed)", ids.size(), batches, failed.load());
}

bool Catalog::fetchConsoles(std::vector<Console>& out, std::string& err, bool* authRejected) {
    out.clear();
    if (authRejected) *authRejected = false;
    const Tokens tk = cfg_.tokens();
    if (tk.xhomeGs.empty() || tk.xhomeBase.empty()) {
        err = "remote play (xHome) is not available for this account";
        return false;
    }
    std::string url = tk.xhomeBase;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/v6/servers/home?mr=50";
    // xhome: always the android/720 fingerprint (green-nx kDeviceInfo for the console list).
    HttpResponse r = Http::request("GET", url, gssv::requestHeaders(tk.xhomeGs, gssv::deviceTierFor(true, "")), "", 20);
    if (!r.ok()) {
        if (authRejected) *authRejected = r.error.empty() && gssv::isAuthRejection(r.status, r.body);
        err = httpFailure("console list", r);
        return false;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded()) {
        err = "console list: invalid JSON";
        return false;
    }
    const json* results = nullptr;
    if (parsed.is_array()) results = &parsed;
    else if (parsed.is_object() && parsed.contains("results") && parsed["results"].is_array())
        results = &parsed["results"];
    if (!results) return true;  // no consoles
    for (const json& entry : *results) {
        Console c;
        c.serverId = str(entry, "serverId");
        if (c.serverId.empty()) continue;
        c.name = str(entry, "deviceName");
        if (c.name.empty()) c.name = str(entry, "serverName");
        if (c.name.empty()) c.name = c.serverId;
        c.powerState = str(entry, "powerState");
        c.consoleType = str(entry, "consoleType");
        out.push_back(std::move(c));
    }
    XC_LOGI("catalog: %zu consoles", out.size());
    return true;
}

bool Catalog::fetchImage(const std::string& url, std::vector<uint8_t>& bytes) {
    bytes.clear();
    if (url.empty()) return false;
    const std::string dir = cacheDir();
    const std::string cached = dir.empty() ? std::string() : dir + "/img/" + fnv1aHex(url) + ".bin";
    if (!cached.empty() && readFile(cached, bytes) && !bytes.empty()) return true;
    bytes.clear();
    std::string u = absoluteImageUrl(url);
    // store-images.s-microsoft.com scales server-side; ask for a grid-sized poster (green-nx h=300)
    // instead of the multi-megabyte original.
    if (u.find("store-images.s-microsoft.com") != std::string::npos && u.find('?') == std::string::npos)
        u += "?h=300";
    HttpResponse r = Http::request("GET", u, {"Accept: image/png,image/jpeg,image/*;q=0.8"}, "", 20);
    if (!r.ok() || r.body.empty()) {
        XC_LOGD("%s", httpFailure("image", r).c_str());
        return false;
    }
    bytes.assign(r.body.begin(), r.body.end());
    if (!cached.empty()) {
        pruneImageCache(dir + "/img");
        writeFileAtomic(cached, r.body);
    }
    return true;
}

void Catalog::setCacheDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_cacheMu);
    g_cacheDir = dir;
}

}  // namespace xc
