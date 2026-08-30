#ifndef TTL2DVI_VIDEO_H
#define TTL2DVI_VIDEO_H

#include <stdint.h>
#include "pico/types.h"     // uint

// 0 = 640x480@60, crops 40 px each side of MDA's 720.
// 1 = 720x576@50, all 720 columns, frame-matched to the ~50 Hz source.
// Both widths are a multiple of 16
#define VIDEO_MODE_576P 1

#if VIDEO_MODE_576P
#define VIDEO_FB_W      720u
#define VIDEO_FB_H      576u
#else
#define VIDEO_FB_W      640u
#define VIDEO_FB_H      480u
#endif

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
