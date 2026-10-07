// Nubix — Login screen: Microsoft device-code flow with QR code and countdown.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <memory>

#include "core/log.hpp"
#include "platform/platform.hpp"
#include "ui/app.hpp"
#include "ui/screens.hpp"
#include "ui/screens_internal.hpp"
#include "ui/ui.hpp"

namespace xc {
namespace {

constexpr int kMaxTransientPollErrors = 5;

struct LoginState {
    enum Phase { Idle, Requesting, Code, Authorizing, Error } phase = Idle;
    DeviceCode dc;
    uint64_t expiresAt = 0, nextPoll = 0;
    bool polling = false;
    int transientErrors = 0;
    std::string error;
};

std::string displayUri(const std::string& uri) {
    std::string s = uri;
    for (const char* p : {"https://", "http://"})
        if (s.rfind(p, 0) == 0) s = s.substr(std::char_traits<char>::length(p));
    if (s.rfind("www.", 0) == 0) s = s.substr(4);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

class LoginScreen : public Screen {
public:
    void onEnter(App& app) override {
        if (st_->phase == LoginState::Idle) requestCode(app);
    }

    void onNav(App& app, NavKey key) override {
        switch (key) {
            case NavKey::Accept:
                if (st_->phase == LoginState::Error || st_->phase == LoginState::Code) requestCode(app);
                break;
            case NavKey::Back: app.push(ui::makeExitConfirm()); break;
            default: break;
        }
    }

    void update(App& app, uint32_t dtMs) override {
        (void)dtMs;
        LoginState& s = *st_;
        if (s.phase != LoginState::Code) return;
        const uint64_t now = platform::monotonicMs();
        if (now >= s.expiresAt && !s.polling) {
            XC_LOGI("login: device code expired, requesting a new one");
            requestCode(app);
            return;
        }
        if (!s.polling && now >= s.nextPoll) poll(app);
    }

    void render(App& app, Ui& ui) override {
        const LoginState& s = *st_;
        const uint64_t now = platform::monotonicMs();
        ui::background(ui);
        ui::logo(ui, 96, 56);
        // Shown on every sign-in screen: this is a third-party client.
        ui.text("Unofficial client \xC2\xB7 not affiliated with Microsoft or Sony", 96,
                1022 - ui.lineHeight(20) / 2, 20, withAlpha(colors::TextDim, 200));  // hint bar row

        switch (s.phase) {
            case LoginState::Idle:
            case LoginState::Requesting:
                ui.spinner(Ui::kWidth / 2, 470, 56, now);
                ui.text("Contacting Microsoft\xE2\x80\xA6", Ui::kWidth / 2, 570, 34, colors::Text, Align::Center);
                ui::hintBar(app, ui, {{PadIcon::Circle, "Exit"}});
                return;
            case LoginState::Authorizing:
                ui.spinner(Ui::kWidth / 2, 470, 56, now);
                ui.text("Signing in to Xbox\xE2\x80\xA6", Ui::kWidth / 2, 570, 34, colors::Text, Align::Center);
                ui.text("Getting your cloud gaming and remote play access", Ui::kWidth / 2, 626, 26, colors::TextDim,
                        Align::Center);
                ui::hintBar(app, ui, {{PadIcon::Circle, "Exit"}});
                return;
            case LoginState::Error: {
                ui::errorBadge(ui, Ui::kWidth / 2, 380, 52);
                ui.text("Sign-in failed", Ui::kWidth / 2, 470, 44, colors::Text, Align::Center, true);
                const int w = 1100;
                ui.textWrapped(s.error, (Ui::kWidth - w) / 2, 548, w, 28, colors::TextDim);
                ui::hintBar(app, ui, {{PadIcon::Cross, "Try again"}, {PadIcon::Circle, "Exit"}});
                return;
            }
            case LoginState::Code: break;
        }

        // ---- code view ----
        const int lx = 160;
        ui.text("Sign in", lx, 190, 64, colors::Text, Align::Left, true);
        ui.text("Use the Microsoft account with your Game Pass plan or your Xbox consoles.", lx, 278, 26,
                colors::TextDim);

        auto step = [&](int n, int y, const std::string& caption) {
            ui.circle(lx + 24, y + 24, 24, colors::Accent);
            ui.text(std::to_string(n), lx + 24, y + 24 - ui.lineHeight(26, true) / 2, 26, colors::White, Align::Center,
                    true);
            ui.text(caption, lx + 72, y + 24 - ui.lineHeight(28) / 2, 28, colors::TextDim);
        };
        step(1, 370, "On your phone or computer, go to");
        ui.text(displayUri(s.dc.verificationUri), lx + 72, 420, 50, colors::White, Align::Left, true);

        step(2, 530, "Enter this code");
        // code box with letter-spaced characters
        const int cs = 84, spacing = 16;
        int codeW = 0;
        for (char c : s.dc.userCode) codeW += ui.textWidth(std::string(1, c), cs, true) + spacing;
        codeW = std::max(0, codeW - spacing);
        const int boxX = lx + 72, boxY = 586, boxW = std::max(520, codeW + 96), boxH = 148;
        ui.rect(boxX, boxY, boxW, boxH, colors::Panel, true, 20);
        ui.outline(boxX, boxY, boxW, boxH, withAlpha(colors::AccentBright, 140), 3, 20);
        int cx = boxX + (boxW - codeW) / 2;
        const int cy = boxY + (boxH - ui.lineHeight(cs, true)) / 2;
        for (char c : s.dc.userCode) {
            const std::string ch(1, c);
            ui.text(ch, cx, cy, cs, colors::White, Align::Left, true);
            cx += ui.textWidth(ch, cs, true) + spacing;
        }

        // waiting + countdown
        ui.spinner(lx + 96, 820, 24, now);
        ui.text("Waiting for you to sign in\xE2\x80\xA6", lx + 140, 820 - ui.lineHeight(28) / 2, 28, colors::Text);
        const int64_t left = s.expiresAt > now ? static_cast<int64_t>((s.expiresAt - now) / 1000) : 0;
        ui.text("Code expires in " + ui::formatDuration(left) + " \xC2\xB7 a new one is created automatically",
                lx + 72, 868, 24, colors::TextDim);

        // QR
        const int qs = 420, qx = 1260, qy = 300;
        ui.qr(s.dc.qrUrl.empty() ? s.dc.verificationUri : s.dc.qrUrl, qx, qy, qs);
        ui.text("Scan to open the sign-in page", qx + qs / 2, qy + qs + 40, 26, colors::TextDim, Align::Center);
        ui.text("(the code is filled in for you)", qx + qs / 2, qy + qs + 78, 22, withAlpha(colors::TextDim, 190),
                Align::Center);

        ui::hintBar(app, ui, {{PadIcon::Cross, "New code"}, {PadIcon::Circle, "Exit"}});
    }

private:
    void requestCode(App& app) {
        LoginState& s = *st_;
        s.phase = LoginState::Requesting;
        s.error.clear();
        s.transientErrors = 0;
        const uint64_t gen = ++generation_;
        std::weak_ptr<LoginState> w = st_;
        App* a = &app;
        app.runAsync([a, w, gen, this] {
            DeviceCode dc;
            std::string err;
            const bool ok = a->auth().requestDeviceCode(dc, err);
            a->postToMain([a, w, gen, ok, dc, err, this] {
                auto s = w.lock();
                if (!s || gen != generation_) return;  // screen gone or superseded
                if (ok) {
                    const uint64_t now = platform::monotonicMs();
                    s->dc = dc;
                    s->phase = LoginState::Code;
                    s->expiresAt = now + static_cast<uint64_t>(std::max(30, dc.expiresIn)) * 1000;
                    s->nextPoll = now + static_cast<uint64_t>(std::max(1, dc.interval)) * 1000;
                    s->polling = false;
                    XC_LOGI("login: user code %s at %s", dc.userCode.c_str(), dc.verificationUri.c_str());
                } else {
                    s->phase = LoginState::Error;
                    s->error = "Could not get a sign-in code: " + (err.empty() ? std::string("unknown error") : err);
                    if (!platform::networkAvailable()) s->error += "\nThe console does not seem to be connected to a network.";
                    (void)a;
                }
            });
        });
    }

    void poll(App& app) {
        LoginState& s = *st_;
        s.polling = true;
        const uint64_t gen = generation_;
        const DeviceCode dc = s.dc;
        std::weak_ptr<LoginState> w = st_;
        App* a = &app;
        app.runAsync([a, w, gen, dc, this] {
            std::string err;
            const PollResult r = a->auth().pollToken(dc, err);
            a->postToMain([a, w, gen, r, err, this] {
                auto s = w.lock();
                if (!s || gen != generation_) return;
                s->polling = false;
                const uint64_t now = platform::monotonicMs();
                switch (r) {
                    case PollResult::Pending:
                        s->transientErrors = 0;
                        if (err == "slow_down") s->dc.interval += 5;  // RFC 8628 §3.5
                        s->nextPoll = now + static_cast<uint64_t>(std::max(1, s->dc.interval)) * 1000;
                        break;
                    case PollResult::Success: authorize(*a); break;
                    case PollResult::Expired: requestCode(*a); break;
                    case PollResult::Denied:
                        s->phase = LoginState::Error;
                        s->error = "The sign-in request was declined.";
                        break;
                    case PollResult::Error:
                        if (++s->transientErrors <= kMaxTransientPollErrors) {
                            XC_LOGW("login: poll error (%d): %s", s->transientErrors, err.c_str());
                            s->nextPoll = now + static_cast<uint64_t>(std::max(2, s->dc.interval * 2)) * 1000;
                        } else {
                            s->phase = LoginState::Error;
                            s->error = err.empty() ? std::string("Polling the sign-in status failed.") : err;
                        }
                        break;
                }
            });
        });
    }

    void authorize(App& app) {
        st_->phase = LoginState::Authorizing;
        const uint64_t gen = generation_;
        std::weak_ptr<LoginState> w = st_;
        App* a = &app;
        app.runAsync([a, w, gen, this] {
            std::string err;
            const bool ok = a->auth().authorizeStreaming(err);
            const std::string tag = ok ? a->config().gamertag() : std::string();
            a->postToMain([a, w, gen, ok, err, tag, this] {
                auto s = w.lock();
                if (!s || gen != generation_) return;
                if (ok) {
                    const std::string msg = tag.empty() ? std::string("Signed in") : "Signed in as " + tag;
                    a->toast(msg);
                    platform::notify("Nubix: " + msg);
                    a->replace(makeHomeScreen());
                } else {
                    s->phase = LoginState::Error;
                    s->error = "Your Microsoft account is signed in, but Xbox streaming access could not be "
                               "obtained: " +
                               (err.empty() ? std::string("unknown error") : err);
                }
            });
        });
    }

    std::shared_ptr<LoginState> st_ = std::make_shared<LoginState>();
    // Bumped on every new code request; stale async results are ignored. Only touched on the
    // main thread (inside postToMain callbacks after the weak_ptr check, the screen is alive).
    uint64_t generation_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeLoginScreen() { return std::make_unique<LoginScreen>(); }

}  // namespace xc
