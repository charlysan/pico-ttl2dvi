#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "capture.h"
#include "source.h"
#include "settings.h"
#include "osd.h"
#include "buttons.h"
#include "menu.h"
#include "tune.h"

#define IDLE_US   30000000  // gives up after this long without input
#define MIN_ALIGN 0.5f      // as detect: real matches score 0.8-0.9
#define K_SLACK   0.05f     // sysclk/dot this far from a whole number: may shimmer

typedef enum { TUNE_OK, TUNE_NO_SIGNAL, TUNE_FEW_EDGES, TUNE_UNCLEAR, TUNE_RANGE } status_t;

typedef struct {
    capture_measure_t m;
    capture_phase_t   p;
    uint              chars, char_px;
    int               pct100;   // measured vs current dotclock, in 0.01 %
    float             k;        // sysclk / measured dot clock
    bool              k_off;    // k not near a whole number
} result_t;

// The dot clock from 3/4 to 4/3 of the current one, then the phase for it.
// Wide, so a card on another crystal is found without selecting a source
// first (the OTI as MDA: 17.75 MHz under MDA16's 16.000). Below 2x across the
// range on purpose: at half the true pixel period every edge lines up too.
static status_t measure(result_t *r)
{
    const source_mode_t *src = source_active();
    const uint cur = capture_dot_hz();
    const bool ok = capture_measure_dot(80u, src->data_base, src->data_bits,
                                        cur / 4u * 3u, cur / 3u * 4u, &r->m);
    if (!r->m.lines)         return TUNE_NO_SIGNAL;
    if (!ok)                 return TUNE_FEW_EDGES;
    if (r->m.align < MIN_ALIGN) return TUNE_UNCLEAR;
    if (!capture_best_phase(&r->m, r->m.dot_hz, &r->p)) return TUNE_RANGE;
    r->char_px = src->data_bits == 2 ? 9u : 8u;
    r->chars   = (uint)(r->m.h_total / (float)r->char_px + 0.5f);
    r->pct100  = (int)(((int64_t)r->m.dot_hz - cur) * 10000 / cur);
    r->k       = (float)clock_get_hz(clk_sys) / (float)r->m.dot_hz;
    r->k_off   = fabsf(r->k - (float)(uint)(r->k + 0.5f)) > K_SLACK;
    return TUNE_OK;
}

static const char *why(status_t s)
{
    switch (s) {
    case TUNE_NO_SIGNAL: return "no signal";
    case TUNE_FEW_EDGES: return "too few edges: put text or a pattern on screen";
    case TUNE_UNCLEAR:   return "no clear match: put text on screen, or try Source > Detect";
    case TUNE_RANGE:     return "dot clock out of range for this sysclk";
    default:             return "";
    }
}

// "+10.94%"
static void fmt_pct(char *s, size_t n, int pct100)
{
    const uint a = (uint)(pct100 < 0 ? -pct100 : pct100);
    snprintf(s, n, "%c%u.%02u%%", pct100 < 0 ? '-' : '+', a / 100u, a % 100u);
}

static void apply(const result_t *r)
{
    capture_set_dot_hz(r->m.dot_hz);     // first: it re-clamps phase
    capture_set_phase((int)r->p.phase);
}

// ---- console ----

static void print(const result_t *r)
{
    const uint d = r->m.dot_hz;
    char pct[16];
    fmt_pct(pct, sizeof pct, r->pct100);
    printf("tune: %u edges in %u lines, alignment %u.%03u\n", r->m.edges, r->m.lines,
           (uint)r->m.align, (uint)(r->m.align * 1000.0f) % 1000u);
    printf("  dotclock %u.%04u MHz (%s), h_total %u.%u px ~ %u chars of %u\n",
           d / 1000000u, (d / 100u) % 10000u, pct, (uint)r->m.h_total,
           (uint)((r->m.h_total - (uint)r->m.h_total) * 10.0f), r->chars, r->char_px);
    printf("  phase %u of %u (%ux), margin %u.%u sysclk, edge spread %u.%u\n",
           r->p.phase, r->p.px_cyc, r->p.os,
           (uint)r->p.margin, (uint)(r->p.margin * 10.0f) % 10u,
           (uint)r->p.spread, (uint)(r->p.spread * 10.0f) % 10u);
    printf("  sysclk / dot clock = %u.%02u%s\n", (uint)r->k, (uint)(r->k * 100.0f) % 100u,
           r->k_off ? ": not a whole number, so there may be some shimmer;"
                      " a source built for this clock would be cleaner" : "");
    if (r->p.margin < r->p.spread)
        printf("  the margin is inside the edge spread: expect some shimmer\n");
}

