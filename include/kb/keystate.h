/*
 * kb/keystate.h — the merged keyboard state.
 *
 * Two independent sources can hold keys down at the same time: the physical
 * matrix and the network clients (web UI / TCP). Each owns a 256-bit held-key
 * bitmap plus a modifier byte; hid.cpp ORs them together into the single
 * 8-byte report the host sees.
 *
 * This is why physical keys cannot go through the existing hid_cmd queue: a
 * queue carries *edges*, and a keyboard has to transmit *state*. If a network
 * report were dequeued between two matrix reports it would release keys the
 * user is still physically holding.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    KB_SRC_MATRIX = 0,
    KB_SRC_NET    = 1,
    KB_SRC_COUNT
} kb_source_t;

void keystate_init(void);

/* Press/release a single HID usage in one source. Modifier usages
 * (0xE0-0xE7) fold into that source's modifier byte automatically. */
void keystate_press(kb_source_t src, uint8_t usage);
void keystate_release(kb_source_t src, uint8_t usage);
void keystate_clear(kb_source_t src);

/* Transient modifier overlay — mods applied on top of physically held ones,
 * e.g. the shift half of LSFT(KC_9) or a one-shot mod. */
void keystate_set_weak_mods(kb_source_t src, uint8_t mods);
uint8_t keystate_get_mods(kb_source_t src);

/* Replace a whole source's state at once. Used by the network path so an
 * incoming 8-byte report still behaves exactly as it does today. */
void keystate_set_report(kb_source_t src, uint8_t mods, const uint8_t keys[6]);

/* True if the composed report differs from the last one handed out. */
bool keystate_dirty(void);

/* Undo a compose whose report was never accepted by the endpoint.
 * keystate_compose() marks itself clean and records what it returned as
 * last-sent; if the send then fails, without this the keyboard stays wedged in
 * a state the host never saw until something else changes. */
void keystate_mark_dirty(void);

/* Compose MATRIX ⊕ NET into `out` and mark clean. Returns false if nothing
 * changed. On >6 concurrent keys all six slots become KC_ROLL_OVER, which is
 * what the HID spec asks for and what hosts expect. */
bool keystate_compose(void *out /* hid_keyboard_report_t* */);
