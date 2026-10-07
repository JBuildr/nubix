// Nubix — internal helpers of the Http module (used only by core/http.cpp and core/auth.cpp).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace xc {
namespace httpdetail {

// While alive, non-2xx responses on the current thread are logged at Debug instead of Warn.
// Used for expected errors such as the 400 "authorization_pending" device-code poll replies.
class QuietErrors {
public:
    QuietErrors();
    ~QuietErrors();
    QuietErrors(const QuietErrors&) = delete;
    QuietErrors& operator=(const QuietErrors&) = delete;

private:
    bool prev_;
};

}  // namespace httpdetail
}  // namespace xc
