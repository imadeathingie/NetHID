/* mousekeys.cpp — see include/kb/mousekeys.h */

#include "pico/stdlib.h"
#include "tusb.h"
#include "nethid.h"
#include "kb/mousekeys.h"
#include "kb/features.h"

/* Held-key state. Directions and wheel are bitmasks so opposing keys cancel
 * naturally and diagonals fall out for free. */
enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8 };

static uint8_t  dirs;
static uint8_t  wheels;
static uint8_t  buttons;      /* HID mouse button bitmask, bit0 = left */
static uint8_t  accel;        /* bit0 = ACL0 held, bit1 = ACL1, bit2 = ACL2 */
static bool     buttons_dirty;

static uint32_t move_due;
static uint32_t wheel_due;
static uint32_t dirs_since;   /* when motion started, for the ramp */

void kb_mousekeys_init(void) {
    dirs = wheels = buttons = accel = 0;
    buttons_dirty = false;
}

uint8_t kb_mousekeys_buttons(void) { return buttons; }

static void send(int8_t x, int8_t y, int8_t wheel, int8_t pan) {
    hid_mouse_report_t rep = {};
    /* Every report carries the current buttons: the host holds button state
     * between reports, and a motion report that omitted them would drop a
     * drag mid-gesture. */
    rep.buttons = buttons;
    rep.x = x; rep.y = y; rep.wheel = wheel; rep.pan = pan;
    hid_push_mouse_report(&rep);
}

/* Pixels per step. A held accel key pins the speed; otherwise it ramps from
 * BASE to MAX over TIME_TO_MAX. When several accel keys are held the slowest
 * wins, which is the safe way round — you reach for ACL0 when you are trying
 * to land on something small. */
static int speed_now(uint32_t now) {
    if (accel & 1) return MOUSEKEY_SPEED_SLOW;
    if (accel & 2) return MOUSEKEY_SPEED_MED;
    if (accel & 4) return MOUSEKEY_SPEED_FAST;
    uint32_t t = now - dirs_since;
    if (t >= MOUSEKEY_TIME_TO_MAX) return MOUSEKEY_MAX_SPEED;
    return MOUSEKEY_BASE_SPEED +
           (int)((MOUSEKEY_MAX_SPEED - MOUSEKEY_BASE_SPEED) * t / MOUSEKEY_TIME_TO_MAX);
}

static inline int8_t clamp8(int v) { return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }

static void step_motion(uint32_t now) {
    int s = speed_now(now);
    int x = 0, y = 0;
    if (dirs & D_RIGHT) x += s;
    if (dirs & D_LEFT)  x -= s;
    if (dirs & D_DOWN)  y += s;
    if (dirs & D_UP)    y -= s;
    /* Diagonals would otherwise travel 1.41x faster than the axes. 181/256 is
     * 1/sqrt(2) in integer arithmetic. */
    if (x && y) { x = x * 181 / 256; y = y * 181 / 256; if (!x) x = (dirs & D_RIGHT) ? 1 : -1;
                                                       if (!y) y = (dirs & D_DOWN)  ? 1 : -1; }
    send(clamp8(x), clamp8(y), 0, 0);
}

static void step_wheel(void) {
    int w = 0, p = 0;
    if (wheels & D_UP)    w += MOUSEKEY_WHEEL_DELTA;
    if (wheels & D_DOWN)  w -= MOUSEKEY_WHEEL_DELTA;
    if (wheels & D_RIGHT) p += MOUSEKEY_WHEEL_DELTA;
    if (wheels & D_LEFT)  p -= MOUSEKEY_WHEEL_DELTA;
    send(0, 0, clamp8(w), clamp8(p));
}

bool kb_mousekeys_process(keyrecord_t *rec) {
    kb_keycode_t kc = rec->keycode;
    if (!kc_in(kc, QK_MOUSE, QK_MOUSE_MAX)) return true;

    bool down = rec->event.pressed;
    uint8_t idx = (uint8_t)(kc & 0xFF);
    uint8_t before_dirs = dirs, before_wheels = wheels;

    switch (kc) {
    case MS_UP:    dirs   = down ? (uint8_t)(dirs | D_UP)      : (uint8_t)(dirs & ~D_UP);      break;
    case MS_DOWN:  dirs   = down ? (uint8_t)(dirs | D_DOWN)    : (uint8_t)(dirs & ~D_DOWN);    break;
    case MS_LEFT:  dirs   = down ? (uint8_t)(dirs | D_LEFT)    : (uint8_t)(dirs & ~D_LEFT);    break;
    case MS_RGHT:  dirs   = down ? (uint8_t)(dirs | D_RIGHT)   : (uint8_t)(dirs & ~D_RIGHT);   break;
    case MS_WHLU:  wheels = down ? (uint8_t)(wheels | D_UP)    : (uint8_t)(wheels & ~D_UP);    break;
    case MS_WHLD:  wheels = down ? (uint8_t)(wheels | D_DOWN)  : (uint8_t)(wheels & ~D_DOWN);  break;
    case MS_WHLL:  wheels = down ? (uint8_t)(wheels | D_LEFT)  : (uint8_t)(wheels & ~D_LEFT);  break;
    case MS_WHLR:  wheels = down ? (uint8_t)(wheels | D_RIGHT) : (uint8_t)(wheels & ~D_RIGHT); break;
    case MS_ACL0:  accel  = down ? (uint8_t)(accel | 1) : (uint8_t)(accel & ~1); break;
    case MS_ACL1:  accel  = down ? (uint8_t)(accel | 2) : (uint8_t)(accel & ~2); break;
    case MS_ACL2:  accel  = down ? (uint8_t)(accel | 4) : (uint8_t)(accel & ~4); break;
    default:
        if (idx >= 0x08 && idx <= 0x0C) {
            uint8_t bit = (uint8_t)(1u << (idx - 0x08));
            buttons = down ? (uint8_t)(buttons | bit) : (uint8_t)(buttons & ~bit);
            buttons_dirty = true;      /* sent from the task, with zero motion */
        }
        break;
    }

    uint32_t now = rec->event.time;
    if (!before_dirs && dirs) {
        /* First direction of a gesture: move once immediately so a tap is a
         * single nudge, then wait DELAY before repeating. */
        dirs_since = now;
        step_motion(now);
        move_due = now + MOUSEKEY_DELAY;
    }
    if (!before_wheels && wheels) {
        step_wheel();
        wheel_due = now + MOUSEKEY_WHEEL_DELAY;
    }
    return false;   /* mouse keycodes never reach the default handler */
}

void kb_mousekeys_task(void) {
    if (!dirs && !wheels && !buttons_dirty) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (buttons_dirty) {
        buttons_dirty = false;
        send(0, 0, 0, 0);
    }
    if (dirs && (int32_t)(now - move_due) >= 0) {
        step_motion(now);
        move_due = now + MOUSEKEY_INTERVAL;
    }
    if (wheels && (int32_t)(now - wheel_due) >= 0) {
        step_wheel();
        wheel_due = now + MOUSEKEY_WHEEL_INTERVAL;
    }
}
