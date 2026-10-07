// Nubix — SDL GameController input mapping (DualSense / any SDL pad -> Xbox layout),
// UI navigation edges with auto-repeat, stream-menu chord, rumble, host keyboard fallback.
//
// PS5 notes (SDL-ps5 joystick driver, src/joystick/ps5/SDL_ps5joystick.c):
//  - the PS and Create buttons are not reported (the system owns them), the touchpad click is
//    SDL_CONTROLLER_BUTTON_TOUCHPAD -> the menu chord on PS5 is Options + touchpad;
//  - L2/R2 are reported as full-range axes that only emit an event once they change, so an
//    untouched trigger would read as half-pressed through the GameController mapping until
//    its first motion event. We therefore report 0 until a motion event was seen.
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "platform/gamepad.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/log.hpp"
#include "platform/platform.hpp"

namespace xc {
namespace {

constexpr uint32_t kRepeatDelayMs = 380;
constexpr uint32_t kRepeatIntervalMs = 110;
constexpr int kStickNavThreshold = 20000;   // ~61 % deflection counts as a direction press
constexpr int kStickNavRelease = 12000;     // hysteresis
constexpr uint32_t kNexusPulseMs = 120;     // short PS press -> Nexus held this long
constexpr int kTriggerNavThreshold = 16000;
constexpr uint32_t kMaxRumbleMs = 4000;
// Options (Menu) / Create+touchpad (View) are held back this long after being pressed: if the
// chord partner follows within it, neither reaches the game (nobody presses both in one frame).
constexpr uint32_t kChordGraceMs = 140;
constexpr uint32_t kTapPulseMs = 80;        // a tap shorter than the grace is replayed this long      // cap for one (approximated) rumble envelope

struct Pad {
    SDL_GameController* gc = nullptr;
    SDL_JoystickID id = -1;
    bool triggerSeen[2] = {false, false};
    bool hasTriggerRumble = false;
    std::string name;
};

int16_t clampAxis(int v) { return static_cast<int16_t>(std::max(-32767, std::min(32767, v))); }

// Radial deadzone in percent with rescaling so the output still reaches full deflection.
void applyDeadzone(int16_t& x, int16_t& y, int percent) {
    if (percent <= 0) return;
    const double dz = std::min(95, percent) / 100.0;
    const double fx = x / 32767.0, fy = y / 32767.0;
    const double mag = std::sqrt(fx * fx + fy * fy);
    if (mag < dz || mag <= 0.0) {
        x = 0;
        y = 0;
        return;
    }
    const double scaled = std::min(1.0, (mag - dz) / (1.0 - dz));
    const double k = scaled / mag;
    x = clampAxis(static_cast<int>(std::lround(fx * k * 32767.0)));
    y = clampAxis(static_cast<int>(std::lround(fy * k * 32767.0)));
}

uint16_t triggerToXbox(int16_t v) {
    if (v <= 0) return 0;
    return static_cast<uint16_t>(std::min<int>(65535, (static_cast<int>(v) * 65535) / 32767));
}

}  // namespace

struct Gamepad::Impl {
    std::vector<Pad> pads;
    SDL_JoystickID active = -1;
    bool inited = false;
    bool lastKeyboard = false;

    // nav auto-repeat (D-pad / stick)
    NavKey heldDir = NavKey::None;
    uint64_t heldSince = 0, lastRepeat = 0;
    int stickDirX = 0, stickDirY = 0;  // -1/0/1 per axis (left stick as D-pad)
    bool ltNav = false, rtNav = false;

    // menu chord / PS short press
    uint64_t guideDownAt = 0;    // 0 = not held
    bool guideConsumed = false;  // chord fired while held: do not send Nexus on release
    uint64_t comboSince = 0;     // Options + Create/touchpad held since
    bool comboConsumed = false;
    uint64_t nexusPulseUntil = 0;
    bool chordFired = false;     // edge for menuChordTriggered()

    // Menu/View hold-back (state() is const but polled once per frame: mutable bookkeeping).
    struct HoldGate {
        uint64_t downAt = 0;     // 0 = released
        bool forwarded = false;  // grace elapsed without the chord partner: sent to the game
        bool consumed = false;   // became part of the chord: never sent until released
    };
    mutable HoldGate startGate, viewGate;
    mutable bool chordLatch = false;  // both were held together; cleared when both are up
    mutable uint64_t menuPulseUntil = 0, viewPulseUntil = 0;

    void resetGates() {
        startGate = HoldGate();
        viewGate = HoldGate();
        chordLatch = false;
        menuPulseUntil = viewPulseUntil = 0;
    }

