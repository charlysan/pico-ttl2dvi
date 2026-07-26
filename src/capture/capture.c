#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "capture.pio.h"
#include "board.h"
#include "sync.h"
#include "capture.h"

// capture.pio hardcodes `wait gpio 27` for the HSYNC edge (fixed across
// MDA/CGA/EGA). board.h is the source of truth; this catches any drift.
_Static_assert(PIN_HSYNC == 27, "capture.pio hardcodes HSYNC at gpio 27 -- update it if PIN_HSYNC changes");

// pio0 (base 0): capture pins 20-27 are within 0-31. DVI owns pio1.
static PIO  pio = pio0;
static uint sm, prog_off;
static int  dma_chan;

// --- sampling geometry -----------------------------------------------------
// 2x oversample (an integer PIO divider -> dither-free). At 256 MHz the dot
// clock (~16 MHz) is an integer 16 sysclk/pixel (K=16), so:
//   px_cyc  = 16   (sysclk per source pixel; whole-pixel window offset)
//   spp     = px_cyc / (2*clkdiv) = 2.0  (raw samples per output pixel)
// These come from the shared 16 MHz dot clock, so they hold for BOTH text and
// graphics; only the LINE LENGTH differs, which is why the window is measured.

// s = (k + 0.5) * spp
//  - At 2× (spp=2.0): s = (k+0.5)*2 = 2k+1  -> picks the odd sample, which sits between the two samples of pixel k = the pixel center. Crisp.
//  - At 1× (spp=1.0): s = (k+0.5)*1 = k+0.5 -> truncates to k -> picks the sample at the pixel edge. 
//    The half-pixel centering is lost, so every sample lands on a transition edge -> jitter/shimmer.
#define OVERSAMPLE       2
#define PIO_CLKDIV       (8.0f / OVERSAMPLE)          // 4.0 at 2x
#define OUT_PIXELS_MAX       CAPTURE_MAX_WIDTH   // from capture.h (single source)
#define SAMPLES_PER_LINE_MAX (OUT_PIXELS_MAX * OVERSAMPLE)
#define WORDS_PER_LINE_MAX   (SAMPLES_PER_LINE_MAX / 16)   // 2bpp: 16 samples/word
#define MAX_LINES        400
#define WORDS_TOTAL      (WORDS_PER_LINE_MAX * MAX_LINES)

static uint32_t g_px_cyc = 16;   // sysclk per pixel (set from clk_sys at init)
static float    g_spp    = 2.0f; // raw samples per output pixel
static int      g_phase  = 2;    // sub-pixel sample phase (SM cycles; even=centre)
static int      g_bp     = 16;   // back-porch trim in pixels (skip to active)

// Per-grab, set by fit_sampling():
static uint g_samples_per_line = SAMPLES_PER_LINE_MAX;
static uint g_words_per_line   = WORDS_PER_LINE_MAX;
static uint g_out_pixels       = OUT_PIXELS_MAX;
static uint g_delay            = 0;   // SM cycles from HSYNC edge to first sample
static uint g_lines            = 0;   // captured frame height

static uint32_t rawbuf[WORDS_TOTAL];

void capture_init(void)
{
    g_px_cyc = clock_get_hz(clk_sys) / 16000000u;      // ~16 at 256 MHz
    g_spp    = (float)g_px_cyc / (2.0f * PIO_CLKDIV);   // ~2.0

    prog_off = pio_add_program(pio, &capture_program);
    sm = pio_claim_unused_sm(pio, true);

    pio_gpio_init(pio, PIN_VIDEO);
    pio_gpio_init(pio, PIN_INTEN);
    // HSYNC pad is shared with sync's SM (already input); the sampler waits on
    // it via `wait gpio 27`. VIDEO+INTENSITY are the 2 sampled input pins.
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_VIDEO, 2, false);

    pio_sm_config c = capture_program_get_default_config(prog_off);
    sm_config_set_in_pins(&c, PIN_VIDEO);
    sm_config_set_in_shift(&c, true, true, 32);        // shift right, autopush @32
    sm_config_set_clkdiv(&c, PIO_CLKDIV);
    pio_sm_init(pio, sm, prog_off, &c);                // left disabled; armed per grab

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));   // false = RX
    dma_channel_configure(dma_chan, &dc, rawbuf, &pio->rxf[sm], WORDS_TOTAL, false);
}

