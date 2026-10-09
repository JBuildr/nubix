// Nubix — Connecting screen (session start / queue / provisioning) and Stream screen
// (full-screen video, input forwarding, rumble, hold-PS overlay menu, statistics).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "core/log.hpp"
#include "platform/platform.hpp"
#include "ui/app.hpp"
#include "ui/screens.hpp"
#include "ui/screens_internal.hpp"
#include "ui/ui.hpp"

namespace xc {
namespace {

constexpr uint32_t kMenuHoldMs = 1000;
constexpr uint64_t kNexusPulseMs = 150;
constexpr uint64_t kEndedReturnMs = 6000;
constexpr uint64_t kStartHintMs = 7000;
constexpr uint64_t kStatsRefreshMs = 500;
constexpr uint16_t kTriggerReleased = 4000;  // a suppressed trigger counts as released below this

// The account was signed out while a session was starting/running (the refresh token was
// rejected on the streamer thread): go to the Login screen instead of a stale Home.
bool leaveIfSignedOut(App& app) {
    if (app.auth().isLoggedIn()) return false;
    app.toast("Your sign-in expired. Please sign in again.");
    app.resetTo(makeLoginScreen());
    return true;
}

bool isTerminal(StreamState s) { return s == StreamState::Ended || s == StreamState::Failed; }

// ============================================================================================
// Connecting
// ============================================================================================

class ConnectingScreen : public Screen {
public:
    ConnectingScreen(SessionKind kind, std::string id, std::string name, bool f2pOnly)
        : kind_(kind), id_(std::move(id)), name_(std::move(name)), f2pOnly_(f2pOnly) {}

    void onEnter(App& app) override {
        if (!issued_) begin(app);
    }

    void onNav(App& app, NavKey key) override {
        if (key == NavKey::Back) {
            cancel(app);
            return;
        }
        if (key == NavKey::Accept && failed_) begin(app);
    }

    void update(App& app, uint32_t dtMs) override {
        (void)dtMs;
        if (!started_ || failed_) return;
        Streamer& s = app.streamer();
        const StreamState st = s.state();
        lastState_ = st;
        if (st == StreamState::Streaming) {
            XC_LOGI("connecting: stream is up after %llu ms",
                    static_cast<unsigned long long>(platform::monotonicMs() - startedAt_));
            app.replace(ui::makeStreamScreenNamed(name_));
            return;
        }
        if (isTerminal(st)) {
            failed_ = true;
            error_ = s.statusText();
            if (error_.empty()) error_ = st == StreamState::Ended ? "The session ended." : "The session could not be started.";
            XC_LOGW("connecting: %s: %s", streamStateName(st), error_.c_str());
            app.streamer().stopAsync();  // release resources in the background
            leaveIfSignedOut(app);
        }
    }

