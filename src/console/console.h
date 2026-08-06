#ifndef TTL2DVI_CONSOLE_H
#define TTL2DVI_CONSOLE_H

// Line-oriented USB CDC console with a "ttl2dvi>" prompt.
//
// Commands are REGISTERED

// Longest input line. Must fit a whole `dump` blob pasted into `restore`
// (settings page + CRC, as hex) plus the command word -- grows with the slot.
#define CONSOLE_LINE_MAX 384

// Command handler. argv[0] is the command name; argc counts all tokens
// (whitespace-split). Tokens point into an internal line buffer valid only for
// the duration of the call.
typedef void (*console_cmd_fn)(int argc, char **argv);

// Register a command. `name` and `help` must be stable strings (usually string
// literals); `help` may be NULL. Call before or after console_init().
void console_register(const char *name, console_cmd_fn fn, const char *help);

// Print the banner + first prompt. Call once after stdio is up. Registers the
// built-in `help` command.
void console_init(void);

// Non-blocking: drain pending input, dispatch complete lines, reprint the
// prompt. Call every iteration of the core-0 main loop.
void console_poll(void);

#endif
