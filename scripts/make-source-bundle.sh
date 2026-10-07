#!/usr/bin/env bash
# Nubix — assemble the GPLv3 section 6 "Corresponding Source" archive for a binary release.
#
# Usage:
#   scripts/make-source-bundle.sh [--nubix-ref <git-ref> | --nubix-dir <dir>] [--offline]
#
# Output (all under dist/):
#   nubix-v<ver>-corresponding-source.tar.xz          the archive to upload next to the binary
#   nubix-v<ver>-corresponding-source.MANIFEST        copy of the MANIFEST inside the archive
#   nubix-v<ver>-corresponding-source.tar.xz.sha256   checksum of the archive
#
# <ver> is taken from `project(nubix VERSION x.y.z ...)` in CMakeLists.txt.
#
# Nubix's own source is taken from (in this order of precedence):
#   --nubix-dir <dir>  every file below <dir> (e.g. a clean export of the release tree),
#                      except .git/, build*/, deps/, dist/, .cache/ and docs/
#   --nubix-ref <ref>  `git archive <ref>` (only committed content; use for tagged releases)
#   default            the CURRENT WORKING TREE: `git ls-files -co --exclude-standard`, i.e.
#                      tracked files (incl. uncommitted modifications) plus untracked files that
#                      are not gitignored. docs/ and files deleted from the working tree are left
#                      out; build trees, deps/, dist/ and .cache/ are gitignored. Make sure the
#                      tree is exactly what the binary was built from.
#
# Third-party sources are downloaded once into .cache/sources/ (tarballs are verified against
# pinned sha256 sums; git commits are fetched by full hash and exported with `git archive`),
# then copied unmodified into the archive under upstream/. Every included file is listed in
# MANIFEST and SHA256SUMS. Re-running the script reuses the cache and rebuilds the archive.
#
# --offline   never touch the network; fail if something is missing from the cache.
#
# Environment:
#   LDC_LOCAL_SRC  existing libdatachannel checkout whose git objects may be used instead of
#                  fetching (default: .cache/libdatachannel-v0.24.6; it is only read, never
#                  modified — exports come from the pinned commits, not from its working tree).
#   XZ_OPT         read by xz itself (e.g. XZ_OPT=-9e for a slightly smaller outer archive).
#
# Requirements: bash, git, curl, tar (GNU tar or bsdtar), xz, sha256sum or shasum.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

NUBIX_REF=""
NUBIX_DIR=""
OFFLINE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --nubix-ref) NUBIX_REF="${2:?--nubix-ref needs a value}"; shift 2 ;;
    --nubix-dir) NUBIX_DIR="${2:?--nubix-dir needs a value}"; shift 2 ;;
    --offline) OFFLINE=1; shift ;;
    -h|--help) sed -n '2,38p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -n "${NUBIX_REF}" && -n "${NUBIX_DIR}" ]]; then
  echo "--nubix-ref and --nubix-dir are mutually exclusive" >&2; exit 2
fi

VERSION="$(sed -nE 's/^[[:space:]]*project\(nubix[[:space:]]+VERSION[[:space:]]+([0-9][0-9.]*).*/\1/p' "${ROOT}/CMakeLists.txt" | head -n1)"
[[ -n "${VERSION}" ]] || { echo "cannot read project(nubix VERSION ...) from CMakeLists.txt" >&2; exit 1; }

NAME="nubix-v${VERSION}-corresponding-source"
CACHE="${ROOT}/.cache/sources"
GITCACHE="${CACHE}/git"
WORK="${ROOT}/.cache/source-bundle"
STAGE="${WORK}/${NAME}"
DIST="${ROOT}/dist"
OUT="${DIST}/${NAME}.tar.xz"
LDC_LOCAL_SRC="${LDC_LOCAL_SRC:-${ROOT}/.cache/libdatachannel-v0.24.6}"

export COPYFILE_DISABLE=1   # macOS bsdtar: no AppleDouble ._* files
export LC_ALL=C

log() { printf '>> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

for t in git tar xz; do command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"; done
[[ ${OFFLINE} -eq 1 ]] || command -v curl >/dev/null 2>&1 || die "missing tool: curl"

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}
fsize() { wc -c <"$1" | tr -d ' '; }

