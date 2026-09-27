add_library(ttl2dvi_core STATIC
    console/console.c
    config/source.c
    capture/sync.c
    capture/capture.c
    video/video.c
    view/view.c
)

# PIO headers: sync-period countdown (HSYNC/VSYNC measure) + the 2bpp sampler.
pico_generate_pio_header(ttl2dvi_core ${CMAKE_CURRENT_LIST_DIR}/capture/sync_period.pio)
pico_generate_pio_header(ttl2dvi_core ${CMAKE_CURRENT_LIST_DIR}/capture/capture.pio)

target_include_directories(ttl2dvi_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/console
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${CMAKE_CURRENT_LIST_DIR}/capture
    ${CMAKE_CURRENT_LIST_DIR}/video
    ${CMAKE_CURRENT_LIST_DIR}/view
    ${CMAKE_SOURCE_DIR}/extern/dvi     # common_dvi_pin_configs.h (via board.h)
)

# libdvi is INTERFACE: it compiles with THIS target's defines, not the app's.
target_compile_definitions(ttl2dvi_core PUBLIC
    DVI_VERTICAL_REPEAT=1      # default 2 = half-height picture
    DVI_MONOCHROME_TMDS=1      # all 3 lanes share one buffer; one encode per line
    DVI_DEFAULT_PIO_INST=pio1  # pio0 is capture's; a PIO's GPIO base is global
    PICO_PIO_USE_GPIO_BASE=1   # TMDS pins 32-39 are outside the default window
)

target_link_libraries(ttl2dvi_core PUBLIC
    pico_stdlib
    pico_multicore
    libdvi
    hardware_dma
    hardware_watchdog
)
