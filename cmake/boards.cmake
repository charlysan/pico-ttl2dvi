# Board profile selection.
#
# A "board" is just three things: the SDK board (PICO_BOARD), the DVI TMDS pin
# config, and the DVI PIO GPIO base. This file handles the CMake-time half --
# PICO_BOARD (which MUST be set before pico_sdk_init) and a profile define that
# src/config/board.h keys off for the code-time half (pin config + GPIO base).
#
# Select at configure time:   cmake -DTTL_BOARD=pico2_dvi ..
# Default is the Waveshare RP2350-PiZero (the current hardware).

set(TTL_BOARD pizero CACHE STRING "Target hardware profile (pizero | pico2_dvi)")
set_property(CACHE TTL_BOARD PROPERTY STRINGS pizero pico2_dvi)

if(TTL_BOARD STREQUAL "pizero")
    # Waveshare RP2350-PiZero, RP2350B (48 GPIO). DVI TMDS on GPIO 32-39 (base 16).
    set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Target board" FORCE)
elseif(TTL_BOARD STREQUAL "pico2_dvi")
    # RP2350A pico2 with a DVI board on GPIO 12-19 (base 0). Pin numbers are
    # filled in by src/config/board.h -- adjust there to match the wiring.
    set(PICO_BOARD pico2 CACHE STRING "Target board" FORCE)
else()
    message(FATAL_ERROR "Unknown TTL_BOARD='${TTL_BOARD}' (expected: pizero | pico2_dvi)")
endif()

# Pass the profile name to the compiler as TTL_BOARD_<name> for board.h.
add_compile_definitions(TTL_BOARD_${TTL_BOARD})

message(STATUS "TTL_BOARD=${TTL_BOARD}  ->  PICO_BOARD=${PICO_BOARD}")
