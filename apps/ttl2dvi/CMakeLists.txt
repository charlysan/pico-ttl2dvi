# Full capture -> DVI pipeline. For now: USB console only (links ttl2dvi_core,
# which pulls in pico_stdlib + the console/config headers transitively).
add_executable(ttl2dvi main.c commands.c)
target_link_libraries(ttl2dvi ttl2dvi_core hardware_watchdog)
pico_enable_stdio_usb(ttl2dvi 1)
pico_enable_stdio_uart(ttl2dvi 0)
pico_add_extra_outputs(ttl2dvi)
