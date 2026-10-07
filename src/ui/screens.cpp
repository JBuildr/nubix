// Nubix — UI screens: shared widgets, confirmation dialog, settings panel / screen and
// the public factories. Login, Home, Connecting and Stream live in screen_*.cpp.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/screens.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.hpp"
#include "platform/platform.hpp"
#include "ui/app.hpp"
#include "ui/screens_internal.hpp"
#include "ui/ui.hpp"

namespace xc {
namespace ui {

namespace {
constexpr int kHintIcon = 40;
constexpr int kHintText = 24;
constexpr int kHintY = 1022;

// Width padIcon() will use for an icon (mirrors Ui::padIcon's metrics).
int padIconWidth(Ui& ui, PadIcon icon, int size) {
    auto pill = [&](const char* label) {
        const int fs = std::max(12, size * 9 / 20);
        return std::max(size + size / 3, ui.textWidth(label, fs, true) + size / 2);
    };
    switch (icon) {
        case PadIcon::L1: return pill("L1");
        case PadIcon::R1: return pill("R1");
        case PadIcon::PS: return pill("PS");
        case PadIcon::Options: return size + size / 3;
        case PadIcon::Touchpad: return size * 2;
        default: return size;
    }
}

int keyCapWidth(Ui& ui, const std::string& label, int size) {
    const int fs = std::max(12, size * 9 / 20);
    return std::max(size, ui.textWidth(label, fs, true) + size / 2);
}

int hintWidth(App& app, Ui& ui, const Hint& h) {
    const bool kb = app.gamepad().lastInputKeyboard();
    const int iw = kb ? keyCapWidth(ui, keyFor(h.icon), kHintIcon) : padIconWidth(ui, h.icon, kHintIcon);
    return h.label.empty() ? iw : iw + 12 + ui.textWidth(h.label, kHintText);
}
}  // namespace

const char* keyFor(PadIcon icon) {
    switch (icon) {
        case PadIcon::Cross: return "Enter";
        case PadIcon::Circle: return "Esc";
        case PadIcon::Square: return "X";
        case PadIcon::Triangle: return "Y";
        case PadIcon::L1: return "Q";
        case PadIcon::R1: return "E";
        case PadIcon::Options: return "N";
        case PadIcon::Touchpad: return "V";
        case PadIcon::PS: return "F1";
    }
    return "?";
}

int hintItem(App& app, Ui& ui, const Hint& h, int x, int cy) {
    const bool kb = app.gamepad().lastInputKeyboard();
    int w = kb ? ui.keyCap(keyFor(h.icon), x, cy, kHintIcon) : ui.padIcon(h.icon, x, cy, kHintIcon);
    if (h.label.empty()) return w;  // glyph paired with the next hint (e.g. "L1 R1 Switch tab")
    w += 12;
    w += ui.text(h.label, x + w, cy - ui.lineHeight(kHintText) / 2, kHintText, colors::TextDim);
    return w;
}

void hintBar(App& app, Ui& ui, const std::vector<Hint>& hints) {
    if (hints.empty()) return;
    auto gapAfter = [](const Hint& h) { return h.label.empty() ? 10 : 40; };
    int total = 0;
    for (auto& h : hints) total += hintWidth(app, ui, h) + gapAfter(h);
    total -= gapAfter(hints.back());
    int x = Ui::kWidth - 96 - total;
    for (auto& h : hints) x += hintItem(app, ui, h, x, kHintY) + gapAfter(h);
}

void background(Ui& ui) {
    ui.gradient(0, 0, Ui::kWidth, 420, Color{16, 32, 70, 255}, colors::Background);
}

void logo(Ui& ui, int x, int y, bool compact) {
    const int r = 26;
    // app icon in miniature: navy rounded square with a two-tone blue X
    ui.rect(x, y, 2 * r, 2 * r, Color{18, 38, 82, 255}, true, 14);
    const float c = r, k = r * 0.48f;
    ui.line(x + c + k, y + c - k, x + c - k, y + c + k, 9.0f, colors::Accent);
    ui.line(x + c - k, y + c - k, x + c + k, y + c + k, 9.0f, colors::AccentBright);
    const int tx = x + 2 * r + 16;
    int w = ui.text("Nubix", tx, y + r - ui.lineHeight(30, true) / 2, 30, colors::Text, Align::Left, true);
    if (!compact) ui.text("for Xbox Cloud Gaming", tx + w + 14, y + r - ui.lineHeight(30) / 2, 30, colors::TextDim);
}

void errorBadge(Ui& ui, int cx, int cy, int radius) {
    ui.circle(cx, cy, radius, colors::Error);
    const int s = radius * 13 / 10;
    ui.text("!", cx, cy - ui.lineHeight(s, true) / 2, s, colors::White, Align::Center, true);
}

std::string formatDuration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    char buf[32];
    if (seconds >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", static_cast<int>(seconds / 3600),
                      static_cast<int>((seconds / 60) % 60), static_cast<int>(seconds % 60));
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", static_cast<int>(seconds / 60), static_cast<int>(seconds % 60));
    return buf;
}

std::string prettyConsoleType(const std::string& t) {
    if (t == "XboxSeriesX") return "Xbox Series X";
    if (t == "XboxSeriesS") return "Xbox Series S";
    if (t == "XboxOne") return "Xbox One";
    if (t == "XboxOneS") return "Xbox One S";
    if (t == "XboxOneX") return "Xbox One X";
    if (t.empty()) return "Xbox";
    // generic: insert spaces before capitals ("XboxFooBar" -> "Xbox Foo Bar")
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && std::isupper(static_cast<unsigned char>(t[i])) &&
            !std::isupper(static_cast<unsigned char>(t[i - 1])))
            out.push_back(' ');
        out.push_back(t[i]);
    }
    return out;
}

