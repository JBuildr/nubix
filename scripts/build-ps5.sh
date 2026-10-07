#!/usr/bin/env bash
# Nubix — PS5 payload build, run inside the "ps5dev" docker image (docker/Dockerfile:
# ubuntu 24.04 + clang-18 + pacbrew ps5-payload-dev v0.40.2 at $PS5_PAYLOAD_SDK).
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# Usage: scripts/build-ps5.sh [Release|Debug|RelWithDebInfo]
# Output:
#   build-ps5/nubix.elf        stripped payload (send to elfldr :9021)
#   build-ps5/nubix-debug.elf  unstripped, for symbolizing crashes
#   dist/nubix/                websrv homebrew folder -> copy to /data/homebrew/nubix/
#       eboot.elf, homebrew.js, sce_sys/icon0.png, assets/,
#       LICENSE, THIRD_PARTY_NOTICES.md, THIRD_PARTY_LICENSES.txt, SOURCE.txt (if present)
#   build-ps5/nubix-v<version>-websrv.zip   the nubix/ folder, zipped for release
#
# The docker image is built from docker/Dockerfile on first use. Override with PS5DEV_IMAGE.
# XC_LINKCHECK=ON additionally builds build-ps5/xc-linkcheck.elf (full dependency link test).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_TYPE="${1:-Release}"
IMAGE="${PS5DEV_IMAGE:-ps5dev}"

if [[ -z "${PS5_PAYLOAD_SDK:-}" ]]; then
  # ---- host side: make sure image + deps exist, then re-run ourselves in docker ----
  if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo ">> building docker image ${IMAGE} from docker/Dockerfile"
    docker build -t "${IMAGE}" "${ROOT}/docker"
  fi
  if [[ ! -f "${ROOT}/deps/ps5/lib/libdatachannel.a" ]]; then
    bash "${SCRIPT_DIR}/build-deps.sh" ps5
  fi
  JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  exec docker run --rm -v "${ROOT}:/w" -w /w -e JOBS="${JOBS}" -e XC_LINKCHECK="${XC_LINKCHECK:-OFF}" "${IMAGE}" \
    bash /w/scripts/build-ps5.sh "${BUILD_TYPE}"
fi

# ---- inside the container ----
# shellcheck disable=SC1091
source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "${ROOT}/deps/ps5/lib/libdatachannel.a" ]]; then
  bash "${SCRIPT_DIR}/build-deps.sh" ps5
fi

# A build dir configured on another machine/path cannot be reused.
if [[ -f "${ROOT}/build-ps5/CMakeCache.txt" ]] && ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=${ROOT}$" "${ROOT}/build-ps5/CMakeCache.txt"; then
  rm -rf "${ROOT}/build-ps5"
fi

"${CMAKE}" -S "${ROOT}" -B "${ROOT}/build-ps5" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_VERBOSE_MAKEFILE=OFF -DXC_BUILD_TESTS=OFF -DXC_BUILD_CLI=OFF \
  -DXC_BUILD_LINKCHECK="${XC_LINKCHECK:-OFF}"
cmake --build "${ROOT}/build-ps5" -j"${JOBS}"

# ---- websrv homebrew layout ----
DIST="${ROOT}/dist/nubix"
rm -rf "${DIST}"
mkdir -p "${DIST}/sce_sys" "${DIST}/assets"
cp "${ROOT}/build-ps5/nubix.elf" "${DIST}/eboot.elf"
cp "${ROOT}/homebrew/homebrew.js" "${DIST}/homebrew.js"
cp "${ROOT}/homebrew/sce_sys/icon0.png" "${DIST}/sce_sys/icon0.png"
cp -R "${ROOT}/assets/." "${DIST}/assets/"
rm -f "${DIST}/assets/icon0.png"
cp "${ROOT}/LICENSE" "${DIST}/LICENSE"
cp "${ROOT}/THIRD_PARTY_NOTICES.md" "${DIST}/THIRD_PARTY_NOTICES.md"
cp "${ROOT}/THIRD_PARTY_LICENSES.txt" "${DIST}/THIRD_PARTY_LICENSES.txt"
if [[ -f "${ROOT}/SOURCE.txt" ]]; then
  cp "${ROOT}/SOURCE.txt" "${DIST}/SOURCE.txt"
fi

# ---- release zip: build-ps5/nubix-v<version>-websrv.zip containing nubix/ ----
VERSION="$(sed -nE 's/^[[:space:]]*project\([^)]*VERSION[[:space:]]+([0-9][0-9A-Za-z.+-]*).*/\1/p' "${ROOT}/CMakeLists.txt" | head -n1)"
if [[ -z "${VERSION}" ]]; then
  echo "!! could not read project(VERSION) from CMakeLists.txt" >&2
  exit 1
fi
ZIP="${ROOT}/build-ps5/nubix-v${VERSION}-websrv.zip"
rm -f "${ZIP}"
(cd "$(dirname "${DIST}")" && python3 - "${ZIP}" "$(basename "${DIST}")" <<'PY'
import os, sys, zipfile
out, top = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, dirs, files in os.walk(top):
        dirs.sort()
        z.write(d, d + "/")
        for f in sorted(files):
            z.write(os.path.join(d, f))
PY
)

echo ">> $(ls -l "${ROOT}/build-ps5/nubix.elf")"
echo ">> dist: ${DIST}"
echo ">> zip:  ${ZIP}"
