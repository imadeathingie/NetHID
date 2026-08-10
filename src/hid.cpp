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
#if ENABLE_KEYBOARD
#include "kb/keystate.h"
#endif
#include <stdio.h>

/* Log every absolute report to the serial console. Build with
 * -DHID_DEBUG_ABS=1 when the pointer is not moving and you need to know
 * whether the reports are leaving the device at all. */
#ifndef HID_DEBUG_ABS
#define HID_DEBUG_ABS 0
#endif
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

void hid_push_abs_pointer(uint16_t x, uint16_t y, uint8_t buttons, int8_t wheel) {
    if (x > HID_ABS_MAX) x = HID_ABS_MAX;
    if (y > HID_ABS_MAX) y = HID_ABS_MAX;

#if ABS_MOUSE_MODE == 1
    // Position on the digitizer, buttons and wheel on the relative mouse with a
    // zero delta. Each report does the thing it is reliably good at: the
    // digitizer is the only one a host will treat as absolute, and the mouse is
    // the only one whose buttons behave the same everywhere.
    hid_abs_report_t rep = {
        .flags = (uint8_t)(HID_ABS_IN_RANGE | (buttons ? HID_ABS_TIP : 0)),
        .x = x, .y = y,
    };
    hid_push_abs_report(&rep);

    if (buttons || wheel) {
        hid_mouse_report_t m = {};
        m.buttons = buttons;
        m.wheel   = wheel;
        hid_push_mouse_report(&m);          // click, no movement
    }
    if (buttons) {
        hid_abs_report_t up = rep;
        up.flags = HID_ABS_IN_RANGE;        // tip lifted, still tracking
        hid_push_abs_report(&up);
        hid_mouse_report_t m = {};
        hid_push_mouse_report(&m);          // buttons released
    }
#else
    hid_abs_report_t rep = { .buttons = buttons, .x = x, .y = y, .wheel = wheel };
    hid_push_abs_report(&rep);
    if (buttons) {
        // The release repeats the position but NOT the wheel: the wheel is a
        // relative delta, so sending it twice scrolls twice for one gesture.
        // X/Y are absolute and re-sending them is a no-op.
        hid_abs_report_t rel = rep;
        rel.buttons = 0;
        rel.wheel   = 0;
        hid_push_abs_report(&rel);
    }
#endif
}


/* Strings pushed but not yet dequeued by core 1. hid_typer_busy() has to count
 * these: a caller that pushes and then waits for the typer would otherwise see
 * "idle" for the whole window between the push and core 1 picking it up, decide
 * the line was already done, and queue the next one straight away. */
static volatile int typer_queued = 0;

bool hid_push_type_string(const char *text, uint8_t len, uint8_t delay_ms) {
    hid_cmd_t cmd = {};
    cmd.type = HID_CMD_TYPE_STRING;
    // len is uint8_t (0..255) and cmd.str.text is 256 bytes, so the copy is
    // always within bounds — no clamp needed.
    memcpy(cmd.str.text, text, len);
    cmd.str.len      = len;
    cmd.str.delay_ms = delay_ms ? delay_ms : TYPE_DELAY_MS;
    if (!q_push(&cmd)) return false;
    typer_queued++;
    return true;
}

