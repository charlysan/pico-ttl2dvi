#include "pico/stdlib.h"
#include "board.h"
#include "buttons.h"

#define LOCKOUT_US 50000    // longer than contact bounce
#define LONG_US    600000   // ENTER held this long is BACK
#define DELAY_US   400000   // UP/DOWN held this long starts repeating
#define REPEAT_US  100000
#define COMBO_US   2000000  // ENTER + UP/DOWN held this long: reset / BOOTSEL

enum { UP, DOWN, ENTER, NBTN };

static const uint pins[NBTN] = { PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_ENTER };

static bool     down[NBTN];
static uint64_t changed[NBTN];      // last accepted state change
static uint64_t next_repeat[NBTN];
static bool     long_sent;
static bool     enter_used;         // another button went down during this ENTER press
static uint     combo;              // BUTTONS_COMBO_*, 0 = none
static uint64_t combo_start;
static bool     combo_fired;

void buttons_init(void)
{
    for (uint i = 0; i < NBTN; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
}

// While ENTER is down, UP/DOWN belong to a combination: they don't step or
// repeat, and ENTER then gives neither ENTER on release nor BACK.
uint buttons_poll(void)
{
    const uint64_t now = time_us_64();
    uint ev = 0;

    for (uint i = 0; i < NBTN; i++) {
        const bool d = !gpio_get(pins[i]);
        if (d != down[i] && now - changed[i] >= LOCKOUT_US) {
            down[i]    = d;
            changed[i] = now;
            if (i == ENTER) {
                if (d) {
                    enter_used = down[UP] || down[DOWN];
                } else {
                    if (!long_sent && !enter_used) ev |= EV_ENTER;
                    long_sent = false;
                    next_repeat[UP] = next_repeat[DOWN] = now + DELAY_US;
                }
            } else if (d) {
                if (down[ENTER]) enter_used = true;
                else             ev |= i == UP ? EV_UP : EV_DOWN;
                next_repeat[i] = now + DELAY_US;
            }
        } else if (down[i]) {
            if (i == ENTER) {
                if (!long_sent && !enter_used && now - changed[i] >= LONG_US) {
                    ev |= EV_BACK;
                    long_sent = true;
                }
            } else if (!down[ENTER] && now >= next_repeat[i]) {
                ev |= i == UP ? EV_UP : EV_DOWN;
                next_repeat[i] = now + REPEAT_US;
            }
        }
    }

    const uint c = !down[ENTER] || down[UP] == down[DOWN] ? 0u
                 : down[UP] ? BUTTONS_COMBO_RESET : BUTTONS_COMBO_BOOTSEL;
    if (c != combo) {
        combo       = c;
        combo_start = now;
        combo_fired = false;
    }
    if (combo && !combo_fired && now - combo_start >= COMBO_US) {
        combo_fired = true;
        ev |= combo == BUTTONS_COMBO_RESET ? EV_RESET : EV_BOOTSEL;
    }
    return ev;
}

uint buttons_combo(uint *left_ms)
{
    if (!combo || combo_fired) return 0;
    const uint64_t held = time_us_64() - combo_start;
    *left_ms = held < COMBO_US ? (uint)((COMBO_US - held) / 1000u) : 0u;
    return combo;
}
