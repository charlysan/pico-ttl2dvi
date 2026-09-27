#include <string.h>
#include "pico/stdlib.h"
#include "capture.h"
#include "video.h"
#include "view.h"
#include "source.h"

// Sample (VIDEO | INTENSITY<<1) -> grey index. 2 is INTENSITY without VIDEO,
// which the card does not emit.
static uint8_t level[4] = { 0, 2, 0, 3 };

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

void view_set_mda_levels(uint normal, uint bright)
{
    level[1] = (uint8_t)(normal & 3u);
    level[3] = (uint8_t)(bright & 3u);
    blut_ready = false;
}

uint view_get_mda_normal(void) { return level[1]; }
uint view_get_mda_bright(void) { return level[3]; }

static uint g_vscale = 1;
static int  g_vpos, g_hpos;

void view_set_vscale(int n)  { g_vscale = n < 1 ? 1 : (n > 4 ? 4 : (uint)n); }
uint view_get_vscale(void)   { return g_vscale; }
void view_set_vpos(int n)    { g_vpos = n; }
int  view_get_vpos(void)     { return g_vpos; }
void view_set_hpos(int n)    { g_hpos = n; }
int  view_get_hpos(void)     { return g_hpos; }

void __not_in_flash_func(view_render)(void)
{
    const uint src_w = capture_width();
    const uint src_h = capture_height();
    if (!src_w || !src_h) return;
    if (!blut_ready) build_blut();

    uint32_t  *fb       = video_fb();
    const uint fb_w     = video_fb_width();
    const uint fb_h     = video_fb_height();
    const uint fb_words = video_fb_words();
    const int  vs       = (int)g_vscale;

    // Centred on the card's active width, not the measured one: that one
    // includes blanking and moves with bp.
    // Negative border = source bigger than the canvas = crop.
    const int h_border = ((int)fb_w - (int)source_active()->active_w) / 2 + g_hpos;
    const int v_border = ((int)fb_h - (int)src_h * vs) / 2 + g_vpos * vs;

    const bool fast = h_border <= 0 && ((-h_border) % 4) == 0
                      && (uint)(-h_border) + fb_w <= src_w;

    static uint8_t src_row[CAPTURE_MAX_WIDTH];
    int prev = -1;

    for (uint r = 0; r < fb_h; r++) {
        uint32_t *fb_line = &fb[r * fb_words];
        const int d    = (int)r - v_border;
        const int line = d < 0 ? -1 : d / vs;

        if (line < 0 || line >= (int)src_h) {
            memset(fb_line, 0, fb_words * 4);
            prev = -1;
            continue;
        }
        if (line == prev) {
            memcpy(fb_line, fb_line - fb_words, fb_words * 4);
            continue;
        }
        prev = line;
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
