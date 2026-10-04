#ifndef TTL2DVI_SETTINGS_H
#define TTL2DVI_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

// Saved knob values, 8 slots per settings group (see source_group()), in the
// last flash sector. A group's default slot is applied whenever the group
// becomes active: at boot and on an EGA family switch.

#define SETTINGS_SLOTS 8
#define SETTINGS_NAME  8

typedef struct {
    uint8_t  used;
    uint8_t  mode;
    uint8_t  vscale;
    uint8_t  scanlines;
    uint8_t  lvl_normal, lvl_bright;
    uint8_t  phase;
    uint8_t  reserved;
    int16_t  bp, vpos, hpos;
    int16_t  reserved2;
    uint32_t dot_hz;
    char     name[SETTINGS_NAME];   // not NUL-terminated when full
} settings_slot_t;

// Boot order: settings_init() first (it only reads flash), then source_init()
// with settings_source_id() / settings_auto(); settings_boot_mode() before
// video_init(); settings_apply_boot() after view_init().
void settings_init(void);
uint8_t settings_source_id(void);       // 0 = none stored
bool    settings_auto(void);
int  settings_boot_mode(void);          // mode for video_preselect_mode(), or -1
void settings_apply_boot(void);
void settings_apply_default(void);      // after an EGA family switch

const settings_slot_t *settings_slot(uint n);   // current group; NULL if empty
int  settings_default(void);                    // current group's, -1 if none

// These write flash, which stops DVI: they reboot and don't return.
void settings_save(uint n, const char *name);
void settings_clear(uint n);
void settings_set_default(int n);               // -1 = none
void settings_save_source(uint8_t id, bool auto_on);   // applies after power cycles

// Erases the sector at flash offset ofs, writes len bytes (a multiple of
// FLASH_PAGE_SIZE) and reboots. Shared with the IR key map, which has its own
// sector. Doesn't return.
void settings_flash_write(uint32_t ofs, const void *data, uint len);

// Applies slot n now. If its output mode differs, switches mode (reboots) and
// applies it after the reboot. False if the slot is empty.
bool settings_load(uint n);

#endif
