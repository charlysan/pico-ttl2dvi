# Standalone DVI test card: 8 colour bars, no capture. Proves the DVI output
# path (TMDS pins 36/34/32 + clk 38, PIO GPIO base 16, overclock, monitor) in
# isolation. Links ONLY libdvi -- libdvi is added once by the root CMakeLists,
# so this app must not re-add it.
add_executable(dvi_test main.c)

# board.h (project board profile) + common_dvi_pin_configs.h (vendored, pulled
# in by board.h). The board profile itself is chosen by cmake/boards.cmake.
target_include_directories(dvi_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/config
    ${CMAKE_SOURCE_DIR}/extern/dvi
)

target_compile_definitions(dvi_test PRIVATE
    # Enable the PIO GPIO-base window (needed when TMDS pins are > 31, e.g. the
    # pizero's 32-39; harmless when base is 0). The actual base + pin config
    # come from board.h.
    PICO_PIO_USE_GPIO_BASE=1
)
target_link_libraries(dvi_test pico_stdlib pico_multicore pico_util libdvi)
pico_enable_stdio_usb(dvi_test 1)
pico_enable_stdio_uart(dvi_test 0)
pico_add_extra_outputs(dvi_test)
