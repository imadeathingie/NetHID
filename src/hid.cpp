/*
 * hid.cpp — USB HID report queue, TinyUSB callbacks, Remote Wakeup
 *
 * Remote Wakeup
 * ─────────────
 * When the USB host suspends, tud_suspend_cb() fires.  If the host granted
 * Remote Wakeup permission (it sets remote_wakeup_en=true), we store that
 * flag.  When a hid_push_wakeup() command is dequeued while suspended, we
 * call tud_remote_wakeup() which asserts the K-state resume signal on the
 * USB bus.  The host wakes, calls tud_resume_cb(), and we then send the
 * queued HID reports normally.
 */

#include "tusb.h"
#include "nethid.h"
#include "config.h"
#include "pico/stdlib.h"
#include "pico/sync.h"
#include <string.h>
#include <stdio.h>

// ── Suspend / wakeup state ────────────────────────────────────────────────────
static volatile bool _suspended          = false;
static volatile bool _remote_wakeup_en   = false;
static volatile bool _wake_requested_while_blocked = false;

// Diagnostics so we can see what actually happened around suspend/wake,
// reported by typing once the host is awake (no UART needed).
static volatile uint32_t _diag_suspend_count = 0;
static volatile uint32_t _diag_wake_cmds     = 0;
static volatile int      _diag_last_wakeup_en = -1;  // -1=never suspended
static volatile int      _diag_last_wake_rc   = -1;  // tud_remote_wakeup() ret

bool hid_is_suspended(void)           { return _suspended; }
bool hid_remote_wakeup_enabled(void)  { return _remote_wakeup_en; }
bool hid_wake_was_blocked(void)       { return _wake_requested_while_blocked; }

// ── Queue ────────────────────────────────────────────────────────────────────

static hid_cmd_t  q_buf[HID_QUEUE_SIZE];
static volatile uint32_t q_head = 0;
static volatile uint32_t q_tail = 0;
static spin_lock_t *q_lock = nullptr;

static inline uint32_t q_next(uint32_t idx) {
    return (idx + 1) & (HID_QUEUE_SIZE - 1);
}

static bool q_push(const hid_cmd_t *cmd) {
    uint32_t save = spin_lock_blocking(q_lock);
    uint32_t next = q_next(q_head);
    if (next == q_tail) { spin_unlock(q_lock, save); return false; }
    q_buf[q_head] = *cmd;
    q_head = next;
    spin_unlock(q_lock, save);
    return true;
}

static bool q_peek(hid_cmd_t *out) {
    if (q_tail == q_head) return false;
    *out = q_buf[q_tail];
    return true;
}

static void q_pop(void) {
    if (q_tail != q_head) q_tail = q_next(q_tail);
}

// ── Public push API ──────────────────────────────────────────────────────────

bool hid_push_key_report(const hid_keyboard_report_t *report, bool auto_release) {
    hid_cmd_t cmd = {};
    cmd.type = HID_CMD_KEY_REPORT;
    cmd.key  = *report;
    cmd.auto_release = auto_release;
    return q_push(&cmd);
}

bool hid_push_key_release(void) {
    hid_cmd_t cmd = {};
    cmd.type = HID_CMD_KEY_RELEASE;
    return q_push(&cmd);
}

bool hid_push_mouse_report(const hid_mouse_report_t *report) {
    hid_cmd_t cmd = {};
    cmd.type  = HID_CMD_MOUSE_REPORT;
    cmd.mouse = *report;
    return q_push(&cmd);
}

bool hid_push_abs_report(const hid_abs_report_t *report) {
    hid_cmd_t cmd = {};
    cmd.type = HID_CMD_ABS_REPORT;
    cmd.abs  = *report;
    return q_push(&cmd);
}


bool hid_push_type_string(const char *text, uint8_t len, uint8_t delay_ms) {
    hid_cmd_t cmd = {};
    cmd.type = HID_CMD_TYPE_STRING;
    // len is uint8_t (0..255) and cmd.str.text is 256 bytes, so the copy is
    // always within bounds — no clamp needed.
    memcpy(cmd.str.text, text, len);
    cmd.str.len      = len;
    cmd.str.delay_ms = delay_ms ? delay_ms : TYPE_DELAY_MS;
    return q_push(&cmd);
}

bool hid_push_wakeup(void) {
    hid_cmd_t cmd = {};
    cmd.type = HID_CMD_WAKEUP;
    return q_push(&cmd);
}

// ── ASCII → HID map ───────────────────────────────────────────────────────────

