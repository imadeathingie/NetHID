/*
 * hid_mods.h — the USB HID keyboard modifier byte, and nothing else.
 *
 * These bits live here rather than in nethid.h because nethid.h declares the
 * HID queue in TinyUSB's types, so including it drags in tusb.h — which a
 * MODULE firmware does not have and deliberately never will (see docs/SPLIT.md;
 * that is what lets a module run on a plain Pico rather than a W).
 *
 * The status renderer only wants to know whether shift is down. It should not
 * have to link a USB stack to find that out, and before this header it did:
 * oled_status.cpp included tusb.h purely to reach these eight defines, which
 * broke every module build.
 *
 * nethid.h and oled/kb_status.h both include this, so existing users see no
 * change and anything decoding kb_status_t.mods gets the names for that field.
 */
#pragma once

#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80
