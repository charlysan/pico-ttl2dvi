#include "pico/stdlib.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "capture.h"
#include "sync.h"
#include "version.h"
#include "console.h"
#include "commands.h"
#include "video.h"
#include "view.h"
#include "source.h"

int main(void) {
    source_init();

    // Overclock
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(source_active()->sysclk_khz, true);

    video_init();
    sync_init();
    capture_init();
    view_init();

    stdio_init_all();
    // Do NOT wait for USB

    // Register console commands
    console_init();
    console_register("version", cmd_version, "firmware version");
    console_register("status", cmd_status, "system status");
    console_register("source", cmd_source, "list / set video source (reboots)");
    console_register("mode", cmd_mode, "list / set output mode (reboots)");
    console_register("mdalevels", cmd_mdalevels, "MDA grey levels: mdalevels [normal [bright]] (0..3)");
    console_register("vscale", cmd_vscale, "vertical scale 1..4");
    console_register("vpos", cmd_vpos, "vertical position, source lines (+ = down)");
    console_register("hpos", cmd_hpos, "horizontal position, source px (+ = right)");
    console_register("capture", cmd_capture, "capture a frames");
    console_register("bp", cmd_bp, "back porch");
    console_register("phase", cmd_phase, "sampling phase, sysclk steps within a pixel");
    console_register("dvi_test", cmd_test, "run dvi test pattern");

    while (true) {
        if (capture_grab()) {
            view_render();

            // EGA follows the card between its two scan-rate families live:
            // same sysclk and raster, so only core0 changes.
            int v = source_ega_check(capture_line_cycles());
            if (v >= 0) {
                video_set_vmap(0, 1, 0);        // black until the next render
                source_ega_select((uint)v);
                capture_reconfigure();
                view_init();
            }
        }
        console_poll();
    }

}
