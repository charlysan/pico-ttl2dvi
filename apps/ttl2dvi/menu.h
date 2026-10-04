#ifndef TTL2DVI_MENU_H
#define TTL2DVI_MENU_H

#include "pico/types.h"     // uint

// One-line OSD menu. ev: EV_* bits from buttons_poll() (or IR), every loop,
// also when 0, which is what times it out.
void menu_poll(uint ev);

#endif
