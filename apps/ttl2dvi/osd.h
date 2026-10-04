#ifndef TTL2DVI_OSD_H
#define TTL2DVI_OSD_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

// What the app puts on the OSD (video.c draws it).

// After every grab: good (line period in sysclk, captured lines) or lost.
// Shows the status line when the signal becomes stable, and drives live mode.
void osd_frame(uint32_t line_cycles, uint lines);
void osd_lost(void);

// A changed knob, printf-style, for 2 s.
void osd_knob(const char *fmt, ...);

// Any text, printf-style, for 5 s.
void osd_message(const char *fmt, ...);

// Live mode: H, V, lines/frame, fps and signal state, every 500 ms.
void osd_live(bool on);
bool osd_live_on(void);

// Source, sysclk and the measured sync rates, as one snapshot line, for 5 s.
// Measuring VSYNC waits out one period. echo: also print it on the console.
void osd_status(bool echo);

// The menu's line. While it is shown, every other message is dropped.
void osd_menu(const char *text);
void osd_menu_close(void);

void cmd_osd(int argc, char **argv);
void cmd_osd_print(int argc, char **argv);

#endif
