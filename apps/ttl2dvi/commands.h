#ifndef TTL2DVI_COMMANDS_H
#define TTL2DVI_COMMANDS_H

// --- diagnostics ---
void cmd_version(int argc, char **argv);
void cmd_status(int argc, char **argv);
void cmd_state(int argc, char **argv);

// --- video ---
void cmd_source(int argc, char **argv);
void cmd_mode(int argc, char **argv);
void cmd_mdalevels(int argc, char **argv);
void cmd_vscale(int argc, char **argv);
void cmd_vpos(int argc, char **argv);
void cmd_hpos(int argc, char **argv);
void cmd_scanlines(int argc, char **argv);
void cmd_dotclock(int argc, char **argv);
void cmd_fastcap(int argc, char **argv);
void cmd_measure(int argc, char **argv);
void cmd_detect(int argc, char **argv);

// --- settings slots ---
void cmd_slots(int argc, char **argv);
void cmd_save(int argc, char **argv);
void cmd_load(int argc, char **argv);
void cmd_clear(int argc, char **argv);
void cmd_default(int argc, char **argv);

// --- capture ---
void cmd_capture(int argc, char **argv);
void cmd_bp(int argc, char **argv);
void cmd_phase(int argc, char **argv);
void cmd_test(int argc, char **argv);

// --- system --- (reboot / USB BOOTSEL; neither returns)
void app_reboot(void);
void app_bootsel(void);
void cmd_reboot(int argc, char **argv);
void cmd_bootsel(int argc, char **argv);

#endif