if tar --version 2>/dev/null | grep -q 'GNU tar'; then
  TAR_NORM=(--owner=0 --group=0 --numeric-owner --format=pax --pax-option=delete=atime,delete=ctime)
else
  TAR_NORM=(--uid 0 --gid 0 --uname root --gname root --format=pax)
fi

# ------------------------------------------------------------- components ---
# Release tarballs: id|version|file|sha256|url [mirror-url ...]
# sha256: from the pacbrew-repo v0.40.2 PKGBUILD (which pacbrew verified when building the
# SDK libraries) unless noted. FFmpeg's PKGBUILD has sha256sums=SKIP and ffmpeg.org publishes
# only PGP signatures, so that sum was computed on download (2026-10-07) and pinned here.
TARBALLS='
ffmpeg|7.0.1|ffmpeg-7.0.1.tar.xz|bce9eeb0f17ef8982390b1f37711a61b4290dc8c2a0c1a37b5857e85bfb0e4ff|https://ffmpeg.org/releases/ffmpeg-7.0.1.tar.xz
libiconv|1.17|libiconv-1.17.tar.gz|8f74213b56238c85a50a5329f77e06198771e70dd9a739779f4c02f65d971313|https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.17.tar.gz https://ftpmirror.gnu.org/libiconv/libiconv-1.17.tar.gz
openssl|3.5.2|openssl-3.5.2.tar.gz|c53a47e5e441c930c3928cf7bf6fb00e5d129b630e0aa873b08258656e7345ec|https://github.com/openssl/openssl/releases/download/openssl-3.5.2/openssl-3.5.2.tar.gz
curl|8.18.0|curl-8.18.0.tar.xz|40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946|https://curl.se/download/curl-8.18.0.tar.xz https://curl.haxx.se/download/curl-8.18.0.tar.xz
libpsl|0.21.5|libpsl-0.21.5.tar.gz|1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208|https://github.com/rockdaboot/libpsl/releases/download/0.21.5/libpsl-0.21.5.tar.gz
SDL2_ttf|2.22.0|SDL2_ttf-2.22.0.tar.gz|d48cbd1ce475b9e178206bf3b72d56b66d84d44f64ac05803328396234d67723|https://libsdl.org/projects/SDL_ttf/release/SDL2_ttf-2.22.0.tar.gz https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-2.22.0.tar.gz
freetype|2.13.2|freetype-2.13.2.tar.gz|1ac27e16c134a7f2ccea177faba19801131116fd682efc1f5737037c5db224b5|https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.gz https://downloads.sourceforge.net/project/freetype/freetype2/2.13.2/freetype-2.13.2.tar.gz
libpng|1.6.43|libpng-1.6.43.tar.xz|6a5ca0652392a2d7c9db2ae5b40210843c0bbc081cbd410825ab00cc59f14a6c|https://download.sourceforge.net/libpng/libpng-1.6.43.tar.xz https://downloads.sourceforge.net/project/libpng/libpng16/1.6.43/libpng-1.6.43.tar.xz
zlib|1.3.2|zlib-1.3.2.tar.gz|bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16|https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
zstd|1.5.6|zstd-1.5.6.tar.gz|8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1|https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
xz|5.4.6|xz-5.4.6.tar.xz|b92d4e3a438affcf13362a1305cd9d94ed47ddda22e456a42791e630a5644f5c|https://github.com/tukaani-project/xz/releases/download/v5.4.6/xz-5.4.6.tar.xz
bzip2|1.0.8|bzip2-1.0.8.tar.gz|ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269|https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
openlibm|0.8.6|openlibm-0.8.6.tar.gz|347998968cfeb2f9b91de6a8e85d2ba92dec0915d53500a4bc483e056f85b94c|https://github.com/JuliaMath/openlibm/archive/refs/tags/v0.8.6.tar.gz
opus|1.5.2|opus-1.5.2.tar.gz|65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1|https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
libsamplerate|0.2.2|libsamplerate-0.2.2.tar.xz|3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893|https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz
'

