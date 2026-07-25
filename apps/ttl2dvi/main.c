// ttl2dvi -- full capture -> DVI pipeline.

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "console.h"
#include "version.h"

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

int main(void) {
    stdio_init_all();
    // TEMPORARY: the console is the only feature, so just wait for USB. Once
    // capture/DVI exist, the pipeline must run regardless of USB -- drop this.
    while (!stdio_usb_connected()) sleep_ms(100);

    console_register("version", cmd_version, "firmware version");
    console_register("reboot",  cmd_reboot,  "restart the firmware");
    console_register("bootsel", cmd_bootsel, "reboot into BOOTSEL to reflash");
    console_init();

    while (true) {
        console_poll();
    }
}
