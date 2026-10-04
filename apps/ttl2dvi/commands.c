#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "commands.h"
#include "version.h"
#include "sync.h"
#include "capture.h"
#include "video.h"
#include "source.h"
#include "view.h"
#include "sigcheck.h"
#include "detect.h"
#include "settings.h"
#include "osd.h"

// --- diagnostics ---
void cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("ttl2dvi %s\n", TTL2DVI_VERSION);
}

void cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t f = clock_get_hz(clk_sys);
    uint32_t pulse;
    uint32_t ph = sync_hsync(&pulse);
    uint32_t pv = ph ? sync_vsync_period() : 0;     // no HSYNC: don't wait for VSYNC too

    const source_mode_t *src = source_active();
    printf("  SOURCE: %s, %u data bits, %ux\n",
           src->name, src->data_bits, capture_oversample());
    printf("  SYSCLK: %lu.%03lu MHz\n",
           (unsigned long)(f / 1000000),
           (unsigned long)((f / 1000) % 1000));
    if (ph) {
        uint32_t h = (uint32_t)((uint64_t)f * 100 / ph);
        uint cyc = capture_px_cyc();
        printf("  HSYNC: %lu.%02lu Hz\n",
               (unsigned long)(h / 100), (unsigned long)(h % 100));
        printf("  PULSE: %lu cyc = %lu px + %lu/%u\n",
               (unsigned long)pulse,
               (unsigned long)(pulse / cyc),
               (unsigned long)(pulse % cyc), cyc);
    } else {
        printf("  HSYNC --\n");
    }
    if (pv) {
        uint32_t v = (uint32_t)((uint64_t)f * 100 / pv);
        printf("  VSYNC: %lu.%02lu Hz\n",
               (unsigned long)(v / 100), (unsigned long)(v % 100));
    } else {
        printf("  VSYNC --\n");
    }

    // Rate since the previous status call (since boot on the first).
    static uint32_t last_frames;
    static uint64_t last_us;
    uint32_t frames = capture_frames();
    uint64_t now = time_us_64();
    uint32_t fps = (uint32_t)((uint64_t)(frames - last_frames) * 100000000u
                              / (now - last_us));
    printf("  CAPTURE: %lu.%02lu frames/s\n",
           (unsigned long)(fps / 100), (unsigned long)(fps % 100));
    last_frames = frames;
    last_us = now;

    uint lo, hi;
    capture_lines_range(&lo, &hi);
    printf("  LINES: %u..%u\n", lo, hi);

    static const char *const state[] = { "none", "unstable", "stable" };
    signal_info_t si;
    signal_get(&si);
    if (si.state == SIGNAL_NONE)
        printf("  SIGNAL: none\n");
    else
        printf("  SIGNAL: %s, %s group, %lu Hz / %lu.%02lu Hz, %lu.%lu s\n",
               state[si.state], signal_group_name(si.group),
               (unsigned long)si.hsync_hz,
               (unsigned long)(si.vsync_mhz / 1000u), (unsigned long)(si.vsync_mhz % 1000u / 10u),
               (unsigned long)(si.ms / 1000u), (unsigned long)(si.ms % 1000u / 100u));
}

// One line, space-separated key=value, for tools polling the device. Only
// values already known: HSYNC/VSYNC come from the signal check, so it never
// waits for a sync measurement.
void cmd_state(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *const sig[] = { "none", "unstable", "stable" };
    signal_info_t si;
    signal_get(&si);
    const bool live = si.state != SIGNAL_NONE;
    const uint32_t hs = live ? si.hsync_hz : 0, vs = live ? si.vsync_mhz : 0;
    printf("state src=%u grp=%u auto=%u mode=%u hs=%lu vs=%lu.%02lu sig=%s bits=%u os=%u"
           " bp=%d phase=%d pxcyc=%u dot=%u vscale=%u vpos=%d hpos=%d scan=%u lvl=%u,%u"
           " def=%d\n",
           source_active_index(), source_group(), source_auto() ? 1u : 0u,
           video_mode_current(), (unsigned long)hs,
           (unsigned long)(vs / 1000u), (unsigned long)(vs % 1000u / 10u), sig[si.state],
           source_active()->data_bits, capture_oversample(), capture_get_bp(),
           capture_get_phase(), capture_px_cyc(), capture_dot_hz(), view_get_vscale(),
           view_get_vpos(), view_get_hpos(), video_get_scanlines() ? 1u : 0u,
           view_get_mda_normal(), view_get_mda_bright(), settings_default());
}