bool ascii_to_hid(char c, ascii_hid_t *out) {
    if (c >= 'a' && c <= 'z') { out->modifier = 0;          out->keycode = KEY_A + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { out->modifier = MOD_LSHIFT; out->keycode = KEY_A + (c - 'A'); return true; }
    if (c >= '1' && c <= '9') { out->modifier = 0;          out->keycode = KEY_1 + (c - '1'); return true; }

    struct { char ch; uint8_t mod; uint8_t kc; } table[] = {
        {'0',  0,          KEY_0},          {' ',  0,          KEY_SPACE},
        {'\n', 0,          KEY_ENTER},      {'\t', 0,          KEY_TAB},
        {'-',  0,          KEY_MINUS},      {'=',  0,          KEY_EQUAL},
        {'[',  0,          KEY_LEFTBRACE},  {']',  0,          KEY_RIGHTBRACE},
        {'\\', 0,          KEY_BACKSLASH},  {';',  0,          KEY_SEMICOLON},
        {'\'', 0,          KEY_APOSTROPHE}, {'`',  0,          KEY_GRAVE},
        {',',  0,          KEY_COMMA},      {'.',  0,          KEY_DOT},
        {'/',  0,          KEY_SLASH},
        {'!',  MOD_LSHIFT, KEY_1},          {'@',  MOD_LSHIFT, KEY_2},
        {'#',  MOD_LSHIFT, KEY_3},          {'$',  MOD_LSHIFT, KEY_4},
        {'%',  MOD_LSHIFT, KEY_5},          {'^',  MOD_LSHIFT, KEY_6},
        {'&',  MOD_LSHIFT, KEY_7},          {'*',  MOD_LSHIFT, KEY_8},
        {'(',  MOD_LSHIFT, KEY_9},          {')',  MOD_LSHIFT, KEY_0},
        {'_',  MOD_LSHIFT, KEY_MINUS},      {'+',  MOD_LSHIFT, KEY_EQUAL},
        {'{',  MOD_LSHIFT, KEY_LEFTBRACE},  {'}',  MOD_LSHIFT, KEY_RIGHTBRACE},
        {'|',  MOD_LSHIFT, KEY_BACKSLASH},  {':',  MOD_LSHIFT, KEY_SEMICOLON},
        {'"',  MOD_LSHIFT, KEY_APOSTROPHE}, {'~',  MOD_LSHIFT, KEY_GRAVE},
        {'<',  MOD_LSHIFT, KEY_COMMA},      {'>',  MOD_LSHIFT, KEY_DOT},
        {'?',  MOD_LSHIFT, KEY_SLASH},
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        if (table[i].ch == c) { out->modifier = table[i].mod; out->keycode = table[i].kc; return true; }
    }
    return false;
}

// ── String typer state machine ────────────────────────────────────────────────

static struct {
    bool     active;
    char     text[256];
    uint8_t  len;
    uint8_t  pos;
    uint8_t  delay_ms;
    bool     waiting_release;
    absolute_time_t next_time;
} typer = {};

static bool typer_step(void) {
    if (!typer.active) return false;
    if (!time_reached(typer.next_time)) return false;

    if (typer.waiting_release) {
        hid_keyboard_report_t rel = {};
        tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rel, sizeof(rel));
        typer.waiting_release = false;
        typer.next_time = make_timeout_time_ms(typer.delay_ms);
        typer.pos++;
        if (typer.pos >= typer.len) typer.active = false;
        return true;
    }

    if (typer.pos < typer.len) {
        ascii_hid_t ah;
        if (ascii_to_hid(typer.text[typer.pos], &ah)) {
            hid_keyboard_report_t rep = {};
            rep.modifier   = ah.modifier;
            rep.keycode[0] = ah.keycode;
            tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rep, sizeof(rep));
            typer.waiting_release = true;
            typer.next_time = make_timeout_time_ms(typer.delay_ms / 2 + 1);
        } else {
            typer.pos++;
            if (typer.pos >= typer.len) typer.active = false;
        }
        return true;
    }

    typer.active = false;
    return false;
}

// ── Core 0 HID task ───────────────────────────────────────────────────────────

void hid_init(void) {
    int lock_num = spin_lock_claim_unused(true);
    q_lock = spin_lock_instance(lock_num);
}

static bool send_pending = false;
static bool pending_auto_release = false;