    void render(App& app, Ui& ui) override {
        ui::background(ui);
        ui::logo(ui, 96, 56);
        const uint64_t now = platform::monotonicMs();
        const int cx = Ui::kWidth / 2;

        if (failed_) {
            ui::errorBadge(ui, cx, 360, 52);
            ui.text("Couldn't start " + name_, cx, 450, 44, colors::Text, Align::Center, true);
            const int w = 1100;
            ui.textWrapped(error_, (Ui::kWidth - w) / 2, 528, w, 28, colors::TextDim);
            ui::hintBar(app, ui, {{PadIcon::Cross, "Try again"}, {PadIcon::Circle, "Back"}});
            return;
        }

        const StreamState st = started_ ? lastState_ : StreamState::Starting;
        ui.spinner(cx, 330, 58, now);
        ui.textEllipsized((kind_ == SessionKind::Home ? "Connecting to " : "Starting ") + name_, cx, 430, 1500, 50,
                          colors::Text, Align::Center, true);
        std::string status = started_ ? app.streamer().statusText() : std::string("Preparing\xE2\x80\xA6");
        if (status.empty()) status = streamStateName(st);
        ui.textEllipsized(status, cx, 506, 1500, 28, colors::TextDim, Align::Center);

        if (st == StreamState::Queued) {
            const int pos = app.streamer().queuePosition();
            if (pos > 0) {
                ui.rect(cx - 260, 570, 520, 120, colors::Panel, true, 20);
                ui.text("Position in queue", cx, 586, 24, colors::TextDim, Align::Center);
                ui.text(std::to_string(pos), cx, 620, 50, colors::AccentBright, Align::Center, true);
            }
        }

        // progress stepper
        struct Step {
            const char* label;
            StreamState state;
        };
        const Step steps[] = {{"Session", StreamState::Starting},
                              {kind_ == SessionKind::Home ? "Console" : "Queue", StreamState::Queued},
                              {kind_ == SessionKind::Home ? "Waking up" : "Provisioning", StreamState::Provisioning},
                              {"Connecting", StreamState::Connecting}};
        const int n = 4, sw = 300, sx = cx - (n - 1) * sw / 2, sy = 790;
        int cur = 0;
        for (int i = 0; i < n; ++i)
            if (static_cast<int>(st) >= static_cast<int>(steps[i].state)) cur = i;
        for (int i = 0; i < n - 1; ++i)
            ui.rect(sx + i * sw + 16, sy - 2, sw - 32, 4, i < cur ? colors::AccentBright : colors::PanelHi, true, 2);
        for (int i = 0; i < n; ++i) {
            const int x = sx + i * sw;
            if (i < cur) {
                ui.circle(x, sy, 12, colors::AccentBright);
            } else if (i == cur) {
                const double pulse = 0.5 + 0.5 * std::sin(now / 180.0);
                ui.circle(x, sy, 12 + static_cast<int>(6 * pulse), withAlpha(colors::AccentBright, 70));
                ui.circle(x, sy, 12, colors::AccentBright);
            } else {
                ui.circle(x, sy, 10, colors::PanelHi);
            }
            ui.text(steps[i].label, x, sy + 28, 22, i <= cur ? colors::Text : colors::TextDim, Align::Center, i == cur);
        }

        const int64_t elapsed = started_ ? static_cast<int64_t>((now - startedAt_) / 1000) : 0;
        ui.text("Elapsed " + ui::formatDuration(elapsed), cx, 900, 24, withAlpha(colors::TextDim, 200), Align::Center);
        ui::hintBar(app, ui, {{PadIcon::Circle, "Cancel"}});
    }

private:
    void begin(App& app) {
        issued_ = true;
        started_ = false;
        failed_ = false;
        error_.clear();
        lastState_ = StreamState::Starting;
        startedAt_ = platform::monotonicMs();
        XC_LOGI("connecting: %s session for %s", kind_ == SessionKind::Home ? "home" : "cloud", id_.c_str());
        // Non-blocking: the Streamer releases a session that is still winding down on its own
        // lifecycle thread before this one starts.
        if (app.streamer().start(kind_, id_, f2pOnly_)) {
            started_ = true;
            startedAt_ = platform::monotonicMs();
        } else {
            failed_ = true;
            error_ = "A previous streaming session is still active. Please try again.";
        }
    }

    void cancel(App& app) {
        if (started_ || issued_) app.streamer().stopAsync();
        app.pop();
    }

    SessionKind kind_;
    std::string id_, name_;
    bool f2pOnly_ = false;
    bool issued_ = false, started_ = false, failed_ = false;
    std::string error_;
    StreamState lastState_ = StreamState::Starting;
    uint64_t startedAt_ = 0;
};

// ============================================================================================
// Stream
// ============================================================================================

class StreamScreen : public Screen {
public:
    explicit StreamScreen(std::string name) : name_(std::move(name)) {}
    ~StreamScreen() override {
        if (tex_) SDL_DestroyTexture(tex_);
    }

    bool capturesGamepad() const override { return !menuOpen_ && !ended_; }

    void onEnter(App& app) override {
        if (enteredAt_ == 0) enteredAt_ = platform::monotonicMs();
        deadzone_ = app.config().settings().stickDeadzone;
        captureHeld(app);  // e.g. Cross still held from starting the title in the library
    }

    void onLeave(App& app) override {
        app.gamepad().stopRumble();
        app.streamer().setGamepad(GamepadState{});
    }

