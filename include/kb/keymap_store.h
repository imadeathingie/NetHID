/*
 * kb/keymap_store.h — the runtime keymap.
 *
 * Without this feature the keymap is the `const` array in your keymap.cpp,
 * read straight out of flash, and changing a key means a recompile and a
 * reflash. With it, that array becomes the *default*: it is copied into RAM at
 * boot and the scanner reads the RAM copy, so a key can be reassigned live
 * over HTTP and — separately, on request — persisted to flash.
 *
 * This is the bit QMK can't do without VIA/Vial plus a host application. The
 * web server is already here, so the editor is just another tab.
 *
 * Layout of the persisted blob (one flash sector, the last one):
 *
 *   magic "NHKM"  u32   identifies the sector
 *   version       u16   bumped on any format change
 *   rows/cols     u8×2  must match the running firmware or the blob is ignored
 *   layers        u8    how many layers the blob carries
 *   reserved      u8
 *   crc32         u32   over the keycode data only
 *   keycodes      u16[layers][rows][cols]  little-endian
 *
 * rows/cols are checked because flashing a different board over the top would
 * otherwise reinterpret the old bytes as a valid keymap of the wrong shape.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Space reserved in RAM for layers, independent of how many the compiled
 * keymap defines — the extra ones start out empty and can be filled in from
 * the web UI. 8 layers of a 6x6 board is 576 bytes. */
#ifndef KB_MAX_LAYERS
#define KB_MAX_LAYERS 8
#endif

/* Ignore anything in flash for this build — the compiled keymap always wins.
 * The recovery flag for when a stored keymap is in your way. */
#ifndef KB_KEYMAP_IGNORE_STORED
#define KB_KEYMAP_IGNORE_STORED 0
#endif

/* Let runtime edits survive a recompile of keymap.cpp. Off by default: a
 * freshly flashed keymap should win over a remembered edit. */
#ifndef KB_KEYMAP_KEEP_ON_REFLASH
#define KB_KEYMAP_KEEP_ON_REFLASH 0
#endif

void kb_keymap_store_init(void);

/* ── Encoders ────────────────────────────────────────────────────────────────
 * Stored alongside the keymap in the SAME flash blob, not a sector of their
 * own. They share the keymap's dirty flag and its save button because they are
 * edited in the same breath — a separate store would mean two Save buttons and
 * a way to persist half your changes.
 *
 * action: 0 = CCW, 1 = CW, 2 = press. Same order as encoder_map[][][3]. */
uint16_t kb_encoder_at(uint8_t layer, uint8_t index, uint8_t action);
bool     kb_encoder_set(uint8_t layer, uint8_t index, uint8_t action, uint16_t kc);
uint8_t  kb_encoder_count(void);

/* Read/write a single position. Callers on core 1 (the scanner) and core 0
 * (the web server) touch these concurrently by design — see the note in the
 * .cpp about why that is safe without a lock. */
uint16_t kb_keymap_at(uint8_t layer, uint8_t row, uint8_t col);
bool     kb_keymap_set(uint8_t layer, uint8_t row, uint8_t col, uint16_t kc);
uint8_t  kb_keymap_layers(void);

/* Back to the compiled-in keymap. Does not touch flash unless `erase`. */
void kb_keymap_reset(bool erase);

/* Persisting. The request is queued; kb_keymap_commit_poll() does the work and
 * must be called from core 0's loop — never from an lwIP callback, and never
 * from core 1. */
void kb_keymap_save_request(void);
void kb_keymap_commit_poll(void);

/* State for the UI: has RAM diverged from what's in flash? */
bool kb_keymap_dirty(void);
bool kb_keymap_stored(void);      /* is there a valid blob in flash at all? */
bool kb_keymap_save_pending(void);
