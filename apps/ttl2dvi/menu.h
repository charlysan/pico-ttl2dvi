#ifndef TTL2DVI_MENU_H
#define TTL2DVI_MENU_H

#include <stddef.h>
#include "pico/types.h"     // uint

// One-line OSD menu. ev: EV_* bits from buttons_poll() (or IR), every loop,
// also when 0, which is what times it out.
void menu_poll(uint ev);

// A settings slot as the menu shows it ("3* name", "3  -"), and saving the
// current settings to it, keeping its name (reboots). Also used by tune.c.
void menu_slot_label(uint i, char *s, size_t n);
void menu_save_slot(uint i);

#endif
