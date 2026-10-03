#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ir.h"
#include "ir_nec.pio.h"

// pio2 (base 0): pio0 is capture's, pio1 DVI's.
static PIO  pio = pio2;
static uint sm;

void ir_init(uint pin)
{
    // The SM only reads the pin, which works whatever its function is.
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);

    const uint off = pio_add_program(pio, &ir_nec_program);
    sm = pio_claim_unused_sm(pio, true);
    pio_sm_config c = ir_nec_program_get_default_config(off);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_jmp_pin(&c, pin);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) * 1.6875e-3f / 64.0f);
    pio_sm_init(pio, sm, off, &c);
    pio_sm_set_enabled(pio, sm, true);
}

bool ir_get(uint32_t *code)
{
    if (pio_sm_is_rx_fifo_empty(pio, sm)) return false;
    *code = pio_sm_get(pio, sm);
    return true;
}

bool ir_valid(uint32_t code)
{
    return code == IR_REPEAT || ((code >> 16) & 0xffu) == (~code >> 24 & 0xffu);
}
