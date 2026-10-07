// Nubix — application object and main loop.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/app.hpp"


#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "core/http.hpp"
#include "core/log.hpp"
#include "core/protocol.hpp"
#include "platform/platform.hpp"
#include "stream/audio.hpp"
#include "stream/video.hpp"
#include "stream/webrtc.hpp"
#include "ui/qr.hpp"
#include "ui/screens_internal.hpp"

namespace xc {
namespace {

constexpr uint64_t kToastMs = 3500;
constexpr uint64_t kToastFadeMs = 400;
constexpr size_t kMaxToasts = 3;

struct Toast {
    std::string msg;
    uint64_t until = 0;
};

struct StackOp {
    enum Kind { Push, Pop, Replace, Reset } kind;
    std::unique_ptr<Screen> screen;
};

// --selftest: bring up every subsystem the stream path needs, tear it down again and report.
// Returns the number of failed checks (each is logged). Audio is optional (headless hosts and
// SDL_AUDIODRIVER=dummy may lack a device) and only warns.
int runSubsystemSelftest() {
    int failed = 0;
    auto check = [&](const char* what, bool ok) {
        xc::log(ok ? LogLevel::Info : LogLevel::Error, "selftest: %-28s %s", what, ok ? "ok" : "FAILED");
        if (!ok) ++failed;
    };

    for (const char* a : {"cacert.pem", "fonts/DejaVuSans.ttf", "fonts/DejaVuSans-Bold.ttf"}) {
        std::string path = platform::assetPath(a);
        FILE* f = std::fopen(path.c_str(), "rb");
        check(("asset " + std::string(a)).c_str(), f != nullptr);
        if (f) std::fclose(f);
    }

    {
        std::vector<bool> modules;
        int size = 0;
        check("qr encode", qr::encode("https://www.microsoft.com/link?otc=ABCD1234", modules, size) && size > 0 &&
                               modules.size() == static_cast<size_t>(size) * static_cast<size_t>(size));
    }
    {
        proto::InputSerializer ser;
        check("input serializer", ser.clientMetadata().size() == proto::kClientMetadataPacketSize &&
                                      ser.gamepad(GamepadState{}).size() == proto::kGamepadPacketSize);
    }
    {
        VideoDecoder dec;
        check("h264 decoder init", dec.init(4));
        dec.close();
    }
    {
        AudioPlayer ap;
        const bool ok = ap.init();
        xc::log(ok ? LogLevel::Info : LogLevel::Warn, "selftest: %-28s %s", "opus + SDL audio init",
               ok ? "ok" : "unavailable (non-fatal)");
        ap.close();
    }
    {
        WebRtc rtc;
        WebRtc::Callbacks cb;
        bool ok = rtc.init(cb);
        std::string ufrag, pwd, fp;
        std::vector<std::string> cands;
        if (ok) ok = rtc.gatherLocal(ufrag, pwd, fp, cands, 3000) && !ufrag.empty() && !fp.empty();
        if (ok) ok = !proto::buildOffer(ufrag, pwd, fp, false, "1080").empty();
        check("webrtc init + gather + offer", ok);
        rtc.close();
    }
    return failed;
}

}  // namespace

struct App::Impl {
    Impl() : cfg(platform::dataDir() + "/config.json"), auth(cfg), catalog(cfg), streamer(cfg, auth) {
        Catalog::setCacheDir(platform::dataDir() + "/cache");  // title list + box art
    }
    Config cfg;
    Auth auth;
    Catalog catalog;
    Streamer streamer;
    Ui ui;
    Gamepad gamepad;
    std::atomic<bool> quit{false};
    // Set when the app exits: HTTP requests of worker jobs (catalog, auth) abort within ~1 s.
    std::atomic<bool> cancel{false};
    bool statsOverlay = false;

    std::vector<std::unique_ptr<Screen>> stack;
    std::vector<StackOp> ops;

    // background worker (FIFO)
    std::thread worker;
    std::mutex workMx;
    std::condition_variable workCv;
    std::deque<std::function<void()>> work;
    bool workerStop = false;

    // main-thread queue
    std::mutex mainMx;
    std::vector<std::function<void()>> mainQueue;

    std::mutex toastMx;
    std::deque<Toast> toasts;

    bool screenshotRequested = false;
    int screenshotCount = 0;
    // Development automation (host): XC_SHOT_FRAMES="60,120" saves screenshots at those frame
    // numbers, XC_EXIT_FRAME=N quits after frame N.
    std::vector<uint64_t> shotFrames;
    uint64_t exitFrame = 0;

