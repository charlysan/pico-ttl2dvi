#ifndef TTL2DVI_TUNE_H
#define TTL2DVI_TUNE_H

#include <stdbool.h>
#include "pico/types.h"     // uint

// Auto tune: measure the dot clock from pixel edges and pick the phase, as
// tools/autotune.py does, on the device. Needs content with vertical edges.

// The menu's screens: measure at once, then Apply / Apply and save / Cancel.
// While active, the buttons' EV_* bits go to tune_poll() instead of the menu.
void tune_start(void);
bool tune_active(void);
void tune_poll(uint ev);

// tune [apply]: print the result; apply sets dotclock then phase.
void cmd_tune(int argc, char **argv);

#endif
