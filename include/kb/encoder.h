/*
 * kb/encoder.h — rotary encoders on their own pins.
 *
 * Encoders are NOT part of the key matrix. They get dedicated GPIO — two
 * quadrature lines and, optionally, a push switch on a third. That is how they
 * are actually wired, and pretending an encoder is three matrix positions means
 * a matrix that has to grow to fit hardware that is not a matrix.
 *
 * A board (or a module) declares them:
 *
 *     #define ENCODERS                  \
 *         ENCODER(26, 27, 28)           // A, B, switch
 *         ENCODER(20, 21, ENCODER_NO_SW)
 *
 * and the keymap maps rotation and the push to keycodes, per layer:
 *
 *     const kb_keycode_t encoder_map[][NUM_ENCODERS][3] = {
 *         [BASE] = { { KC_VOLD, KC_VOLU, KC_MUTE } },   // CCW, CW, press
 *     };
 *
 * The push switch is the third column rather than a separate table because it
 * is part of the same physical control: you want to see all three of a knob's
 * actions on one line when reading a keymap.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "keyboard.h"

#define ENCODER_NO_SW 0xFF

/*
 * ENCODERS lists the encoders wired to THIS firmware's own pins — the module
 * header supplies it, so each module counts only its own.
 *
 * NUM_ENCODERS is the total across the whole keyboard, because that is what the
 * keymap is indexed by: encoder 0 is the primary's, encoder 1 the next module's,
 * and a keymap should not have to be rewritten when a knob moves between
 * modules. A board with encoders on more than one module states the total as
 * TOTAL_ENCODERS; otherwise the local count is the total.
 */
/*
 * Counts are stated, not derived.
 *
 * The obvious trick — redefine ENCODER as `+1`, expand the list, undef — does
 * not work: the resulting macro expands at its USE site, by which point ENCODER
 * is gone and you get "missing binary operator before token ENCODER" pointing
 * at a board header that is perfectly correct. Two explicit numbers are duller
 * and they work.
 *
 *   NUM_LOCAL_ENCODERS  wired to THIS firmware's pins (a module header sets it)
 *   TOTAL_ENCODERS      across the whole keyboard; what encoder_map is indexed
 *                       by, so a knob can move between modules without the
 *                       keymap being rewritten
 */
#ifndef NUM_LOCAL_ENCODERS
#define NUM_LOCAL_ENCODERS 0
#endif
#ifndef TOTAL_ENCODERS
#define TOTAL_ENCODERS NUM_LOCAL_ENCODERS
#endif
#define NUM_ENCODERS TOTAL_ENCODERS

/* Detents per keycode emission. Most encoders produce four quadrature
 * transitions per physical click; a few produce two. If one click sends two
 * volume steps, this is the knob to turn. */
#ifndef ENCODER_RESOLUTION
#define ENCODER_RESOLUTION 4
#endif

/*
 * Which way is clockwise depends on which of A and B you soldered where, so
 * there is no correct default — only a convention plus a way to flip it. If a
 * knob turns volume down when you expect up, set this rather than swapping the
 * wires or rewriting encoder_map.
 *
 * Per encoder if you have several wired inconsistently: ENCODER_REVERSED_MASK
 * is a bitmap, bit n for encoder n. ENCODER_REVERSED flips all of them.
 */
#ifndef ENCODER_REVERSED
#define ENCODER_REVERSED 0
#endif
#ifndef ENCODER_REVERSED_MASK
#define ENCODER_REVERSED_MASK (ENCODER_REVERSED ? 0xFFFFFFFFu : 0u)
#endif

typedef enum { ENC_CCW = 0, ENC_CW = 1, ENC_PRESS = 2 } enc_action_t;

void encoder_init(void);

/* Poll the pins. Returns the number of pending actions; read them with
 * encoder_next(). Non-blocking, safe to call every scan. */
void encoder_task(void);

/* Pop one pending action. Returns false when there are none. */
bool encoder_next(uint8_t *index, enc_action_t *action, bool *pressed);

/* Raw state, for a module to report over the bus: one signed delta per encoder
 * plus a switch bitmap. Clears the deltas it returns. */
void encoder_drain(int8_t *deltas, uint8_t *sw_bits);

/* Apply deltas that arrived from a module, as if they were local. */
void encoder_inject(uint8_t base_index, const int8_t *deltas, uint8_t count,
                    uint8_t sw_bits);
