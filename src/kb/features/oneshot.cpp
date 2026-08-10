/*
 * oneshot.cpp — OSM(mods) and OSL(layer): sticky modifiers and layers.
 *
 * Same idea as the double-tap sticky mods already in the web UI, applied to
 * physical keys. Tap OSM(MOD_LSHIFT), then a letter, and you get one capital.
 * Tap it ONESHOT_TAP_TOGGLE times in a row to lock it until tapped again.
 */

#include "pico/stdlib.h"
#include "kb/oneshot.h"
#include "kb/features.h"
#include "kb/keystate.h"
#if KB_FEATURE_LAYERS
#include "kb/layers.h"
#endif

static uint8_t  pend_mods;
static bool     have_layer;
static uint8_t  pend_layer;
static bool     locked;
static uint32_t armed_at;
static uint8_t  tap_streak;
static kb_keycode_t last_oneshot;

static void apply(void) {
    keystate_set_weak_mods(KB_SRC_NET, 0);   /* never touches the net source */
}

void kb_oneshot_init(void) {
    pend_mods = 0; have_layer = false; locked = false; tap_streak = 0;
    last_oneshot = KC_NO;
}

uint8_t kb_oneshot_mods(void) { return pend_mods; }

void kb_oneshot_cancel(void) {
    if (pend_mods) {
        for (uint8_t u = KEY_LEFTCTRL; u <= KEY_RIGHTGUI; u++)
            if (pend_mods & (1u << (u - KEY_LEFTCTRL))) keystate_release(KB_SRC_MATRIX, u);
        pend_mods = 0;
    }
#if KB_FEATURE_LAYERS
    if (have_layer) kb_layer_off(pend_layer);
#endif
    have_layer = false;
    locked = false;
    tap_streak = 0;
}

bool kb_oneshot_process(keyrecord_t *rec) {
    kb_keycode_t kc = rec->keycode;

    if ((kc & 0xFF00) == QK_ONE_SHOT_MOD || (kc & 0xFF00) == QK_ONE_SHOT_LAYER) {
        if (!rec->event.pressed) return false;

        if (kc == last_oneshot) tap_streak++; else { tap_streak = 1; last_oneshot = kc; }

        if (ONESHOT_TAP_TOGGLE && tap_streak >= ONESHOT_TAP_TOGGLE) {
            if (locked) { kb_oneshot_cancel(); return false; }
            locked = true;
        }

        armed_at = rec->event.time;
        if ((kc & 0xFF00) == QK_ONE_SHOT_MOD) {
            pend_mods = (uint8_t)(kc & 0xFF);
            for (uint8_t u = KEY_LEFTCTRL; u <= KEY_RIGHTGUI; u++)
                if (pend_mods & (1u << (u - KEY_LEFTCTRL))) keystate_press(KB_SRC_MATRIX, u);
        } else {
            pend_layer = (uint8_t)(kc & 0xFF);
            have_layer = true;
#if KB_FEATURE_LAYERS
            kb_layer_on(pend_layer);
#endif
        }
        apply();
        return false;
    }

    /* Any other key consumes the one-shot on its RELEASE, so the modifier is
     * still applied while the key is down. */
    if ((pend_mods || have_layer) && !locked && !rec->event.pressed && kc != KC_NO) {
        kb_oneshot_cancel();
    }
    if (rec->event.pressed) { last_oneshot = KC_NO; tap_streak = 0; }
    return true;
}

void kb_oneshot_task(void) {
    if (locked || (!pend_mods && !have_layer)) return;
    if (ONESHOT_TIMEOUT_MS == 0) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - armed_at > ONESHOT_TIMEOUT_MS) kb_oneshot_cancel();
}