    void takeScreenshot() {
        char name[64];
        std::snprintf(name, sizeof(name), "/screenshot-%03d.bmp", screenshotCount++);
        if (ui.saveScreenshot(platform::dataDir() + name)) {
            std::lock_guard<std::mutex> lk(toastMx);
            toasts.push_back({std::string("Screenshot saved: ") + (name + 1), platform::monotonicMs() + kToastMs});
        }
    }

    void workerLoop() {
        // Every job's requests (and the catalog helper threads, which inherit the flag) end
        // quickly once the app is closing instead of running into 20-30 s timeouts.
        Http::AbortScope abortOnExit(&cancel);
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(workMx);
                workCv.wait(lk, [&] { return workerStop || !work.empty(); });
                if (workerStop) return;
                job = std::move(work.front());
                work.pop_front();
            }
            if (!job) continue;
            try {
                job();
            } catch (const std::exception& e) {
                // A failed job must not take the app down (std::terminate); its UI simply does
                // not get its result.
                XC_LOGE("app: background job failed: %s", e.what());
                std::lock_guard<std::mutex> lk(toastMx);
                toasts.push_back({"Something went wrong, please try again", platform::monotonicMs() + kToastMs});
            } catch (...) {
                XC_LOGE("app: background job failed with an unknown exception");
            }
        }
    }

    void stopWorker() {
        if (!worker.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(workMx);
            workerStop = true;
            work.clear();
        }
        cancel = true;
        workCv.notify_all();
        worker.join();  // waits for the job in progress (bounded by HTTP timeouts)
    }

    void drainMainQueue() {
        std::vector<std::function<void()>> q;
        {
            std::lock_guard<std::mutex> lk(mainMx);
            q.swap(mainQueue);
        }
        for (auto& fn : q)
            if (fn) fn();
    }

    void applyOps(App& app) {
        // Ops queued from onEnter/onLeave are processed in the same pass.
        for (int guard = 0; guard < 32 && !ops.empty(); ++guard) {
            std::vector<StackOp> batch;
            batch.swap(ops);
            for (auto& op : batch) {
                switch (op.kind) {
                    case StackOp::Push:
                        if (!stack.empty()) stack.back()->onLeave(app);
                        stack.push_back(std::move(op.screen));
                        stack.back()->onEnter(app);
                        break;
                    case StackOp::Pop:
                        if (stack.empty()) break;
                        stack.back()->onLeave(app);
                        stack.pop_back();
                        if (!stack.empty()) stack.back()->onEnter(app);
                        break;
                    case StackOp::Replace:
                        if (!stack.empty()) {
                            stack.back()->onLeave(app);
                            stack.pop_back();
                        }
                        stack.push_back(std::move(op.screen));
                        stack.back()->onEnter(app);
                        break;
                    case StackOp::Reset:
                        if (!stack.empty()) stack.back()->onLeave(app);
                        stack.clear();
                        stack.push_back(std::move(op.screen));
                        stack.back()->onEnter(app);
                        break;
                }
            }
        }
    }

    void renderToasts(Ui& ui, uint64_t now) {
        std::lock_guard<std::mutex> lk(toastMx);
        while (!toasts.empty() && toasts.front().until <= now) toasts.pop_front();
        int y = Ui::kHeight - 150;
        for (auto it = toasts.rbegin(); it != toasts.rend(); ++it) {
            const uint64_t left = it->until - now;
            const uint8_t a = left < kToastFadeMs ? static_cast<uint8_t>(255 * left / kToastFadeMs) : 255;
            const int size = 26;
            const int w = std::min(1500, ui.textWidth(it->msg, size) + 64);
            const int h = 64;
            const int x = (Ui::kWidth - w) / 2;
            ui.rect(x, y - h, w, h, withAlpha(colors::PanelHi, static_cast<uint8_t>(a * 240 / 255)), true, h / 2);
            ui.rect(x, y - h, 8, h, withAlpha(colors::AccentBright, a), true, 4);
            ui.textEllipsized(it->msg, Ui::kWidth / 2, y - h + (h - ui.lineHeight(size)) / 2, w - 64, size,
                              withAlpha(colors::Text, a), Align::Center);
            y -= h + 12;
        }
    }
};

App::App() : d_(new Impl) {}
App::~App() {
    d_->stopWorker();
}

