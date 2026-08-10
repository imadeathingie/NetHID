/*
 * kb/mousekeys.h — move the pointer from the keymap.
 *
 * Relative motion, wheel, and five buttons. Motion repeats on a timer with an
 * acceleration ramp, because a fixed step is either uselessly slow across a
 * 4K display or impossible to land on a checkbox with.
 *
 * NetHID already has a mouse; this is the same HID report driven from the
 * matrix instead of over the network. The two share one endpoint, so a network
 * mouse report sent while a physical button is held will clear that button —
 * unlike the keyboard path there is no state merge here, because mouse motion
 * is genuinely event-based and only the button bits are state.
 *
 * Absolute positioning (HID_CMD_ABS_REPORT) is deliberately not wired up yet.
 */
#pragma once

#include <stdint.h>

/* Milliseconds between motion reports once repeating. 16 ms is ~60 Hz, which
 * is smooth and well under the 1 kHz the endpoint can carry. */
#ifndef MOUSEKEY_INTERVAL
#define MOUSEKEY_INTERVAL 16
#endif

/* Pause after the first step before repeating, so a tap nudges by exactly one
 * step instead of skating away. */
#ifndef MOUSEKEY_DELAY
#define MOUSEKEY_DELAY 12
#endif

/* Ramp: pixels per step at the start, at full speed, and how long to get
 * there. */
#ifndef MOUSEKEY_BASE_SPEED
#define MOUSEKEY_BASE_SPEED 2
#endif
#ifndef MOUSEKEY_MAX_SPEED
#define MOUSEKEY_MAX_SPEED 9
#endif
#ifndef MOUSEKEY_TIME_TO_MAX
#define MOUSEKEY_TIME_TO_MAX 350
#endif

/* Fixed speeds selected by holding MS_ACL0 / MS_ACL1 / MS_ACL2. */
#ifndef MOUSEKEY_SPEED_SLOW
#define MOUSEKEY_SPEED_SLOW 1
#endif
#ifndef MOUSEKEY_SPEED_MED
#define MOUSEKEY_SPEED_MED 4
#endif
#ifndef MOUSEKEY_SPEED_FAST
#define MOUSEKEY_SPEED_FAST 14
#endif

/* The wheel is discrete and wants to be much slower than the cursor. */
#ifndef MOUSEKEY_WHEEL_INTERVAL
#define MOUSEKEY_WHEEL_INTERVAL 80
#endif
#ifndef MOUSEKEY_WHEEL_DELAY
#define MOUSEKEY_WHEEL_DELAY 100
#endif
#ifndef MOUSEKEY_WHEEL_DELTA
#define MOUSEKEY_WHEEL_DELTA 1
#endif

/* Current state, for the web UI or diagnostics. */
uint8_t kb_mousekeys_buttons(void);