    void onNav(App& app, NavKey key) override {
        if (ended_) {
            if (key == NavKey::Accept || key == NavKey::Back) leave(app);
            return;
        }
        if (!menuOpen_) {
            if (key == NavKey::Menu) openMenu(app);
            return;
        }
        switch (key) {
            case NavKey::Up: menuFocus_ = (menuFocus_ + kMenuItems - 1) % kMenuItems; break;
            case NavKey::Down: menuFocus_ = (menuFocus_ + 1) % kMenuItems; break;
            case NavKey::Back:
            case NavKey::Menu: closeMenu(app); break;
            case NavKey::Accept: activate(app, menuFocus_); break;
            default: break;
        }
    }

    void update(App& app, uint32_t dtMs) override {
        (void)dtMs;
        const uint64_t now = platform::monotonicMs();
        Streamer& s = app.streamer();
        Gamepad& pad = app.gamepad();

        // session end
        const StreamState st = s.state();
        if (!ended_ && isTerminal(st)) {
            ended_ = true;
            menuOpen_ = false;
            endedAt_ = now;
            endMsg_ = s.statusText();
            failedEnd_ = st == StreamState::Failed;
            pad.stopRumble();
            XC_LOGI("stream: %s (%s)", streamStateName(st), endMsg_.c_str());
            if (failedEnd_) platform::notify("Nubix: stream ended: " + (endMsg_.empty() ? std::string("error") : endMsg_));
            app.streamer().stopAsync();
        }
        if (ended_) {
            if (now - endedAt_ > kEndedReturnMs) leave(app);
            return;
        }

        // video
        if (AVFrame* f = s.currentFrame()) {
            upload(app.ui(), f);
            av_frame_free(&f);
            ++videoFrames_;
        }

        // input (state() every frame: it also advances the Menu/View chord hold-back)
        GamepadState gs = pad.state(deadzone_);
        const bool chord = pad.menuChordTriggered(kMenuHoldMs);
        if (!menuOpen_) {
            if (chord) {
                openMenu(app);
            } else {
                suppressHeld(gs);
                if (now < nexusUntil_) gs.buttons |= btn::Nexus;
                s.setGamepad(gs);
            }
        } else {
            if (chord) closeMenu(app);
            s.setGamepad(GamepadState{});
        }

        // rumble
        Vibration v;
        while (s.takeVibration(v))
            if (!menuOpen_) pad.rumble(v);

        // microphone: first Live of this stream -> tell the user it is on and how to mute it
        micState_ = s.micState();
        micLevel_ = micState_ == MicState::Live ? s.micLevel() : 0.0f;
        if (micState_ == MicState::Live && !micToastShown_) {
            micToastShown_ = true;
            app.toast("Microphone on - mute it in the stream menu (Options + touchpad)");
        }

        // stats
        ++displayFrames_;
        if (now - statsAt_ >= kStatsRefreshMs) {
            const double secs = (now - statsAt_) / 1000.0;
            if (statsAt_ != 0 && secs > 0) displayFps_ = displayFrames_ / secs;
            displayFrames_ = 0;
            statsAt_ = now;
            stats_ = s.stats();
        }
    }

