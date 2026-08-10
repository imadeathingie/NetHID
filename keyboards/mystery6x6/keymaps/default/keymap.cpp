/*
 * mystery6x6 :: default — a starting point once keymaps/debug has told you
 * where the keys actually are.
 *
 * Written through LAYOUT(), so it reads in physical order. Fix the LAYOUT()
 * macro in ../../keyboard.h if debug says the thumb cluster is arranged
 * differently; every keymap then follows automatically.
 */

#include "kb/kb.h"
#include "kb/layers.h"

enum layers { BASE = 0, NAV, MOUSE };

const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [BASE] = LAYOUT(
        KC_TAB,  KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,
        KC_ESC,  KC_A,    KC_S,    KC_D,    KC_F,    KC_G,
        KC_LSFT, KC_Z,    KC_X,    KC_C,    KC_V,    KC_B,
        KC_LCTL, KC_GRV,  KC_LALT, KC_LGUI, KC_MINS, KC_EQL,
                                   KC_1,    KC_2,
                                            KC_3,    KC_4,
                                                     MO(NAV),
                                            KC_SPC,  KC_ENT,
                                                     KC_BSPC
    ),

    [NAV] = LAYOUT(
        KC_TRNS, KC_1,    KC_2,    KC_3,    KC_4,    KC_5,
        KC_TRNS, KC_LEFT, KC_DOWN, KC_UP,   KC_RGHT, KC_HOME,
        KC_TRNS, KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_END,
        TG(MOUSE), KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, QK_BOOT,
                                   KC_TRNS, KC_TRNS,
                                            KC_TRNS, KC_TRNS,
                                                     KC_TRNS,
                                            KC_TRNS, KC_DEL,
                                                     KC_TRNS
    ),

    /* Mouse layer. Latched from NAV with TG(MOUSE) rather than held, because
     * pointing takes both hands and a fair while — a momentary layer means
     * holding a key down for the entire time you are trying to click something.
     * TO(BASE) on the bottom right gets you out.
     *
     * The cursor cross sits on the same keys as the arrow cross on NAV, and the
     * accelerator keys are on the row above: hold MS_ACL0 with the other hand
     * to crawl onto a small target, MS_ACL2 to cross a large display. */
    [MOUSE] = LAYOUT(
        KC_TRNS, MS_ACL0, MS_ACL1, MS_ACL2, KC_TRNS, KC_TRNS,
        KC_TRNS, MS_LEFT, MS_DOWN, MS_UP,   MS_RGHT, KC_TRNS,
        KC_TRNS, MS_WHLL, MS_WHLD, MS_WHLU, MS_WHLR, KC_TRNS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, TO(BASE),
                                   MS_BTN1, MS_BTN2,
                                            MS_BTN3, KC_TRNS,
                                                     KC_TRNS,
                                            MS_BTN1, MS_BTN2,
                                                     KC_TRNS
    ),
};

const uint8_t keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);
