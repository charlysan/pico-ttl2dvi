#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "capture.h"
#include "sync.h"
#include "version.h"
#include "console.h"
#include "video.h"
#include "view.h"

#define DVI_CLK_KHZ  256000

int main(void) {
    // Overclock
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(DVI_CLK_KHZ, true);

    video_init();
    sync_init();
    capture_init();

    stdio_init_all();
    // Do NOT wait for USB

    // Register console commands
    console_register("version", cmd_version, "firmware version");
    console_register("status", cmd_status, "system status");
    console_register("capture", cmd_capture, "capture a frames");
    console_register("bp", cmd_bp, "back porch");
    console_register("phase", cmd_phase, "sampling phase 0-15 (1/16 px)");
    console_register("dvi_test", cmd_test, "run dvi test pattern");

    while (true) {
        if (capture_grab()) view_render();
        console_poll();
    }

}