    // Returns whether the gated button is forwarded to the game this frame.
    bool gate(HoldGate& g, bool down, uint64_t& pulseUntil, uint64_t now) const {
        if (!down) {
            // Short tap that never got past the grace period and was not part of the chord:
            // replay it as a brief press so a quick Options tap still pauses the game.
            if (g.downAt != 0 && !g.forwarded && !g.consumed) pulseUntil = now + kTapPulseMs;
            g = HoldGate();
            return now < pulseUntil;
        }
        if (g.downAt == 0) g.downAt = now;
        if (chordLatch) g.consumed = true;
        if (g.consumed) return false;
        if (!g.forwarded && now - g.downAt >= kChordGraceMs) g.forwarded = true;
        return g.forwarded;
    }

    Pad* find(SDL_JoystickID id) {
        for (auto& p : pads)
            if (p.id == id) return &p;
        return nullptr;
    }
    const Pad* activePad() const {
        for (auto& p : pads)
            if (p.id == active) return &p;
        return pads.empty() ? nullptr : &pads.front();
    }

    void open(int deviceIndex) {
        if (!SDL_IsGameController(deviceIndex)) {
            XC_LOGI("gamepad: joystick %d (%s) has no controller mapping, ignored", deviceIndex,
                    SDL_JoystickNameForIndex(deviceIndex) ? SDL_JoystickNameForIndex(deviceIndex) : "?");
            return;
        }
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
        if (find(id)) return;
        SDL_GameController* gc = SDL_GameControllerOpen(deviceIndex);
        if (!gc) {
            XC_LOGW("gamepad: open %d failed: %s", deviceIndex, SDL_GetError());
            return;
        }
        Pad p;
        p.gc = gc;
        p.id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        const char* n = SDL_GameControllerName(gc);
        p.name = n ? n : "Controller";
#if SDL_VERSION_ATLEAST(2, 0, 18)
        p.hasTriggerRumble = SDL_GameControllerHasRumbleTriggers(gc) == SDL_TRUE;
#endif
        // Non-PS5 drivers report triggers correctly from the start.
        if (!platform::isPs5()) p.triggerSeen[0] = p.triggerSeen[1] = true;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (SDL_GameControllerHasLED(gc)) SDL_GameControllerSetLED(gc, 36, 110, 240);  // Nubix blue
#endif
        XC_LOGI("gamepad: connected \"%s\" (instance %d, trigger rumble %s)", p.name.c_str(), (int)p.id,
                p.hasTriggerRumble ? "yes" : "no");
        pads.push_back(p);
        if (active < 0) active = p.id;
    }

    void close(SDL_JoystickID id) {
        for (auto it = pads.begin(); it != pads.end(); ++it) {
            if (it->id != id) continue;
            XC_LOGI("gamepad: disconnected \"%s\"", it->name.c_str());
            SDL_GameControllerClose(it->gc);
            pads.erase(it);
            break;
        }
        if (active == id) {
            active = pads.empty() ? -1 : pads.front().id;
            guideDownAt = 0;
            comboSince = 0;
            resetGates();
            heldDir = NavKey::None;
            stickDirX = stickDirY = 0;
        }
    }

    bool button(SDL_GameControllerButton b) const {
        const Pad* p = activePad();
        return p && SDL_GameControllerGetButton(p->gc, b) == 1;
    }

    // Options + (Create or touchpad) currently held on the active pad.
    bool comboHeld() const {
        return button(SDL_CONTROLLER_BUTTON_START) &&
               (button(SDL_CONTROLLER_BUTTON_BACK) || button(SDL_CONTROLLER_BUTTON_TOUCHPAD));
    }

    void updateCombo(uint64_t now) {
        if (comboHeld()) {
            if (comboSince == 0) comboSince = now;
        } else {
            comboSince = 0;
            comboConsumed = false;
        }
    }

    NavKey startHeld(NavKey k, uint64_t now) {
        heldDir = k;
        heldSince = now;
        lastRepeat = now;
        return k;
    }
    void releaseHeld(NavKey k) {
        if (heldDir == k) heldDir = NavKey::None;
    }
};

Gamepad::Gamepad() : d_(new Impl) {}
Gamepad::~Gamepad() { shutdown(); }

bool Gamepad::init() {
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            XC_LOGE("gamepad: SDL_INIT_GAMECONTROLLER failed: %s", SDL_GetError());
            return false;
        }
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    d_->inited = true;
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; ++i) d_->open(i);
    XC_LOGI("gamepad: %d joystick(s), %zu controller(s) open", n, d_->pads.size());
    return true;
}

