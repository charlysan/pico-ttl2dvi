#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "osd.h"
#include "buttons.h"
#include "remote.h"
#include "learn.h"

#define IDLE_US  60000000   // gives up after this long without input
#define MAX_KEYS 24

// The navigation actions, asked in turn, then the shortcuts, picked from the
// list. Shortcuts are console commands, so only ones that exist.
static const struct { const char *label, *action; char rep; } acts[] = {
    { "PREV",       "@prev",            'r' },
    { "NEXT",       "@next",            'r' },
    { "ENTER",      "@enter",           '-' },
    { "BACK",       "@back",            '-' },
    { "OSD status", "osd status",       '-' },
    { "OSD live",   "osd live",         '-' },
    { "OSD hide",   "osd hide",         '-' },
    { "Scanlines",  "scanlines switch", '-' },
    { "Load 0",     "load 0",           '-' },
    { "Load 1",     "load 1",           '-' },
    { "Load 2",     "load 2",           '-' },
    { "Load 3",     "load 3",           '-' },
    { "Load 4",     "load 4",           '-' },
    { "Load 5",     "load 5",           '-' },
    { "Load 6",     "load 6",           '-' },
    { "Load 7",     "load 7",           '-' },
};
#define NNAV  4u
#define NACTS count_of(acts)
#define NLIST (NACTS - NNAV + 1u)   // the shortcuts, after "Done"

typedef enum { OFF, KEYS, SHORTCUTS, WAITKEY, CONFIRM } state_t;

static state_t  s_state;
static uint     s_step;             // KEYS: the action asked for
static uint     s_sel;              // SHORTCUTS: 0 = Done, else acts[NNAV + s_sel - 1]
static uint32_t s_code[MAX_KEYS];
static uint8_t  s_act[MAX_KEYS];
static uint     s_n;
static bool     s_yes;
static char     s_note[40];         // the last thing that happened
static uint64_t s_last;

bool learn_active(void) { return s_state != OFF; }

static uint keys_for(uint act)
{
    uint k = 0;
    for (uint i = 0; i < s_n; i++) k += s_act[i] == act;
    return k;
}

static void forget(uint act)
{
    uint j = 0;
    for (uint i = 0; i < s_n; i++)
        if (s_act[i] != act) {
            s_code[j] = s_code[i];
            s_act[j++] = s_act[i];
        }
    s_n = j;
}

static uint picked(void) { return NNAV + s_sel - 1u; }

static void draw(void)
{
    char text[80];
    switch (s_state) {
    case KEYS:
        if (!keys_for(s_step))
            snprintf(text, sizeof text, "Learn %u/%u  Press %s on the remote  %s",
                     s_step + 1, NNAV, acts[s_step].label, s_note);
        else
            snprintf(text, sizeof text, "Learn %u/%u  %s: %u key%s  %s  ENTER: next",
                     s_step + 1, NNAV, acts[s_step].label, keys_for(s_step),
                     keys_for(s_step) == 1 ? "" : "s", s_note);
        break;
    case SHORTCUTS:
        if (!s_sel)
            snprintf(text, sizeof text, "Shortcuts  <  Done, save         >  %s", s_note);
        else
            snprintf(text, sizeof text, "Shortcuts  <  %-10s %-6s  >  %s", acts[picked()].label,
                     keys_for(picked()) ? "(set)" : "", s_note);
        break;
    case WAITKEY:
        snprintf(text, sizeof text, "Shortcuts  Press a key for %s  %s",
                 acts[picked()].label, s_note);
        break;
    default:
        snprintf(text, sizeof text, "Learn  Save remote and reboot?  %s",
                 s_yes ? " No  [Yes]" : "[No]  Yes ");
        break;
    }
    osd_menu(text);
}

static void stop(const char *why)
{
    s_state = OFF;
    osd_menu_close();
    osd_message("%s", why);
}

