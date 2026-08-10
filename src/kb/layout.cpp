/*
 * layout.cpp — expands the board's LAYOUT_GEOMETRY into a table.
 *
 * One translation unit does the expansion so the KB_KEY macros only ever have
 * one meaning. If the board defines no geometry this compiles to an empty array
 * and the web UI falls back to the grid view.
 */

#include "kb/layout.h"

#ifdef LAYOUT_GEOMETRY
const kb_layout_key_t kb_layout[] = { LAYOUT_GEOMETRY };
const uint16_t kb_layout_count = (uint16_t)(sizeof(kb_layout) / sizeof(kb_layout[0]));
#else
/* A one-element dummy: a zero-length array is not valid C++ and the count is
 * what callers actually test. */
const kb_layout_key_t kb_layout[1] = {};
const uint16_t kb_layout_count = 0;
#endif