bool hid_push_consumer(uint16_t usage) {
    hid_cmd_t cmd = {};
    cmd.type = HID_CMD_CONSUMER;
    cmd.consumer = usage;
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

/* Mirrors typer.active for cross-core reads. hid_typer_busy() is called from
 * core 0 while the typer runs on core 1, and a plain bool read inside a wait
 * loop is fair game for the compiler to hoist into a register — a spin that
 * never observes the write. */
static volatile bool typer_busy_flag = false;

bool hid_typer_busy(void) { return typer_busy_flag || typer_queued > 0; }

/*
 * Advance the typer by one report.
 *
 * The critical rule here is that state advances ONLY when the report was
 * actually accepted. tud_hid_n_report() returns false when the endpoint is not
 * ready to take another report, and the earlier version ignored that: it
 * incremented pos and flipped waiting_release regardless, so a rejected report
 * silently vanished. Two visible symptoms, both intermittent, both matching a
 * roughly one-in-three failure rate on a busy boot:
 *
 *   - a dropped PRESS loses that character outright;
 *   - a dropped RELEASE leaves the host believing the previous key is still
 *     held. If that key carried shift, everything typed afterwards — by this
 *     firmware or by the person at the keyboard — appears shifted until some
 *     other report clears it. "Incorrect shifting" is this, not a bad mapping.
 *
 * tud_hid_n_ready() is checked by the caller, but readiness can lapse between
 * that check and this send, so the return value is the only thing that actually
 * proves the report went out.
 */
static bool typer_step(void) {
    if (!typer.active) return false;
    if (!time_reached(typer.next_time)) return false;

    if (typer.waiting_release) {
        hid_keyboard_report_t rel = {};
        if (!tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rel, sizeof(rel)))
            return false;      // endpoint busy: retry this same release
        typer.waiting_release = false;
        typer.next_time = make_timeout_time_ms(typer.delay_ms);
        typer.pos++;
        if (typer.pos >= typer.len) { typer.active = false; typer_busy_flag = false; }
        return true;
    }

    if (typer.pos < typer.len) {
        ascii_hid_t ah;
        if (ascii_to_hid(typer.text[typer.pos], &ah)) {
            hid_keyboard_report_t rep = {};
            rep.modifier   = ah.modifier;
            rep.keycode[0] = ah.keycode;
            if (!tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rep, sizeof(rep)))
                return false;  // endpoint busy: retry this same character
            typer.waiting_release = true;
            typer.next_time = make_timeout_time_ms(typer.delay_ms / 2 + 1);
        } else {
            // Unmappable byte. Nothing was sent, so nothing to confirm.
            typer.pos++;
            if (typer.pos >= typer.len) { typer.active = false; typer_busy_flag = false; }
        }
        return true;
    }

    typer.active = false;
    typer_busy_flag = false;
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

    // Endpoint readiness is PER HID INTERFACE, and in ABS_MOUSE_MODE 2 the
    // absolute pointer has one of its own. Gating everything on instance 0 made
    // an absolute report wait on an endpoint it does not use — normally just
    // added latency, but a host that stopped polling interface 0 would stall
    // the absolute pointer indefinitely with a perfectly healthy endpoint 2.
    //
    // Instance 0 ready keeps the old path exactly; only when it is NOT ready do
    // we look at whether the one thing waiting is an absolute report.
    if (!tud_hid_n_ready(0)) {
        hid_cmd_t head;
        if (HID_ABS_INSTANCE == 0 || typer.active || pending_auto_release ||
            !q_peek(&head) || head.type != HID_CMD_ABS_REPORT ||
            !tud_hid_n_ready(HID_ABS_INSTANCE))
            return;
    }

    // Let the typer advance.
    //
    // Return UNCONDITIONALLY while a string is in flight, whatever typer_step()
    // reports. Falling through to the queue drain below is how two strings end
    // up interleaved character by character: typer_step() returns false for
    // every ordinary inter-character gap ("not time yet"), and the drain would
    // then dequeue the next TYPE_STRING and overwrite typer.text/len/pos
    // mid-word. The output looks like fragments of one message spliced into
    // another, with the tails piling up at the end.
    //
    // That was always latent; making typer_step() also return false on a
    // rejected report widened the window enormously, which is why it went from
    // occasional to constant.
    if (typer.active) {
        if (typer_step()) send_pending = true;
        return;
    }

#if ENABLE_KEYBOARD
    // ── Merged keyboard state ────────────────────────────────────────────────
    // The matrix and the network each own a held-key bitmap; keystate_compose()
    // ORs them into one report. This runs ahead of the command queue so a
    // physically held key is never starved by network traffic.
    //
    // The string typer is skipped while it runs: it drives the endpoint
    // directly and would fight the composer over the same report.
    if (!typer.active && keystate_dirty()) {
        hid_keyboard_report_t merged;
        if (keystate_compose(&merged)) {
            if (!tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &merged, sizeof(merged))) {
                // keystate_compose() has already marked itself clean and
                // recorded `merged` as last-sent, so a silent failure here would
                // wedge the keyboard until the next state change. Put the state
                // back so the next pass recomposes and retries.
                keystate_mark_dirty();
                return;
            }
            send_pending = true;
            return;
        }
    }
#endif

    // ── Auto-release ─────────────────────────────────────────────────────────
    //
    // AFTER the compose, and only once the press has actually been sent.
    //
    // This used to run before it, which meant a web-UI keypress was set into the
    // network source by the queue drain and cleared again on the very next pass
    // — before anything had composed it — so the host never saw the key at all.
    // Physical keys were unaffected because they never set this flag, which is
    // why the matrix kept working while the on-screen keyboard did nothing.
    //
    // keystate_dirty() going false is the signal that the composed report
    // carrying the press has gone to the endpoint.
#if ENABLE_KEYBOARD
    // keystate_dirty() going false is the signal that the composed report
    // carrying the press has gone to the endpoint.
    if (pending_auto_release && !keystate_dirty()) {
        pending_auto_release = false;
        keystate_clear(KB_SRC_NET);   // composed and sent on the next pass
    }
