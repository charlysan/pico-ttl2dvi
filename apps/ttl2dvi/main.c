// ttl2dvi -- full capture -> DVI pipeline.

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "console.h"
#include "version.h"
#include "video.h"
#include "sync.h"
#include "capture.h"

static void cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("ttl2dvi %s\n", TTL2DVI_VERSION);
}

// Restart the firmware.
static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("rebooting...\n");
    sleep_ms(50);              // let the line flush over USB first
    watchdog_reboot(0, 0, 0);
}

// Reboot into BOOTSEL (USB mass-storage) so a new .uf2 can be flashed without
// the physical button -- the "quit to reflash" command during development.
static void cmd_bootsel(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("entering BOOTSEL (reflash mode)...\n");
    sleep_ms(50);
    reset_usb_boot(0, 0);
}

// Show a DVI test pattern. Optional arg: 0 = stripes (default), 1 = checkerboard.
static void cmd_test(int argc, char **argv) {
    int which = (argc >= 2) ? atoi(argv[1]) : 0;
    if (which == 1) {
        video_test_pattern_checkerboard();
        printf("checkerboard on DVI\n");
    } else {
        video_test_pattern_stripes();
        printf("stripes on DVI\n");
    }
}

// Report HSYNC/VSYNC frequency, measured live via the pio0 countdown SMs.
static void cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t f  = clock_get_hz(clk_sys);
    uint32_t ph = sync_hsync_period();     // clk_sys cycles, 0 = no signal
    uint32_t pv = sync_vsync_period();

    if (ph) {
        uint32_t h = (uint32_t)((uint64_t)f * 100 / ph);   // Hz x100
        printf("HSYNC %lu.%02lu Hz", (unsigned long)(h / 100), (unsigned long)(h % 100));
    } else {
        printf("HSYNC --");
    }
    if (pv) {
        uint32_t v = (uint32_t)((uint64_t)f * 100 / pv);
        printf("   VSYNC %lu.%02lu Hz\n", (unsigned long)(v / 100), (unsigned long)(v % 100));
    } else {
        printf("   VSYNC --\n");
    }
}

// Grab one frame of VIDEO+INTENSITY and dump it over USB (host: tools/grab.py).
static void cmd_capture(int argc, char **argv) {
    (void)argc; (void)argv;
    capture_dump_frame();
}

int main(void) {
    // DVI (+ the 256 MHz overclock) first, before stdio brings up USB.
    video_init();
    sync_init();                 // HSYNC/VSYNC measurement SMs on pio0
    capture_init();              // sampler SM + DMA on pio0 (needs sync_init first)

    stdio_init_all();
    // TEMPORARY: wait for USB so the banner is seen. Fine now (DVI already runs
    // on core1 regardless); revisit when capture arrives.
    while (!stdio_usb_connected()) sleep_ms(100);

    console_register("version", cmd_version, "firmware version");
    console_register("reboot",  cmd_reboot,  "restart the firmware");
    console_register("bootsel", cmd_bootsel, "reboot into BOOTSEL to reflash");
    console_register("test",    cmd_test,    "DVI pattern: test [0=stripes|1=checker]");
    console_register("status",  cmd_status,  "HSYNC/VSYNC frequency");
    console_register("capture", cmd_capture, "grab a frame, dump over USB");
    console_init();

    while (true) {
        console_poll();
    }
}
