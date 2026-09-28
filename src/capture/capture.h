#ifndef TTL2DVI_CAPTURE_H
#define TTL2DVI_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

#define CAPTURE_MAX_WIDTH 864

// Claim the sampler SM + DMA on pio0 and configure it for the active source.
// Call once, after sync_init()
void capture_init(void);
uint capture_sample_cyc(void);                   // sysclk per sample
uint32_t capture_frames(void);                   // frames grabbed since boot
bool capture_grab(void); // true = got a frame, false = no sync
void capture_set_bp(int bp);
int  capture_get_bp(void);
void capture_set_phase(int phase);
int  capture_get_phase(void);
uint capture_width(void);
uint capture_height(void);
void capture_get_line(uint line, uint8_t *dst);  // unpack one line, one sample per byte
const uint8_t *capture_raw_line(uint line);      // packed samples, LSB first
void capture_hold(void);                         // stop the sampler; next grab re-syncs
void capture_dump_frame(void);
#endif