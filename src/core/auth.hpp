// Nubix — Microsoft account + Xbox Live + gssv authentication.
// Flow: MSAL device code (client 1f907974-…) -> XBL user token -> XSTS (gssv relying party)
//       -> /v2/login/user gsToken per offering (xhome, xgpuweb, xgpuwebf2p) + LPT for xCloud /connect.
// Flow as implemented by green-nx (src/core/auth.cpp) and xal-node (src/msal.ts).
// Portions derived from green-nx (https://github.com/rmrf404/green-nx), Copyright (C) the green-nx authors, GPL-3.0; modified by Nubix contributors, 2026.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <mutex>
#include <string>

#include "core/config.hpp"

namespace xc {

struct DeviceCode {
    std::string userCode;         // short code the user types at verificationUri
    std::string deviceCode;       // opaque code used for polling
    std::string verificationUri;  // e.g. https://www.microsoft.com/link
    std::string qrUrl;            // verificationUri with ?otc=<userCode> for the QR code
    int interval = 5;             // poll interval in seconds
    int expiresIn = 900;          // lifetime in seconds
    std::string message;          // server's human-readable instruction text (English)
};

enum class PollResult { Pending, Success, Expired, Denied, Error };

class Auth {
public:
    // Tokens are read from and written to cfg (snapshots / locked updates); cfg.save() is
    // called on changes.
    explicit Auth(Config& cfg);

    // Start device-code login. Returns false and sets err on failure.
    bool requestDeviceCode(DeviceCode& out, std::string& err);

    // Poll the token endpoint once; the caller sleeps dc.interval between calls.
    // On Success the MSA tokens are stored in the config.
    PollResult pollToken(const DeviceCode& dc, std::string& err);

    // Refresh the MSA access token with the stored refresh token (persists the new refresh token).
    bool refresh(std::string& err);

    // XBL user -> XSTS(gssv) -> gsToken for xhome, xgpuweb, xgpuwebf2p (each optional), plus
    // gamertag/uhs. Succeeds if at least one offering returned a token.
    bool authorizeStreaming(std::string& err);

    // Fetch the passport long-lived token (LPT) needed by xCloud POST /connect.
    bool fetchLpt(std::string& outLpt, std::string& err);

    // True if a refresh token is stored. Never blocks on a running refresh.
    bool isLoggedIn() const;

    // A gssv/catalog request was rejected with an auth error (401/403) although the stored
    // gsExpiry says the tokens are valid (revocation, entitlement change, wall clock jump):
    // mark the streaming tokens expired so the next ensureFresh() re-authorizes.
    void invalidateStreaming();

    // Forget all tokens and persist.
    void logout();

    // Ensure MSA + gs tokens are valid for at least marginSec (refresh / re-authorize as needed).
    bool ensureFresh(std::string& err, int marginSec = 300);

    // Re-select xcloudBase / xcloudF2pBase from the stored regions after Settings::region changed
    // (no network), and persist. xhomeBase always uses the server default region.
    void applyRegion();

    // All public methods (except isLoggedIn / invalidateStreaming) are serialised by an internal
    // mutex, so the UI thread and the streamer thread may share one Auth. Config is itself
    // thread-safe (snapshots), so token readers never race these writers.

private:
    Config& cfg_;
    mutable std::recursive_mutex mu_;
};

}  // namespace xc
