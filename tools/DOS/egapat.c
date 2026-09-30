/* egapat.c -- EGA test-pattern generator for ttl2dvi.
   Turbo C 2.x/3.x, small model, run on the PC with the EGA card.

   Keys (mode / pattern):
     1 2 3   mode 0Dh  320x200x16   bars / stripes / checkerboard
     4 5 6   mode 0Eh  640x200x16   bars / stripes / checkerboard
     7 8 9   mode 10h  640x350x16   bars / stripes / checkerboard
     b       mode 10h  BIT WALK -- one signal line per bar (wiring test)
     v       mode 0Eh  BIT WALK at 200 lines (see note below)
     c       mode 10h  64-COLOUR SWEEP -- 14 labelled swatches, paged
     s       mode 10h  all 64 at once via split-screen palette reload
     t       mode 3 text, 16 colour bars
     q       quit

   SCAN RATE differs per mode, and so does the number of live signal lines:
     0Dh / 0Eh   15.7 kHz, 200 lines -- the card drives RGBI ONLY (pins 3,4,5,6).
                 An Enhanced Color Display runs these as a CGA monitor, so
                 secondary Red (pin 2) and secondary Blue (pin 7) stay low.
     10h / 3     21.8 kHz, 350 lines -- all SIX lines live, 16 colours chosen
                 from 64. This is the only place the 6-bit path can be tested.

   That is what `v` is for. The bit walk paints 8 bars, each driving ONE line;
   run it in both modes and the capture should read:

     bar  drives      350-line (`b`)   200-line (`v`)
      0   black          0x00             0x00
      1   sBlue          0x01             0x00   <- dark, line not driven
      2   sGreen         0x02             0x01
      3   Red            0x04             0x02
      4   Green          0x08             0x04
      5   Blue           0x10             0x08
      6   sRed           0x20             0x00   <- dark, line not driven
      7   all            0x3f             0x0f */

#include <dos.h>
#include <conio.h>
#include <mem.h>
#include <stdio.h>

#define SC_INDEX  0x3C4          /* Sequencer */
#define SC_DATA   0x3C5
#define GC_INDEX  0x3CE          /* Graphics Controller */
#define GC_DATA   0x3CF

#define PAT_BARS16   0           /* 16 vertical colour bars */
#define PAT_STRIPES  1           /* 1px vertical stripes, 15/0 */
#define PAT_CHECKER  2           /* 1px checkerboard */
#define PAT_BITWALK  3           /* 8 bars, palette walks one bit at a time */

static void set_mode(int mode)
{
    union REGS r;
    r.h.ah = 0x00; r.h.al = (unsigned char)mode;
    int86(0x10, &r, &r);
}

/* Attribute Controller palette register: AH=10h AL=00h, BL=reg, BH=6-bit value.
   Value is 00rgbRGB -- bit0 B, bit1 G, bit2 R, bit3 sB, bit4 sG, bit5 sR. */
static void set_pal_reg(int reg, int val)
{
    union REGS r;
    r.h.ah = 0x10; r.h.al = 0x00;
    r.h.bl = (unsigned char)reg;
    r.h.bh = (unsigned char)val;
    int86(0x10, &r, &r);
}

/* Select which planes a CPU write lands in (Sequencer Map Mask). */
static void set_plane(int p)
{
    outportb(SC_INDEX, 2);
    outportb(SC_DATA, (unsigned char)(1 << p));
}

/* Put the Graphics Controller in plain "write mode 0, replace" so a byte
   written to VRAM goes through untouched. These are the BIOS defaults after a
   mode set, but a previous program may have left them elsewhere. */
static void gc_reset(void)
{
    outportb(GC_INDEX, 0); outportb(GC_DATA, 0x00);   /* set/reset          */
    outportb(GC_INDEX, 1); outportb(GC_DATA, 0x00);   /* enable set/reset   */
    outportb(GC_INDEX, 3); outportb(GC_DATA, 0x00);   /* function = replace */
    outportb(GC_INDEX, 5); outportb(GC_DATA, 0x00);   /* write mode 0       */
    outportb(GC_INDEX, 8); outportb(GC_DATA, 0xFF);   /* bit mask = all     */
}

static int color_at(int x, int y, int pat, int width)
{
    switch (pat) {
    case PAT_BARS16:  return (x * 16) / width;        /* 0..15 */
    case PAT_STRIPES: return (x & 1) ? 15 : 0;
    case PAT_CHECKER: return ((x ^ y) & 1) ? 15 : 0;
    case PAT_BITWALK: return (x * 8) / width;         /* 0..7  */
    }
    return 0;
}

