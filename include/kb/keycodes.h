/*
 * kb/keycodes.h — keycode space for the physical-keyboard layer.
 *
 * Basic keycodes (0x0000-0x00FF) are exactly USB HID usage IDs, so they alias
 * straight onto the KEY_* defines already in nethid.h — one source of truth.
 * Everything above 0x00FF is a "quantum" keycode interpreted by a feature in
 * src/kb/features/. If the owning feature is compiled out, the keycode is
 * simply never matched and behaves as KC_NO.
 *
 * The MODS / MOD_TAP / LAYER_TAP encodings are deliberately identical to QMK's
 * so keymaps written for QMK read the same here.
 */
#pragma once

#include <stdint.h>
#include "tusb.h"      /* nethid.h references TinyUSB report types */
#include "nethid.h"

typedef uint16_t kb_keycode_t;

/* ── Basic (0x0000-0x00FF) ─────────────────────────────────────────────── */
#define KC_NO            0x0000
#define KC_TRNS          0x0001   /* fall through to the layer below       */
#define KC_TRANSPARENT   KC_TRNS
#define KC_ROLL_OVER     0x0001   /* HID ErrorRollOver, sent on overflow   */

#define KC_A          KEY_A
#define KC_B          KEY_B
#define KC_C          KEY_C
#define KC_D          KEY_D
#define KC_E          KEY_E
#define KC_F          KEY_F
#define KC_G          KEY_G
#define KC_H          KEY_H
#define KC_I          KEY_I
#define KC_J          KEY_J
#define KC_K          KEY_K
#define KC_L          KEY_L
#define KC_M          KEY_M
#define KC_N          KEY_N
#define KC_O          KEY_O
#define KC_P          KEY_P
#define KC_Q          KEY_Q
#define KC_R          KEY_R
#define KC_S          KEY_S
#define KC_T          KEY_T
#define KC_U          KEY_U
#define KC_V          KEY_V
#define KC_W          KEY_W
#define KC_X          KEY_X
#define KC_Y          KEY_Y
#define KC_Z          KEY_Z
#define KC_1          KEY_1
#define KC_2          KEY_2
#define KC_3          KEY_3
#define KC_4          KEY_4
#define KC_5          KEY_5
#define KC_6          KEY_6
#define KC_7          KEY_7
#define KC_8          KEY_8
#define KC_9          KEY_9
#define KC_0          KEY_0
#define KC_ENTER      KEY_ENTER
#define KC_ENT        KEY_ENTER
#define KC_ESC        KEY_ESC
#define KC_BACKSPACE  KEY_BACKSPACE
#define KC_BSPC       KEY_BACKSPACE
#define KC_TAB        KEY_TAB
#define KC_SPACE      KEY_SPACE
#define KC_SPC        KEY_SPACE
#define KC_MINUS      KEY_MINUS
#define KC_MINS       KEY_MINUS
#define KC_EQUAL      KEY_EQUAL
#define KC_EQL        KEY_EQUAL
#define KC_LEFTBRACE  KEY_LEFTBRACE
#define KC_LBRC       KEY_LEFTBRACE
#define KC_RIGHTBRACE KEY_RIGHTBRACE
#define KC_RBRC       KEY_RIGHTBRACE
#define KC_BACKSLASH  KEY_BACKSLASH
#define KC_BSLS       KEY_BACKSLASH
#define KC_SEMICOLON  KEY_SEMICOLON
#define KC_SCLN       KEY_SEMICOLON
#define KC_APOSTROPHE KEY_APOSTROPHE
#define KC_QUOT       KEY_APOSTROPHE
#define KC_GRAVE      KEY_GRAVE
#define KC_GRV        KEY_GRAVE
#define KC_COMMA      KEY_COMMA
#define KC_COMM       KEY_COMMA
#define KC_DOT        KEY_DOT
#define KC_SLASH      KEY_SLASH
#define KC_SLSH       KEY_SLASH
#define KC_CAPSLOCK   KEY_CAPSLOCK
#define KC_CAPS       KEY_CAPSLOCK
#define KC_F1         KEY_F1
#define KC_F2         KEY_F2
#define KC_F3         KEY_F3
#define KC_F4         KEY_F4
#define KC_F5         KEY_F5
#define KC_F6         KEY_F6
#define KC_F7         KEY_F7
#define KC_F8         KEY_F8
#define KC_F9         KEY_F9
#define KC_F10        KEY_F10
#define KC_F11        KEY_F11
#define KC_F12        KEY_F12
#define KC_PRTSCN     KEY_PRTSCN
#define KC_PSCR       KEY_PRTSCN
#define KC_SCRLK      KEY_SCRLK
#define KC_SCRL       KEY_SCRLK
#define KC_PAUSE      KEY_PAUSE
#define KC_PAUS       KEY_PAUSE
#define KC_INSERT     KEY_INSERT
#define KC_INS        KEY_INSERT
#define KC_HOME       KEY_HOME
#define KC_PAGEUP     KEY_PAGEUP
#define KC_PGUP       KEY_PAGEUP
#define KC_DELETE     KEY_DELETE
#define KC_DEL        KEY_DELETE
#define KC_END        KEY_END
#define KC_PAGEDOWN   KEY_PAGEDOWN
#define KC_PGDN       KEY_PAGEDOWN
#define KC_RIGHT      KEY_RIGHT
#define KC_RGHT       KEY_RIGHT
#define KC_LEFT       KEY_LEFT
#define KC_DOWN       KEY_DOWN
#define KC_UP         KEY_UP
#define KC_NUMLOCK    KEY_NUMLOCK
#define KC_NUM        KEY_NUMLOCK
#define KC_F13        KEY_F13
#define KC_F14        KEY_F14
#define KC_F15        KEY_F15
#define KC_F16        KEY_F16
#define KC_F17        KEY_F17
#define KC_F18        KEY_F18
#define KC_F19        KEY_F19
#define KC_F20        KEY_F20
#define KC_F21        KEY_F21
#define KC_F22        KEY_F22
#define KC_F23        KEY_F23
#define KC_F24        KEY_F24
#define KC_APP        KEY_APP

