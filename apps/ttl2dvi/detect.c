#include <stdio.h>
#include "pico/stdlib.h"
#include "capture.h"
#include "sigcheck.h"
#include "source.h"
#include "detect.h"

#define DETECT_TOL_PPM   5000       // measured dot clock vs a candidate's
#define DETECT_MIN_ALIGN 0.5f       // real matches score 0.8-0.9
#define NONE_TRIGGER_MS  2000u      // this long without signal: maybe a new card
#define RETRY_MS         5000u      // after an undecided mismatch

void detect_run(detect_result_t *r)
{
    signal_info_t si;
    signal_get(&si);
    r->measured = false;
    r->ncand = 0;
    r->hsync_hz = si.hsync_hz;
    if (si.state != SIGNAL_STABLE) { r->status = DETECT_NO_SIGNAL; return; }

    r->ncand = source_candidates(si.hsync_hz, r->cand, 8);
    if (!r->ncand)      { r->status = DETECT_UNKNOWN; return; }
    r->pick = 0;
    if (r->ncand == 1)  { r->status = DETECT_OK; return; }

    // Same line rate: tell them apart by dot clock. Search a range that covers
    // every candidate, on the first candidate's pins (all RGBI).
    uint lo = ~0u, hi = 0;
    for (uint i = 0; i < r->ncand; i++) {
        const uint d = r->cand[i].mode->dot_hz;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    const source_mode_t *c0 = r->cand[0].mode;
    if (!capture_measure_dot(80u, c0->data_base, c0->data_bits,
                             lo / 100u * 98u, hi / 100u * 102u, &r->m)) {
        r->status = DETECT_NO_EDGES;
        return;
    }
    r->measured = true;
    if (r->m.align < DETECT_MIN_ALIGN) { r->status = DETECT_UNCLEAR; return; }

    int64_t best = INT64_MAX;
    for (uint i = 0; i < r->ncand; i++) {
        int64_t d = (int64_t)r->m.dot_hz - r->cand[i].mode->dot_hz;
        if (d < 0) d = -d;
        if (d < best) { best = d; r->pick = i; }
    }
    r->status = best * 1000000 > (int64_t)DETECT_TOL_PPM * r->cand[r->pick].mode->dot_hz
              ? DETECT_NO_MATCH : DETECT_OK;
}

static bool     s_pending = true;   // detect at boot
static uint64_t s_none_since, s_last_try;

void auto_kick(void) { s_pending = true; }

void auto_poll(uint32_t line)
{
    if (!source_auto()) return;

    signal_info_t si;
    signal_get(&si);
    const uint64_t now = time_us_64();

    if (si.state == SIGNAL_NONE) {
        if (!s_none_since) s_none_since = now;
        return;
    }
    if (s_none_since) {
        if (now - s_none_since >= NONE_TRIGGER_MS * 1000u) s_pending = true;
        s_none_since = 0;
    }
    if (si.state != SIGNAL_STABLE) return;

    const bool misfit = !source_line_ok(line)
                        && now - s_last_try >= RETRY_MS * 1000u;
    if (!s_pending && !misfit) return;
    s_pending  = false;
    s_last_try = now;

    detect_result_t r;
    detect_run(&r);
    if (r.status != DETECT_OK) return;
    const uint idx = r.cand[r.pick].index;
    if (idx == source_active_index()) return;

    printf("auto: switching to %s (reboot)...\n", source_get(idx)->name);
    sleep_ms(50);
    source_select(idx);
}
