#ifndef TTL2DVI_BUTTONS_H
#define TTL2DVI_BUTTONS_H

#include "pico/types.h"     // uint

// Input events, as bits. The IR receiver produces the first four too.
#define EV_UP      (1u << 0)    // on press, repeating while held
#define EV_DOWN    (1u << 1)    // on press, repeating while held
#define EV_ENTER   (1u << 2)    // short press, on release
#define EV_BACK    (1u << 3)    // long press, once while held
#define EV_RESET   (1u << 4)    // ENTER + UP held 2 s
#define EV_BOOTSEL (1u << 5)    // ENTER + DOWN held 2 s
#define EV_NAV     (EV_UP | EV_DOWN | EV_ENTER | EV_BACK)

void buttons_init(void);

// Events since the last call. Polled from the main loop, once per frame.
uint buttons_poll(void);

// A combination being held, before it fires: which, and ms until it does.
#define BUTTONS_COMBO_RESET   1u
#define BUTTONS_COMBO_BOOTSEL 2u
uint buttons_combo(uint *left_ms);      // 0 = none

#endif
