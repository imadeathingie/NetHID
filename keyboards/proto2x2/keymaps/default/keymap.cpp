/* proto2x2 :: default — plain keys, no quantum behaviour. */

#include "kb/kb.h"

const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        KC_A, KC_B,
        KC_C, KC_D
    ),
};

const uint8_t keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);

/* Combos are compiled in for this board but this keymap defines none. */
#include "kb/combo.h"
const kb_combo_t kb_combos[] = {};
const uint16_t   kb_combo_count = 0;
