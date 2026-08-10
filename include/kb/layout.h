/*
 * kb/layout.h — physical key geometry.
 *
 * The matrix tells you how keys are *wired*. This tells you where they *are*,
 * so the web editor can draw your actual keyboard instead of a rows×cols grid.
 * It is a static const table — compiled into flash, which is right: once a
 * board is wired and screwed together its geometry does not change.
 *
 * Optional. A board that defines no LAYOUT_GEOMETRY still works; the editor
 * falls back to the grid.
 *
 * ── Defining it ─────────────────────────────────────────────────────────────
 * In keyboards/<name>/keyboard.h:
 *
 *   #define LAYOUT_GEOMETRY                    \
 *       KB_KEY(0,0,   0,  0)                   \
 *       KB_KEY(0,1, 100,  0)                   \
 *       KB_KEY_S(3,0,  0,300, 225,100)         \  // 2.25u wide caps lock
 *       KB_KEY_R(4,2, 500,400, 100,100, -15, 450,400)
 *
 * Coordinates and sizes are in hundredths of a key unit: 100 = 1u, 150 = 1.5u.
 * Origin is top-left, y increases downward — the same convention as
 * keyboard-layout-editor.com, which is not an accident. Rotation is in whole
 * degrees about (rx, ry).
 *
 * Writing that by hand for anything past a macropad is miserable, so don't:
 *
 *   # drag your layout on keyboard-layout-editor.com, put "row,col" in each
 *   # key's top-left legend, copy the Raw Data, then:
 *   python3 tools/keyboard/kle_to_layout.py my_layout.json
 *
 * and paste the result in. The same tool runs backwards (--to-kle) to turn an
 * existing LAYOUT_GEOMETRY back into KLE Raw Data, so you can round-trip:
 * export, drag things around in the browser, import.
 */
#pragma once

#include <stdint.h>
#include "keyboard.h"

/* Everything is hundredths of a key unit. Exposed to the web UI as "unit". */
#define KB_LAYOUT_UNIT 100

typedef struct {
    uint8_t  row, col;   /* matrix position this key is wired to */
    uint16_t x, y;       /* top-left corner */
    uint16_t w, h;       /* size; 100 x 100 is a normal key */
    int16_t  rot;        /* degrees clockwise, 0 for almost every key */
    uint16_t rx, ry;     /* centre of rotation */
} kb_layout_key_t;

/* Empty (count 0) when the board defines no geometry. */
extern const kb_layout_key_t kb_layout[];
extern const uint16_t        kb_layout_count;

#define KB_KEY(r, c, x, y)                        { r, c, x, y, 100, 100,   0,  0,  0 },
#define KB_KEY_S(r, c, x, y, w, h)                { r, c, x, y,   w,   h,   0,  0,  0 },
#define KB_KEY_R(r, c, x, y, w, h, rt, rx, ry)    { r, c, x, y,   w,   h,  rt, rx, ry },
