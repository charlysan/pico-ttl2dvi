/* cgapat.c -- CGA test-pattern generator for ttl2dvi.
   Turbo C 2.x/3.x, small model, run on the PC with the CGA card.
   
   Modes:
     1  320x200, 4 colours   (BIOS mode 4) -- vertical bars, palette 1 (hi)
     2  320x200, 4 colours   (BIOS mode 4) -- palette 0 (green/red/brown)
     3  320x200, 4 colours   (BIOS mode 4) -- 1px vertical stripes
     4  320x200, 4 colours   (BIOS mode 4) -- 1px checkerboard
     t  80x25 text, 16 col   (BIOS mode 3) -- 16 vertical colour bars (640-wide)
     6  640x200, 2 colours   (BIOS mode 6) -- black/white bars
     q  quit (restore 80x25 text) */

#include <dos.h>
#include <conio.h>
#include <mem.h>
#include <stdio.h>

static void set_mode(int mode)
{
    union REGS r;
    r.h.ah = 0x00; r.h.al = (unsigned char)mode;
    int86(0x10, &r, &r);
}

/* 320x200 palette select: AH=0Bh, BH=1, BL=palette (0 or 1). */
static void set_palette(int pal)
{
    union REGS r;
    r.h.ah = 0x0B; r.h.bh = 1; r.h.bl = (unsigned char)pal;
    int86(0x10, &r, &r);
}

/* 320x200 4-colour vertical bars: colour = (x*4)/320 -> 0,1,2,3. Direct VRAM
   (fast): mode 4 is interleaved -- even lines at B800:0000, odd at B800:2000,
   80 bytes/line, 4 px/byte, 2 bits each, MSB = leftmost pixel. */
static void bars_320(int palette)
{
    unsigned char far *v0 = (unsigned char far *)MK_FP(0xB800, 0x0000);
    unsigned char far *v1 = (unsigned char far *)MK_FP(0xB800, 0x2000);
    unsigned char line[80];
    int b, p, y;

    set_mode(4);
    set_palette(palette);
    for (b = 0; b < 80; b++) {
        unsigned char byte = 0;
        for (p = 0; p < 4; p++) {
            int x = b * 4 + p;
            int color = (x * 4) / 320;          /* 0..3 */
            byte |= (unsigned char)((color & 3) << ((3 - p) * 2));
        }
        line[b] = byte;
    }
    for (y = 0; y < 100; y++) {
        _fmemcpy(v0 + (long)y * 80, line, 80);  /* even scanlines */
        _fmemcpy(v1 + (long)y * 80, line, 80);  /* odd scanlines  */
    }
}

/* 320x200 mode 4, fill from a per-scanline byte. Mode 4 is interleaved: even
   scanlines at B800:0000, odd at B800:2000, 80 bytes/line. */
static void fill_320(unsigned char even_byte, unsigned char odd_byte, int palette)
{
    unsigned char far *v0 = (unsigned char far *)MK_FP(0xB800, 0x0000);
    unsigned char far *v1 = (unsigned char far *)MK_FP(0xB800, 0x2000);
    int y;
    set_mode(4);
    set_palette(palette);
    for (y = 0; y < 100; y++) {
        _fmemset(v0 + (long)y * 80, even_byte, 80);   /* even scanlines */
        _fmemset(v1 + (long)y * 80, odd_byte,  80);   /* odd scanlines  */
    }
}

/* 1px vertical stripes: every pixel alternates colour 3 / 0 across the line
   (byte 0xCC = pixels 3,0,3,0). Highest horizontal frequency -> dot-clock /
   sampling-phase test. Every scanline identical. */
static void stripes_320(void) { fill_320(0xCC, 0xCC, 1); }

/* 1px checkerboard: even scanlines 3,0,3,0 (0xCC), odd 0,3,0,3 (0x33). 
   Exposes sample jitter as shimmer. */
static void checker_320(void) { fill_320(0xCC, 0x33, 1); }

/* 80x25 text, 16 vertical colour bars: char 0xDB (full block), fg = colour
   0..15 across. Attribute nibble is IRGB, so this shows every RGBI value. */
static void bars_text16(void)
{
    unsigned char far *vram = (unsigned char far *)MK_FP(0xB800, 0x0000);
    int r, c;
    set_mode(3);
    for (r = 0; r < 25; r++)
        for (c = 0; c < 80; c++) {
            int i = (r * 80 + c) * 2;
            vram[i]     = 0xDB;                  /* full block */
            vram[i + 1] = (unsigned char)((c * 16) / 80);  /* fg colour 0..15 */
        }
}

/* 640x200 2-colour vertical bars (mode 6): alternating black/white columns. */
static void bars_640(void)
{
    unsigned char far *v0 = (unsigned char far *)MK_FP(0xB800, 0x0000);
    unsigned char far *v1 = (unsigned char far *)MK_FP(0xB800, 0x2000);
    unsigned char line[80];
    int b, y;
    set_mode(6);
    for (b = 0; b < 80; b++)                     /* 8 px/byte; wide bars */
        line[b] = (b & 1) ? 0xFF : 0x00;
    for (y = 0; y < 100; y++) {
        _fmemcpy(v0 + (long)y * 80, line, 80);
        _fmemcpy(v1 + (long)y * 80, line, 80);
    }
}

int main(void)
{
    int k;
    printf("CGA pattern generator (ttl2dvi). Keys:\n");
    printf(" 1  320x200 4-colour bars, palette 1 (cyan/magenta/white)\n");
    printf(" 2  320x200 4-colour bars, palette 0 (green/red/brown)\n");
    printf(" 3  320x200 1px vertical stripes  (dot-clock / phase test)\n");
    printf(" 4  320x200 1px checkerboard      (Nyquist / shimmer test)\n");
    printf(" t  text 80x25, 16-colour bars (640-wide)\n");
    printf(" 6  640x200 2-colour bars\n");
    printf(" q  quit\n");
    printf("press a key...\n");

    while ((k = getch()) != 'q') {
        switch (k) {
        case '1': bars_320(1);   break;
        case '2': bars_320(0);   break;
        case '3': stripes_320(); break;
        case '4': checker_320(); break;
        case 't': bars_text16(); break;
        case '6': bars_640();    break;
        }
    }
    set_mode(3);                 /* restore 80x25 colour text */
    return 0;
}
