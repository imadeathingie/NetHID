#pragma once
#include "kb/kb.h"

/* Implement this in your keymap to handle KB_MACRO(n) keycodes.
 * Return false to stop the event here, true to let it continue. */
bool kb_macro_user(uint8_t id, keyrecord_t *rec);

/* Type a string on the host. Reuses NetHID's existing typer, so it obeys
 * TYPE_DELAY_MS and interleaves correctly with network-issued strings. */
void kb_macro_string(const char *s);
