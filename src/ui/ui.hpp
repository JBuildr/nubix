// Nubix — immediate-mode drawing helpers on SDL_Renderer + SDL2_ttf: text, rects,
// buttons, grid tiles, spinner, QR code, images. Logical resolution 1920x1080.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace xc {

struct Color {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

namespace colors {
constexpr Color Background{16, 20, 18, 255};
constexpr Color Panel{32, 38, 35, 255};
constexpr Color Accent{36, 110, 240, 255};  // Nubix blue
constexpr Color Text{240, 245, 240, 255};
constexpr Color TextDim{150, 160, 155, 255};
constexpr Color Error{220, 70, 60, 255};
// additional palette
constexpr Color PanelHi{44, 52, 48, 255};       // hovered / raised panel
constexpr Color PanelLo{24, 29, 27, 255};       // sunken panel, header bar
constexpr Color AccentBright{92, 170, 255, 255}; // focus ring, highlights
constexpr Color Warning{232, 170, 40, 255};
constexpr Color Black{0, 0, 0, 255};
constexpr Color White{255, 255, 255, 255};
}  // namespace colors

// Returns c with alpha replaced.
constexpr Color withAlpha(Color c, uint8_t a) { return Color{c.r, c.g, c.b, a}; }

enum class Align { Left, Center, Right };

// Controller / keyboard hint glyphs drawn by Ui::padIcon.
enum class PadIcon { Cross, Circle, Square, Triangle, L1, R1, Options, Touchpad, PS };

class Ui {
public:
    static constexpr int kWidth = 1920;   // logical width
    static constexpr int kHeight = 1080;  // logical height

    Ui();
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Create the window + renderer (fullscreen on PS5, resizable window on host), set logical
    // size, init SDL_ttf and load the regular + bold fonts.
    bool init(const std::string& fontPath, const std::string& boldFontPath, const std::string& title);

    // Destroy textures, fonts, renderer and window.
    void shutdown();

    SDL_Renderer* renderer() const;
    SDL_Window* window() const;

    // Clear to the background color / present the frame.
    void beginFrame();
    void endFrame();

    // Draw UTF-8 text with its top edge at y. Glyph textures are cached per (text, size, bold, color).
    // Returns the rendered width.
    int text(const std::string& s, int x, int y, int size, Color c = colors::Text, Align align = Align::Left,
             bool bold = false);

    // Draw text wrapped to maxWidth; returns the total height used.
    int textWrapped(const std::string& s, int x, int y, int maxWidth, int size, Color c = colors::Text,
                    bool bold = false);

    // Width in pixels of s at the given size.
    int textWidth(const std::string& s, int size, bool bold = false);

    // Filled or outlined rectangle; radius > 0 rounds the corners.
    void rect(int x, int y, int w, int h, Color c, bool filled = true, int radius = 0);

    // Button with label; focused draws the highlight.
    void button(const std::string& label, int x, int y, int w, int h, bool focused);

    // Grid tile with optional image (box art), caption and badge text (e.g. "Game Pass").
    void tile(SDL_Texture* image, const std::string& caption, const std::string& badge, int x, int y, int w, int h,
              bool focused);

    // Animated loading spinner centred at (cx, cy); tMs = monotonic time.
    void spinner(int cx, int cy, int radius, uint64_t tMs);

    // QR code of data inside a white square of side `size` at (x, y). Cached per data string.
    void qr(const std::string& data, int x, int y, int size);

    // Draw a texture scaled to the rectangle (aspect-fit if fit == true).
    void image(SDL_Texture* tex, int x, int y, int w, int h, bool fit = true);

    // Decode PNG/JPEG bytes (via libavcodec) into a new texture; nullptr on failure. Caller owns it.
    SDL_Texture* textureFromImage(const std::vector<uint8_t>& bytes);

    // Drop cached text/QR textures (call on screen change if memory is tight).
    void clearCache();

    // ---- additions -------------------------------------------------------------------------

    // Outlined rectangle with the given line thickness (logical px); radius > 0 rounds corners.
    void outline(int x, int y, int w, int h, Color c, int thickness, int radius = 0);

    // Vertical gradient fill (top color -> bottom color).
    void gradient(int x, int y, int w, int h, Color top, Color bottom);

    // Filled circle centred at (cx, cy).
    void circle(int cx, int cy, int radius, Color c);

    // Thick line segment (anti-aliasing not guaranteed).
    void line(float x0, float y0, float x1, float y1, float thickness, Color c);

    // Text truncated with "…" to fit maxWidth. Returns the drawn width.
    int textEllipsized(const std::string& s, int x, int y, int maxWidth, int size, Color c = colors::Text,
                       Align align = Align::Left, bool bold = false);

    // Line height (distance between baselines) for the font size.
    int lineHeight(int size, bool bold = false);

    // Controller glyph (PlayStation style) in a circle/pill of height `size`, centred
    // vertically at cy, starting at x. Returns the width used.
    int padIcon(PadIcon icon, int x, int cy, int size);

    // Keyboard key cap with a label (host hints), same metrics as padIcon. Returns width used.
    int keyCap(const std::string& label, int x, int cy, int size);

    // Box art / remote image, decoded + scaled to exactly (w x h) logical px (cover crop) on a
    // background loader thread and cached (LRU). Returns nullptr until loaded (or on failure).
    // Call every frame for visible items; requests not repeated for a while are dropped.
    SDL_Texture* remoteImage(const std::string& url, int w, int h);

    // Number of remote images queued or downloading.
    int remoteImagesPending() const;

    // Mask the corners of a rectangle with `bg` so that content drawn inside looks rounded.
    void roundCorners(int x, int y, int w, int h, int radius, Color bg);

    // Clip subsequent drawing to the rectangle (logical coords); clearClip() resets.
    void setClip(int x, int y, int w, int h);
    void clearClip();

    // Save the current back buffer (call before endFrame) as a BMP file.
    bool saveScreenshot(const std::string& path);

    // Monotonic frame counter (incremented by endFrame) and the output scale (pixels per
    // logical px; 1.0 on PS5).
    uint64_t frameCount() const;
    float scale() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace xc