// ---- confirmation dialog ------------------------------------------------------------------

namespace {

class ConfirmScreen : public Screen {
public:
    ConfirmScreen(std::string title, std::string message, std::string confirm, std::function<void(App&)> fn)
        : title_(std::move(title)), message_(std::move(message)), confirm_(std::move(confirm)), fn_(std::move(fn)) {}

    bool isOverlay() const override { return true; }

    void onNav(App& app, NavKey key) override {
        switch (key) {
            case NavKey::Left: focus_ = 0; break;
            case NavKey::Right: focus_ = 1; break;
            case NavKey::Back: app.pop(); break;
            case NavKey::Accept:
                app.pop();
                if (focus_ == 1 && fn_) fn_(app);
                break;
            default: break;
        }
    }
    void update(App& app, uint32_t dtMs) override {
        (void)app;
        (void)dtMs;
    }
    void render(App& app, Ui& ui) override {
        ui.rect(0, 0, Ui::kWidth, Ui::kHeight, withAlpha(colors::Black, 170));
        // cover the (dimmed) hint bar of the screen below so only ours is readable
        ui.rect(0, 990, Ui::kWidth, 70, Color{5, 7, 6, 255});
        const int w = 900, h = 400;
        const int x = (Ui::kWidth - w) / 2, y = (Ui::kHeight - h) / 2;
        ui.rect(x, y, w, h, colors::Panel, true, 24);
        ui.outline(x, y, w, h, withAlpha(colors::AccentBright, 90), 2, 24);
        ui.text(title_, x + 56, y + 48, 42, colors::Text, Align::Left, true);
        ui.textWrapped(message_, x + 56, y + 122, w - 112, 28, colors::TextDim);
        const int bw = 300, bh = 72, by = y + h - bh - 48;
        ui.button("Cancel", x + w - 56 - 2 * bw - 24, by, bw, bh, focus_ == 0);
        ui.button(confirm_, x + w - 56 - bw, by, bw, bh, focus_ == 1);
        hintBar(app, ui, {{PadIcon::Cross, "Select"}, {PadIcon::Circle, "Cancel"}});
    }

private:
    std::string title_, message_, confirm_;
    std::function<void(App&)> fn_;
    int focus_ = 1;
};

}  // namespace

