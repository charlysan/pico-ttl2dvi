// Console command implementations for the ttl2dvi app.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"

#include "commands.h"
#include "source.h"
#include "version.h"
#include "video.h"
#include "sync.h"
#include "capture.h"
#include "view.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// shared argument helpers
// ---------------------------------------------------------------------------

// Knob argument: a BARE "+" or "-" nudges by one (`phase +`, `phase +`,
// `phase -` while tuning); anything containing digits is an ABSOLUTE value,
// including negatives -- `vpos -5` sets -5.
//
static int knob_arg(int cur, const char *a) {
    if (!strcmp(a, "+")) return cur + 1;
    if (!strcmp(a, "-")) return cur - 1;
    return atoi(a);                       // "5", "-5" and "+5" are all absolute
}

// Parse and range-check a profile slot index, complaining on stdout if bad.
static bool slot_arg(const char *a, int *out) {
    int n = atoi(a);
    if (n < 0 || n >= settings_slot_count()) {
        printf("slot must be 0..%d\n", settings_slot_count() - 1);
        return false;
    }
    *out = n;
    return true;
}

// ---------------------------------------------------------------------------
// diagnostics
// ---------------------------------------------------------------------------

void cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("ttl2dvi %s\n", TTL2DVI_VERSION);
}

// Restart the firmware.
void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("rebooting...\n");
    sleep_ms(50);              // let the line flush over USB first
    watchdog_reboot(0, 0, 0);
}

// Reboot into BOOTSEL (USB mass-storage)
void cmd_bootsel(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("entering BOOTSEL (reflash mode)...\n");
    sleep_ms(50);
    reset_usb_boot(0, 0);
}

// Show a DVI test pattern. Optional arg: 0 = stripes (default), 1 = checkerboard.
// The sleep holds the pattern on screen: with a live source the capture loop
// overwrites the framebuffer as soon as this returns.
void cmd_test(int argc, char **argv) {
    int which = (argc >= 2) ? atoi(argv[1]) : 0;
    if (which == 1) {
        video_test_pattern_checkerboard();
        printf("checkerboard on DVI\n");
        sleep_ms(5000);
    } else {
        video_test_pattern_stripes();
        printf("stripes on DVI\n");
        sleep_ms(5000);
    }
}

