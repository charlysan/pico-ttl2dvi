#ifndef TTL2DVI_CAPTURE_H
#define TTL2DVI_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

#define CAPTURE_MAX_WIDTH 864
#define CAPTURE_MAX_LINES 400
#define SAMPLES_PER_WORD  16u   // 2bpp: 32-bit autopush / 2 bits
#define RAW_WORDS  ((CAPTURE_MAX_WIDTH / SAMPLES_PER_WORD) * CAPTURE_MAX_LINES)

// Claim the sampler SM + DMA on pio0. Call once, after sync_init()
void capture_init(void);
bool capture_grab(void); // true = got a frame, false = no sync
void capture_set_bp(int bp);
int  capture_get_bp(void);
uint capture_width(void);
uint capture_height(void);
void capture_dump_frame(void);
#endif