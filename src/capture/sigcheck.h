#ifndef TTL2DVI_SIGCHECK_H
#define TTL2DVI_SIGCHECK_H

#include <stdint.h>
#include "pico/types.h"     // uint

// Is the incoming signal valid and stable enough to act on? Fed after every
// grab; nothing here measures anything itself.

typedef enum { SIGNAL_NONE, SIGNAL_UNSTABLE, SIGNAL_STABLE } signal_state_t;
typedef enum { GROUP_NONE, GROUP_15K, GROUP_18K, GROUP_22K } signal_group_t;

void signal_feed(uint32_t line_cycles, uint lines);   // after a good grab
void signal_lost(void);                               // after a failed grab

typedef struct {
    signal_state_t state;
    signal_group_t group;
    uint32_t hsync_hz, vsync_mhz;   // last valid reading (VSYNC in mHz)
    uint32_t ms;                    // how long the current run has held
} signal_info_t;
void signal_get(signal_info_t *out);
const char *signal_group_name(signal_group_t g);

#endif
