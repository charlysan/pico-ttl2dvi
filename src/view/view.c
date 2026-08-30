#include <string.h>
#include "pico/stdlib.h"
#include "capture.h"
#include "video.h"
#include "view.h"

// Sample (VIDEO | INTENSITY<<1) -> grey index. 2 is INTENSITY without VIDEO,
// which the card does not emit.
static const uint8_t level[4] = { 0, 2, 0, 3 };

// 2bpp both sides and one sample per pixel, so a source byte is 4 pixels is a
// framebuffer byte.
static uint8_t blut[256];
static bool blut_ready;

static void build_blut(void)
{
    for (uint b = 0; b < 256; b++) {
        uint v = 0;
        for (uint i = 0; i < 4; i++)
            v |= (uint)level[(b >> (2 * i)) & 3] << (2 * i);
        blut[b] = (uint8_t)v;
    }
    blut_ready = true;
}

void __not_in_flash_func(view_render)(void)
{
    const uint src_w = capture_width();
    const uint src_h = capture_height();
    if (!src_w || !src_h) return;
    if (!blut_ready) build_blut();

    uint32_t  *fb       = video_fb();
    const uint fb_h     = video_fb_height();
    const uint fb_words = video_fb_words();

    // Negative border = source bigger than the canvas = crop.
    const int h_border = ((int)video_fb_width() - (int)src_w) / 2;
    const int v_border = ((int)fb_h - (int)src_h) / 2;

    const bool fast = h_border <= 0 && ((-h_border) % 4) == 0
                      && src_w >= video_fb_width();

    static uint8_t src_row[CAPTURE_MAX_WIDTH];

    for (uint r = 0; r < fb_h; r++) {
        uint32_t *fb_line = &fb[r * fb_words];
        const int line = (int)r - v_border;

        if (line < 0 || line >= (int)src_h) {
            memset(fb_line, 0, fb_words * 4);
            continue;
        }
        if (fast) {
            const uint8_t *b = capture_raw_line((uint)line) + (uint)(-h_border) / 4;
            for (uint w = 0; w < fb_words; w++, b += 4)
                fb_line[w] = (uint32_t)blut[b[0]]
                           | (uint32_t)blut[b[1]] << 8
                           | (uint32_t)blut[b[2]] << 16
                           | (uint32_t)blut[b[3]] << 24;
            continue;
        }

        capture_get_line((uint)line, src_row);

        for (uint w = 0; w < fb_words; w++) {
            uint32_t word = 0;
            for (uint i = 0; i < VIDEO_FB_PPW; i++) {
                const int k = (int)(w * VIDEO_FB_PPW + i) - h_border;
                const uint idx = (k >= 0 && k < (int)src_w) ? level[src_row[k]] : 0;
                word |= idx << (VIDEO_FB_BPP * i);
            }
            fb_line[w] = word;
        }
    }
}
