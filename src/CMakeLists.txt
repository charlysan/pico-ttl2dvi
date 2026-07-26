# ttl2dvi_core: the shared module library that apps link. Grows as modules are
# added. For now: console + video (DVI output), plus config headers.
add_library(ttl2dvi_core STATIC
    config/source.c
    console/console.c
    video/video.c
    capture/sync.c
    capture/capture.c
    view/view.c
)

# PIO headers: sync-period countdown (HSYNC/VSYNC measure) + the 2bpp sampler.
pico_generate_pio_header(ttl2dvi_core ${CMAKE_CURRENT_LIST_DIR}/capture/sync_period.pio)
pico_generate_pio_header(ttl2dvi_core ${CMAKE_CURRENT_LIST_DIR}/capture/capture.pio)

# PUBLIC so apps that link ttl2dvi_core inherit these headers, libs, and the
# libdvi config defines. The defines MUST reach the app too: the app compiles
# libdvi's INTERFACE sources, so its libdvi and our video.c must agree on the
# monochrome/2bpp config or the DVI ABI mismatches.
target_include_directories(ttl2dvi_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/console
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${CMAKE_CURRENT_LIST_DIR}/video
    ${CMAKE_CURRENT_LIST_DIR}/capture
    ${CMAKE_CURRENT_LIST_DIR}/view
    ${CMAKE_SOURCE_DIR}/extern/dvi     # common_dvi_pin_configs.h (via board.h)
)
target_link_libraries(ttl2dvi_core PUBLIC
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    libdvi
)
target_compile_definitions(ttl2dvi_core PUBLIC
    # TMDS pins may be > 31 (pizero 32-39) -> need the PIO GPIO-base window.
    PICO_PIO_USE_GPIO_BASE=1
    # 2bpp grayscale, single TMDS channel replicated to all lanes.
    DVI_MONOCHROME_TMDS
    DVI_N_TMDS_BUFFERS=3
    # Full 480-line framebuffer -> no vertical doubling.
    DVI_VERTICAL_REPEAT=1
)
