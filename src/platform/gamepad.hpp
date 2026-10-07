// Nubix — SDL GameController -> xc::GamepadState (DualSense -> Xbox mapping), rumble,
// hotplug, and UI navigation edges.
// Mapping: Cross=A, Circle=B, Square=X, Triangle=Y, L1=LB, R1=RB, L2=LT, R2=RT, L3=LS, R3=RS,
// Options=Menu, touchpad click=View, D-pad=D-pad. Host only (the PS5 pad driver does not report
// these buttons): Create=View, PS=Nexus (short press).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/protocol.hpp"

union SDL_Event;

namespace xc {

// Edge-triggered navigation keys for the UI (from pad or keyboard on host).
// Pad: A=Accept, B=Back, Y=Option, LB/LT=TabLeft, RB/RT=TabRight, D-pad/left stick=directions.
// Keyboard: arrows, Enter/Space=Accept, Esc/Backspace=Back, Q/E=tabs, Y/Tab=Option, F1=Menu.
// The pad's Options button is never a NavKey (it is the game's Menu button while streaming).
enum class NavKey { None, Up, Down, Left, Right, Accept, Back, TabLeft, TabRight, Option, Menu };

class Gamepad {
public:
    Gamepad();
    ~Gamepad();

    // Init SDL_INIT_GAMECONTROLLER subsystem state and open already connected pads.
    bool init();

    // Close all controllers.
    void shutdown();

    // Feed every SDL event (controller add/remove, buttons, axes; keyboard on host).
    // Returns a navigation key for UI use (None if the event is not a navigation press).
    NavKey handleEvent(const SDL_Event& ev);

    // True if at least one controller is connected.
    bool connected() const;

    // Current mapped state of the active controller (deadzone percent applied radially).
    // Menu (Options) and View (Create/touchpad) are held back ~140 ms after being pressed and
    // dropped entirely while they form the stream-menu chord (a tap shorter than that is
    // replayed as a short press). Call once per frame while streaming.
    GamepadState state(int deadzonePercent = 0) const;

    // True once when the stream-menu chord was held long enough: Options + touchpad (PS5 and
    // host), Options + Create or the PS button (host only), held for holdMs. While held, the PS
    // button is NOT forwarded as Nexus.
    bool menuChordTriggered(uint32_t holdMs = 1000);

    // Apply a server rumble command (SDL_GameControllerRumble + trigger rumble if supported).
    void rumble(const Vibration& v);

    // Auto-repeat for held directions (D-pad, left stick, arrow keys are repeated by the OS):
    // call once per frame; returns a direction NavKey when a repeat is due, else None.
    NavKey pollRepeat();

    // Name of the active controller ("" if none), for diagnostics / UI.
    std::string name() const;

    // True if the last navigation input came from the keyboard (UI shows key names then).
    bool lastInputKeyboard() const;

    // Stop all rumble immediately (e.g. when a stream ends or the overlay opens).
    void stopRumble();

    // Host keyboard -> pad mapping used while streaming (merged into state()):
    // arrows = D-pad, WASD = left stick, IJKL = right stick, Enter/Space = A, Backspace = B,
    // X = X, Y = Y, Q = LB, E = RB, Z = LT, C = RT, V/Tab = View, N = Menu, G = Nexus,
    // F = LS, H = RS. F1 opens the stream menu (NavKey::Menu).

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