    void render(App& app, Ui& ui) override {
        const uint64_t now = platform::monotonicMs();
        SDL_Renderer* r = ui.renderer();
        if (tex_ && texW_ > 0 && texH_ > 0) {
            const float sc = std::min(static_cast<float>(Ui::kWidth) / texW_, static_cast<float>(Ui::kHeight) / texH_);
            const int w = static_cast<int>(std::lround(texW_ * sc)), h = static_cast<int>(std::lround(texH_ * sc));
            const int x = (Ui::kWidth - w) / 2, y = (Ui::kHeight - h) / 2;
            // letterbox bars only (the video covers the rest)
            if (x > 0) {
                ui.rect(0, 0, x, Ui::kHeight, colors::Black);
                ui.rect(x + w, 0, Ui::kWidth - x - w, Ui::kHeight, colors::Black);
            }
            if (y > 0) {
                ui.rect(0, 0, Ui::kWidth, y, colors::Black);
                ui.rect(0, y + h, Ui::kWidth, Ui::kHeight - y - h, colors::Black);
            }
            SDL_Rect dst{x, y, w, h};
            SDL_RenderCopy(r, tex_, nullptr, &dst);
        } else {
            ui.rect(0, 0, Ui::kWidth, Ui::kHeight, colors::Black);
            if (!ended_) {
                ui.spinner(Ui::kWidth / 2, Ui::kHeight / 2 - 30, 48, now);
                ui.text("Waiting for video\xE2\x80\xA6", Ui::kWidth / 2, Ui::kHeight / 2 + 50, 30, colors::TextDim,
                        Align::Center);
            }
        }

        if (app.statsOverlay() && !ended_) renderStats(ui);
        if (!ended_) renderMicIndicator(ui);

        if (!ended_ && !menuOpen_ && now - enteredAt_ < kStartHintMs) {
            const bool kb = app.gamepad().lastInputKeyboard();
            const std::string msg = kb ? "Press F1 for the stream menu"
                                       : (platform::isPs5() ? "Hold Options + touchpad for 1 s for the stream menu"
                                                            : "Hold PS (or Options + Create) for 1 s for the stream menu");
            const uint64_t left = kStartHintMs - (now - enteredAt_);
            const uint8_t a = left < 500 ? static_cast<uint8_t>(left * 255 / 500) : 255;
            const int w = ui.textWidth(msg, 24) + 60;
            ui.rect((Ui::kWidth - w) / 2, 40, w, 56, withAlpha(colors::Panel, static_cast<uint8_t>(a * 220 / 255)), true, 28);
            ui.text(msg, Ui::kWidth / 2, 40 + (56 - ui.lineHeight(24)) / 2, 24, withAlpha(colors::Text, a), Align::Center);
        }

        if (menuOpen_) renderMenu(app, ui);
        if (ended_) renderEnded(app, ui, now);
    }

private:
    enum MenuItem { ItemResume, ItemNexus, ItemMic, ItemStats, ItemKeyframe, ItemDisconnect, kMenuItems };

    void openMenu(App& app) {
        menuOpen_ = true;
        menuFocus_ = ItemResume;
        app.gamepad().stopRumble();
        app.streamer().setGamepad(GamepadState{});
        captureHeld(app);
    }

    // Close the overlay. The button that closed it (Cross on an item, Circle, the chord) is
    // still physically down: it must not reach the game when forwarding resumes.
    void closeMenu(App& app) {
        menuOpen_ = false;
        captureHeld(app);
    }

    // Remember every button / trigger held right now; suppressHeld() keeps each one away from
    // the game until it has been released once.
    void captureHeld(App& app) {
        const GamepadState cur = app.gamepad().state(deadzone_);
        suppressMask_ = cur.buttons;
        suppressLt_ = cur.lt >= kTriggerReleased;
        suppressRt_ = cur.rt >= kTriggerReleased;
    }

    void suppressHeld(GamepadState& gs) {
        if (suppressMask_) {
            suppressMask_ &= gs.buttons;  // released bits are forwarded normally from now on
            gs.buttons = static_cast<uint16_t>(gs.buttons & ~suppressMask_);
        }
        if (suppressLt_) {
            if (gs.lt < kTriggerReleased) suppressLt_ = false;
            else gs.lt = 0;
        }
        if (suppressRt_) {
            if (gs.rt < kTriggerReleased) suppressRt_ = false;
            else gs.rt = 0;
        }
    }

    // Back to the previous screen, or to Login when the account got signed out meanwhile.
    void leave(App& app) {
        if (!leaveIfSignedOut(app)) app.pop();
    }

