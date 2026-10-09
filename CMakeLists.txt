# Nubix — native Xbox Cloud Gaming client for PS5 (homebrew payload) + host build.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# Host:  scripts/build-host.sh   (or: cmake -S . -B build-host && cmake --build build-host)
# PS5:   scripts/build-ps5.sh    (inside docker "ps5dev" with the prospero toolchain)
cmake_minimum_required(VERSION 3.20)
project(nubix VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(XcPlatform)

option(XC_BUILD_TESTS "Build unit tests (host only)" ON)
option(XC_BUILD_CLI "Build the xc-cli tool (host only)" ON)
option(XC_BUILD_LINKCHECK "Build xc-linkcheck (links every third-party dependency)" OFF)
option(XC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(XcDeps)

# ---- version ---------------------------------------------------------------
set(XC_VERSION "${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _git_desc
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _git_rc)
  if(_git_rc EQUAL 0 AND _git_desc)
    set(XC_VERSION "${PROJECT_VERSION}-${_git_desc}")
  endif()
endif()

# ---- common compile settings ----------------------------------------------
add_library(xc_options INTERFACE)
target_include_directories(xc_options INTERFACE "${CMAKE_SOURCE_DIR}/src")
target_include_directories(xc_options SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party")
target_compile_definitions(xc_options INTERFACE XC_VERSION="${XC_VERSION}")
target_compile_options(xc_options INTERFACE -Wall -Wextra -Wno-unused-parameter)
if(XC_WARNINGS_AS_ERRORS)
  target_compile_options(xc_options INTERFACE -Werror)
endif()
if(XC_PS5)
  target_compile_definitions(xc_options INTERFACE XC_PS5=1)
else()
  target_compile_definitions(xc_options INTERFACE XC_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()

# ---- xc_core: auth, http, config, catalog, gssv, protocol, log -------------
file(GLOB XC_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/core/*.cpp")
add_library(xc_core STATIC ${XC_CORE_SOURCES})
target_link_libraries(xc_core PUBLIC xc_options xc::curl xc::openssl Threads::Threads)

# ---- xc_platform: paths, notify, gamepad, PS5 shims ------------------------
set(XC_PLATFORM_SOURCES
  src/platform/common.cpp
  src/platform/gamepad.cpp
  src/platform/net_compat.c)
if(XC_PS5)
  # audio_in_ps5.cpp resolves libSceAudioIn at run time (dlopen), so no extra link library.
  list(APPEND XC_PLATFORM_SOURCES src/platform/ps5.cpp src/platform/ps5_shims.c src/platform/audio_in_ps5.cpp)
else()
  list(APPEND XC_PLATFORM_SOURCES src/platform/host.cpp src/platform/audio_in_host.cpp)
endif()
add_library(xc_platform STATIC ${XC_PLATFORM_SOURCES})
target_link_libraries(xc_platform PUBLIC xc_core xc::sdl2)

# ---- xc_stream: webrtc, rtp, video, audio, streamer -------------------------
file(GLOB XC_STREAM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/stream/*.cpp")
add_library(xc_stream STATIC ${XC_STREAM_SOURCES})
target_link_libraries(xc_stream PUBLIC xc_core xc_platform xc::datachannel xc::avcodec xc::avutil xc::opus xc::sdl2)

# ---- xc_ui: drawing, screens, app, QR ---------------------------------------
file(GLOB XC_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/ui/*.cpp")
add_library(xc_ui STATIC ${XC_UI_SOURCES})
target_link_libraries(xc_ui PUBLIC xc_stream xc_platform xc::sdl2_ttf xc::sdl2)
# vendored Nayuki qrcodegen: keep its warnings out of our output
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/ui/qrcodegen.cpp" PROPERTIES COMPILE_OPTIONS "-w")

# ---- app ----------------------------------------------------------------------
add_executable(nubix src/main.cpp)
target_link_libraries(nubix PRIVATE xc_ui)

if(XC_PS5)
  # Unstripped ELF keeps symbols for crash analysis; the stripped one is what ships.
  set_target_properties(nubix PROPERTIES OUTPUT_NAME "nubix-debug" SUFFIX ".elf")
  # SDL2 PS5 port + payload system libraries (most already come from sdl2.pc --static)
  target_link_libraries(nubix PRIVATE
    SceSystemService SceUserService ScePad SceAudioOut SceVideoOut SceKeyboard SceImeDialog SceNet)
  add_custom_command(TARGET nubix POST_BUILD
    COMMAND "${CMAKE_STRIP}" -o "${CMAKE_BINARY_DIR}/nubix.elf" "$<TARGET_FILE:nubix>"
    COMMENT "Stripping -> nubix.elf")
else()
  # host: make assets reachable next to the binary
  add_custom_command(TARGET nubix POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E create_symlink "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:nubix>/assets"
    COMMENT "Linking assets/ next to nubix")
endif()

# ---- dependency link check (any target) ---------------------------------------------
if(XC_BUILD_LINKCHECK)
  add_executable(xc-linkcheck tools/linkcheck.cpp)
  target_link_libraries(xc-linkcheck PRIVATE xc_platform xc::datachannel xc::avcodec xc::avutil xc::opus
                                             xc::curl xc::openssl xc::sdl2_ttf xc::sdl2)
  if(XC_PS5)
    set_target_properties(xc-linkcheck PROPERTIES SUFFIX ".elf")
    target_link_libraries(xc-linkcheck PRIVATE
      SceSystemService SceUserService ScePad SceAudioOut SceVideoOut SceKeyboard SceImeDialog SceNet)
  endif()
endif()

# ---- host tools / tests ------------------------------------------------------------
if(NOT XC_PS5 AND XC_BUILD_CLI)
  add_executable(xc-cli tools/xc-cli.cpp tools/cli_gssv.cpp)
  target_link_libraries(xc-cli PRIVATE xc_stream xc_platform)
endif()

if(NOT XC_PS5 AND XC_BUILD_TESTS)
  enable_testing()
  file(GLOB XC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*.cpp")
  add_executable(xc-tests ${XC_TEST_SOURCES})
  target_link_libraries(xc-tests PRIVATE xc_ui)
  foreach(suite smoke protocol sdp rtp config ratelimit net voice voice_sdp)
    add_test(NAME ${suite} COMMAND xc-tests ${suite})
  endforeach()
endif()
