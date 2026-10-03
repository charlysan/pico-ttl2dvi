#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "sync.h"
#include "video.h"
#include "source.h"
#include "sigcheck.h"
#include "osd.h"

#define KNOB_MS     2000u
#define TEXT_MS     5000u
#define LIVE_MS     500u        // live refresh period
#define LIVE_FRAMES 64u         // frame intervals kept per refresh

static bool           s_live;
static uint64_t       s_pause_until;    // live yields to another message until then
static signal_state_t s_prev = SIGNAL_NONE;

// Live accumulators, reset every refresh. s_last_frame is not: the interval
// across a refresh still counts.
static uint64_t s_t0, s_last_frame;
static uint     s_frames, s_nfr, s_ndt;     // frames seen, kept, intervals kept
static uint32_t s_cyc[LIVE_FRAMES], s_lin[LIVE_FRAMES], s_dt[LIVE_FRAMES];

static bool s_menu;            // the menu owns the line while open

void osd_menu(const char *text)
{
    s_menu = true;
    video_osd_show(text, 0);
}

void osd_menu_close(void)
{
    s_menu = false;
    video_osd_hide();
    s_pause_until = 0;
}

static void show(const char *text, uint ms)
{
    if (s_menu) return;
    video_osd_show(text, ms);
    s_pause_until = time_us_64() + (uint64_t)ms * 1000u;
}

