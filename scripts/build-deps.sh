#!/usr/bin/env bash
# Nubix — build third-party dependencies that are not part of the PS5 SDK.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# Usage: scripts/build-deps.sh <ps5|host> [--clean]
#
# Fetches libdatachannel v0.24.6 (with submodules), applies patches/libdatachannel-*.patch
# and installs a static build into deps/<target>/ (include/, lib/).
#
#   ps5  : runs inside the "ps5dev" docker image (built from docker/Dockerfile if missing),
#          cross-compiled with the prospero toolchain against the SDK's OpenSSL.
#   host : native build against Homebrew openssl@3 (macOS) or system OpenSSL (Linux).
#
# Environment:
#   LDC_LOCAL_SRC   optional path to an existing libdatachannel v0.24.6 checkout (with
#                   submodules) to copy instead of cloning from GitHub.
#   PS5DEV_IMAGE    docker image name (default: ps5dev)
#   JOBS            parallel build jobs
set -euo pipefail

LDC_VERSION="v0.24.6"
LDC_REPO="https://github.com/paullouisageneau/libdatachannel.git"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET="${1:-}"
shift || true
CLEAN=0
IN_DOCKER=0
for a in "$@"; do
  case "$a" in
    --clean) CLEAN=1 ;;
    --in-docker) IN_DOCKER=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

case "${TARGET}" in
  ps5|host) ;;
  *) echo "usage: $0 <ps5|host> [--clean]" >&2; exit 2 ;;
esac

ncpu() {
  if [[ -n "${JOBS:-}" ]]; then echo "${JOBS}"; return; fi
  getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4
}

SRC="${ROOT}/.cache/libdatachannel-${LDC_VERSION}"
BUILD="${ROOT}/.cache/build-libdatachannel-${TARGET}"
DEPS_PREFIX="${ROOT}/deps/${TARGET}"

# ---------------------------------------------------------------- source ----
fetch_source() {
  if [[ -f "${SRC}/.xc-ready" ]]; then
    return
  fi
  rm -rf "${SRC}"
  mkdir -p "${ROOT}/.cache"
  if [[ -n "${LDC_LOCAL_SRC:-}" && -f "${LDC_LOCAL_SRC}/CMakeLists.txt" ]]; then
    echo ">> copying libdatachannel from ${LDC_LOCAL_SRC}"
    mkdir -p "${SRC}"
    # copy everything except previous build trees
    (cd "${LDC_LOCAL_SRC}" && tar cf - --exclude='./build*' .) | (cd "${SRC}" && tar xf -)
    # discard local modifications so the patch applies to pristine sources
    if [[ -d "${SRC}/.git" ]]; then
      (cd "${SRC}" && git checkout -q -- . && git submodule foreach -q --recursive git checkout -q -- . ) || true
    fi
  else
    echo ">> cloning libdatachannel ${LDC_VERSION}"
    git clone --depth 1 --branch "${LDC_VERSION}" --recurse-submodules --shallow-submodules \
      "${LDC_REPO}" "${SRC}"
  fi
  for p in "${ROOT}"/patches/libdatachannel-*.patch; do
    [[ -f "$p" ]] || continue
    echo ">> applying $(basename "$p")"
    (cd "${SRC}" && git apply --whitespace=nowarn "$p")
  done
  touch "${SRC}/.xc-ready"
}

# ------------------------------------------------------------------ build ---
common_args=(
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF
  -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON
  -DUSE_GNUTLS=OFF -DUSE_MBEDTLS=OFF -DUSE_NICE=OFF
  -Dsctp_werror=OFF -DWARNINGS_AS_ERRORS=OFF
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_INSTALL_PREFIX="${DEPS_PREFIX}"
  -DCMAKE_INSTALL_LIBDIR=lib
)

build_host() {
  local ossl=""
  if command -v brew >/dev/null 2>&1; then
    ossl="$(brew --prefix openssl@3 2>/dev/null || true)"
  fi
  local extra=()
  [[ -n "${ossl}" ]] && extra+=(-DOPENSSL_ROOT_DIR="${ossl}")
  cmake -S "${SRC}" -B "${BUILD}" "${common_args[@]}" ${extra[@]+"${extra[@]}"}
  cmake --build "${BUILD}" -j"$(ncpu)" --target datachannel
  cmake --install "${BUILD}"
}

build_ps5_in_container() {
  # shellcheck disable=SC1091
  source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"
  "${CMAKE}" -S "${SRC}" -B "${BUILD}" "${common_args[@]}" \
    -DOPENSSL_USE_STATIC_LIBS=ON -DOPENSSL_ROOT_DIR="${PS5_SYSROOT}/user/homebrew" \
    -DCMAKE_C_FLAGS="-DXC_NET_COMPAT_REDIRECT -include ${ROOT}/src/platform/net_compat.h" \
    -DCMAKE_CXX_FLAGS="-DXC_NET_COMPAT_REDIRECT -include ${ROOT}/src/platform/net_compat.h"
  cmake --build "${BUILD}" -j"$(ncpu)" --target datachannel
  # prospero.sh exports DESTDIR=<sysroot>; install into our own prefix instead.
  DESTDIR= cmake --install "${BUILD}"
}

docker_image_ensure() {
  local img="${PS5DEV_IMAGE:-ps5dev}"
  if ! docker image inspect "${img}" >/dev/null 2>&1; then
    echo ">> building docker image ${img} from docker/Dockerfile"
    docker build -t "${img}" "${ROOT}/docker"
  fi
}

if [[ ${CLEAN} -eq 1 && ${IN_DOCKER} -eq 0 ]]; then
  rm -rf "${BUILD}" "${DEPS_PREFIX}" "${SRC}"
fi

if [[ "${TARGET}" == "ps5" && -z "${PS5_PAYLOAD_SDK:-}" ]]; then
  # Host side: fetch sources (needs git/network), then re-enter inside docker.
  fetch_source
  docker_image_ensure
  exec docker run --rm -v "${ROOT}:/w" -w /w -e JOBS="$(ncpu)" "${PS5DEV_IMAGE:-ps5dev}" \
    bash /w/scripts/build-deps.sh ps5 --in-docker
fi

fetch_source
if [[ "${TARGET}" == "ps5" ]]; then
  # build directory may contain a cache from a host path; always reconfigure cleanly
  [[ -f "${BUILD}/CMakeCache.txt" ]] && ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=${SRC}$" "${BUILD}/CMakeCache.txt" && rm -rf "${BUILD}"
  build_ps5_in_container
else
  [[ -f "${BUILD}/CMakeCache.txt" ]] && ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=${SRC}$" "${BUILD}/CMakeCache.txt" && rm -rf "${BUILD}"
  build_host
fi

echo ">> libdatachannel ${LDC_VERSION} installed to ${DEPS_PREFIX}"
ls -1 "${DEPS_PREFIX}/lib"/*.a
