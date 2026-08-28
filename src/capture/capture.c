#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "capture.pio.h"
#include "board.h"
#include "sync.h"
#include "capture.h"

#define PIO_CLKDIV        8.0f
#define SAMPLE_CYC        16u   // sysclk per sample: 2 SM cycles × clkdiv 8
#define SAMPLES_PER_WORD  16u   // 2bpp, autopush @32
#define BP_PX             16u   // extra pixels after the measured HSYNC pulse

_Static_assert(PIN_HSYNC == 27, "capture.pio wait gpio 27");

// pio0 (base 0): capture pins 20-27. DVI owns pio1.
static PIO pio = pio0;
static uint sm, prog_off;
static uint g_lines, g_samples_per_line, g_words_per_line, g_delay;
static uint g_bp = BP_PX;
static int dma_chan;
static uint32_t rawbuf[RAW_WORDS];

uint capture_width(void)  { return g_samples_per_line; }
uint capture_height(void) { return g_lines; }

void capture_set_bp(int bp) { g_bp = bp; }

int capture_get_bp(void) { return g_bp; }

void capture_init(void)
{
    prog_off = pio_add_program(pio, &capture_program);
    sm = pio_claim_unused_sm(pio, true);

    pio_gpio_init(pio, PIN_VIDEO);
    pio_gpio_init(pio, PIN_INTEN);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_VIDEO, 2, false);

    pio_sm_config c = capture_program_get_default_config(prog_off);
    sm_config_set_in_pins(&c, PIN_VIDEO);
    sm_config_set_in_shift(&c, true, true, 32);        // right, autopush @32
    sm_config_set_clkdiv(&c, PIO_CLKDIV);
    pio_sm_init(pio, sm, prog_off, &c);               // left disabled

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
    dma_channel_configure(dma_chan, &dc, rawbuf, &pio->rxf[sm], RAW_WORDS, false);
}

static bool wait_vsync(bool level)
{
    absolute_time_t d = make_timeout_time_ms(100);
    while (gpio_get(PIN_VSYNC) != (uint)level)
        if (time_reached(d)) return false;
    return true;
}

static bool fit_sampling(void)
{
    uint32_t pulse;
    uint32_t line = sync_hsync(&pulse);
    if (!line) return false;

    // Whole pixels after the pulse. Remainder pulse%SAMPLE_CYC is phase
    uint32_t delay_px  = pulse / SAMPLE_CYC + g_bp;
    uint32_t delay_cyc = delay_px * SAMPLE_CYC;
    g_delay = delay_px * 2u;                          // SM cycles; even = whole px

    uint32_t relock  = 3u * SAMPLE_CYC + 8u;
    uint32_t need    = delay_cyc + relock;
    uint32_t avail   = (line > need) ? line - need : SAMPLE_CYC;
    uint32_t samples = avail / SAMPLE_CYC;
    samples -= samples % SAMPLES_PER_WORD;

    if (samples > CAPTURE_MAX_WIDTH) samples = CAPTURE_MAX_WIDTH;
    if (samples < SAMPLES_PER_WORD)  samples = SAMPLES_PER_WORD;

    g_samples_per_line = (uint)samples;
    g_words_per_line   = (uint)(samples / SAMPLES_PER_WORD);
    return true;
}

bool capture_grab(void)
{
    if (!fit_sampling()) return false;

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_clkdiv_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(prog_off));

    dma_channel_set_write_addr(dma_chan, rawbuf, false);
    dma_channel_set_trans_count(dma_chan, RAW_WORDS, false);

    if (!wait_vsync(true) || !wait_vsync(false)) return false;

    dma_channel_start(dma_chan);
    pio_sm_put_blocking(pio, sm, g_samples_per_line - 1);
    pio_sm_put_blocking(pio, sm, g_delay);
    pio_sm_set_enabled(pio, sm, true);

    bool ok = wait_vsync(true) && wait_vsync(false);

    pio_sm_set_enabled(pio, sm, false);
    uint32_t written = (dma_channel_hw_addr(dma_chan)->write_addr
                        - (uintptr_t)rawbuf) / 4;
    dma_channel_abort(dma_chan);
    if (!ok) return false;

    g_lines = written / g_words_per_line;
    uint max_lines = RAW_WORDS / g_words_per_line;
    if (g_lines > max_lines) g_lines = max_lines;
    return true;
}

void capture_dump_frame(void)
{
    if (!capture_grab()) {
        printf("capture: no sync signal\n");
        return;
    }

    printf("@@@BEGIN\n");
    printf("W %u H %u BPP %u\n", g_samples_per_line, g_lines, 2);
    static char row[CAPTURE_MAX_WIDTH + 1];
    for (uint line = 0; line < g_lines; line++) {
        for (uint s = 0; s < g_samples_per_line; s++) {
            uint32_t word = rawbuf[line * g_words_per_line + (s / SAMPLES_PER_WORD)];
            uint v = (word >> ((s % SAMPLES_PER_WORD) * 2)) & 3;
            row[s] = (char)('0' + v);
        }
        row[g_samples_per_line] = '\0';
        puts(row);
    }
    printf("@@@END\n");
    fflush(stdout);
}
