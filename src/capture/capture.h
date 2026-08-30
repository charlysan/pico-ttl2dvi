#ifndef TTL2DVI_CAPTURE_H
#define TTL2DVI_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

#define CAPTURE_MAX_WIDTH 864
#define CAPTURE_MAX_LINES 400
#define SAMPLE_CYC        16u   // sysclk per sample: 2 SM cycles × clkdiv 8
#define SAMPLES_PER_WORD  16u   // 2bpp: 32-bit autopush / 2 bits
#define RAW_WORDS  ((CAPTURE_MAX_WIDTH / SAMPLES_PER_WORD) * CAPTURE_MAX_LINES)

// Claim the sampler SM + DMA on pio0. Call once, after sync_init()
void capture_init(void);
bool capture_grab(void); // true = got a frame, false = no sync
void capture_set_bp(int bp);
int  capture_get_bp(void);
void capture_set_phase(int phase);
int  capture_get_phase(void);
uint capture_width(void);
uint capture_height(void);
void capture_get_line(uint line, uint8_t *dst);  // unpack one line to 0..3 bytes
const uint8_t *capture_raw_line(uint line);      // packed samples, 4 px per byte
void capture_hold(void);                         // stop the sampler; next grab re-syncs
void capture_dump_frame(void);
#endif