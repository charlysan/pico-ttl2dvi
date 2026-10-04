#ifndef TTL2DVI_BUTTONS_H
#define TTL2DVI_BUTTONS_H

#include "pico/types.h"     // uint

// Input events, as bits. The IR receiver will produce the same ones.
#define EV_UP    (1u << 0)      // on press, repeating while held
#define EV_DOWN  (1u << 1)      // on press, repeating while held
#define EV_ENTER (1u << 2)      // short press, on release
#define EV_BACK  (1u << 3)      // long press, once while held

void buttons_init(void);

// Events since the last call. Polled from the main loop, once per frame.
uint buttons_poll(void);

#endif
