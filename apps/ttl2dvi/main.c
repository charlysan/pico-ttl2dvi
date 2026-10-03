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
#include "sigcheck.h"
#include "detect.h"
#include "settings.h"
#include "osd.h"

int main(void) {
    settings_init();
    source_init(settings_source_id(), settings_auto());

    // Overclock; 1.25 V above 256 MHz (CGA 258, EGA 266.4) for margin.
    const uint khz = source_active()->sysclk_khz;
    vreg_set_voltage(khz > 256000u ? VREG_VOLTAGE_1_25 : VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(khz, true);

    // A saved slot may choose the output mode, which must be known before DVI
    // starts; its knobs can only be applied once everything is up.
    const int mode = settings_boot_mode();
    if (mode >= 0) video_preselect_mode((uint)mode);
    video_init();
    sync_init();
    capture_init();
    view_init();
    settings_apply_boot();

    stdio_init_all();
    // Do NOT wait for USB

    // Register console commands
    console_init();
    console_register("version", cmd_version, "firmware version");
    console_register("status", cmd_status, "system status");
    console_register("state", cmd_state, "all state in one key=value line, for tools");
    console_register("slots", cmd_slots, "this source's saved settings");
    console_register("save", cmd_save, "save settings: save <slot> [name] (reboots)");
    console_register("load", cmd_load, "load settings: load <slot>");
    console_register("clear", cmd_clear, "clear a slot: clear <slot> (reboots)");
    console_register("default", cmd_default, "slot applied at boot: default <slot>|off (reboots)");
    console_register("source", cmd_source, "list / set video source (reboots), or source auto on|off");
    console_register("mode", cmd_mode, "list / set output mode (reboots)");
    console_register("mdalevels", cmd_mdalevels, "MDA grey levels: mdalevels [normal [bright]] (0..3)");
    console_register("vscale", cmd_vscale, "vertical scale 1..4");
    console_register("vpos", cmd_vpos, "vertical position, source lines (+ = down)");
    console_register("hpos", cmd_hpos, "horizontal position, source px (+ = right)");
    console_register("scanlines", cmd_scanlines, "scanlines on|off (needs vscale >= 2)");
    console_register("osd", cmd_osd, "on-screen display: osd on|off|auto|hold|hide|status|live [off]; osd ? for details");
    console_register("osd_print", cmd_osd_print, "show text on screen for 5 s: osd_print <text>");
    console_register("capture", cmd_capture, "capture a frame");
    console_register("bp", cmd_bp, "back porch");
    console_register("dotclock", cmd_dotclock, "dot clock in MHz, or default");
    console_register("phase", cmd_phase, "sampling phase, sysclk steps within a pixel");
    console_register("detect", cmd_detect, "which source auto detection would pick");
    console_register("measure", cmd_measure, "measure the dot clock from pixel edges: measure [skip lines]");
    console_register("fastcap", cmd_fastcap, "high-rate capture for tools/autotune.py: fastcap [skip lines]");
    console_register("dvi_test", cmd_test, "run dvi test pattern");

    while (true) {
        if (!capture_grab()) {
            signal_lost();
            osd_lost();
            auto_poll(0);
        } else {
            const uint32_t line = capture_line_cycles();
            signal_feed(line, capture_height());
            osd_frame(line, capture_height());

            // A frame from another source or EGA family is not rendered: the
            // last good frame stays on screen.
            if (source_line_ok(line)) view_render();

            // EGA follows the card between its two scan-rate families live:
            // same sysclk and raster, so only core0 changes.
            int v = source_ega_check(line);
            if (v >= 0) {
                source_ega_select((uint)v);
                capture_reconfigure();
                view_init();
                settings_apply_default();
            }

            // Last: a detection overwrites rawbuf with its fast capture.
            auto_poll(line);
        }
        console_poll();
    }

}