std::unique_ptr<Screen> makeConfirmScreen(const std::string& title, const std::string& message,
                                          const std::string& confirmLabel, std::function<void(App&)> onConfirm) {
    return std::make_unique<ConfirmScreen>(title, message, confirmLabel, std::move(onConfirm));
}

std::unique_ptr<Screen> makeExitConfirm() {
    return makeConfirmScreen("Exit Nubix?", "Any running stream will be stopped.", "Exit",
                             [](App& app) { app.quit(); });
}

// ---- settings panel -----------------------------------------------------------------------

namespace {

// Fallback gssv region names (offeringSettings.regions[].name), used only until Auth has stored
// the account's real region list (Tokens::xcloudRegions, filled by authorizeStreaming). Auth
// falls back to the offering's default region when the stored name is not offered.
const char* const kFallbackRegions[] = {"WestEurope",     "NorthEurope",   "UKSouth",        "FranceCentral",
                                        "GermanyWestCentral", "SwedenCentral", "EastUS", "EastUS2",
                                        "NorthCentralUS", "SouthCentralUS", "WestUS",         "WestUS2",
                                        "CanadaCentral",  "BrazilSouth",   "JapanEast",      "KoreaCentral",
                                        "SoutheastAsia",  "AustraliaEast", "AustraliaSouthEast"};

// Region choices for the Settings row: "" (Automatic = server default) followed by the account's
// xgpuweb regions (or the f2p ones, or the fallback list).
std::vector<std::string> regionChoices(const Tokens& t) {
    std::vector<std::string> out{std::string()};
    const std::vector<Region>& src = !t.xcloudRegions.empty() ? t.xcloudRegions : t.xcloudF2pRegions;
    for (const auto& r : src)
        if (!r.name.empty() && std::find(out.begin(), out.end(), r.name) == out.end()) out.push_back(r.name);
    if (out.size() == 1)
        for (const char* n : kFallbackRegions) out.emplace_back(n);
    return out;
}

const char* const kResolutions[] = {"720", "1080", "1080HQ"};

enum Row { RowResolution, RowBitrate, RowRegion, RowF2p, RowDeadzone, RowStats, RowSignOut, RowExit, RowCount };

}  // namespace

struct SettingsPanel::Impl {
    int focus = 0;

    // Per-frame reads go through cached snapshots (Config is shared with the worker threads);
    // refreshed when the config revision changes.
    uint64_t cachedRev = ~uint64_t(0);
    Settings settings;
    std::vector<Region> cloudRegions;
    std::vector<std::string> regions;  // regionChoices()
    std::string gamertag;
    // getifaddrs() is not a per-frame call: refresh the IP about once per second.
    std::string ip;
    uint64_t ipAt = 0;

    void refresh(App& app) {
        const uint64_t rev = app.config().revision();
        if (rev == cachedRev) return;
        cachedRev = rev;
        settings = app.config().settings();
        const Tokens t = app.config().tokens();
        cloudRegions = t.xcloudRegions;
        regions = regionChoices(t);
        gamertag = t.gamertag;
    }

    const std::string& localIp() {
        const uint64_t now = platform::monotonicMs();
        if (ipAt == 0 || now - ipAt >= 1000) {
            ip = platform::localIpv4();
            ipAt = now;
        }
        return ip;
    }

    static void save(App& app) {
        Config* cfg = &app.config();
        app.runAsync([cfg] {
            if (!cfg->save()) XC_LOGW("settings: saving %s failed", cfg->path().c_str());
        });
    }

    std::string label(int row) const {
        switch (row) {
            case RowResolution: return "Stream resolution";
            case RowBitrate: return "Maximum bitrate";
            case RowRegion: return "Server region";
            case RowF2p: return "Free-to-play fallback";
            case RowDeadzone: return "Extra stick deadzone";
            case RowStats: return "Show stream statistics";
            case RowSignOut: return "Sign out";
            case RowExit: return "Exit Nubix";
        }
        return "";
    }

