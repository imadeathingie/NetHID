#pragma once

// ── Controller ───────────────────────────────────────────────────────────────
// OPT_MCU_RP2040 is correct for both RP2040 and RP2350 in TinyUSB
#define CFG_TUSB_MCU            OPT_MCU_RP2040

// ── OS integration ───────────────────────────────────────────────────────────
#define CFG_TUSB_OS             OPT_OS_PICO

// ── Debug ────────────────────────────────────────────────────────────────────
#define CFG_TUSB_DEBUG          0

// ── Port mode ────────────────────────────────────────────────────────────────
// Required by newer TinyUSB (SDK 2.x bundled version).
// The Pico SDK cmake integration sets BOARD_DEVICE_RHPORT_NUM=0 but we must
// also declare the mode explicitly here.
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#endif

// ── Device ───────────────────────────────────────────────────────────────────
#define CFG_TUD_ENABLED         1

// One composite HID interface (keyboard Report ID 1 + mouse Report ID 2), plus
// a second one for the absolute pointer in ABS_MOUSE_MODE 2.
//
// Deliberately derived from the same macro the descriptor uses rather than set
// by hand: this number sizes TinyUSB's per-instance state, so a descriptor that
// declares two interfaces while this says one enumerates and then drops every
// report on the second. Both come from CMake, so they cannot disagree.
#ifndef ABS_MOUSE_MODE
#define ABS_MOUSE_MODE 2
#endif
#ifndef ENABLE_ABS_MOUSE
#define ENABLE_ABS_MOUSE 1
#endif
#if ENABLE_ABS_MOUSE && ABS_MOUSE_MODE == 2
#define CFG_TUD_HID             2
#else
#define CFG_TUD_HID             1
#endif
#define CFG_TUD_HID_EP_BUFSIZE  64

// Disable everything we don't use
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0
