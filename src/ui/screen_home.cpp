// Nubix — Home screen: tabs Cloud library (box-art grid) / My consoles (xHome) / Settings.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/log.hpp"
#include "platform/platform.hpp"
#include "ui/app.hpp"
#include "ui/screens.hpp"
#include "ui/screens_internal.hpp"
#include "ui/ui.hpp"

namespace xc {
namespace {

// grid geometry (logical px)
constexpr int kGridLeft = 96;
constexpr int kGridTop = 300;
constexpr int kCols = 8;
constexpr int kGap = 30;
constexpr int kTileW = (Ui::kWidth - 2 * kGridLeft - (kCols - 1) * kGap) / kCols;  // 189
constexpr int kImageH = kTileW * 4 / 3;                                             // poster 3:4 (252)
constexpr int kTileH = kImageH + 58;
constexpr int kRowPitch = kTileH + 30;                                              // 340
constexpr int kViewH = 2 * kRowPitch;                                               // two full rows
constexpr int kGridBottom = kGridTop + kViewH;

enum Tab { TabLibrary, TabConsoles, TabSettings, TabCount };
enum Filter { FilterAll, FilterGamePass, FilterOwned, FilterFree, FilterCount };

const char* tabName(int t) {
    switch (t) {
        case TabLibrary: return "Cloud library";
        case TabConsoles: return "My consoles";
        default: return "Settings";
    }
}

const char* filterName(int f) {
    switch (f) {
        case FilterGamePass: return "Game Pass";
        case FilterOwned: return "Owned";
        case FilterFree: return "Free to play";
        default: return "All games";
    }
}

std::string normalizeUrl(const std::string& u) {
    if (u.rfind("//", 0) == 0) return "https:" + u;
    return u;
}

struct HomeState {
    // session / tokens
    enum Phase { Idle, Authorizing, Ready, AuthFailed } phase = Idle;
    std::string authError;
    bool hasCloud = false, hasHome = false;

    // library
    bool titlesLoading = false, titlesLoaded = false;
    std::string titlesError;
    std::vector<Title> titles;
    uint64_t titlesVersion = 0;  // bumped whenever `titles` is replaced

    // consoles
    bool consolesLoading = false, consolesLoaded = false;
    std::string consolesError;
    std::vector<Console> consoles;
};

class HomeScreen : public Screen {
public:
    void onEnter(App& app) override {
        if (st_->phase == HomeState::Idle || st_->phase == HomeState::AuthFailed) authorize(app);
    }

    void onNav(App& app, NavKey key) override {
        if (key == NavKey::TabLeft || key == NavKey::TabRight) {
            tab_ = (tab_ + (key == NavKey::TabRight ? 1 : TabCount - 1)) % TabCount;
            if (tab_ == TabSettings) settings_.resetFocus();
            return;
        }
        if (st_->phase == HomeState::AuthFailed) {
            if (key == NavKey::Accept) authorize(app);
            else if (key == NavKey::Option) app.resetTo(makeLoginScreen());
            else if (key == NavKey::Back) app.push(ui::makeExitConfirm());
            return;
        }
        switch (tab_) {
            case TabLibrary: navLibrary(app, key); break;
            case TabConsoles: navConsoles(app, key); break;
            case TabSettings:
                if (!settings_.onNav(app, key) && key == NavKey::Back) app.push(ui::makeExitConfirm());
                break;
        }
    }