void Gamepad::shutdown() {
    if (!d_) return;
    for (auto& p : d_->pads)
        if (p.gc) SDL_GameControllerClose(p.gc);
    d_->pads.clear();
    d_->active = -1;
    d_->inited = false;
}

NavKey Gamepad::handleEvent(const SDL_Event& ev) {
    const uint64_t now = platform::monotonicMs();
    Impl& d = *d_;
    switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED:
            d.open(ev.cdevice.which);
            return NavKey::None;
        case SDL_CONTROLLERDEVICEREMOVED:
            d.close(ev.cdevice.which);
            return NavKey::None;

        case SDL_CONTROLLERBUTTONDOWN: {
            if (!d.find(ev.cbutton.which)) return NavKey::None;
            d.active = ev.cbutton.which;
            d.lastKeyboard = false;
            d.updateCombo(now);
            switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A: return NavKey::Accept;
                case SDL_CONTROLLER_BUTTON_B: return NavKey::Back;
                case SDL_CONTROLLER_BUTTON_Y: return NavKey::Option;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return NavKey::TabLeft;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return NavKey::TabRight;
                // START is deliberately not a NavKey: while streaming it is the game's Menu
                // button; the stream overlay opens via menuChordTriggered() / keyboard F1.
                case SDL_CONTROLLER_BUTTON_GUIDE:
                    d.guideDownAt = now;
                    d.guideConsumed = false;
                    return NavKey::None;
                case SDL_CONTROLLER_BUTTON_DPAD_UP: return d.startHeld(NavKey::Up, now);
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return d.startHeld(NavKey::Down, now);
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return d.startHeld(NavKey::Left, now);
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return d.startHeld(NavKey::Right, now);
                default: return NavKey::None;
            }
        }
        case SDL_CONTROLLERBUTTONUP: {
            if (!d.find(ev.cbutton.which)) return NavKey::None;
            d.updateCombo(now);
            switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_GUIDE:
                    if (d.guideDownAt != 0 && !d.guideConsumed) d.nexusPulseUntil = now + kNexusPulseMs;
                    d.guideDownAt = 0;
                    d.guideConsumed = false;
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP: d.releaseHeld(NavKey::Up); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: d.releaseHeld(NavKey::Down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: d.releaseHeld(NavKey::Left); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: d.releaseHeld(NavKey::Right); break;
                default: break;
            }
            return NavKey::None;
        }
        case SDL_CONTROLLERAXISMOTION: {
            Pad* p = d.find(ev.caxis.which);
            if (!p) return NavKey::None;
            const int v = ev.caxis.value;
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) p->triggerSeen[0] = true;
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) p->triggerSeen[1] = true;
            if (ev.caxis.which != d.active && std::abs(v) > kStickNavThreshold &&
                ev.caxis.axis != SDL_CONTROLLER_AXIS_TRIGGERLEFT && ev.caxis.axis != SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                d.active = ev.caxis.which;
            if (ev.caxis.which != d.active) return NavKey::None;
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                int dir = v > kStickNavThreshold ? 1 : v < -kStickNavThreshold ? -1 : (std::abs(v) < kStickNavRelease ? 0 : d.stickDirX);
                if (dir != d.stickDirX) {
                    NavKey old = d.stickDirX > 0 ? NavKey::Right : NavKey::Left;
                    if (d.stickDirX != 0) d.releaseHeld(old);
                    d.stickDirX = dir;
                    if (dir != 0) {
                        d.lastKeyboard = false;
                        return d.startHeld(dir > 0 ? NavKey::Right : NavKey::Left, now);
                    }
                }
            } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                int dir = v > kStickNavThreshold ? 1 : v < -kStickNavThreshold ? -1 : (std::abs(v) < kStickNavRelease ? 0 : d.stickDirY);
                if (dir != d.stickDirY) {
                    NavKey old = d.stickDirY > 0 ? NavKey::Down : NavKey::Up;
                    if (d.stickDirY != 0) d.releaseHeld(old);
                    d.stickDirY = dir;
                    if (dir != 0) {
                        d.lastKeyboard = false;
                        return d.startHeld(dir > 0 ? NavKey::Down : NavKey::Up, now);
                    }
                }
            } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
                bool on = v > kTriggerNavThreshold;
                if (on != d.ltNav) {
                    d.ltNav = on;
                    if (on) return NavKey::TabLeft;
                }
            } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                bool on = v > kTriggerNavThreshold;
                if (on != d.rtNav) {
                    d.rtNav = on;
                    if (on) return NavKey::TabRight;
                }
            }
            return NavKey::None;
        }

        case SDL_KEYDOWN: {
            d.lastKeyboard = true;
            switch (ev.key.keysym.sym) {
                case SDLK_UP: return NavKey::Up;  // OS key repeat handles holding
                case SDLK_DOWN: return NavKey::Down;
                case SDLK_LEFT: return NavKey::Left;
                case SDLK_RIGHT: return NavKey::Right;
                default: break;
            }
            if (ev.key.repeat) return NavKey::None;
            switch (ev.key.keysym.sym) {
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE: return NavKey::Accept;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE: return NavKey::Back;
                case SDLK_q:
                case SDLK_PAGEUP: return NavKey::TabLeft;
                case SDLK_e:
                case SDLK_PAGEDOWN: return NavKey::TabRight;
                case SDLK_TAB:
                case SDLK_y: return NavKey::Option;
                case SDLK_F1: return NavKey::Menu;
                default: return NavKey::None;
            }
        }
        default:
            return NavKey::None;
    }
}

