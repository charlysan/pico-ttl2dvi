#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "capture.h"
#include "sync.h"
#include "version.h"
#include "console.h"

#define DVI_CLK_KHZ  256000

int main(void) {
    // Overclock
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(DVI_CLK_KHZ, true);

    sync_init();
    capture_init();

    stdio_init_all();
    // Do NOT wait for USB

    // Register console commands
    console_register("version", cmd_version, "firmware version");
    console_register("status", cmd_status, "system status");
    console_register("capture", cmd_capture, "capture a frames");
    console_register("bp", cmd_bp, "back porch");
    console_register("phase", cmd_phase, "sampling phase 0|1 (half px)");

    while (true) {
        console_poll();
    }

}