    void update(App& app, uint32_t dtMs) override {
        (void)app;
        // smooth grid scroll
        const auto& list = visible();
        const int rows = static_cast<int>((list.size() + kCols - 1) / kCols);
        const int row = focusTitle_ / kCols;
        // minimal scrolling: keep the focused row fully visible, rows snap to the grid
        const float rowTop = static_cast<float>(row * kRowPitch);
        if (rowTop < targetScroll_) targetScroll_ = rowTop;
        if (rowTop + kRowPitch > targetScroll_ + kViewH) targetScroll_ = rowTop + kRowPitch - kViewH;
        const float maxScroll = static_cast<float>(std::max(0, rows * kRowPitch - kViewH));
        targetScroll_ = std::max(0.0f, std::min(maxScroll, targetScroll_));
        const float k = std::min(1.0f, dtMs * 0.014f);
        scroll_ += (targetScroll_ - scroll_) * k;
        if (std::fabs(targetScroll_ - scroll_) < 0.5f) scroll_ = targetScroll_;
    }

    void render(App& app, Ui& ui) override {
        ui::background(ui);
        renderHeader(app, ui);
        const uint64_t now = platform::monotonicMs();

        if (st_->phase == HomeState::Authorizing || st_->phase == HomeState::Idle) {
            ui.spinner(Ui::kWidth / 2, 500, 52, now);
            ui.text("Connecting to Xbox\xE2\x80\xA6", Ui::kWidth / 2, 590, 32, colors::Text, Align::Center);
            ui::hintBar(app, ui, {{PadIcon::Circle, "Exit"}});
            return;
        }
        if (st_->phase == HomeState::AuthFailed) {
            ui::errorBadge(ui, Ui::kWidth / 2, 380, 48);
            ui.text("Couldn't connect to Xbox", Ui::kWidth / 2, 460, 42, colors::Text, Align::Center, true);
            ui.textWrapped(st_->authError, (Ui::kWidth - 1100) / 2, 532, 1100, 28, colors::TextDim);
            ui::hintBar(app, ui,
                        {{PadIcon::Cross, "Retry"}, {PadIcon::Triangle, "Sign in again"}, {PadIcon::Circle, "Exit"}});
            return;
        }

        switch (tab_) {
            case TabLibrary: renderLibrary(app, ui, now); break;
            case TabConsoles: renderConsoles(app, ui, now); break;
            case TabSettings:
                settings_.render(app, ui, 200);
                ui::hintBar(app, ui, {{PadIcon::L1, ""}, {PadIcon::R1, "Switch tab"}, {PadIcon::Cross, "Change"},
                                      {PadIcon::Circle, "Exit"}});
                break;
        }
    }

private:
    // ---- data loading -----------------------------------------------------------------------

    void authorize(App& app) {
        st_->phase = HomeState::Authorizing;
        std::weak_ptr<HomeState> w = st_;
        App* a = &app;
        app.runAsync([a, w] {
            std::string err;
            const bool ok = a->auth().ensureFresh(err);
            const bool loggedIn = a->auth().isLoggedIn();
            const Tokens t = a->config().tokens();
            const bool hasCloud = !t.xcloudGs.empty() || !t.xcloudF2pGs.empty();
            const bool hasHome = !t.xhomeGs.empty();
            a->postToMain([a, w, ok, loggedIn, hasCloud, hasHome, err] {
                auto s = w.lock();
                if (!s) return;
                if (!ok && !loggedIn) {
                    a->toast("Your sign-in expired. Please sign in again.");
                    a->resetTo(makeLoginScreen());
                    return;
                }
                if (!ok) {
                    s->phase = HomeState::AuthFailed;
                    s->authError = err.empty() ? std::string("Token refresh failed.") : err;
                    if (!platform::networkAvailable()) s->authError += "\nNo network connection detected.";
                    return;
                }
                s->phase = HomeState::Ready;
                s->hasCloud = hasCloud;
                s->hasHome = hasHome;
            });
            if (ok) {
                loadTitles(a, w);
                loadConsoles(a, w);
            }
        });
    }

    // Worker thread: the server refused a gsToken the stored expiry still called valid. Mark it
    // stale, re-authorize and run `retry` once. False (err set) if re-authorizing failed.
    static bool reauthorizeAndRetry(App* a, std::string& err, const std::function<bool()>& retry) {
        a->auth().invalidateStreaming();
        std::string aerr;
        if (!a->auth().ensureFresh(aerr)) {
            err = "Sign-in failed: " + aerr;
            return false;
        }
        err.clear();
        return retry();
    }

