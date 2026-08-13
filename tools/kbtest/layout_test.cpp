/* Host-side check of what the device will actually TYPE, per layout.
 *
 * ascii_to_hid() assumed US ANSI everywhere and silently. On a host set to
 * British that is not a near miss: six characters come out as a different
 * character, in both directions — ask for @ and get ", ask for " and get @ —
 * so a typed password is wrong in a way that looks like the device is broken.
 *
 * check_layout_tables.py already proves the firmware's table and the web UI's
 * agree. This proves the table is actually reached: that the layout setting is
 * read, that the delta wins over the US table, and that switching back does
 * switch back.
 */
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "nethid.h"
#include "config.h"
#include "settings.h"
#include "hid_layout.h"
#ifndef TAPPING_TERM
#define TAPPING_TERM 200
#endif
#include "hardware/flash.h"
extern uint8_t fake_flash[];

/* The real src/hid.cpp is linked for ascii_to_hid(), which drags in the report
 * queue and therefore TinyUSB. Nothing here sends a report — the queue is not
 * what is under test — so these only have to exist. typer_test.cpp is where
 * the sending is exercised. */
void tud_task(void) {}
bool tud_mounted(void) { return true; }
bool tud_hid_n_ready(uint8_t) { return true; }
bool tud_remote_wakeup(void) { return true; }
bool tud_hid_n_report(uint8_t, uint8_t, void const *, uint16_t) { return true; }

static int fails = 0;
static void ck(const char *n, bool ok) {
    printf("%-52s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

/* What ascii_to_hid resolves `c` to right now, as a printable claim. */
static bool types(char c, uint8_t mod, uint8_t kc) {
    ascii_hid_t a = {};
    if (!ascii_to_hid(c, &a)) return false;
    return a.modifier == mod && a.keycode == kc;
}

static void layout(int l) {
    if (!settings_set("keyboard_layout", l)) {
        printf("  settings_set(keyboard_layout, %d) was REJECTED\n", l);
        fails++;
    }
}

int main(void) {
    memset(fake_flash, 0xFF, PICO_FLASH_SIZE_BYTES);
    settings_init();

    ck("default layout is US", hid_layout_active() == HID_LAYOUT_US);

    /* ── US: unchanged, which is the other half of the promise ─────────────── */
    ck("US: @ is shift-2",            types('@',  MOD_LSHIFT, KEY_2));
    ck("US: a double quote is shift-apostrophe",
                                      types('"',  MOD_LSHIFT, KEY_APOSTROPHE));
    ck("US: # is shift-3",            types('#',  MOD_LSHIFT, KEY_3));
    ck("US: ~ is shift-grave",        types('~',  MOD_LSHIFT, KEY_GRAVE));
    ck("US: a backslash is the ANSI backslash key",
                                      types('\\', 0,          KEY_BACKSLASH));
    ck("US: a pipe is shift on the same key",
                                      types('|',  MOD_LSHIFT, KEY_BACKSLASH));

    /* ── UK ────────────────────────────────────────────────────────────────── */
    layout(HID_LAYOUT_UK);
    ck("UK: the setting is what the typer reads",
       hid_layout_active() == HID_LAYOUT_UK);

    ck("UK: @ moves to the apostrophe key",
                                      types('@',  MOD_LSHIFT, KEY_APOSTROPHE));
    ck("UK: a double quote moves to shift-2",
                                      types('"',  MOD_LSHIFT, KEY_2));
    ck("UK: # is the ISO key by Enter, unshifted",
                                      types('#',  0,          KEY_NONUS_HASH));
    ck("UK: ~ is that key shifted",   types('~',  MOD_LSHIFT, KEY_NONUS_HASH));
    ck("UK: a backslash is the ISO key by left Shift",
                                      types('\\', 0,          KEY_NONUS_BSLS));
    ck("UK: a pipe is that key shifted",
                                      types('|',  MOD_LSHIFT, KEY_NONUS_BSLS));

    /* The delta is a delta. If it had replaced the table rather than being
     * consulted first, everything not listed in it would stop resolving. */
    ck("UK: letters are untouched",   types('a', 0, KEY_A) &&
                                      types('Z', MOD_LSHIFT, KEY_A + 25));
    ck("UK: digits are untouched",    types('1', 0, KEY_1) &&
                                      types('0', 0, KEY_0));
    ck("UK: shift-1 is still !",      types('!', MOD_LSHIFT, KEY_1));
    ck("UK: the semicolon is untouched",
                                      types(';', 0, KEY_SEMICOLON) &&
                                      types(':', MOD_LSHIFT, KEY_SEMICOLON));
    ck("UK: the brackets are untouched",
                                      types('[', 0, KEY_LEFTBRACE) &&
                                      types('}', MOD_LSHIFT, KEY_RIGHTBRACE));
    ck("UK: nothing new becomes typeable",
       !types('\x80', 0, 0) && !types('\x01', 0, 0));

    /* ── and back ──────────────────────────────────────────────────────────── */
    layout(HID_LAYOUT_US);
    ck("switching back restores the US answers",
       types('@', MOD_LSHIFT, KEY_2) && types('\\', 0, KEY_BACKSLASH));

    /* The stored byte is trusted by every table that indexes on it. A value
     * from a build that knew more layouts must not index off the end. */
    ck("an out-of-range layout is refused by the setter",
       !settings_set("keyboard_layout", HID_LAYOUT_COUNT));
    ck("and the layout is unchanged by the refusal",
       hid_layout_active() == HID_LAYOUT_US);

    /* The choice has to survive a power cycle, or it is a per-session toggle
     * dressed up as configuration. */
    layout(HID_LAYOUT_UK);
    settings_save_request();
    settings_commit_poll();
    settings_init();
    ck("the layout survives a reboot",
       hid_layout_active() == HID_LAYOUT_UK &&
       types('@', MOD_LSHIFT, KEY_APOSTROPHE));

    printf(fails ? "\n%d FAILED\n" : "\nall green\n", fails);
    return fails ? 1 : 0;
}
