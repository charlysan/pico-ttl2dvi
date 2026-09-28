#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "capture.pio.h"
#include "board.h"
#include "source.h"
#include "sync.h"
#include "capture.h"

#define PIO_CLKDIV        1.0f
#define BP_MAX            255u
#define RAW_WORDS         49000u   // one frame; lines = RAW_WORDS / words per line

_Static_assert(PIN_HSYNC == 27, "capture.pio wait gpio 27");

// pio0 (base 0): capture pins 20-27. DVI owns pio1.
static PIO pio = pio0;
static uint sm, prog_off;
static uint g_cyc, g_bits, g_spw;         // sysclk/sample, bits/sample, samples/word
static uint g_shift;                      // bit of the first sample in a word
static uint g_lines, g_samples_per_line, g_words_per_line, g_delay;
static uint g_lines_prev;
static bool g_armed;
static uint32_t g_frames;
static uint g_bp;
static uint g_phase;
static int dma_chan;
static uint32_t rawbuf[RAW_WORDS];

uint capture_width(void)      { return g_samples_per_line; }
uint capture_height(void)     { return g_lines; }
uint capture_sample_cyc(void) { return g_cyc; }
uint32_t capture_frames(void) { return g_frames; }

void capture_set_bp(int bp)
{
    if (bp < 0)           bp = 0;
    if (bp > (int)BP_MAX) bp = BP_MAX;
    g_bp = (uint)bp;
}

int capture_get_bp(void) { return g_bp; }

// 1 SM cycle = 1 sysclk = 1/g_cyc px
void capture_set_phase(int phase)
{
    if (phase < 0)                phase = 0;
    if (phase > (int)g_cyc - 1)   phase = (int)g_cyc - 1;
    g_phase = (uint)phase;
}

int capture_get_phase(void) { return g_phase; }

// Rewrite the two-instruction sample loop for this source's bit count and
// sample period, split across both delay slots. instr_mem is write-only.
static void patch_sample_loop(void)
{
    uint at = prog_off + capture_offset_sample;
    pio->instr_mem[at]     = pio_encode_in(pio_pins, g_bits)
                           | pio_encode_delay((g_cyc + 1) / 2 - 1);
    pio->instr_mem[at + 1] = pio_encode_jmp_x_dec(at)
                           | pio_encode_delay(g_cyc / 2 - 1);
}

void capture_init(void)
{
    const source_mode_t *src = source_active();
    g_cyc   = src->sample_cyc;
    g_bits  = src->data_bits;
    g_spw   = 32u / g_bits;
    g_shift = 32u - g_spw * g_bits;
    g_bp    = (uint)src->def_bp;
    g_phase = (uint)src->def_phase;

    prog_off = pio_add_program(pio, &capture_program);
    patch_sample_loop();
    sm = pio_claim_unused_sm(pio, true);

    for (uint i = 0; i < g_bits; i++)
        pio_gpio_init(pio, src->data_base + i);
    pio_sm_set_consecutive_pindirs(pio, sm, src->data_base, g_bits, false);

    pio_sm_config c = capture_program_get_default_config(prog_off);
    sm_config_set_in_pins(&c, src->data_base);
    // Right shift, autopush at a whole number of samples: 32 bits, or 30 at
    // 6 bits, which leaves the samples in the top 30 bits of each word.
    sm_config_set_in_shift(&c, true, true, g_spw * g_bits);
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

    uint32_t relock = 3u * g_cyc + 8u;

    // The delay must fit inside the line, or the sampler runs past the next HSYNC.
    if (line <= relock + g_cyc * g_spw) return false;
    uint32_t max_px = (line - relock) / g_cyc - g_spw;

    // Whole pixels after the pulse. Remainder pulse%g_cyc is phase
    uint32_t delay_px  = pulse / g_cyc + g_bp;
    if (delay_px > max_px) return false;
    uint32_t delay_cyc = delay_px * g_cyc;
    g_delay = delay_cyc + g_phase;                    // sysclk

    uint32_t need    = delay_cyc + relock;
    uint32_t avail   = (line > need) ? line - need : g_cyc;   // what's left over for sampling
    uint32_t samples = avail / g_cyc;

    if (samples > CAPTURE_MAX_WIDTH) samples = CAPTURE_MAX_WIDTH;
    samples -= samples % g_spw;
    if (samples < g_spw)             samples = g_spw;

    g_samples_per_line = (uint)samples;
    g_words_per_line   = (uint)(samples / g_spw);
    return true;
}

static void arm(void)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_clkdiv_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(prog_off));

    dma_channel_set_write_addr(dma_chan, rawbuf, false);
    dma_channel_set_trans_count(dma_chan, RAW_WORDS, false);
    dma_channel_start(dma_chan);
    pio_sm_put_blocking(pio, sm, g_samples_per_line - 1);
    pio_sm_put_blocking(pio, sm, g_delay);
    pio_sm_set_enabled(pio, sm, true);
}

void capture_hold(void)
{
    pio_sm_set_enabled(pio, sm, false);
    dma_channel_abort(dma_chan);
    g_armed = false;
}

const uint8_t *capture_raw_line(uint line)
{
    return (const uint8_t *)&rawbuf[line * g_words_per_line];
}

bool capture_grab(void)
{
    g_lines = 0;

    // An armed capture goes stale if the caller was away longer than a frame.
    if (g_armed) {
        uint32_t w = (dma_channel_hw_addr(dma_chan)->write_addr
                      - (uintptr_t)rawbuf) / 4;
        if (w >= g_lines_prev * g_words_per_line) capture_hold();
    }
    if (!g_armed) {
        if (!fit_sampling()) return false;
        if (!wait_vsync(true) || !wait_vsync(false)) return false;
        arm();
    }
    g_armed = false;

    bool ok = wait_vsync(true) && wait_vsync(false);

    pio_sm_set_enabled(pio, sm, false);
    dma_channel_abort(dma_chan);                      // FIFO can still drain
    uint32_t written = (dma_channel_hw_addr(dma_chan)->write_addr
                        - (uintptr_t)rawbuf) / 4;
    if (!ok) return false;

    g_lines = written / g_words_per_line;
    uint max_lines = RAW_WORDS / g_words_per_line;
    if (g_lines > max_lines) g_lines = max_lines;
    g_lines_prev = g_lines;
    g_frames++;

    // Re-arm on the edge we are standing on.
    // Do not wait for the next high->low pair
    if (fit_sampling()) { arm(); g_armed = true; }
    return true;
}

void capture_get_line(uint line, uint8_t *dst)
{
    const uint32_t *src  = &rawbuf[line * g_words_per_line];
    const uint32_t  mask = (1u << g_bits) - 1u;
    for (uint s = 0; s < g_samples_per_line; s++)
        dst[s] = (uint8_t)((src[s / g_spw] >> (g_shift + (s % g_spw) * g_bits)) & mask);
}

void capture_dump_frame(void)
{
    if (!capture_grab()) {
        printf("capture: no sync signal\n");
        return;
    }
    capture_hold();

    printf("@@@BEGIN\n");
    printf("W %u H %u BPP %u\n", g_samples_per_line, g_lines, g_bits);
    static uint8_t vals[CAPTURE_MAX_WIDTH];
    static char    row[CAPTURE_MAX_WIDTH + 1];
    for (uint line = 0; line < g_lines; line++) {
        capture_get_line(line, vals);
        for (uint s = 0; s < g_samples_per_line; s++)
            row[s] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/"[vals[s]];
        row[g_samples_per_line] = '\0';
        puts(row);
    }
    printf("@@@END\n");
    fflush(stdout);
}
