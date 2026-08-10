/*
 * proto2x2 :: advanced — one key per feature, so you can tell instantly
 * which part of the stack is broken.
 *
 *   top-left      LT(1, KC_A)     tap = a, hold = layer 1
 *   top-right     SFT_T(KC_B)     tap = b, hold = left shift
 *   bottom-left   OSM(MOD_LCTRL)  one-shot ctrl, tap twice to lock
 *   bottom-right  KC_D
 *
 *   A + B together                Esc  (combo)
 *
 * Layer 1:
 *   top-right     KB_MACRO(0)     types a string via NetHID's typer
 *   bottom-left   TG(1)           latch layer 1 on
 */

#include "kb/kb.h"
#include "kb/combo.h"
#include "kb/macros.h"

enum { MACRO_HELLO = 0 };

/* Named once, used by both the keymap and the combo list. Combo triggers
 * match the keycode as written in the keymap — combos run before the tapping
 * machine, so a dual-role key's trigger is the whole LT()/MT(), not its tap. */
#define K_TL  LT(1, KC_A)
#define K_TR  SFT_T(KC_B)

const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        K_TL,             K_TR,
        OSM(MOD_LCTRL),   KC_D
    ),
    [1] = LAYOUT(
        KC_TRNS,          KB_MACRO(MACRO_HELLO),
        TG(1),            LSFT(KC_4)      /* $ */
    ),
};

const uint8_t keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);

/* A + B chorded → Escape. Trigger lists are KC_NO-terminated and match on
 * keycode, so the combo follows the keys wherever they move on the board. */
static const kb_keycode_t combo_esc[] = { K_TL, K_TR, KC_NO };

const kb_combo_t kb_combos[] = {
    { combo_esc, KC_ESC },
};
const uint16_t kb_combo_count = sizeof(kb_combos) / sizeof(kb_combos[0]);

bool kb_macro_user(uint8_t id, keyrecord_t *rec) {
    if (!rec->event.pressed) return false;
    switch (id) {
    case MACRO_HELLO:
        kb_macro_string("hello from the matrix\n");
        return false;
    }
    return false;
}
