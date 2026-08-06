#ifndef TTL2DVI_SETTINGS_H
#define TTL2DVI_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include "pico/types.h"     // uint

// Persistent settings profiles, stored in the last flash sector.
//
// A slot captures everything you would otherwise re-dial after a power cycle:
// output mode, capture framing (bp/phase), display framing (vscale/vpos/hpos)
// and the MDA grey levels. One slot can be marked as the BOOT slot, which is
// applied automatically at startup.
//
// Layering: this module sits ABOVE video/capture/view (it reads and writes
// their live state through their getters/setters), the same way view sits above
// capture+video. Nothing below it should ever include this header.
//
// Call order at startup (see apps/ttl2dvi/main.c):
//     settings_init();                       // flash read only, no side effects
//     m = settings_boot_mode();              // -1 if no boot slot
//     if (m >= 0) video_preselect_mode(m);   // must precede video_init()
//     video_init(); sync_init(); capture_init();
//     settings_apply_boot();                 // applies the rest of the slot
//
// The mode has to be handled separately because switching it reboots -- see
// settings_load().

#define SETTINGS_SLOTS   4        // grows to 8 when CGA/EGA need their own
#define SETTINGS_NAME_MAX 14      // including the NUL

// Read the flash page into the RAM mirror. Call once, FIRST -- it touches no
// other module, so it is safe before video_init().
void settings_init(void);

// Number of slots, and whether slot n holds a profile (magic + format version;
// the content itself is not verified).
int  settings_slot_count(void);
bool settings_slot_valid(int n);

// True when a slot is valid but was saved on a DIFFERENT source card. Its
// bp/phase are in that card's pixel units and its mode index addresses that
// card's table, so load/boot refuse it rather than mis-framing the picture.
// Switch cards with `source` first, then load.
bool settings_slot_foreign(int n);

// Slot name ("" if unnamed, NULL if n is out of range or the slot is empty).
const char *settings_slot_name(int n);

// One-line human summary of slot n into buf (mode, framing, levels). Returns
// false if the slot is empty. For the `slots` listing.
bool settings_slot_summary(int n, char *buf, uint len);

// Capture the CURRENT live settings into slot n and commit to flash. `name` may
// be NULL. Returns false (having changed nothing on flash) if n is out of range
// or the write failed to verify.
//
// ALWAYS REBOOTS -- it never returns, whether the write succeeded or not, and
// prints the outcome first. Writing flash stops core1 (and so the DVI output)
// for good; see commit_and_reboot in settings.c for why that is not worth
// fighting. Harmless in practice: the values are already on flash, and a boot
// slot restores them immediately. The bool return exists only for the
// out-of-range case, which is rejected before anything is written.
bool settings_save(int n, const char *name);

// Apply slot n. If its mode matches the running one, everything is applied live
// and this returns true. If NOT, it stashes n and REBOOTS into the right mode
// (this call does not return); settings_apply_boot() then applies the rest.
bool settings_load(int n);

// Erase slot n (clears the boot slot too if it pointed there). Writes flash, so
// like settings_save it REBOOTS on success and does not return.
bool settings_clear(int n);

// Boot slot: the profile applied automatically at startup. -1 = none.
// settings_set_boot_slot writes flash, so it too REBOOTS on success.
int  settings_boot_slot(void);
bool settings_set_boot_slot(int n);      // n < 0 turns auto-load off

// The boot slot's output mode, or -1 if there is no valid boot slot. Feed this
// to video_preselect_mode() BEFORE video_init().
int  settings_boot_mode(void);

// Apply the pending slot from a settings_load() reboot, else the boot slot.
// Everything except the mode (video_init already established that). Call after
// capture_init(), so nothing overwrites the values afterwards.
void settings_apply_boot(void);

// --- backup / recovery -----------------------------------------------------
// Print the whole settings page (header + every slot) as one line of hex, ready
// to paste straight back into `restore`. Keep the output somewhere safe and a
// wiped or corrupted device is one command away from its old profiles.
void settings_dump(void);

typedef enum {
    SETTINGS_RESTORE_OK = 0,     // never actually returned -- it reboots
    SETTINGS_RESTORE_BAD_HEX,    // not hex, odd digit count, or wrong length
    SETTINGS_RESTORE_BAD_DATA,   // magic / format version mismatch
    SETTINGS_RESTORE_WRITE_FAIL, // flash write did not verify
} settings_restore_t;

// Rebuild the whole page from a `dump` blob and commit it. Whitespace in the
// hex is ignored, so a wrapped copy-paste is fine. The blob is validated
// BEFORE anything touches flash, so a bad paste leaves the device untouched.
// REBOOTS on success (like every other writer) and does not return.
settings_restore_t settings_restore_hex(const char *hex);

#endif