int App::run(int argc, char** argv) {
    Impl& d = *d_;
    // --selftest: init all subsystems, render the Login screen for kSelftestFrames frames, exit
    // 0 when everything came up (used headless with SDL_VIDEODRIVER=dummy).
    constexpr uint64_t kSelftestFrames = 30;
    bool selftest = false;
    for (int i = 1; i < argc; ++i)
        if (argv[i] && std::string(argv[i]) == "--selftest") selftest = true;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
#endif
#ifdef SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        XC_LOGE("SDL_Init failed: %s", SDL_GetError());
        platform::notify("Nubix: SDL init failed");
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) XC_LOGW("SDL audio init failed: %s (no sound)", SDL_GetError());
    // Stream video is BT.709 at HD sizes, BT.601 at SD: let SDL pick per resolution.
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_AUTOMATIC);

    Http::globalInit(platform::assetPath("cacert.pem"));
    if (!d.cfg.load()) XC_LOGW("config: %s is malformed, using defaults", d.cfg.path().c_str());

    if (!d.ui.init(platform::assetPath("fonts/DejaVuSans.ttf"), platform::assetPath("fonts/DejaVuSans-Bold.ttf"),
                   "Nubix")) {
        platform::notify("Nubix: cannot start UI (assets missing?)");
        d.ui.shutdown();
        Http::globalCleanup();
        SDL_Quit();
        return 1;
    }
    if (platform::isPs5()) SDL_ShowCursor(SDL_DISABLE);
    {
        SDL_version v;
        SDL_GetVersion(&v);
        XC_LOGI("SDL %d.%d.%d, video driver %s, network %s (%s)", v.major, v.minor, v.patch,
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
                platform::networkAvailable() ? "up" : "DOWN", platform::localIpv4().c_str());
    }
    d.gamepad.init();

    int selftestFailures = 0;
    if (selftest) {
        XC_LOGI("selftest: starting");
        selftestFailures = runSubsystemSelftest();
        d.exitFrame = kSelftestFrames;
    }

    try {
        d.worker = std::thread([this] { d_->workerLoop(); });
    } catch (const std::exception& e) {
        XC_LOGE("app: cannot start the worker thread: %s", e.what());
        platform::notify("Nubix: cannot start (out of threads)");
        d.gamepad.shutdown();
        d.ui.shutdown();
        Http::globalCleanup();
        SDL_Quit();
        return 1;
    }

    push((d.auth.isLoggedIn() && !selftest) ? makeHomeScreen() : makeLoginScreen());
    if (!platform::networkAvailable()) toast("No network connection detected");

    if (const char* e = std::getenv("XC_SHOT_FRAMES")) {
        for (const char* p = e; *p;) {
            char* end = nullptr;
            unsigned long long v = std::strtoull(p, &end, 10);
            if (end == p) break;
            d.shotFrames.push_back(v);
            p = (*end == ',') ? end + 1 : end;
        }
    }
    if (const char* e = std::getenv("XC_EXIT_FRAME"); e && !selftest) d.exitFrame = std::strtoull(e, nullptr, 10);

    SDL_RendererInfo rinfo{};
    SDL_GetRendererInfo(d.ui.renderer(), &rinfo);
    const bool vsync = (rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0 || platform::isPs5();  // PS5 flip waits
    uint64_t last = platform::monotonicMs();
    bool firstFrame = true;
    uint64_t lastFrame = 0;

    while (!d.quit.load()) {
        const uint64_t frameStart = platform::monotonicMs();
        const uint32_t dt = static_cast<uint32_t>(std::min<uint64_t>(250, frameStart - last));
        last = frameStart;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                d.quit = true;
                break;
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F12 && !ev.key.repeat) d.screenshotRequested = true;
            NavKey k = d.gamepad.handleEvent(ev);
            if (k == NavKey::None || d.stack.empty()) continue;
            Screen* top = d.stack.back().get();
            if (top->capturesGamepad() && k != NavKey::Menu) continue;
            top->onNav(*this, k);
        }
        if (NavKey r = d.gamepad.pollRepeat(); r != NavKey::None && !d.stack.empty()) {
            Screen* top = d.stack.back().get();
            if (!top->capturesGamepad()) top->onNav(*this, r);
        }

        d.drainMainQueue();
        d.applyOps(*this);
        if (d.stack.empty()) break;
        d.stack.back()->update(*this, dt);
        d.applyOps(*this);
        if (d.stack.empty()) break;

        d.ui.beginFrame();
        size_t first = d.stack.size() - 1;
        while (first > 0 && d.stack[first]->isOverlay()) --first;
        for (size_t i = first; i < d.stack.size(); ++i) d.stack[i]->render(*this, d.ui);
        const uint64_t frameNo = d.ui.frameCount();
        lastFrame = frameNo;
        if (std::find(d.shotFrames.begin(), d.shotFrames.end(), frameNo) != d.shotFrames.end())
            d.screenshotRequested = true;
        if (d.screenshotRequested) {
            d.screenshotRequested = false;
            d.takeScreenshot();  // before toasts so the "saved" toast is not in the picture
        }
        d.renderToasts(d.ui, platform::monotonicMs());
        d.ui.endFrame();
        if (d.exitFrame && frameNo >= d.exitFrame) d.quit = true;
        if (firstFrame) {
            platform::onFirstFrame();
            firstFrame = false;
        }

        // Present normally blocks on vsync; when it does not (hidden/occluded window, no vsync
        // support, high-refresh displays) cap the loop at ~60 fps to keep CPU use sane.
        const uint64_t spent = platform::monotonicMs() - frameStart;
        if (spent < (vsync ? 15u : 16u)) SDL_Delay(static_cast<Uint32>((vsync ? 15u : 16u) - spent));
    }

    XC_LOGI("shutting down");
    d.cancel = true;  // running worker job: abort its requests now
    if (!d.stack.empty()) d.stack.back()->onLeave(*this);
    d.ops.clear();
    d.gamepad.stopRumble();
    // Stopping a stream (final DELETE, bounded to ~5 s) and joining the worker can take a moment:
    // show that the app is closing instead of a frozen last frame.
    d.ui.beginFrame();
    ui::background(d.ui);
    d.ui.spinner(Ui::kWidth / 2, Ui::kHeight / 2 - 40, 48, platform::monotonicMs());
    d.ui.text("Closing\xE2\x80\xA6", Ui::kWidth / 2, Ui::kHeight / 2 + 40, 32, colors::Text, Align::Center);
    d.ui.endFrame();
    d.streamer.stop();
    d.stopWorker();
    d.stack.clear();
    {
        std::lock_guard<std::mutex> lk(d.mainMx);
        d.mainQueue.clear();
    }
    d.gamepad.shutdown();
    d.ui.shutdown();
    Http::globalCleanup();
    SDL_Quit();
    if (selftest) {
        const bool framesOk = lastFrame >= kSelftestFrames;
        if (!framesOk) ++selftestFailures;
        XC_LOGI("selftest: rendered %llu frames; %s", static_cast<unsigned long long>(lastFrame),
                selftestFailures ? "FAILED" : "PASSED");
        return selftestFailures ? 1 : 0;
    }
    return 0;
}

