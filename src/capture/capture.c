#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "capture.pio.h"
#include "board.h"
#include "source.h"
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
#define OVERSAMPLE       1
// A sample loop iteration is 2 instructions plus two 5-bit delay slots.
#define SAMPLE_CYC_MAX   (2 + 31 + 31)
#define OUT_PIXELS_MAX       CAPTURE_MAX_WIDTH   // from capture.h (single source)
#define SAMPLES_PER_LINE_MAX (OUT_PIXELS_MAX * OVERSAMPLE)

// rawbuf is the biggest allocation in the firmware, so it is sized by TOTAL
// WORDS, not by the product of the per-axis worst cases `[why]`. Only one card
// is active per boot, and the two have opposite shapes -- MDA is narrow and
// tall, CGA640 wide and short -- so the product over-allocates by ~160 KB and
// pushes the combined build past 520 KB of SRAM:
//   MDA    : 108 words/line (864 px x2 @2bpp) x ~369 lines = ~40k words
//   CGA640 : 216 words/line (1728 samples @4bpp) x ~262 lines = ~57k words
// The line count is therefore a RUNTIME limit (g_max_lines), derived from the
// measured words-per-line, rather than a compile-time MAX_LINES.
#define RAW_WORDS_TOTAL  61440              // 240 KB
#define WORDS_PER_LINE_MAX 256              // per-line clamp; the real bound is
                                            // SAMPLES_PER_LINE_MAX below

static uint32_t g_dot_hz = 16000000;  // dot clock in use (card default, or tuned)
static uint32_t g_px_cyc = 16;   // sysclk per pixel (set from clk_sys at init)
static uint32_t g_sample_cyc = 8;    // sysclk per raw sample (patched into the loop)
static float    g_spp    = 2.0f; // raw samples per output pixel
static int      g_phase  = 4;    // sub-pixel sample phase (sysclk cycles)
static int      g_bp     = 17;   // back-porch trim in pixels (skip to active)

// Data format, from the active card. Only ONE sampler configuration is loaded
// per boot (a card switch reboots), so these are constants after capture_init.
static uint g_data_bits = 2;     // bits per sample: MDA 2, CGA 4, EGA 6
static uint g_spw       = 16;    // samples per 32-bit word = 32 / data_bits
static uint g_val_mask  = 3;     // (1 << data_bits) - 1

// Per-grab, set by fit_sampling():
static uint g_samples_per_line = SAMPLES_PER_LINE_MAX;
static uint g_words_per_line   = WORDS_PER_LINE_MAX;
static uint g_out_pixels       = OUT_PIXELS_MAX;
static uint g_delay            = 0;   // sysclk from HSYNC edge to first sample
static uint g_lines            = 0;   // captured frame height
static uint g_max_lines        = 1;   // RAW_WORDS_TOTAL / g_words_per_line

static uint32_t rawbuf[RAW_WORDS_TOTAL];

// Derive the sampling rate from a dot clock. Kept separate from capture_init
// (with patch_sample_loop below) because the two together are the whole
// rate-setting contract -- see the note at the end of this file on
// characterising a new card.
//
//   px_cyc     sysclk per source pixel, ROUNDED -- the whole-pixel window offset
//   sample_cyc sysclk per raw sample, an integer the sample loop can hit exactly
//   spp        samples per output pixel from the EXACT (unrounded) ratio, so the
//              reconstruction tracks any sub-pixel rate error across the line
//              instead of accumulating it toward the right edge
static uint32_t derive_rate(uint32_t dot_hz)
{
    float px_exact = (float)clock_get_hz(clk_sys) / (float)dot_hz;
    g_px_cyc     = (uint32_t)(px_exact + 0.5f);                 // 16 at 256 MHz (MDA)
    g_sample_cyc = (uint32_t)(px_exact / OVERSAMPLE + 0.5f);    // 8
    if (g_sample_cyc < 2)               g_sample_cyc = 2;
    if (g_sample_cyc > SAMPLE_CYC_MAX)  g_sample_cyc = SAMPLE_CYC_MAX;
    g_spp        = px_exact / (float)g_sample_cyc;              // ~2.0
    return g_sample_cyc;
}

