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

// Display-side framing, applies next frame. vscale 1..4 draws each source line
// N times; vpos (source lines, + = down) and hpos (source px, + = right) move
// the already captured image.
void view_set_vscale(int n);
uint view_get_vscale(void);
void view_set_vpos(int n);
int  view_get_vpos(void);
void view_set_hpos(int n);
int  view_get_hpos(void);

#endif