void App::push(std::unique_ptr<Screen> s) {
    if (s) d_->ops.push_back({StackOp::Push, std::move(s)});
}

void App::pop() { d_->ops.push_back({StackOp::Pop, nullptr}); }

void App::replace(std::unique_ptr<Screen> s) {
    if (s) d_->ops.push_back({StackOp::Replace, std::move(s)});
}

void App::resetTo(std::unique_ptr<Screen> s) {
    if (s) d_->ops.push_back({StackOp::Reset, std::move(s)});
}

void App::quit() {
    d_->quit = true;
    d_->cancel = true;
}

void App::runAsync(std::function<void()> work) {
    if (!work) return;
    {
        std::lock_guard<std::mutex> lk(d_->workMx);
        if (d_->workerStop) return;
        d_->work.push_back(std::move(work));
    }
    d_->workCv.notify_one();
}

void App::postToMain(std::function<void()> fn) {
    if (!fn) return;
    std::lock_guard<std::mutex> lk(d_->mainMx);
    d_->mainQueue.push_back(std::move(fn));
}

void App::toast(const std::string& msg) {
    XC_LOGI("toast: %s", msg.c_str());
    std::lock_guard<std::mutex> lk(d_->toastMx);
    d_->toasts.push_back({msg, platform::monotonicMs() + kToastMs});
    while (d_->toasts.size() > kMaxToasts) d_->toasts.pop_front();
}

Config& App::config() { return d_->cfg; }
Auth& App::auth() { return d_->auth; }
Catalog& App::catalog() { return d_->catalog; }
Streamer& App::streamer() { return d_->streamer; }
Ui& App::ui() { return d_->ui; }
Gamepad& App::gamepad() { return d_->gamepad; }
bool App::statsOverlay() const { return d_->statsOverlay; }
void App::setStatsOverlay(bool on) { d_->statsOverlay = on; }

}  // namespace xc
