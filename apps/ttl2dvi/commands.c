#include <stdlib.h>
#include <stdio.h>
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

    printf("  SYSCLK: %lu.%03lu MHz\n",
           (unsigned long)(f / 1000000),
           (unsigned long)((f / 1000) % 1000));
    if (ph) {
        uint32_t h = (uint32_t)((uint64_t)f * 100 / ph);
        printf("  HSYNC: %lu.%02lu Hz\n",
               (unsigned long)(h / 100), (unsigned long)(h % 100));
        printf("  PULSE: %lu cyc = %lu px + %lu/%u\n",
               (unsigned long)pulse,
               (unsigned long)(pulse / SAMPLE_CYC),
               (unsigned long)(pulse % SAMPLE_CYC), (unsigned)SAMPLE_CYC);
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
    for (uint i = 0; i < source_count(); i++)
        printf("  %u  %s%s\n", i, source_get(i)->name,
               i == source_active_index() ? "   <- current" : "");
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
    printf("phase = %d/%u px\n", capture_get_phase(), SAMPLE_CYC);
}

void cmd_test(int argc, char **argv) {
    (void)argc; (void)argv;
    video_test_pattern_stripes();
}
