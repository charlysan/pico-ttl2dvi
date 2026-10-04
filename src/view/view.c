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

void view_init(void)         { g_vscale = source_active()->def_vscale; }
void view_set_vscale(int n)  { g_vscale = n < 1 ? 1 : (n > 4 ? 4 : (uint)n); }
uint view_get_vscale(void)   { return g_vscale; }
void view_set_vpos(int n)    { g_vpos = n; }
int  view_get_vpos(void)     { return g_vpos; }
void view_set_hpos(int n)    { g_hpos = n; }
int  view_get_hpos(void)     { return g_hpos; }

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The general resampler, one fb word: pixel k takes sample
// floor((k + 0.5) * spp), the one at the pixel's centre, black outside the
// captured width.
typedef struct {
    const uint8_t *map;
    uint32_t spp, mask;
    uint ppw, bpp, bits, spw, sft, shift, src_w;
    int h_border;
} resample_t;

static inline uint32_t __not_in_flash_func(resample_word)(const resample_t *r,
                                                          const uint32_t *raw, uint w)
{
    uint32_t word = 0;
    for (uint j = 0; j < r->ppw; j++) {
        const int k = (int)(w * r->ppw + j) - r->h_border;
        uint v = 0;
        if (k >= 0 && k < (int)r->src_w) {
            const uint s = ((2u * (uint)k + 1u) * r->spp) >> 17;
            const uint32_t d = r->sft ? raw[s >> r->sft] : raw[s / r->spw];
            const uint off   = r->sft ? (s & (r->spw - 1u)) : (s % r->spw);
            v = r->map[(d >> (r->shift + off * r->bits)) & r->mask];
        }
        word |= v << (r->bpp * j);
    }
    return word;
}