void cmd_source(int argc, char **argv) {
    // Both forms also go to flash, so they hold after a power cycle; the
    // flash write reboots.
    if (argc >= 2 && !strcmp(argv[1], "auto")) {
        const bool on = argc >= 3 && !strcmp(argv[2], "on");
        if (argc < 3 || (!on && strcmp(argv[2], "off"))) {
            printf("source auto on|off\n");
        } else {
            source_set_auto(on);
            settings_save_source(source_active()->id, on);
        }
    } else if (argc >= 2) {
        char *end;
        long i = strtol(argv[1], &end, 10);
        if (*end || i < 0 || (uint)i >= source_count()) {
            printf("no such source: %s\n", argv[1]);
        } else {
            printf("switching to %s...\n", source_get((uint)i)->name);
            source_set_auto(false);
            source_store((uint)i);
            settings_save_source(source_get((uint)i)->id, false);
        }
    }
    printf("  auto detection: %s\n", source_auto() ? "on" : "off");
    for (uint i = 0; i < source_count(); i++) {
        const source_mode_t *s = source_get(i);
        printf("  %u  %-6s %u data bits @ GP%u, %u px, %u.%03u MHz dot%s, sysclk %u",
               i, s->name, s->data_bits, s->data_base, s->active_w,
               s->dot_hz / 1000000u, (s->dot_hz / 1000u) % 1000u,
               i == source_active_index() && capture_oversample() > 1 ? " (2x)" : "",
               s->sysclk_khz / 1000u);
        if (s->sysclk_khz % 1000u) printf(".%u", (s->sysclk_khz % 1000u) / 100u);
        printf(" MHz%s%s\n", s->id == SRC_ID_EGA ? ", both families" : "",
               i == source_active_index() ? "   <- current" : "");
    }
}

void cmd_mode(int argc, char **argv) {
    if (argc >= 2) {
        char *end;
        long i = strtol(argv[1], &end, 10);
        if (*end || i < 0 || (uint)i >= video_mode_count()) {
            printf("no such mode: %s\n", argv[1]);
        } else {
            printf("switching to %s (reboot)...\n", video_mode_name((uint)i));
            sleep_ms(50);
            video_set_mode((uint)i);
        }
    }
    for (uint i = 0; i < video_mode_count(); i++)
        printf("  %u  %s%s\n", i, video_mode_name(i),
               i == video_mode_current() ? "   <- current" : "");
}

void cmd_mdalevels(int argc, char **argv) {
    uint normal = view_get_mda_normal();
    uint bright = view_get_mda_bright();
    if (argc >= 2) normal = (uint)atoi(argv[1]);
    if (argc >= 3) bright = (uint)atoi(argv[2]);
    if (normal > 3 || bright > 3)
        printf("levels are 0..3\n");
    else
        view_set_mda_levels(normal, bright);
    if (argc >= 2)
        osd_knob("mdalevels %u %u", view_get_mda_normal(), view_get_mda_bright());
    printf("mdalevels normal=%u bright=%u\n", view_get_mda_normal(), view_get_mda_bright());
}

void cmd_vscale(int argc, char **argv) {
    if (argc >= 2) {
        view_set_vscale(atoi(argv[1]));
        osd_knob("vscale %u", view_get_vscale());
    }
    printf("vscale = %ux\n", view_get_vscale());
}

void cmd_vpos(int argc, char **argv) {
    if (argc >= 2) {
        view_set_vpos(atoi(argv[1]));
        osd_knob("vpos %d", view_get_vpos());
    }
    printf("vpos = %d source lines\n", view_get_vpos());
}

void cmd_hpos(int argc, char **argv) {
    if (argc >= 2) {
        view_set_hpos(atoi(argv[1]));
        osd_knob("hpos %d", view_get_hpos());
    }
    printf("hpos = %d source px\n", view_get_hpos());
}

void cmd_dotclock(int argc, char **argv) {
    if (argc >= 2) {
        uint hz = 0;
        if (strcmp(argv[1], "default")) {
            char *end;
            double mhz = strtod(argv[1], &end);
            if (*end || mhz <= 0.0) {
                printf("dotclock <MHz>|default\n");
                return;
            }
            hz = (uint)(mhz * 1e6 + 0.5);
        }
        if (!capture_set_dot_hz(hz))
            printf("out of range for this sysclk\n");
        const uint d = capture_dot_hz();
        osd_knob("dotclock %u.%04u", d / 1000000u, (d / 100u) % 10000u);
    }

    const uint dot = capture_dot_hz();
    const uint32_t spp = capture_spp();
    const uint64_t px100 = (uint64_t)clock_get_hz(clk_sys) * 100u / dot;
    printf("dotclock = %u.%03u MHz%s: %lu.%02lu sysclk/px, sample every %u (%ux), spp %lu.%04lu\n",
           dot / 1000000u, (dot / 1000u) % 1000u,
           dot == source_active()->dot_hz ? " (default)" : "",
           (unsigned long)(px100 / 100u), (unsigned long)(px100 % 100u),
           capture_sample_cyc(), capture_oversample(),
           (unsigned long)(spp >> 16), (unsigned long)(((spp & 0xffffu) * 10000u) >> 16));

    // At 1x a whole pixel would be skipped or repeated every
    // 1 / |px / round(px) - 1| px, the first at half that.
    const uint64_t r100 = (px100 + 50u) / 100u * 100u;
    const uint64_t d100 = px100 > r100 ? px100 - r100 : r100 - px100;
    if (d100) printf("  at 1x a pixel would slip every %lu px\n",
                     (unsigned long)(r100 / d100));
    else      printf("  at 1x no slip\n");
}

