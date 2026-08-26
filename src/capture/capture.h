#ifndef TTL2DVI_CAPTURE_H
#define TTL2DVI_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

#define CAPTURE_MAX_WIDTH 864

// Claim the sampler SM on pio0. Call once, after sync_init()
void capture_init(void);

#endif