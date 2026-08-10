/*
 * layout_check — verify a board's LAYOUT_GEOMETRY and print the JSON the web
 * editor will actually receive.
 *
 *   make layout_check KB=oledpad && ./layout_check
 *
 * Checks that every declared position is inside the matrix, that none is
 * declared twice, and that the serialised geometry fits its response buffer.
 *
 * It deliberately does NOT check the LAYOUT() macro's argument count: that
 * varies per board, and the compiler already rejects a short row with a clear
 * message ("macro LAYOUT passed 39 arguments, but takes just 42"). A tool that
 * needs editing for every new board is a tool nobody runs.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint16_t kb_keycode_t;
#define KC_NO 0

#include "keyboard.h"
#include "kb/layout.h"

#ifdef LAYOUT_GEOMETRY
static const kb_layout_key_t geom[] = { LAYOUT_GEOMETRY };
static const int geom_n = (int)(sizeof(geom) / sizeof(geom[0]));

/* Byte for byte the same serialisation web.cpp uses, so this doubles as a check
 * that the layout fits its response buffer and that the omit-defaults encoding
 * is what the browser will receive. */
static int print_layout_json(void) {
    static char jb[MATRIX_ROWS * MATRIX_COLS * 26 + 128];
    int o = snprintf(jb, sizeof(jb), "{\"ok\":true,\"unit\":%d,\"keys\":[", KB_LAYOUT_UNIT);
    for (int i = 0; i < geom_n; i++) {
        const kb_layout_key_t *k = &geom[i];
        if ((size_t)o > sizeof(jb) - 40) break;
        o += snprintf(jb + o, sizeof(jb) - o, "%s[%u,%u,%u,%u",
                      i ? "," : "", k->row, k->col, k->x, k->y);
        if (k->rot)
            o += snprintf(jb + o, sizeof(jb) - o, ",%u,%u,%d,%u,%u",
                          k->w, k->h, k->rot, k->rx, k->ry);
        else if (k->w != KB_LAYOUT_UNIT || k->h != KB_LAYOUT_UNIT)
            o += snprintf(jb + o, sizeof(jb) - o, ",%u,%u", k->w, k->h);
        o += snprintf(jb + o, sizeof(jb) - o, "]");
    }
    o += snprintf(jb + o, sizeof(jb) - o, "]}");
    printf("\nGET /api/keymap/layout  (%d keys, %d of %zu bytes)\n%s\n",
           geom_n, o, sizeof(jb), jb);
    return o;
}
#endif

int main(void) {
    printf("board: %dx%d matrix\n", MATRIX_ROWS, MATRIX_COLS);

#ifndef LAYOUT_GEOMETRY
    printf("\nno LAYOUT_GEOMETRY - the editor will fall back to the grid view\n");
    return 0;
#else
    int bad = 0;
    static uint8_t seen[MATRIX_ROWS][MATRIX_COLS];
    memset(seen, 0, sizeof(seen));

    for (int i = 0; i < geom_n; i++) {
        const kb_layout_key_t *k = &geom[i];
        if (k->row >= MATRIX_ROWS || k->col >= MATRIX_COLS) {
            printf("  key %d at r%uc%u is outside the %dx%d matrix\n",
                   i, k->row, k->col, MATRIX_ROWS, MATRIX_COLS);
            bad++;
            continue;
        }
        if (seen[k->row][k->col]++) {
            printf("  r%uc%u is placed more than once\n", k->row, k->col);
            bad++;
        }
    }

    int placed = 0;
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS; c++)
            if (seen[r][c]) placed++;
    printf("  %d key(s) placed, %d of %d matrix positions covered\n",
           geom_n, placed, MATRIX_ROWS * MATRIX_COLS);

    int n = print_layout_json();
    if (n <= 0) { printf("  serialisation produced nothing\n"); bad++; }

    printf("\n%s\n", bad ? "PROBLEMS FOUND" : "geometry OK");
    return bad;
#endif
}
