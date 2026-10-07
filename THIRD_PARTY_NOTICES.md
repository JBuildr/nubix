# Third-party notices

Nubix is free software, licensed under the **GNU General Public License, version 3 only
(GPL-3.0-only)**; see `LICENSE`. It is an unofficial client and is not affiliated with or
endorsed by Microsoft or Sony.

The PS5 binary (`eboot.elf` / `nubix.elf`) **statically links** every component listed under
"Linked into the PS5 binary" below. All of them are under licenses compatible with distribution of
the combined work under GPL-3.0. The complete, unmodified license text of each component, taken
from the exact upstream version used, is in `THIRD_PARTY_LICENSES.txt` (shipped next to the binary in
every release; the `[name]` after each component below is its section in that file).

Because FFmpeg is built with `--enable-gpl --enable-version3` and the ps5-payload-sdk C runtime
is GPL-3.0-or-later, the PS5 binary as a whole can only be distributed under GPL-3.0, which is
Nubix's own license.

## Linked into the PS5 binary

All prebuilt components except libdatachannel and its submodules come from the
[ps5-payload-dev pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) bundle release
**v0.40.2** (commit `1687e827675c66e1b040751c7f0811222bd3aea0`), which `docker/Dockerfile`
installs into the build image.

### Multimedia

| Component | Version | License | Copyright | License file |
|---|---|---|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec 61.3.100, libavutil, libswresample) | 7.0.1 | **GPL-3.0-or-later as built** (configured `--enable-gpl --enable-version3 --enable-openssl --enable-libx264`; FFmpeg is otherwise LGPL-2.1-or-later) | Copyright (c) 2000-2024 the FFmpeg developers | `THIRD_PARTY_LICENSES.txt` [ffmpeg], `LICENSE` |
| [x264](https://www.videolan.org/developers/x264.html) | 0.165.3222 (commit `b35605ace3ddf7c1a5d67a2eb553f034aef41d55`) | GPL-2.0-or-later | Copyright (C) 2003-2025 x264 project | `THIRD_PARTY_LICENSES.txt` [x264] |
| [Opus](https://opus-codec.org) | 1.5.2 | BSD-3-Clause | Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla, Amazon | `THIRD_PARTY_LICENSES.txt` [opus] |
| [libsamplerate](https://github.com/libsndfile/libsamplerate) | 0.2.2 | BSD-2-Clause | Copyright (c) 2012-2016, Erik de Castro Lopo | `THIRD_PARTY_LICENSES.txt` [libsamplerate] |

### WebRTC

Built from source by `scripts/build-deps.sh` (libdatachannel tag `v0.24.6` with its pinned
submodules).

| Component | Version | License | Copyright | License file |
|---|---|---|---|---|
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) | v0.24.6, **modified** by `patches/libdatachannel-xbox-pli.patch` (the patch is MPL-2.0) | MPL-2.0 | Copyright (c) 2019-2025 Paul-Louis Ageneau; also Filip Klembara, Staz Modrzynski, Arda Cinar, Eric Gressman, Zita Liao, Robert Edmonds, Sean DuBois, Shigemasa Watanabe, Vladimir Voronin, Alex Potsides | `THIRD_PARTY_LICENSES.txt` [libdatachannel] |
| [libjuice](https://github.com/paullouisageneau/libjuice) | v1.7.4 (`b89c792`) | MPL-2.0 | Copyright (c) 2020-2022 Paul-Louis Ageneau | `THIRD_PARTY_LICENSES.txt` [libjuice] |
| [libsrtp](https://github.com/cisco/libsrtp) | 2.8.0 (commit `24b3bf8`) | BSD-3-Clause | Copyright (c) 2001-2017 Cisco Systems, Inc. | `THIRD_PARTY_LICENSES.txt` [libsrtp] |
| [usrsctp](https://github.com/sctplab/usrsctp) | commit `fec583d` | BSD-3-Clause | Copyright (c) 2015, Randall Stewart and Michael Tuexen; Copyright (c) 2001-2008 Cisco Systems, Inc.; portions Copyright The Regents of the University of California | `THIRD_PARTY_LICENSES.txt` [usrsctp] |
| [plog](https://github.com/SergiusTheBest/plog) (via libdatachannel) | commit `94899e0` | MIT | Copyright (c) 2022 Sergey Podobry | `THIRD_PARTY_LICENSES.txt` [plog] |
| [nlohmann/json](https://github.com/nlohmann/json) (header-only; Nubix compiles the vendored copy in `third_party/nlohmann/`) | 3.11.3 | MIT | Copyright (c) 2013-2022 Niels Lohmann | `THIRD_PARTY_LICENSES.txt` [nlohmann-json] |

### Networking and TLS

| Component | Version | License | Copyright | License file |
|---|---|---|---|---|
| [OpenSSL](https://www.openssl.org) (libssl, libcrypto) | 3.5.2 | Apache-2.0 (OpenSSL 3.5.2 ships no NOTICE file) | Copyright 1995-2025 The OpenSSL Project Authors | `THIRD_PARTY_LICENSES.txt` [openssl] |
| [libcurl](https://curl.se) | 8.18.0 | curl | Copyright (c) 1996 - 2025, Daniel Stenberg, <daniel@haxx.se>, and many contributors | `THIRD_PARTY_LICENSES.txt` [curl] |
| [libpsl](https://github.com/rockdaboot/libpsl) | 0.21.5 | MIT; `psl-make-dafsa` / `lookup_string_in_fixed_set.c` BSD-3-Clause (Chromium) | Copyright (C) 2014-2024 Tim Rühsen; Copyright 2015 The Chromium Authors | `THIRD_PARTY_LICENSES.txt` [libpsl], `THIRD_PARTY_LICENSES.txt` [libpsl-chromium] |
| [Public Suffix List](https://publicsuffix.org) data (compiled into libpsl) | snapshot bundled with libpsl 0.21.5 | MPL-2.0 | Mozilla Foundation and Public Suffix List contributors | `THIRD_PARTY_LICENSES.txt` [MPL-2.0] |

### Graphics, text and input

| Component | Version | License | Copyright | License file |
|---|---|---|---|---|
| [SDL2](https://www.libsdl.org) (PS5 port [ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL), branch `release-2.30.x-ps5`) | 2.30.12 | Zlib | Copyright (C) 1997-2025 Sam Lantinga | `THIRD_PARTY_LICENSES.txt` [SDL2] |
| [SDL2_ttf](https://github.com/libsdl-org/SDL_ttf) | 2.22.0 | Zlib | Copyright (C) 1997-2024 Sam Lantinga | `THIRD_PARTY_LICENSES.txt` [SDL2_ttf] |
| [FreeType](https://freetype.org) | 2.13.2 | FreeType License (FTL), chosen from FTL / GPL-2.0 | Copyright 1996-2002, 2006 by David Turner, Robert Wilhelm, and Werner Lemberg. Portions of this software are copyright © 2023 The FreeType Project (www.freetype.org). All rights reserved. | `THIRD_PARTY_LICENSES.txt` [freetype-FTL], `THIRD_PARTY_LICENSES.txt` [freetype] |
| [libpng](http://www.libpng.org) | 1.6.43 | libpng-2.0 | Copyright (c) 1995-2024 The PNG Reference Library Authors; Copyright (c) 2018-2024 Cosmin Truta; Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson; Copyright (c) 1996-1997 Andreas Dilger; Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc. | `THIRD_PARTY_LICENSES.txt` [libpng] |
| [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) (`src/ui/qrcodegen.*`) | vendored | MIT | Copyright (c) Project Nayuki | `THIRD_PARTY_LICENSES.txt` [qrcodegen] |

### Compression and character conversion

| Component | Version | License | Copyright | License file |
|---|---|---|---|---|
| [zlib](https://zlib.net) | 1.3.2 | Zlib | (C) 1995-2026 Jean-loup Gailly and Mark Adler | `THIRD_PARTY_LICENSES.txt` [zlib] |
| [zstd](https://github.com/facebook/zstd) | 1.5.6 | BSD-3-Clause (chosen from BSD-3-Clause / GPL-2.0) | Copyright (c) Meta Platforms, Inc. and affiliates | `THIRD_PARTY_LICENSES.txt` [zstd] |
| [xz / liblzma](https://tukaani.org/xz/) | 5.4.6 | public domain (liblzma) | Lasse Collin, Igor Pavlov and others; placed in the public domain | `THIRD_PARTY_LICENSES.txt` [xz] |
| [bzip2 / libbzip2](https://sourceware.org/bzip2/) | 1.0.8 | bzip2-1.0.6 | Copyright (C) 1996-2019 Julian R Seward | `THIRD_PARTY_LICENSES.txt` [bzip2] |
| [GNU libiconv + libcharset](https://www.gnu.org/software/libiconv/) | 1.17 | LGPL-2.1-or-later | Copyright (C) 1999-2022 Free Software Foundation, Inc. | `THIRD_PARTY_LICENSES.txt` [libiconv], `THIRD_PARTY_LICENSES.txt` [libiconv-NOTE] |

### C / C++ runtime

| Component | Version | License | Copyright | License file |
|---|---|---|---|---|
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) `crt1.o` and `libc.a` (pacbrew v0.40.2; SDK sources at tag v0.42, `4eb7012`) | — | **GPL-3.0-or-later** (no linking exception); individual libc files carry FreeBSD / University of California BSD, OpenBSD / NetBSD ISC-style, musl MIT, public-domain and LLVM (Apache-2.0 WITH LLVM-exception, `emutls.c`) notices | Copyright (C) 2023-2026 John Törnblom (some crt files also idlesauce and sleirsgoevy); Copyright © 2005-2020 Rich Felker, et al.; Copyright The Regents of the University of California; others as listed | `THIRD_PARTY_LICENSES.txt` [ps5-payload-sdk], `THIRD_PARTY_LICENSES.txt` [ps5-payload-sdk-libc-notices], `THIRD_PARTY_LICENSES.txt` [musl] |
| [openlibm](https://github.com/JuliaMath/openlibm) | 0.8.6 | MIT / BSD-2-Clause / ISC / Sun FDLIBM permissive notice | Copyright (c) 2011-14 The Julia Project; Copyright 1992-2011 The FreeBSD Project; Copyright (C) 1993 by Sun Microsystems, Inc.; Copyright (c) 2008 Stephen L. Moshier; others in the file | `THIRD_PARTY_LICENSES.txt` [openlibm] |
| [LLVM](https://llvm.org) libc++, libc++abi, libunwind | 18.1.8 | Apache-2.0 WITH LLVM-exception | Copyright the LLVM Project contributors | `THIRD_PARTY_LICENSES.txt` [llvm-libcxx], `THIRD_PARTY_LICENSES.txt` [llvm-libcxxabi], `THIRD_PARTY_LICENSES.txt` [llvm-libunwind] |

PS5 system libraries (`libkernel`, `libSceLibcInternal`, `libScePad`, `libSceAudioOut`, ...) are
resolved dynamically at run time from the console's firmware; they are not included in the binary
and fall under the GPL-3.0 "System Libraries" exclusion.

The host (desktop) build used for development links the operating system's copies of the same
libraries dynamically and is not distributed.

## Shipped as data

| Component | License | Notes |
|---|---|---|
| [DejaVu fonts](https://dejavu-fonts.github.io) | Bitstream Vera / Arev fonts license, DejaVu changes public domain | `assets/fonts/`; license in `assets/fonts/LICENSE-DejaVu.txt` (copy in `assets/fonts/LICENSE-DejaVu.txt`). Shipped as separate files (mere aggregation), not compiled into the binary. |
| Mozilla CA certificate bundle `assets/cacert.pem` ([curl.se/ca](https://curl.se/docs/caextract.html)) | MPL-2.0 | Extracted from Mozilla's NSS certdata by the curl project; unmodified. Its source form is the file itself; license in `THIRD_PARTY_LICENSES.txt` [MPL-2.0]. |

## Code ported or adapted from other projects

| Project | License | Authors | What |
|---|---|---|---|
| [green-nx](https://github.com/rmrf404/green-nx) | GPL-3.0 | rmrf404, luishidalgoa, praneetreddy017, Dispnser | auth/session/catalog flow, SDP template, stream engine behaviour |
| [GreenOvercast](https://github.com/Producdevity/GreenOvercast) | MPL-2.0 | Producdevity | libdatachannel usage and the keyframe-request (PLI) fix in `patches/libdatachannel-xbox-pli.patch` |
| [ProsperoLight](https://github.com/xEasy4Breezy/ProsperoLight) | GPL-3.0 | xEasy4Breezy / blackbearreloaded | PS5 platform notes |

**GreenOvercast / MPL-2.0 statement.** Parts of Nubix are derived from GreenOvercast, which is
"Covered Software" under the Mozilla Public License 2.0. As permitted by MPL-2.0 section 3.3, those
parts are distributed as part of a Larger Work under the GNU General Public License v3.0. They
also remain available under the terms of the MPL-2.0 (`THIRD_PARTY_LICENSES.txt` [MPL-2.0]); their source form is
included in the Nubix Corresponding Source. `patches/libdatachannel-xbox-pli.patch` is a
Modification of libdatachannel and is itself licensed under MPL-2.0 only.

Protocol details were also cross-checked against (no code copied) [Greenlight](https://github.com/unknownskl/greenlight),
[xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player), [xal-node](https://github.com/unknownskl/xal-node),
[XStreaming](https://github.com/Geocld/XStreaming) (all MIT) and [green-vita](https://github.com/Day-OS/green-vita) (MPL-2.0).

## Source code

For every binary release, the complete **Corresponding Source** (GPL-3.0 section 6) is published
as a release asset right next to the binary, named
`nubix-v<version>-corresponding-source.tar.xz` (see `SOURCE.txt` for its exact layout). It contains the Nubix source at the release tag,
`patches/`, the build scripts and Dockerfile, and the exact source of every third-party component
listed above (including FFmpeg, x264, the ps5-payload-sdk and the libdatachannel tree with its
submodules) at the versions given here.

To rebuild the PS5 binary from that source:

1. Build the toolchain image: `docker build -t ps5dev docker/` — Ubuntu 24.04, clang-18 and the
   ps5-payload-dev pacbrew bundle **v0.40.2** (`docker/Dockerfile`).
2. `scripts/build-deps.sh ps5` — fetches libdatachannel v0.24.6 with its submodules, applies
   `patches/libdatachannel-*.patch`, and builds it into `deps/ps5/`.
3. `scripts/build-ps5.sh` — builds `build-ps5/nubix.elf` inside the `ps5dev` container, assembles
   the `dist/nubix/` homebrew folder (binary, assets, `LICENSE`, this file, `THIRD_PARTY_LICENSES.txt`) and
   packages it as `build-ps5/nubix-v<version>-websrv.zip`.

To replace a library (for example libiconv under LGPL-2.1 section 6), rebuild that library from
its source, install it into the toolchain sysroot and rerun steps 2 and 3.