    // Main thread: the stored login died on a worker (refresh token rejected).
    static void goToLogin(App* a) {
        a->toast("Your sign-in expired. Please sign in again.");
        a->resetTo(makeLoginScreen());
    }

    // Runs on the worker (called from a worker job) or schedules itself.
    static void loadTitles(App* a, std::weak_ptr<HomeState> w, bool force = false) {
        a->postToMain([w] {
            if (auto s = w.lock()) {
                s->titlesLoading = true;
                s->titlesError.clear();
            }
        });
        a->runAsync([a, w, force] {
            std::vector<Title> list;
            std::string err;
            bool ok = true;
            auto fetch = [&](bool* authRejected) {
                const Tokens t = a->config().tokens();
                if (t.xcloudGs.empty() && t.xcloudF2pGs.empty()) return true;
                return a->catalog().fetchCloudTitles(list, err, authRejected, force || authRejected == nullptr);
            };
            bool authRejected = false;
            ok = fetch(&authRejected);
            if (authRejected) ok = reauthorizeAndRetry(a, err, [&] { return fetch(nullptr); });
            const bool signedOut = !a->auth().isLoggedIn();
            a->postToMain([a, w, ok, signedOut, list = std::move(list), err]() mutable {
                auto s = w.lock();
                if (!s) return;
                if (signedOut) {
                    goToLogin(a);
                    return;
                }
                s->titlesLoading = false;
                s->titlesLoaded = true;
                if (ok) {
                    s->titles = std::move(list);
                    ++s->titlesVersion;
                } else {
                    s->titlesError = err.empty() ? std::string("Could not load the cloud library.") : err;
                }
            });
        });
    }

    static void loadConsoles(App* a, std::weak_ptr<HomeState> w) {
        a->postToMain([w] {
            if (auto s = w.lock()) {
                s->consolesLoading = true;
                s->consolesError.clear();
            }
        });
        a->runAsync([a, w] {
            std::vector<Console> list;
            std::string err;
            auto fetch = [&](bool* authRejected) {
                if (a->config().tokens().xhomeGs.empty()) return true;
                return a->catalog().fetchConsoles(list, err, authRejected);
            };
            bool authRejected = false;
            bool ok = fetch(&authRejected);
            if (authRejected) ok = reauthorizeAndRetry(a, err, [&] { return fetch(nullptr); });
            const bool signedOut = !a->auth().isLoggedIn();
            a->postToMain([a, w, ok, signedOut, list = std::move(list), err]() mutable {
                auto s = w.lock();
                if (!s) return;
                if (signedOut) {
                    goToLogin(a);
                    return;
                }
                s->consolesLoading = false;
                s->consolesLoaded = true;
                if (ok) {
                    s->consoles = std::move(list);
                } else {
                    s->consolesError = err.empty() ? std::string("Could not load your consoles.") : err;
                }
            });
        });
    }

    void refresh(App& app) {
        std::weak_ptr<HomeState> w = st_;
        App* a = &app;
        if (tab_ == TabLibrary && !st_->titlesLoading) {
            st_->titlesLoading = true;
            app.runAsync([a, w] {
                std::string err;
                a->auth().ensureFresh(err);
                loadTitles(a, w, /*force=*/true);
            });
        } else if (tab_ == TabConsoles && !st_->consolesLoading) {
            st_->consolesLoading = true;
            app.runAsync([a, w] {
                std::string err;
                a->auth().ensureFresh(err);
                loadConsoles(a, w);
            });
        }
    }

    // ---- library ------------------------------------------------------------------------------