/* ── Keypad ───────────────────────────────────────────────────────────────
 * Long and short spellings both, as QMK has them: keymaps written by hand
 * tend to use the long form, and the web editor's export emits the short one.
 * A name the editor can produce but the firmware cannot compile is a keymap
 * that exports and then fails to build — which is exactly what these did,
 * because the editor has always offered the number pad and this header never
 * defined it. */
#define KC_KP_SLASH    KEY_KP_SLASH
#define KC_PSLS        KEY_KP_SLASH
#define KC_KP_ASTERISK KEY_KP_ASTERISK
#define KC_PAST        KEY_KP_ASTERISK
#define KC_KP_MINUS    KEY_KP_MINUS
#define KC_PMNS        KEY_KP_MINUS
#define KC_KP_PLUS     KEY_KP_PLUS
#define KC_PPLS        KEY_KP_PLUS
#define KC_KP_ENTER    KEY_KP_ENTER
#define KC_PENT        KEY_KP_ENTER
#define KC_KP_1        KEY_KP_1
#define KC_P1          KEY_KP_1
#define KC_KP_2        KEY_KP_2
#define KC_P2          KEY_KP_2
#define KC_KP_3        KEY_KP_3
#define KC_P3          KEY_KP_3
#define KC_KP_4        KEY_KP_4
#define KC_P4          KEY_KP_4
#define KC_KP_5        KEY_KP_5
#define KC_P5          KEY_KP_5
#define KC_KP_6        KEY_KP_6
#define KC_P6          KEY_KP_6
#define KC_KP_7        KEY_KP_7
#define KC_P7          KEY_KP_7
#define KC_KP_8        KEY_KP_8
#define KC_P8          KEY_KP_8
#define KC_KP_9        KEY_KP_9
#define KC_P9          KEY_KP_9
#define KC_KP_0        KEY_KP_0
#define KC_P0          KEY_KP_0
#define KC_KP_DOT      KEY_KP_DOT
#define KC_PDOT        KEY_KP_DOT
#define KC_KP_EQUAL    KEY_KP_EQUAL
#define KC_PEQL        KEY_KP_EQUAL
#define KC_KP_COMMA    KEY_KP_COMMA
#define KC_PCMM        KEY_KP_COMMA

