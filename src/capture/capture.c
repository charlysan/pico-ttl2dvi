#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
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
static uint g_dot_hz, g_oversample;
static uint g_px_cyc, g_cyc;              // sysclk per pixel (rounded), per sample
static uint32_t g_spp;                    // samples per pixel, 16.16
static uint g_bits, g_spw;                // bits/sample, samples/word
static uint g_shift;                      // bit of the first sample in a word
static uint g_active_w;
static uint g_lines, g_samples_per_line, g_words_per_line, g_delay;
static uint g_lines_prev;
static uint32_t g_line_cyc;
static bool g_armed;
static uint32_t g_frames;
static uint g_lines_lo = ~0u, g_lines_hi;
static uint g_bp;
static uint g_phase;
static int dma_chan;
static uint32_t rawbuf[RAW_WORDS];

uint capture_width(void)      { return (uint)(((uint64_t)g_samples_per_line << 16) / g_spp); }
uint capture_samples(void)    { return g_samples_per_line; }
uint capture_height(void)     { return g_lines; }
uint capture_px_cyc(void)     { return g_px_cyc; }
uint capture_sample_cyc(void) { return g_cyc; }
uint capture_dot_hz(void)     { return g_dot_hz; }
uint capture_oversample(void) { return g_oversample; }
uint32_t capture_spp(void)    { return g_spp; }
uint32_t capture_line_cycles(void) { return g_line_cyc; }
uint32_t capture_frames(void) { return g_frames; }

void capture_lines_range(uint *lo, uint *hi)
{
    *lo = g_lines_lo == ~0u ? 0 : g_lines_lo;
    *hi = g_lines_hi;
    g_lines_lo = ~0u;
    g_lines_hi = 0;
}

void capture_set_bp(int bp)
{
    if (bp < 0)           bp = 0;
    if (bp > (int)BP_MAX) bp = BP_MAX;
    g_bp = (uint)bp;
}

int capture_get_bp(void) { return g_bp; }

