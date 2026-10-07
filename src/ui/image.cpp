// Nubix — still-image decoding via libavcodec + resampling.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include "core/log.hpp"

namespace xc {
namespace image {
namespace {

AVCodecID sniff(const uint8_t* d, size_t n) {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return AV_CODEC_ID_PNG;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return AV_CODEC_ID_MJPEG;
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return AV_CODEC_ID_WEBP;
    if (n >= 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) return AV_CODEC_ID_GIF;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return AV_CODEC_ID_BMP;
    return AV_CODEC_ID_NONE;
}

inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// BT.601 YCbCr -> RGB (still images are JPEG/WebP: BT.601), limited or full range.
inline void yuvToRgb(int y, int u, int v, bool full, uint8_t* o) {
    int c = full ? y : (y - 16);
    int dd = u - 128, e = v - 128;
    if (full) {
        o[0] = clamp8((256 * c + 359 * e + 128) >> 8);
        o[1] = clamp8((256 * c - 88 * dd - 183 * e + 128) >> 8);
        o[2] = clamp8((256 * c + 454 * dd + 128) >> 8);
    } else {
        o[0] = clamp8((298 * c + 409 * e + 128) >> 8);
        o[1] = clamp8((298 * c - 100 * dd - 208 * e + 128) >> 8);
        o[2] = clamp8((298 * c + 516 * dd + 128) >> 8);
    }
}

bool frameToRgba(const AVFrame* f, Rgba& out) {
    const int w = f->width, h = f->height;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(f->format);
    out.width = w;
    out.height = h;
    out.pixels.assign(static_cast<size_t>(w) * h * 4, 255);
    uint8_t* dst = out.pixels.data();

    auto rows = [&](auto fn) {
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0];
            uint8_t* o = dst + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x, o += 4) fn(s, x, o);
        }
    };