void cmd_measure(int argc, char **argv) {
    printf("measure: measuring...\n");
    fflush(stdout);
    capture_measure_t m;
    const source_mode_t *src = source_active();
    const uint cur = capture_dot_hz();
    const uint64_t t0 = time_us_64();
    const bool ok = capture_measure_dot(argc >= 2 ? (uint)atoi(argv[1]) : 80u,
                                        src->data_base, src->data_bits,
                                        cur / 100u * 95u, cur / 100u * 105u, &m);
    const uint ms = (uint)((time_us_64() - t0) / 1000u);
    if (!m.lines) { printf("measure: no sync signal\n"); return; }
    if (!ok) {
        printf("measure: %u edges in %u lines, too few (put text or a pattern on "
               "screen, or try another skip)\n", m.edges, m.lines);
        return;
    }
    const uint char_px = source_active()->data_bits == 2 ? 9u : 8u;
    const uint chars = (uint)(m.h_total / (float)char_px + 0.5f);
    const uint dot = capture_dot_hz();
    const int ppm = (int)(((int64_t)m.dot_hz - dot) * 1000000 / dot);
    printf("measure: %u edges in %u lines, %u ms\n", m.edges, m.lines, ms);
    printf("  dot clock %u.%04u MHz (period %u.%04u sysclk), alignment %u.%03u\n",
           m.dot_hz / 1000000u, (m.dot_hz / 100u) % 10000u,
           (uint)m.period, (uint)((m.period - (uint)m.period) * 10000.0f),
           (uint)m.align, (uint)(m.align * 1000.0f) % 1000u);
    printf("  h_total %u.%u px ~ %u chars of %u\n",
           (uint)m.h_total, (uint)((m.h_total - (uint)m.h_total) * 10.0f), chars, char_px);
    printf("  current dotclock %u.%04u MHz: %+d ppm\n",
           dot / 1000000u, (dot / 100u) % 10000u, ppm);
    if (m.align < 0.5f)
        printf("  low alignment: no real match within +/-5%% of the current dotclock;"
               " try detect, or dotclock near the card's clock first\n");
}

// What source auto detection would choose, and why. Changes nothing.
void cmd_detect(int argc, char **argv) {
    (void)argc; (void)argv;
    detect_result_t r;
    detect_run(&r);
    if (r.status == DETECT_NO_SIGNAL) {
        printf("detect: no stable signal, nothing to decide\n");
        return;
    }

    printf("detect: %lu Hz, %u candidate%s\n",
           (unsigned long)r.hsync_hz, r.ncand, r.ncand == 1 ? "" : "s");
    for (uint i = 0; i < r.ncand; i++) {
        const source_mode_t *m = r.cand[i].mode;
        printf("  %-5s %u-bit  %u.%04u MHz\n", m->name, m->data_bits,
               m->dot_hz / 1000000u, (m->dot_hz / 100u) % 10000u);
    }
    if (r.measured)
        printf("  measured %u.%04u MHz, alignment %u.%03u\n",
               r.m.dot_hz / 1000000u, (r.m.dot_hz / 100u) % 10000u,
               (uint)r.m.align, (uint)(r.m.align * 1000.0f) % 1000u);

    switch (r.status) {
    case DETECT_UNKNOWN:
        printf("detect: no known source at this line rate\n");
        break;
    case DETECT_NO_EDGES:
        printf("detect: can't measure the dot clock (%u edges); would keep %s\n",
               r.m.edges, source_active()->name);
        break;
    case DETECT_UNCLEAR:
        printf("detect: the edges don't line up at any candidate's clock; would keep %s\n",
               source_active()->name);
        break;
    case DETECT_NO_MATCH:
        printf("detect: no candidate close enough; would keep %s\n", source_active()->name);
        break;
    default: {
        const source_cand_t *c = &r.cand[r.pick];
        printf("detect: %s (%u-bit)%s\n", c->mode->name, c->mode->data_bits,
               c->index == source_active_index() ? ", already selected" : ", would switch");
    }
    }
}

