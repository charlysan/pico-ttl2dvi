#ifndef TTL2DVI_VIEW_H
#define TTL2DVI_VIEW_H

// Composite the last captured frame into the DVI framebuffer. Call after a
// successful capture_grab().
void view_render(void);

#endif
