#include <string.h>
#include "pico/stdlib.h"
#include "capture.h"
#include "video.h"
#include "view.h"
#include "source.h"

// Sample (VIDEO | INTENSITY<<1) -> grey index. 2 is INTENSITY without VIDEO,
// which the card does not emit.
static uint8_t level[4] = { 0, 2, 0, 3 };

// Byte lookups for the fast path, one sample per pixel, low bits = left pixel.
// MDA: a source byte is 4 pixels at 2 bits -> one 2bpp framebuffer byte.
// CGA: a source byte is 2 pixels at 4 bits -> two RGB222 bytes.
static uint8_t  blut[256];
static uint16_t clut[256];
static bool blut_ready, clut_ready;

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

static void build_clut(void)
{
    const uint8_t *rgb = video_cga_rgb222();
    for (uint b = 0; b < 256; b++)
        clut[b] = (uint16_t)(rgb[b & 15] | rgb[b >> 4] << 8);
    clut_ready = true;
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

    const bool mono = video_fb_bpp() == 2;
    if (mono && !blut_ready)  build_blut();
    if (!mono && !clut_ready) build_clut();

    uint32_t  *fb       = video_fb();
    const uint fb_w     = video_fb_width();
    const uint fb_h     = video_fb_height();
    const uint fb_words = video_fb_words();
    const uint bpp      = video_fb_bpp();
    const uint ppw      = 32u / bpp;                    // fb pixels per word
    const uint spb      = mono ? 4u : 2u;               // source pixels per byte
    const uint8_t *map  = mono ? level : video_cga_rgb222();
    const int  vs       = (int)g_vscale;

    // Centred on the card's active width, not the measured one: that one
    // includes blanking and moves with bp.
    // Negative border = source bigger than the canvas = crop.
    const int h_border = ((int)fb_w - (int)source_active()->active_w) / 2 + g_hpos;
    const int v_border = ((int)fb_h - (int)src_h * vs) / 2 + g_vpos * vs;

    const bool fast = h_border <= 0 && ((-h_border) % (int)spb) == 0
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
            const uint8_t *b = capture_raw_line((uint)line) + (uint)(-h_border) / spb;
            if (mono) {
                for (uint w = 0; w < fb_words; w++, b += 4)
                    fb_line[w] = (uint32_t)blut[b[0]]
                               | (uint32_t)blut[b[1]] << 8
                               | (uint32_t)blut[b[2]] << 16
                               | (uint32_t)blut[b[3]] << 24;
            } else {
                for (uint w = 0; w < fb_words; w++, b += 2)
                    fb_line[w] = (uint32_t)clut[b[0]]
                               | (uint32_t)clut[b[1]] << 16;
            }
            continue;
        }

        capture_get_line((uint)line, src_row);

        for (uint w = 0; w < fb_words; w++) {
            uint32_t word = 0;
            for (uint i = 0; i < ppw; i++) {
                const int k = (int)(w * ppw + i) - h_border;
                const uint v = (k >= 0 && k < (int)src_w) ? map[src_row[k]] : 0;
                word |= v << (bpp * i);
            }
            fb_line[w] = word;
        }
    }
}
