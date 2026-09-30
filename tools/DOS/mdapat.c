/* mdapat.c — MDA/Hercules test pattern generator for ttl2dvi
   Turbo C 2.x/3.x, small model. Patterns 7-8 use BGI, so link graphics.lib:
      bgiobj herc
      tcc -ml mdapat.c ..\BGI\herc.obj ..\LIB\GRAPHICS.LIB  
   */

#include <dos.h>
#include <conio.h>
#include <stdio.h>
#include <graphics.h>

#define VRAM  ((unsigned char far *)MK_FP(0xB000, 0x0000))
#define COLS  80
#define ROWS  25

int gfx = 0;

/* Hide / restore the hardware cursor.
   BIOS INT 10h AH=01h sets the cursor scan lines; bit 5 of CH disables it. */
void cursor(int on)
{
    union REGS r;
    r.h.ah = 0x01;
    if (on) { r.h.ch = 0x0B; r.h.cl = 0x0C; }   /* MDA 9x14 underline */
    else    { r.h.ch = 0x20; r.h.cl = 0x00; }   /* bit 5 = cursor off */
    int86(0x10, &r, &r);
}

/* Back to text mode, so the VRAM patterns and the final clear are not
   written while the card is still in Hercules graphics mode.
   Re-hides the cursor: leaving graphics mode restores it. */
void text_mode(void)
{
    if (gfx) { closegraph(); gfx = 0; }
    cursor(0);
}

/* Fill the whole screen with one char/attr pair. */
void fill(unsigned char ch, unsigned char attr)
{
    unsigned i;
    text_mode();
    for (i = 0; i < COLS * ROWS; i++) {
        VRAM[i * 2]     = ch;
        VRAM[i * 2 + 1] = attr;
    }
}

/* Alternate two characters horizontally: ch1 ch2 ch1 ch2 ... */
void alternate(unsigned char ch1, unsigned char ch2, unsigned char attr)
{
    unsigned r, c, i;
    text_mode();
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            i = r * COLS + c;
            VRAM[i * 2]     = (c & 1) ? ch2 : ch1;
            VRAM[i * 2 + 1] = attr;
        }
}

/* Mark the extreme columns so you can find the active window edges:
   column 0 and column 79 solid, everything else blank. */
void edges(unsigned char attr)
{
    unsigned r, c, i;
    text_mode();
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            i = r * COLS + c;
            VRAM[i * 2]     = (c == 0 || c == COLS - 1) ? 0xDB : ' ';
            VRAM[i * 2 + 1] = attr;
        }
}

/* Hercules 720x348 */
int gfx_open(void)
{
    static int reg = 0;
    int gd = HERCMONO, gm = HERCMONOHI, err;

    if (gfx) { cleardevice(); return 1; }
    if (!reg) {
        if (registerbgidriver(Herc_driver) < 0) {
            printf("registerbgidriver: %s\n", grapherrormsg(graphresult()));
            return 0;
        }
        reg = 1;
    }
    initgraph(&gd, &gm, "");
    err = graphresult();
    if (err != grOk) { printf("initgraph: %s\n", grapherrormsg(err)); return 0; }
    gfx = 1;
    return 1;
}

/* 1px vertical stripes -> dot clock / horizontal Nyquist. */
void vlines(void)
{
    int x;
    if (!gfx_open()) return;
    for (x = 0; x <= getmaxx(); x += 2)
        line(x, 0, x, getmaxy());
}

/* 1px horizontal stripes -> line count / vscale. */
void hlines(void)
{
    int y;
    if (!gfx_open()) return;
    for (y = 0; y <= getmaxy(); y += 2)
        line(0, y, getmaxx(), y);
}

int main(void)
{
    int k = 0;

    printf("MDA pattern generator. Keys:\n");
    printf(" 1 solid blocks (0xDB) normal   -> active window width\n");
    printf(" 2 solid blocks bright         -> INTENSITY line check\n");
    printf(" 3 medium shade (0xB1)         -> dot-clock resolution\n");
    printf(" 4 vertical bars (0xDB/space)  -> 9px char clock\n");
    printf(" 5 edges only (col 0 and 79)   -> active window edges\n");
    printf(" 6 blank (all spaces)          -> baseline / porch check\n");
    printf(" 7 gfx 1px vertical stripes    -> dot clock\n");
    printf(" 8 gfx 1px horizontal stripes  -> line count\n");
    printf(" 9 medium shade bright         -> INTENSITY at 1px\n");
    printf(" q quit\n");
    printf("press a key...\n");

    cursor(0);          /* before the first pattern, not after */

    while ((k = getch()) != 'q') {
        switch (k) {
        case '1': fill(0xDB, 0x07); break;
        case '2': fill(0xDB, 0x0F); break;
        case '3': fill(0xB1, 0x07); break;
        case '4': alternate(0xDB, ' ', 0x07); break;
        case '5': edges(0x07); break;
        case '6': fill(' ', 0x07); break;
        case '7': vlines(); break;
        case '8': hlines(); break;
        /* Bright dither: the only pattern that drives INTENSITY at 1px
           resolution. view.c assumes the card never emits INTENSITY without
           VIDEO (level[4] = {0,2,0,3}); this is what tests that. */
        case '9': fill(0xB1, 0x0F); break;
        }
    }
    fill(' ', 0x07);
    cursor(1);          /* leave DOS usable */
    return 0;
}
