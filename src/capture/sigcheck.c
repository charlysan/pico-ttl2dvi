#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "sigcheck.h"

// Valid = HSYNC inside one of the groups and VSYNC inside 45..65 Hz.
// Stable = valid, same group, HSYNC within 0.5% of where the run started,
// for STABLE_MS.
#define STABLE_MS     1000u
#define VSYNC_MIN_MHZ 45000u
#define VSYNC_MAX_MHZ 65000u

static const struct { signal_group_t g; uint32_t lo, hi; const char *name; } groups[] = {
    { GROUP_15K, 15300, 16200, "15.7 kHz" },
    { GROUP_18K, 17800, 18800, "18 kHz"   },
    { GROUP_22K, 21300, 22700, "22 kHz"   },
};

static signal_state_t g_state = SIGNAL_NONE;
static signal_group_t g_group;
static uint32_t g_ref_hz, g_hz, g_vmhz;
static uint64_t g_since;

const char *signal_group_name(signal_group_t g)
{
    for (uint i = 0; i < sizeof groups / sizeof groups[0]; i++)
        if (groups[i].g == g) return groups[i].name;
    return "none";
}

static signal_group_t group_of(uint32_t hz)
{
    for (uint i = 0; i < sizeof groups / sizeof groups[0]; i++)
        if (hz >= groups[i].lo && hz <= groups[i].hi) return groups[i].g;
    return GROUP_NONE;
}

void signal_lost(void)
{
    g_state = SIGNAL_NONE;
}

// A captured frame is one VSYNC period, minus the ~3 lines before the sampler
// locks after the edge.
void signal_feed(uint32_t line_cycles, uint lines)
{
    if (!line_cycles || !lines) { signal_lost(); return; }
    const uint32_t hz   = clock_get_hz(clk_sys) / line_cycles;
    const uint32_t vmhz = (uint32_t)((uint64_t)hz * 1000u / (lines + 3u));
    const signal_group_t grp = group_of(hz);
    const uint64_t now = time_us_64();

    if (grp == GROUP_NONE || vmhz < VSYNC_MIN_MHZ || vmhz > VSYNC_MAX_MHZ) {
        g_state = SIGNAL_NONE;
        return;
    }
    g_hz = hz;
    g_vmhz = vmhz;

    const uint32_t tol = g_ref_hz / 200u;
    if (g_state == SIGNAL_NONE || grp != g_group
        || hz + tol < g_ref_hz || hz > g_ref_hz + tol) {
        g_state  = SIGNAL_UNSTABLE;
        g_group  = grp;
        g_ref_hz = hz;
        g_since  = now;
        return;
    }
    if (now - g_since >= STABLE_MS * 1000u) g_state = SIGNAL_STABLE;
}

void signal_get(signal_info_t *out)
{
    out->state     = g_state;
    out->group     = g_state == SIGNAL_NONE ? GROUP_NONE : g_group;
    out->hsync_hz  = g_hz;
    out->vsync_mhz = g_vmhz;
    out->ms        = g_state == SIGNAL_NONE ? 0 : (uint32_t)((time_us_64() - g_since) / 1000u);
}
