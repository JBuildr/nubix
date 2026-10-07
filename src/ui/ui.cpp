// Nubix — drawing helpers (SDL_Renderer + SDL2_ttf).
//
// Rendering notes: on PS5 the SDL port renders in software into a 1920x1080 framebuffer, so
// everything here is designed for 1:1 blits: text is rasterised at the output pixel size
// (logical size * output scale), rounded corners come from small cached anti-aliased circle
// textures, and box art is pre-scaled to the exact tile size on a loader thread.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/ui.hpp"


#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/catalog.hpp"
#include "core/http.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "ui/image.hpp"
#include "ui/qr.hpp"

namespace xc {
namespace {

constexpr uint64_t kTextEvictFrames = 240;     // drop text textures unused for ~4 s
constexpr size_t kTextCacheSoftCap = 1200;
constexpr size_t kImageCacheCap = 120;         // box-art textures kept (~360 KB each at 1080p)
constexpr uint64_t kImageStaleFrames = 90;     // queued requests not refreshed for this long are dropped
constexpr uint64_t kImageRetryFrames = 60 * 30;
constexpr int kLoaderThreads = 2;
constexpr int kMaxUploadsPerFrame = 6;
constexpr double kPi = 3.14159265358979323846;

struct TextEntry {
    SDL_Texture* tex = nullptr;
    int pw = 0, ph = 0;  // pixel size
    uint64_t lastUsed = 0;
};

struct ImageEntry {
    enum State { Queued, Ready, Failed } state = Queued;
    SDL_Texture* tex = nullptr;
    uint64_t lastUsed = 0;
    uint64_t failedAt = 0;
};

// Shared between the main thread and the box-art loader threads (kept alive by shared_ptr so
// shutdown never races a thread that is finishing an HTTP request).
struct LoaderShared {
    struct Job {
        std::string url;
        int pw = 0, ph = 0;
        uint64_t wanted = 0;  // frame of the last request
    };
    struct Done {
        std::string key;
        image::Rgba rgba;
        enum { Ok, Failed, Dropped } result = Failed;
    };
    std::mutex m;
    std::condition_variable cv;
    std::map<std::string, Job> jobs;  // key -> job (not yet started)
    std::deque<Done> done;
    int inFlight = 0;
    bool stop = false;
    // Set at shutdown together with stop: an image request in flight aborts within ~1 s
    // (Http per-thread abort flag), so the loader threads can always be joined.
    std::atomic<bool> abort{false};
    std::atomic<uint64_t> frame{0};
};

void loaderThread(std::shared_ptr<LoaderShared> sh) {
    Http::AbortScope abortOnShutdown(&sh->abort);
    for (;;) {
        std::string key;
        LoaderShared::Job job;
        {
            std::unique_lock<std::mutex> lk(sh->m);
            sh->cv.wait(lk, [&] { return sh->stop || !sh->jobs.empty(); });
            if (sh->stop) return;
            // Most recently requested job first (visible tiles beat stale ones); drop stale.
            const uint64_t now = sh->frame.load();
            auto best = sh->jobs.end();
            for (auto it = sh->jobs.begin(); it != sh->jobs.end();) {
                if (it->second.wanted + kImageStaleFrames < now) {
                    LoaderShared::Done d;
                    d.key = it->first;
                    d.result = LoaderShared::Done::Dropped;
                    sh->done.push_back(std::move(d));
                    it = sh->jobs.erase(it);
                    continue;
                }
                if (best == sh->jobs.end() || it->second.wanted > best->second.wanted) best = it;
                ++it;
            }
            if (best == sh->jobs.end()) continue;
            key = best->first;
            job = best->second;
            sh->jobs.erase(best);
            ++sh->inFlight;
        }
        LoaderShared::Done d;
        d.key = key;
        std::vector<uint8_t> bytes;
        image::Rgba full;
        if (Catalog::fetchImage(job.url, bytes) && image::decode(bytes.data(), bytes.size(), full)) {
            image::resize(full, job.pw, job.ph, true, d.rgba);
            d.result = d.rgba.empty() ? LoaderShared::Done::Failed : LoaderShared::Done::Ok;
        } else {
            XC_LOGD("ui: image load failed: %s", job.url.c_str());
            d.result = LoaderShared::Done::Failed;
        }
        std::lock_guard<std::mutex> lk(sh->m);
        --sh->inFlight;
        sh->done.push_back(std::move(d));
    }
}

// Coverage of the pixel (px, py) by a disc of radius r centred at (r, r): 4x4 supersampling.
float discCoverage(int px, int py, float r) {
    int inside = 0;
    for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) {
            const float x = px + (sx + 0.5f) / 4.0f - r, y = py + (sy + 0.5f) / 4.0f - r;
            if (x * x + y * y <= r * r) ++inside;
        }
    return inside / 16.0f;
}

// Split UTF-8 string into code-point boundaries (for ellipsizing).
std::vector<size_t> utf8Boundaries(const std::string& s) {
    std::vector<size_t> b;
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) b.push_back(i);
    b.push_back(s.size());
    return b;
}

}  // namespace

