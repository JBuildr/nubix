#!/usr/bin/env bash
# Nubix — host (macOS/Linux) build: deps (if missing) + app + xc-cli + tests.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# Usage: scripts/build-host.sh [Release|Debug] [--test]
# Output: build-host/nubix, build-host/xc-cli, build-host/xc-tests
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_TYPE="Release"
RUN_TESTS=0
for a in "$@"; do
  case "$a" in
    Release|Debug|RelWithDebInfo) BUILD_TYPE="$a" ;;
    --test) RUN_TESTS=1 ;;
    *) echo "usage: $0 [Release|Debug|RelWithDebInfo] [--test]" >&2; exit 2 ;;
  esac
done

if [[ ! -f "${ROOT}/deps/host/lib/libdatachannel.a" ]]; then
  bash "${SCRIPT_DIR}/build-deps.sh" host
fi

# Homebrew: keg-only openssl@3 must be visible to pkg-config
if command -v brew >/dev/null 2>&1; then
  OSSL="$(brew --prefix openssl@3 2>/dev/null || true)"
  [[ -n "${OSSL}" ]] && export PKG_CONFIG_PATH="${OSSL}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
  # prefer Homebrew curl (OpenSSL backend, same as PS5) over the macOS system curl if installed
  CURLP="$(brew --prefix curl 2>/dev/null || true)"
  [[ -n "${CURLP}" && -d "${CURLP}/lib/pkgconfig" ]] && export PKG_CONFIG_PATH="${CURLP}/lib/pkgconfig:${PKG_CONFIG_PATH}"
fi

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cmake -S "${ROOT}" -B "${ROOT}/build-host" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
cmake --build "${ROOT}/build-host" -j"${JOBS}"

echo ">> built ${ROOT}/build-host/nubix"
if [[ ${RUN_TESTS} -eq 1 ]]; then
  (cd "${ROOT}/build-host" && ctest --output-on-failure)
fi
