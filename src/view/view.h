#ifndef TTL2DVI_VIEW_H
#define TTL2DVI_VIEW_H

// Scan converter: composite the most recently captured frame into the DVI
// framebuffer. It reads source pixels from the capture engine and writes the
// display framebuffer from video -- the join between the two halves.
//
// For now the mapping is fixed: 1:1, the ~720-px source centred in the 736-px
// framebuffer (8px border each side, no scaling so the checkerboard stays
// perfect) and vertically letterboxed. Framing knobs (pan/scale/scanlines/
// per-level brightness) will layer on here later.

// Composite the latest grabbed frame into the framebuffer. Call after a
// successful capture_grab(), on core0.
void view_render(void);

#endif
