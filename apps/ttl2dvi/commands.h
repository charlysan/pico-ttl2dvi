#ifndef TTL2DVI_COMMANDS_H
#define TTL2DVI_COMMANDS_H

// --- diagnostics ---
void cmd_version(int argc, char **argv);
void cmd_status(int argc, char **argv);

// --- video ---
void cmd_source(int argc, char **argv);
void cmd_mode(int argc, char **argv);
void cmd_mdalevels(int argc, char **argv);
void cmd_vscale(int argc, char **argv);
void cmd_vpos(int argc, char **argv);
void cmd_hpos(int argc, char **argv);

// --- capture ---
void cmd_capture(int argc, char **argv);
void cmd_bp(int argc, char **argv);
void cmd_phase(int argc, char **argv);
void cmd_test(int argc, char **argv);

#endif