struct Ui::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::string fontPath, boldPath;
    bool ttfInited = false;
    float scale = 1.0f;
    uint64_t frame = 0;

    std::unordered_map<int, TTF_Font*> fonts;  // key: px * 2 + bold
    std::unordered_map<std::string, TextEntry> text;
    std::unordered_map<std::string, int> widths;  // text-width cache (pixels), cleared when large
    std::unordered_map<std::string, TextEntry> qr;
    std::unordered_map<int, SDL_Texture*> discs;   // pixel radius -> filled disc
    std::unordered_map<int, SDL_Texture*> masks;   // pixel radius -> inverted quarter (corner mask)
    std::unordered_map<int64_t, SDL_Texture*> rings;  // (radius << 16 | thickness) -> ring

    std::unordered_map<std::string, ImageEntry> images;
    std::shared_ptr<LoaderShared> loader = std::make_shared<LoaderShared>();
    std::vector<std::thread> loaderThreads;

    int px(float logical) const { return std::max(1, static_cast<int>(std::lround(logical * scale))); }

    TTF_Font* font(int size, bool bold) {
        const int p = px(static_cast<float>(size));
        const int key = p * 2 + (bold ? 1 : 0);
        auto it = fonts.find(key);
        if (it != fonts.end()) return it->second;
        TTF_Font* f = TTF_OpenFont((bold ? boldPath : fontPath).c_str(), p);
        if (!f) {
            XC_LOGE("ui: TTF_OpenFont(%s, %d) failed: %s", (bold ? boldPath : fontPath).c_str(), p, TTF_GetError());
            return nullptr;
        }
        TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
        fonts[key] = f;
        return f;
    }

    TextEntry* textEntry(const std::string& s, int size, bool bold) {
        const int p = px(static_cast<float>(size));
        std::string key;
        key.reserve(s.size() + 8);
        key.push_back(bold ? 'B' : 'R');
        key += std::to_string(p);
        key.push_back('\x1f');
        key += s;
        auto it = text.find(key);
        if (it != text.end()) {
            it->second.lastUsed = frame;
            return &it->second;
        }
        TTF_Font* f = font(size, bold);
        if (!f) return nullptr;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), SDL_Color{255, 255, 255, 255});
        if (!surf) return nullptr;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
        TextEntry e;
        e.pw = surf->w;
        e.ph = surf->h;
        SDL_FreeSurface(surf);
        if (!tex) return nullptr;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        e.tex = tex;
        e.lastUsed = frame;
        return &(text[key] = e);
    }

    SDL_Texture* makeAlphaTexture(int w, int h, const std::vector<uint8_t>& alpha) {
        std::vector<uint32_t> pix(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < pix.size(); ++i) pix[i] = (static_cast<uint32_t>(alpha[i]) << 24) | 0x00FFFFFFu;
        SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t) return nullptr;
        SDL_UpdateTexture(t, nullptr, pix.data(), w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        return t;
    }

    SDL_Texture* disc(int r) {
        auto it = discs.find(r);
        if (it != discs.end()) return it->second;
        const int d = r * 2;
        std::vector<uint8_t> a(static_cast<size_t>(d) * d);
        for (int y = 0; y < d; ++y)
            for (int x = 0; x < d; ++x) a[y * d + x] = static_cast<uint8_t>(std::lround(discCoverage(x, y, r) * 255));
        return discs[r] = makeAlphaTexture(d, d, a);
    }

    // Top-left quarter of (1 - disc): paints the area outside a rounded corner.
    SDL_Texture* mask(int r) {
        auto it = masks.find(r);
        if (it != masks.end()) return it->second;
        std::vector<uint8_t> a(static_cast<size_t>(r) * r);
        for (int y = 0; y < r; ++y)
            for (int x = 0; x < r; ++x) a[y * r + x] = static_cast<uint8_t>(std::lround((1.0f - discCoverage(x, y, r)) * 255));
        return masks[r] = makeAlphaTexture(r, r, a);
    }

    SDL_Texture* ring(int r, int t) {
        const int64_t key = (static_cast<int64_t>(r) << 16) | t;
        auto it = rings.find(key);
        if (it != rings.end()) return it->second;
        const int d = r * 2;
        const float ri = std::max(0.0f, static_cast<float>(r - t));
        std::vector<uint8_t> a(static_cast<size_t>(d) * d);
        for (int y = 0; y < d; ++y)
            for (int x = 0; x < d; ++x) {
                int inO = 0, inI = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const float fx = x + (sx + 0.5f) / 4.0f - r, fy = y + (sy + 0.5f) / 4.0f - r;
                        const float dd = fx * fx + fy * fy;
                        if (dd <= static_cast<float>(r) * r) ++inO;
                        if (dd <= ri * ri) ++inI;
                    }
                a[y * d + x] = static_cast<uint8_t>(std::lround((inO - inI) / 16.0f * 255));
            }
        return rings[key] = makeAlphaTexture(d, d, a);
    }

    void setColor(SDL_Texture* t, Color c) {
        SDL_SetTextureColorMod(t, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(t, c.a);
    }

    void fill(float x, float y, float w, float h, Color c) {
        if (w <= 0 || h <= 0) return;
        SDL_SetRenderDrawBlendMode(renderer, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        SDL_FRect r{x, y, w, h};
        SDL_RenderFillRectF(renderer, &r);
    }

    // Draw the four quarters of a (2r x 2r)-pixel texture at the corners of a rectangle.
    void corners(SDL_Texture* t, int rp, float x, float y, float w, float h, float r) {
        const SDL_Rect src[4] = {{0, 0, rp, rp}, {rp, 0, rp, rp}, {0, rp, rp, rp}, {rp, rp, rp, rp}};
        const SDL_FRect dst[4] = {{x, y, r, r}, {x + w - r, y, r, r}, {x, y + h - r, r, r}, {x + w - r, y + h - r, r, r}};
        for (int i = 0; i < 4; ++i) SDL_RenderCopyF(renderer, t, &src[i], &dst[i]);
    }

    void destroyMap(std::unordered_map<std::string, TextEntry>& m) {
        for (auto& kv : m)
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        m.clear();
    }

    void drainImages() {
        std::deque<LoaderShared::Done> ready;
        {
            std::lock_guard<std::mutex> lk(loader->m);
            int n = 0;
            while (!loader->done.empty() && n < kMaxUploadsPerFrame) {
                if (loader->done.front().result == LoaderShared::Done::Ok) ++n;
                ready.push_back(std::move(loader->done.front()));
                loader->done.pop_front();
            }
        }
        for (auto& d : ready) {
            auto it = images.find(d.key);
            if (it == images.end()) continue;
            if (d.result == LoaderShared::Done::Dropped) {
                images.erase(it);
                continue;
            }
            if (d.result == LoaderShared::Done::Failed) {
                it->second.state = ImageEntry::Failed;
                it->second.failedAt = frame;
                continue;
            }
            SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, d.rgba.width,
                                               d.rgba.height);
            if (!t) {
                it->second.state = ImageEntry::Failed;
                it->second.failedAt = frame;
                continue;
            }
            SDL_UpdateTexture(t, nullptr, d.rgba.pixels.data(), d.rgba.width * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            it->second.tex = t;
            it->second.state = ImageEntry::Ready;
        }
    }

    void evict() {
        // text: unused for a while (or everything not used this frame when over the soft cap)
        const uint64_t limit = text.size() > kTextCacheSoftCap ? 1 : kTextEvictFrames;
        for (auto it = text.begin(); it != text.end();) {
            if (frame - it->second.lastUsed > limit) {
                SDL_DestroyTexture(it->second.tex);
                it = text.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = qr.begin(); it != qr.end();) {
            if (frame - it->second.lastUsed > kTextEvictFrames * 4) {
                SDL_DestroyTexture(it->second.tex);
                it = qr.erase(it);
            } else {
                ++it;
            }
        }
        // images: LRU beyond the cap
        size_t ready = 0;
        for (auto& kv : images)
            if (kv.second.state == ImageEntry::Ready) ++ready;
        if (ready > kImageCacheCap) {
            std::vector<std::pair<uint64_t, std::string>> order;
            for (auto& kv : images)
                if (kv.second.state == ImageEntry::Ready) order.emplace_back(kv.second.lastUsed, kv.first);
            std::sort(order.begin(), order.end());
            size_t toDrop = ready - kImageCacheCap;
            for (auto& o : order) {
                if (toDrop == 0 || o.first + 2 >= frame) break;
                auto it = images.find(o.second);
                SDL_DestroyTexture(it->second.tex);
                images.erase(it);
                --toDrop;
            }
        }
    }
};

Ui::Ui() : d_(new Impl) {}
Ui::~Ui() { shutdown(); }

bool Ui::init(const std::string& fontPath, const std::string& boldFontPath, const std::string& title) {
    Impl& d = *d_;
    d.fontPath = fontPath;
    d.boldPath = boldFontPath;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        XC_LOGE("ui: SDL video init failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    Uint32 flags = SDL_WINDOW_SHOWN;
    int w = 1280, h = 720;
    if (platform::isPs5()) {
        flags |= SDL_WINDOW_FULLSCREEN;
        w = kWidth;
        h = kHeight;
    } else {
        flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (const char* fs = SDL_getenv("XC_FULLSCREEN"); fs && *fs == '1') flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    d.window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!d.window) {
        XC_LOGE("ui: SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    if (!platform::isPs5()) SDL_SetWindowMinimumSize(d.window, 640, 360);
    d.renderer = SDL_CreateRenderer(d.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!d.renderer) {
        XC_LOGI("ui: no accelerated vsync renderer (%s), falling back", SDL_GetError());
        d.renderer = SDL_CreateRenderer(d.window, -1, 0);
    }
    if (!d.renderer) {
        XC_LOGE("ui: SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }
    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(d.renderer, &info) == 0) {
        XC_LOGI("ui: renderer %s (flags 0x%x, max texture %dx%d)", info.name ? info.name : "?", info.flags,
                info.max_texture_width, info.max_texture_height);
    }
    SDL_RenderSetLogicalSize(d.renderer, kWidth, kHeight);

    if (TTF_WasInit() == 0 && TTF_Init() != 0) {
        XC_LOGE("ui: TTF_Init failed: %s", TTF_GetError());
        return false;
    }
    d.ttfInited = true;
    float sx = 1, sy = 1;
    SDL_RenderGetScale(d.renderer, &sx, &sy);
    d.scale = sx > 0 ? sx : 1.0f;
    if (!d.font(32, false) || !d.font(32, true)) {
        XC_LOGE("ui: cannot load fonts %s / %s", fontPath.c_str(), boldFontPath.c_str());
        return false;
    }

    d.loader = std::make_shared<LoaderShared>();
    for (int i = 0; i < kLoaderThreads; ++i) {
        try {
            d.loaderThreads.emplace_back(loaderThread, d.loader);
        } catch (const std::exception& e) {  // thread limit: fewer loaders still work
            XC_LOGW("ui: image loader thread %d not started: %s", i, e.what());
            break;
        }
    }
    return true;
}

void Ui::shutdown() {
    if (!d_) return;
    Impl& d = *d_;
    if (!d.loaderThreads.empty()) {
        {
            std::lock_guard<std::mutex> lk(d.loader->m);
            d.loader->stop = true;
            d.loader->jobs.clear();
        }
        d.loader->abort = true;
        d.loader->cv.notify_all();
        // A thread inside an HTTP request sees the abort flag from curl's progress callback and
        // returns within ~1 s: join them all, so none outlives curl_global_cleanup / SDL_Quit.
        for (auto& t : d.loaderThreads)
            if (t.joinable()) t.join();
        d.loaderThreads.clear();
    }
    clearCache();
    for (auto& kv : d.discs) SDL_DestroyTexture(kv.second);
    for (auto& kv : d.masks) SDL_DestroyTexture(kv.second);
    for (auto& kv : d.rings) SDL_DestroyTexture(kv.second);
    d.discs.clear();
    d.masks.clear();
    d.rings.clear();
    for (auto& kv : d.fonts) TTF_CloseFont(kv.second);
    d.fonts.clear();
    if (d.ttfInited) {
        TTF_Quit();
        d.ttfInited = false;
    }
    if (d.renderer) {
        SDL_DestroyRenderer(d.renderer);
        d.renderer = nullptr;
    }
    if (d.window) {
        SDL_DestroyWindow(d.window);
        d.window = nullptr;
    }
}

SDL_Renderer* Ui::renderer() const { return d_->renderer; }
SDL_Window* Ui::window() const { return d_->window; }
uint64_t Ui::frameCount() const { return d_->frame; }
float Ui::scale() const { return d_->scale; }

void Ui::beginFrame() {
    Impl& d = *d_;
    float sx = 1, sy = 1;
    SDL_RenderGetScale(d.renderer, &sx, &sy);
    if (sx > 0.01f && std::fabs(sx - d.scale) > 0.001f) d.scale = sx;
    d.loader->frame.store(d.frame);
    d.drainImages();
    SDL_RenderSetClipRect(d.renderer, nullptr);
    SDL_SetRenderDrawBlendMode(d.renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(d.renderer, colors::Background.r, colors::Background.g, colors::Background.b, 255);
    SDL_RenderClear(d.renderer);
}

void Ui::endFrame() {
    Impl& d = *d_;
    SDL_RenderPresent(d.renderer);
    ++d.frame;
    if (d.frame % 30 == 0) d.evict();
}

int Ui::text(const std::string& s, int x, int y, int size, Color c, Align align, bool bold) {
    if (s.empty()) return 0;
    TextEntry* e = d_->textEntry(s, size, bold);
    if (!e) return 0;
    const float w = e->pw / d_->scale, h = e->ph / d_->scale;
    float dx = static_cast<float>(x);
    if (align == Align::Center) dx -= w / 2;
    else if (align == Align::Right) dx -= w;
    // Snap to output pixels so glyphs stay crisp.
    dx = std::round(dx * d_->scale) / d_->scale;
    const float dy = std::round(y * d_->scale) / d_->scale;
    d_->setColor(e->tex, c);
    SDL_FRect dst{dx, dy, w, h};
    SDL_RenderCopyF(d_->renderer, e->tex, nullptr, &dst);
    return static_cast<int>(std::lround(w));
}

int Ui::textWidth(const std::string& s, int size, bool bold) {
    if (s.empty()) return 0;
    Impl& d = *d_;
    const int p = d.px(static_cast<float>(size));
    std::string key;
    key.reserve(s.size() + 8);
    key.push_back(bold ? 'B' : 'R');
    key += std::to_string(p);
    key.push_back('\x1f');
    key += s;
    int w = 0;
    auto it = d.widths.find(key);
    if (it != d.widths.end()) {
        w = it->second;
    } else {
        TTF_Font* f = d.font(size, bold);
        if (!f) return 0;
        int h = 0;
        if (TTF_SizeUTF8(f, s.c_str(), &w, &h) != 0) return 0;
        if (d.widths.size() > 8192) d.widths.clear();
        d.widths.emplace(std::move(key), w);
    }
    return static_cast<int>(std::lround(w / d.scale));
}

int Ui::lineHeight(int size, bool bold) {
    TTF_Font* f = d_->font(size, bold);
    if (!f) return size;
    return static_cast<int>(std::lround(TTF_FontLineSkip(f) / d_->scale));
}

int Ui::textWrapped(const std::string& s, int x, int y, int maxWidth, int size, Color c, bool bold) {
    if (s.empty()) return 0;
    const int lh = lineHeight(size, bold);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        // greedy word wrap
        std::string cur;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string cand = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && textWidth(cand, size, bold) > maxWidth) {
                lines.push_back(cur);
                cur = word;
            } else {
                cur = cand;
            }
            // hard-break words wider than the line
            while (textWidth(cur, size, bold) > maxWidth && cur.size() > 1) {
                auto b = utf8Boundaries(cur);
                size_t cut = 1;
                for (size_t k = 1; k < b.size(); ++k) {
                    if (textWidth(cur.substr(0, b[k]), size, bold) > maxWidth) break;
                    cut = k;
                }
                lines.push_back(cur.substr(0, b[cut]));
                cur = cur.substr(b[cut]);
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        lines.push_back(cur);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    int yy = y;
    for (auto& l : lines) {
        text(l, x, yy, size, c, Align::Left, bold);
        yy += lh;
    }
    return yy - y;
}

int Ui::textEllipsized(const std::string& s, int x, int y, int maxWidth, int size, Color c, Align align, bool bold) {
    if (textWidth(s, size, bold) <= maxWidth) return text(s, x, y, size, c, align, bold);
    static const std::string ell = "\xE2\x80\xA6";  // …
    auto b = utf8Boundaries(s);
    // binary search the longest prefix that fits with the ellipsis
    size_t lo = 0, hi = b.size() - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (textWidth(s.substr(0, b[mid]) + ell, size, bold) <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    std::string cut = s.substr(0, b[lo]);
    while (!cut.empty() && cut.back() == ' ') cut.pop_back();
    return text(cut + ell, x, y, size, c, align, bold);
}

void Ui::rect(int x, int y, int w, int h, Color c, bool filled, int radius) {
    if (w <= 0 || h <= 0) return;
    if (!filled) {
        outline(x, y, w, h, c, 2, radius);
        return;
    }
    Impl& d = *d_;
    const float r = static_cast<float>(std::min(radius, std::min(w, h) / 2));
    if (r < 1.0f) {
        d.fill(x, y, w, h, c);
        return;
    }
    const int rp = d.px(r);
    SDL_Texture* t = d.disc(rp);
    if (!t) {
        d.fill(x, y, w, h, c);
        return;
    }
    d.setColor(t, c);
    d.corners(t, rp, x, y, w, h, r);
    d.fill(x + r, y, w - 2 * r, h, c);
    d.fill(x, y + r, r, h - 2 * r, c);
    d.fill(x + w - r, y + r, r, h - 2 * r, c);
}

void Ui::outline(int x, int y, int w, int h, Color c, int thickness, int radius) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    Impl& d = *d_;
    const float t = static_cast<float>(thickness);
    const float r = static_cast<float>(std::min(radius, std::min(w, h) / 2));
    if (r < t) {
        d.fill(x, y, w, t, c);
        d.fill(x, y + h - t, w, t, c);
        d.fill(x, y + t, t, h - 2 * t, c);
        d.fill(x + w - t, y + t, t, h - 2 * t, c);
        return;
    }
    const int rp = d.px(r);
    SDL_Texture* ring = d.ring(rp, d.px(t));
    if (ring) {
        d.setColor(ring, c);
        d.corners(ring, rp, x, y, w, h, r);
    }
    d.fill(x + r, y, w - 2 * r, t, c);
    d.fill(x + r, y + h - t, w - 2 * r, t, c);
    d.fill(x, y + r, t, h - 2 * r, c);
    d.fill(x + w - t, y + r, t, h - 2 * r, c);
}

void Ui::roundCorners(int x, int y, int w, int h, int radius, Color bg) {
    Impl& d = *d_;
    const float r = static_cast<float>(std::min(radius, std::min(w, h) / 2));
    if (r < 1.0f) return;
    const int rp = d.px(r);
    SDL_Texture* m = d.mask(rp);
    if (!m) return;
    d.setColor(m, bg);
    const SDL_FRect dst[4] = {{(float)x, (float)y, r, r},
                              {x + w - r, (float)y, r, r},
                              {(float)x, y + h - r, r, r},
                              {x + w - r, y + h - r, r, r}};
    const SDL_RendererFlip flips[4] = {SDL_FLIP_NONE, SDL_FLIP_HORIZONTAL, SDL_FLIP_VERTICAL,
                                       static_cast<SDL_RendererFlip>(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL)};
    for (int i = 0; i < 4; ++i) SDL_RenderCopyExF(d.renderer, m, nullptr, &dst[i], 0.0, nullptr, flips[i]);
}

void Ui::gradient(int x, int y, int w, int h, Color top, Color bottom) {
    if (w <= 0 || h <= 0) return;
    SDL_Vertex v[4];
    const SDL_Color ct{top.r, top.g, top.b, top.a}, cb{bottom.r, bottom.g, bottom.b, bottom.a};
    v[0] = {{(float)x, (float)y}, ct, {0, 0}};
    v[1] = {{(float)(x + w), (float)y}, ct, {0, 0}};
    v[2] = {{(float)x, (float)(y + h)}, cb, {0, 0}};
    v[3] = {{(float)(x + w), (float)(y + h)}, cb, {0, 0}};
    const int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_SetRenderDrawBlendMode(d_->renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(d_->renderer, nullptr, v, 4, idx, 6);
}

void Ui::circle(int cx, int cy, int radius, Color c) {
    if (radius <= 0) return;
    Impl& d = *d_;
    const int rp = d.px(static_cast<float>(radius));
    SDL_Texture* t = d.disc(rp);
    if (!t) return;
    d.setColor(t, c);
    SDL_FRect dst{(float)(cx - radius), (float)(cy - radius), (float)radius * 2, (float)radius * 2};
    SDL_RenderCopyF(d.renderer, t, nullptr, &dst);
}

void Ui::line(float x0, float y0, float x1, float y1, float thickness, Color c) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) return;
    const float nx = -dy / len * thickness / 2, ny = dx / len * thickness / 2;
    const SDL_Color col{c.r, c.g, c.b, c.a};
    SDL_Vertex v[4] = {{{x0 + nx, y0 + ny}, col, {0, 0}},
                       {{x1 + nx, y1 + ny}, col, {0, 0}},
                       {{x0 - nx, y0 - ny}, col, {0, 0}},
                       {{x1 - nx, y1 - ny}, col, {0, 0}}};
    const int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_SetRenderDrawBlendMode(d_->renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(d_->renderer, nullptr, v, 4, idx, 6);
}

void Ui::button(const std::string& label, int x, int y, int w, int h, bool focused) {
    const int r = std::min(14, h / 2);
    const int size = std::max(18, std::min(30, h * 2 / 5));
    if (focused) {
        outline(x - 5, y - 5, w + 10, h + 10, withAlpha(colors::AccentBright, 200), 3, r + 5);
        rect(x, y, w, h, colors::Accent, true, r);
        text(label, x + w / 2, y + (h - lineHeight(size, true)) / 2, size, colors::White, Align::Center, true);
    } else {
        rect(x, y, w, h, colors::PanelHi, true, r);
        text(label, x + w / 2, y + (h - lineHeight(size, true)) / 2, size, colors::Text, Align::Center, true);
    }
}

void Ui::tile(SDL_Texture* img, const std::string& caption, const std::string& badge, int x, int y, int w, int h,
              bool focused) {
    const int captionH = caption.empty() ? 0 : 58;
    const int ih = h - captionH;
    const int r = 12;
    if (focused) outline(x - 6, y - 6, w + 12, ih + 12, colors::AccentBright, 4, r + 6);
    if (img) {
        // Box art from remoteImage() is already exactly w x ih; other textures get a centred
        // cover crop so they are never distorted.
        int tw = 0, th = 0;
        SDL_QueryTexture(img, nullptr, nullptr, &tw, &th);
        SDL_SetTextureColorMod(img, 255, 255, 255);
        SDL_SetTextureAlphaMod(img, 255);
        SDL_FRect dst{(float)x, (float)y, (float)w, (float)ih};
        if (tw > 0 && th > 0 && std::abs(static_cast<double>(tw) / th - static_cast<double>(w) / ih) > 0.01) {
            SDL_Rect src{0, 0, tw, th};
            if (static_cast<double>(tw) / th > static_cast<double>(w) / ih) {
                src.w = static_cast<int>(static_cast<double>(th) * w / ih);
                src.x = (tw - src.w) / 2;
            } else {
                src.h = static_cast<int>(static_cast<double>(tw) * ih / w);
                src.y = (th - src.h) / 2;
            }
            SDL_RenderCopyF(d_->renderer, img, &src, &dst);
        } else {
            SDL_RenderCopyF(d_->renderer, img, nullptr, &dst);
        }
    } else {
        // Placeholder: panel with the title's initials.
        gradient(x, y, w, ih, colors::PanelHi, colors::Panel);
        std::string initials;
        bool takeNext = true;
        for (size_t i = 0; i < caption.size() && initials.size() < 2; ++i) {
            const unsigned char ch = static_cast<unsigned char>(caption[i]);
            if (ch == ' ' || ch == ':' || ch == '-') {
                takeNext = true;
            } else if (takeNext && std::isalnum(ch)) {
                initials.push_back(static_cast<char>(std::toupper(ch)));
                takeNext = false;
            } else {
                takeNext = false;
            }
        }
        text(initials, x + w / 2, y + ih / 2 - 36, 64, withAlpha(colors::TextDim, 160), Align::Center, true);
    }
    roundCorners(x, y, w, ih, r, colors::Background);
    if (!badge.empty()) {
        const int bs = 18;
        const int bw = textWidth(badge, bs, true) + 20;
        rect(x + 10, y + 10, bw, 30, withAlpha(colors::Accent, 235), true, 15);
        text(badge, x + 20, y + 10 + (30 - lineHeight(bs, true)) / 2, bs, colors::White, Align::Left, true);
    }
    if (captionH > 0)
        textEllipsized(caption, x + 2, y + ih + 14, w - 4, 24, focused ? colors::White : colors::TextDim,
                       Align::Left, focused);
}

void Ui::spinner(int cx, int cy, int radius, uint64_t tMs) {
    constexpr int kDots = 12;
    const int dot = std::max(3, radius / 7);
    const double phase = (tMs % 1000) / 1000.0 * kDots;
    for (int i = 0; i < kDots; ++i) {
        const double a = (static_cast<double>(i) / kDots) * 2 * kPi - kPi / 2;
        double age = phase - i;
        while (age < 0) age += kDots;
        const double k = 1.0 - age / kDots;  // 1 = head
        const uint8_t alpha = static_cast<uint8_t>(40 + 215 * k * k);
        const int px = cx + static_cast<int>(std::lround(std::cos(a) * (radius - dot)));
        const int py = cy + static_cast<int>(std::lround(std::sin(a) * (radius - dot)));
        circle(px, py, dot, withAlpha(colors::AccentBright, alpha));
    }
}

void Ui::qr(const std::string& data, int x, int y, int size) {
    Impl& d = *d_;
    const int p = d.px(static_cast<float>(size));
    const std::string key = std::to_string(p) + "|" + data;
    auto it = d.qr.find(key);
    if (it == d.qr.end()) {
        std::vector<bool> modules;
        int n = 0;
        if (!qr::encode(data, modules, n) || n <= 0) {
            rect(x, y, size, size, colors::White, true, 12);
            text("QR unavailable", x + size / 2, y + size / 2 - 14, 24, colors::Error, Align::Center);
            return;
        }
        // 2-module quiet zone inside the texture; the white card drawn around it adds the rest.
        const int total = n + 4;
        const int mod = std::max(1, p / total);
        const int side = mod * total;
        const int off = (p - side) / 2 + 2 * mod;
        std::vector<uint32_t> pix(static_cast<size_t>(p) * p, 0xFFFFFFFFu);
        for (int my = 0; my < n; ++my)
            for (int mx = 0; mx < n; ++mx) {
                if (!modules[static_cast<size_t>(my) * n + mx]) continue;
                for (int yy = 0; yy < mod; ++yy) {
                    const int py = off + my * mod + yy;
                    if (py < 0 || py >= p) continue;
                    uint32_t* row = pix.data() + static_cast<size_t>(py) * p;
                    for (int xx = 0; xx < mod; ++xx) {
                        const int pxx = off + mx * mod + xx;
                        if (pxx >= 0 && pxx < p) row[pxx] = 0xFF000000u;
                    }
                }
            }
        SDL_Texture* t = SDL_CreateTexture(d.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, p, p);
        if (!t) return;
        SDL_UpdateTexture(t, nullptr, pix.data(), p * 4);
        TextEntry e;
        e.tex = t;
        e.pw = e.ph = p;
        it = d.qr.emplace(key, e).first;
    }
    it->second.lastUsed = d.frame;
    // white rounded backing so the quiet zone reads as a card
    rect(x - 12, y - 12, size + 24, size + 24, colors::White, true, 16);
    SDL_FRect dst{(float)x, (float)y, (float)size, (float)size};
    SDL_RenderCopyF(d.renderer, it->second.tex, nullptr, &dst);
}

void Ui::image(SDL_Texture* tex, int x, int y, int w, int h, bool fit) {
    if (!tex || w <= 0 || h <= 0) return;
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, 255);
    SDL_FRect dst{(float)x, (float)y, (float)w, (float)h};
    if (fit) {
        int tw = 0, th = 0;
        if (SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th) != 0 || tw <= 0 || th <= 0) return;
        const float s = std::min(static_cast<float>(w) / tw, static_cast<float>(h) / th);
        dst.w = tw * s;
        dst.h = th * s;
        dst.x = x + (w - dst.w) / 2;
        dst.y = y + (h - dst.h) / 2;
    }
    SDL_RenderCopyF(d_->renderer, tex, nullptr, &dst);
}

SDL_Texture* Ui::textureFromImage(const std::vector<uint8_t>& bytes) {
    image::Rgba img;
    if (!image::decode(bytes.data(), bytes.size(), img)) return nullptr;
    SDL_Texture* t =
        SDL_CreateTexture(d_->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
    if (!t) {
        XC_LOGW("ui: texture %dx%d failed: %s", img.width, img.height, SDL_GetError());
        return nullptr;
    }
    SDL_UpdateTexture(t, nullptr, img.pixels.data(), img.width * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

SDL_Texture* Ui::remoteImage(const std::string& url, int w, int h) {
    if (url.empty() || w <= 0 || h <= 0) return nullptr;
    Impl& d = *d_;
    const int pw = d.px(static_cast<float>(w)), ph = d.px(static_cast<float>(h));
    std::string key = url;
    key += '#';
    key += std::to_string(pw);
    key += 'x';
    key += std::to_string(ph);
    auto it = d.images.find(key);
    if (it != d.images.end()) {
        ImageEntry& e = it->second;
        e.lastUsed = d.frame;
        if (e.state == ImageEntry::Ready) return e.tex;
        if (e.state == ImageEntry::Failed) {
            if (d.frame - e.failedAt < kImageRetryFrames) return nullptr;
            e.state = ImageEntry::Queued;
        }
    } else {
        ImageEntry e;
        e.lastUsed = d.frame;
        d.images.emplace(key, e);
    }
    {
        std::lock_guard<std::mutex> lk(d.loader->m);
        auto& job = d.loader->jobs[key];
        if (job.url.empty()) {
            job.url = url;
            job.pw = pw;
            job.ph = ph;
        }
        job.wanted = d.frame;
    }
    d.loader->cv.notify_one();
    return nullptr;
}

int Ui::remoteImagesPending() const {
    std::lock_guard<std::mutex> lk(d_->loader->m);
    return static_cast<int>(d_->loader->jobs.size()) + d_->loader->inFlight;
}

void Ui::setClip(int x, int y, int w, int h) {
    SDL_Rect r{x, y, w, h};
    SDL_RenderSetClipRect(d_->renderer, &r);
}

void Ui::clearClip() { SDL_RenderSetClipRect(d_->renderer, nullptr); }

int Ui::padIcon(PadIcon icon, int x, int cy, int size) {
    const float r = size / 2.0f;
    const float cx = x + r;
    const Color bg{58, 66, 62, 255};
    const float t = std::max(2.0f, size / 11.0f);
    auto pill = [&](const std::string& label) {
        const int fs = std::max(12, size * 9 / 20);
        const int w = std::max(size + size / 3, textWidth(label, fs, true) + size / 2);
        rect(x, cy - size / 2, w, size, bg, true, size / 2);
        text(label, x + w / 2, cy - lineHeight(fs, true) / 2, fs, colors::Text, Align::Center, true);
        return w;
    };
    switch (icon) {
        case PadIcon::Cross: {
            circle(static_cast<int>(cx), cy, size / 2, bg);
            const float k = r * 0.42f;
            const Color c{140, 170, 255, 255};
            line(cx - k, cy - k, cx + k, cy + k, t, c);
            line(cx - k, cy + k, cx + k, cy - k, t, c);
            return size;
        }
        case PadIcon::Circle: {
            circle(static_cast<int>(cx), cy, size / 2, bg);
            const int rr = static_cast<int>(r * 0.5f);
            Impl& d = *d_;
            const int rp = d.px(static_cast<float>(rr));
            if (SDL_Texture* ring = d.ring(rp, d.px(t))) {
                d.setColor(ring, Color{255, 110, 110, 255});
                SDL_FRect dst{cx - rr, (float)(cy - rr), (float)rr * 2, (float)rr * 2};
                SDL_RenderCopyF(d.renderer, ring, nullptr, &dst);
            }
            return size;
        }
        case PadIcon::Square: {
            circle(static_cast<int>(cx), cy, size / 2, bg);
            const int k = static_cast<int>(r * 0.42f);
            outline(static_cast<int>(cx) - k, cy - k, 2 * k, 2 * k, Color{240, 140, 210, 255}, static_cast<int>(t), 0);
            return size;
        }
        case PadIcon::Triangle: {
            circle(static_cast<int>(cx), cy, size / 2, bg);
            const float k = r * 0.48f;
            const Color c{90, 220, 180, 255};
            const float ax = cx, ay = cy - k, bx = cx - k * 0.95f, by = cy + k * 0.62f, qx = cx + k * 0.95f, qy = by;
            line(ax, ay, bx, by, t, c);
            line(bx, by, qx, qy, t, c);
            line(qx, qy, ax, ay, t, c);
            return size;
        }
        case PadIcon::L1: return pill("L1");
        case PadIcon::R1: return pill("R1");
        case PadIcon::PS: return pill("PS");
        case PadIcon::Options: {
            const int w = size + size / 3;
            rect(x, cy - size / 2, w, size, bg, true, size / 2);
            const float k = size * 0.26f;
            for (int i = -1; i <= 1; ++i)
                line(x + w / 2.0f - k, cy + i * k * 0.7f, x + w / 2.0f + k, cy + i * k * 0.7f, t * 0.8f, colors::Text);
            return w;
        }
        case PadIcon::Touchpad: {
            const int w = size * 2;
            rect(x, cy - size / 2, w, size, bg, true, size / 2);
            outline(x + size / 3, cy - size / 4, w - 2 * size / 3, size / 2, colors::Text, static_cast<int>(t * 0.8f), 4);
            return w;
        }
    }
    return size;
}

int Ui::keyCap(const std::string& label, int x, int cy, int size) {
    const int fs = std::max(12, size * 9 / 20);
    const int w = std::max(size, textWidth(label, fs, true) + size / 2);
    rect(x, cy - size / 2, w, size, Color{58, 66, 62, 255}, true, 8);
    outline(x, cy - size / 2, w, size, Color{90, 100, 95, 255}, 2, 8);
    text(label, x + w / 2, cy - lineHeight(fs, true) / 2, fs, colors::Text, Align::Center, true);
    return w;
}

bool Ui::saveScreenshot(const std::string& path) {
    SDL_Renderer* r = d_->renderer;
    // Read the whole output in real pixels: drop the logical size (resets scale + viewport,
    // the back buffer is untouched), read, restore.
    SDL_RenderSetLogicalSize(r, 0, 0);
    int w = 0, h = 0;
    bool ok = false;
    if (SDL_GetRendererOutputSize(r, &w, &h) == 0 && w > 0 && h > 0) {
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888)) {
            ok = SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 &&
                 SDL_SaveBMP(s, path.c_str()) == 0;
            SDL_FreeSurface(s);
        }
    }
    SDL_RenderSetLogicalSize(r, kWidth, kHeight);
    if (ok) XC_LOGI("ui: screenshot saved to %s", path.c_str());
    else XC_LOGW("ui: screenshot failed: %s", SDL_GetError());
    return ok;
}

void Ui::clearCache() {
    Impl& d = *d_;
    d.destroyMap(d.text);
    d.destroyMap(d.qr);
    d.widths.clear();
    for (auto& kv : d.images)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    d.images.clear();
    if (d.loader) {
        std::lock_guard<std::mutex> lk(d.loader->m);
        d.loader->jobs.clear();
    }
}

}  // namespace xc
