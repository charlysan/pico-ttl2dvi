cmake_minimum_required(VERSION 3.13)

# --- board / platform ------------------------------------------------------
# Platform is fixed (both boards are RP2350). The board profile (PICO_BOARD +
# the DVI pin/base config) is selected by cmake/boards.cmake via -DTTL_BOARD=...
# (default: pizero). Both run before pico_sdk_init, which PICO_BOARD requires.
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(boards)   # sets PICO_BOARD + TTL_BOARD_<name> define from TTL_BOARD

include(pico_sdk_import.cmake)

project(ttl2dvi C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- third-party (vendored) ------------------------------------------------
# libdvi from mlorenzati PicoDVI RP2350 fork. Added once here so every app
# can link the `libdvi` target; apps must NOT re-add it.
add_subdirectory(extern/dvi/libdvi)

# src/ builds the shared module library (ttl2dvi_core); apps/ are thin
# executables that link it. src must come before any app that links it.
add_subdirectory(src)
add_subdirectory(apps/dvi_test)     # standalone DVI test card (links libdvi only)
add_subdirectory(apps/ttl2dvi)      # full pipeline; for now: USB console only
