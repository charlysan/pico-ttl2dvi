#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "sync_period.pio.h"
#include "board.h"
#include "sync.h"

// pio0 (base 0): HSYNC/VSYNC are GPIO 26/27, within 0-31.
static PIO pio = pio0;
static uint sm_h, sm_v, prog_off;

static void sm_setup(uint sm, uint pin)
{
    pio_gpio_init(pio, pin);
    gpio_set_input_hysteresis_enabled(pin, true);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, false); // input
    pio_sm_config c = sync_period_program_get_default_config(prog_off);
    sm_config_set_in_pins(&c, pin); // `wait pin 0`
    sm_config_set_jmp_pin(&c, pin); // counting-loop pin test
    pio_sm_init(pio, sm, prog_off, &c);
    pio_sm_set_enabled(pio, sm, true);
}

void sync_init(void)
{
    prog_off = pio_add_program(pio, &sync_period_program);
    sm_h = pio_claim_unused_sm(pio, true);
    sm_v = pio_claim_unused_sm(pio, true);
    sm_setup(sm_h, PIN_HSYNC);
    sm_setup(sm_v, PIN_VSYNC);
}

// Only for a failed measure (stuck at `wait` with no edges).
static void sm_restart(uint sm)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(prog_off));
    pio_sm_set_enabled(pio, sm, true);
}

static bool rx_get(uint sm, uint32_t *v)
{
    absolute_time_t deadline = make_timeout_time_ms(250);
    while (pio_sm_is_rx_fifo_empty(pio, sm))
        if (time_reached(deadline))
            return false;
    *v = pio_sm_get(pio, sm);
    return true;
}

static uint32_t measure(uint sm, uint32_t preload, uint32_t *pulse)
{
    pio_sm_put_blocking(pio, sm, preload);
    uint32_t pr, gr;
    if (!rx_get(sm, &pr) || !rx_get(sm, &gr))
    {
        sm_restart(sm);
        if (pulse) *pulse = 0;
        return 0;
    }
    if (pr <= 1 || gr <= 1) // countdown ran out -> no edge
    {
        if (pulse) *pulse = 0;
        return 0;
    }
    uint32_t high = (preload - pr) * 2;
    uint32_t low  = (preload - gr) * 2;
    if (pulse) *pulse = high;
    return high + low;
}

uint32_t sync_hsync_period(void) { return measure(sm_h, 100000, NULL); }
uint32_t sync_vsync_period(void) { return measure(sm_v, 6000000, NULL); }
uint32_t sync_hsync(uint32_t *pulse) { return measure(sm_h, 100000, pulse); }
