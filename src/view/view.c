#include <string.h>
#include "pico/stdlib.h"
#include "capture.h"
#include "video.h"
#include "view.h"
#include "source.h"

// The source is centred on the card's NOMINAL active width (g_src->active_w),
// not the live capture_width (which wobbles a pixel or two per grab), so the
// image never jitters horizontally as the measured width changes.

static uint g_lvl_normal = 2;      // VIDEO alone  → normal
static uint g_lvl_bright = 3;      // VIDEO+INTENSITY → bright

// Vertical scale: each source line drawn g_vscale times. 1 = the 1:1 default.
static uint g_vscale = 1;
static int  g_vpos   = 0;

// Horizontal position, in SOURCE pixels, named by IMAGE motion like g_vpos:
// positive moves the picture RIGHT. Display-side only -- it slides the already
// captured image inside the framebuffer and never touches sampling, unlike `bp`
// (capture-side) which moves the sampling window itself. Use `bp` to find the
// active picture, `hpos` to place it.
static int  g_hpos   = 0;

static uint clamp_lvl(int val)
{
    if (val < 0) return 0;
    if (val > 3) return 3;
    return (uint)val;
}

void view_set_mda_lvl_normal(int val) { g_lvl_normal = clamp_lvl(val); }
int  view_get_mda_lvl_normal(void)    { return (int)g_lvl_normal; }
void view_set_mda_lvl_bright(int val) { g_lvl_bright = clamp_lvl(val); }
int  view_get_mda_lvl_bright(void)    { return (int)g_lvl_bright; }

void view_set_vscale(int val)
{
    if (val < 1) val = 1;
    if (val > 4) val = 4;             // >2 crops hard; allowed, not useful
    g_vscale = (uint)val;
}
int  view_get_vscale(void) { return (int)g_vscale; }

void view_set_vpos(int val) { g_vpos = val; }
int  view_get_vpos(void)    { return g_vpos; }

void view_set_hpos(int val) { g_hpos = val; }
int  view_get_hpos(void)    { return g_hpos; }

// Reconstructed sample value -> framebuffer index.
//
//   COLOUR (CGA640): the raw 4-bit RGBI value IS the palette index, straight
//   through -- video.c built the palette in that order (I | R<<1 | G<<2 | B<<3).
//   MONO (MDA): VIDEO off -> 0 (black; INTENSITY-alone ignored), VIDEO on ->
//   g_lvl_normal, VIDEO+INTENSITY -> g_lvl_bright, all indices into the fixed
//   4-level grayscale of tmds_encode_2bpp.
static inline uint index_of(uint v)
{
    if (g_src->color) return v & 15u;
    if (!(v & 1u)) return 0;
    return (v & 2u) ? g_lvl_bright : g_lvl_normal;
}

void view_render(void)
{
    uint32_t *fb       = video_framebuffer();
    const uint fb_w    = video_fb_width();     // 736 MDA / 640 CGA640
    const uint fb_h    = video_fb_height();    // ENCODED lines: 480 / 240
    const uint fb_words = video_fb_words();    // 46 (2bpp) / 80 (4bpp)
    const uint bpp     = video_fb_bpp();       // 2 (MDA gray) or 4 (CGA index)
    const uint ppw     = 32u / bpp;            // pixels per framebuffer word
    const uint src_w   = capture_width();      // ~720 / ~640
    const uint src_h   = capture_height();     // ~369 / ~262

    // Fixed borders: centre the card's nominal-width image in fb_w, and the
    // scaled src_h lines in fb_h. Using the nominal active_w (constant) rather
    // than the live src_w keeps it jump-free as the measured width varies.
    // v_border goes NEGATIVE once src_h*vscale exceeds fb_h - that is the crop
    // case, and the per-row range check below handles it unchanged.
    // h_border folds g_hpos in, so the pan costs nothing per pixel.
    const int img_w    = (int)g_src->active_w;                        // 720 (MDA)
    const int h_border = ((int)fb_w - img_w) / 2 + g_hpos;            // 8 at hpos 0
    const int vs       = (int)g_vscale;
    const int v_border = ((int)fb_h - (int)src_h * vs) / 2;           // ~55 at 1x

    static uint8_t src_row[CAPTURE_MAX_WIDTH];

    // At vscale > 1 consecutive rows repeat the same source line: pack it once,
    // then copy the packed words. Keeps the per-pixel cost flat as scale rises.
    int             prev_line = -1;
    const uint32_t *prev_fb   = NULL;

    for (uint r = 0; r < fb_h; r++) {
        uint32_t *fb_line = &fb[r * fb_words];

        // Floor-divide: r - v_border can be negative when the image is cropped
        // or panned, and C truncates toward zero, which would fold two distinct
        // source lines onto 0 and mirror the top row.
        int rel  = (int)r - v_border;
        int line = ((rel >= 0) ? (rel / vs) : -(((-rel) + vs - 1) / vs)) - g_vpos;

        if (line < 0 || line >= (int)src_h) {
            memset(fb_line, 0, fb_words * 4);            // letterbox: black row
            prev_line = -1;
            continue;
        }
        if (line == prev_line && prev_fb) {              // repeated line: copy
            memcpy(fb_line, prev_fb, fb_words * 4);
            continue;
        }
        capture_get_line((uint)line, src_row);

        // Pack ppw pixels per word (bpp bits each), LSB = leftmost.
        // 16 x 2bpp for MDA, 8 x 4bpp for CGA640.
        for (uint w = 0; w < fb_words; w++) {
            uint32_t word = 0;
            uint base = w * ppw;
            for (uint i = 0; i < ppw; i++) {
                int k = (int)(base + i) - h_border;      // source pixel
                uint idx = 0;
                if (k >= 0 && k < (int)src_w) idx = index_of(src_row[k]);
                word |= idx << (bpp * i);
            }
            fb_line[w] = word;
        }
        prev_line = line;
        prev_fb   = fb_line;
    }
}
