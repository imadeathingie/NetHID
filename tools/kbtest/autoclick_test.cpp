/*
 * autoclick_test — triggers, rate, and not leaving a key held.
 *
 * The properties that matter are the ones that go wrong quietly:
 *
 *   - a hold that stops when released, with no key left down
 *   - a latch that survives release and stops on the NEXT tap
 *   - press and release on SEPARATE passes, or the host sees nothing at all
 *   - a rate that is actually the rate asked for
 */
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "kb/kb.h"
#include "kb/autoclick.h"
#include "kb/keystate.h"

extern uint32_t fake_now_ms;

/* The bits of the wider firmware this slice does not link. Consumer keycodes
 * and the OLED status snapshot are not what is under test here. */
struct kb_status_t;
void kb_status_set(const kb_status_t *) {}
bool hid_push_consumer(uint16_t) { return true; }

/* Count what the host would actually see for the target key. */
static int presses, releases;
static bool down;

static void observe(void) {
    if (!keystate_dirty()) return;
    hid_keyboard_report_t r;
    if (!keystate_compose(&r)) return;
    bool now_down = r.keycode[0] == KC_SPC;
    if (now_down && !down) presses++;
    if (!now_down && down) releases++;
    down = now_down;
}

static void run(int ms) {
    for (int i = 0; i < ms; i++) { kb_task(); observe(); fake_now_ms++; }
}

static int fails = 0;
static void ck(const char *n, bool ok) {
    printf("%-46s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

/* Slot 1 on oledpad is KC_SPC, 50 ms, AC_TAP2. */
/* Let the tap window lapse, so a case cannot inherit a partial tap sequence
 * from the one before it. Without this, case 1's single tap was still inside
 * the window when case 2 began, so case 2's FIRST press completed a double-tap
 * and its second press stopped it again — a failure that looked like the latch
 * being broken when the latch was fine. */
static void settle(void) { run(AC_TAP_WINDOW_MS + 50); }

static void tap(int n) {
    for (int i = 0; i < n; i++) {
        kb_send_keycode(AUTOCLK(1), true);  run(2);
        kb_send_keycode(AUTOCLK(1), false); run(2);
    }
}

int main(void) {
    kb_init();
    run(20);

    /* 1. A single tap does nothing: a double-tap trigger must not fire on one. */
    settle();
    presses = releases = 0;
    tap(1); run(200);
    ck("one tap does not start it", presses == 0 && !autoclick_active());

    /* 2. Double-tap latches, and it keeps going after the key is released. */
    settle();
    presses = releases = 0;
    tap(2); run(400);
    ck("double-tap latches and keeps running", autoclick_active() && presses >= 5);

    /* 3. Every press is matched by a release. A press and release inside one
     *    scan would collapse into a single composed report and the host would
     *    see nothing, so this also proves they land on separate passes. */
    ck("presses and releases are balanced", presses - releases <= 1);

    /* 4. The rate is roughly the one asked for: 400 ms at 50 ms is ~8. */
    ck("rate is about right", presses >= 5 && presses <= 11);

    /* 5. Any tap stops it - turning it off must be as easy as turning it on. */
    /* Deliberately NOT settled first: this is the running case, and a single
     * tap while it runs must stop it whatever the tap counter holds. */
    presses = releases = 0;
    tap(1); run(200);
    ck("a tap stops a latched autoclicker", !autoclick_active() && presses == 0);

    /* 6. Nothing is left held. */
    ck("no key is left down", !down);

    printf("\n%s\n", fails ? "FAILURES" : "all green");
    return fails;
}