void learn_start(void)
{
    s_state = KEYS;
    s_step  = 0;
    s_n     = 0;
    s_note[0] = 0;
    s_last  = time_us_64();
    draw();
}

// A remote key for act: refused if another action has it.
static bool assign(uint32_t code, uint act)
{
    for (uint i = 0; i < s_n; i++)
        if (s_code[i] == code) {
            if (s_act[i] == act) return true;
            snprintf(s_note, sizeof s_note, "(%08lX already %s)",
                     (unsigned long)code, acts[s_act[i]].label);
            return false;
        }
    if (s_n == MAX_KEYS) {
        snprintf(s_note, sizeof s_note, "(no room for more keys)");
        return false;
    }
    s_code[s_n] = code;
    s_act[s_n++] = (uint8_t)act;
    snprintf(s_note, sizeof s_note, "(%08lX)", (unsigned long)code);
    return true;
}

void learn_code(uint32_t code)
{
    if (s_state != KEYS && s_state != WAITKEY) return;
    s_last = time_us_64();
    if (s_state == KEYS) {
        assign(code, s_step);
    } else {
        // One key per shortcut: a new one replaces it, unless another action
        // has it (then the old one stays).
        const uint act = picked();
        bool taken = false;
        for (uint i = 0; i < s_n; i++) taken |= s_code[i] == code && s_act[i] != act;
        if (!taken) forget(act);
        if (assign(code, act)) s_state = SHORTCUTS;
    }
    draw();
}

static void save(void)
{
    const uint size = 64u + MAX_KEYS * 40u;
    char *text = malloc(size);
    if (!text) { stop("Learn: out of memory, remote not changed"); return; }
    int len = snprintf(text, size,
                       "# ttl2dvi IR key map, learned on the device\n"
                       "# code     repeat  action\n");
    for (uint i = 0; i < s_n; i++)
        len += snprintf(text + len, size - (uint)len, "%08lX   %c       %s\n",
                        (unsigned long)s_code[i], acts[s_act[i]].rep, acts[s_act[i]].action);
    osd_menu("Learn  Saving, rebooting...");
    remote_save_map(text);
    free(text);
    stop("Learn: map too big, remote not changed");
}

void learn_poll(uint ev)
{
    if (s_state == OFF) return;
    const uint64_t now = time_us_64();
    if (!ev) {
        if (now - s_last >= IDLE_US) stop("Learn: timed out, remote not changed");
        return;
    }
    s_last = now;

    // Long press: back to the list while waiting for a key, cancel elsewhere.
    if (ev & EV_BACK) {
        if (s_state == WAITKEY) {
            s_state = SHORTCUTS;
            s_note[0] = 0;
            draw();
        } else {
            stop("Learn: cancelled, remote not changed");
        }
        return;
    }

    switch (s_state) {
    case KEYS:
        // UP: forget this action's keys and press them again.
        if (ev & EV_UP) {
            forget(s_step);
            snprintf(s_note, sizeof s_note, "(cleared)");
        }
        if ((ev & (EV_ENTER | EV_DOWN)) && keys_for(s_step)) {
            s_note[0] = 0;
            if (++s_step == NNAV) {
                s_state = SHORTCUTS;
                s_sel   = 0;
            }
        }
        break;
    case SHORTCUTS:
        if (ev & EV_UP)   { s_sel = (s_sel + NLIST - 1u) % NLIST; s_note[0] = 0; }
        if (ev & EV_DOWN) { s_sel = (s_sel + 1u) % NLIST;          s_note[0] = 0; }
        if (ev & EV_ENTER) {
            s_note[0] = 0;
            if (!s_sel) { s_state = CONFIRM; s_yes = false; }
            else        s_state = WAITKEY;
        }
        break;
    case CONFIRM:
        if (ev & (EV_UP | EV_DOWN)) s_yes = !s_yes;
        if (ev & EV_ENTER) {
            if (s_yes) { save(); return; }
            stop("Learn: remote not changed");
            return;
        }
        break;
    default:
        break;
    }
    draw();
}
