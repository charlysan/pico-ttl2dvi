#ifndef TTL2DVI_DETECT_H
#define TTL2DVI_DETECT_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"
#include "capture.h"
#include "source.h"

typedef enum {
    DETECT_NO_SIGNAL,    // no signal, or not stable yet
    DETECT_UNKNOWN,      // no source at this line rate
    DETECT_NO_EDGES,     // needed the dot clock, couldn't measure it
    DETECT_UNCLEAR,      // measured, but the edges didn't line up at any period
    DETECT_NO_MATCH,     // measured, but no candidate within tolerance
    DETECT_OK,
} detect_status_t;

typedef struct {
    detect_status_t   status;
    uint32_t          hsync_hz;
    uint              ncand;
    source_cand_t     cand[8];
    bool              measured;
    capture_measure_t m;
    uint              pick;     // into cand[], when DETECT_OK
} detect_result_t;

// Which source the signal comes from: line rate first, dot clock when several
// sources share it. Needs a stable signal. Freezes capture for ~0.5 s when it
// measures.
void detect_run(detect_result_t *r);

// Auto mode: call after every grab (line = its line period, 0 after a failed
// grab). Detects at boot, after >= 2 s without signal, and when a stable
// signal doesn't fit the running source; reboots into another source if
// that's what it finds.
void auto_poll(uint32_t line);
void auto_kick(void);           // detect at the next stable signal

#endif