void hid_task(void) {
    tud_task();

    // ── Handle wakeup command ─────────────────────────────────────────────────
    // If suspended and there's a WAKEUP command at the head of the queue,
    // assert the Remote Wakeup signal.  We then return — the host will call
    // tud_resume_cb() and normal report sending resumes from the next call.
    if (_suspended) {
        // Scan the whole queue for a pending wake command (not just the head),
        // so a wake request is honoured even if other commands are queued
        // ahead of it. Consume everything up to and including the wake.
        bool wake_found = false;
        hid_cmd_t peek;
        while (q_peek(&peek)) {
            if (peek.type == HID_CMD_WAKEUP) {
                q_pop();
                wake_found = true;
                break;
            }
            // Drop non-wake commands queued while suspended (they can't be
            // delivered to a sleeping host anyway).
            q_pop();
        }
        if (wake_found) {
            _diag_wake_cmds++;
            if (_remote_wakeup_en) {
                printf("[usb] Asserting Remote Wakeup\n");
                bool rc = tud_remote_wakeup();
                _diag_last_wake_rc = rc ? 1 : 0;
                _wake_requested_while_blocked = false;
            } else {
                printf("[usb] Remote Wakeup NOT enabled by host\n");
                _diag_last_wake_rc = -2;   // -2 = permission not granted
                _wake_requested_while_blocked = true;
            }
        }
        return;   // don't try to send reports while suspended
    }

    if (send_pending)                       return;
    if (!tud_mounted())                     return;
    if (!tud_hid_n_ready(0))               return;

    // Let the typer advance
    if (typer.active) {
        if (typer_step()) { send_pending = true; return; }
    }

    // A key report with auto_release sets this flag; the next time the
    // endpoint is ready we send the release directly (never via the queue,
    // so it can't be dropped if the queue is full).
    if (pending_auto_release) {
        pending_auto_release = false;
        hid_keyboard_report_t rel = {};
        tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rel, sizeof(rel));
        send_pending = true;
        return;
    }

    // Drain one command
    hid_cmd_t cmd;
    if (!q_peek(&cmd)) return;
    q_pop();

    switch (cmd.type) {
    case HID_CMD_KEY_REPORT:
        tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &cmd.key, sizeof(cmd.key));
        send_pending = true;
        if (cmd.auto_release) pending_auto_release = true;
        break;

    case HID_CMD_KEY_RELEASE: {
        hid_keyboard_report_t rel = {};
        tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rel, sizeof(rel));
        send_pending = true;
        break;
    }

    case HID_CMD_MOUSE_REPORT:
        tud_hid_n_report(0, HID_REPORT_ID_MOUSE, &cmd.mouse, sizeof(cmd.mouse));
        send_pending = true;
        break;

    case HID_CMD_ABS_REPORT:
        tud_hid_n_report(0, HID_REPORT_ID_ABSMOUSE, &cmd.abs, sizeof(cmd.abs));
        send_pending = true;
        break;

    case HID_CMD_TYPE_STRING:
        typer.active          = true;
        typer.len             = cmd.str.len;
        typer.pos             = 0;
        typer.delay_ms        = cmd.str.delay_ms;
        typer.waiting_release = false;
        typer.next_time       = get_absolute_time();
        memcpy(typer.text, cmd.str.text, cmd.str.len);
        if (typer_step()) send_pending = true;
        break;

    case HID_CMD_WAKEUP:
        // Already handled in the suspended path above; if we get here
        // the host is awake — nothing to do.
        break;
    }
}

// ── TinyUSB callbacks ─────────────────────────────────────────────────────────

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report,
                                 uint16_t len) {
    (void)instance; (void)report; (void)len;
    send_pending = false;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    _suspended         = true;
    _remote_wakeup_en  = remote_wakeup_en;
    _diag_suspend_count++;
    _diag_last_wakeup_en = remote_wakeup_en ? 1 : 0;
    printf("[usb] Suspended (remote wakeup %s)\n",
           remote_wakeup_en ? "enabled" : "disabled");
}

void tud_resume_cb(void) {
    _suspended = false;
    printf("[usb] Resumed\n");
}

// Format the wake diagnostics into a caller-supplied buffer.
void hid_wake_diag(char *buf, size_t buflen) {
    snprintf(buf, buflen,
        "WAKE DIAG: suspends=%u wakeups_en=%d wake_cmds=%u last_rc=%d",
        (unsigned)_diag_suspend_count,
        _diag_last_wakeup_en,
        (unsigned)_diag_wake_cmds,
        _diag_last_wake_rc);
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)bufsize;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)reqlen;
    return 0;
}
