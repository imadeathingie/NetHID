/*
 * oled/oled.h — SSD1306 / SH1106 status display.
 *
 * ── Why this never runs on the scan core ────────────────────────────────────
 * A 128x64 framebuffer is 1024 bytes; at 400 kHz I2C that is roughly 25 ms on
 * the wire. Pushed synchronously from the core that scans the matrix, that is
 * 25 ms of no scanning every time the screen changes — dropped keystrokes that
 * would look exactly like a flaky USB problem, which this project has already
 * spent two rounds chasing once.
 *
 * Two things prevent it:
 *
 *   1. The panel is eight 128-byte pages, and only changed pages are sent. A
 *      status screen touches one or two, so a typical update is ~3 ms.
 *   2. oled_task() sends AT MOST ONE PAGE per call and returns. It is a state
 *      machine, not a loop, so no single call can stall its caller.
 *
 * On the primary it runs on core 0 beside lwIP, which has slack; core 1 does
 * nothing but USB and the matrix and is never asked to wait for a display. On a
 * module there is one core, but there is also nothing else on it.
 *
 * ── SH1106 ──────────────────────────────────────────────────────────────────
 * Sold as SSD1306 constantly. It is 132 columns wide with the visible 128
 * offset by 2, so a driver that does not know produces a picture shifted two
 * pixels with a wrapped stripe down one edge. Set OLED_SH1106.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef OLED_ENABLE
#define OLED_ENABLE 0
#endif

#ifndef OLED_WIDTH
#define OLED_WIDTH 128
#endif
#ifndef OLED_HEIGHT
#define OLED_HEIGHT 64
#endif
#ifndef OLED_I2C_ADDR
#define OLED_I2C_ADDR 0x3C          /* 0x3D on some panels */
#endif
#ifndef OLED_I2C_HZ
#define OLED_I2C_HZ 400000
#endif

#ifndef OLED_SH1106
#define OLED_SH1106 0
#endif
#if OLED_SH1106
#define OLED_COL_OFFSET 2
#else
#define OLED_COL_OFFSET 0
#endif

/*
 * Blank the panel after this long without a keypress. OLEDs burn in, and a
 * static layer indicator shown eight hours a day is exactly the pattern that
 * does it. 0 disables.
 */
#ifndef OLED_TIMEOUT_MS
#define OLED_TIMEOUT_MS 600000      /* 10 minutes */
#endif

#define OLED_PAGES (OLED_HEIGHT / 8)

bool oled_init(void);

/* Send at most one dirty page, then return. */
void oled_task(void);

/* Note activity, for the burn-in timeout. */
void oled_activity(void);

/* ── Drawing ────────────────────────────────────────────────────────────────
 * These touch the framebuffer only and mark pages dirty; nothing reaches the
 * wire until oled_task() runs. */
void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_text(int x, int y, const char *s);
void oled_text_inv(int x, int y, const char *s);
void oled_rect(int x, int y, int w, int h, bool on);
bool oled_dirty(void);

/* Framebuffer access, for tests and for a future configurable renderer. */
const uint8_t *oled_buffer(void);
