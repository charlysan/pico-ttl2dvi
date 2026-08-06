#ifndef TTL2DVI_VIEW_H
#define TTL2DVI_VIEW_H

// Scan converter: composite the most recently captured frame into the DVI
// framebuffer. It reads source pixels from the capture engine and writes the
// display framebuffer from video -- the join between the two halves.
//
// Mapping is 1:1 for now: the ~720-px source centred in the 736-px
// framebuffer (8px border each side. VIDEO / VIDEO+INTENSITY gray levels
// are tunable (view_set_mda_lvl_*)

// Composite the latest grabbed frame into the framebuffer. Call after a
// successful capture_grab(), on core0.
void view_render(void);

// Display gray level (0..3) for MDA VIDEO-alone and VIDEO+INTENSITY pixels.
void view_set_mda_lvl_normal(int val);
int  view_get_mda_lvl_normal(void);
void view_set_mda_lvl_bright(int val);
int  view_get_mda_lvl_bright(void);

// Vertical scale (console `vscale`) - each source line is drawn this many times.
void view_set_vscale(int val);
int  view_get_vscale(void);

// Vertical position (console `vpos`)
void view_set_vpos(int val);
int  view_get_vpos(void);

// Horizontal position (console `hpos`), in SOURCE pixels; positive moves the
// image RIGHT. Display-side: it slides the captured image inside the
// framebuffer without touching sampling. `bp` is the capture-side counterpart
// (it moves the sampling window) -- use `bp` to find the picture, `hpos` to
// place it.
void view_set_hpos(int val);
int  view_get_hpos(void);

#endif