    const std::vector<const Title*>& visible() {
        // rebuild the filtered view when the source or filter changed
        if (viewVersion_ != st_->titlesVersion || viewFilter_ != filter_) {
            view_.clear();
            for (const auto& t : st_->titles) {
                if (filter_ == FilterGamePass && !t.gamePass) continue;
                // Owned: playable through a purchase ("stream your own game"), not via the plan or ads.
                if (filter_ == FilterOwned && !(t.hasEntitlement && !t.gamePass && !t.f2pOnly)) continue;
                if (filter_ == FilterFree && !t.f2p) continue;
                view_.push_back(&t);
            }
            viewVersion_ = st_->titlesVersion;
            viewFilter_ = filter_;
            if (focusTitle_ >= static_cast<int>(view_.size())) focusTitle_ = std::max(0, static_cast<int>(view_.size()) - 1);
        }
        return view_;
    }

    void navLibrary(App& app, NavKey key) {
        const auto& list = visible();
        const int n = static_cast<int>(list.size());
        switch (key) {
            case NavKey::Left:
                if (n && focusTitle_ % kCols > 0) --focusTitle_;
                break;
            case NavKey::Right:
                if (n && focusTitle_ % kCols < kCols - 1 && focusTitle_ + 1 < n) ++focusTitle_;
                break;
            case NavKey::Up:
                if (focusTitle_ >= kCols) focusTitle_ -= kCols;
                break;
            case NavKey::Down:
                if (focusTitle_ + kCols < n) focusTitle_ += kCols;
                else if (focusTitle_ / kCols < (n - 1) / kCols) focusTitle_ = n - 1;  // partial last row
                break;
            case NavKey::Accept:
                if (n > 0) {
                    const Title& t = *list[focusTitle_];
                    app.push(makeConnectingScreen(SessionKind::Cloud, t.titleId, t.name.empty() ? t.titleId : t.name, t.f2pOnly));
                } else if (!st_->titlesError.empty()) {
                    refresh(app);
                }
                break;
            case NavKey::Option:
                filter_ = (filter_ + 1) % FilterCount;
                focusTitle_ = 0;
                scroll_ = targetScroll_ = 0;
                break;
            case NavKey::Back: app.push(ui::makeExitConfirm()); break;
            default: break;
        }
    }

