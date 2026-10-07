# Nubix — third-party dependencies.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# pkg-config: sdl2, SDL2_ttf, libavcodec, libavutil, opus, libcurl, openssl
#   (PS5: prospero-pkg-config, which always passes --static and points into the SDK sysroot)
# libdatachannel (static, + juice, srtp2, usrsctp) from ${XC_DEPS_DIR} (scripts/build-deps.sh)
#
# Provides imported targets:
#   xc::sdl2 xc::sdl2_ttf xc::avcodec xc::avutil xc::opus xc::curl xc::openssl xc::datachannel

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# PS5: the toolchain file sets PKG_CONFIG_EXECUTABLE to prospero-pkg-config, which always
# passes --static and resolves inside the SDK sysroot.

macro(xc_pkg name module)
  pkg_check_modules(XC_PC_${name} REQUIRED IMPORTED_TARGET GLOBAL ${module})
  add_library(xc::${name} ALIAS PkgConfig::XC_PC_${name})
  message(STATUS "  ${module} ${XC_PC_${name}_VERSION}")
endmacro()

xc_pkg(sdl2 sdl2)
xc_pkg(sdl2_ttf SDL2_ttf)
xc_pkg(avcodec libavcodec)
xc_pkg(avutil libavutil)
xc_pkg(opus opus)
xc_pkg(curl libcurl)
xc_pkg(openssl openssl)

# ---- libdatachannel -------------------------------------------------------
set(XC_DEPS_DIR "${CMAKE_SOURCE_DIR}/deps/${XC_TARGET}" CACHE PATH "Prefix with libdatachannel (scripts/build-deps.sh)")

if(NOT EXISTS "${XC_DEPS_DIR}/include/rtc/rtc.hpp")
  message(FATAL_ERROR "libdatachannel not found in ${XC_DEPS_DIR}. Run: scripts/build-deps.sh ${XC_TARGET}")
endif()

set(_xc_ldc_libs datachannel juice srtp2 usrsctp)
set(_xc_ldc_targets "")
foreach(lib IN LISTS _xc_ldc_libs)
  set(_path "${XC_DEPS_DIR}/lib/lib${lib}.a")
  if(NOT EXISTS "${_path}")
    message(FATAL_ERROR "missing ${_path}. Run: scripts/build-deps.sh ${XC_TARGET}")
  endif()
  add_library(xc_ldc_${lib} STATIC IMPORTED GLOBAL)
  set_target_properties(xc_ldc_${lib} PROPERTIES IMPORTED_LOCATION "${_path}")
  list(APPEND _xc_ldc_targets xc_ldc_${lib})
endforeach()

add_library(xc_datachannel INTERFACE)
target_include_directories(xc_datachannel SYSTEM INTERFACE "${XC_DEPS_DIR}/include")
# static archives listed in dependency order, followed by OpenSSL and threads
target_link_libraries(xc_datachannel INTERFACE ${_xc_ldc_targets} xc::openssl Threads::Threads)
add_library(xc::datachannel ALIAS xc_datachannel)
message(STATUS "  libdatachannel from ${XC_DEPS_DIR}")
