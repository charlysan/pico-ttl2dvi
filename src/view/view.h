#ifndef TTL2DVI_VIEW_H
#define TTL2DVI_VIEW_H

#include "pico/types.h"     // uint

// Composite the last captured frame into the DVI framebuffer. Call after a
// successful capture_grab().
void view_render(void);

// Grey level (0..3) for VIDEO alone and VIDEO+INTENSITY. Applies next frame.
void view_set_mda_levels(uint normal, uint bright);
uint view_get_mda_normal(void);
uint view_get_mda_bright(void);

#endif