    void renderLibrary(App& app, Ui& ui, uint64_t now) {
        const HomeState& s = *st_;
        const auto& list = visible();
        // sub header: filter chips + count
        int x = kGridLeft;
        for (int f = 0; f < FilterCount; ++f) {
            const bool on = f == filter_;
            const int w = ui.textWidth(filterName(f), 24, on) + 44;
            ui.rect(x, 150, w, 48, on ? colors::Accent : colors::Panel, true, 24);
            ui.text(filterName(f), x + w / 2, 150 + (48 - ui.lineHeight(24, on)) / 2, 24, on ? colors::White : colors::TextDim,
                    Align::Center, on);
            x += w + 14;
        }
        if (s.titlesLoaded && s.titlesError.empty()) {
            const std::string count = std::to_string(list.size()) + (list.size() == 1 ? " game" : " games");
            ui.text(count, x + 16, 150 + (48 - ui.lineHeight(24)) / 2, 24, colors::TextDim);
        }
        if (s.titlesLoading && !s.titles.empty()) ui.spinner(Ui::kWidth - kGridLeft - 24, 174, 18, now);

        // focused title details
        if (!list.empty()) {
            const Title& t = *list[focusTitle_];
            int w = ui.textEllipsized(t.name, kGridLeft, 220, 1300, 34, colors::Text, Align::Left, true);
            if (!t.publisher.empty())
                ui.textEllipsized(t.publisher, kGridLeft + w + 20, 228, 1700 - w - 20, 24, colors::TextDim);
        }

        if (!s.hasCloud && s.titlesLoaded) {
            emptyState(ui, "No cloud gaming access",
                       "This account has no Xbox Cloud Gaming access (a Game Pass plan with cloud gaming, or the "
                       "free-to-play offering in your region). You can still stream from your own console under My consoles.");
            hintsLibrary(app, ui, false);
            return;
        }
        if (!s.titlesLoaded || (s.titlesLoading && s.titles.empty())) {
            ui.spinner(Ui::kWidth / 2, 560, 48, now);
            ui.text("Loading your cloud library\xE2\x80\xA6", Ui::kWidth / 2, 640, 30, colors::TextDim, Align::Center);
            hintsLibrary(app, ui, false);
            return;
        }
        if (!s.titlesError.empty()) {
            emptyState(ui, "Couldn't load the library", s.titlesError);
            ui::hintBar(app, ui, {{PadIcon::Cross, "Retry"}, {PadIcon::R1, "Next tab"}, {PadIcon::Circle, "Exit"}});
            return;
        }
        if (list.empty()) {
            emptyState(ui, "Nothing here", filter_ == FilterAll ? "No cloud titles are available for this account."
                                                                : "No titles match this filter. Press Y / \xE2\x96\xB3 to change it.");
            hintsLibrary(app, ui, false);
            return;
        }

        // grid
        ui.setClip(0, kGridTop - 12, Ui::kWidth, kViewH + 8);
        const int scroll = static_cast<int>(std::lround(scroll_));
        const int firstRow = std::max(0, scroll / kRowPitch);
        const int lastRow = (scroll + kViewH) / kRowPitch + 1;  // +1 row prefetch
        const int n = static_cast<int>(list.size());
        for (int row = firstRow; row <= lastRow; ++row) {
            for (int col = 0; col < kCols; ++col) {
                const int i = row * kCols + col;
                if (i >= n) break;
                const Title& t = *list[i];
                const int tx = kGridLeft + col * (kTileW + kGap);
                const int ty = kGridTop + row * kRowPitch - scroll;
                SDL_Texture* img = ui.remoteImage(normalizeUrl(t.imageUrl), kTileW, kImageH);
                if (ty >= kGridBottom) continue;  // prefetch only
                std::string badge;
                if (t.gamePass) badge = "Game Pass";
                else if (t.hasEntitlement && !t.f2pOnly) badge = "Owned";
                else if (t.f2p) badge = "Free";
                ui.tile(img, t.name, badge, tx, ty, kTileW, kTileH, i == focusTitle_);
            }
        }
        ui.clearClip();
        // scroll indicator
        const int rows = (n + kCols - 1) / kCols;
        const int totalH = rows * kRowPitch;
        const int viewH = kViewH - 30;
        if (totalH > kViewH) {
            const int barH = std::max(60, viewH * kViewH / totalH);
            const int barY = kGridTop + static_cast<int>((viewH - barH) * (scroll_ / std::max(1, totalH - kViewH)));
            ui.rect(Ui::kWidth - 40, kGridTop, 6, viewH, withAlpha(colors::Panel, 200), true, 3);
            ui.rect(Ui::kWidth - 40, std::min(kGridTop + viewH - barH, barY), 6, barH, colors::AccentBright, true, 3);
        }
        hintsLibrary(app, ui, true);
    }

    void hintsLibrary(App& app, Ui& ui, bool canPlay) {
        std::vector<ui::Hint> h;
        h.push_back({PadIcon::L1, ""});
        h.push_back({PadIcon::R1, "Switch tab"});
        h.push_back({PadIcon::Triangle, "Filter"});
        if (canPlay) h.push_back({PadIcon::Cross, "Play"});
        h.push_back({PadIcon::Circle, "Exit"});
        ui::hintBar(app, ui, h);
    }

    void emptyState(Ui& ui, const std::string& title, const std::string& msg) {
        ui.text(title, Ui::kWidth / 2, 470, 40, colors::Text, Align::Center, true);
        const int w = 1100;
        ui.textWrapped(msg, (Ui::kWidth - w) / 2, 540, w, 28, colors::TextDim);
    }