# Git commits: id|version-label|repo-url|full-commit|archive-prefix
# Exported with `git archive --prefix=<archive-prefix>/ <commit> | xz -T1 -6`.
# The libdatachannel submodule archives use prefixes inside libdatachannel-v0.24.6/ so that
# extracting all six into one directory reproduces the recursive checkout.
GITSRCS='
libdatachannel|v0.24.6|https://github.com/paullouisageneau/libdatachannel.git|6b1e2e620f1e37f0eafeee702eaea0043cb305fd|libdatachannel-v0.24.6
libjuice|v1.7.4|https://github.com/paullouisageneau/libjuice.git|b89c792e3612faf2f12cf35bcc56857313a06be3|libdatachannel-v0.24.6/deps/libjuice
libsrtp|24b3bf8|https://github.com/cisco/libsrtp.git|24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5|libdatachannel-v0.24.6/deps/libsrtp
usrsctp|fec583d|https://github.com/paullouisageneau/usrsctp.git|fec583d54493f879d2ae44a743423bf8a04371ab|libdatachannel-v0.24.6/deps/usrsctp
plog|94899e0|https://github.com/SergiusTheBest/plog.git|94899e0b926ac1b0f4750bfbd495167b4a6ae9ef|libdatachannel-v0.24.6/deps/plog
json|55f9368|https://github.com/nlohmann/json.git|55f93686c01528224f448c19128836e7df245f72|libdatachannel-v0.24.6/deps/json
x264|r3222-b35605a|https://code.videolan.org/videolan/x264.git|b35605ace3ddf7c1a5d67a2eb553f034aef41d55|x264-b35605a
SDL-ps5|2.30.12-a47182a20|https://github.com/ps5-payload-dev/SDL.git|a47182a20e741e4735d1fcd41e8656bf338b0c72|SDL-ps5-a47182a20
ps5-payload-sdk|4eb7012|https://github.com/ps5-payload-dev/sdk.git|4eb701204fc3f8d31e84cf8ca272974e2be9c867|ps5-payload-sdk-4eb7012
pacbrew-repo|v0.40.2|https://github.com/ps5-payload-dev/pacbrew-repo.git|1687e827675c66e1b040751c7f0811222bd3aea0|pacbrew-repo-v0.40.2
'

# ---------------------------------------------------------------- helpers ---
fetch_tarball() { # file sha256 urls...
  local file="$1" want="$2"; shift 2
  local dst="${CACHE}/${file}"
  if [[ -f "${dst}" ]]; then
    [[ "$(sha256 "${dst}")" == "${want}" ]] && return 0
    log "cached ${file} has a wrong checksum, re-downloading"
    rm -f "${dst}"
  fi
  [[ ${OFFLINE} -eq 0 ]] || die "${file} not in cache and --offline given"
  local url got
  for url in "$@"; do
    log "downloading ${url}"
    if curl -fsSL --retry 3 --connect-timeout 30 -o "${dst}.part" "${url}"; then
      got="$(sha256 "${dst}.part")"
      if [[ "${got}" == "${want}" ]]; then mv "${dst}.part" "${dst}"; return 0; fi
      log "checksum mismatch for ${url}: got ${got}, want ${want}"
    fi
    rm -f "${dst}.part"
  done
  die "could not download ${file} with sha256 ${want}"
}

have_commit() { git -C "$1" cat-file -e "$2^{commit}" 2>/dev/null; }

# Find a git dir that already has <commit>: our bare cache, or the read-only libdatachannel checkout.
local_git_for() { # id commit -> prints dir or nothing
  local id="$1" commit="$2" d
  local cands=("${GITCACHE}/${id}.git")
  if [[ -d "${LDC_LOCAL_SRC}" ]]; then
    case "${id}" in
      libdatachannel) cands+=("${LDC_LOCAL_SRC}") ;;
      libjuice|libsrtp|usrsctp|plog|json) cands+=("${LDC_LOCAL_SRC}/deps/${id}") ;;
    esac
  fi
  for d in "${cands[@]}"; do
    [[ -e "${d}" ]] && have_commit "${d}" "${commit}" && { echo "${d}"; return 0; }
  done
  return 0
}