/* ── Keys the ANSI layout does not have ───────────────────────────────── */
#define KC_NONUS_HASH KEY_NONUS_HASH
#define KC_NUHS       KEY_NONUS_HASH
#define KC_NONUS_BSLS KEY_NONUS_BSLS
#define KC_NUBS       KEY_NONUS_BSLS
#define KC_KB_POWER   KEY_KB_POWER

/* ── Editing and system ───────────────────────────────────────────────── */
#define KC_EXECUTE    KEY_EXECUTE
#define KC_EXEC       KEY_EXECUTE
#define KC_HELP       KEY_HELP
#define KC_MENU       KEY_MENU
#define KC_SELECT     KEY_SELECT
#define KC_SLCT       KEY_SELECT
#define KC_STOP       KEY_STOP
#define KC_AGAIN      KEY_AGAIN
#define KC_AGIN       KEY_AGAIN
#define KC_UNDO       KEY_UNDO
#define KC_CUT        KEY_CUT
#define KC_COPY       KEY_COPY
#define KC_PASTE      KEY_PASTE
#define KC_PSTE       KEY_PASTE
#define KC_FIND       KEY_FIND

/* Volume on the KEYBOARD page. Deliberately KB_-prefixed: KC_MUTE, KC_VOLU and
 * KC_VOLD are the Consumer Control usages (report 4), which is what actually
 * changes the volume on a modern host. These are the other thing with the same
 * name, and silently having one shadow the other would be very hard to debug. */
#define KC_KB_MUTE    KEY_KB_MUTE
#define KC_KB_VOLU    KEY_KB_VOLUP
#define KC_KB_VOLD    KEY_KB_VOLDOWN

/* ── International and language ───────────────────────────────────────── */
#define KC_INT1       KEY_INT1
#define KC_RO         KEY_INT1
#define KC_INT2       KEY_INT2
#define KC_KANA       KEY_INT2
#define KC_INT3       KEY_INT3
#define KC_JYEN       KEY_INT3
#define KC_INT4       KEY_INT4
#define KC_HENK       KEY_INT4
#define KC_INT5       KEY_INT5
#define KC_MHEN       KEY_INT5
#define KC_LANG1      KEY_LANG1
#define KC_HAEN       KEY_LANG1
#define KC_LANG2      KEY_LANG2
#define KC_HANJ       KEY_LANG2
#define KC_LEFTCTRL   KEY_LEFTCTRL
#define KC_LCTL       KEY_LEFTCTRL
#define KC_LEFTSHIFT  KEY_LEFTSHIFT
#define KC_LSFT       KEY_LEFTSHIFT
#define KC_LEFTALT    KEY_LEFTALT
#define KC_LALT       KEY_LEFTALT
#define KC_LEFTGUI    KEY_LEFTGUI
#define KC_LGUI       KEY_LEFTGUI
#define KC_RIGHTCTRL  KEY_RIGHTCTRL
#define KC_RCTL       KEY_RIGHTCTRL
#define KC_RIGHTSHIFT KEY_RIGHTSHIFT
#define KC_RSFT       KEY_RIGHTSHIFT
#define KC_RIGHTALT   KEY_RIGHTALT
#define KC_RALT       KEY_RIGHTALT
#define KC_RIGHTGUI   KEY_RIGHTGUI
#define KC_RGUI       KEY_RIGHTGUI
/* ── Quantum ranges ───────────────────────────────────────────────────────
 * 0x0100-0x1FFF  modified basic      LCTL(kc), LSFT(kc), ...
 * 0x2000-0x3FFF  mod-tap            MT(mods, kc)
 * 0x4000-0x4FFF  layer-tap          LT(layer, kc)
 * 0x5100-0x57FF  layer / one-shot / caps-word ops
 * 0x7E00-0x7EFF  user macros        KB_MACRO(n)
 */
