// Nubix — still-image decoding (PNG/JPEG/WebP/GIF/BMP via libavcodec) into RGBA8 pixels,
// plus a CPU resampler, so box art can be decoded + scaled off the main thread.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xc {
namespace image {

// Tightly packed RGBA8 (byte order R,G,B,A), width * height * 4 bytes.
struct Rgba {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
    bool empty() const { return width <= 0 || height <= 0; }
};

// Decode an encoded image (format sniffed from magic bytes). Thread-safe.
bool decode(const uint8_t* data, size_t size, Rgba& out);

// Scale src to exactly w x h. cover = true crops to the target aspect ratio first (centred,
// like CSS object-fit: cover); false stretches. Box filter when shrinking, bilinear otherwise.
void resize(const Rgba& src, int w, int h, bool cover, Rgba& out);

}  // namespace image
}  // namespace xc