fetch_commit() { # id repo commit -> prints git dir containing commit
  local id="$1" repo="$2" commit="$3" d
  d="$(local_git_for "${id}" "${commit}")"
  if [[ -n "${d}" ]]; then echo "${d}"; return 0; fi
  [[ ${OFFLINE} -eq 0 ]] || die "commit ${commit} of ${id} not in cache and --offline given"
  d="${GITCACHE}/${id}.git"
  [[ -d "${d}" ]] || git init -q --bare "${d}"
  log "fetching ${repo} ${commit}"
  if ! git -C "${d}" fetch -q --depth 1 "${repo}" "${commit}" >&2; then
    log "fetch by hash refused, fetching all branches and tags of ${repo}"
    git -C "${d}" fetch -q "${repo}" '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*' >&2
  fi
  have_commit "${d}" "${commit}" || die "commit ${commit} not found in ${repo}"
  echo "${d}"
}

export_commit() { # id label repo commit prefix -> prints archive file name (in CACHE)
  local id="$1" label="$2" repo="$3" commit="$4" prefix="$5"
  local file="${id}-${label}-${commit:0:12}.tar.xz" dst d
  dst="${CACHE}/${file}"
  if [[ ! -f "${dst}" ]]; then
    d="$(fetch_commit "${id}" "${repo}" "${commit}")"
    log "exporting ${id} ${commit} from ${d}"
    git -C "${d}" archive --format=tar --prefix="${prefix}/" "${commit}" | xz -T1 -6 -c >"${dst}.part"
    mv "${dst}.part" "${dst}"
  fi
  echo "${file}"
}

# ------------------------------------------------------------ nubix source ---
nubix_source() { # -> writes ${STAGE}/nubix-v${VERSION}-src.tar.xz, prints description
  local top="nubix-v${VERSION}" tmp="${WORK}/nubix-tree"
  local out="${STAGE}/${top}-src.tar.xz"
  rm -rf "${tmp}"; mkdir -p "${tmp}/${top}"
  if [[ -n "${NUBIX_REF}" ]]; then
    git -C "${ROOT}" archive --format=tar "${NUBIX_REF}" | tar -xf - -C "${tmp}/${top}"
    rm -rf "${tmp}/${top}/docs"
    echo "git archive $(git -C "${ROOT}" rev-parse "${NUBIX_REF}^{commit}") (docs/ excluded)"
  elif [[ -n "${NUBIX_DIR}" ]]; then
    local src; src="$(cd "${NUBIX_DIR}" && pwd)"
    (cd "${src}" && find . -type f \
        ! -path './.git/*' ! -path './.git' ! -path './build*' ! -path './deps/*' \
        ! -path './dist/*' ! -path './.cache/*' ! -path './docs/*' ! -name '.DS_Store' -print0 \
      | tar -cf - --null -T -) | tar -xf - -C "${tmp}/${top}"
    echo "directory ${src##*/}"
  else
    (cd "${ROOT}" && git ls-files -z -co --exclude-standard \
      | while IFS= read -r -d '' f; do
          case "$f" in docs/*|dist/*|deps/*|.cache/*|build*/*) continue ;; esac
          [[ -f "$f" || -L "$f" ]] && printf '%s\0' "$f"
        done \
      | tar -cf - --null -T -) | tar -xf - -C "${tmp}/${top}"
    local head dirty
    head="$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo none)"
    dirty="$(git -C "${ROOT}" status --porcelain 2>/dev/null | grep -vc '^?? docs/' || true)"
    echo "working tree of ${ROOT##*/} at HEAD ${head} (${dirty} uncommitted/untracked paths; docs/ excluded)"
  fi
  (cd "${tmp}" && find "${top}" -print0 | LC_ALL=C sort -z \
     | tar -cf - "${TAR_NORM[@]}" --no-recursion --null -T -) | xz -T1 -6 -c >"${out}"
  rm -rf "${tmp}"
}

# ------------------------------------------------------------------ main ----
mkdir -p "${CACHE}" "${GITCACHE}" "${DIST}"
rm -rf "${WORK}"; mkdir -p "${STAGE}/upstream"

MANIFEST_ROWS=()   # path|component|version|origin

log "Nubix ${VERSION}: packaging own source"
NUBIX_DESC="$(nubix_source)"
MANIFEST_ROWS+=("nubix-v${VERSION}-src.tar.xz|nubix|${VERSION}|${NUBIX_DESC}")

