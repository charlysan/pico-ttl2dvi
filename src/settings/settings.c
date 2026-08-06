#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "settings.h"
#include "source.h"
#include "video.h"
#include "capture.h"
#include "view.h"

// --- flash location --------------------------------------------------------
// The LAST sector of flash: 16 MB in on the pizero, megabytes clear of the
// ~200 KB image, so it cannot collide with code however the binary grows.
// Erase granularity is a whole 4 KB sector but the payload is one 256-byte
// page, so a save is read-modify-write of the RAM mirror + erase + one program.
#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES not defined by the board header"
#endif
#define FLASH_OFF   (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

#define HDR_MAGIC   0x7444u
#define SLOT_MAGIC  0x7455u
#define FORMAT_VER  4
#define BOOT_NONE   0xffu

// Pending-load marker across the reboot settings_load() does for a mode change.
// watchdog scratch[2]: [0]/[1] are the bootrom's USB-boot parameters, [3] is
// video's mode selection, [4] is clobbered by watchdog_reboot() itself, and the
// cga branch claims [5]/[6] for `source`. See video.c for the full map.
#define PEND_SCRATCH 2
#define PEND_MAGIC   0x7412d0u

// Written to flash verbatim, so field order/alignment is part of the format:
// changing it means bumping FORMAT_VER. Reserved words keep the offsets stable
// (they held CRCs before those were dropped) -- reuse them for new fields.
typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  source;                  // SRC_ID_* of the card it was saved on
    uint8_t  mode;                    // video mode index (WITHIN that source)
    uint8_t  lvl_normal;
    uint8_t  lvl_bright;
    uint8_t  _pad0;
    uint32_t dot_hz;                  // trimmed dot clock (0 = card default)
    int16_t  bp;
    int16_t  phase;
    int16_t  vscale;
    int16_t  vpos;
    int16_t  hpos;
    char     name[SETTINGS_NAME_MAX];
    uint16_t _rsvd0;
    uint16_t _rsvd1;
} settings_slot_t;
_Static_assert(sizeof(settings_slot_t) == 40, "slot layout changed -- bump FORMAT_VER");

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  boot_slot;               // BOOT_NONE = auto-load off
    uint16_t _rsvd0;
    uint16_t _rsvd1;
} settings_hdr_t;
_Static_assert(sizeof(settings_hdr_t) == 8, "header layout changed");

typedef struct {
    settings_hdr_t  hdr;
    settings_slot_t slot[SETTINGS_SLOTS];
} settings_page_t;
_Static_assert(sizeof(settings_page_t) <= FLASH_PAGE_SIZE, "page overflows one flash page");

static settings_page_t g_page;        // RAM mirror of the flash page

// Magic + version only: enough to reject blank flash (0xffff), a cleared slot
// (0) and an older format. Content is trusted.
static bool hdr_ok(const settings_hdr_t *h)
{
    return h->magic == HDR_MAGIC && h->version == FORMAT_VER;
}

static void hdr_seal(settings_hdr_t *h)
{
    h->magic   = HDR_MAGIC;
    h->version = FORMAT_VER;
}

static bool slot_ok(const settings_slot_t *s)
{
    return s->magic == SLOT_MAGIC && s->version == FORMAT_VER;
}

// Erase the sector and write the RAM mirror back as one page.
static bool flash_commit(void)
{
    hdr_seal(&g_page.hdr);

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xff, sizeof page);
    memcpy(page, &g_page, sizeof g_page);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_OFF, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_OFF, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    return memcmp((const void *)(XIP_BASE + FLASH_OFF), &g_page, sizeof g_page) == 0;
}

// Stop core1, commit, report, restart.
static void commit_and_reboot(void)
{
    multicore_reset_core1();

    bool ok = flash_commit();
    printf(ok ? "settings written -- rebooting\n"
              : "settings write FAILED to verify -- rebooting\n");
    sleep_ms(80);                     // let that reach the console over USB
    watchdog_reboot(0, 0, 50);
    while (1) tight_loop_contents();
}