// Write the sample-loop period + data width into the PIO program. The SM MUST
// be stopped -- capture_init runs before it is ever started.
static void patch_sample_loop(void)
{
    uint sample_at = prog_off + capture_offset_sample;
    uint extra = g_sample_cyc - 2;                 // cycles beyond the 2 instructions
    uint d1 = extra / 2, d2 = extra - d1;          // <= 31 each (5-bit delay field)
    pio->instr_mem[sample_at]     = pio_encode_in(pio_pins, g_data_bits)
                                  | pio_encode_delay(d1);
    pio->instr_mem[sample_at + 1] = pio_encode_jmp_x_dec(sample_at)
                                  | pio_encode_delay(d2);
}

void capture_init(void)
{
    g_dot_hz = g_src->dot_clock_hz;
    derive_rate(g_dot_hz);

    // Data format + per-card framing defaults from the active descriptor.
    g_data_bits = g_src->data_bits;                    // 2 (MDA) or 4 (CGA640)
    g_spw       = 32u / g_data_bits;                   // samples per 32-bit word
    g_val_mask  = (1u << g_data_bits) - 1u;
    g_bp        = g_src->def_bp;
    g_phase     = g_src->def_phase;

    prog_off = pio_add_program(pio, &capture_program);
    sm = pio_claim_unused_sm(pio, true);

    // `in pins, N` for this card's data lines plus the two delay slots, so one
    // loop iteration spans exactly g_sample_cyc sysclk. The split between the
    // slots is arbitrary -- `in` samples on the loop's first cycle either way,
    // only the period matters.
    patch_sample_loop();

    // The card's data lines are data_bits pins from data_base (MDA 20/2 =
    // VIDEO+INTENSITY, CGA640 21/4 = I,R,G,B). HSYNC (GP27) is read via
    // `wait gpio 27` and shared with sync's SM, so it is not claimed here.
    for (uint i = 0; i < g_data_bits; i++)
        pio_gpio_init(pio, g_src->data_base + i);
    pio_sm_set_consecutive_pindirs(pio, sm, g_src->data_base, g_data_bits, false);

    pio_sm_config c = capture_program_get_default_config(prog_off);
    sm_config_set_in_pins(&c, g_src->data_base);
    sm_config_set_in_shift(&c, true, true, 32);        // shift right, autopush @32
    sm_config_set_clkdiv(&c, 1.0f);                    // rate is in the patched loop
    pio_sm_init(pio, sm, prog_off, &c);                // left disabled; armed per grab

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));   // false = RX
    dma_channel_configure(dma_chan, &dc, rawbuf, &pio->rxf[sm], RAW_WORDS_TOTAL, false);
}

// Size the sampling window from the measured line: skip sync pulse + back-porch
// to active video, fill the rest of the line (minus a re-lock margin). The
// offset is a WHOLE number of pixels so it doesn't disturb the sub-pixel phase.
static bool fit_sampling(void)
{
    uint32_t pulse;
    uint32_t line = sync_hsync(&pulse);
    if (!line) return false;                           // no signal

    const uint32_t sample_cyc = g_sample_cyc;
    int32_t offset_px = (int32_t)(pulse / g_px_cyc) + g_bp;
    if (offset_px < 0) offset_px = 0;
    uint32_t offset_cyc = (uint32_t)offset_px * g_px_cyc;
    uint32_t relock = 3u * g_px_cyc + 8u;

    uint32_t avail = (line > offset_cyc + relock) ? line - offset_cyc - relock : sample_cyc;
    // Round DOWN to whole 32-bit words: g_spw samples per word (16 at 2bpp,
    // 8 at 4bpp), so a partial word never straddles the DMA boundary.
    uint32_t samples = (avail / sample_cyc) & ~(g_spw - 1u);
    // Clamp to whichever binds first -- the rawbuf line or the row buffers
    // callers size with CAPTURE_MAX_WIDTH.
    uint32_t cap = WORDS_PER_LINE_MAX * g_spw;
    if (cap > SAMPLES_PER_LINE_MAX) cap = SAMPLES_PER_LINE_MAX;
    if (samples > cap)   samples = cap;
    if (samples < g_spw) samples = g_spw;

    g_samples_per_line = samples;
    g_words_per_line   = samples / g_spw;
    g_out_pixels       = (uint)((float)samples / g_spp);
    // How many of those lines rawbuf can hold, given this line's width.
    g_max_lines        = RAW_WORDS_TOTAL / g_words_per_line;

    // capture delay that will be pulled by capture SM
    g_delay = (uint32_t)offset_px * g_px_cyc + (uint32_t)g_phase;
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
    dma_channel_set_trans_count(dma_chan, RAW_WORDS_TOTAL, false);

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
    if (g_lines > g_max_lines) g_lines = g_max_lines;
    return true;
}

