#ifndef TTL2DVI_COMMANDS_H
#define TTL2DVI_COMMANDS_H

#include "console.h"        // console_cmd_fn

// Console command implementations.

// --- diagnostics ---
void cmd_version(int argc, char **argv);
void cmd_reboot (int argc, char **argv);
void cmd_bootsel(int argc, char **argv);
void cmd_test   (int argc, char **argv);
void cmd_status (int argc, char **argv);
void cmd_capture(int argc, char **argv);

// --- source card + output mode (both reboot) ---
void cmd_source(int argc, char **argv);
void cmd_mode  (int argc, char **argv);

// --- capture framing ---
void cmd_bp      (int argc, char **argv);
void cmd_phase   (int argc, char **argv);
void cmd_dotclock(int argc, char **argv);

// --- display framing + levels ---
void cmd_vscale    (int argc, char **argv);
void cmd_vpos      (int argc, char **argv);
void cmd_hpos      (int argc, char **argv);
void cmd_mda_levels(int argc, char **argv);

// --- persistent profiles (save/clear/boot/restore reboot) ---
void cmd_slots  (int argc, char **argv);
void cmd_save   (int argc, char **argv);
void cmd_load   (int argc, char **argv);
void cmd_clear  (int argc, char **argv);
void cmd_boot   (int argc, char **argv);
void cmd_dump   (int argc, char **argv);
void cmd_restore(int argc, char **argv);

#endif