    std::string value(App& app, int row) const {
        const Settings& s = settings;
        char buf[64];
        switch (row) {
            case RowResolution:
                if (s.resolution == "720") return "720p";
                if (s.resolution == "1080HQ") return "1080p (high quality)";
                return "1080p";
            case RowBitrate:
                std::snprintf(buf, sizeof(buf), "%d Mbps", (s.bitrateKbps + 500) / 1000);
                return buf;
            case RowRegion: {
                if (s.region.empty()) return "Automatic";
                for (const auto& r : cloudRegions)
                    if (r.name == s.region) return s.region;
                // Not (yet) known to be offered: Auth uses the server default in that case.
                return cloudRegions.empty() ? s.region : s.region + " (unavailable)";
            }
            case RowF2p: return s.f2pFallback ? "On" : "Off";
            case RowDeadzone:
                if (s.stickDeadzone <= 0) return "Off";
                std::snprintf(buf, sizeof(buf), "%d %%", s.stickDeadzone);
                return buf;
            case RowStats: return app.statsOverlay() ? "On" : "Off";
            default: return "";
        }
    }

    std::string help(int row) const {
        switch (row) {
            case RowResolution: return "720p decodes fastest. 1080p (high quality) asks the server for a higher-quality profile.";
            case RowBitrate: return "Upper limit the server may use. Lower it on Wi-Fi or if the picture stutters.";
            case RowRegion: return "Automatic uses the region Xbox assigns to your account.";
            case RowF2p: return "Retry with the free-to-play offering when a title is not in your Game Pass library.";
            case RowDeadzone: return "Ignore small stick movements (helps with drifting sticks).";
            case RowStats: return "Resolution, frame rate, bitrate and decode time while streaming.";
            case RowSignOut: return "Forget the stored Microsoft account on this console.";
            case RowExit: return "Close the app.";
        }
        return "";
    }

    // dir: -1 / +1 (Left / Right), 0 = Accept
    void change(App& app, int row, int dir) {
        refresh(app);
        Settings s = settings;  // edit a copy, publish it with one locked write
        const int step = dir == 0 ? 1 : dir;
        auto commit = [&] {
            app.config().setSettings(s);
            refresh(app);
        };
        switch (row) {
            case RowResolution: {
                int i = 1;
                for (int k = 0; k < 3; ++k)
                    if (s.resolution == kResolutions[k]) i = k;
                i = (i + step + 3) % 3;
                s.resolution = kResolutions[i];
                commit();
                save(app);
                break;
            }
            case RowBitrate: {
                int mb = (s.bitrateKbps + 500) / 1000 + step;
                if (dir == 0 && mb > 20) mb = 5;
                s.bitrateKbps = std::max(5, std::min(20, mb)) * 1000;
                commit();
                save(app);
                break;
            }
            case RowRegion: {
                const std::vector<std::string> choices = regions;
                const int n = static_cast<int>(choices.size());
                int i = 0;
                for (int k = 0; k < n; ++k)
                    if (s.region == choices[static_cast<size_t>(k)]) i = k;
                i = (i + step + n) % n;
                s.region = choices[static_cast<size_t>(i)];
                commit();
                // Re-pick the cloud base URIs for the new region (no network) and persist.
                Auth* auth = &app.auth();
                app.runAsync([auth] { auth->applyRegion(); });
                break;
            }
            case RowF2p:
                s.f2pFallback = !s.f2pFallback;
                commit();
                save(app);
                break;
            case RowDeadzone: {
                int v = s.stickDeadzone + step * 5;
                if (dir == 0 && v > 30) v = 0;
                s.stickDeadzone = std::max(0, std::min(30, v));
                commit();
                save(app);
                break;
            }
            case RowStats: app.setStatsOverlay(!app.statsOverlay()); break;
            case RowSignOut:
                if (dir != 0) break;
                app.push(makeConfirmScreen("Sign out?", "You will need to sign in again with a code to stream.",
                                           "Sign out", [](App& a) {
                                               a.runAsync([&a] {
                                                   a.auth().logout();
                                                   a.postToMain([&a] {
                                                       a.toast("Signed out");
                                                       a.resetTo(makeLoginScreen());
                                                   });
                                               });
                                           }));
                break;
            case RowExit:
                if (dir == 0) app.push(makeExitConfirm());
                break;
        }
    }
};