    switch (fmt) {
        case AV_PIX_FMT_RGBA:
            rows([](const uint8_t* s, int x, uint8_t* o) { std::memcpy(o, s + x * 4, 4); });
            return true;
        case AV_PIX_FMT_BGRA:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = s[x * 4 + 2]; o[1] = s[x * 4 + 1]; o[2] = s[x * 4]; o[3] = s[x * 4 + 3]; });
            return true;
        case AV_PIX_FMT_ARGB:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = s[x * 4 + 1]; o[1] = s[x * 4 + 2]; o[2] = s[x * 4 + 3]; o[3] = s[x * 4]; });
            return true;
        case AV_PIX_FMT_ABGR:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = s[x * 4 + 3]; o[1] = s[x * 4 + 2]; o[2] = s[x * 4 + 1]; o[3] = s[x * 4]; });
            return true;
        case AV_PIX_FMT_RGB0:
            rows([](const uint8_t* s, int x, uint8_t* o) { std::memcpy(o, s + x * 4, 3); });
            return true;
        case AV_PIX_FMT_BGR0:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = s[x * 4 + 2]; o[1] = s[x * 4 + 1]; o[2] = s[x * 4]; });
            return true;
        case AV_PIX_FMT_RGB24:
            rows([](const uint8_t* s, int x, uint8_t* o) { std::memcpy(o, s + x * 3, 3); });
            return true;
        case AV_PIX_FMT_BGR24:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = s[x * 3 + 2]; o[1] = s[x * 3 + 1]; o[2] = s[x * 3]; });
            return true;
        case AV_PIX_FMT_GRAY8:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = o[1] = o[2] = s[x]; });
            return true;
        case AV_PIX_FMT_YA8:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = o[1] = o[2] = s[x * 2]; o[3] = s[x * 2 + 1]; });
            return true;
        case AV_PIX_FMT_GRAY16BE:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = o[1] = o[2] = s[x * 2]; });
            return true;
        case AV_PIX_FMT_YA16BE:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = o[1] = o[2] = s[x * 4]; o[3] = s[x * 4 + 2]; });
            return true;
        case AV_PIX_FMT_RGB48BE:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = s[x * 6]; o[1] = s[x * 6 + 2]; o[2] = s[x * 6 + 4]; });
            return true;
        case AV_PIX_FMT_RGBA64BE:
            rows([](const uint8_t* s, int x, uint8_t* o) { o[0] = s[x * 8]; o[1] = s[x * 8 + 2]; o[2] = s[x * 8 + 4]; o[3] = s[x * 8 + 6]; });
            return true;
        case AV_PIX_FMT_PAL8: {
            const uint32_t* pal = reinterpret_cast<const uint32_t*>(f->data[1]);  // native-endian ARGB
            if (!pal) return false;
            rows([pal](const uint8_t* s, int x, uint8_t* o) {
                uint32_t c = pal[s[x]];
                o[0] = (c >> 16) & 0xFF; o[1] = (c >> 8) & 0xFF; o[2] = c & 0xFF; o[3] = (c >> 24) & 0xFF;
            });
            return true;
        }
        default: break;
    }

    // Generic 8-bit planar YUV(A) (yuv420p, yuvj422p, yuv444p, yuva420p, ...).
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (desc && !(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)) &&
        (desc->flags & AV_PIX_FMT_FLAG_PLANAR) && desc->nb_components >= 3 && desc->comp[0].depth == 8 &&
        desc->comp[1].depth == 8 && desc->comp[2].depth == 8) {
        const bool full = f->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P ||
                          fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ444P || fmt == AV_PIX_FMT_YUVJ440P ||
                          fmt == AV_PIX_FMT_YUVJ411P;
        const int sx = desc->log2_chroma_w, sy = desc->log2_chroma_h;
        const bool alpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) && desc->nb_components == 4;
        const int pY = desc->comp[0].plane, pU = desc->comp[1].plane, pV = desc->comp[2].plane;
        const int pA = alpha ? desc->comp[3].plane : 0;
        for (int y = 0; y < h; ++y) {
            const uint8_t* ry = f->data[pY] + static_cast<ptrdiff_t>(y) * f->linesize[pY];
            const uint8_t* ru = f->data[pU] + static_cast<ptrdiff_t>(y >> sy) * f->linesize[pU];
            const uint8_t* rv = f->data[pV] + static_cast<ptrdiff_t>(y >> sy) * f->linesize[pV];
            const uint8_t* ra = alpha ? f->data[pA] + static_cast<ptrdiff_t>(y) * f->linesize[pA] : nullptr;
            uint8_t* o = dst + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x, o += 4) {
                yuvToRgb(ry[x], ru[x >> sx], rv[x >> sx], full, o);
                if (ra) o[3] = ra[x];
            }
        }
        return true;
    }
    XC_LOGW("image: unsupported pixel format %s", av_get_pix_fmt_name(fmt) ? av_get_pix_fmt_name(fmt) : "?");
    return false;
}

}  // namespace