while IFS='|' read -r id ver file sum urls; do
  [[ -n "${id}" ]] || continue
  # shellcheck disable=SC2086
  fetch_tarball "${file}" "${sum}" ${urls}
  cp "${CACHE}/${file}" "${STAGE}/upstream/${file}"
  MANIFEST_ROWS+=("upstream/${file}|${id}|${ver}|${urls%% *}")
done <<<"${TARBALLS}"

while IFS='|' read -r id label repo commit prefix; do
  [[ -n "${id}" ]] || continue
  file="$(export_commit "${id}" "${label}" "${repo}" "${commit}" "${prefix}")"
  cp "${CACHE}/${file}" "${STAGE}/upstream/${file}"
  MANIFEST_ROWS+=("upstream/${file}|${id}|${label}|${repo} @ ${commit}")
done <<<"${GITSRCS}"

[[ -f "${ROOT}/SOURCE.txt" ]] && cp "${ROOT}/SOURCE.txt" "${STAGE}/SOURCE.txt"
[[ -f "${ROOT}/LICENSE" ]] && cp "${ROOT}/LICENSE" "${STAGE}/LICENSE"

cat >"${STAGE}/README.txt" <<EOF
Nubix ${VERSION} - Corresponding Source (GNU GPL v3, section 6)

nubix-v${VERSION}-src.tar.xz   Nubix source code (includes patches/ applied to libdatachannel,
                               docker/Dockerfile and scripts/ used to build the release)
upstream/                      unmodified upstream sources of every library linked into the
                               PS5 binary, plus the build recipes (pacbrew-repo PKGBUILDs, SDK)
SOURCE.txt                     component list, exact versions/commits and rebuild instructions
MANIFEST / SHA256SUMS          checksums of everything in this archive

libdatachannel: extract upstream/libdatachannel-* , libjuice-*, libsrtp-*, usrsctp-*, plog-*
and json-* into the same directory; they recreate libdatachannel-v0.24.6/ with its submodules.
Then apply nubix-v${VERSION}/patches/libdatachannel-*.patch (scripts/build-deps.sh does this).
EOF

# MANIFEST + SHA256SUMS
{
  echo "# Nubix ${VERSION} Corresponding Source - MANIFEST"
  echo "# generated $(date -u +%Y-%m-%dT%H:%M:%SZ) by scripts/make-source-bundle.sh"
  echo "# columns: sha256  size-bytes  path  component  version  origin"
  for row in "${MANIFEST_ROWS[@]}"; do
    IFS='|' read -r p comp ver origin <<<"${row}"
    printf '%s  %s  %s  %s  %s  %s\n' "$(sha256 "${STAGE}/${p}")" "$(fsize "${STAGE}/${p}")" "${p}" "${comp}" "${ver}" "${origin}"
  done
  for p in SOURCE.txt LICENSE README.txt; do
    [[ -f "${STAGE}/${p}" ]] || continue
    printf '%s  %s  %s  %s  %s  %s\n' "$(sha256 "${STAGE}/${p}")" "$(fsize "${STAGE}/${p}")" "${p}" "nubix" "${VERSION}" "documentation"
  done
} >"${STAGE}/MANIFEST"
(cd "${STAGE}" && find . -type f ! -name SHA256SUMS ! -name MANIFEST | sed 's|^\./||' | LC_ALL=C sort \
  | while IFS= read -r p; do printf '%s  %s\n' "$(sha256 "$p")" "$p"; done) >"${STAGE}/SHA256SUMS"

log "writing ${OUT}"
rm -f "${OUT}.part"
(cd "${WORK}" && find "${NAME}" -print0 | LC_ALL=C sort -z \
   | tar -cf - "${TAR_NORM[@]}" --no-recursion --null -T -) | xz -6 -T0 -c >"${OUT}.part"
mv "${OUT}.part" "${OUT}"
cp "${STAGE}/MANIFEST" "${DIST}/${NAME}.MANIFEST"
(cd "${DIST}" && printf '%s  %s\n' "$(sha256 "${NAME}.tar.xz")" "${NAME}.tar.xz" >"${NAME}.tar.xz.sha256")
rm -rf "${WORK}"

log "done: ${OUT} ($(fsize "${OUT}") bytes)"
cat "${DIST}/${NAME}.MANIFEST"