SettingsPanel::SettingsPanel() : d_(new Impl) {}
SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::resetFocus() { d_->focus = 0; }

bool SettingsPanel::onNav(App& app, NavKey key) {
    d_->refresh(app);
    switch (key) {
        case NavKey::Up: d_->focus = (d_->focus + RowCount - 1) % RowCount; return true;
        case NavKey::Down: d_->focus = (d_->focus + 1) % RowCount; return true;
        case NavKey::Left: d_->change(app, d_->focus, -1); return true;
        case NavKey::Right: d_->change(app, d_->focus, +1); return true;
        case NavKey::Accept: d_->change(app, d_->focus, 0); return true;
        default: return false;
    }
}

void SettingsPanel::render(App& app, Ui& ui, int top) {
    d_->refresh(app);
    const int x = 360, w = 1200, h = 74, gap = 8;
    int y = top;
    for (int row = 0; row < RowCount; ++row) {
        const bool f = row == d_->focus;
        const bool action = row == RowSignOut || row == RowExit;
        if (row == RowSignOut) y += 16;  // separate actions from settings
        ui.rect(x, y, w, h, f ? colors::PanelHi : colors::Panel, true, 14);
        if (f) {
            ui.outline(x - 4, y - 4, w + 8, h + 8, colors::AccentBright, 3, 18);
            ui.rect(x, y + 14, 6, h - 28, colors::AccentBright, true, 3);
        }
        const int ty = y + (h - ui.lineHeight(28)) / 2;
        const Color lc = action && row == RowSignOut ? Color{255, 140, 130, 255} : colors::Text;
        ui.text(d_->label(row), x + 36, ty, 28, lc, Align::Left, f);
        if (!action) {
            const std::string v = d_->value(app, row);
            const int vx = x + w - 36;
            if (f) {
                const int vw = ui.textWidth(v, 28, true);
                ui.text("\xE2\x80\xBA", vx, ty, 28, colors::AccentBright, Align::Right, true);   // ›
                ui.text(v, vx - 34, ty, 28, colors::White, Align::Right, true);
                ui.text("\xE2\x80\xB9", vx - 34 - vw - 18, ty, 28, colors::AccentBright, Align::Right, true);  // ‹
            } else {
                ui.text(v, vx, ty, 28, colors::TextDim, Align::Right);
            }
        }
        y += h + gap;
    }
    // help line for the focused row + account info
    ui.text(d_->help(d_->focus), x, y + 16, 24, colors::TextDim);
    std::string info;
    const std::string& tag = d_->gamertag;
    info = tag.empty() ? std::string("Signed in") : "Signed in as " + tag;
    info += "   \xC2\xB7   v" XC_VERSION;
    const std::string ip = d_->localIp();
    if (!ip.empty()) info += "   \xC2\xB7   IP " + ip;
    if (app.gamepad().connected()) info += "   \xC2\xB7   " + app.gamepad().name();
    ui.text(info, x, y + 60, 22, withAlpha(colors::TextDim, 200));
}

}  // namespace ui

// ---- standalone settings screen -----------------------------------------------------------

namespace {

class SettingsScreen : public Screen {
public:
    void onNav(App& app, NavKey key) override {
        if (key == NavKey::Back) {
            app.pop();
            return;
        }
        panel_.onNav(app, key);
    }
    void update(App& app, uint32_t dtMs) override {
        (void)app;
        (void)dtMs;
    }
    void render(App& app, Ui& ui) override {
        ui::background(ui);
        ui::logo(ui, 96, 56);
        ui.text("Settings", 360, 150, 48, colors::Text, Align::Left, true);
        panel_.render(app, ui, 236);
        ui::hintBar(app, ui, {{PadIcon::Cross, "Change"}, {PadIcon::Circle, "Back"}});
    }

private:
    ui::SettingsPanel panel_;
};

}  // namespace

std::unique_ptr<Screen> makeSettingsScreen() { return std::make_unique<SettingsScreen>(); }

std::unique_ptr<Screen> makeStreamScreen() { return ui::makeStreamScreenNamed(""); }

}  // namespace xc