#else
    // Without the keyboard feature there is no composer: the press was sent
    // directly by the queue drain, so the release can follow immediately.
    if (pending_auto_release) {
        hid_keyboard_report_t rel = {};
        if (!tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rel, sizeof(rel)))
            return;
        pending_auto_release = false;
        send_pending = true;
        return;
    }
#endif

    // Drain one command
    hid_cmd_t cmd;
    if (!q_peek(&cmd)) return;

    /*
     * NOT popped yet.
     *
     * tud_hid_n_report() returns false when the endpoint cannot take another
     * report, and popping first meant a rejected report was simply lost —
     * tud_hid_n_ready() was checked above, but readiness can lapse between that
     * check and the send. The same defect as the string typer had, in the other
     * half of this file: a mouse click or an absolute move would vanish
     * intermittently with nothing to show for it.
     *
     * Each case below pops only once its send is confirmed, or returns to retry
     * the same command on the next pass.
     */

    switch (cmd.type) {
    case HID_CMD_KEY_REPORT:
        q_pop();   /* cannot fail; consumed unconditionally */
#if ENABLE_KEYBOARD
        // Network keys become state rather than an immediate report, so they
        // merge with whatever the matrix is holding instead of clobbering it.
        keystate_set_report(KB_SRC_NET, cmd.key.modifier, cmd.key.keycode);
        if (cmd.auto_release) pending_auto_release = true;
#else
        tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &cmd.key, sizeof(cmd.key));
        send_pending = true;
        if (cmd.auto_release) pending_auto_release = true;
#endif
        break;

    case HID_CMD_KEY_RELEASE: {
        q_pop();   /* cannot fail; consumed unconditionally */
#if ENABLE_KEYBOARD
        keystate_clear(KB_SRC_NET);
#else
        hid_keyboard_report_t rel = {};
        tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &rel, sizeof(rel));
        send_pending = true;
#endif
        break;
    }

    case HID_CMD_MOUSE_REPORT:
        if (!tud_hid_n_report(0, HID_REPORT_ID_MOUSE, &cmd.mouse, sizeof(cmd.mouse))) return;   /* endpoint busy: retry this command */
        q_pop();
        send_pending = true;
        break;

    case HID_CMD_ABS_REPORT:
        /* HID_ABS_INSTANCE, not 0: in ABS_MOUSE_MODE 2 the absolute pointer is
         * a separate HID interface with its own endpoint, and sending it to
         * instance 0 would put an absolute report down the relative mouse's
         * pipe — accepted by TinyUSB, decoded as nonsense by the host. */
        if (!tud_hid_n_report(HID_ABS_INSTANCE, HID_REPORT_ID_ABSMOUSE, &cmd.abs, sizeof(cmd.abs))) return;   /* endpoint busy: retry this command */
#if HID_DEBUG_ABS
        /* The absolute pointer is the one report whose failures are invisible:
         * the host may enumerate it, accept every report, and simply not act on
         * them. This says what actually went out, so "is it being sent?" and
         * "is the host ignoring it?" can be told apart. */
#if ABS_MOUSE_MODE == 1
        printf("[abs] sent flags=%02X (tip=%d range=%d) x=%u y=%u (%u bytes)\n",
               cmd.abs.flags, !!(cmd.abs.flags & HID_ABS_TIP),
               !!(cmd.abs.flags & HID_ABS_IN_RANGE),
               cmd.abs.x, cmd.abs.y, (unsigned)sizeof(cmd.abs));
#else
        printf("[abs] sent buttons=%02X x=%u y=%u wheel=%d (%u bytes)\n",
               cmd.abs.buttons, cmd.abs.x, cmd.abs.y, cmd.abs.wheel,
               (unsigned)sizeof(cmd.abs));
#endif
#endif
        q_pop();
        send_pending = true;
        break;

    case HID_CMD_CONSUMER: {
        uint16_t u = cmd.consumer;
        if (!tud_hid_n_report(0, HID_REPORT_ID_CONSUMER, &u, sizeof(u))) return;
        q_pop();
        send_pending = true;
        break;
    }

    case HID_CMD_TYPE_STRING:
        q_pop();   /* cannot fail; consumed unconditionally */
        typer.active          = true;
        typer_busy_flag       = true;
        if (typer_queued > 0) typer_queued--;
        typer.len             = cmd.str.len;
        typer.pos             = 0;
        typer.delay_ms        = cmd.str.delay_ms;
        typer.waiting_release = false;
        typer.next_time       = get_absolute_time();
        memcpy(typer.text, cmd.str.text, cmd.str.len);
        if (typer_step()) send_pending = true;
        break;

    case HID_CMD_WAKEUP:
        q_pop();   /* cannot fail; consumed unconditionally */
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
