/*
 * caps_word.cpp — shift the current word, then get out of the way.
 *
 * Activated by the CAPSWRD keycode. Letters get shifted; digits, backspace,
 * minus and underscore pass through and keep it alive; anything else (space,
 * punctuation, enter) ends it. Much nicer than caps lock for SCREAMING_CASE
 * identifiers, and it can't get stuck on.
 */

#include "pico/stdlib.h"
#include "kb/features.h"
#include "kb/keystate.h"

#ifndef CAPS_WORD_IDLE_TIMEOUT_MS
#define CAPS_WORD_IDLE_TIMEOUT_MS 5000   /* 0 = no timeout */
#endif

static bool     active;
static uint32_t last_ms;

static bool continues_word(uint8_t u) {
    if (u >= KEY_A && u <= KEY_Z) return true;
    if (u >= KEY_1 && u <= KEY_0) return true;
    return u == KEY_BACKSPACE || u == KEY_MINUS || u == KEY_DELETE;
}

void kb_caps_word_init(void) { active = false; }

bool kb_caps_word_process(keyrecord_t *rec) {
    if (rec->keycode == QK_CAPS_WORD) {
        if (rec->event.pressed) {
            active = !active;
            if (!active) keystate_set_weak_mods(KB_SRC_MATRIX, 0);
            last_ms = rec->event.time;
        }
        return false;
    }

    if (!active) return true;

    /* Only basic keycodes are meaningful here; let the rest through untouched
     * so layer keys and mods don't end the word. */
    if (!kc_is_basic(rec->keycode) || kc_is_mod(rec->keycode)) return true;

    uint8_t u = kc_basic_of(rec->keycode);
    if (!continues_word(u)) {
        if (rec->event.pressed) { active = false; }
        return true;
    }

    last_ms = rec->event.time;
    if (u >= KEY_A && u <= KEY_Z) {
        if (rec->event.pressed) keystate_press(KB_SRC_MATRIX, KEY_LEFTSHIFT);
        else                    keystate_release(KB_SRC_MATRIX, KEY_LEFTSHIFT);
    }
    return true;
}

void kb_caps_word_task(void) {
#if CAPS_WORD_IDLE_TIMEOUT_MS
    if (!active) return;
    if (to_ms_since_boot(get_absolute_time()) - last_ms > CAPS_WORD_IDLE_TIMEOUT_MS)
        active = false;
#endif
}
