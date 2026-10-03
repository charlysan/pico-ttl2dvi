#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "version.h"
#include "capture.h"
#include "video.h"
#include "view.h"
#include "source.h"
#include "settings.h"
#include "detect.h"
#include "osd.h"
#include "buttons.h"
#include "menu.h"

#define IDLE_US 10000000    // closes after this long without input
#define DEPTH   4

typedef enum { ITEM_SUB, ITEM_KNOB, ITEM_TOGGLE, ITEM_LIST, ITEM_ACTION } item_kind_t;

typedef struct item {
    const char         *name;
    item_kind_t         kind;
    bool              (*shown)(void);       // NULL: always
    bool                confirm;            // ask before acting
    const struct item  *sub;                // SUB
    uint                nsub;
    int               (*get)(void);         // KNOB; TOGGLE as 0/1
    void              (*set)(int);
    void              (*fmt)(char *s, size_t n, int v);    // KNOB, NULL: %d
    uint              (*count)(void);       // LIST
    void              (*label)(uint i, char *s, size_t n);
    bool              (*ok)(uint i);        // LIST, NULL: every entry
    void              (*pick)(uint i);      // LIST, after the menu closes
    void              (*run)(void);         // ACTION, after the menu closes
} item_t;

// ---- Picture ----

static int  get_vscale(void)    { return (int)view_get_vscale(); }
static bool has_repeat(void)    { return view_get_vscale() >= 2; }
static int  get_scanlines(void) { return video_get_scanlines(); }
static void set_scanlines(int v) { video_set_scanlines(v); }

static bool is_mda(void)        { return source_active()->data_bits == 2; }
static int  clamp3(int v)       { return v < 0 ? 0 : (v > 3 ? 3 : v); }
static int  get_normal(void)    { return (int)view_get_mda_normal(); }
static int  get_bright(void)    { return (int)view_get_mda_bright(); }
static void set_normal(int v)   { view_set_mda_levels((uint)clamp3(v), view_get_mda_bright()); }
static void set_bright(int v)   { view_set_mda_levels(view_get_mda_normal(), (uint)clamp3(v)); }

static const item_t picture[] = {
    { "H position", ITEM_KNOB, .get = view_get_hpos, .set = view_set_hpos },
    { "V position", ITEM_KNOB, .get = view_get_vpos, .set = view_set_vpos },
    { "V scale",    ITEM_KNOB, .get = get_vscale,    .set = view_set_vscale },
    { "Scanlines",  ITEM_TOGGLE, .shown = has_repeat, .get = get_scanlines, .set = set_scanlines },
    { "MDA normal", ITEM_KNOB, .shown = is_mda, .get = get_normal, .set = set_normal },
    { "MDA bright", ITEM_KNOB, .shown = is_mda, .get = get_bright, .set = set_bright },
};

// ---- Sampling ----

static void fmt_phase(char *s, size_t n, int v) { snprintf(s, n, "%d/%u", v, capture_px_cyc()); }

static int  get_dot(void)   { return (int)((capture_dot_hz() + 500u) / 1000u); }    // kHz
static void set_dot(int v)  { if (v > 0) capture_set_dot_hz((uint)v * 1000u); }
static void fmt_dot(char *s, size_t n, int v) { snprintf(s, n, "%d.%03d", v / 1000, v % 1000); }

static void dot_default(void)
{
    capture_set_dot_hz(0);
    const uint d = capture_dot_hz();
    osd_knob("dotclock %u.%04u (default)", d / 1000000u, (d / 100u) % 10000u);
}

static const item_t sampling[] = {
    { "Phase",       ITEM_KNOB, .get = capture_get_phase, .set = capture_set_phase, .fmt = fmt_phase },
    { "Back porch",  ITEM_KNOB, .get = capture_get_bp,    .set = capture_set_bp },
    { "Dot clock",   ITEM_KNOB, .get = get_dot,           .set = set_dot, .fmt = fmt_dot },
    { "Dot default", ITEM_ACTION, .run = dot_default },
};

// ---- Profiles ----

static uint nslots(void)         { return SETTINGS_SLOTS; }
static uint nslots_off(void)     { return SETTINGS_SLOTS + 1u; }
static bool slot_used(uint i)    { return settings_slot(i) != NULL; }
static bool default_ok(uint i)   { return !i || slot_used(i - 1u); }

static void slot_label(uint i, char *s, size_t n)
{
    const settings_slot_t *t = settings_slot(i);
    const char mark = (int)i == settings_default() ? '*' : ' ';
    if (t) snprintf(s, n, "%u%c %.8s", i, mark, t->name);
    else   snprintf(s, n, "%u  -", i);
}

static void default_label(uint i, char *s, size_t n)
{
    if (i) slot_label(i - 1u, s, n);
    else   snprintf(s, n, "off%s", settings_default() < 0 ? " *" : "");
}