    // ---- consoles -----------------------------------------------------------------------------

    void navConsoles(App& app, NavKey key) {
        const int n = static_cast<int>(st_->consoles.size());
        switch (key) {
            case NavKey::Left:
                if (focusConsole_ > 0) --focusConsole_;
                break;
            case NavKey::Right:
                if (focusConsole_ + 1 < n) ++focusConsole_;
                break;
            case NavKey::Up:
                if (focusConsole_ >= 3) focusConsole_ -= 3;
                break;
            case NavKey::Down:
                if (focusConsole_ + 3 < n) focusConsole_ += 3;
                break;
            case NavKey::Accept:
                if (n > 0) {
                    const Console& c = st_->consoles[focusConsole_];
                    app.push(makeConnectingScreen(SessionKind::Home, c.serverId, c.name.empty() ? "your console" : c.name));
                } else {
                    refresh(app);
                }
                break;
            case NavKey::Option: refresh(app); break;
            case NavKey::Back: app.push(ui::makeExitConfirm()); break;
            default: break;
        }
    }

    void renderConsoles(App& app, Ui& ui, uint64_t now) {
        const HomeState& s = *st_;
        ui.text("Stream from your own Xbox (Remote Play)", kGridLeft, 160, 28, colors::TextDim);
        if (s.consolesLoading && !s.consoles.empty()) ui.spinner(Ui::kWidth - kGridLeft - 24, 176, 18, now);
        std::vector<ui::Hint> hints = {{PadIcon::L1, ""}, {PadIcon::R1, "Switch tab"}, {PadIcon::Triangle, "Refresh"}};

        if (!s.hasHome && s.consolesLoaded) {
            emptyState(ui, "Remote Play unavailable",
                       "This account did not receive an Xbox Remote Play token. Sign in with the account that owns "
                       "the console.");
        } else if (!s.consolesLoaded || (s.consolesLoading && s.consoles.empty())) {
            ui.spinner(Ui::kWidth / 2, 520, 48, now);
            ui.text("Looking for your consoles\xE2\x80\xA6", Ui::kWidth / 2, 600, 30, colors::TextDim, Align::Center);
        } else if (!s.consolesError.empty()) {
            emptyState(ui, "Couldn't load your consoles", s.consolesError);
            hints.push_back({PadIcon::Cross, "Retry"});
        } else if (s.consoles.empty()) {
            emptyState(ui, "No consoles found",
                       "Turn on remote features on your Xbox: Settings \xE2\x80\xBA Devices & connections \xE2\x80\xBA "
                       "Remote features, and use the \"Sleep\" power mode.");
        } else {
            const int cw = 520, ch = 300, gap = 40;
            for (size_t i = 0; i < s.consoles.size(); ++i) {
                const Console& c = s.consoles[i];
                const int col = static_cast<int>(i % 3), row = static_cast<int>(i / 3);
                const int x = kGridLeft + col * (cw + gap), y = 240 + row * (ch + gap);
                if (y > 1000) break;
                const bool f = static_cast<int>(i) == focusConsole_;
                if (f) ui.outline(x - 6, y - 6, cw + 12, ch + 12, colors::AccentBright, 4, 26);
                ui.rect(x, y, cw, ch, f ? colors::PanelHi : colors::Panel, true, 20);
                // console glyph: tower with a power light
                const int gx = x + 48, gy = y + 60;
                ui.rect(gx, gy, 96, 150, Color{20, 22, 21, 255}, true, 12);
                ui.outline(gx, gy, 96, 150, Color{70, 78, 74, 255}, 2, 12);
                ui.circle(gx + 48, gy + 26, 14, Color{40, 44, 42, 255});
                const bool on = c.powerState == "On";
                const bool standby = c.powerState == "ConnectedStandby";
                const Color dot = on ? colors::AccentBright : standby ? colors::Warning : colors::TextDim;
                ui.circle(gx + 18, gy + 130, 6, dot);

                const int tx = x + 180;
                ui.textEllipsized(c.name.empty() ? "Xbox" : c.name, tx, y + 64, cw - 200, 34, colors::Text, Align::Left,
                                  true);
                ui.text(ui::prettyConsoleType(c.consoleType), tx, y + 116, 26, colors::TextDim);
                const std::string ps = on ? "On" : standby ? "Standby (instant-on)" : (c.powerState.empty() ? "Unknown" : c.powerState);
                ui.circle(tx + 8, y + 186, 7, dot);
                ui.text(ps, tx + 26, y + 186 - ui.lineHeight(24) / 2, 24, dot);
                if (f) ui.text("Press to stream", tx, y + 230, 22, withAlpha(colors::TextDim, 200));
            }
            hints.push_back({PadIcon::Cross, "Stream"});
        }
        hints.push_back({PadIcon::Circle, "Exit"});
        ui::hintBar(app, ui, hints);
    }

