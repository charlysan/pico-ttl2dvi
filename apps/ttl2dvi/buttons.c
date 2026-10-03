#include "pico/stdlib.h"
#include "board.h"
#include "buttons.h"

#define LOCKOUT_US 50000    // longer than contact bounce
#define LONG_US    600000   // ENTER held this long is BACK
#define DELAY_US   400000   // UP/DOWN held this long starts repeating
#define REPEAT_US  100000

enum { UP, DOWN, ENTER, NBTN };

static const uint pins[NBTN] = { PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_ENTER };

static bool     down[NBTN];
static uint64_t changed[NBTN];      // last accepted state change
static uint64_t next_repeat[NBTN];
static bool     long_sent;

void buttons_init(void)
{
    for (uint i = 0; i < NBTN; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
}

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
                if (!d && !long_sent) ev |= EV_ENTER;
                long_sent = false;
            } else if (d) {
                ev |= i == UP ? EV_UP : EV_DOWN;
                next_repeat[i] = now + DELAY_US;
            }
        } else if (down[i]) {
            if (i == ENTER) {
                if (!long_sent && now - changed[i] >= LONG_US) {
                    ev |= EV_BACK;
                    long_sent = true;
                }
            } else if (now >= next_repeat[i]) {
                ev |= i == UP ? EV_UP : EV_DOWN;
                next_repeat[i] = now + REPEAT_US;
            }
        }
    }
    return ev;
}
