/*
 * hid_layout.cpp — the UK ISO delta from US ANSI.
 *
 * Six characters, and every one of them is a character people put in
 * passwords, paths and email addresses. The failure was silent in both
 * directions: ask for @ and get ", ask for " and get @.
 */

#include "tusb.h"     /* before nethid.h — see tools/check/check_includes.py */
#include "nethid.h"   /* KEY_* and ascii_hid_t */
#include "hid_layout.h"
#include "config.h"
#if ENABLE_SETTINGS
#include "settings.h"
#endif
#include <stddef.h>

const char *const HID_LAYOUT_NAMES[HID_LAYOUT_COUNT + 1] = {
    "US", "UK", nullptr
};

/* The settings page offers exactly these names and stores the index. A layout
 * added to the enum without a name here would be selectable as a number and
 * unnamed in the list; a name without an enum value would be selectable and
 * do nothing. */
static_assert(sizeof(HID_LAYOUT_NAMES) / sizeof(HID_LAYOUT_NAMES[0])
              == HID_LAYOUT_COUNT + 1,
              "HID_LAYOUT_NAMES must name every hid_layout_t, and nothing else");

/*
 * British ISO, as Windows and Linux map it.
 *
 *   @ "   the apostrophe key carries @ shifted; " moved to shift-2
 *   # ~   live on the ISO key beside Enter (usage 0x32), which US ANSI has
 *         no key for at all
 *   \ |   live on the ISO key beside left Shift (usage 0x64). US ANSI puts
 *         them on 0x31, which an ISO board does not have — and Windows maps
 *         0x31 to the same scan code as 0x32, so asking for \ on UK does not
 *         merely fail, it types #
 *
 * £ is shift-3 and is not ASCII, so nothing here can ask for it. The rest of
 * the number row, the brackets, the semicolon and the comma/period/slash
 * cluster are identical to US and are deliberately absent.
 */
static const struct { char ch; uint8_t mod; uint8_t kc; } UK[] = {
    {'@',  MOD_LSHIFT, KEY_APOSTROPHE},   /* 0x34 */
    {'"',  MOD_LSHIFT, KEY_2},            /* 0x1F */
    {'#',  0,          KEY_NONUS_HASH},   /* 0x32 */
    {'~',  MOD_LSHIFT, KEY_NONUS_HASH},
    {'\\', 0,          KEY_NONUS_BSLS},   /* 0x64 */
    {'|',  MOD_LSHIFT, KEY_NONUS_BSLS},
};

bool hid_layout_override(uint8_t layout, char c, struct ascii_hid_s *out) {
    if (layout != HID_LAYOUT_UK) return false;
    for (size_t i = 0; i < sizeof(UK) / sizeof(UK[0]); i++) {
        if (UK[i].ch == c) {
            out->modifier = UK[i].mod;
            out->keycode  = UK[i].kc;
            return true;
        }
    }
    return false;
}

uint8_t hid_layout_active(void) {
#if ENABLE_SETTINGS
    uint8_t l = settings()->keyboard_layout;
    /* A stored byte from a build that knew more layouts than this one would
     * index off the end of every table that trusts it. Fall back rather than
     * type from whatever is next in memory. */
    return l < HID_LAYOUT_COUNT ? l : (uint8_t)HID_LAYOUT_US;
#else
    return (uint8_t)KEYBOARD_LAYOUT;
#endif
}
