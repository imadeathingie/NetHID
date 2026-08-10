/*
 * kb/macro_store.h — runtime macros for the physical keymap.
 *
 * KB_MACRO(n) on its own needs C in your keymap.cpp and a reflash. This gives
 * the same keycodes a bytecode body you can build in the web UI: the same
 * step model the custom-button tab already uses (tap / hold / release / text /
 * delay), except it executes on the device, because nobody is holding a
 * browser open when you press a physical key.
 *
 * A compiled kb_macro_user() still wins for any id it claims, so the C escape
 * hatch keeps working for anything a step list cannot express — IR bursts,
 * conditionals, reading a sensor.
 *
 * ── Bytecode ────────────────────────────────────────────────────────────────
 *   0x00 END
 *   0x01 TAP    mods key      press mods+key, release both
 *   0x02 DOWN   mods key      press and hold
 *   0x03 UP     mods key      release
 *   0x04 DELAY  lo hi         milliseconds, little-endian
 *   0x05 TEXT   len bytes…    type an ASCII string
 *
 * `mods` is a raw HID modifier byte (MOD_LCTRL … MOD_RGUI), so a step can mix
 * left and right modifiers — which the QK_MODS keycode packing cannot.
 *
 * ── Flash ───────────────────────────────────────────────────────────────────
 * Its own sector, one below the keymap's:
 *
 *   magic "NHMC" u32, version u16, count u8, reserved u8, crc32 u32,
 *   pool_len u16, reserved u16, offsets u16[KB_MACRO_COUNT], pool u8[]
 *
 * 0xFFFF in the offset table means "no macro at this id".
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef KB_MACRO_COUNT
#define KB_MACRO_COUNT 16
#endif

/* Bytecode pool. One flash sector minus the header and offset table. */
#ifndef KB_MACRO_POOL
#define KB_MACRO_POOL 3072
#endif

/* Longest single step payload — caps TEXT so one step cannot eat the pool. */
#ifndef KB_MACRO_TEXT_MAX
#define KB_MACRO_TEXT_MAX 64
#endif

enum {
    MOP_END   = 0x00,
    MOP_TAP   = 0x01,
    MOP_DOWN  = 0x02,
    MOP_UP    = 0x03,
    MOP_DELAY = 0x04,
    MOP_TEXT  = 0x05,
};

void kb_macro_store_init(void);

/* Bytecode for one id, or NULL if that id has no stored macro. */
const uint8_t *kb_macro_body(uint8_t id, uint16_t *len_out);

/* Replace one macro. `body` must be valid bytecode; it is verified before
 * anything is written, so a malformed program cannot get into the pool.
 * Passing len 0 deletes. Returns false if it fails to verify or won't fit. */
bool kb_macro_set(uint8_t id, const uint8_t *body, uint16_t len);

/* Bytes used and available, for the UI. */
uint16_t kb_macro_pool_used(void);
uint16_t kb_macro_pool_size(void);

void kb_macro_save_request(void);
void kb_macro_commit_poll(void);
bool kb_macro_dirty(void);
bool kb_macro_stored(void);
void kb_macro_clear_all(void);
