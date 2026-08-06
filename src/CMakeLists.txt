# ttl2dvi_core: the shared module library that apps link. Grows as modules are
# added. For now: console + video (DVI output), plus config headers.
add_library(ttl2dvi_core STATIC
    config/source.c
    console/console.c
    video/video.c
    capture/sync.c
    capture/capture.c
    view/view.c
    settings/settings.c
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
    ${CMAKE_CURRENT_LIST_DIR}/settings
    ${CMAKE_SOURCE_DIR}/extern/dvi     # common_dvi_pin_configs.h (via board.h)
)
target_link_libraries(ttl2dvi_core PUBLIC
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_flash        # settings: persistent profiles in the last sector
    hardware_sync         # settings: interrupt guard around the flash write
    libdvi
)
target_compile_definitions(ttl2dvi_core PUBLIC
    # TMDS pins may be > 31 (pizero 32-39) -> need the PIO GPIO-base window.
    PICO_PIO_USE_GPIO_BASE=1
    # COLOUR: 3 independent TMDS lanes. NOT monochrome, even though MDA is a
    # mono source -- DVI_MONOCHROME_TMDS is compile-time for the whole library,
    # so one binary cannot have it both ways, and CGA640 needs colour. MDA still
    # uses the cheap tmds_encode_2bpp and simply copies its lane to the other
    # two (video.c core1_mono), which is what mono mode did in hardware anyway.
    DVI_N_TMDS_BUFFERS=3
    # Default only. Vertical repeat is a RUNTIME value now (dvi_inst.v_repeat,
    # see the vendored libdvi patch): MDA runs 1 for full-res, CGA640 runs 2
    # because the 3-lane palette encode will not fit a single scanline budget.
    DVI_VERTICAL_REPEAT=1
)