// Size the sampling window from the measured line: skip sync pulse + back-porch
// to active video, fill the rest of the line (minus a re-lock margin). The
// offset is a WHOLE number of pixels so it doesn't disturb the sub-pixel phase.
static bool fit_sampling(void)
{
    uint32_t pulse;
    uint32_t line = sync_hsync(&pulse);
    if (!line) return false;                           // no signal

    const uint32_t sample_cyc = 2u * (uint32_t)PIO_CLKDIV;    // 8
    int32_t offset_px = (int32_t)(pulse / g_px_cyc) + g_bp;
    if (offset_px < 0) offset_px = 0;
    uint32_t offset_cyc = (uint32_t)offset_px * g_px_cyc;
    uint32_t relock = 3u * g_px_cyc + 8u;

    uint32_t avail = (line > offset_cyc + relock) ? line - offset_cyc - relock : sample_cyc;
    uint32_t samples = (avail / sample_cyc) & ~15u;    // whole 2bpp words (16 samples)
    if (samples > SAMPLES_PER_LINE_MAX) samples = SAMPLES_PER_LINE_MAX;
    if (samples < 16) samples = 16;

    g_samples_per_line = samples;
    g_words_per_line   = samples / 16;
    g_out_pixels       = samples / OVERSAMPLE;

    // capture delay that will be pulled by capture SM
    g_delay = (uint32_t)offset_px * (g_px_cyc / (uint32_t)PIO_CLKDIV) + g_phase;
    return true;
}

// Bounded wait for VSYNC to reach `level`; false on timeout (no signal).
static bool wait_vsync(bool level)
{
    absolute_time_t d = make_timeout_time_ms(100);     // > one ~20 ms frame
    while (gpio_get(PIN_VSYNC) != level)
        if (time_reached(d)) return false;
    return true;
}

// Grab one VSYNC-bounded frame into rawbuf. Returns false on no-signal timeout.
// Public: both the live view (view_render) and the USB dump grab this way.
bool capture_grab(void)
{
    if (!fit_sampling()) return false;

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_clkdiv_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(prog_off));

    dma_channel_set_write_addr(dma_chan, rawbuf, false);
    dma_channel_set_trans_count(dma_chan, WORDS_TOTAL, false);

    // Frame start: VSYNC idle-high, then its falling edge (MDA VSYNC active low).
    if (!wait_vsync(true) || !wait_vsync(false)) return false;

    dma_channel_start(dma_chan);
    pio_sm_put_blocking(pio, sm, g_samples_per_line - 1); // feeds pull #1
    pio_sm_put_blocking(pio, sm, g_delay);                // feeds pull #2 (delay)
    pio_sm_set_enabled(pio, sm, true); 

    // Run to the next VSYNC = exactly one frame.
    bool ok = wait_vsync(true) && wait_vsync(false);

    pio_sm_set_enabled(pio, sm, false);
    uint32_t written = (dma_channel_hw_addr(dma_chan)->write_addr
                        - (uint32_t)(uintptr_t)rawbuf) / 4;
    dma_channel_abort(dma_chan);
    if (!ok) return false;

    g_lines = written / g_words_per_line;
    if (g_lines > MAX_LINES) g_lines = MAX_LINES;
    return true;
}

// Raw 2-bit sample s of a line: 16 samples/word, sample s at bits [2s+1:2s].
static inline uint sample_2bpp(uint line, uint s)
{
    uint32_t word = rawbuf[line * g_words_per_line + (s >> 4)];
    return (word >> ((s & 15u) << 1)) & 3u;
}

// Geometry of the last grab (source frame the view maps onto the display).
uint capture_width(void)  { return g_out_pixels; }
uint capture_height(void) { return g_lines; }

// Reconstruct one source line into dst[0..width-1] as 2-bit values (0..3 =
// VIDEO | INTENSITY<<1). Each output pixel is the raw sample nearest its centre.
// Batched per line so the tight sample-pick loop stays here (direct rawbuf
// access); the view then composites the row into the framebuffer.
void capture_get_line(uint line, uint8_t *dst)
{
    for (uint k = 0; k < g_out_pixels; k++) {
        uint s = (uint)((k + 0.5f) * g_spp);       // centred sample per pixel
        if (s >= g_samples_per_line) s = g_samples_per_line - 1;
        dst[k] = (uint8_t)sample_2bpp(line, s);
    }
}

void capture_dump_frame(void)
{
    if (!capture_grab()) {
        printf("capture: no sync signal\n");
        return;
    }

    printf("@@@BEGIN\n");
    printf("W %u H %u\n", g_out_pixels, g_lines);
    static uint8_t src[OUT_PIXELS_MAX];
    char row[OUT_PIXELS_MAX + 1];
    for (uint line = 0; line < g_lines; line++) {
        capture_get_line(line, src);
        for (uint k = 0; k < g_out_pixels; k++) row[k] = (char)('0' + src[k]);
        row[g_out_pixels] = '\0';
        puts(row);
    }
    printf("@@@END\n");
    fflush(stdout);
}