NavKey Gamepad::pollRepeat() {
    Impl& d = *d_;
    if (d.heldDir == NavKey::None) return NavKey::None;
    const uint64_t now = platform::monotonicMs();
    if (now - d.heldSince < kRepeatDelayMs) return NavKey::None;
    if (now - d.lastRepeat < kRepeatIntervalMs) return NavKey::None;
    d.lastRepeat = now;
    return d.heldDir;
}

bool Gamepad::connected() const { return !d_->pads.empty(); }

std::string Gamepad::name() const {
    const Pad* p = d_->activePad();
    return p ? p->name : std::string();
}

bool Gamepad::lastInputKeyboard() const { return d_->lastKeyboard || d_->pads.empty(); }

GamepadState Gamepad::state(int deadzonePercent) const {
    const Impl& d = *d_;
    GamepadState st;
    const uint64_t now = platform::monotonicMs();
    if (const Pad* p = d.activePad()) {
        SDL_GameController* gc = p->gc;
        auto b = [gc](SDL_GameControllerButton x) { return SDL_GameControllerGetButton(gc, x) == 1; };
        uint16_t m = 0;
        if (b(SDL_CONTROLLER_BUTTON_A)) m |= btn::A;
        if (b(SDL_CONTROLLER_BUTTON_B)) m |= btn::B;
        if (b(SDL_CONTROLLER_BUTTON_X)) m |= btn::X;
        if (b(SDL_CONTROLLER_BUTTON_Y)) m |= btn::Y;
        if (b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) m |= btn::LB;
        if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) m |= btn::RB;
        if (b(SDL_CONTROLLER_BUTTON_LEFTSTICK)) m |= btn::LS;
        if (b(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) m |= btn::RS;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) m |= btn::DUp;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) m |= btn::DDown;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) m |= btn::DLeft;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) m |= btn::DRight;
        const bool start = b(SDL_CONTROLLER_BUTTON_START);
        const bool view = b(SDL_CONTROLLER_BUTTON_BACK) || b(SDL_CONTROLLER_BUTTON_TOUCHPAD);
        // Options + Create/touchpad is the stream-menu chord. Each half is held back for
        // kChordGraceMs so the game never sees the first button of the chord; once both were
        // down together neither is forwarded until it is released (also after the menu fired).
        if (start && view) d.chordLatch = true;
        else if (!start && !view) d.chordLatch = false;
        if (d.gate(d.startGate, start, d.menuPulseUntil, now)) m |= btn::Menu;
        if (d.gate(d.viewGate, view, d.viewPulseUntil, now)) m |= btn::View;
        st.buttons = m;

        st.lx = clampAxis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        st.ly = clampAxis(-static_cast<int>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)));
        st.rx = clampAxis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        st.ry = clampAxis(-static_cast<int>(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY)));
        if (p->triggerSeen[0]) st.lt = triggerToXbox(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        if (p->triggerSeen[1]) st.rt = triggerToXbox(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        applyDeadzone(st.lx, st.ly, deadzonePercent);
        applyDeadzone(st.rx, st.ry, deadzonePercent);
    }
    if (now < d.nexusPulseUntil) st.buttons |= btn::Nexus;

    // Keyboard fallback (host development; also works with a USB keyboard on PS5).
    int numKeys = 0;
    const Uint8* ks = SDL_GetKeyboardState(&numKeys);
    if (ks && numKeys > SDL_SCANCODE_F1) {
        auto k = [ks](SDL_Scancode sc) { return ks[sc] != 0; };
        if (k(SDL_SCANCODE_RETURN) || k(SDL_SCANCODE_SPACE)) st.buttons |= btn::A;
        if (k(SDL_SCANCODE_BACKSPACE)) st.buttons |= btn::B;
        if (k(SDL_SCANCODE_X)) st.buttons |= btn::X;
        if (k(SDL_SCANCODE_Y)) st.buttons |= btn::Y;
        if (k(SDL_SCANCODE_Q)) st.buttons |= btn::LB;
        if (k(SDL_SCANCODE_E)) st.buttons |= btn::RB;
        if (k(SDL_SCANCODE_F)) st.buttons |= btn::LS;
        if (k(SDL_SCANCODE_H)) st.buttons |= btn::RS;
        if (k(SDL_SCANCODE_V) || k(SDL_SCANCODE_TAB)) st.buttons |= btn::View;
        if (k(SDL_SCANCODE_N)) st.buttons |= btn::Menu;
        if (k(SDL_SCANCODE_G)) st.buttons |= btn::Nexus;
        if (k(SDL_SCANCODE_UP)) st.buttons |= btn::DUp;
        if (k(SDL_SCANCODE_DOWN)) st.buttons |= btn::DDown;
        if (k(SDL_SCANCODE_LEFT)) st.buttons |= btn::DLeft;
        if (k(SDL_SCANCODE_RIGHT)) st.buttons |= btn::DRight;
        if (k(SDL_SCANCODE_Z)) st.lt = 65535;
        if (k(SDL_SCANCODE_C)) st.rt = 65535;
        auto axis = [&](SDL_Scancode neg, SDL_Scancode pos, int16_t& out) {
            int v = (k(pos) ? 32767 : 0) - (k(neg) ? 32767 : 0);
            if (v != 0) out = static_cast<int16_t>(v);
        };
        axis(SDL_SCANCODE_A, SDL_SCANCODE_D, st.lx);
        axis(SDL_SCANCODE_S, SDL_SCANCODE_W, st.ly);  // Xbox convention: up = +
        axis(SDL_SCANCODE_J, SDL_SCANCODE_L, st.rx);
        axis(SDL_SCANCODE_K, SDL_SCANCODE_I, st.ry);
    }
    return st;
}