#define QK_MODS            0x0100
#define QK_MODS_MAX        0x1FFF
#define QK_MOD_TAP         0x2000
#define QK_MOD_TAP_MAX     0x3FFF
#define QK_LAYER_TAP       0x4000
#define QK_LAYER_TAP_MAX   0x4FFF
#define QK_MOMENTARY       0x5100   /* | layer   MO() */
#define QK_DEF_LAYER       0x5200   /* | layer   DF() */
#define QK_TOGGLE_LAYER    0x5300   /* | layer   TG() */
#define QK_ONE_SHOT_LAYER  0x5400   /* | layer   OSL() */
#define QK_TO              0x5500   /* | layer   TO() */
#define QK_ONE_SHOT_MOD    0x5600   /* | mods    OSM() */
#define QK_LAYER_OP_MAX    0x56FF
#define QK_CAPS_WORD       0x5700
#define QK_BOOT            0x5800   /* reset into the USB bootloader */
#define QK_MOUSE           0x5900   /* | index   mouse keys, see MS_* below */
#define QK_MOUSE_MAX       0x59FF
#define QK_CONSUMER        0x5A00   /* | index into CONSUMER_USAGES[]       */
#define QK_CONSUMER_MAX    0x5A1F
#define QK_AUTOCLICK       0x5B00   /* | slot index, see kb/autoclick.h      */
#define QK_AUTOCLICK_MAX   0x5B0F
#define QK_MACRO           0x7E00   /* | index   KB_MACRO() */
#define QK_MACRO_MAX       0x7EFF

/* Mod bitfield packed into bits 12..8. Bit 12 set = right-hand mods. */
#define QK_LCTL 0x0100
#define QK_LSFT 0x0200
#define QK_LALT 0x0400
#define QK_LGUI 0x0800
#define QK_RMOD 0x1000
#define QK_RCTL (QK_RMOD | QK_LCTL)
#define QK_RSFT (QK_RMOD | QK_LSFT)
#define QK_RALT (QK_RMOD | QK_LALT)
#define QK_RGUI (QK_RMOD | QK_LGUI)

/* ── Keymap macros ────────────────────────────────────────────────────── */
#define LCTL(kc) (QK_LCTL | (kc))
#define LSFT(kc) (QK_LSFT | (kc))
#define LALT(kc) (QK_LALT | (kc))
#define LGUI(kc) (QK_LGUI | (kc))
#define RCTL(kc) (QK_RCTL | (kc))
#define RSFT(kc) (QK_RSFT | (kc))
#define RALT(kc) (QK_RALT | (kc))
#define RGUI(kc) (QK_RGUI | (kc))
#define HYPR(kc) (QK_LCTL | QK_LSFT | QK_LALT | QK_LGUI | (kc))
#define MEH(kc)  (QK_LCTL | QK_LSFT | QK_LALT | (kc))

#define MT(mods, kc) (QK_MOD_TAP | ((mods) & 0x1F00) | ((kc) & 0xFF))
#define CTL_T(kc) MT(QK_LCTL, kc)
#define SFT_T(kc) MT(QK_LSFT, kc)
#define ALT_T(kc) MT(QK_LALT, kc)
#define GUI_T(kc) MT(QK_LGUI, kc)

#define LT(layer, kc) (QK_LAYER_TAP | (((layer) & 0xF) << 8) | ((kc) & 0xFF))
#define MO(layer)     (QK_MOMENTARY      | ((layer) & 0xFF))
#define DF(layer)     (QK_DEF_LAYER      | ((layer) & 0xFF))
#define TG(layer)     (QK_TOGGLE_LAYER   | ((layer) & 0xFF))
#define OSL(layer)    (QK_ONE_SHOT_LAYER | ((layer) & 0xFF))
#define TO(layer)     (QK_TO             | ((layer) & 0xFF))
#define OSM(mods)     (QK_ONE_SHOT_MOD   | ((mods)  & 0xFF))
#define CAPSWRD       QK_CAPS_WORD
#define QK_RESET      QK_BOOT
#define KB_MACRO(n)   (QK_MACRO | ((n) & 0xFF))
#define AUTOCLK(n)    (QK_AUTOCLICK | ((n) & 0x0F))

/* ── Mouse keys (KB_FEATURE_MOUSEKEYS) ────────────────────────────────────
 * Deliberately NOT squatting in the basic range: 0x00-0xFF is reserved for
 * real USB HID keyboard usage IDs in this firmware, and mouse actions are not
 * keyboard usages. */