void __not_in_flash_func(view_render)(void)
{
    const uint src_w = capture_width();
    const uint src_h = capture_height();
    if (!src_w || !src_h) return;

    const uint bits = source_active()->data_bits;       // 2 MDA, 4 RGBI, 6 EGA
    const bool mono = bits == 2;
    if (mono && !blut_ready)       build_blut();
    if (bits == 4 && !clut_ready)  build_clut();

    uint32_t  *fb       = video_fb();
    const uint fb_w     = video_fb_width();
    const uint fb_words = video_fb_words();
    const uint bpp      = video_fb_bpp();
    const uint ppw      = 32u / bpp;                    // fb pixels per word
    const uint8_t *map  = mono ? level : bits == 4 ? video_cga_rgb222()
                                                   : video_ega_rgb222();
    const int  lines    = (int)video_lines();
    const int  vs       = (int)g_vscale;

    // Centred on the card's active width, not the measured one: that one
    // includes blanking and moves with bp.
    // Negative border = source bigger than the canvas = crop.
    const int h_border = ((int)fb_w - (int)source_active()->active_w) / 2 + g_hpos;

    // One stored row per source line; core1 repeats each vs times and blacks
    // out the rest. If not every line can be stored, vpos picks which ones and
    // the image stays centred; otherwise vpos moves the image.
    int n = (int)src_h;
    if (n > lines / vs)             n = lines / vs;
    if (n > (int)video_fb_rows())   n = (int)video_fb_rows();
    const int row_slack  = lines - n * vs;
    const int line_slack = (int)src_h - n;
    int first_row  = row_slack / 2;
    int first_line = 0;
    if (line_slack > 0)
        first_line = clampi(line_slack / 2 - g_vpos, 0, line_slack);
    else
        first_row  = clampi(row_slack / 2 + g_vpos * vs, 0, row_slack);

    // The byte paths take sample k as pixel k, so they need one sample per
    // pixel. At 2x only the resampling loop below applies.
    const uint samples = capture_samples();
    const bool one     = capture_oversample() == 1;
    const uint spb     = 8u / bits;                      // source pixels per byte
    const bool fast    = one && bits != 6 && h_border <= 0
                         && ((-h_border) % (int)spb) == 0
                         && (uint)(-h_border) + fb_w <= samples;
    const bool word5   = one && bits == 6 && h_border == 0 && fb_w <= samples
                         && fb_words % 5 == 0;

    const uint     spw   = 32u / bits;
    const uint     shift = 32u - spw * bits;             // 2 at 6 bits, else 0
    const uint32_t mask  = (1u << bits) - 1u;
    const uint32_t spp   = capture_spp();
    const uint     sft   = bits == 6 ? 0u : (uint)__builtin_ctz(spw);

    // Oversampled RGBI into 8bpp with the whole row inside the capture: the
    // resampling loop without its per-pixel multiply, bounds check and word
    // packing. It must beat the DMA refilling rawbuf behind it (one line per
    // ~63 us at 15.8 kHz), or the bottom rows come from the next capture.
    const bool res8 = !one && bits == 4 && bpp == 8 && h_border <= 0
                      && (uint)(-h_border) + fb_w <= src_w;

    // Oversampled MDA into 2bpp (a card off MDA16's 16.000 MHz, e.g. the OTI
    // at 17.75): res8's running sample position, 16 pixels packed per word.
    // Unlike res8 the picture needn't fill the row (720 in 736 has an 8 px
    // border): words wholly inside it take the fast loop, the one or two at
    // its edges the general one.
    const bool res2 = !one && mono && bpp == 2;
    const int  x0   = h_border > 0 ? h_border : 0;
    const int  x1   = h_border + (int)src_w < (int)fb_w ? h_border + (int)src_w : (int)fb_w;

    const resample_t rs = {
        .map = map, .spp = spp, .mask = mask, .ppw = ppw, .bpp = bpp, .bits = bits,
        .spw = spw, .sft = sft, .shift = shift, .src_w = src_w, .h_border = h_border,
    };

    for (int i = 0; i < n; i++) {
        uint32_t  *fb_line = &fb[(uint)i * fb_words];
        const uint line    = (uint)(first_line + i);
        if (res2) {
            const uint32_t *raw = (const uint32_t *)capture_raw_line(line);
            for (uint w = 0; w < fb_words; w++) {
                const int xs = (int)(w * 16u);
                if (xs < x0 || xs + 16 > x1) {
                    fb_line[w] = resample_word(&rs, raw, w);
                    continue;
                }
                uint32_t acc  = spp / 2u + (uint32_t)(xs - h_border) * spp;
                uint32_t word = 0;
                for (uint j = 0; j < 32u; j += 2u, acc += spp) {
                    const uint s = acc >> 16;
                    word |= (uint32_t)map[(raw[s >> 4] >> ((s & 15u) << 1)) & 3u] << j;
                }
                fb_line[w] = word;
            }
            continue;
        }
        if (res8) {
            const uint32_t *raw = (const uint32_t *)capture_raw_line(line);
            uint8_t *out = (uint8_t *)fb_line;
            uint32_t acc = spp / 2 + (uint32_t)(-h_border) * spp;
            for (uint x = 0; x < fb_w; x++, acc += spp) {
                const uint s = acc >> 16;
                out[x] = map[(raw[s >> 3] >> ((s & 7u) << 2)) & 15u];
            }
            continue;
        }
        if (word5) {
            // 5 samples per source word at bits 2, 8, 14, 20, 26; 4 pixels per
            // fb word. 4 source words fill 5 fb words.
            const uint32_t *p = (const uint32_t *)capture_raw_line(line);
            #define S(d, sh) ((uint32_t)map[((d) >> (sh)) & 63u])
            for (uint b = 0; b < fb_words; b += 5, p += 4) {
                const uint32_t d0 = p[0], d1 = p[1], d2 = p[2], d3 = p[3];
                fb_line[b + 0] = S(d0, 2)  | S(d0, 8)  << 8 | S(d0, 14) << 16 | S(d0, 20) << 24;
                fb_line[b + 1] = S(d0, 26) | S(d1, 2)  << 8 | S(d1, 8)  << 16 | S(d1, 14) << 24;
                fb_line[b + 2] = S(d1, 20) | S(d1, 26) << 8 | S(d2, 2)  << 16 | S(d2, 8)  << 24;
                fb_line[b + 3] = S(d2, 14) | S(d2, 20) << 8 | S(d2, 26) << 16 | S(d3, 2)  << 24;
                fb_line[b + 4] = S(d3, 8)  | S(d3, 14) << 8 | S(d3, 20) << 16 | S(d3, 26) << 24;
            }
            #undef S
            continue;
        }
        if (fast) {
            const uint8_t *b = capture_raw_line(line) + (uint)(-h_border) / spb;
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

        // Resample. At 1x pixel k is sample k.
        const uint32_t *raw = (const uint32_t *)capture_raw_line(line);
        for (uint w = 0; w < fb_words; w++)
            fb_line[w] = resample_word(&rs, raw, w);
    }

    video_set_vmap((uint)first_row, (uint)vs, (uint)n);
}
