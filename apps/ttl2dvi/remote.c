#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "board.h"
#include "console.h"
#include "ir.h"
#include "buttons.h"
#include "remote.h"

// The key map: the text tools/irlearn.py writes, in the sector before the
// settings one, ended by a NUL or erased flash. tools/irmap2uf2.py installs it.
#define MAP_OFS  (PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE)
#define MAP_MAX  32

#define HOLD_US  200000     // repeats come every ~108 ms while held
#define DELAY_US 400000     // held this long before repeats act, as the buttons

typedef struct {
    uint32_t    code;
    bool        rep;
    uint        ev;         // an EV_*, or 0 for a command
    const char *cmd;        // in flash, not NUL-terminated
    uint        len;
} irkey_t;

static irkey_t s_map[MAP_MAX];
static uint  s_nmap, s_bad;     // entries, and lines that didn't parse

static bool         s_sniff;
static uint32_t     s_held;     // last code, 0 once released
static const irkey_t *s_key;      // its entry, if mapped
static uint64_t     s_held_us, s_press_us;

// @up/@down: the first names, still accepted.
static const struct { const char *name; uint ev; } nav[] = {
    { "@prev", EV_UP }, { "@next", EV_DOWN }, { "@enter", EV_ENTER }, { "@back", EV_BACK },
    { "@up",   EV_UP }, { "@down", EV_DOWN },
};

static bool space(char c) { return c == ' ' || c == '\t'; }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "CODE  r|-  action": false if the line doesn't fit that.
static bool parse(const char *p, const char *end, irkey_t *k)
{
    k->code = 0;
    for (uint i = 0; i < 8; i++, p++) {
        if (p >= end || hexval(*p) < 0) return false;
        k->code = k->code << 4 | (uint32_t)hexval(*p);
    }
    if (p >= end || !space(*p)) return false;
    while (p < end && space(*p)) p++;
    if (p >= end || (*p != 'r' && *p != '-')) return false;
    k->rep = *p++ == 'r';
    while (p < end && space(*p)) p++;
    while (end > p && space(end[-1])) end--;
    if (p == end) return false;

    k->cmd = p;
    k->len = (uint)(end - p);
    k->ev  = 0;
    if (*p == '@') {
        for (uint i = 0; i < count_of(nav); i++)
            if (k->len == strlen(nav[i].name) && !memcmp(p, nav[i].name, k->len))
                k->ev = nav[i].ev;
        if (!k->ev) return false;
    }
    return true;
}

static void load_map(void)
{
    const char *p   = (const char *)(XIP_BASE + MAP_OFS);
    const char *end = p + FLASH_SECTOR_SIZE;
    while (p < end && *p && *p != (char)0xff) {
        const char *eol = p;
        while (eol < end && *eol && *eol != (char)0xff && *eol != '\n') eol++;
        const char *e = eol > p && eol[-1] == '\r' ? eol - 1 : eol;
        const char *q = p;
        while (q < e && space(*q)) q++;
        if (q < e && *q != '#') {
            if (s_nmap < MAP_MAX && parse(q, e, &s_map[s_nmap])) s_nmap++;
            else s_bad++;
        }
        p = eol < end && *eol == '\n' ? eol + 1 : eol;
    }
}

static const irkey_t *find(uint32_t code)
{
    for (uint i = 0; i < s_nmap; i++)
        if (s_map[i].code == code) return &s_map[i];
    return NULL;
}

static uint act(const irkey_t *k)
{
    if (k->ev) return k->ev;
    char line[CONSOLE_LINE_MAX];
    const uint n = k->len < sizeof line - 1 ? k->len : sizeof line - 1;
    memcpy(line, k->cmd, n);
    line[n] = '\0';
    console_exec(line);
    return 0;
}

void remote_init(void)
{
    load_map();
    ir_init(PIN_IR);
}

uint remote_poll(void)
{
    const uint64_t now = time_us_64();
    if (now - s_held_us >= HOLD_US) {
        s_held = 0;
        s_key  = NULL;
    }

    uint ev = 0;
    uint32_t c;
    while (ir_get(&c)) {
        if (!ir_valid(c)) {
            if (s_sniff) printf("ir: bad %08lX\n", (unsigned long)c);
            continue;
        }
        // A repeat carries no code: it's the button still held.
        if (c == IR_REPEAT) {
            if (!s_held) continue;
            s_held_us = now;
            if (s_sniff) printf("ir: repeat %08lX\n", (unsigned long)s_held);
            if (s_key && s_key->rep && now - s_press_us >= DELAY_US) ev |= act(s_key);
            continue;
        }
        s_held     = c;
        s_key      = find(c);
        s_held_us  = now;
        s_press_us = now;
        if (s_sniff)
            printf("ir: %08lX  addr %04lX cmd %02lX%s%.*s\n", (unsigned long)c,
                   (unsigned long)(c & 0xffffu), (unsigned long)((c >> 16) & 0xffu),
                   s_key ? "  -> " : "", s_key ? (int)s_key->len : 0, s_key ? s_key->cmd : "");
        if (s_key) ev |= act(s_key);
    }
    return ev;
}

static void print_map(void)
{
    printf("ir map: %u key%s", s_nmap, s_nmap == 1 ? "" : "s");
    if (s_bad) printf(", %u line%s ignored", s_bad, s_bad == 1 ? "" : "s");
    printf("\n");
    for (uint i = 0; i < s_nmap; i++)
        printf("  %08lX  %c  %.*s\n", (unsigned long)s_map[i].code,
               s_map[i].rep ? 'r' : '-', (int)s_map[i].len, s_map[i].cmd);
}

void cmd_ir(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "map")) {
        print_map();
        return;
    }
    if (argc >= 2) s_sniff = !strcmp(argv[1], "on");
    printf("ir: print codes %s\n", s_sniff ? "on" : "off");
}
