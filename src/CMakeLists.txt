add_library(ttl2dvi_core STATIC
    console/console.c
    capture/sync.c
    capture/capture.c
)

# PIO headers: sync-period countdown (HSYNC/VSYNC measure) + the 2bpp sampler.
pico_generate_pio_header(ttl2dvi_core ${CMAKE_CURRENT_LIST_DIR}/capture/sync_period.pio)
pico_generate_pio_header(ttl2dvi_core ${CMAKE_CURRENT_LIST_DIR}/capture/capture.pio)

target_include_directories(ttl2dvi_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/console
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${CMAKE_CURRENT_LIST_DIR}/capture
    ${CMAKE_SOURCE_DIR}/extern/dvi     # common_dvi_pin_configs.h (via board.h)
)

target_link_libraries(ttl2dvi_core PUBLIC
    pico_stdlib
    libdvi
    hardware_dma
)