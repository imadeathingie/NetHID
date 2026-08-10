/*
 * oled_status.cpp — the status screen.
 *
 * Deliberately the only file that decides what appears. The driver knows how to
 * put pixels on a panel and nothing about keyboards; this knows about keyboards
 * and nothing about I2C. Making the content configurable later means replacing
 * this function, not unpicking the driver.
 *
 * Identical on the primary and on every module: both render from a kb_status_t,
 * one read from local state and one unpacked off the bus. A module therefore
 * shows exactly what the primary shows without owning a keymap.
 */

#include "oled/oled.h"
#include "oled/oled_font.h"   /* OLED_FONT_W, for text layout */
#include "oled/kb_status.h"
/* Deliberately NOT tusb.h/nethid.h. This file renders a kb_status_t and touches
 * nothing else — and a MODULE build has no TinyUSB at all, so including them
 * failed outright with "tusb.h: No such file or directory". The point of a
 * module is that it runs on a plain Pico with no USB stack; see docs/SPLIT.md. */
#include <stdio.h>
#include <string.h>

#if OLED_ENABLE

#ifndef OLED_TITLE
#define OLED_TITLE "NETHID"
#endif

static const char *LAYER_NAMES[] = { "BASE", "NAV", "NUM", "FN" };
#define LAYER_NAME_COUNT ((int)(sizeof(LAYER_NAMES) / sizeof(LAYER_NAMES[0])))

void oled_render_status(void) {
    kb_status_t s;
    kb_status_get(&s);

    oled_clear();
    char line[32];

    /* Title, with the link state next to it: the two things you look at when
     * something is wrong. */
    oled_text(0, 0, OLED_TITLE);
    const char *net = (s.flags & KB_ST_AP)   ? "SETUP"
                    : (s.flags & KB_ST_WIFI) ? "WIFI"
                                             : "NO NET";
    oled_text(OLED_WIDTH - (int)strlen(net) * (OLED_FONT_W + 1), 0, net);

    /* Layer, inverted so it reads at a glance across a desk. */
    const char *lname = (s.layer < LAYER_NAME_COUNT) ? LAYER_NAMES[s.layer] : "?";
    snprintf(line, sizeof(line), " L%u %s ", s.layer, lname);
    oled_text_inv(0, 12, line);

    /* Modifiers, fixed positions. A row that reflows as keys are pressed is
     * unreadable while you are pressing them; blanks in fixed slots are not. */
    oled_text(0, 24, (s.mods & (MOD_LSHIFT | MOD_RSHIFT)) ? "SHF" : "   ");
    oled_text(24, 24, (s.mods & (MOD_LCTRL | MOD_RCTRL))  ? "CTL" : "   ");
    oled_text(48, 24, (s.mods & (MOD_LALT | MOD_RALT))    ? "ALT" : "   ");
    oled_text(72, 24, (s.mods & (MOD_LGUI | MOD_RGUI))    ? "GUI" : "   ");

    if (s.flags & KB_ST_CAPS_WORD)      oled_text(0, 34, "CAPSWORD");
    else if (s.flags & KB_ST_CAPSLOCK)  oled_text(0, 34, "CAPS LOCK");

#if OLED_HEIGHT >= 64
    snprintf(line, sizeof(line), "WPM %u", s.wpm);
    oled_text(0, 46, line);

    /* Which modules are answering. A dash for a module that has gone quiet is
     * the fastest way to find a cable that has worked loose. */
    char mods[20];
    int n = 0;
    mods[n++] = 'M';
    for (int i = 0; i < 8 && n < (int)sizeof(mods) - 1; i++)
        mods[n++] = (s.modules & (1u << i)) ? (char)('0' + i) : '-';
    mods[n] = '\0';
    oled_text(0, 56, mods);

    if (!(s.flags & KB_ST_USB)) oled_text(OLED_WIDTH - 30, 46, "USB?");
#endif
}

#else
void oled_render_status(void) {}
#endif