void cmd_tune(int argc, char **argv)
{
    printf("tune: measuring...\n");
    fflush(stdout);
    result_t r;
    const status_t s = measure(&r);
    if (s != TUNE_OK) { printf("tune: %s\n", why(s)); return; }
    print(&r);
    if (argc >= 2 && !strcmp(argv[1], "apply")) {
        apply(&r);
        printf("tune: applied\n");
    } else {
        printf("  to apply: tune apply  (or: dotclock %u.%04u, then phase %u)\n",
               r.m.dot_hz / 1000000u, (r.m.dot_hz / 100u) % 10000u, r.p.phase);
    }
}

// ---- the menu's screens ----

typedef enum { OFF, RESULT, SLOT } state_t;
static const char *const choices[] = { "Apply", "Apply and save", "Cancel" };

static state_t  s_state;
static uint     s_sel;
static result_t s_r;
static uint64_t s_last;

bool tune_active(void) { return s_state != OFF; }

static void draw(void)
{
    char text[96];
    if (s_state == SLOT) {
        char slot[32];
        menu_slot_label(s_sel, slot, sizeof slot);
        snprintf(text, sizeof text, "Tune  Save to slot  <  %-14s  >", slot);
    } else {
        const uint d = s_r.m.dot_hz;
        char pct[16];
        fmt_pct(pct, sizeof pct, s_r.pct100);
        snprintf(text, sizeof text, "Tune %u.%04u MHz %s  phase %u  margin %u.%u  %u ch%s  < %s >",
                 d / 1000000u, (d / 100u) % 10000u, pct, s_r.p.phase,
                 (uint)s_r.p.margin, (uint)(s_r.p.margin * 10.0f) % 10u, s_r.chars,
                 s_r.k_off ? "  may shimmer" : "", choices[s_sel]);
    }
    osd_menu(text);
}

static void stop(void)
{
    s_state = OFF;
    osd_menu_close();
}

void tune_start(void)
{
    osd_menu("Tune  measuring...");
    sleep_ms(50);                       // let a frame show it before core0 is busy
    const status_t s = measure(&s_r);
    if (s != TUNE_OK) {
        osd_menu_close();
        osd_message("Tune: %s", why(s));
        return;
    }
    s_state = RESULT;
    s_sel   = 0;
    s_last  = time_us_64();
    draw();
}

void tune_poll(uint ev)
{
    if (s_state == OFF) return;
    const uint64_t now = time_us_64();
    if (!ev) {
        if (now - s_last >= IDLE_US) stop();
        return;
    }
    s_last = now;

    if (s_state == RESULT) {
        const uint n = count_of(choices);
        if (ev & EV_UP)   s_sel = (s_sel + n - 1u) % n;
        if (ev & EV_DOWN) s_sel = (s_sel + 1u) % n;
        if (ev & EV_BACK) { stop(); return; }
        if (ev & EV_ENTER) {
            if (s_sel == 2) { stop(); return; }
            apply(&s_r);
            if (s_sel == 0) {
                stop();
                osd_message("Tuned: dotclock %u.%04u, phase %u",
                            s_r.m.dot_hz / 1000000u, (s_r.m.dot_hz / 100u) % 10000u,
                            s_r.p.phase);
                return;
            }
            s_state = SLOT;
            s_sel   = 0;
        }
    } else {
        if (ev & EV_UP)   s_sel = (s_sel + SETTINGS_SLOTS - 1u) % SETTINGS_SLOTS;
        if (ev & EV_DOWN) s_sel = (s_sel + 1u) % SETTINGS_SLOTS;
        if (ev & EV_BACK) { s_state = RESULT; s_sel = 1; }
        if (ev & EV_ENTER) {
            osd_menu("Tune  Saving, rebooting...");
            menu_save_slot(s_sel);
            return;
        }
    }
    draw();
}
