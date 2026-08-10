/*
 * kb/autoclick.h — repeat a key or a mouse button at a fixed rate.
 *
 * A board declares slots; a keycode selects one:
 *
 *     #define AUTOCLICKS                              \
 *         AUTOCLICK(0, MS_BTN1, 100, AC_HOLD)         \
 *         AUTOCLICK(1, KC_SPC,   50, AC_TAP2)         \
 *         AUTOCLICK(2, MS_BTN1,  25, AC_TAP3 | AC_HOLD)
 *
 *     ... AUTOCLK(0) ... in the keymap
 *
 * The target is an ordinary keycode, so anything the feature chain already
 * understands works: a mouse button, a letter, LCTL(KC_V), a macro. Autoclick
 * does not know or care which — it re-enters the chain from the top, exactly as
 * if the key had been pressed.
 *
 * ── Triggers ────────────────────────────────────────────────────────────────
 *   AC_HOLD   runs while the key is held, stops on release
 *   AC_TAP2   double-tap toggles it on; any tap turns it off again
 *   AC_TAP3   triple-tap toggles
 *
 * These OR together. AC_TAP2 | AC_HOLD is the useful combination: hold for a
 * burst, double-tap to leave it running.
 *
 * A toggle and a hold on the same key need the taps counted BEFORE the hold can
 * start, or a double-tap's second press begins holding and the toggle never
 * fires. So a key with any tap trigger waits AC_TAP_WINDOW_MS before starting a
 * hold. That delay is only paid on keys that asked for both.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"      /* the board's NUM_AUTOCLICKS/AUTOCLICKS, before the
                            * #ifndef default below can claim there are none */
#include "kb/keycodes.h"

#define AC_HOLD  0x01
#define AC_TAP2  0x02
#define AC_TAP3  0x04

/* Taps must land within this of each other to count as a sequence. */
#ifndef AC_TAP_WINDOW_MS
#define AC_TAP_WINDOW_MS 250
#endif

/* Floor on the interval. A 1 ms autoclicker is not faster in any useful sense:
 * each click is a press and a release, both needing their own USB frame, and
 * asking for more than the endpoint can carry just fills the queue. */
#ifndef AC_MIN_INTERVAL_MS
#define AC_MIN_INTERVAL_MS 8
#endif
#ifndef AC_MAX_INTERVAL_MS
#define AC_MAX_INTERVAL_MS 5000
#endif

typedef struct {
    kb_keycode_t target;
    uint16_t     interval_ms;
    uint8_t      trigger;
} autoclick_slot_t;

/*
 * The count is STATED, not derived — same as NUM_LOCAL_ENCODERS, and for the
 * same reason. The obvious trick (redefine AUTOCLICK as `+1`, expand the list,
 * undef) does not work: the resulting macro expands at its USE site, by which
 * point AUTOCLICK is gone, and you get "expected ')' before 'AUTOCLICK'"
 * pointing at a board header that is perfectly correct.
 *
 * I wrote that note in kb/encoder.h and then made the identical mistake here,
 * which is a fair argument for the duller construction being the right one.
 *
 * This default only applies to a board that declares no slots. It used to fire
 * on boards that DO declare them, because this header was included before
 * keyboard.h and won the race — the board's value then arrived as a conflicting
 * redefinition. Hence the #include "keyboard.h" above.
 */
#ifndef NUM_AUTOCLICKS
#define NUM_AUTOCLICKS 0
#endif

/* True while any slot is repeating — for the OLED, so a running autoclicker is
 * visible rather than something you discover later. */
bool     autoclick_active(void);
uint16_t autoclick_interval(void);   /* the rate in force, ms */
void     autoclick_stop_all(void);

/*
 * ── Runtime slots ───────────────────────────────────────────────────────────
 *
 * The AUTOCLICKS list above is the DEFAULT, not the definition. Slots live in
 * RAM and are editable from the web UI, exactly as the dynamic keymap works:
 * a compiled default, a stored copy in flash that overrides it, and saving as
 * a separate explicit step so experimenting costs nothing.
 *
 * Rate was already changeable at runtime through the `autoclick_ms` setting,
 * but that is a single override applied to every slot — it cannot say "slot 0
 * at 100 ms, slot 2 at 25 ms", which is the whole reason slots have their own
 * interval. Editing the slot changes the slot; the setting still wins over all
 * of them when it is non-zero.
 */
uint8_t autoclick_count(void);

/* NULL if `i` is out of range. */
const autoclick_slot_t *autoclick_slot(uint8_t i);

/* Validates range and trigger bits; false (and no change) if either is bad.
 * Stops the slot first — a slot whose target changes mid-repeat would otherwise
 * release the NEW keycode and leave the old one held down on the host. */
bool autoclick_set_slot(uint8_t i, kb_keycode_t target,
                        uint16_t interval_ms, uint8_t trigger);

/* True if `trigger` is a combination that can actually start. Exposed so the
 * API can reject a bad one with a reason instead of silently storing a slot
 * that never fires. */
bool autoclick_trigger_valid(uint8_t trigger);

/* Persistence. The request is queued; autoclick_commit_poll() does the write on
 * core 0, because a flash erase parks the other core and must not happen inside
 * a web request. */
bool autoclick_stored(void);        /* a saved copy exists in flash        */
bool autoclick_dirty(void);         /* live differs from what was saved    */
bool autoclick_save_pending(void);
void autoclick_save(void);
void autoclick_reset(bool erase);   /* back to the compiled defaults       */
void autoclick_commit_poll(void);
