#ifndef TTL2DVI_CONSOLE_H
#define TTL2DVI_CONSOLE_H

#define CONSOLE_LINE_MAX 384

typedef void (*console_cmd_fn)(int argc, char **argv);

// USB CDC (primary) / UART1 (fallback) line-oriented console.
// Call from the core-0 main loop; non-blocking poll.
void console_poll(void);

void console_register(const char *name, console_cmd_fn fn, const char *help);
void console_init(void);

#endif
