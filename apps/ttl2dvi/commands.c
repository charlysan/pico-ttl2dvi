#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "commands.h"
#include "version.h"
#include "sync.h"
#include "capture.h"
#include "video.h"

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
