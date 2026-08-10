/*
 * modular :: default — 42 keys across two modules, plus a 4-key macropad.
 *
 * Each of the first three rows is 12 keys: six on the primary, six on the right
 * module, written in the order they appear. Then 6 thumb keys, then the
 * macropad's 4. LAYOUT() takes exactly 46 arguments and the compiler will say
 * so if a row is short — worth more than it sounds, because a keymap one key
 * out silently shifts everything after it.
 */

#include "kb/kb.h"
#include "kb/combo.h"
#include "kb/layers.h"
#include "kb/macros.h"

enum layers { BASE = 0, NAV, NUM, FN };

const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [BASE] = LAYOUT(
        KC_TAB,  KC_Q,        KC_W,        KC_E,        KC_R,        KC_T,
        KC_Y,    KC_U,        KC_I,        KC_O,        KC_P,        KC_BSPC,

        KC_ESC,  GUI_T(KC_A), ALT_T(KC_S), CTL_T(KC_D), SFT_T(KC_F), KC_G,
        KC_H,    SFT_T(KC_J), CTL_T(KC_K), ALT_T(KC_L), GUI_T(KC_SCLN), KC_QUOT,

        KC_LSFT, KC_Z,        KC_X,        KC_C,        KC_V,        KC_B,
        KC_N,    KC_M,        KC_COMM,     KC_DOT,      KC_SLSH,     KC_RSFT,

                 KC_LGUI, LT(NAV, KC_SPC), LT(NUM, KC_TAB),
                 LT(NUM, KC_ENT), LT(NAV, KC_SPC), KC_RALT,

        /* Macropad. Nothing marks these as living on another module — that is
         * the point of the row-offset layout.
         *
         * Transport keys: Consumer Control usages on their own HID report. */
        KC_MPLY, KC_MPRV, KC_MNXT, KB_MACRO(0)
    ),

    [NAV] = LAYOUT(
        KC_TRNS, KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_NO,
        KC_HOME, KC_PGDN, KC_PGUP, KC_END,  KC_NO,   KC_BSPC,
        KC_TRNS, KC_LGUI, KC_LALT, KC_LCTL, KC_LSFT, CAPSWRD,
        KC_LEFT, KC_DOWN, KC_UP,   KC_RGHT, KC_NO,   KC_NO,
        KC_TRNS, KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_NO,
        KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_TRNS,
                 KC_TRNS, KC_TRNS, MO(FN),
                 MO(FN),  KC_TRNS, KC_TRNS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS
    ),

    [NUM] = LAYOUT(
        KC_TRNS, KC_1,    KC_2,    KC_3,    KC_4,    KC_5,
        KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_BSPC,
        KC_TRNS, KC_LGUI, KC_LALT, KC_LCTL, KC_LSFT, KC_GRV,
        KC_MINS, KC_EQL,  KC_LBRC, KC_RBRC, KC_BSLS, KC_QUOT,
        KC_TRNS, KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_NO,
        KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_TRNS,
                 KC_TRNS, MO(FN),  KC_TRNS,
                 KC_TRNS, MO(FN),  KC_TRNS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS
    ),

    /* Reachable only by holding both thumb layer keys, which puts QK_BOOT
     * somewhere you cannot hit by accident. */
    [FN] = LAYOUT(
        KC_NO,   KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,
        KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,
        KC_NO,   MS_ACL0, MS_ACL1, MS_ACL2, KC_NO,   KC_NO,
        MS_LEFT, MS_DOWN, MS_UP,   MS_RGHT, KC_NO,   KC_F12,
        QK_BOOT, KC_NO,   KC_NO,   KC_NO,   KC_NO,   KC_NO,
        MS_WHLD, MS_WHLU, KC_NO,   KC_NO,   KC_NO,   KC_NO,
                 KC_TRNS, KC_TRNS, KC_TRNS,
                 MS_BTN1, MS_BTN2, KC_TRNS,
        KC_NO,   KC_NO,   KC_NO,   KC_NO
    ),
};

const uint8_t keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);

/*
 * Encoders: [layer][encoder][CCW, CW, press].
 *
 * Encoder 0 is the knob on the primary, encoder 1 the one on the macropad
 * module. All three actions of a knob on one line, because that is how you read
 * a knob. KC_TRNS falls through to the layer below exactly as it does for keys,
 * so a layer that says nothing about a knob leaves it working.
 */
const kb_keycode_t encoder_map[][NUM_ENCODERS][3] = {
    [BASE] = { { KC_VOLD, KC_VOLU, KC_MUTE },
               { KC_MPRV, KC_MNXT, KC_MPLY } },
    [NAV]  = { { KC_PGDN, KC_PGUP, KC_TRNS },
               { KC_BRID, KC_BRIU, KC_TRNS } },
    [NUM]  = { { KC_TRNS, KC_TRNS, KC_TRNS },
               { KC_TRNS, KC_TRNS, KC_TRNS } },
    [FN]   = { { MS_WHLD, MS_WHLU, MS_BTN3 },
               { KC_TRNS, KC_TRNS, KC_TRNS } },
};

const uint8_t encoder_map_layers =
    (uint8_t)(sizeof(encoder_map) / sizeof(encoder_map[0]));

bool kb_macro_user(uint8_t, keyrecord_t *rec) {
    if (rec->event.pressed) kb_macro_string("hello from the macropad\n");
    return false;
}

/* A combo spanning two modules. The bus carries state, so a chord across the
 * cable behaves exactly like a local one — the rows arrive together and
 * combo.cpp cannot tell where any of them came from. */
static const kb_keycode_t combo_esc[] = { SFT_T(KC_J), CTL_T(KC_K), KC_NO };

const kb_combo_t kb_combos[] = {
    { combo_esc, KC_ESC },
};
const uint16_t kb_combo_count = sizeof(kb_combos) / sizeof(kb_combos[0]);
