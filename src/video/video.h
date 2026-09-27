#ifndef TTL2DVI_VIDEO_H
#define TTL2DVI_VIDEO_H

#include <stdint.h>
#include "pico/types.h"     // uint

#define VIDEO_FB_BPP    2u
#define VIDEO_FB_PPW    (32u / VIDEO_FB_BPP)

// After the overclock, before sync_init/capture_init.
void video_init(void);

// Output modes. Switching reboots; the choice survives in watchdog scratch[3].
uint video_mode_count(void);
uint video_mode_current(void);
const char *video_mode_name(uint i);
void video_set_mode(uint i);

// 2bpp, LSB = leftmost pixel, 0..3 = the four fixed greys in tmds_encode_2bpp.
// core0 writes while core1 reads: no double buffer (for now)
uint32_t *video_fb(void);
uint video_fb_width(void);
uint video_fb_height(void);
uint video_fb_words(void);

void video_clear(void);
void video_test_pattern_stripes(void);

#endif