    // ---- header -------------------------------------------------------------------------------

    void renderHeader(App& app, Ui& ui) {
        ui::logo(ui, 96, 40, true);
        // tabs centred
        const int ts = 30;
        int total = 0;
        for (int t = 0; t < TabCount; ++t) total += ui.textWidth(tabName(t), ts, true) + (t ? 64 : 0);
        int x = (Ui::kWidth - total) / 2;
        const bool kb = app.gamepad().lastInputKeyboard();
        if (kb) ui.keyCap(ui::keyFor(PadIcon::L1), x - 76, 66, 40);
        else ui.padIcon(PadIcon::L1, x - 76, 66, 40);
        for (int t = 0; t < TabCount; ++t) {
            const bool on = t == tab_;
            const int w = ui.textWidth(tabName(t), ts, true);
            ui.text(tabName(t), x + w / 2, 66 - ui.lineHeight(ts, true) / 2, ts, on ? colors::White : colors::TextDim,
                    Align::Center, true);
            if (on) ui.rect(x, 96, w, 5, colors::AccentBright, true, 2);
            x += w + 64;
        }
        x -= 64;
        if (kb) ui.keyCap(ui::keyFor(PadIcon::R1), x + 24, 66, 40);
        else ui.padIcon(PadIcon::R1, x + 24, 66, 40);

        // gamertag + clock
        char clock[16] = {0};
        std::time_t tt = std::time(nullptr);
        std::tm lt{};
        if (localtime_r(&tt, &lt)) std::strftime(clock, sizeof(clock), "%H:%M", &lt);
        int rx = Ui::kWidth - 96;
        rx -= ui.text(clock, rx, 66 - ui.lineHeight(28) / 2, 28, colors::Text, Align::Right);
        const std::string tag = app.config().gamertag();
        if (!tag.empty()) {
            rx -= 32;
            const int tw = ui.textWidth(tag, 26, true);
            ui.text(tag, rx, 66 - ui.lineHeight(26, true) / 2, 26, colors::Text, Align::Right, true);
            const int ax = rx - tw - 38;
            ui.circle(ax, 66, 22, colors::Accent);
            ui.text(tag.substr(0, 1), ax, 66 - ui.lineHeight(24, true) / 2, 24, colors::White, Align::Center, true);
        }
    }

    std::shared_ptr<HomeState> st_ = std::make_shared<HomeState>();
    ui::SettingsPanel settings_;
    int tab_ = TabLibrary;
    int filter_ = FilterAll;
    int focusTitle_ = 0, focusConsole_ = 0;
    float scroll_ = 0, targetScroll_ = 0;

    std::vector<const Title*> view_;
    uint64_t viewVersion_ = static_cast<uint64_t>(-1);
    int viewFilter_ = -1;
};

}  // namespace

std::unique_ptr<Screen> makeHomeScreen() { return std::make_unique<HomeScreen>(); }

}  // namespace xc