#define MS_UP     (QK_MOUSE | 0x00)
#define MS_DOWN   (QK_MOUSE | 0x01)
#define MS_LEFT   (QK_MOUSE | 0x02)
#define MS_RGHT   (QK_MOUSE | 0x03)
#define MS_WHLU   (QK_MOUSE | 0x04)
#define MS_WHLD   (QK_MOUSE | 0x05)
#define MS_WHLL   (QK_MOUSE | 0x06)
#define MS_WHLR   (QK_MOUSE | 0x07)
#define MS_BTN1   (QK_MOUSE | 0x08)   /* left   */
#define MS_BTN2   (QK_MOUSE | 0x09)   /* right  */
#define MS_BTN3   (QK_MOUSE | 0x0A)   /* middle */
#define MS_BTN4   (QK_MOUSE | 0x0B)   /* back    */
#define MS_BTN5   (QK_MOUSE | 0x0C)   /* forward */
#define MS_ACL0   (QK_MOUSE | 0x0D)   /* hold for a fixed slow speed */
#define MS_ACL1   (QK_MOUSE | 0x0E)
#define MS_ACL2   (QK_MOUSE | 0x0F)   /* hold for a fixed fast speed */

/* ── Consumer Control (KB_FEATURE_CONSUMER) ───────────────────────────────
 * Media, volume, brightness and browser keys. These are Consumer page usages,
 * not keyboard usages, and travel on their own HID report.
 *
 * The keycode carries an INDEX into a table rather than the usage itself: the
 * usages are 10-bit and would not fit alongside the 0x5A00 tag in 16 bits.
 * Keep this list and CONSUMER_USAGES[] in src/kb/features/consumer.cpp in the
 * same order — there is a static_assert on the count, but not on the order. */
#define KC_MUTE   (QK_CONSUMER | 0x00)
#define KC_VOLU   (QK_CONSUMER | 0x01)
#define KC_VOLD   (QK_CONSUMER | 0x02)
#define KC_MNXT   (QK_CONSUMER | 0x03)   /* next track     */
#define KC_MPRV   (QK_CONSUMER | 0x04)   /* previous track */
#define KC_MSTP   (QK_CONSUMER | 0x05)   /* stop           */
#define KC_MPLY   (QK_CONSUMER | 0x06)   /* play / pause   */
#define KC_MFFD   (QK_CONSUMER | 0x07)
#define KC_MRWD   (QK_CONSUMER | 0x08)
#define KC_EJCT   (QK_CONSUMER | 0x09)
#define KC_BRIU   (QK_CONSUMER | 0x0A)   /* brightness up   */
#define KC_BRID   (QK_CONSUMER | 0x0B)   /* brightness down */
#define KC_WSCH   (QK_CONSUMER | 0x0C)   /* browser search  */
#define KC_WHOM   (QK_CONSUMER | 0x0D)
#define KC_WBAK   (QK_CONSUMER | 0x0E)
#define KC_WFWD   (QK_CONSUMER | 0x0F)
#define KC_WREF   (QK_CONSUMER | 0x10)
#define KC_CALC   (QK_CONSUMER | 0x11)
#define KC_MYCM   (QK_CONSUMER | 0x12)   /* my computer / file explorer */
#define KC_MAIL   (QK_CONSUMER | 0x13)
#define CONSUMER_COUNT 20

/* ── Predicates ───────────────────────────────────────────────────────── */
static inline bool kc_is_basic(kb_keycode_t kc)   { return kc <= 0x00FF; }
static inline bool kc_is_mod(kb_keycode_t kc)     { return kc >= KEY_LEFTCTRL && kc <= KEY_RIGHTGUI; }
static inline bool kc_in(kb_keycode_t kc, kb_keycode_t lo, kb_keycode_t hi) { return kc >= lo && kc <= hi; }

/* Unpack the 5-bit mod field of a MODS / MOD_TAP keycode into a real HID
 * modifier bitmask (MOD_LCTRL ... MOD_RGUI). */
static inline uint8_t kc_unpack_mods(kb_keycode_t kc) {
    uint8_t m = (uint8_t)((kc >> 8) & 0x0F);
    return (kc & QK_RMOD) ? (uint8_t)(m << 4) : m;
}
static inline uint8_t kc_basic_of(kb_keycode_t kc) { return (uint8_t)(kc & 0xFF); }
static inline uint8_t kc_layer_of(kb_keycode_t kc) { return (uint8_t)((kc >> 8) & 0x0F); }
