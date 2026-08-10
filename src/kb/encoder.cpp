/* encoder.cpp — see include/kb/encoder.h */

#include "kb/encoder.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <string.h>

#if NUM_ENCODERS > 0

typedef struct { uint8_t a, b, sw; } enc_pins_t;

/*
 * Gated on NUM_LOCAL_ENCODERS, not NUM_ENCODERS. The two are different numbers
 * and this is the one that says "pins THIS firmware scans": NUM_ENCODERS is the
 * total across the whole keyboard, which is what encoder_map is indexed by.
 *
 * A module with no knob of its own still has NUM_ENCODERS > 0 — module 1 of the
 * `modular` board sets NUM_LOCAL_ENCODERS 0 while the board declares
 * TOTAL_ENCODERS 2 — so the old guard let this expand `{ ENCODERS }` on a
 * header that never defines ENCODERS, and the module firmware did not compile.
 */
#if NUM_LOCAL_ENCODERS > 0
#define ENCODER(a, b, sw) { a, b, sw },
static const enc_pins_t enc_pins[NUM_LOCAL_ENCODERS] = { ENCODERS };
#undef ENCODER
#else
/* Every loop over the local encoders is bounded by NUM_LOCAL_ENCODERS and so
 * never runs; this exists only because C++ has no zero-length array. Same
 * dodge as `enc[]` below. */
static const enc_pins_t enc_pins[1] = { { 0, 0, ENCODER_NO_SW } };
#endif

/*
 * Quadrature transition table, indexed by (previous << 2) | current.
 *
 * A table rather than edge logic on one line: a cheap encoder bounces, and
 * reading only the A edge turns a bounce into a burst of steps in one
 * direction. Every invalid transition maps to 0, so a bounce contributes
 * nothing rather than being counted twice.
 */
static const int8_t QTABLE[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

typedef struct {
    uint8_t  prev;        /* last AB reading */
    int8_t   accum;       /* quadrature steps since the last emission */
    int8_t   pending;     /* whole clicks not yet reported */
    bool     sw_state;
    bool     sw_changed;
    uint32_t sw_at;
} enc_state_t;

/* Sized for every encoder on the keyboard, not just the local ones: injected
 * remote encoders need a slot to queue into. Only the first NUM_LOCAL_ENCODERS
 * have pins. */
static enc_state_t enc[NUM_ENCODERS > 0 ? NUM_ENCODERS : 1];

/* Debounce for the push switch. The rotation path does not need one — the
 * transition table already rejects bounces — but a button does. */
#ifndef ENCODER_SW_DEBOUNCE_MS
#define ENCODER_SW_DEBOUNCE_MS 5
#endif

void encoder_init(void) {
    memset(enc, 0, sizeof(enc));
    for (int i = 0; i < NUM_LOCAL_ENCODERS; i++) {
        gpio_init(enc_pins[i].a);
        gpio_set_dir(enc_pins[i].a, GPIO_IN);
        gpio_pull_up(enc_pins[i].a);
        gpio_init(enc_pins[i].b);
        gpio_set_dir(enc_pins[i].b, GPIO_IN);
        gpio_pull_up(enc_pins[i].b);
        if (enc_pins[i].sw != ENCODER_NO_SW) {
            gpio_init(enc_pins[i].sw);
            gpio_set_dir(enc_pins[i].sw, GPIO_IN);
            gpio_pull_up(enc_pins[i].sw);
        }
        enc[i].prev = (uint8_t)((gpio_get(enc_pins[i].a) << 1) | gpio_get(enc_pins[i].b));
        enc[i].sw_state = false;
    }
}

void encoder_task(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < NUM_LOCAL_ENCODERS; i++) {
        uint8_t cur = (uint8_t)((gpio_get(enc_pins[i].a) << 1) | gpio_get(enc_pins[i].b));
        if (cur != enc[i].prev) {
            int8_t step = QTABLE[(enc[i].prev << 2) | cur];
            if ((ENCODER_REVERSED_MASK >> i) & 1u) step = (int8_t)-step;
            enc[i].accum = (int8_t)(enc[i].accum + step);
            enc[i].prev = cur;

            while (enc[i].accum >= ENCODER_RESOLUTION) {
                enc[i].accum = (int8_t)(enc[i].accum - ENCODER_RESOLUTION);
                if (enc[i].pending < 100) enc[i].pending++;
            }
            while (enc[i].accum <= -ENCODER_RESOLUTION) {
                enc[i].accum = (int8_t)(enc[i].accum + ENCODER_RESOLUTION);
                if (enc[i].pending > -100) enc[i].pending--;
            }
        }

        if (enc_pins[i].sw == ENCODER_NO_SW) continue;
        bool down = !gpio_get(enc_pins[i].sw);        /* pull-up: low = pressed */
        if (down != enc[i].sw_state && (now - enc[i].sw_at) >= ENCODER_SW_DEBOUNCE_MS) {
            enc[i].sw_state  = down;
            enc[i].sw_changed = true;
            enc[i].sw_at = now;
        }
    }
}

bool encoder_next(uint8_t *index, enc_action_t *action, bool *pressed) {
    for (int i = 0; i < NUM_ENCODERS; i++) {
        if (enc[i].sw_changed) {
            enc[i].sw_changed = false;
            *index = (uint8_t)i;
            *action = ENC_PRESS;
            *pressed = enc[i].sw_state;
            return true;
        }
        if (enc[i].pending != 0) {
            bool cw = enc[i].pending > 0;
            enc[i].pending = (int8_t)(enc[i].pending + (cw ? -1 : 1));
            *index = (uint8_t)i;
            *action = cw ? ENC_CW : ENC_CCW;
            /* A detent is momentary: the caller taps the keycode. There is no
             * "held" state for a rotation, which is why `pressed` is true here
             * and the tap is completed by the caller. */
            *pressed = true;
            return true;
        }
    }
    return false;
}

/*
 * Fold in encoders that live on another module. Their deltas arrive over the
 * bus and are queued exactly like local ones, so nothing downstream — the
 * keymap, the layer lookup, the feature chain — can tell where a knob is
 * physically mounted.
 */
void encoder_inject(uint8_t base, const int8_t *deltas, uint8_t count, uint8_t sw_bits) {
    for (uint8_t i = 0; i < count && (base + i) < NUM_LOCAL_ENCODERS; i++) {
        enc[base + i].pending = (int8_t)(enc[base + i].pending + deltas[i]);
        bool down = (sw_bits >> i) & 1;
        if (down != enc[base + i].sw_state) {
            enc[base + i].sw_state = down;
            enc[base + i].sw_changed = true;
        }
    }
}

void encoder_drain(int8_t *deltas, uint8_t *sw_bits) {
    uint8_t bits = 0;
    for (int i = 0; i < NUM_LOCAL_ENCODERS; i++) {
        deltas[i] = enc[i].pending;
        enc[i].pending = 0;
        if (enc[i].sw_state) bits |= (uint8_t)(1u << i);
    }
    *sw_bits = bits;
}

#else   /* no encoders on this board */

void encoder_init(void) {}
void encoder_task(void) {}
bool encoder_next(uint8_t *, enc_action_t *, bool *) { return false; }
void encoder_drain(int8_t *, uint8_t *sw) { *sw = 0; }
void encoder_inject(uint8_t, const int8_t *, uint8_t, uint8_t) {}

#endif