bool decode(const uint8_t* data, size_t size, Rgba& out) {
    out = Rgba();
    if (!data || size < 8 || size > (64u << 20)) return false;
    const AVCodecID id = sniff(data, size);
    if (id == AV_CODEC_ID_NONE) {
        XC_LOGD("image: unknown format (%zu bytes)", size);
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) {
        XC_LOGW("image: no decoder for %s", avcodec_get_name(id));
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = false;
    if (ctx && pkt && frame) {
        ctx->thread_count = 1;
        if (avcodec_open2(ctx, codec, nullptr) == 0 && av_new_packet(pkt, static_cast<int>(size)) == 0) {
            std::memcpy(pkt->data, data, size);
            int rc = avcodec_send_packet(ctx, pkt);
            if (rc >= 0) {
                rc = avcodec_receive_frame(ctx, frame);
                if (rc == AVERROR(EAGAIN)) {
                    avcodec_send_packet(ctx, nullptr);  // flush (some decoders emit on drain)
                    rc = avcodec_receive_frame(ctx, frame);
                }
                if (rc == 0) ok = frameToRgba(frame, out);
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (!ok) out = Rgba();
    return ok;
}

void resize(const Rgba& src, int w, int h, bool cover, Rgba& out) {
    out = Rgba();
    if (src.empty() || w <= 0 || h <= 0) return;
    // Source window (cover = centred crop with the target aspect).
    double cx0 = 0, cy0 = 0, cw = src.width, ch = src.height;
    if (cover) {
        const double ta = static_cast<double>(w) / h, sa = static_cast<double>(src.width) / src.height;
        if (sa > ta) {
            cw = src.height * ta;
            cx0 = (src.width - cw) / 2;
        } else {
            ch = src.width / ta;
            cy0 = (src.height - ch) / 2;
        }
    }
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h * 4);
    const double fx = cw / w, fy = ch / h;
    const uint8_t* sp = src.pixels.data();
    const int sw = src.width, sh = src.height;

    for (int y = 0; y < h; ++y) {
        uint8_t* o = out.pixels.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x, o += 4) {
            if (fx > 1.0 || fy > 1.0) {
                // Box filter over the source footprint (alpha-weighted to avoid dark fringes).
                int x0 = static_cast<int>(cx0 + x * fx), x1 = static_cast<int>(cx0 + (x + 1) * fx);
                int y0 = static_cast<int>(cy0 + y * fy), y1 = static_cast<int>(cy0 + (y + 1) * fy);
                x0 = std::clamp(x0, 0, sw - 1);
                y0 = std::clamp(y0, 0, sh - 1);
                x1 = std::clamp(std::max(x1, x0 + 1), 1, sw);
                y1 = std::clamp(std::max(y1, y0 + 1), 1, sh);
                uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int yy = y0; yy < y1; ++yy) {
                    const uint8_t* p = sp + (static_cast<size_t>(yy) * sw + x0) * 4;
                    for (int xx = x0; xx < x1; ++xx, p += 4) {
                        r += p[0] * p[3];
                        g += p[1] * p[3];
                        b += p[2] * p[3];
                        a += p[3];
                        ++n;
                    }
                }
                if (a > 0) {
                    o[0] = static_cast<uint8_t>(r / a);
                    o[1] = static_cast<uint8_t>(g / a);
                    o[2] = static_cast<uint8_t>(b / a);
                } else {
                    o[0] = o[1] = o[2] = 0;
                }
                o[3] = static_cast<uint8_t>(n ? a / n : 0);
            } else {
                // Bilinear.
                double sxf = cx0 + (x + 0.5) * fx - 0.5, syf = cy0 + (y + 0.5) * fy - 0.5;
                sxf = std::clamp(sxf, 0.0, sw - 1.0);
                syf = std::clamp(syf, 0.0, sh - 1.0);
                const int ix = static_cast<int>(sxf), iy = static_cast<int>(syf);
                const int ix1 = std::min(ix + 1, sw - 1), iy1 = std::min(iy + 1, sh - 1);
                const double ax = sxf - ix, ay = syf - iy;
                const uint8_t* p00 = sp + (static_cast<size_t>(iy) * sw + ix) * 4;
                const uint8_t* p10 = sp + (static_cast<size_t>(iy) * sw + ix1) * 4;
                const uint8_t* p01 = sp + (static_cast<size_t>(iy1) * sw + ix) * 4;
                const uint8_t* p11 = sp + (static_cast<size_t>(iy1) * sw + ix1) * 4;
                for (int c = 0; c < 4; ++c) {
                    const double top = p00[c] + (p10[c] - p00[c]) * ax;
                    const double bot = p01[c] + (p11[c] - p01[c]) * ax;
                    o[c] = clamp8(static_cast<int>(std::lround(top + (bot - top) * ay)));
                }
            }
        }
    }
}

}  // namespace image
}  // namespace xc