void settings_init(void)
{
    memcpy(&g_page, (const void *)(XIP_BASE + FLASH_OFF), sizeof g_page);
    if (!hdr_ok(&g_page.hdr)) {                // blank, foreign, or older format
        memset(&g_page, 0, sizeof g_page);
        g_page.hdr.boot_slot = BOOT_NONE;
        hdr_seal(&g_page.hdr);
        // Not committed: an unwritten device stays unwritten until you `save`.
    }
}

int  settings_slot_count(void) { return SETTINGS_SLOTS; }

bool settings_slot_valid(int n)
{
    return n >= 0 && n < SETTINGS_SLOTS && slot_ok(&g_page.slot[n]);
}

const char *settings_slot_name(int n)
{
    if (!settings_slot_valid(n)) return NULL;
    return g_page.slot[n].name;
}

bool settings_slot_summary(int n, char *buf, uint len)
{
    if (!settings_slot_valid(n)) return false;
    const settings_slot_t *s = &g_page.slot[n];
    const source_mode_t *card = source_by_id(s->source);
    // Mode names are per-source, so only resolve one for a slot saved on the
    // card we are actually running -- otherwise the index means something else.
    bool mine = (s->source == source_current_id());
    snprintf(buf, len, "%-7s %-10s bp %d  phase %d  vscale %dx  vpos %d  hpos %d  lvl %d/%d",
             card ? card->name : "?",
             mine ? video_mode_name(s->mode) : "(other card)",
             s->bp, s->phase, s->vscale, s->vpos, s->hpos,
             s->lvl_normal, s->lvl_bright);
    return true;
}

// True if slot n was saved on a DIFFERENT card than the one running now.
// Its bp/phase/mode all mean something else there, so it cannot be applied.
bool settings_slot_foreign(int n)
{
    return settings_slot_valid(n) &&
           g_page.slot[n].source != source_current_id();
}

bool settings_save(int n, const char *name)
{
    if (n < 0 || n >= SETTINGS_SLOTS) return false;

    settings_slot_t *s = &g_page.slot[n];
    memset(s, 0, sizeof *s);
    s->magic      = SLOT_MAGIC;
    s->version    = FORMAT_VER;
    s->source     = source_current_id();
    s->mode       = (uint8_t)video_mode_current();
    s->dot_hz     = capture_get_dot_hz();
    s->bp         = (int16_t)capture_get_bp();
    s->phase      = (int16_t)capture_get_phase();
    s->vscale     = (int16_t)view_get_vscale();
    s->vpos       = (int16_t)view_get_vpos();
    s->hpos       = (int16_t)view_get_hpos();
    s->lvl_normal = (uint8_t)view_get_mda_lvl_normal();
    s->lvl_bright = (uint8_t)view_get_mda_lvl_bright();
    if (name && *name) {
        strncpy(s->name, name, SETTINGS_NAME_MAX - 1);
        s->name[SETTINGS_NAME_MAX - 1] = '\0';
    }

    commit_and_reboot();                 // reports and reboots; never returns
    return true;                         // unreachable
}

// Everything a slot holds EXCEPT the mode - video_init() has already applied
// that by the time this runs
static void apply_slot(const settings_slot_t *s)
{
    if (s->dot_hz) capture_set_dot_hz(s->dot_hz);   // before bp: it sets px_cyc
    capture_set_bp((int)s->bp);
    capture_set_phase((int)s->phase);
    view_set_vscale((int)s->vscale);
    view_set_vpos((int)s->vpos);
    view_set_hpos((int)s->hpos);
    view_set_mda_lvl_normal((int)s->lvl_normal);
    view_set_mda_lvl_bright((int)s->lvl_bright);
}

bool settings_load(int n)
{
    if (!settings_slot_valid(n)) return false;
    // Never apply a profile saved on another card: bp/phase are in that card's
    // pixel units and the mode index addresses that card's table, so it would
    // mis-frame rather than fail visibly. Switch cards first, then load.
    if (settings_slot_foreign(n)) return false;
    const settings_slot_t *s = &g_page.slot[n];

    if (s->mode != (uint8_t)video_mode_current()) {
        // Mode changes reboot, so the rest of the slot cannot be applied here.
        // Park the slot index and let settings_apply_boot() finish the job.
        watchdog_hw->scratch[PEND_SCRATCH] = (PEND_MAGIC << 8) | (uint32_t)(n & 0xff);
        video_set_mode(s->mode);              // does not return
    }
    apply_slot(s);
    return true;
}

