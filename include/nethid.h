#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Host-OS detection ────────────────────────────────────────────────────────
// Detected from USB enumeration behaviour (see usb_descriptors.c). Only Windows
// is reliably detectable (it requests the 0xEE MS OS string descriptor); macOS
// and Linux are left as HOST_OS_UNKNOWN since they can't be told apart reliably.
typedef enum {
    HOST_OS_UNKNOWN = 0,
    HOST_OS_WINDOWS = 1,
} host_os_t;

extern volatile uint8_t g_host_os;   // current detection (host_os_t value)
uint8_t usb_host_os(void);           // accessor for the web server

#ifdef __cplusplus
}
#endif


void dbg(const char *line);  // boot/diagnostic line (printf + HID-typed)

// ── HID report IDs ──────────────────────────────────────────────────────────
#define HID_REPORT_ID_KEYBOARD  1
#define HID_REPORT_ID_MOUSE     2
#define HID_REPORT_ID_ABSMOUSE  3

// ── Report structs ───────────────────────────────────────────────────────────
// TinyUSB (hid.h) already defines hid_keyboard_report_t and hid_mouse_report_t
// with identical layouts. We include tusb.h first everywhere, so we just use
// those definitions directly — no redefinition needed here.

// Absolute pointer report (Report ID 3). X/Y are 0..32767 spanning the full
// screen width/height regardless of resolution. Wheel is a relative delta.
typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    uint16_t x;       // 0..32767, left→right
    uint16_t y;       // 0..32767, top→bottom
    int8_t   wheel;
} hid_abs_report_t;

// Full-scale logical maximum for absolute coordinates.
#define HID_ABS_MAX  32767

// ── HID report queue ─────────────────────────────────────────────────────────

#define HID_QUEUE_SIZE 32

typedef enum {
    HID_CMD_KEY_REPORT,
    HID_CMD_KEY_RELEASE,
    HID_CMD_MOUSE_REPORT,
    HID_CMD_ABS_REPORT,     // absolute pointer position
    HID_CMD_TYPE_STRING,
    HID_CMD_WAKEUP,         // assert USB Remote Wakeup signal
} hid_cmd_type_t;

typedef struct {
    hid_cmd_type_t type;
    union {
        hid_keyboard_report_t key;
        hid_mouse_report_t    mouse;
        hid_abs_report_t      abs;
        struct {
            char    text[256];
            uint8_t len;
            uint8_t delay_ms;
        } str;
    };
    bool auto_release;
} hid_cmd_t;

// Push commands (safe from any core / IRQ level)
bool hid_push_key_report(const hid_keyboard_report_t *report, bool auto_release);
bool hid_push_key_release(void);
bool hid_push_mouse_report(const hid_mouse_report_t *report);
bool hid_push_abs_report(const hid_abs_report_t *report);

bool hid_push_type_string(const char *text, uint8_t len, uint8_t delay_ms);
bool hid_push_wakeup(void);

// State queries
bool hid_is_suspended(void);
bool hid_remote_wakeup_enabled(void);
bool hid_wake_was_blocked(void);
void hid_wake_diag(char *buf, size_t buflen);

// Core 0 task — drains queue and calls tud_task()
void hid_init(void);
void hid_task(void);

// ── ASCII → HID lookup ───────────────────────────────────────────────────────
typedef struct {
    uint8_t modifier;
    uint8_t keycode;
} ascii_hid_t;

bool ascii_to_hid(char c, ascii_hid_t *out);

// ── Modifier bitmasks ────────────────────────────────────────────────────────
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

// ── HID key codes ────────────────────────────────────────────────────────────
#define KEY_NONE        0x00
#define KEY_A           0x04
#define KEY_B           0x05
#define KEY_C           0x06
#define KEY_D           0x07
#define KEY_E           0x08
#define KEY_F           0x09
#define KEY_G           0x0A
#define KEY_H           0x0B
#define KEY_I           0x0C
#define KEY_J           0x0D
#define KEY_K           0x0E
#define KEY_L           0x0F
#define KEY_M           0x10
#define KEY_N           0x11
#define KEY_O           0x12
#define KEY_P           0x13
#define KEY_Q           0x14
#define KEY_R           0x15
#define KEY_S           0x16
#define KEY_T           0x17
#define KEY_U           0x18
#define KEY_V           0x19
#define KEY_W           0x1A
#define KEY_X           0x1B
#define KEY_Y           0x1C
#define KEY_Z           0x1D
#define KEY_1           0x1E
#define KEY_2           0x1F
#define KEY_3           0x20
#define KEY_4           0x21
#define KEY_5           0x22
#define KEY_6           0x23
#define KEY_7           0x24
#define KEY_8           0x25
#define KEY_9           0x26
#define KEY_0           0x27
#define KEY_ENTER       0x28
#define KEY_ESC         0x29
#define KEY_BACKSPACE   0x2A
#define KEY_TAB         0x2B
#define KEY_SPACE       0x2C
#define KEY_MINUS       0x2D
#define KEY_EQUAL       0x2E
#define KEY_LEFTBRACE   0x2F
#define KEY_RIGHTBRACE  0x30
#define KEY_BACKSLASH   0x31
#define KEY_SEMICOLON   0x33
#define KEY_APOSTROPHE  0x34
#define KEY_GRAVE       0x35
#define KEY_COMMA       0x36
#define KEY_DOT         0x37
#define KEY_SLASH       0x38
#define KEY_CAPSLOCK    0x39
#define KEY_F1          0x3A
#define KEY_F2          0x3B
#define KEY_F3          0x3C
#define KEY_F4          0x3D
#define KEY_F5          0x3E
#define KEY_F6          0x3F
#define KEY_F7          0x40
#define KEY_F8          0x41
#define KEY_F9          0x42
#define KEY_F10         0x43
#define KEY_F11         0x44
#define KEY_F12         0x45
#define KEY_PRTSCN      0x46
#define KEY_SCRLK       0x47
#define KEY_PAUSE       0x48
#define KEY_INSERT      0x49
#define KEY_HOME        0x4A
#define KEY_PAGEUP      0x4B
#define KEY_DELETE      0x4C
#define KEY_END         0x4D
#define KEY_PAGEDOWN    0x4E
#define KEY_RIGHT       0x4F
#define KEY_LEFT        0x50
#define KEY_DOWN        0x51
#define KEY_UP          0x52
#define KEY_NUMLOCK     0x53
#define KEY_F13         0x68
#define KEY_F14         0x69
#define KEY_F15         0x6A
#define KEY_F16         0x6B
#define KEY_F17         0x6C
#define KEY_F18         0x6D
#define KEY_F19         0x6E
#define KEY_F20         0x6F
#define KEY_F21         0x70
#define KEY_F22         0x71
#define KEY_F23         0x72
#define KEY_F24         0x73
#define KEY_APP         0x65    // Application/Menu key
#define KEY_LEFTCTRL    0xE0
#define KEY_LEFTSHIFT   0xE1
#define KEY_LEFTALT     0xE2
#define KEY_LEFTGUI     0xE3
#define KEY_RIGHTCTRL   0xE4
#define KEY_RIGHTSHIFT  0xE5
#define KEY_RIGHTALT    0xE6
#define KEY_RIGHTGUI    0xE7
