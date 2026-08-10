/*
 * oledpad :: default — media on the base layer, a macro layer behind the knob.
 *
 * The bottom-right key is MO(FN), so holding it and turning the knob does
 * something different from turning it alone. Watch the display: the layer
 * indicator changes as you hold it, which is the quickest way to confirm the
 * status snapshot is actually reaching the renderer.
 */

#include "kb/kb.h"
#include "kb/layers.h"
#include "kb/macros.h"
#include "kb/autoclick.h"

enum layers { BASE = 0, FN };

enum macros { M_EMAIL = 0, M_SIG };

const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [BASE] = LAYOUT(
        KC_MPRV,     KC_MPLY,     KC_MNXT,     KC_MUTE,
        LCTL(KC_C),  LCTL(KC_V),  LCTL(KC_Z),  LCTL(KC_Y),
        KB_MACRO(M_EMAIL), KB_MACRO(M_SIG), KC_CALC, MO(FN)
    ),

    /*
     * FN. The three autoclick keys sit here rather than on the base layer, so
     * a stray double-tap while typing cannot leave the spacebar repeating.
     *
     *   AUTOCLK(0)  hold to burst left-click
     *   AUTOCLK(1)  double-tap to latch a space repeat; any tap stops it
     *   AUTOCLK(2)  hold for a fast burst, or triple-tap to latch it
     */
    [FN] = LAYOUT(
        KC_BRID,    KC_BRIU,    KC_WBAK,    QK_BOOT,
        MS_BTN1,    MS_BTN2,    MS_BTN3,    KC_WREF,
        AUTOCLK(0), AUTOCLK(1), AUTOCLK(2), KC_TRNS
    ),
};

const uint8_t keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);

/*
 * The knob: [encoder][CCW, CW, press].
 *
 * Volume normally, scroll on FN. KC_TRNS on the press means FN leaves the mute
 * button alone rather than making it dead.
 */
const kb_keycode_t encoder_map[][NUM_ENCODERS][3] = {
    [BASE] = { { KC_VOLD, KC_VOLU, KC_MUTE } },
    [FN]   = { { MS_WHLD, MS_WHLU, KC_TRNS } },
};

const uint8_t encoder_map_layers =
    (uint8_t)(sizeof(encoder_map) / sizeof(encoder_map[0]));

bool kb_macro_user(uint8_t id, keyrecord_t *rec) {
    if (!rec->event.pressed) return false;
    switch (id) {
    case M_EMAIL: kb_macro_string("me@example.com");        return false;
    case M_SIG:   kb_macro_string("Sent from my keyboard"); return false;
    }
    return false;
}