static void load_slot(uint i)
{
    if (settings_load(i)) osd_message("Loaded slot %u", i);
}

// Keeps the name the slot already has.
static void save_slot(uint i)
{
    char name[SETTINGS_NAME + 1] = "";
    const settings_slot_t *t = settings_slot(i);
    if (t) memcpy(name, t->name, SETTINGS_NAME);
    settings_save(i, name[0] ? name : NULL);
}

static void default_slot(uint i) { settings_set_default(i ? (int)i - 1 : -1); }

static const item_t profiles[] = {
    { "Load",    ITEM_LIST, .count = nslots, .label = slot_label, .ok = slot_used, .pick = load_slot },
    { "Save",    ITEM_LIST, .confirm = true, .count = nslots, .label = slot_label, .pick = save_slot },
    { "Clear",   ITEM_LIST, .confirm = true, .count = nslots, .label = slot_label,
                 .ok = slot_used, .pick = settings_clear },
    { "Default", ITEM_LIST, .confirm = true, .count = nslots_off, .label = default_label,
                 .ok = default_ok, .pick = default_slot },
};

// ---- Source ----

static void source_label(uint i, char *s, size_t n)
{
    snprintf(s, n, "%s%s", source_get(i)->name, i == source_active_index() ? " *" : "");
}

static void source_pick(uint i)
{
    source_set_auto(false);
    source_store(i);
    settings_save_source(source_get(i)->id, false);
}

static int  get_auto(void) { return source_auto(); }

static void set_auto(int v)
{
    source_set_auto(v);
    settings_save_source(source_active()->id, v);
}

static void detect_now(void)
{
    detect_result_t r;
    detect_run(&r);
    const char *cur = source_active()->name;
    switch (r.status) {
    case DETECT_NO_SIGNAL: osd_message("Detect: no stable signal"); break;
    case DETECT_UNKNOWN:   osd_message("Detect: unknown rate, %lu Hz", (unsigned long)r.hsync_hz); break;
    case DETECT_OK: {
        const source_cand_t *c = &r.cand[r.pick];
        osd_message("Detect: %s%s", c->mode->name,
                    c->index == source_active_index() ? " (current)" : ", not selected");
        break;
    }
    default:               osd_message("Detect: unsure, keeping %s", cur); break;
    }
}

static const item_t source[] = {
    { "Select",      ITEM_LIST, .confirm = true, .count = source_count, .label = source_label,
                     .pick = source_pick },
    { "Auto detect", ITEM_TOGGLE, .confirm = true, .get = get_auto, .set = set_auto },
    { "Detect now",  ITEM_ACTION, .run = detect_now },
};

// ---- Output ----

static void mode_label(uint i, char *s, size_t n)
{
    snprintf(s, n, "%s%s", video_mode_name(i), i == video_mode_current() ? " *" : "");
}

// ---- OSD / Info ----

static void show_status(void)  { osd_status(true); }
static void show_version(void) { osd_message("ttl2dvi %s", TTL2DVI_VERSION); }
static int  get_live(void)     { return osd_live_on(); }
static void set_live(int v)    { osd_live(v); }
static int  get_hold(void)     { return video_osd_hold(); }
static void set_hold(int v)    { video_osd_set_hold(v); }

static const item_t osd[] = {
    { "Status", ITEM_ACTION, .run = show_status },
    { "Live",   ITEM_TOGGLE, .get = get_live, .set = set_live },
    { "Hold",   ITEM_TOGGLE, .get = get_hold, .set = set_hold },
};

static const item_t info[] = {
    { "Version", ITEM_ACTION, .run = show_version },
    { "Status",  ITEM_ACTION, .run = show_status },
};

#define SUB(n, list) { n, ITEM_SUB, .sub = list, .nsub = count_of(list) }

static const item_t top[] = {
    SUB("Picture",  picture),
    SUB("Sampling", sampling),
    SUB("Profiles", profiles),
    SUB("Source",   source),
    { "Output", ITEM_LIST, .confirm = true, .count = video_mode_count, .label = mode_label,
                .pick = video_set_mode },
    SUB("OSD",      osd),
    SUB("Info",     info),
};

// ---- engine ----

// A level lists either static items, or the entries of its owner's LIST.
typedef struct {
    const item_t *owner;        // NULL at the top
    const item_t *items;        // NULL: a LIST
    uint          n, sel;
} level_t;

typedef enum { BROWSE, EDIT, CONFIRM } state_t;

static level_t       s_lv[DEPTH];
static int           s_depth = -1;  // -1: closed
static state_t       s_state;
static const item_t *s_pend;        // CONFIRM: what, and with which value
static int           s_arg;
static bool          s_yes;
static uint64_t      s_last;

static level_t *lv(void) { return &s_lv[s_depth]; }

static bool visible(const level_t *l, uint i)
{
    return !l->items || !l->items[i].shown || l->items[i].shown();
}