// 1 SM cycle = 1 sysclk = 1/g_px_cyc px
void capture_set_phase(int phase)
{
    if (phase < 0)                  phase = 0;
    if (phase > (int)g_px_cyc - 1)  phase = (int)g_px_cyc - 1;
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

// px_cyc: whole-pixel unit for bp and phase. cyc: the sample period the loop
// can hit exactly. spp from the unrounded ratio, so the view tracks the true
// pixel rate across the line. False if the loop can't run that period (two
// 5-bit delay slots: 2..64 sysclk).
//
// 1x slips a whole pixel every 0.5 / |px / round(px) - 1| pixels; if that lands
// inside the captured width, sample at 2x instead. Not at 6 bits: rawbuf can't
// hold a 2x EGA 350 frame.
typedef struct {
    uint     os, px_cyc, cyc;
    uint32_t spp;
} rate_t;

static bool rate_for(uint dot_hz, rate_t *r)
{
    const float px  = (float)clock_get_hz(clk_sys) / (float)dot_hz;
    float err = px / (float)(uint)(px + 0.5f) - 1.0f;
    if (err < 0.0f) err = -err;
    r->os     = (g_bits <= 4 && err * (float)(g_active_w + 16u) > 0.5f) ? 2u : 1u;
    r->cyc    = (uint)(px / (float)r->os + 0.5f);
    r->px_cyc = (uint)(px + 0.5f);
    r->spp    = (uint32_t)(px / (float)r->cyc * 65536.0f + 0.5f);
    return r->cyc >= 2 && r->cyc <= 64;
}

static bool derive_rate(uint dot_hz)
{
    rate_t r;
    if (!rate_for(dot_hz, &r)) return false;
    g_dot_hz     = dot_hz;
    g_oversample = r.os;
    g_px_cyc     = r.px_cyc;
    g_cyc        = r.cyc;
    g_spp        = r.spp;
    return true;
}

// Data pins and sample width. The SM must be stopped; patch_sample_loop()
// must follow, since the loop's `in pins, N` carries the width.
static void set_pins(uint base, uint bits)
{
    g_bits  = bits;
    g_spw   = 32u / bits;
    g_shift = 32u - g_spw * bits;

    for (uint i = 0; i < bits; i++)
        pio_gpio_init(pio, base + i);
    pio_sm_set_consecutive_pindirs(pio, sm, base, bits, false);

    pio_sm_config c = capture_program_get_default_config(prog_off);
    sm_config_set_in_pins(&c, base);
    // Right shift, autopush at a whole number of samples: 32 bits, or 30 at
    // 6 bits, which leaves the samples in the top 30 bits of each word.
    sm_config_set_in_shift(&c, true, true, g_spw * bits);
    sm_config_set_clkdiv(&c, PIO_CLKDIV);
    pio_sm_init(pio, sm, prog_off, &c);               // left disabled
}

// Everything that depends on the active source. The SM must be stopped.
static void apply_source(void)
{
    const source_mode_t *src = source_active();

    g_active_w = src->active_w;
    set_pins(src->data_base, src->data_bits);
    derive_rate(src->dot_hz);
    g_bp       = (uint)src->def_bp;
    g_phase    = (uint)src->def_phase;
    patch_sample_loop();
}

void capture_reconfigure(void)
{
    capture_hold();
    pio_sm_clear_fifos(pio, sm);
    apply_source();
}

// Live: re-patches the sample loop, keeps bp; phase is clamped to the new
// pixel. 0 = the source's own dot clock.
bool capture_set_dot_hz(uint dot_hz)
{
    if (!dot_hz) dot_hz = source_active()->dot_hz;
    capture_hold();
    pio_sm_clear_fifos(pio, sm);
    if (!derive_rate(dot_hz)) return false;
    patch_sample_loop();
    capture_set_phase((int)g_phase);
    return true;
}

void capture_init(void)
{
    prog_off = pio_add_program(pio, &capture_program);
    sm = pio_claim_unused_sm(pio, true);
    apply_source();

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

static uint32_t g_pulse;
static uint g_max_samples = CAPTURE_MAX_SAMPLES;

static bool fit_sampling(void)
{
    uint32_t pulse;
    uint32_t line = sync_hsync(&pulse);
    if (!line) return false;
    g_line_cyc = line;
    g_pulse    = pulse;

    uint32_t relock = 3u * g_px_cyc + 8u;

    // Whole pixels after the pulse. Remainder pulse%g_px_cyc is phase
    uint32_t delay_cyc = (pulse / g_px_cyc + g_bp) * g_px_cyc;

    // The window must fit inside the line, or the sampler runs past the next HSYNC.
    if (delay_cyc + relock + g_cyc * g_spw >= line) return false;
    g_delay = delay_cyc + g_phase;                    // sysclk

    // Up to the end of the line, but no further than the active picture plus a
    // margin: the front porch is not worth rawbuf.
    uint32_t samples = (line - delay_cyc - relock) / g_cyc;
    uint32_t cap = (uint32_t)(((uint64_t)(g_active_w + 16u) * g_spp) >> 16);
    if (cap > g_max_samples) cap = g_max_samples;
    if (samples > cap)       samples = cap;
    samples -= samples % g_spw;

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
    // Same order as the re-arm below (edge, measure, arm), so a frame started
    // here begins on the same line as one started there.
    if (!g_armed) {
        if (!wait_vsync(true) || !wait_vsync(false)) return false;
        if (!fit_sampling()) return false;
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
    if (g_lines < g_lines_lo) g_lines_lo = g_lines;
    if (g_lines > g_lines_hi) g_lines_hi = g_lines;

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

#define ALPHABET "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/"
#define FAST_CYC         2u       // sysclk per sample in fastcap
#define FAST_MAX_SAMPLES 8192u

// What a fast grab left in rawbuf, and its sample format.
typedef struct {
    uint lines, samples, words;
    uint bits, spw, shift;
    uint32_t delay;
} fast_t;

// One sample every FAST_CYC sysclk from data pins base..base+bits-1, same
// window start as normal capture (same bp, phase 0), for as many lines as
// rawbuf holds, starting skip lines after the VSYNC edge: pixel edges timed to
// FAST_CYC. Normal capture settings are restored before returning.
static bool fast_grab(uint skip, uint base, uint bits, fast_t *f)
{
    const source_mode_t *src = source_active();
    capture_hold();
    pio_sm_clear_fifos(pio, sm);

    const uint cyc = g_cyc, phase = g_phase;
    const uint32_t spp = g_spp;
    set_pins(base, bits);
    g_cyc   = FAST_CYC;
    g_spp   = (uint32_t)(((uint64_t)spp * cyc) / FAST_CYC);
    g_phase = 0;
    g_max_samples = FAST_MAX_SAMPLES;
    patch_sample_loop();

    bool ok = wait_vsync(true) && wait_vsync(false) && fit_sampling();
    f->lines = 0;
    if (ok) {
        const uint32_t mhz = clock_get_hz(clk_sys) / 1000000u;
        busy_wait_us((uint64_t)skip * g_line_cyc / mhz);
        arm();
        absolute_time_t d = make_timeout_time_ms(200);
        while (dma_channel_is_busy(dma_chan) && !time_reached(d))
            tight_loop_contents();
        pio_sm_set_enabled(pio, sm, false);
        dma_channel_abort(dma_chan);
        uint32_t written = (dma_channel_hw_addr(dma_chan)->write_addr
                            - (uintptr_t)rawbuf) / 4;
        f->lines = written / g_words_per_line;
    }
    f->samples = g_samples_per_line;
    f->words   = g_words_per_line;
    f->delay   = g_delay;
    f->bits    = g_bits;
    f->spw     = g_spw;
    f->shift   = g_shift;

    set_pins(src->data_base, src->data_bits);
    g_cyc = cyc;
    g_spp = spp;
    g_phase = phase;
    g_max_samples = CAPTURE_MAX_SAMPLES;
    patch_sample_loop();
    return ok && f->lines;
}

static inline uint fast_sample(const fast_t *f, const uint32_t *line, uint s)
{
    return (line[s / f->spw] >> (f->shift + (s % f->spw) * f->bits)) & ((1u << f->bits) - 1u);
}

// For tools/autotune.py.
void capture_dump_fast(uint skip)
{
    const source_mode_t *src = source_active();
    fast_t f;
    if (!fast_grab(skip, src->data_base, src->data_bits, &f)) {
        printf("fastcap: no sync signal\n");
        return;
    }
    const uint lines = f.lines, samples = f.samples, words = f.words;
    const uint32_t delay = f.delay;

    printf("@@@BEGIN\n");
    printf("FAST SYSCLK %lu LINE %lu PULSE %lu BP %u DELAY %lu CYC %u "
           "DOT %u BITS %u ACTIVE %u SKIP %u\n",
           (unsigned long)clock_get_hz(clk_sys), (unsigned long)g_line_cyc,
           (unsigned long)g_pulse, g_bp, (unsigned long)delay, FAST_CYC,
           g_dot_hz, g_bits, g_active_w, skip);
    printf("W %u H %u BPP %u\n", samples, lines, g_bits);
    char buf[256];
    for (uint line = 0; line < lines; line++) {
        const uint32_t *raw = &rawbuf[line * words];
        uint n = 0;
        for (uint s = 0; s < samples; s++) {
            buf[n++] = ALPHABET[fast_sample(&f, raw, s)];
            if (n == sizeof buf) { fwrite(buf, 1, n, stdout); n = 0; }
        }
        buf[n++] = '\n';
        fwrite(buf, 1, n, stdout);
    }
    printf("@@@END\n");
    fflush(stdout);
}

// Dot clock from pixel edges, as tools/autotune.py does it: every transition
// sits at t0 + k * P, so search P for the tightest alignment of edge times
// modulo P. Alignment = length of the mean unit vector of the edge phases,
// with the phase quantised to 256 steps and cos/sin from a Q14 table.
#define MEASURE_EDGES 8192u
static uint16_t s_edges[MEASURE_EDGES];
static int16_t  s_cos[256], s_sin[256];

// Also the edges' mean position within P, in sysclk from the first sample.
static float alignment_at(uint n, float P, float *mean)
{
    const float k = 256.0f / P;
    int32_t c = 0, s = 0;
    for (uint i = 0; i < n; i++) {
        const uint a = (uint)((float)s_edges[i] * k) & 255u;
        c += s_cos[a];
        s += s_sin[a];
    }
    if (mean) {
        float a = atan2f((float)s, (float)c);
        if (a < 0.0f) a += 6.2831853f;
        *mean = a / 6.2831853f * P;
    }
    return sqrtf((float)c * (float)c + (float)s * (float)s) / ((float)n * 16384.0f);
}

static float alignment(uint n, float P) { return alignment_at(n, P, NULL); }

bool capture_measure_dot(uint skip, uint base, uint bits,
                         uint lo_hz, uint hi_hz, capture_measure_t *m)
{
    if (!s_cos[0])
        for (uint a = 0; a < 256; a++) {
            s_cos[a] = (int16_t)(cosf(a * 6.2831853f / 256.0f) * 16384.0f);
            s_sin[a] = (int16_t)(sinf(a * 6.2831853f / 256.0f) * 16384.0f);
        }

    fast_t f;
    m->edges = 0;
    const bool got = fast_grab(skip, base, bits, &f);
    m->lines = got ? f.lines : 0;
    if (!got) return false;

    // Transition between samples s-1 and s: at FAST_CYC * s - FAST_CYC / 2.
    uint n = 0;
    for (uint line = 0; line < f.lines && n < MEASURE_EDGES; line++) {
        const uint32_t *raw = &rawbuf[line * f.words];
        uint prev = fast_sample(&f, raw, 0);
        for (uint s = 1; s < f.samples && n < MEASURE_EDGES; s++) {
            const uint v = fast_sample(&f, raw, s);
            if (v != prev) s_edges[n++] = (uint16_t)(FAST_CYC * s - FAST_CYC / 2);
            prev = v;
        }
    }
    m->edges = n;
    if (n < 200) return false;

    // Coarse to fine over the period range for lo_hz..hi_hz.
    const float sys = (float)clock_get_hz(clk_sys);
    float best_p = sys / (float)hi_hz, best_r = 0.0f;
    float lo = sys / (float)hi_hz, hi = sys / (float)lo_hz, step = 0.005f;
    // The coarse round only has to find the neighbourhood: 2048 edges do.
    for (uint round = 0; round < 3; round++) {
        const uint use = round == 0 && n > 2048u ? 2048u : n;
        best_r = 0.0f;
        for (float P = lo; P <= hi; P += step) {
            const float r = alignment(use, P);
            if (r > best_r) { best_r = r; best_p = P; }
        }
        lo = best_p - 4.0f * step;
        hi = best_p + 4.0f * step;
        step /= 10.0f;
    }

    m->period    = best_p;
    m->dot_hz    = (uint)(sys / best_p + 0.5f);
    m->h_total   = (float)g_line_cyc / best_p;
    m->pulse     = g_pulse;

    // Where the edges sit within a pixel, counted from the HSYNC edge: the
    // fast grab's first sample is f.delay after it, as a normal one is.
    float mean;
    m->align   = alignment_at(n, best_p, &mean);
    m->edge_at = fmodf((float)f.delay + mean, best_p);
    return true;
}

// As tools/autotune.py step 4: the window for dot_hz starts at
// (pulse / px_cyc + bp) * px_cyc + phase, and view.c takes sample
// floor((k + 0.5) * spp) for pixel k. For each phase, the worst distance from
// any of those samples to the edges; keep the phase where that's largest.
bool capture_best_phase(const capture_measure_t *m, uint dot_hz, capture_phase_t *out)
{
    rate_t r;
    if (!rate_for(dot_hz, &r)) return false;
    const float P     = (float)clock_get_hz(clk_sys) / (float)dot_hz;
    const uint  start = (m->pulse / r.px_cyc + g_bp) * r.px_cyc;
    float e = fmodf(m->edge_at - (float)start, P);
    if (e < 0.0f) e += P;

    out->phase  = 0;
    out->margin = -1.0f;
    for (uint phase = 0; phase < r.px_cyc; phase++) {
        float worst = P;
        for (uint k = 0; k < g_active_w; k++) {
            const uint j = (uint)(((uint64_t)(2u * k + 1u) * r.spp) >> 17);
            float x = (float)phase + (float)j * (float)r.cyc - e;
            x -= P * floorf(x / P);
            const float d = x < P - x ? x : P - x;
            if (d < worst) worst = d;
        }
        if (worst > out->margin) { out->margin = worst; out->phase = phase; }
    }
    const float ra = m->align > 1e-6f ? m->align : 1e-6f;
    out->spread = sqrtf(fmaxf(0.0f, -2.0f * logf(ra))) * P / 6.2831853f;
    out->os     = r.os;
    out->px_cyc = r.px_cyc;
    return true;
}

void capture_dump_frame(void)
{
    if (!capture_grab()) {
        printf("capture: no sync signal\n");
        return;
    }
    capture_hold();

    printf("@@@BEGIN\n");
    printf("W %u H %u BPP %u SPP %lu.%04lu\n", g_samples_per_line, g_lines, g_bits,
           (unsigned long)(g_spp >> 16),
           (unsigned long)(((g_spp & 0xffffu) * 10000u) >> 16));
    static uint8_t vals[CAPTURE_MAX_SAMPLES];
    static char    row[CAPTURE_MAX_SAMPLES + 1];
    for (uint line = 0; line < g_lines; line++) {
        capture_get_line(line, vals);
        for (uint s = 0; s < g_samples_per_line; s++)
            row[s] = ALPHABET[vals[s]];
        row[g_samples_per_line] = '\0';
        puts(row);
    }
    printf("@@@END\n");
    fflush(stdout);
}
