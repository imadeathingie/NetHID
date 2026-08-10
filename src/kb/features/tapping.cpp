/*
 * tapping.cpp — dual-role keys: MT(mods, kc) and LT(layer, kc).
 *
 * One undecided key at a time. Pressing a second dual-role key while the
 * first is undecided resolves the first as a hold, which is the behaviour
 * people expect from stacked layer keys.
 *
 * A tap is registered and then released a moment later rather than in the
 * same scan: a press and release inside one scan would collapse into a
 * single composed report and the host would never see the key at all. The
 * release waits for keystate to be flushed (with a timeout in case USB is
 * suspended).
 *
 * NOT IMPLEMENTED: permissive hold / retro tapping. Both need the
 * interrupting key's event to be buffered and replayed, which means an event
 * queue in front of this state machine. The hook point is `other_key()`.
 */

#include "pico/stdlib.h"
#if ENABLE_SETTINGS
#include "settings.h"
#endif
#include "kb/tapping.h"
#include "kb/features.h"
#include "kb/keystate.h"
#if KB_FEATURE_LAYERS
#include "kb/layers.h"
#endif

/* Weak, so a keymap can override per keycode. The default reads the runtime
 * setting when there is one — tapping term is the tunable you most want to
 * adjust by feel, and reflashing between 180 and 200 ms to find out which
 * feels right is a miserable loop. */
__attribute__((weak)) uint16_t kb_tapping_term(kb_keycode_t kc) {
    (void)kc;
#if ENABLE_SETTINGS
    return settings()->tapping_term_ms;
#else
    return TAPPING_TERM;
#endif
}

typedef enum { TS_IDLE, TS_UNDECIDED, TS_HELD } tap_state_t;

static tap_state_t   state;
static keypos_t      key;
static kb_keycode_t  keycode;
static uint32_t      pressed_at;
static uint8_t       held_mods;
static uint8_t       held_layer;
static bool          held_is_layer;

/* Deferred tap release */
static bool          tap_pending;
static kb_keycode_t  tap_keycode;
static uint32_t      tap_at;

static inline bool is_tap_hold(kb_keycode_t kc) {
    return kc_in(kc, QK_MOD_TAP, QK_MOD_TAP_MAX) ||
           kc_in(kc, QK_LAYER_TAP, QK_LAYER_TAP_MAX);
}

static void resolve_hold(void) {
    if (kc_in(keycode, QK_MOD_TAP, QK_MOD_TAP_MAX)) {
        held_mods     = kc_unpack_mods(keycode);
        held_is_layer = false;
        for (uint8_t u = KEY_LEFTCTRL; u <= KEY_RIGHTGUI; u++)
            if (held_mods & (1u << (u - KEY_LEFTCTRL))) keystate_press(KB_SRC_MATRIX, u);
    } else {
        held_layer    = kc_layer_of(keycode);
        held_is_layer = true;
#if KB_FEATURE_LAYERS
        kb_layer_on(held_layer);
#endif
    }
    state = TS_HELD;
}

static void release_hold(void) {
    if (held_is_layer) {
#if KB_FEATURE_LAYERS
        kb_layer_off(held_layer);
#endif
    } else {
        for (uint8_t u = KEY_LEFTCTRL; u <= KEY_RIGHTGUI; u++)
            if (held_mods & (1u << (u - KEY_LEFTCTRL))) keystate_release(KB_SRC_MATRIX, u);
    }
    state = TS_IDLE;
}

static void resolve_tap(uint32_t now) {
    tap_keycode = (kb_keycode_t)kc_basic_of(keycode);
    kb_register(tap_keycode);
    tap_pending = true;
    tap_at      = now;
    state       = TS_IDLE;
}

/* Another key was pressed while a dual-role key was undecided. */
static void other_key(void) {
#if HOLD_ON_OTHER_KEY_PRESS
    resolve_hold();
#endif
}

void kb_tapping_init(void) {
    state = TS_IDLE; tap_pending = false;
}

bool kb_tapping_process(keyrecord_t *rec) {
    kb_keycode_t kc = rec->keycode;
    uint32_t now = rec->event.time;

    if (state != TS_IDLE &&
        (rec->event.key.row != key.row || rec->event.key.col != key.col)) {
        if (rec->event.pressed && state == TS_UNDECIDED) other_key();
        return true;   /* the other key carries on down the chain */
    }

    if (is_tap_hold(kc)) {
        if (rec->event.pressed) {
            if (state == TS_UNDECIDED) resolve_hold();   /* nested: first one holds */
            state      = TS_UNDECIDED;
            key        = rec->event.key;
            keycode    = kc;
            pressed_at = now;
        } else if (state == TS_UNDECIDED) {
            resolve_tap(now);
            rec->tap_count = 1;
        } else if (state == TS_HELD) {
            release_hold();
        }
        return false;   /* dual-role keys never reach the default handler */
    }

    return true;
}

void kb_tapping_task(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (state == TS_UNDECIDED && (now - pressed_at) >= kb_tapping_term(keycode))
        resolve_hold();

    /* Let the tap be seen by the host before releasing it. keystate_dirty()
     * goes false once hid_task() has composed and sent a report; the 20 ms
     * fallback covers a suspended or unmounted host. */
    if (tap_pending && (!keystate_dirty() || (now - tap_at) > 20)) {
        kb_unregister(tap_keycode);
        tap_pending = false;
    }
}
