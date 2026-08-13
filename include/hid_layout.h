/*
 * hid_layout.h — which characters the HOST's keyboard layout puts where.
 *
 * A HID usage is a POSITION on the board, not a character. Usage 0x1F is "the
 * second key on the number row"; whether shifting it gives you @ or " is
 * decided entirely by the layout the host has selected, and the device is
 * never told which that is.
 *
 * ascii_to_hid() has to pick a position for a character anyway, so it has to
 * assume a layout. It assumed US ANSI, silently, everywhere — which is fine
 * until the host is set to British, at which point typing a password
 * containing @ puts a " in the box and there is nothing on screen to explain
 * why.
 *
 * This is a DELTA, not a second full table. US and UK ISO agree on all but six
 * printable ASCII characters, and writing the other ninety again would be six
 * useful lines buried in ninety that can rot independently.
 *
 * ── Why this is a setting and not a detection ───────────────────────────────
 * The layout cannot be detected. USB tells the device nothing about it, and
 * the host applies it after the report arrives. usb_host_os() can tell
 * Windows from the rest, but Windows-UK and Windows-US are indistinguishable
 * on the wire, so the choice has to come from whoever set the machine up.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* nethid.h's ascii_hid_t, by tag. Including nethid.h here would oblige every
 * file that includes THIS header to include tusb.h first, for a struct of two
 * bytes it may never name. */
struct ascii_hid_s;

typedef enum {
    HID_LAYOUT_US = 0,   /* US ANSI — what this firmware always assumed */
    HID_LAYOUT_UK = 1,   /* British ISO, as Windows and Linux map it */
    HID_LAYOUT_COUNT
} hid_layout_t;

/* Names for the settings page, indexed by hid_layout_t. NULL-terminated so the
 * settings table can derive its own range from the list and cannot drift. */
extern const char *const HID_LAYOUT_NAMES[HID_LAYOUT_COUNT + 1];

/* Where `layout` puts `c`, if that differs from US ANSI. Returns false when
 * the layout agrees with US — which is the common case, and why the caller
 * keeps its own US table. */
bool hid_layout_override(uint8_t layout, char c, struct ascii_hid_s *out);

/* The layout the typer should assume: the stored setting, or the compiled
 * KEYBOARD_LAYOUT default when the settings store is not built. */
uint8_t hid_layout_active(void);
