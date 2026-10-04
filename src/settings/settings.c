#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "source.h"
#include "capture.h"
#include "video.h"
#include "view.h"
#include "settings.h"

// Bump FORMAT_VER whenever the layout changes: a stored page with another
// version is ignored rather than misread.
#define MAGIC        0x53544c54u   // "TLTS"
#define FORMAT_VER   2u
#define FLASH_OFS    (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define NO_DEFAULT   0xffu

// A load that had to switch mode, carried across the reboot:
// (PEND_MAGIC << 8) | group << 4 | slot.
#define PEND_SCRATCH 2
#define PEND_MAGIC   0x5e7d00u

typedef struct {
    uint32_t        magic;
    uint16_t        ver;
    uint8_t         def[SOURCE_GROUPS];
    uint8_t         src_id;         // last source chosen by hand (0 = none)
    uint8_t         auto_on;
    uint8_t         pad[3];
    settings_slot_t slot[SOURCE_GROUPS][SETTINGS_SLOTS];
} store_t;

_Static_assert(sizeof(settings_slot_t) == 28, "slots are stored in flash as-is");

// Flash is programmed in whole 256-byte pages.
static union {
    store_t s;
    uint8_t raw[(sizeof(store_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE];
} u;

void settings_init(void)
{
    memcpy(u.raw, (const void *)(XIP_BASE + FLASH_OFS), sizeof u.raw);
    if (u.s.magic != MAGIC || u.s.ver != FORMAT_VER) {
        memset(u.raw, 0, sizeof u.raw);
        memset(u.s.def, NO_DEFAULT, sizeof u.s.def);
    }
}

uint8_t settings_source_id(void) { return u.s.src_id; }
bool    settings_auto(void)      { return u.s.auto_on != 0; }

static settings_slot_t *slot_at(uint g, uint n)
{
    return g < SOURCE_GROUPS && n < SETTINGS_SLOTS && u.s.slot[g][n].used
         ? &u.s.slot[g][n] : NULL;
}

const settings_slot_t *settings_slot(uint n) { return slot_at(source_group(), n); }

int settings_default(void)
{
    const uint8_t d = u.s.def[source_group()];
    return d < SETTINGS_SLOTS && slot_at(source_group(), d) ? d : -1;
}

static const settings_slot_t *pending(void)
{
    const uint32_t p = watchdog_hw->scratch[PEND_SCRATCH];
    if ((p >> 8) != PEND_MAGIC || ((p >> 4) & 15u) != source_group()) return NULL;
    return slot_at(source_group(), p & 15u);
}

int settings_boot_mode(void)
{
    const settings_slot_t *t = pending();
    if (!t && settings_default() >= 0) t = settings_slot((uint)settings_default());
    return t ? t->mode : -1;
}

// dotclock first: it re-derives the pixel period, which phase is clamped to.
static void apply(const settings_slot_t *t)
{
    capture_set_dot_hz(t->dot_hz);
    capture_set_bp(t->bp);
    capture_set_phase(t->phase);
    view_set_vscale(t->vscale);
    view_set_vpos(t->vpos);
    view_set_hpos(t->hpos);
    view_set_mda_levels(t->lvl_normal, t->lvl_bright);
    video_set_scanlines(t->scanlines);
}

void settings_apply_default(void)
{
    if (settings_default() >= 0) apply(settings_slot((uint)settings_default()));
}

void settings_apply_boot(void)
{
    const settings_slot_t *t = pending();
    watchdog_hw->scratch[PEND_SCRATCH] = 0;
    if (t) apply(t);
    else   settings_apply_default();
}

bool settings_load(uint n)
{
    const settings_slot_t *t = settings_slot(n);
    if (!t) return false;
    if (t->mode == video_mode_current()) { apply(t); return true; }

    watchdog_hw->scratch[PEND_SCRATCH] = (PEND_MAGIC << 8) | source_group() << 4 | n;
    printf("slot %u is for %s: switching mode (reboot)...\n", n, video_mode_name(t->mode));
    sleep_ms(50);
    video_set_mode(t->mode);
    return true;
}

// A flash write stops DVI for good (libdvi's DMA chain runs dry and nothing
// restarts it), so core1 is stopped first and the Pico reboots after.
void settings_flash_write(uint32_t ofs, const void *data, uint len)
{
    printf("writing flash (reboot)...\n");
    sleep_ms(50);

    multicore_reset_core1();
    const uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(ofs, FLASH_SECTOR_SIZE);
    flash_range_program(ofs, data, len);
    restore_interrupts(ints);

    if (memcmp((const void *)(XIP_BASE + ofs), data, len))
        printf("flash verify FAILED\n");
    sleep_ms(50);
    watchdog_reboot(0, 0, 50);
    while (true) tight_loop_contents();
}

static void commit(void)
{
    u.s.magic = MAGIC;
    u.s.ver   = FORMAT_VER;
    settings_flash_write(FLASH_OFS, u.raw, sizeof u.raw);
}

void settings_save(uint n, const char *name)
{
    if (n >= SETTINGS_SLOTS) return;
    settings_slot_t *t = &u.s.slot[source_group()][n];
    memset(t, 0, sizeof *t);
    t->used       = 1;
    t->mode       = (uint8_t)video_mode_current();
    t->bp         = (int16_t)capture_get_bp();
    t->phase      = (uint8_t)capture_get_phase();
    t->dot_hz     = capture_dot_hz();
    t->vscale     = (uint8_t)view_get_vscale();
    t->vpos       = (int16_t)view_get_vpos();
    t->hpos       = (int16_t)view_get_hpos();
    t->scanlines  = video_get_scanlines();
    t->lvl_normal = (uint8_t)view_get_mda_normal();
    t->lvl_bright = (uint8_t)view_get_mda_bright();
    if (name) strncpy(t->name, name, SETTINGS_NAME);
    commit();
}

void settings_clear(uint n)
{
    if (n >= SETTINGS_SLOTS) return;
    memset(&u.s.slot[source_group()][n], 0, sizeof(settings_slot_t));
    if (u.s.def[source_group()] == n) u.s.def[source_group()] = NO_DEFAULT;
    commit();
}

void settings_set_default(int n)
{
    u.s.def[source_group()] = n < 0 ? NO_DEFAULT : (uint8_t)n;
    commit();
}

void settings_save_source(uint8_t id, bool auto_on)
{
    u.s.src_id  = id;
    u.s.auto_on = auto_on ? 1u : 0u;
    commit();
}