bool Gamepad::menuChordTriggered(uint32_t holdMs) {
    Impl& d = *d_;
    const uint64_t now = platform::monotonicMs();
    // Re-check the chord against live button state (events may have been consumed elsewhere).
    d.updateCombo(now);
    if (d.guideDownAt != 0 && !d.guideConsumed && !d.button(SDL_CONTROLLER_BUTTON_GUIDE)) d.guideDownAt = 0;
    if (d.guideDownAt != 0 && !d.guideConsumed && now - d.guideDownAt >= holdMs) {
        d.guideConsumed = true;
        return true;
    }
    if (d.comboSince != 0 && !d.comboConsumed && now - d.comboSince >= holdMs) {
        d.comboConsumed = true;
        return true;
    }
    return false;
}

void Gamepad::rumble(const Vibration& v) {
    const Pad* p = d_->activePad();
    if (!p) return;
    auto scale = [](uint8_t pct) -> Uint16 { return static_cast<Uint16>(std::min<int>(100, pct) * 65535 / 100); };
    // The server describes a pulse train (duration on, delay off, repeated `repeat` more times).
    // SDL cannot play the gaps, so approximate the whole envelope as one effect like green-nx
    // (handle_input_report): duration + repeat * (duration + delay), capped at 4 s.
    uint32_t total = v.durationMs;
    if (v.durationMs && v.repeat)
        total += static_cast<uint32_t>(v.repeat) * (static_cast<uint32_t>(v.durationMs) + v.delayMs);
    const Uint32 dur = std::min<uint32_t>(total, kMaxRumbleMs);
    if (dur == 0 || (v.left == 0 && v.right == 0 && v.lt == 0 && v.rt == 0)) {
        SDL_GameControllerRumble(p->gc, 0, 0, 0);
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (p->hasTriggerRumble) SDL_GameControllerRumbleTriggers(p->gc, 0, 0, 0);
#endif
        return;
    }
    // Xbox: left = low-frequency (heavy) motor, right = high-frequency motor.
    SDL_GameControllerRumble(p->gc, scale(v.left), scale(v.right), dur);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (p->hasTriggerRumble) SDL_GameControllerRumbleTriggers(p->gc, scale(v.lt), scale(v.rt), dur);
#endif
}

void Gamepad::stopRumble() {
    for (auto& p : d_->pads) {
        SDL_GameControllerRumble(p.gc, 0, 0, 0);
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (p.hasTriggerRumble) SDL_GameControllerRumbleTriggers(p.gc, 0, 0, 0);
#endif
    }
}

}  // namespace xc