// --- settings slots ---
static bool slot_arg(int argc, char **argv, uint *n) {
    char *end;
    long v = argc >= 2 ? strtol(argv[1], &end, 10) : -1;
    if (argc < 2 || *end || v < 0 || v >= SETTINGS_SLOTS) {
        printf("slot 0..%u\n", SETTINGS_SLOTS - 1);
        return false;
    }
    *n = (uint)v;
    return true;
}

void cmd_slots(int argc, char **argv) {
    (void)argc; (void)argv;
    const int def = settings_default();
    if (def >= 0) printf("  %s slots (default: %d):\n", source_group_name(source_group()), def);
    else          printf("  %s slots (no default):\n", source_group_name(source_group()));
    for (uint n = 0; n < SETTINGS_SLOTS; n++) {
        const settings_slot_t *t = settings_slot(n);
        if (!t) { printf("  %u  -\n", n); continue; }
        printf("  %u%c %-8.8s %s, bp %d, phase %u, dot %lu.%04lu, vscale %u, vpos %d, hpos %d,"
               " scanlines %s, levels %u/%u\n",
               n, (int)n == def ? '*' : ' ', t->name, video_mode_name(t->mode),
               t->bp, t->phase, (unsigned long)(t->dot_hz / 1000000u),
               (unsigned long)((t->dot_hz / 100u) % 10000u), t->vscale, t->vpos, t->hpos,
               t->scanlines ? "on" : "off", t->lvl_normal, t->lvl_bright);
    }
    if (def >= 0) printf("  * = default, applied at boot\n");
}

void cmd_save(int argc, char **argv) {
    uint n;
    if (slot_arg(argc, argv, &n)) settings_save(n, argc >= 3 ? argv[2] : NULL);
}

void cmd_load(int argc, char **argv) {
    uint n;
    if (!slot_arg(argc, argv, &n)) return;
    if (!settings_load(n)) printf("slot %u is empty\n", n);
    else                   printf("loaded slot %u\n", n);
}

void cmd_clear(int argc, char **argv) {
    uint n;
    if (slot_arg(argc, argv, &n)) settings_clear(n);
}

void cmd_default(int argc, char **argv) {
    uint n;
    if (argc < 2) {
        const int def = settings_default();
        if (def < 0) printf("default: none\n");
        else         printf("default: slot %d (%.8s)\n", def, settings_slot((uint)def)->name);
        return;
    }
    if (!strcmp(argv[1], "off")) { settings_set_default(-1); return; }
    if (!slot_arg(argc, argv, &n)) return;
    if (!settings_slot(n)) { printf("slot %u is empty\n", n); return; }
    settings_set_default((int)n);
}

void cmd_fastcap(int argc, char **argv) {
    capture_dump_fast(argc >= 2 ? (uint)atoi(argv[1]) : 80u);
}

void cmd_scanlines(int argc, char **argv) {
    if (argc >= 2) {
        if (!strcmp(argv[1], "on"))       video_set_scanlines(true);
        else if (!strcmp(argv[1], "off")) video_set_scanlines(false);
        else if (!strcmp(argv[1], "switch")) video_set_scanlines(!video_get_scanlines());
        else printf("scanlines on|off|switch\n");
        osd_knob("scanlines %s", video_get_scanlines() ? "on" : "off");
    }
    printf("scanlines = %s\n", video_get_scanlines() ? "on" : "off");
}

void cmd_capture_stat(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!capture_grab()) {
        printf("capture: no sync\n");
        return;
    }
    printf("W %u H %u\n", capture_width(), capture_height());
}

void cmd_capture(int argc, char **argv) {
    (void)argc; (void)argv;
    capture_dump_frame();

}

void cmd_bp(int argc, char **argv) {
    if (argc >= 2) {
        capture_set_bp(atoi(argv[1]));
        osd_knob("bp %d", capture_get_bp());
    }
    printf("bp = %d px\n", capture_get_bp());
}

void cmd_phase(int argc, char **argv) {
    if (argc >= 2) {
        capture_set_phase(atoi(argv[1]));
        osd_knob("phase %d/%u", capture_get_phase(), capture_px_cyc());
    }
    printf("phase = %d/%u px\n", capture_get_phase(), capture_px_cyc());
}

void cmd_test(int argc, char **argv) {
    (void)argc; (void)argv;
    video_test_pattern_stripes();
}
