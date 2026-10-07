// Nubix — xc-cli subcommands for the catalog + gssv session API (live testing).
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Run a gssv/catalog subcommand. argv[0] is the subcommand name:
//   titles [filter]                      playable cloud titles (both offerings merged, hydrated)
//   consoles                             xHome consoles
//   play <titleId> [--no-connect] [--timeout S]
//                                        start a cloud session, poll /state (queue, /connect at
//                                        ReadyToConnect) until Provisioned, then DELETE it
//   play --home <serverId> [--timeout S] same against a home console (xhome offering)
// Precondition: the caller has run platform::init(), logInit() and Http::globalInit().
// Uses the same config.json (platform::dataDir()) as the app. Returns a process exit code
// (0 ok, 1 failure, 2 usage).
int cli_gssv(int argc, char** argv);

// True if `cmd` is handled by cli_gssv().
bool cli_gssv_handles(const char* cmd);
