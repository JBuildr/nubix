// Nubix — application object: owns config/auth/catalog/streamer/ui/gamepad, the screen
// stack and the SDL event loop; runs blocking work on a background worker.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/auth.hpp"
#include "core/catalog.hpp"
#include "core/config.hpp"
#include "platform/gamepad.hpp"
#include "stream/streamer.hpp"
#include "ui/screens.hpp"
#include "ui/ui.hpp"

namespace xc {

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Full lifecycle: load config, init SDL/UI/pad/HTTP, push Login or Home, run the loop until
    // quit(), shut down. Returns the process exit code.
    int run(int argc, char** argv);

    // Screen stack (applied between frames, so safe to call from screen callbacks).
    void push(std::unique_ptr<Screen> s);
    void pop();
    void replace(std::unique_ptr<Screen> s);  // pop + push
    // Clear the whole stack and push s (e.g. after logout).
    void resetTo(std::unique_ptr<Screen> s);

    // Request the main loop to exit.
    void quit();

    // Run work on the background worker thread (HTTP etc.), FIFO.
    void runAsync(std::function<void()> work);
    // Queue a function to run on the main thread before the next frame (thread-safe).
    void postToMain(std::function<void()> fn);

    // Show a transient toast message at the bottom of the screen.
    void toast(const std::string& msg);

    Config& config();
    Auth& auth();
    Catalog& catalog();
    Streamer& streamer();
    Ui& ui();
    Gamepad& gamepad();

    // Stream statistics overlay toggle (kept for the app's lifetime).
    bool statsOverlay() const;
    void setStatsOverlay(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
