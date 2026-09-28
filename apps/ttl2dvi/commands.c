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
    uint32_t pv = sync_vsync_period();

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
}

void cmd_source(int argc, char **argv) {
    if (argc >= 2) {
        char *end;
        long i = strtol(argv[1], &end, 10);
        if (*end || i < 0 || (uint)i >= source_count()) {
            printf("no such source: %s\n", argv[1]);
        } else {
            printf("switching to %s (reboot)...\n", source_get((uint)i)->name);
            sleep_ms(50);
            source_select((uint)i);
        }
    }
    for (uint i = 0; i < source_count(); i++) {
        const source_mode_t *s = source_get(i);
        printf("  %u  %-6s %u data bits @ GP%u, %u px, %u.%03u MHz dot%s, sysclk %u",
               i, s->name, s->data_bits, s->data_base, s->active_w,
               s->dot_hz / 1000000u, (s->dot_hz / 1000u) % 1000u,
               i == source_active_index() && capture_oversample() > 1 ? " (2x)" : "",
               s->sysclk_khz / 1000u);
        if (s->sysclk_khz % 1000u) printf(".%u", (s->sysclk_khz % 1000u) / 100u);
        printf(" MHz%s%s\n", s->hsync_hz ? ", auto" : "",
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
    printf("mdalevels normal=%u bright=%u\n", view_get_mda_normal(), view_get_mda_bright());
}

void cmd_vscale(int argc, char **argv) {
    if (argc >= 2) view_set_vscale(atoi(argv[1]));
    printf("vscale = %ux\n", view_get_vscale());
}

void cmd_vpos(int argc, char **argv) {
    if (argc >= 2) view_set_vpos(atoi(argv[1]));
    printf("vpos = %d source lines\n", view_get_vpos());
}

void cmd_hpos(int argc, char **argv) {
    if (argc >= 2) view_set_hpos(atoi(argv[1]));
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

void cmd_fastcap(int argc, char **argv) {
    capture_dump_fast(argc >= 2 ? (uint)atoi(argv[1]) : 80u);
}

void cmd_scanlines(int argc, char **argv) {
    if (argc >= 2) {
        if (!strcmp(argv[1], "on"))       video_set_scanlines(true);
        else if (!strcmp(argv[1], "off")) video_set_scanlines(false);
        else printf("scanlines on|off\n");
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
    if (argc >= 2) capture_set_bp(atoi(argv[1]));
    printf("bp = %d px\n", capture_get_bp());
}

void cmd_phase(int argc, char **argv) {
    if (argc >= 2) capture_set_phase(atoi(argv[1]));
    printf("phase = %d/%u px\n", capture_get_phase(), capture_px_cyc());
}

void cmd_test(int argc, char **argv) {
    (void)argc; (void)argv;
    video_test_pattern_stripes();
}
