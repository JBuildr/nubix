// Nubix — QR code encoding (thin wrapper over Nayuki qrcodegen, MIT; see qrcodegen.hpp).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace xc {
namespace qr {

// Encode text (byte mode, ECC level M with boost). On success size = modules per side and
// modules[y * size + x] == true for dark modules. Returns false if the text is too long.
bool encode(const std::string& text, std::vector<bool>& modules, int& size);

}  // namespace qr
}  // namespace xc
