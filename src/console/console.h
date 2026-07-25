#ifndef TTL2DVI_CONSOLE_H
#define TTL2DVI_CONSOLE_H

// USB CDC (primary) / UART1 (fallback) line-oriented console.
// Call from the core-0 main loop; non-blocking poll.
void console_poll(void);

#endif
