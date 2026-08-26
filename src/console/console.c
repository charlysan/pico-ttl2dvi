
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include "console.h"
#include "version.h"
#include "sync.h"
#include "hardware/clocks.h"
// #include "config.h"

#define MAX_CMDS 32
#define LINE_MAX CONSOLE_LINE_MAX
#define MAX_ARGS 8
#define PROMPT   "ttl2dvi> "

struct cmd {
    const char     *name;
    console_cmd_fn  fn;
    const char     *help;
};

static struct cmd s_cmds[MAX_CMDS];
static int        s_ncmds = 0;

static char s_line[LINE_MAX];
static int  s_len = 0;


static void print_prompt(void) {
    fputs(PROMPT, stdout);
    fflush(stdout);
}

// Built-in: list registered commands.
static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < s_ncmds; i++)
        printf("  %-10s %s\n", s_cmds[i].name, s_cmds[i].help ? s_cmds[i].help : "");
}

void console_register(const char *name, console_cmd_fn fn, const char *help) {
    if (s_ncmds >= MAX_CMDS) return;   // silently drop past the cap (bump MAX_CMDS)
    s_cmds[s_ncmds++] = (struct cmd){ name, fn, help };
}

void console_init(void) {
    console_register("help", cmd_help, "list commands");
    printf("\nttl2dvi console\n");
    print_prompt();
}

static void dispatch(char *line) {
    char *argv[MAX_ARGS];
    int   argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < MAX_ARGS; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;

    if (argc == 0) return;   // empty line -> just a fresh prompt

    for (int i = 0; i < s_ncmds; i++) {
        if (strcmp(argv[0], s_cmds[i].name) == 0) {
            s_cmds[i].fn(argc, argv);
            return;
        }
    }
    printf("command not found: %s\n", argv[0]);
}

void console_poll(void) {
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (ch == '\r' || ch == '\n') {
            putchar('\n');
            s_line[s_len] = '\0';
            dispatch(s_line);
            s_len = 0;
            print_prompt();
        } else if (ch == 8 || ch == 127) {        // backspace / delete
            if (s_len > 0) { s_len--; fputs("\b \b", stdout); fflush(stdout); }
        } else if (ch >= 32 && ch < 127 && s_len < LINE_MAX - 1) {
            s_line[s_len++] = (char)ch;
            putchar(ch); fflush(stdout);          // echo
        }
    }
}

// ***** COMMANDS ***** //

// --- diagnostics ---
void cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("ttl2dvi %s\n", TTL2DVI_VERSION);
}

void cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t f = clock_get_hz(clk_sys);
    uint32_t ph = sync_hsync_period();
    uint32_t pv = sync_vsync_period();

    printf("  SYSCLK: %lu.%03lu MHz\n", (unsigned long)(f / 1000000), (unsigned long)(f % 1000000)); 
    if (ph) {
        uint32_t h = (uint32_t)(f / ph);
        printf("  HSYNC: %lu.%02lu KHz\n", (unsigned long)(h / 1000), (unsigned long)(h % 1000)); 
    } else {
        printf("HSYNC --\n");
    }
    if (pv) {
        uint32_t v = (uint32_t)(f / pv);
        printf("  VSYNC: %lu.%02lu Hz\n", (unsigned long)(v / 1), (unsigned long)(v % 1)); 
    } else {
        printf("VSYNC --\n");
    }
}