/* Draw a whole screen, one plane at a time. Every pattern here depends only on
   x and on y's PARITY, so two scanlines are built per plane and blasted down
   the screen -- four passes over VRAM instead of a per-pixel BIOS call. */
static void draw_pixels(int width, int wbytes, int height, int pat)
{
    unsigned char far *v = (unsigned char far *)MK_FP(0xA000, 0x0000);
    unsigned char l0[80], l1[80];
    int p, b, bit, x, y;

    for (p = 0; p < 4; p++) {
        set_plane(p);
        for (b = 0; b < wbytes; b++) {
            unsigned char b0 = 0, b1 = 0;
            for (bit = 0; bit < 8; bit++) {
                x = b * 8 + bit;                      /* MSB = leftmost pixel */
                if ((color_at(x, 0, pat, width) >> p) & 1)
                    b0 |= (unsigned char)(0x80 >> bit);
                if ((color_at(x, 1, pat, width) >> p) & 1)
                    b1 |= (unsigned char)(0x80 >> bit);
            }
            l0[b] = b0; l1[b] = b1;
        }
        for (y = 0; y < height; y++)
            _fmemcpy(v + (long)y * wbytes, (y & 1) ? l1 : l0, wbytes);
    }
}

static void draw(int mode, int width, int wbytes, int height, int pat)
{
    set_mode(mode);
    gc_reset();
    draw_pixels(width, wbytes, height, pat);
}

/* Eight bars driving ONE output line each: black, then sB, sG, R, G, B, sR,
   then white. */
static void bitwalk(int mode, int width, int wbytes, int height)
{
    static const int vals[8] = { 0x00,   /* black                */
                                 0x08,   /* secondary Blue  only */
                                 0x10,   /* secondary Green only */
                                 0x04,   /* primary Red     only */
                                 0x02,   /* primary Green   only */
                                 0x01,   /* primary Blue    only */
                                 0x20,   /* secondary Red   only */
                                 0x3F }; /* all six              */
    int i;
    /* Order matters. set_mode restores the DEFAULT palette, and the EGA BIOS
       palette call waits for vertical retrace -- so programming 8 registers
       takes ~130 ms. Do it BEFORE painting, or the screen is briefly drawn in
       the default colours and wipes across to these one bar at a time. */
    set_mode(mode);
    gc_reset();
    for (i = 0; i < 8; i++) set_pal_reg(i, vals[i]);
    draw_pixels(width, wbytes, height, PAT_BITWALK);
}

/* ---- 64-colour sweep (mode 10h) ---------------------------------------- */
/* EGA can only display 16 colours at once -- there are 16 Attribute Controller
   palette registers and nothing else drives the pins -- so all 64 need PAGING.
   Two registers are reserved so the page stays readable:
       reg 0  = 0x00 black, the background
       reg 15 = 0x3F white, the labels
   leaving 14 swatches per page, so 5 pages cover 0x00..0x3F. Each swatch is
   labelled with its own 6-bit value, which is what ttl2dvi should capture.
   Press `c` again to advance a page. */

#define SW_COLS 7
#define SW_ROWS 2
#define SW_N    (SW_COLS * SW_ROWS)          /* 14 per page */
#define SW_X0   16                           /* left margin, px             */
#define SW_PIT  88                           /* column pitch = 11 char cells */
#define SW_W    80                           /* swatch width, px            */

/* Palette index for a point in one of the two swatch bands: 0 outside. */
static int swatch_at(int x, int band)
{
    int col, in;
    if (x < SW_X0) return 0;
    col = (x - SW_X0) / SW_PIT;
    in  = (x - SW_X0) - col * SW_PIT;
    if (col >= SW_COLS || in >= SW_W) return 0;
    return band * SW_COLS + col + 1;         /* 1..14 */
}

static void gotoxy_bios(int col, int row)
{
    union REGS r;
    r.h.ah = 0x02; r.h.bh = 0;
    r.h.dh = (unsigned char)row; r.h.dl = (unsigned char)col;
    int86(0x10, &r, &r);
}

/* AH=0Eh teletype: in a graphics mode this renders the BIOS 8x14 font with BL
   as the foreground colour -- so the text is palette index 15. */
static void puts_bios(const char *s, int color)
{
    union REGS r;
    while (*s) {
        r.h.ah = 0x0E; r.h.al = (unsigned char)*s++;
        r.h.bh = 0; r.h.bl = (unsigned char)color;
        int86(0x10, &r, &r);
    }
}

