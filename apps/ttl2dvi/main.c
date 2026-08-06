// ttl2dvi -- full capture -> DVI pipeline.

#include "pico/stdlib.h"
#include "console.h"
#include "commands.h"
#include "video.h"
#include "sync.h"
#include "capture.h"
#include "view.h"
#include "settings.h"
#include "source.h"

int main(void) {
    // The source card comes FIRST of all: it decides the sysclk, the mode table
    // and the sampler's pin count, so everything below reads g_src as settled.
    source_apply_boot_choice();

    // Saved profiles next: a pure flash read, no side effects, so it is safe
    // before anything else is up -- and video_init needs the boot slot's mode.
    settings_init();
    int boot_mode = settings_boot_mode();          // -1 = no boot slot
    if (boot_mode >= 0) video_preselect_mode((uint)boot_mode);

    // DVI (+ the 256 MHz overclock) first, before stdio brings up USB.
    video_init();
    sync_init();                 // HSYNC/VSYNC measurement SMs on pio0
    capture_init();              // sampler SM + DMA on pio0 (needs sync_init first)

    // The rest of the boot slot (framing, levels) -- after capture_init, so
    // nothing it initialises overwrites the restored values.
    settings_apply_boot();

    stdio_init_all();
    // Do NOT wait for USB: the live pipeline must run whether or not a PC is
    // attached.

    console_register("version", cmd_version, "firmware version");
    console_register("reboot",  cmd_reboot,  "restart the firmware");
    console_register("bootsel", cmd_bootsel, "reboot into BOOTSEL to reflash");
    console_register("test",    cmd_test,    "DVI pattern: test [0=stripes|1=checker]");
    console_register("status",  cmd_status,  "HSYNC/VSYNC frequency");
    console_register("capture", cmd_capture, "grab a frame, dump over USB");
    console_register("source",  cmd_source,  "source card: source [n|name] (reboots)");
    console_register("mode",    cmd_mode,    "DVI output mode: mode [n] (reboots)");
    console_register("bp",      cmd_bp,      "framing: bp [n|+|-] (whole px)");
    console_register("phase",   cmd_phase,   "sampling instant: phase [n|+|-] (sysclk)");
    console_register("dotclock", cmd_dotclock, "trim dot clock: dotclock [MHz|+|-]");
    console_register("vscale",  cmd_vscale,  "vertical scale: vscale [1|2|+|-]");
    console_register("vpos",    cmd_vpos,    "vertical position: vpos [n|+|-] (source lines)");
    console_register("hpos",    cmd_hpos,    "horizontal position: hpos [n|+|-] (source px)");
    console_register("mdalevels", cmd_mda_levels, "MDA gray levels: mdalevels [normal [bright]] (0..3)");
    console_register("slots",   cmd_slots,   "list the saved profiles");
    console_register("save",    cmd_save,    "save current settings: save <n> [name]");
    console_register("load",    cmd_load,    "apply a saved profile: load <n>");
    console_register("clear",   cmd_clear,   "erase a profile: clear <n>");
    console_register("boot",    cmd_boot,    "auto-load profile: boot [n|off]");
    console_register("dump",    cmd_dump,    "print all profiles as hex (backup)");
    console_register("restore", cmd_restore, "restore profiles: restore <hex>");
    console_init();

    // Live view: grab a frame, composite it into the DVI framebuffer, service
    // the console. With no signal capture_grab() times out (~false) and the
    // framebuffer keeps its last content (so a `test` pattern stays visible).
    while (true) {
        if (capture_grab())
            view_render();
        console_poll();
    }
}
