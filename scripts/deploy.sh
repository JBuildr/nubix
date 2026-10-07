#!/usr/bin/env bash
# Nubix — deploy to a jailbroken PS5.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# Usage:
#   PS5_HOST=192.168.1.50 scripts/deploy.sh run      send build-ps5/nubix.elf to elfldr (port 9021)
#   PS5_HOST=192.168.1.50 scripts/deploy.sh install  upload dist/nubix/ to /data/homebrew/nubix/
#                                                    over FTP (ftpsrv payload, port $PS5_FTP_PORT, default 2121)
#   PS5_HOST=... scripts/deploy.sh log               fetch /data/nubix/log.txt over FTP
#
# "run" starts the payload directly (stdout appears on elfldr's log / klog). The websrv
# Homebrew Launcher (install) is the supported way to start it with video/pad/audio access.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CMD="${1:-run}"
HOST="${PS5_HOST:-}"
ELF_PORT="${PS5_ELF_PORT:-9021}"
FTP_PORT="${PS5_FTP_PORT:-2121}"

if [[ -z "${HOST}" ]]; then
  echo "error: set PS5_HOST to the console's IP address" >&2
  exit 2
fi

send_elf() {
  local elf="$1"
  [[ -f "${elf}" ]] || { echo "error: ${elf} not found (run scripts/build-ps5.sh)" >&2; exit 1; }
  echo ">> sending $(basename "${elf}") ($(wc -c <"${elf}" | tr -d ' ') bytes) to ${HOST}:${ELF_PORT}"
  if command -v nc >/dev/null 2>&1; then
    nc -w 3 "${HOST}" "${ELF_PORT}" <"${elf}"
  else
    exec 3<>"/dev/tcp/${HOST}/${ELF_PORT}"
    cat "${elf}" >&3
    exec 3>&-
  fi
  echo ">> sent"
}

ftp_upload_dir() {
  local src="$1" dst="$2"
  command -v curl >/dev/null 2>&1 || { echo "error: curl required for FTP upload" >&2; exit 1; }
  (cd "${src}" && find . -type f | sed 's|^\./||') | while read -r f; do
    echo "   ${dst}/${f}"
    curl -sS --ftp-create-dirs -T "${src}/${f}" "ftp://${HOST}:${FTP_PORT}${dst}/${f}"
  done
}

case "${CMD}" in
  run)
    send_elf "${ROOT}/build-ps5/nubix.elf"
    ;;
  install)
    [[ -d "${ROOT}/dist/nubix" ]] || { echo "error: dist/nubix missing (run scripts/build-ps5.sh)" >&2; exit 1; }
    echo ">> uploading dist/nubix to ftp://${HOST}:${FTP_PORT}/data/homebrew/nubix"
    ftp_upload_dir "${ROOT}/dist/nubix" "/data/homebrew/nubix"
    echo ">> done; start it from the websrv Homebrew Launcher"
    ;;
  log)
    curl -sS "ftp://${HOST}:${FTP_PORT}/data/nubix/log.txt"
    ;;
  *)
    echo "usage: $0 <run|install|log>" >&2
    exit 2
    ;;
esac
