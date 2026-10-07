// Nubix — UI screens: Login, Home (Cloud library / My consoles / Settings tabs),
// Settings, Connecting, Stream (video + overlay menu + stats).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include "core/gssv.hpp"
#include "platform/gamepad.hpp"

namespace xc {

class App;
class Ui;

// Base class for a screen on the App's stack. All methods run on the main thread.
class Screen {
public:
    virtual ~Screen() = default;
    // Became the top screen.
    virtual void onEnter(App& app) { (void)app; }
    // Another screen was pushed on top / this one is popped.
    virtual void onLeave(App& app) { (void)app; }
    // A navigation key was pressed.
    virtual void onNav(App& app, NavKey key) = 0;
    // Per-frame logic (poll async results, streamer state, ...). dtMs = frame time.
    virtual void update(App& app, uint32_t dtMs) = 0;
    // Draw the screen.
    virtual void render(App& app, Ui& ui) = 0;
    // True while the screen forwards raw pad state to the stream instead of NavKeys.
    virtual bool capturesGamepad() const { return false; }
    // True for modal dialogs: the screen below is rendered first (dimmed by the dialog).
    virtual bool isOverlay() const { return false; }
};

// Device-code login with QR; on success replaces itself with the Home screen.
std::unique_ptr<Screen> makeLoginScreen();
// Tabbed home: Cloud library grid, My consoles list, Settings.
std::unique_ptr<Screen> makeHomeScreen();
// Settings list (resolution, bitrate, region, F2P fallback, logout).
std::unique_ptr<Screen> makeSettingsScreen();
// Starts the Streamer and shows status/queue until Streaming, then replaces itself with the
// Stream screen. displayName is shown in the status text.
// f2pOnly: cloud title playable only via the free-to-play offering (Title::f2pOnly).
std::unique_ptr<Screen> makeConnectingScreen(SessionKind kind, const std::string& titleOrServerId,
                                             const std::string& displayName, bool f2pOnly = false);
// Full-screen video, input forwarding, hold-PS overlay menu and stats.
std::unique_ptr<Screen> makeStreamScreen();

}  // namespace xc
