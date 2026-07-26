#include <string.h>
#include "pico/stdlib.h"
#include "capture.h"
#include "video.h"
#include "view.h"
#include "source.h"

// The source is centred on the card's NOMINAL active width (g_src->active_w),
// not the live capture_width (which wobbles a pixel or two per grab), so the
// image never jitters horizontally as the measured width changes.

// 2-bit source value (VIDEO | INTENSITY<<1) -> display grayscale level 0..3:
//   VIDEO off        -> 0 (black)
//   VIDEO on         -> 2 (normal)
//   VIDEO+INTENSITY  -> 3 (bright)
static inline uint level_of(uint v)
{
    if (!(v & 1u)) return 0;
    return (v & 2u) ? 3u : 2u;
}

void view_render(void)
{
    uint32_t *fb       = video_framebuffer();
    const uint fb_w    = video_fb_width();     // 736
    const uint fb_h    = video_fb_height();    // 480
    const uint fb_words = video_fb_words();    // 46
    const uint src_w   = capture_width();      // ~720
    const uint src_h   = capture_height();     // ~369

    // Fixed borders: centre the card's nominal-width image in fb_w, and the
    // src_h lines in fb_h (letterbox). Using the nominal active_w (constant)
    // rather than the live src_w keeps it jump-free as the measured width varies.
    const int img_w    = (int)g_src->active_w;           // 720 (MDA)
    const int h_border = ((int)fb_w - img_w) / 2;        // 8
    const int v_border = ((int)fb_h - (int)src_h) / 2;   // ~55

    static uint8_t src_row[CAPTURE_MAX_WIDTH];

    for (uint r = 0; r < fb_h; r++) {
        uint32_t *fb_line = &fb[r * fb_words];

        int line = (int)r - v_border;
        if (line < 0 || line >= (int)src_h) {
            memset(fb_line, 0, fb_words * 4);            // letterbox: black row
            continue;
        }
        capture_get_line((uint)line, src_row);

        // Pack 16 pixels per word (2 bits each), LSB = leftmost.
        for (uint w = 0; w < fb_words; w++) {
            uint32_t word = 0;
            uint base = w * 16;
            for (uint i = 0; i < 16; i++) {
                int k = (int)(base + i) - h_border;      // source pixel
                uint lvl = 0;
                if (k >= 0 && k < (int)src_w) lvl = level_of(src_row[k]);
                word |= lvl << (2 * i);
            }
            fb_line[w] = word;
        }
    }
}
