/*
 * oled/kb_status.h — what the display is allowed to know.
 *
 * A deliberately small, flat snapshot. It exists because the display has two
 * awkward consumers:
 *
 *   1. On the primary it is rendered on CORE 0 while the state it describes
 *      lives on core 1. A snapshot of plain scalars, published whole, avoids
 *      the display reaching into the scan loop's internals — and avoids any
 *      locking on the scan hot path.
 *
 *   2. On a module it is rendered from data that arrived over the BUS. A module
 *      has no keymap and cannot know what layer 2 means, so it cannot render
 *      anything the primary has not told it. Shipping a framebuffer instead is
 *      not an option: 1 KB against a bus budgeting a few dozen bytes per poll.
 *
 * So the wire format IS this struct, packed to 6 bytes and carried in the
 * payload of the poll frame the primary already sends every cycle. No extra
 * traffic, and every module is updated every poll.
 *
 * Keep it small. Every byte added is paid on every poll to every module.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hid_mods.h"   /* MOD_*, for decoding the `mods` field below */

#define KB_STATUS_BYTES 6

typedef struct {
    uint8_t  layer;         /* highest active layer                        */
    uint8_t  mods;          /* HID modifier bitmap, for a SHIFT/CTRL line  */
    uint8_t  flags;         /* KB_ST_* below                               */
    uint8_t  wpm;           /* words per minute, saturating at 255         */
    uint16_t modules;       /* bit n = module n online                     */
} kb_status_t;

#define KB_ST_CAPS_WORD  (1u << 0)
#define KB_ST_USB        (1u << 1)   /* host enumerated us          */
#define KB_ST_WIFI       (1u << 2)   /* joined a network            */
#define KB_ST_AP         (1u << 3)   /* serving our own network     */
#define KB_ST_NUMLOCK    (1u << 4)
#define KB_ST_CAPSLOCK   (1u << 5)

/* Publish from core 1 / the primary. Cheap; safe to call every scan. */
void kb_status_set(const kb_status_t *s);

/* Read the latest snapshot. */
void kb_status_get(kb_status_t *out);

/* Wire encoding, shared by both ends so they cannot disagree. */
void kb_status_pack(const kb_status_t *s, uint8_t out[KB_STATUS_BYTES]);
void kb_status_unpack(const uint8_t in[KB_STATUS_BYTES], kb_status_t *out);

/* Draw the status screen into the framebuffer. Isolated on purpose: making the
 * content configurable later means replacing this one function, not unpicking
 * the driver. */
void oled_render_status(void);