void osd_knob(const char *fmt, ...)
{
    char text[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    show(text, KNOB_MS);
}

void osd_message(const char *fmt, ...)
{
    char text[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    show(text, TEXT_MS);
}

void osd_status(bool echo)
{
    const uint32_t f = clock_get_hz(clk_sys);
    uint32_t pulse;
    const uint32_t ph = sync_hsync(&pulse);
    const uint32_t pv = ph ? sync_vsync_period() : 0;

    // Fixed-width fields: the line is centred, so a constant length keeps
    // every value in the same place across sources and readings.
    char h[16] = "--", v[16] = "--";
    if (ph) snprintf(h, sizeof h, "%lu", (unsigned long)(f / ph));
    if (pv) {
        const uint32_t v100 = (uint32_t)((uint64_t)f * 100u / pv);
        snprintf(v, sizeof v, "%lu.%02lu",
                 (unsigned long)(v100 / 100u), (unsigned long)(v100 % 100u));
    }
    char text[80];
    snprintf(text, sizeof text, "%-7s  %3lu.%01lu MHz  H %5s Hz  V %5s Hz",
             source_group_name(source_group()),
             (unsigned long)(f / 1000000u), (unsigned long)((f / 100000u) % 10u), h, v);
    if (echo) printf("osd: %s\n", text);
    show(text, TEXT_MS);
}

static void live_reset(uint64_t now)
{
    s_t0 = now;
    s_frames = 0;
    s_nfr = 0;
    s_ndt = 0;
}

// Sorts in place; the arrays are refilled after every refresh.
static uint32_t median(uint32_t *a, uint n)
{
    for (uint i = 1; i < n; i++) {
        const uint32_t x = a[i];
        uint j = i;
        for (; j && a[j - 1] > x; j--) a[j] = a[j - 1];
        a[j] = x;
    }
    return a[n / 2u];
}

static const char *state_name(signal_state_t s)
{
    return s == SIGNAL_STABLE ? "stable" : s == SIGNAL_UNSTABLE ? "unstable" : "none";
}

// H and L are the medians of what each grab measures, so one bad frame
// doesn't move them. V from the time between consecutive grabs: one VSYNC
// period each, skipping intervals that span a dropped frame.
// Fixed-width fields, "--" when there's nothing to show, as in osd_status.
static void live_show(uint64_t now)
{
    signal_info_t si;
    signal_get(&si);
    char h[16] = "--", v[16] = "--", l[16] = "--", fps[16] = "--";

    if (s_nfr) {
        const uint32_t cyc = median(s_cyc, s_nfr);
        if (cyc) snprintf(h, sizeof h, "%lu", (unsigned long)(clock_get_hz(clk_sys) / cyc));
        snprintf(l, sizeof l, "%lu", (unsigned long)median(s_lin, s_nfr));
    }
    if (s_frames) {
        const uint32_t fps10 = (uint32_t)((uint64_t)s_frames * 10000000u / (now - s_t0));
        snprintf(fps, sizeof fps, "%lu.%lu",
                 (unsigned long)(fps10 / 10u), (unsigned long)(fps10 % 10u));
    }
    if (s_ndt) {
        uint32_t min = ~0u;
        for (uint i = 0; i < s_ndt; i++) if (s_dt[i] < min) min = s_dt[i];
        uint64_t sum = 0;
        uint n = 0;
        for (uint i = 0; i < s_ndt; i++)
            if (s_dt[i] < min + min / 2u) { sum += s_dt[i]; n++; }
        const uint32_t vmhz = (uint32_t)(1000000000ull * n / sum);
        snprintf(v, sizeof v, "%lu.%03lu", (unsigned long)(vmhz / 1000u),
                 (unsigned long)(vmhz % 1000u));
    }

    char text[96];
    snprintf(text, sizeof text, "%-7s  H %5s  V %6s  L %3s  %4s fps  %-8s",
             source_group_name(source_group()), h, v, l, fps, state_name(si.state));
    video_osd_show(text, 0);
}

static void poll(uint64_t now)
{
    // Status once whenever the signal becomes stable: after boot (so after a
    // source or mode change, which reboot), an EGA family switch, or the PC
    // coming back.
    signal_info_t si;
    signal_get(&si);
    if (si.state == SIGNAL_STABLE && s_prev != SIGNAL_STABLE && !s_live)
        osd_status(false);
    s_prev = si.state;

    if (!s_live || now - s_t0 < LIVE_MS * 1000u) return;
    if (now >= s_pause_until && !s_menu) live_show(now);
    live_reset(now);
}

void osd_frame(uint32_t line_cycles, uint lines)
{
    const uint64_t now = time_us_64();
    if (s_live) {
        if (s_nfr < LIVE_FRAMES) {
            s_cyc[s_nfr] = line_cycles;
            s_lin[s_nfr] = lines;
            s_nfr++;
        }
        s_frames++;
        if (s_last_frame && s_ndt < LIVE_FRAMES)
            s_dt[s_ndt++] = (uint32_t)(now - s_last_frame);
    }
    s_last_frame = now;
    poll(now);
}

void osd_lost(void)
{
    s_last_frame = 0;
    poll(time_us_64());
}

bool osd_live_on(void) { return s_live; }

void osd_live(bool on)
{
    s_live = on;
    if (on) {
        s_pause_until = 0;
        live_reset(time_us_64());
    } else {
        video_osd_hide();
    }
}

static void osd_usage(void)
{
    printf("  osd              show the state\n"
           "  osd on|off       enable or disable; off also hides it\n"
           "  osd auto|hold    messages hide after a timeout, or stay until replaced\n"
           "  osd hide         hide the current message and stop live\n"
           "  osd status       source, sysclk, HSYNC, VSYNC (5 s)\n"
           "  osd live [off]   H, V, lines/frame, capture fps, signal, every 500 ms\n"
           "  osd_print <text> show text (5 s)\n");
}

void cmd_osd(int argc, char **argv) {
    if (argc >= 2) {
        const char *a = argv[1];
        if      (!strcmp(a, "on"))     video_osd_enable(true);
        else if (!strcmp(a, "off"))    { osd_live(false); video_osd_enable(false); }
        else if (!strcmp(a, "auto"))   video_osd_set_hold(false);
        else if (!strcmp(a, "hold"))   video_osd_set_hold(true);
        else if (!strcmp(a, "hide"))   osd_live(false);
        else if (!strcmp(a, "status")) osd_status(true);
        else if (!strcmp(a, "live"))   osd_live(argc < 3 || strcmp(argv[2], "off"));
        else { osd_usage(); return; }
    }
    printf("osd %s, %s%s\n", video_osd_enabled() ? "on" : "off",
           video_osd_hold() ? "hold" : "auto", s_live ? ", live" : "");
}

void cmd_osd_print(int argc, char **argv) {
    if (argc < 2) {
        printf("osd_print <text>\n");
        return;
    }
    char text[96] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(text, " ", sizeof text - strlen(text) - 1);
        strncat(text, argv[i], sizeof text - strlen(text) - 1);
    }
    show(text, TEXT_MS);
}