static void hex2(int v, char *out)
{
    static const char H[16] = "0123456789abcdef";
    out[0] = H[(v >> 4) & 15]; out[1] = H[v & 15]; out[2] = '\0';
}

static void sweep64(int page)
{
    unsigned char far *v = (unsigned char far *)MK_FP(0xA000, 0x0000);
    unsigned char band[2][80], blank[80];
    char lbl[8];
    int p, b, bit, x, y, i;

    set_mode(0x10);
    gc_reset();

    /* Palette BEFORE painting -- see the note in bitwalk(). */
    set_pal_reg(0, 0x00);
    set_pal_reg(15, 0x3F);
    for (i = 0; i < SW_N; i++) {
        int val = page * SW_N + i;
        set_pal_reg(i + 1, val < 64 ? val : 0x00);
    }

    _fmemset(blank, 0, 80);
    for (p = 0; p < 4; p++) {
        set_plane(p);
        for (b = 0; b < 80; b++) {
            unsigned char b0 = 0, b1 = 0;
            for (bit = 0; bit < 8; bit++) {
                x = b * 8 + bit;
                if ((swatch_at(x, 0) >> p) & 1) b0 |= (unsigned char)(0x80 >> bit);
                if ((swatch_at(x, 1) >> p) & 1) b1 |= (unsigned char)(0x80 >> bit);
            }
            band[0][b] = b0; band[1][b] = b1;
        }
        for (y = 0; y < 350; y++) {
            unsigned char *src = blank;
            if (y <  126)             src = band[0];   /* char rows 0-8   */
            else if (y >= 154 && y < 280) src = band[1];   /* char rows 11-19 */
            _fmemcpy(v + (long)y * 80, src, 80);
        }
    }

    /* Labels: 8x14 cells, so 80x25. Swatch column c starts at char col
       2 + 11c; centre a 2-char label in the 10-cell swatch. */
    for (i = 0; i < SW_N; i++) {
        int val = page * SW_N + i;
        if (val >= 64) break;
        hex2(val, lbl);
        gotoxy_bios(2 + (i % SW_COLS) * 11 + 4, (i / SW_COLS) ? 20 : 9);
        puts_bios(lbl, 15);
    }
    gotoxy_bios(2, 23);
    puts_bios("EGA 64-colour sweep - press c for next page", 15);
}

/* ---- 64 colours on ONE screen, split-screen (mode 10h) ------------------ */
/* Only 16 palette registers exist, so 64 at once is impossible in a static
   frame. This paints four horizontal BANDS of 16 swatches and rewrites all 16
   registers at each band boundary, so the card really does emit 64 distinct
   6-bit values in one frame -- one `capture` grab then exercises the whole
   palette, and it is the only pattern here that tests a card whose output
   changes character mid-frame. */

#define AC_INDEX  0x3C0          /* Attribute Controller: index AND data     */
#define IS1       0x3DA          /* Input Status 1: bit0 display-enable,
                                    bit3 vertical retrace; reading it also
                                    resets the AC index/data flip-flop        */
#define SPLIT_BANDS 4
#define BAND_H      (350 / SPLIT_BANDS)   /* 87 lines each */

/* Rewrite all 16 palette registers. Must be fast -- no BIOS. */
static void ac_palette(const unsigned char *vals)
{
    int i;
    (void)inportb(IS1);                  /* flip-flop -> index state */
    for (i = 0; i < 16; i++) {
        outportb(AC_INDEX, (unsigned char)i);
        outportb(AC_INDEX, vals[i]);
    }
    outportb(AC_INDEX, 0x20);            /* set bit5: re-enable video */
}

/* Spin until the start of the next vertical retrace, then until it ends --
   leaves us at the top of the active display. */
static void wait_vretrace(void)
{
    while (inportb(IS1) & 0x08) ;        /* wait out any retrace in progress */
    while (!(inportb(IS1) & 0x08)) ;     /* wait for it to begin             */
    while (inportb(IS1) & 0x08) ;        /* wait for it to end               */
}

/* Count `n` horizontal blanking intervals. Bit0 of IS1 is set during blanking
   (display NOT enabled), so one line is a low->high->low transit. */
static void wait_lines(int n)
{
    while (n--) {
        while (inportb(IS1) & 0x01) ;    /* wait for active video   */
        while (!(inportb(IS1) & 0x01)) ; /* wait for the next blank */
    }
}

