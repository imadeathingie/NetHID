/*
 * consumer.cpp — media, volume, brightness and browser keys.
 *
 * These are Consumer page usages and travel on their own HID report, which is
 * why they could not be expressed at all until the descriptor gained report 4.
 *
 * The report carries ONE usage at a time. That is a real limitation of the
 * descriptor chosen — an array of one — and it means two media keys held
 * together do not both register. In practice nobody holds mute and next-track
 * simultaneously, and the alternative is a fixed bitmap that has to be extended
 * every time a new key is wanted.
 */

#include "tusb.h"
#include "nethid.h"
#include "kb/features.h"

/* USB HID Consumer Page (0x0C) usage IDs. Order must match the KC_* defines in
 * kb/keycodes.h. */
static const uint16_t CONSUMER_USAGES[] = {
    0x00E2,   /* KC_MUTE */
    0x00E9,   /* KC_VOLU */
    0x00EA,   /* KC_VOLD */
    0x00B5,   /* KC_MNXT */
    0x00B6,   /* KC_MPRV */
    0x00B7,   /* KC_MSTP */
    0x00CD,   /* KC_MPLY */
    0x00B3,   /* KC_MFFD */
    0x00B4,   /* KC_MRWD */
    0x00B8,   /* KC_EJCT */
    0x006F,   /* KC_BRIU */
    0x0070,   /* KC_BRID */
    0x0221,   /* KC_WSCH */
    0x0223,   /* KC_WHOM */
    0x0224,   /* KC_WBAK */
    0x0225,   /* KC_WFWD */
    0x0227,   /* KC_WREF */
    0x0192,   /* KC_CALC */
    0x0194,   /* KC_MYCM */
    0x018A,   /* KC_MAIL */
};

static_assert(sizeof(CONSUMER_USAGES) / sizeof(CONSUMER_USAGES[0]) == CONSUMER_COUNT,
              "CONSUMER_USAGES and the KC_* list in keycodes.h disagree");

static uint16_t held;

uint16_t kb_consumer_usage(kb_keycode_t kc) {
    uint8_t i = (uint8_t)(kc & 0xFF);
    if (i >= CONSUMER_COUNT) return 0;
    return CONSUMER_USAGES[i];
}

void kb_consumer_init(void) { held = 0; }

bool kb_consumer_process(keyrecord_t *rec) {
    if (!kc_in(rec->keycode, QK_CONSUMER, QK_CONSUMER_MAX)) return true;

    uint16_t usage = kb_consumer_usage(rec->keycode);
    if (!usage) return false;

    if (rec->event.pressed) {
        held = usage;
        hid_push_consumer(usage);
    } else if (held == usage) {
        /* Only release if this is still the key the host thinks is down. A
         * second media key pressed on top replaces it, and releasing the first
         * afterwards must not cancel the second. */
        held = 0;
        hid_push_consumer(0);
    }
    return false;
}

void kb_consumer_task(void) { }