bool settings_clear(int n)
{
    if (n < 0 || n >= SETTINGS_SLOTS) return false;
    memset(&g_page.slot[n], 0, sizeof g_page.slot[n]);
    if (g_page.hdr.boot_slot == (uint8_t)n)            // don't boot into nothing
        g_page.hdr.boot_slot = BOOT_NONE;
    commit_and_reboot();                               // seals the header; never returns
    return true;                                       // unreachable
}

int settings_boot_slot(void)
{
    uint8_t b = g_page.hdr.boot_slot;
    return (b < SETTINGS_SLOTS) ? (int)b : -1;
}

bool settings_set_boot_slot(int n)
{
    if (n >= SETTINGS_SLOTS) return false;
    if (n >= 0 && !settings_slot_valid(n)) return false;   // never arm an empty slot
    g_page.hdr.boot_slot = (n < 0) ? BOOT_NONE : (uint8_t)n;
    commit_and_reboot();                                   // seals the header; never returns
    return true;                                           // unreachable
}

int settings_boot_mode(void)
{
    int b = settings_boot_slot();
    // A boot slot from the other card is ignored rather than applied -- its
    // mode index addresses a different table. Same rule as settings_load().
    if (b < 0 || !settings_slot_valid(b) || settings_slot_foreign(b)) return -1;
    return (int)g_page.slot[b].mode;
}

// --- backup / recovery -----------------------------------------------------

#define DUMP_BYTES (sizeof(settings_page_t))

void settings_dump(void)
{
    const uint8_t *p = (const uint8_t *)&g_page;
    printf("settings dump: format v%d, %u bytes -- paste back with `restore <hex>`\n",
           FORMAT_VER, (unsigned)DUMP_BYTES);
    for (uint i = 0; i < sizeof g_page; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

settings_restore_t settings_restore_hex(const char *hex)
{
    // Decode into a TEMPORARY buffer
    uint8_t raw[DUMP_BYTES];
    uint n = 0;
    int  hi = -1;

    for (const char *p = hex; *p; p++) {
        int v = hexval(*p);
        if (v < 0) continue;                       // skip spaces / wrapping
        if (hi < 0) { hi = v; continue; }
        if (n >= sizeof raw) return SETTINGS_RESTORE_BAD_HEX;   // too long
        raw[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    if (hi >= 0 || n != sizeof raw) return SETTINGS_RESTORE_BAD_HEX;

    settings_page_t tmp;
    memcpy(&tmp, raw, sizeof tmp);

    // Header magic + version: rejects a blob from a different firmware format.
    // Nothing verifies the CONTENT -- a hand-edited byte is accepted.
    if (!hdr_ok(&tmp.hdr)) return SETTINGS_RESTORE_BAD_DATA;
    if (tmp.hdr.boot_slot != BOOT_NONE && tmp.hdr.boot_slot >= SETTINGS_SLOTS)
        return SETTINGS_RESTORE_BAD_DATA;

    g_page = tmp;
    commit_and_reboot();                           // never returns
    return SETTINGS_RESTORE_WRITE_FAIL;            // unreachable
}

void settings_apply_boot(void)
{
    uint32_t p = watchdog_hw->scratch[PEND_SCRATCH];
    watchdog_hw->scratch[PEND_SCRATCH] = 0;            // consume: one-shot
    if ((p >> 8) == PEND_MAGIC) {
        int n = (int)(p & 0xffu);
        if (settings_slot_valid(n) && !settings_slot_foreign(n)) {
            apply_slot(&g_page.slot[n]);
            return;
        }
    }
    int b = settings_boot_slot();
    if (b >= 0 && settings_slot_valid(b) && !settings_slot_foreign(b))
        apply_slot(&g_page.slot[b]);
}