static void sweep64_split(void)
{
    unsigned char far *v = (unsigned char far *)MK_FP(0xA000, 0x0000);
    unsigned char line[80], pal[SPLIT_BANDS][16];
    int p, b, bit, x, y, i, band;

    set_mode(0x10);
    gc_reset();

    /* 16 swatches across, 40 px each; the SAME pixel values in every band --
       only the palette differs, which is the whole point. */
    for (p = 0; p < 4; p++) {
        set_plane(p);
        for (b = 0; b < 80; b++) {
            unsigned char byte = 0;
            for (bit = 0; bit < 8; bit++) {
                x = b * 8 + bit;
                if ((((x * 16) / 640) >> p) & 1) byte |= (unsigned char)(0x80 >> bit);
            }
            line[b] = byte;
        }
        for (y = 0; y < 350; y++) _fmemcpy(v + (long)y * 80, line, 80);
    }

    for (band = 0; band < SPLIT_BANDS; band++)
        for (i = 0; i < 16; i++)
            pal[band][i] = (unsigned char)(band * 16 + i);

    /* Rewrite the palette every frame until a key; interrupts off so the timer ISR can't make band boundaries jitter. */
    while (!kbhit()) {
        disable();
        wait_vretrace();
        ac_palette(pal[0]);
        for (band = 1; band < SPLIT_BANDS; band++) {
            wait_lines(BAND_H);
            ac_palette(pal[band]);
        }
        enable();                        /* let the tick and keyboard through */
    }
    (void)getch();
}

/* 80x25 text, 16 vertical colour bars: char 0xDB (full block), fg 0..15.
   On an Enhanced Color Display this is a 350-line mode, so all six lines are
   live -- the text-mode counterpart of `b`. */
static void bars_text16(void)
{
    unsigned char far *vram = (unsigned char far *)MK_FP(0xB800, 0x0000);
    int r, c;
    set_mode(3);
    for (r = 0; r < 25; r++)
        for (c = 0; c < 80; c++) {
            int i = (r * 80 + c) * 2;
            vram[i]     = 0xDB;
            vram[i + 1] = (unsigned char)((c * 16) / 80);
        }
}

int main(void)
{
    int k, sweep_page = 0;
    printf("EGA pattern generator (ttl2dvi). Keys:\n");
    printf("  0Dh 320x200x16 (15.7 kHz, RGBI only)   1 bars  2 stripes  3 checker\n");
    printf("  0Eh 640x200x16 (15.7 kHz, RGBI only)   4 bars  5 stripes  6 checker\n");
    printf("  10h 640x350x16 (21.8 kHz, all 6 bits)  7 bars  8 stripes  9 checker\n");
    printf("  b  bit walk, 350 lines -- 8 bars, one signal line each\n");
    printf("  v  bit walk, 200 lines -- 2nd and 7th bars dark (pins 2,7 idle)\n");
    printf("  c  64-colour sweep, 350 lines -- 14 labelled swatches per page,\n");
    printf("     press again to page through all 64 (only 16 fit at once)\n");
    printf("  s  all 64 on ONE screen via split-screen palette reload\n");
    printf("     (4 bands x 16; holds until a key; black line at each boundary)\n");
    printf("  t  text 80x25, 16 colour bars (350 lines)\n");
    printf("  q  quit\n");
    printf("press a key...\n");

    while ((k = getch()) != 'q') {
        switch (k) {
        case '1': draw(0x0D, 320, 40, 200, PAT_BARS16);  break;
        case '2': draw(0x0D, 320, 40, 200, PAT_STRIPES); break;
        case '3': draw(0x0D, 320, 40, 200, PAT_CHECKER); break;
        case '4': draw(0x0E, 640, 80, 200, PAT_BARS16);  break;
        case '5': draw(0x0E, 640, 80, 200, PAT_STRIPES); break;
        case '6': draw(0x0E, 640, 80, 200, PAT_CHECKER); break;
        case '7': draw(0x10, 640, 80, 350, PAT_BARS16);  break;
        case '8': draw(0x10, 640, 80, 350, PAT_STRIPES); break;
        case '9': draw(0x10, 640, 80, 350, PAT_CHECKER); break;
        case 'b': bitwalk(0x10, 640, 80, 350);           break;
        case 'v': bitwalk(0x0E, 640, 80, 200);           break;
        case 't': bars_text16();                         break;
        case 'c': sweep64(sweep_page);
                  sweep_page = (sweep_page + 1) % 5;     break;
        case 's': sweep64_split();                       break;
        }
    }
    set_mode(3);
    return 0;
}
