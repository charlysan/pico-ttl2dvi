#ifndef TTL2DVI_VIDEO_H
#define TTL2DVI_VIDEO_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

// After the overclock, before sync_init/capture_init.
void video_init(void);

// Output modes. Switching reboots; the choice survives in watchdog scratch[3].
uint video_mode_count(void);
uint video_mode_current(void);
const char *video_mode_name(uint i);
void video_set_mode(uint i);
void video_mode_store(uint i);      // as video_set_mode, without the reboot
// Mode to use at the next video_init() when no mode was stored since the last
// power cycle. Call before video_init().
void video_preselect_mode(uint i);

// LSB = leftmost pixel. Depth is per mode:
//   2bpp: 0..3 = the four fixed greys in tmds_encode_2bpp (MDA)
//   8bpp: RGB222, R 5:4, G 3:2, B 1:0 (CGA, C128)
// The framebuffer holds up to video_fb_rows() stored rows, one per source line.
// core0 writes while core1 reads: no double buffer (for now)
uint32_t *video_fb(void);
uint video_fb_width(void);
uint video_fb_rows(void);
uint video_fb_words(void);
uint video_fb_bpp(void);
uint video_lines(void);             // display lines (the mode's v_active_lines)

// Map display lines to stored rows: line y shows row (y - first) / rep for the
// n * rep lines from first, black elsewhere. rep is the vertical scale.
void video_set_vmap(uint first, uint rep, uint n);

// Scanlines: when rows repeat (rep > 1), the last repeat of each is black.
// Live, no reboot.
void video_set_scanlines(bool on);
bool video_get_scanlines(void);

// One line of text near the bottom of the screen, for ms milliseconds
// (0 = until hidden). Live, no reboot.
void video_osd_show(const char *s, uint ms);
void video_osd_hide(void);

// RGBI sample (I | R<<1 | G<<2 | B<<3) -> RGB222 byte, 16 entries.
// Used by every 4-bit RGBI source (CGA, C128).
const uint8_t *video_cga_rgb222(void);

// EGA 6-bit sample (sB | sG<<1 | R<<2 | G<<3 | B<<4 | sR<<5) -> RGB222, 64 entries.
const uint8_t *video_ega_rgb222(void);

void video_clear(void);
void video_test_pattern_stripes(void);

#endif
