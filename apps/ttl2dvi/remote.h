#ifndef TTL2DVI_REMOTE_H
#define TTL2DVI_REMOTE_H

#include "pico/types.h"     // uint

// Loads the key map from flash and starts the decoder.
void remote_init(void);

// Drains the IR decoder. Mapped @up/@down/@enter/@back come back as EV_*
// bits, as from buttons_poll(); other actions run as console commands.
uint remote_poll(void);

// ir on|off: print every received code. ir map: list the loaded map.
void cmd_ir(int argc, char **argv);

#endif