    void activate(App& app, int item) {
        switch (item) {
            case ItemResume: closeMenu(app); break;
            case ItemNexus:
                closeMenu(app);  // Cross is suppressed: the game gets Nexus alone
                nexusUntil_ = platform::monotonicMs() + kNexusPulseMs;
                break;
            case ItemMic: {
                // Unavailable / voice chat off: the item is shown greyed out and does nothing.
                const MicState ms = app.streamer().micState();
                if (ms != MicState::Live && ms != MicState::Muted) break;
                const bool mute = !app.streamer().micMuted();
                app.streamer().setMicMuted(mute);
                micState_ = mute ? MicState::Muted : MicState::Live;
                if (mute) micLevel_ = 0.0f;
                XC_LOGI("stream: microphone %s by user", mute ? "muted" : "unmuted");
                app.toast(mute ? "Microphone muted" : "Microphone on");
                break;  // the menu stays open
            }
            case ItemStats: app.setStatsOverlay(!app.statsOverlay()); break;
            case ItemKeyframe:
                app.streamer().requestKeyframe();
                app.toast("Requested a fresh video frame");
                closeMenu(app);
                break;
            case ItemDisconnect: {
                XC_LOGI("stream: disconnect requested by user");
                app.streamer().stopAsync();
                leave(app);
                break;
            }
            default: break;
        }
    }

