# Nubix — target detection.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
# Sets XC_PS5 (BOOL) and XC_TARGET ("ps5" | "host").
#
# The pacbrew prospero toolchain file (prospero.cmake) sets PS5/PROSPERO = TRUE and
# CMAKE_SYSTEM_NAME = FreeBSD; the prospero-clang driver defines __PROSPERO__.

include(CheckCXXSourceCompiles)

set(_xc_ps5_default OFF)
if(PS5 OR PROSPERO)
  set(_xc_ps5_default ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" AND CMAKE_CROSSCOMPILING)
  check_cxx_source_compiles("
    #ifndef __PROSPERO__
    #error not prospero
    #endif
    int main() { return 0; }" XC_HAVE_PROSPERO_MACRO)
  if(XC_HAVE_PROSPERO_MACRO)
    set(_xc_ps5_default ON)
  endif()
endif()

option(XC_PS5 "Build the PS5 payload (auto-detected from the prospero toolchain)" ${_xc_ps5_default})

if(XC_PS5)
  set(XC_TARGET "ps5")
else()
  set(XC_TARGET "host")
endif()
message(STATUS "nubix target: ${XC_TARGET}")