static void move(int dir)
{
    level_t *l = lv();
    for (uint k = 0; k < l->n; k++) {
        l->sel = (l->sel + l->n + (uint)dir) % l->n;
        if (visible(l, l->sel)) return;
    }
}

static void push(const item_t *owner, const item_t *items, uint n)
{
    if (s_depth + 1 >= DEPTH) return;
    s_depth++;
    *lv() = (level_t){ owner, items, n, 0 };
    if (!visible(lv(), 0)) move(1);
}

static void menu_close(void)
{
    s_depth = -1;
    s_state = BROWSE;
    osd_menu_close();
}

static void act(const item_t *it, int arg)
{
    switch (it->kind) {
    case ITEM_TOGGLE: it->set(arg); break;
    case ITEM_LIST:   menu_close(); it->pick((uint)arg); break;
    case ITEM_ACTION: menu_close(); it->run(); break;
    default: break;
    }
}

static void ask(const item_t *it, int arg)
{
    if (!it->confirm) { act(it, arg); return; }
    s_state = CONFIRM;
    s_pend  = it;
    s_arg   = arg;
    s_yes   = false;
}

static void value(const item_t *it, char *s, size_t n)
{
    if (it->kind == ITEM_TOGGLE)  snprintf(s, n, "%s", it->get() ? "on" : "off");
    else if (it->kind != ITEM_KNOB) s[0] = 0;
    else if (it->fmt)             it->fmt(s, n, it->get());
    else                          snprintf(s, n, "%d", it->get());
}

static void draw(void)
{
    const level_t *l = lv();
    const char *title = l->owner ? l->owner->name : "Menu";
    char text[80], what[32], v[16];

    if (s_state == CONFIRM) {
        if (s_pend->kind == ITEM_LIST) s_pend->label((uint)s_arg, what, sizeof what);
        else if (s_pend->kind == ITEM_TOGGLE) snprintf(what, sizeof what, "%s %s",
                                                       s_pend->name, s_arg ? "on" : "off");
        else snprintf(what, sizeof what, "%s", s_pend->name);
        snprintf(text, sizeof text, "%-8s  %s?  %s", title, what,
                 s_yes ? " No  [Yes]" : "[No]  Yes ");
    } else if (!l->items) {
        l->owner->label(l->sel, what, sizeof what);
        snprintf(text, sizeof text, "%-8s  <  %-16s  >", title, what);
    } else {
        const item_t *it = &l->items[l->sel];
        value(it, v, sizeof v);
        if (s_state == EDIT)
            snprintf(text, sizeof text, "%-8s  >  %-12s [%6s]", title, it->name, v);
        else
            snprintf(text, sizeof text, "%-8s  <  %-12s %6s   >", title, it->name, v);
    }
    osd_menu(text);
}

static void handle(uint ev)
{
    if (s_state == CONFIRM) {
        if (ev == EV_UP || ev == EV_DOWN) s_yes = !s_yes;
        if (ev == EV_ENTER || ev == EV_BACK) {
            s_state = BROWSE;
            if (ev == EV_ENTER && s_yes) act(s_pend, s_arg);
        }
        return;
    }

    level_t *l = lv();
    const item_t *it = l->items ? &l->items[l->sel] : NULL;

    if (s_state == EDIT) {
        if (ev == EV_UP)   it->set(it->get() - 1);
        if (ev == EV_DOWN) it->set(it->get() + 1);
        if (ev == EV_ENTER || ev == EV_BACK) s_state = BROWSE;
        return;
    }

    switch (ev) {
    case EV_UP:   move(-1); break;
    case EV_DOWN: move(1);  break;
    case EV_BACK: if (--s_depth < 0) menu_close(); break;
    case EV_ENTER:
        if (!it) {
            if (!l->owner->ok || l->owner->ok(l->sel)) ask(l->owner, (int)l->sel);
            break;
        }
        switch (it->kind) {
        case ITEM_SUB:    push(it, it->sub, it->nsub); break;
        case ITEM_KNOB:   s_state = EDIT; break;
        case ITEM_TOGGLE: ask(it, !it->get()); break;
        case ITEM_LIST:   push(it, NULL, it->count()); break;
        case ITEM_ACTION: ask(it, 0); break;
        }
        break;
    }
}

void menu_poll(uint ev)
{
    const uint64_t now = time_us_64();

    if (s_depth < 0) {
        if (!(ev & EV_ENTER)) return;
        push(NULL, top, count_of(top));
        s_last = now;
        draw();
        return;
    }
    if (!ev) {
        if (now - s_last >= IDLE_US) menu_close();
        return;
    }
    s_last = now;
    for (uint b = EV_UP; b <= EV_BACK && s_depth >= 0; b <<= 1)
        if (ev & b) handle(b);
    if (s_depth >= 0) draw();
}
