#ifndef TTL2DVI_IR_H
#define TTL2DVI_IR_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

// NEC IR decoder on pio2. A frame is one word, LSB first as sent: address
// (bits 0-15, or 8 + its inverse), command (16-23), inverse command (24-31).
#define IR_REPEAT 0xffffffffu   // a held button, every ~108 ms

// Call after the overclock: the SM clock is derived from sysclk.
void ir_init(uint pin);

// Next frame, or IR_REPEAT. False when there is none.
bool ir_get(uint32_t *code);

// The command byte matches its inverse (a repeat is valid too).
bool ir_valid(uint32_t code);

#endif
