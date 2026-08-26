#include "board.h"
#include "hardware/pio.h"
#include "capture.pio.h"


_Static_assert(PIN_HSYNC == 27, "capture.pio wait gpio 27");

// pio0 (base 0): capture pins 20-27. DVI owns pio1.
static PIO  pio = pio0;
static uint sm, prog_off;

void capture_init(void) {
    prog_off = pio_add_program(pio0, &capture_program);
    sm = pio_claim_unused_sm(pio0, true);

    // Video + Intensity
    pio_gpio_init(pio, PIN_VIDEO);
    pio_gpio_init(pio, PIN_INTEN);

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_VIDEO, 2, false);

    pio_sm_config c = capture_program_get_default_config(prog_off);
    sm_config_set_in_pins(&c, PIN_VIDEO);

    // autopush @32
    sm_config_set_in_shift(&c, true, true, 32);

    // 2 SM cycles × 8 = 16 sysclk = 1 MDA pixel
    sm_config_set_clkdiv(&c, 8.0f);

    pio_sm_init(pio, sm, prog_off, &c);
}