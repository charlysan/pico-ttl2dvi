#ifndef TTL2DVI_VIDEO_H
#define TTL2DVI_VIDEO_H

#include <stdint.h>
#include "pico/types.h"     // uint

// After the overclock, before sync_init/capture_init.
void video_init(void);

// Output modes. Switching reboots; the choice survives in watchdog scratch[3].
uint video_mode_count(void);
uint video_mode_current(void);
const char *video_mode_name(uint i);
void video_set_mode(uint i);

// LSB = leftmost pixel. Depth is per mode:
//   2bpp: 0..3 = the four fixed greys in tmds_encode_2bpp (MDA)
//   8bpp: RGB222, R 5:4, G 3:2, B 1:0 (CGA)
// Height is framebuffer rows; core1 shows each row on several display lines
// when the mode repeats rows.
// core0 writes while core1 reads: no double buffer (for now)
uint32_t *video_fb(void);
uint video_fb_width(void);
uint video_fb_height(void);
uint video_fb_words(void);
uint video_fb_bpp(void);

// RGBI sample (I | R<<1 | G<<2 | B<<3) -> RGB222 byte, 16 entries.
// Used by every 4-bit RGBI source (CGA, C128).
const uint8_t *video_cga_rgb222(void);

void video_clear(void);
void video_test_pattern_stripes(void);

#endif
