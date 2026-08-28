#ifndef TTL2DVI_VIDEO_H
#define TTL2DVI_VIDEO_H

#include <stdint.h>
#include "pico/types.h"     // uint

// 640x480@60: 640 = 20 x 32 - tmds_encode_2bpp encodes 32 px per pass.
#define VIDEO_FB_W      640u
#define VIDEO_FB_H      480u
#define VIDEO_FB_BPP    2u
#define VIDEO_FB_PPW    (32u / VIDEO_FB_BPP)
#define VIDEO_FB_WORDS  (VIDEO_FB_W / VIDEO_FB_PPW)

// After the overclock, before sync_init/capture_init.
void video_init(void);

// 2bpp, LSB = leftmost pixel, 0..3 = the four fixed greys in tmds_encode_2bpp.
// core0 writes while core1 reads: no double buffer (for now)
uint32_t *video_fb(void);
uint video_fb_width(void);
uint video_fb_height(void);
uint video_fb_words(void);

void video_clear(void);
void video_test_pattern_stripes(void);

#endif
