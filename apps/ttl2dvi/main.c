// ttl2dvi -- full capture -> DVI pipeline.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "console.h"
#include "version.h"
#include "video.h"

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

// Show the DVI line/torture pattern (1px black/white stripes).
static void cmd_test(int argc, char **argv) {
    (void)argc; (void)argv;
    video_test_pattern();
    printf("test pattern on DVI\n");
}

int main(void) {
    // DVI (+ the 256 MHz overclock) first, before stdio brings up USB.
    video_init();

    stdio_init_all();
    // TEMPORARY: wait for USB so the banner is seen. Fine now (DVI already runs
    // on core1 regardless); revisit when capture arrives.
    while (!stdio_usb_connected()) sleep_ms(100);

    console_register("version", cmd_version, "firmware version");
    console_register("reboot",  cmd_reboot,  "restart the firmware");
    console_register("bootsel", cmd_bootsel, "reboot into BOOTSEL to reflash");
    console_register("test",    cmd_test,    "show DVI line pattern");
    console_init();

    while (true) {
        console_poll();
    }
}