    void upload(Ui& ui, const AVFrame* f) {
        const int fmt = f->format;
        Uint32 sdlFmt = 0;
        if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P) sdlFmt = SDL_PIXELFORMAT_IYUV;
        else if (fmt == AV_PIX_FMT_NV12) sdlFmt = SDL_PIXELFORMAT_NV12;
        if (sdlFmt == 0 || f->width <= 0 || f->height <= 0) {
            if (!warnedFormat_) {
                XC_LOGE("stream: unsupported frame format %d (%dx%d)", fmt, f->width, f->height);
                warnedFormat_ = true;
            }
            return;
        }
        if (!tex_ || f->width != texW_ || f->height != texH_ || sdlFmt != texFmt_) {
            if (tex_) SDL_DestroyTexture(tex_);
            const bool full = fmt == AV_PIX_FMT_YUVJ420P || f->color_range == AVCOL_RANGE_JPEG;
            SDL_SetYUVConversionMode(full ? SDL_YUV_CONVERSION_JPEG : SDL_YUV_CONVERSION_AUTOMATIC);
            tex_ = SDL_CreateTexture(ui.renderer(), sdlFmt, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
            if (!tex_) {
                XC_LOGE("stream: SDL_CreateTexture %dx%d failed: %s", f->width, f->height, SDL_GetError());
                texW_ = texH_ = 0;
                return;
            }
            texW_ = f->width;
            texH_ = f->height;
            texFmt_ = sdlFmt;
            XC_LOGI("stream: video %dx%d %s", texW_, texH_, sdlFmt == SDL_PIXELFORMAT_NV12 ? "NV12" : "IYUV");
        }
        int rc;
        if (sdlFmt == SDL_PIXELFORMAT_IYUV)
            rc = SDL_UpdateYUVTexture(tex_, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
                                      f->linesize[2]);
        else
            rc = SDL_UpdateNVTexture(tex_, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
        if (rc != 0 && !warnedUpload_) {
            XC_LOGE("stream: texture upload failed: %s", SDL_GetError());
            warnedUpload_ = true;
        }
    }

    void renderStats(Ui& ui) {
        char l1[96], l2[96], l3[128], l4[64], l5[96];
        const StreamStats& s = stats_;
        std::snprintf(l1, sizeof(l1), "%d \xC3\x97 %d  \xC2\xB7  %.1f fps", s.width ? s.width : texW_,
                      s.height ? s.height : texH_, s.fps);
        std::snprintf(l2, sizeof(l2), "%.1f Mbps  \xC2\xB7  loss %.1f %%", s.bitrateKbps / 1000.0, s.lossPercent);
        if (s.rttMs >= 0)
            std::snprintf(l3, sizeof(l3), "decode %.1f ms  \xC2\xB7  RTT %d ms  \xC2\xB7  audio %d ms", s.decodeMs, s.rttMs,
                          s.audioBufferMs);
        else
            std::snprintf(l3, sizeof(l3), "decode %.1f ms  \xC2\xB7  audio %d ms", s.decodeMs, s.audioBufferMs);
        std::snprintf(l4, sizeof(l4), "display %.0f fps", displayFps_);
        std::snprintf(l5, sizeof(l5), "mic %s %d kbps | chat rx %llu", micStateLabel(micState_), s.micTxKbps,
                      static_cast<unsigned long long>(s.chatRxPackets));
        const char* lines[] = {l1, l2, l3, l4, l5};
        constexpr int n = static_cast<int>(sizeof(lines) / sizeof(lines[0]));
        int w = 0;
        for (auto* l : lines) w = std::max(w, ui.textWidth(l, 22));
        const int lh = ui.lineHeight(22);
        ui.rect(28, 28, w + 48, lh * n + 32, withAlpha(colors::Black, 160), true, 14);
        ui.rect(28, 44, 4, lh * n, colors::AccentBright, true, 2);
        int y = 44;
        for (int i = 0; i < n; ++i) {
            ui.text(lines[i], 52, y, 22, i == 0 ? colors::White : colors::Text, Align::Left, i == 0);
            y += lh;
        }
    }

    static const char* micStateLabel(MicState m) {
        switch (m) {
            case MicState::Off: return "off";
            case MicState::Unavailable: return "unavailable";
            case MicState::Muted: return "muted";
            case MicState::Live: return "live";
        }
        return "?";
    }

    // Top-right microphone badge while voice chat runs: white capsule + green level bar when
    // live, grey capsule with a red slash when muted. Hidden in every other state.
    void renderMicIndicator(Ui& ui) {
        if (micState_ != MicState::Live && micState_ != MicState::Muted) return;
        const bool live = micState_ == MicState::Live;
        constexpr int kBox = 40, kMargin = 16;
        const int bx = Ui::kWidth - kMargin - kBox, by = kMargin;
        ui.rect(bx, by, kBox, kBox, withAlpha(colors::Black, 150), true, 10);
        const Color cap = live ? colors::White : colors::TextDim;
        // capsule 12x20, centred horizontally, a little above centre to leave room for the stand
        const int cw = 12, ch = 20, cx = bx + (kBox - cw) / 2, cy = by + 6;
        ui.rect(cx, cy, cw, ch, cap, true, 6);
        if (live) {
            // level fill from the bottom of the capsule, height = level x 20
            const float lv = std::max(0.0f, std::min(1.0f, micLevel_));
            const int lh = static_cast<int>(std::lround(lv * ch));
            if (lh > 0) ui.rect(cx + 2, cy + ch - lh, cw - 4, lh, Color{60, 200, 90, 255}, true, 4);
        }
        // stand: short stem + base bar
        ui.rect(bx + kBox / 2 - 1, cy + ch + 1, 2, 5, cap);
        ui.rect(bx + kBox / 2 - 6, cy + ch + 6, 12, 2, cap, true, 1);
        if (!live) ui.line(bx + 9.0f, by + kBox - 9.0f, bx + kBox - 9.0f, by + 9.0f, 3.0f, colors::Error);
    }

    void renderMenu(App& app, Ui& ui) {
        ui.rect(0, 0, Ui::kWidth, Ui::kHeight, withAlpha(colors::Black, 150));
        const int w = 640, itemH = 76, gap = 12;
        const int h = 150 + kMenuItems * (itemH + gap) + 20;
        const int x = (Ui::kWidth - w) / 2, y = (Ui::kHeight - h) / 2;
        ui.rect(x, y, w, h, colors::Panel, true, 26);
        ui.outline(x, y, w, h, withAlpha(colors::AccentBright, 80), 2, 26);
        ui.textEllipsized(name_.empty() ? std::string("Stream") : name_, x + 48, y + 40, w - 96, 36, colors::Text,
                          Align::Left, true);
        const StreamStats& s = stats_;
        char sub[96];
        std::snprintf(sub, sizeof(sub), "%dx%d \xC2\xB7 %.0f fps \xC2\xB7 %.1f Mbps", s.width ? s.width : texW_,
                      s.height ? s.height : texH_, s.fps, s.bitrateKbps / 1000.0);
        if (s.width > 0 || texW_ > 0) ui.text(sub, x + 48, y + 92, 22, colors::TextDim);
        int iy = y + 150;
        for (int i = 0; i < kMenuItems; ++i) {
            std::string label;
            bool dim = false;  // shown but inactive
            switch (i) {
                case ItemResume: label = "Resume"; break;
                case ItemNexus: label = "Press Xbox button"; break;
                case ItemMic:
                    switch (micState_) {
                        case MicState::Live: label = "Mute microphone"; break;
                        case MicState::Muted: label = "Unmute microphone"; break;
                        case MicState::Unavailable:
                            label = "Microphone unavailable";
                            dim = true;
                            break;
                        case MicState::Off:
                            label = "Voice chat off";
                            dim = true;
                            break;
                    }
                    break;
                case ItemStats: label = app.statsOverlay() ? "Hide statistics" : "Show statistics"; break;
                case ItemKeyframe: label = "Refresh video"; break;
                case ItemDisconnect: label = "Disconnect"; break;
            }
            const bool f = i == menuFocus_;
            if (f) {
                const Color bg = dim ? colors::PanelHi : (i == ItemDisconnect ? Color{150, 45, 40, 255} : colors::Accent);
                ui.rect(x + 32, iy, w - 64, itemH, bg, true, 14);
            }
            const Color fg = dim ? colors::TextDim
                                 : (f ? colors::White : (i == ItemDisconnect ? Color{255, 140, 130, 255} : colors::Text));
            ui.text(label, x + 64, iy + (itemH - ui.lineHeight(28, f)) / 2, 28, fg, Align::Left, f);
            iy += itemH + gap;
        }
        ui::hintBar(app, ui, {{PadIcon::Cross, "Select"}, {PadIcon::Circle, "Resume"}});
    }

    void renderEnded(App& app, Ui& ui, uint64_t now) {
        ui.rect(0, 0, Ui::kWidth, Ui::kHeight, withAlpha(colors::Black, 170));
        const int w = 1000, h = 330;
        const int x = (Ui::kWidth - w) / 2, y = (Ui::kHeight - h) / 2;
        ui.rect(x, y, w, h, colors::Panel, true, 24);
        if (failedEnd_) ui::errorBadge(ui, x + 80, y + 82, 30);
        ui.text(failedEnd_ ? "Stream interrupted" : "Stream ended", x + (failedEnd_ ? 132 : 56), y + 58, 40,
                colors::Text, Align::Left, true);
        if (!endMsg_.empty()) ui.textWrapped(endMsg_, x + 56, y + 130, w - 112, 26, colors::TextDim);
        const int64_t left = static_cast<int64_t>((kEndedReturnMs - std::min(kEndedReturnMs, now - endedAt_)) / 1000) + 1;
        ui.text("Returning in " + std::to_string(left) + " s", x + 56, y + h - 70, 24, withAlpha(colors::TextDim, 200));
        ui::hintBar(app, ui, {{PadIcon::Cross, "Back now"}});
    }

    std::string name_;
    SDL_Texture* tex_ = nullptr;
    int texW_ = 0, texH_ = 0;
    Uint32 texFmt_ = 0;
    bool warnedFormat_ = false, warnedUpload_ = false;

    bool menuOpen_ = false;
    int menuFocus_ = 0;
    uint64_t nexusUntil_ = 0;
    int deadzone_ = 0;
    uint16_t suppressMask_ = 0;
    bool suppressLt_ = false, suppressRt_ = false;

    bool ended_ = false, failedEnd_ = false;
    uint64_t endedAt_ = 0;
    std::string endMsg_;

    uint64_t enteredAt_ = 0;
    MicState micState_ = MicState::Off;
    float micLevel_ = 0.0f;
    bool micToastShown_ = false;  // "Microphone on" toast: once per stream screen
    StreamStats stats_;
    uint64_t statsAt_ = 0;
    uint64_t videoFrames_ = 0;
    int displayFrames_ = 0;
    double displayFps_ = 0;
};

}  // namespace

namespace ui {
std::unique_ptr<Screen> makeStreamScreenNamed(const std::string& displayName) {
    return std::make_unique<StreamScreen>(displayName);
}
}  // namespace ui

std::unique_ptr<Screen> makeConnectingScreen(SessionKind kind, const std::string& titleOrServerId,
                                             const std::string& displayName, bool f2pOnly) {
    return std::make_unique<ConnectingScreen>(kind, titleOrServerId, displayName, f2pOnly);
}

}  // namespace xc