// Raw sample s of a line -> its data-bits value. g_spw samples per word (16 at
// 2bpp, 8 at 4bpp); sample s sits at bit (s % g_spw) * data_bits in its word.
static inline uint sample_raw(uint line, uint s)
{
    uint32_t word = rawbuf[line * g_words_per_line + (s / g_spw)];
    return (word >> ((s % g_spw) * g_data_bits)) & g_val_mask;
}

// Geometry of the last grab (source frame the view maps onto the display).
uint capture_width(void)  { return g_out_pixels; }
uint capture_height(void) { return g_lines; }



// Horizontal framing knob (back-porch trim, whole source pixels). See capture.h.
void capture_set_bp(int bp) { g_bp = bp; }
int  capture_get_bp(void)   { return g_bp; }

// Sub-pixel sampling phase, in sysclk cycles. Clamped to one pixel: whole
// pixels are what `bp` is for, and letting it run past a pixel would eat into
// the sampling window fit_sampling() sized. See capture.h.
void capture_set_phase(int phase)
{
    if (phase < 0)                    phase = 0;
    if (phase > (int)g_px_cyc - 1)    phase = (int)g_px_cyc - 1;
    g_phase = phase;
}
int  capture_get_phase(void) { return g_phase; }

// sysclk per source pixel = the number of phase steps in one pixel.
uint capture_px_cyc(void) { return g_px_cyc; }

// Trim the assumed dot clock, for a card whose crystal differs from its
// profile's. A wrong value makes g_spp wrong, so the comb walks: the image
// comes out the wrong WIDTH (and shimmers), in proportion to the error. No
// other knob scales horizontally. Safe live -- the SM is stopped between grabs.
void capture_set_dot_hz(uint32_t hz)
{
    if (hz < 1000000u || hz > 100000000u) return;
    g_dot_hz = hz;
    derive_rate(g_dot_hz);
    patch_sample_loop();
}
uint32_t capture_get_dot_hz(void) { return g_dot_hz; }

// Reconstruct one source line into dst[0..width-1] as data-bits values (MDA
// 0..3 = VIDEO | INTENSITY<<1; CGA640 0..15 = I | R<<1 | G<<2 | B<<3). Each
// output pixel is the raw sample nearest its centre. Batched per line so the
// tight sample-pick loop stays here (direct rawbuf access); the view then
// composites the row into the framebuffer.
void capture_get_line(uint line, uint8_t *dst)
{
    for (uint k = 0; k < g_out_pixels; k++) {
        uint s = (uint)((k + 0.5f) * g_spp);       // centred sample per pixel
        if (s >= g_samples_per_line) s = g_samples_per_line - 1;
        dst[k] = (uint8_t)sample_raw(line, s);
    }
}

// Bits per reconstructed sample (2 = MDA gray, 4 = CGA RGBI). The view needs it
// to interpret capture_get_line()'s values; the dump puts it in the header.
uint capture_data_bits(void) { return g_data_bits; }

void capture_dump_frame(void)
{
    if (!capture_grab()) {
        printf("capture: no sync signal\n");
        return;
    }

    // BPP in the header so the host maps values right: 2 = MDA gray (0..3),
    // 4 = CGA colour (0..15). One hex digit per pixel covers both.
    static const char HEX[16] = "0123456789abcdef";
    printf("@@@BEGIN\n");
    printf("W %u H %u BPP %u\n", g_out_pixels, g_lines, g_data_bits);
    static uint8_t src[OUT_PIXELS_MAX];
    char row[OUT_PIXELS_MAX + 1];
    for (uint line = 0; line < g_lines; line++) {
        capture_get_line(line, src);
        for (uint k = 0; k < g_out_pixels; k++) row[k] = HEX[src[k] & 15u];
        row[g_out_pixels] = '\0';
        puts(row);
    }
    printf("@@@END\n");
    fflush(stdout);
}
