// Nubix — helpers shared by the screen implementations (not part of the public UI API).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "platform/gamepad.hpp"
#include "ui/screens.hpp"
#include "ui/ui.hpp"

namespace xc {

class App;

namespace ui {

// One entry of the bottom hint bar: a controller glyph (or the matching key on keyboard).
struct Hint {
    PadIcon icon;
    std::string label;
};

// Bottom-right hint bar ("✕ Select   ○ Back"), keyboard key names when no pad is in use.
void hintBar(App& app, Ui& ui, const std::vector<Hint>& hints);

// Keyboard key that corresponds to a pad glyph in the UI ("Enter", "Esc", ...).
const char* keyFor(PadIcon icon);

// Draw one glyph-or-key + label pair; returns the width used.
int hintItem(App& app, Ui& ui, const Hint& h, int x, int cy);

// Background with a subtle green glow at the top.
void background(Ui& ui);

// Product logo (navy square with a blue X + "Nubix" wordmark, plus "for Xbox Cloud Gaming" unless compact).
void logo(Ui& ui, int x, int y, bool compact = false);

// Error badge (red roundel with "!") centred at (cx, cy).
void errorBadge(Ui& ui, int cx, int cy, int radius);

// "m:ss" / "h:mm:ss".
std::string formatDuration(int64_t seconds);

// Pretty console model name ("XboxSeriesX" -> "Xbox Series X").
std::string prettyConsoleType(const std::string& t);

// Modal confirmation dialog over the current screen. onConfirm runs on the main thread.
std::unique_ptr<Screen> makeConfirmScreen(const std::string& title, const std::string& message,
                                          const std::string& confirmLabel, std::function<void(App&)> onConfirm);

// Confirm + quit the app.
std::unique_ptr<Screen> makeExitConfirm();

// Stream screen with a title for the overlay menu.
std::unique_ptr<Screen> makeStreamScreenNamed(const std::string& displayName);

// Settings list used by the Home "Settings" tab and the standalone Settings screen.
class SettingsPanel {
public:
    SettingsPanel();
    ~SettingsPanel();
    // Returns true if the key was consumed.
    bool onNav(App& app, NavKey key);
    void render(App& app, Ui& ui, int top);
    void resetFocus();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace ui
}  // namespace xc
