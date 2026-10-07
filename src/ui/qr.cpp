// Nubix — QR code encoding.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/qr.hpp"

#include <exception>

#include "ui/qrcodegen.hpp"

namespace xc {
namespace qr {

bool encode(const std::string& text, std::vector<bool>& modules, int& size) {
    modules.clear();
    size = 0;
    try {
        const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        size = code.getSize();
        modules.resize(static_cast<size_t>(size) * static_cast<size_t>(size));
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) modules[static_cast<size_t>(y) * size + x] = code.getModule(x, y);
        return true;
    } catch (const std::exception&) {
        modules.clear();
        size = 0;
        return false;
    }
}

}  // namespace qr
}  // namespace xc