// Report HSYNC/VSYNC frequency, measured live via the pio0 countdown SMs.
void cmd_status(int argc, char **argv) {
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
void cmd_capture(int argc, char **argv) {
    (void)argc; (void)argv;
    capture_dump_frame();
}

// ---------------------------------------------------------------------------
// source card
// ---------------------------------------------------------------------------

// `source` lists the cards, `source <n|name>` switches and REBOOTS into it.
// A switch changes the sampler pin count, the sysclk AND the DVI encoder, so a
// reboot is the only sane way to apply it.
void cmd_source(int argc, char **argv) {
    if (argc >= 2) {
        uint sel = source_count();                      // = "not found"
        if (argv[1][0] >= '0' && argv[1][0] <= '9' && argv[1][1] == '\0')
            sel = (uint)(argv[1][0] - '0');
        if (sel >= source_count())                      // else match on the name
            for (uint i = 0; i < source_count(); i++)
                if (strstr(source_at(i)->name, argv[1])) { sel = i; break; }
        if (sel >= source_count()) {
            printf("no such source: %s\n", argv[1]);
        } else {
            printf("switching to %s (reboots; USB will drop)...\n", source_at(sel)->name);
            sleep_ms(50);
            source_set(sel);                            // does not return
        }
    }
    for (uint i = 0; i < source_count(); i++) {
        const source_mode_t *s = source_at(i);
        printf("  %u  %-8s %u data bits @ GP%u, %u px, %u.%03u MHz dot, sysclk %u MHz%s\n",
               i, s->name, s->data_bits, s->data_base, s->active_w,
               s->dot_clock_hz / 1000000u, (s->dot_clock_hz / 1000u) % 1000u,
               s->sysclk_khz / 1000u,
               i == source_current_index() ? "   <- current" : "");
    }
}

// ---------------------------------------------------------------------------
// output mode
// ---------------------------------------------------------------------------

// DVI output mode. `mode` lists them, `mode <n>` or `mode <substring>` (e.g.
// `mode 576`) selects one and reboots into it.
void cmd_mode(int argc, char **argv) {
    if (argc >= 2) {
        uint sel = video_mode_count();                  // = "not found"
        if (argv[1][0] >= '0' && argv[1][0] <= '9' && argv[1][1] == '\0')
            sel = (uint)(argv[1][0] - '0');             // single digit = index
        if (sel >= video_mode_count())                  // else match on the name
            for (uint i = 0; i < video_mode_count(); i++)
                if (strstr(video_mode_name(i), argv[1])) { sel = i; break; }
        if (sel >= video_mode_count()) {
            printf("no such mode: %s\n", argv[1]);
        } else {
            printf("switching to %s (reboot)...\n", video_mode_name(sel));
            sleep_ms(50);                               // flush over USB first
            video_set_mode(sel);                        // does not return
        }
    }
    for (uint i = 0; i < video_mode_count(); i++)
        printf("  %u  %s%s\n", i, video_mode_name(i),
               i == video_mode_current() ? "   <- current" : "");
}

// ---------------------------------------------------------------------------
// capture framing
// ---------------------------------------------------------------------------

// Horizontal framing: slide the capture window by WHOLE source pixels.
// `bp` shows it, `bp <n>` / `bp +` / `bp -` set it.
void cmd_bp(int argc, char **argv) {
    if (argc >= 2) capture_set_bp(knob_arg(capture_get_bp(), argv[1]));
    printf("bp = %d px (horizontal back-porch trim, whole source pixels)\n",
           capture_get_bp());
}

// Sampling instant WITHIN the pixel, in sysclk cycles (1/16 pixel at 256 MHz).
// `phase` shows it, `phase <n>` / `phase +` / `phase -` set it.
void cmd_phase(int argc, char **argv) {
    if (argc >= 2) capture_set_phase(knob_arg(capture_get_phase(), argv[1]));
    printf("phase = %d/%u px (sampling instant within the pixel, sysclk cycles)\n",
           capture_get_phase(), capture_px_cyc());
}

// `dotclock [MHz|Hz|+|-]` -- trim the assumed dot clock for a card whose
// crystal differs from its profile's. The only knob that scales horizontally:
// a wrong value shows as wrong image WIDTH. `+`/`-` step 1 kHz. Saved by `save`.
void cmd_dotclock(int argc, char **argv) {
    if (argc >= 2) {
        const char *a = argv[1];
        uint32_t cur = capture_get_dot_hz();
        if (!strcmp(a, "+"))      capture_set_dot_hz(cur + 1000);
        else if (!strcmp(a, "-")) capture_set_dot_hz(cur - 1000);
        else {
            double v = atof(a);                  // "16" / "16.257" = MHz
            capture_set_dot_hz((v < 1000.0) ? (uint32_t)(v * 1e6 + 0.5)
                                            : (uint32_t)v);
        }
    }
    uint32_t hz = capture_get_dot_hz();
    printf("dotclock = %lu.%03lu MHz   px_cyc %u%s\n",
           (unsigned long)(hz / 1000000u), (unsigned long)((hz / 1000u) % 1000u),
           capture_px_cyc(),
           hz == g_src->dot_clock_hz ? "  (card default)" : "  (trimmed)");
}

// ---------------------------------------------------------------------------
// display framing + levels
// ---------------------------------------------------------------------------

// Vertical scale: `vscale 2` line-doubles
// Useful for games that look squat at 1:1 in Hercules mode.
void cmd_vscale(int argc, char **argv) {
    if (argc >= 2) view_set_vscale(knob_arg(view_get_vscale(), argv[1]));
    printf("vscale = %dx vertical (horizontal is always 1:1)\n", view_get_vscale());
}

// Vertical position in SOURCE lines; positive moves the IMAGE DOWN.
void cmd_vpos(int argc, char **argv) {
    if (argc >= 2) view_set_vpos(knob_arg(view_get_vpos(), argv[1]));
    printf("vpos = %d source lines (+ moves the image down)\n", view_get_vpos());
}

// Horizontal position in SOURCE pixels; positive moves the IMAGE RIGHT.
void cmd_hpos(int argc, char **argv) {
    if (argc >= 2) view_set_hpos(knob_arg(view_get_hpos(), argv[1]));
    printf("hpos = %d source px (+ moves the image right)\n", view_get_hpos());
}

// MDA gray levels for VIDEO-alone and VIDEO+INTENSITY (0..3).
void cmd_mda_levels(int argc, char **argv) {
    if (argc >= 2) view_set_mda_lvl_normal(knob_arg(view_get_mda_lvl_normal(), argv[1]));
    if (argc >= 3) view_set_mda_lvl_bright(knob_arg(view_get_mda_lvl_bright(), argv[2]));
    printf("mda_levels normal=%d bright=%d\n",
           view_get_mda_lvl_normal(), view_get_mda_lvl_bright());
}

// ---------------------------------------------------------------------------
// persistent profiles (flash)
// ---------------------------------------------------------------------------

// List every slot: name, contents, and which one auto-loads at boot.
void cmd_slots(int argc, char **argv) {
    (void)argc; (void)argv;
    char sum[96];
    int boot = settings_boot_slot();
    for (int i = 0; i < settings_slot_count(); i++) {
        if (settings_slot_summary(i, sum, sizeof sum))
            printf("  %d  %-14s %s%s%s\n", i, settings_slot_name(i), sum,
                   settings_slot_foreign(i) ? "   (other card -- not loadable)" : "",
                   i == boot ? "   [boot]" : "");
        else
            printf("  %d  <empty>\n", i);
    }
    if (boot < 0) printf("  boot: off\n");
}

// `save <n> [name]` - capture the CURRENT settings into slot n.
void cmd_save(int argc, char **argv) {
    int n;
    if (argc < 2 || !slot_arg(argv[1], &n)) {
        printf("usage: save <0..%d> [name]\n", settings_slot_count() - 1);
        return;
    }
    char name[32] = "";
    for (int i = 2; i < argc; i++) {
        if (*name) strncat(name, " ", sizeof name - strlen(name) - 1);
        strncat(name, argv[i], sizeof name - strlen(name) - 1);
    }
    // Reboots on success and never returns - a flash write kills the DVI output
    // until restart, so every writer restarts (see settings.h).
    printf("saving slot %d (reboots; USB will drop)...\n", n);
    sleep_ms(50);                       // flush the line over USB first
    settings_save(n, name);             // reports the outcome, then reboots
}

// `load <n>` - apply slot n. Reboots first if its output mode differs.
void cmd_load(int argc, char **argv) {
    int n;
    if (argc < 2 || !slot_arg(argv[1], &n)) {
        printf("usage: load <0..%d>\n", settings_slot_count() - 1);
        return;
    }
    if (!settings_slot_valid(n)) {
        printf("slot %d is empty\n", n);
        return;
    }
    if (settings_slot_foreign(n)) {
        printf("slot %d was saved on a different source card -- "
               "switch with `source` first\n", n);
        return;
    }
    printf("loading slot %d...\n", n);
    sleep_ms(50);                       // in case it reboots, flush over USB
    if (settings_load(n))               // may not return (mode change -> reboot)
        printf("loaded slot %d\n", n);
}

// `clear <n>` - erase slot n (and disarm boot if it pointed there).
void cmd_clear(int argc, char **argv) {
    int n;
    if (argc < 2 || !slot_arg(argv[1], &n)) {
        printf("usage: clear <0..%d>\n", settings_slot_count() - 1);
        return;
    }
    printf("clearing slot %d (reboots; USB will drop)...\n", n);
    sleep_ms(50);
    settings_clear(n);                  // reports the outcome, then reboots
}

// `boot [n|off]` - pick the slot applied automatically at power-on.
void cmd_boot(int argc, char **argv) {
    if (argc >= 2) {
        int n = -1;
        if (strcmp(argv[1], "off") && !slot_arg(argv[1], &n)) return;
        if (n >= 0 && !settings_slot_valid(n)) {
            printf("slot %d is empty -- save it before arming boot\n", n);
            return;
        }
        printf("setting boot slot (reboots; USB will drop)...\n");
        sleep_ms(50);
        settings_set_boot_slot(n);      // reports the outcome, then reboots
        return;
    }
    int b = settings_boot_slot();
    if (b < 0) printf("boot: off (nothing auto-loads)\n");
    else       printf("boot: slot %d  %s\n", b, settings_slot_name(b));
}

// `dump` - print every profile as one hex line, to keep as an offline backup.
void cmd_dump(int argc, char **argv) {
    (void)argc; (void)argv;
    settings_dump();
}

// `restore <hex>` - rebuild every profile from a `dump` blob. Validated before
// anything is written
void cmd_restore(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: restore <hex from `dump`>\n");
        return;
    }
    // The blob is one unbroken token, but rejoin any argv split so a paste that
    // picked up stray spaces still works (strtok turned them into NULs).
    static char hex[CONSOLE_LINE_MAX];
    hex[0] = '\0';
    for (int i = 1; i < argc; i++)
        strncat(hex, argv[i], sizeof hex - strlen(hex) - 1);

    printf("restoring (reboots; USB will drop)...\n");
    sleep_ms(50);
    switch (settings_restore_hex(hex)) {          // reboots on success
    case SETTINGS_RESTORE_BAD_HEX:
        printf("restore FAILED: not valid hex, or wrong length -- re-run `dump`\n");
        break;
    case SETTINGS_RESTORE_BAD_DATA:
        printf("restore FAILED: wrong format version. Nothing was written.\n");
        break;
    default:                            // OK reports and reboots inside
        break;
    }
}
