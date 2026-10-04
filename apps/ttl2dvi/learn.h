#ifndef TTL2DVI_LEARN_H
#define TTL2DVI_LEARN_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

// Learning a remote on the device, driven by the push buttons: the OSD asks
// for each navigation action's key(s), then saves the map (reboots).

void learn_start(void);
bool learn_active(void);

// While active: every remote frame goes here instead of the key map, and the
// buttons' EV_* bits instead of the menu.
void learn_code(uint32_t code);
void learn_poll(uint ev);

#endif
