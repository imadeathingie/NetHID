#!/usr/bin/env python3
"""
nethid-capture.py — capture this Mac's keyboard + mouse and forward them to a
NetHID device, turning your Mac into a software KVM head. Built on `pynput` for
input capture and the bundled `client.py` for the wire protocol (the persistent
binary socket, so latency stays low; optionally TLS).

WHAT IT DOES
  • Mirrors your real key state: key-down adds the key to a HID report, key-up
    removes it — so holds, modifiers, key-repeat and chords all pass through,
    not just taps.
  • Absolute mouse by default: your cursor's position as a fraction (0..1) of the
    Mac screen maps straight to the target, so screen edges line up and there's no
    relative drift. Buttons (incl. drags) and scroll are forwarded too. Use
    `--mouse relative` for delta-based motion (e.g. FPS games).

SAFETY MODEL (read this)
  • Forwarding starts OFF. Press the TOGGLE key (default F12) to start/stop.
  • Default mode is MIRROR: events go to BOTH your Mac and the device. You keep
    full local control; Ctrl-C in this terminal quits.
  • --grab adds true KVM behaviour: while forwarding, local key/mouse events are
    SUPPRESSED (they only reach the device). This can lock you out if misused, so:
      – it starts OFF (toggle on deliberately),
      – the TOGGLE and QUIT keys are never suppressed — press TOGGLE (F12) to
        regain local control, or QUIT (default F10) to exit from anywhere,
      – use F-keys for --toggle/--quit-key in grab mode (they pass through
        reliably). grab is the less battle-tested path; expect to tune it.
  • On toggle-off and on quit, the tool releases all keys/buttons on the device
    so nothing sticks on the target.

macOS PERMISSIONS (required for global capture)
  System Settings → Privacy & Security →
    • Accessibility      → enable your terminal app (Terminal/iTerm) or the
                           Python you run this with.
    • Input Monitoring   → same. (Needed to see events; and to suppress in --grab.)
  You may need to quit & reopen the terminal after granting. If the tool runs but
  sees no input, that's almost always a missing permission.

INSTALL
  python3 -m pip install pynput
  (Run from the same directory as client.py, or put both on PYTHONPATH.)

EXAMPLES
  # Mirror mode, plain socket :9000, password from prompt/env:
  ./nethid-capture.py 192.168.8.205

  # Encrypted, control socket TLS on :9443, verify by cert FQDN:
  ./nethid-capture.py nethid.example.com --tls

  # True KVM (local suppressed while forwarding), Cmd acts as Ctrl for a PC target:
  ./nethid-capture.py 192.168.8.205 --grab --cmd-is-ctrl
"""

import argparse
import inspect
import os
import sys
import threading
import time
import getpass

try:
    from pynput import keyboard, mouse
except ImportError:
    sys.exit("pynput is required:  python3 -m pip install pynput")

try:
    from client import NetHIDClient, KEY_CODES
except ImportError:
    sys.exit("client.py not found next to this script (need NetHIDClient).")


# ── macOS virtual keycode (CGKeyCode) → USB HID usage id ─────────────────────
# Physical-key mapping: layout/shift independent. Modifiers are handled
# separately (they set the modifier bitmask, not the key array).
MAC_VK_HID = {
    0x00: 0x04, 0x0B: 0x05, 0x08: 0x06, 0x02: 0x07, 0x0E: 0x08, 0x03: 0x09,  # a b c d e f
    0x05: 0x0A, 0x04: 0x0B, 0x22: 0x0C, 0x26: 0x0D, 0x28: 0x0E, 0x25: 0x0F,  # g h i j k l
    0x2E: 0x10, 0x2D: 0x11, 0x1F: 0x12, 0x23: 0x13, 0x0C: 0x14, 0x0F: 0x15,  # m n o p q r
    0x01: 0x16, 0x11: 0x17, 0x20: 0x18, 0x09: 0x19, 0x0D: 0x1A, 0x07: 0x1B,  # s t u v w x
    0x10: 0x1C, 0x06: 0x1D,                                                  # y z
    0x12: 0x1E, 0x13: 0x1F, 0x14: 0x20, 0x15: 0x21, 0x17: 0x22, 0x16: 0x23,  # 1 2 3 4 5 6
    0x1A: 0x24, 0x1C: 0x25, 0x19: 0x26, 0x1D: 0x27,                          # 7 8 9 0
    0x24: 0x28, 0x35: 0x29, 0x33: 0x2A, 0x30: 0x2B, 0x31: 0x2C,              # ret esc bksp tab space
    0x1B: 0x2D, 0x18: 0x2E, 0x21: 0x2F, 0x1E: 0x30, 0x2A: 0x31,              # - = [ ] backslash
    0x29: 0x33, 0x27: 0x34, 0x32: 0x35, 0x2B: 0x36, 0x2F: 0x37, 0x2C: 0x38,  # ; ' ` , . /
    0x39: 0x39,                                                              # caps lock
    0x7A: 0x3A, 0x78: 0x3B, 0x63: 0x3C, 0x76: 0x3D, 0x60: 0x3E, 0x61: 0x3F,  # F1-F6
    0x62: 0x40, 0x64: 0x41, 0x65: 0x42, 0x6D: 0x43, 0x67: 0x44, 0x6F: 0x45,  # F7-F12
    0x72: 0x49, 0x73: 0x4A, 0x74: 0x4B, 0x75: 0x4C, 0x77: 0x4D, 0x79: 0x4E,  # ins home pgup del end pgdn
    0x7C: 0x4F, 0x7B: 0x50, 0x7D: 0x51, 0x7E: 0x52,                          # right left down up
    # keypad
    0x47: 0x53, 0x4B: 0x54, 0x43: 0x55, 0x4E: 0x56, 0x45: 0x57, 0x4C: 0x58,
    0x53: 0x59, 0x54: 0x5A, 0x55: 0x5B, 0x56: 0x5C, 0x57: 0x5D, 0x58: 0x5E,
    0x59: 0x5F, 0x5B: 0x60, 0x5C: 0x61, 0x52: 0x62, 0x41: 0x63, 0x51: 0x67,
}

# pynput Key enum (special keys delivered as Key, not KeyCode) → HID usage.
_ENUM_HID_NAMES = {
    'enter': 0x28, 'esc': 0x29, 'backspace': 0x2A, 'tab': 0x2B, 'space': 0x2C,
    'caps_lock': 0x39,
    'f1': 0x3A, 'f2': 0x3B, 'f3': 0x3C, 'f4': 0x3D, 'f5': 0x3E, 'f6': 0x3F,
    'f7': 0x40, 'f8': 0x41, 'f9': 0x42, 'f10': 0x43, 'f11': 0x44, 'f12': 0x45,
    'f13': 0x68, 'f14': 0x69, 'f15': 0x6A, 'f16': 0x6B, 'f17': 0x6C,
    'f18': 0x6D, 'f19': 0x6E, 'f20': 0x6F,
    'print_screen': 0x46, 'scroll_lock': 0x47, 'pause': 0x48, 'insert': 0x49,
    'home': 0x4A, 'page_up': 0x4B, 'delete': 0x4C, 'end': 0x4D, 'page_down': 0x4E,
    'right': 0x4F, 'left': 0x50, 'down': 0x51, 'up': 0x52, 'menu': 0x65,
}
KEY_ENUM_HID = {getattr(keyboard.Key, n): v
                for n, v in _ENUM_HID_NAMES.items() if hasattr(keyboard.Key, n)}

# Modifier Key enum → bitmask bit (cmd may be remapped to ctrl via --cmd-is-ctrl).
_MOD_NAMES = {
    'shift': 0x02, 'shift_l': 0x02, 'shift_r': 0x02,
    'ctrl': 0x01, 'ctrl_l': 0x01, 'ctrl_r': 0x01,
    'alt': 0x04, 'alt_l': 0x04, 'alt_r': 0x04, 'alt_gr': 0x04,
    'cmd': 0x08, 'cmd_l': 0x08, 'cmd_r': 0x08,
}
BUTTON_BIT = {mouse.Button.left: 1, mouse.Button.right: 2, mouse.Button.middle: 4}


def build_mod_keys(cmd_is_ctrl: bool):
    bits = dict(_MOD_NAMES)
    if cmd_is_ctrl:
        for k in ('cmd', 'cmd_l', 'cmd_r'):
            bits[k] = 0x01
    return {getattr(keyboard.Key, n): v
            for n, v in bits.items() if hasattr(keyboard.Key, n)}


def key_to_name(spec: str):
    """Parse a --toggle/--quit-key spec ('f12', 'esc', or a single char') into a
    matcher predicate and (if known) its macOS vk for grab pass-through."""
    enum = getattr(keyboard.Key, spec, None)
    if enum is not None:
        vk = getattr(getattr(enum, 'value', None), 'vk', None)
        return (lambda k: k == enum), vk
    ch = spec
    return (lambda k: isinstance(k, keyboard.KeyCode) and k.char == ch), None


def get_main_screen_size():
    """(width, height) of the main display in the same units pynput reports
    cursor positions (points, top-left origin), or None if it can't be read."""
    try:
        from Quartz import CGMainDisplayID, CGDisplayBounds
        b = CGDisplayBounds(CGMainDisplayID())
        return float(b.size.width), float(b.size.height)
    except Exception:
        return None


class Capture:
    def __init__(self, client, args):
        self.c = client
        self.args = args
        self.mod_keys = build_mod_keys(args.cmd_is_ctrl)
        self.lock = threading.Lock()       # guards held-state AND socket writes
        self.forwarding = False
        self.running = True

        # keyboard held state
        self.down_mods = {}                # key -> bit  (currently-held modifiers)
        self.down_keys = []                # ordered HID usages currently held (<=6)

        # mouse accumulators
        self.buttons = 0
        self.acc_dx = self.acc_dy = self.acc_wheel = 0
        self.btn_dirty = False
        self.last_pos = None
        # absolute-mode mouse state
        self.mouse_mode = getattr(args, 'mouse', 'abs')
        self.screen_w, self.screen_h = (getattr(args, 'screen_size', None) or (None, None))
        self.flip_y = getattr(args, 'flip_y', False)
        self.cur_x = self.cur_y = None
        self.pos_dirty = False

        self.match_toggle, self.toggle_vk = key_to_name(args.toggle)
        self.match_quit, self.quit_vk = key_to_name(args.quit_key)
        self.reserved_vks = {v for v in (self.toggle_vk, self.quit_vk) if v is not None}

    # ── HID resolution ───────────────────────────────────────────────────────
    def resolve(self, key):
        """Return ('mod', bit) | ('key', hid) | (None, None)."""
        if key in self.mod_keys:
            return ('mod', self.mod_keys[key])
        if isinstance(key, keyboard.Key):
            hid = KEY_ENUM_HID.get(key)
            if hid:
                return ('key', hid)
            vk = getattr(getattr(key, 'value', None), 'vk', None)
            if vk in MAC_VK_HID:
                return ('key', MAC_VK_HID[vk])
            return (None, None)
        vk = getattr(key, 'vk', None)
        if vk in MAC_VK_HID:
            return ('key', MAC_VK_HID[vk])
        ch = getattr(key, 'char', None)
        if ch and KEY_CODES.get(ch.lower()):
            return ('key', KEY_CODES[ch.lower()])
        return (None, None)

    def _mod_mask(self):
        m = 0
        for bit in self.down_mods.values():
            m |= bit
        return m

    def _send_keys(self):
        # caller holds self.lock
        self.c.key_down(self._mod_mask(), list(self.down_keys))

    # ── keyboard callbacks ─────────────────────────────────────────────────────
    def on_press(self, key):
        if self.match_quit(key):
            self.quit()
            return False
        if self.match_toggle(key):
            self.set_forwarding(not self.forwarding)
            return
        if not self.forwarding:
            return
        kind, val = self.resolve(key)
        if kind is None:
            return
        with self.lock:
            if kind == 'mod':
                if key not in self.down_mods:
                    self.down_mods[key] = val
            elif val not in self.down_keys:
                if len(self.down_keys) < 6:
                    self.down_keys.append(val)
                else:
                    return  # 6-key rollover full; ignore extra
            self._send_keys()

    def on_release(self, key):
        if self.match_toggle(key) or self.match_quit(key):
            return
        if not self.forwarding:
            return
        kind, val = self.resolve(key)
        if kind is None:
            return
        with self.lock:
            if kind == 'mod':
                self.down_mods.pop(key, None)
            elif val in self.down_keys:
                self.down_keys.remove(val)
            self._send_keys()

    # ── mouse callbacks ─────────────────────────────────────────────────────────
    def on_move(self, x, y):
        if not self.forwarding:
            self.last_pos = (x, y)
            self.cur_x, self.cur_y = x, y
            return
        with self.lock:
            if self.mouse_mode == 'abs':
                self.cur_x, self.cur_y = x, y
                self.pos_dirty = True
            else:
                if self.last_pos is not None:
                    self.acc_dx += int(round((x - self.last_pos[0]) * self.args.mouse_scale))
                    self.acc_dy += int(round((y - self.last_pos[1]) * self.args.mouse_scale))
                self.last_pos = (x, y)

    def on_click(self, x, y, button, pressed):
        if not self.forwarding:
            return
        bit = BUTTON_BIT.get(button, 0)
        if not bit:
            return
        with self.lock:
            if self.mouse_mode == 'abs':       # anchor the click at its position
                self.cur_x, self.cur_y = x, y
                self.pos_dirty = True
            if pressed:
                self.buttons |= bit
            else:
                self.buttons &= ~bit
            self.btn_dirty = True

    def on_scroll(self, x, y, dx, dy):
        if not self.forwarding:
            return
        with self.lock:
            self.acc_wheel += int(round(dy))   # +ve = up, matches device wheel

    # ── mouse flusher (coalesces to ~125 Hz) ────────────────────────────────────
    def flusher(self):
        while self.running:
            time.sleep(1 / 125)
            if not self.forwarding:
                continue
            with self.lock:
                b = self.buttons
                if self.mouse_mode == 'abs':
                    send_pos = self.pos_dirty or self.btn_dirty
                    wheel = self.acc_wheel
                    cx, cy = self.cur_x, self.cur_y
                    self.pos_dirty = False; self.btn_dirty = False; self.acc_wheel = 0
                    if send_pos and cx is not None and self.screen_w:
                        fx = min(1.0, max(0.0, cx / self.screen_w))
                        fy = min(1.0, max(0.0, cy / self.screen_h))
                        if self.flip_y:
                            fy = 1.0 - fy
                        self.c.mouse_move_abs(fx, fy, buttons=b)   # 0.0..1.0 fraction
                    if wheel:
                        self._emit_wheel(b, wheel)
                else:
                    dx, dy, wheel = self.acc_dx, self.acc_dy, self.acc_wheel
                    dirty = self.btn_dirty
                    self.acc_dx = self.acc_dy = self.acc_wheel = 0
                    self.btn_dirty = False
                    if dx or dy or wheel or dirty:
                        self._emit_mouse(b, dx, dy, wheel)

    def _emit_wheel(self, buttons, wheel):
        # caller holds self.lock; wheel rides the relative report (abs report has none)
        while wheel:
            sw = max(-127, min(127, wheel))
            self.c._send_binary_mouse(buttons, 0, 0, sw)
            wheel -= sw

    def _emit_mouse(self, buttons, dx, dy, wheel):
        # caller holds self.lock; chunk to the ±127 wire range
        sent = False
        while dx or dy or wheel:
            sx = max(-127, min(127, dx)); sy = max(-127, min(127, dy))
            sw = max(-127, min(127, wheel))
            self.c._send_binary_mouse(buttons, sx, sy, sw)
            dx -= sx; dy -= sy; wheel -= sw
            sent = True
        if not sent:
            self.c._send_binary_mouse(buttons, 0, 0, 0)

    # ── state transitions ────────────────────────────────────────────────────
    def set_forwarding(self, on):
        with self.lock:
            if on == self.forwarding:
                return
            self.forwarding = on
            # reset state on every transition so nothing sticks / no jump
            self.down_mods.clear(); self.down_keys.clear()
            self.buttons = 0; self.acc_dx = self.acc_dy = self.acc_wheel = 0
            self.btn_dirty = False; self.last_pos = None; self.pos_dirty = False
            if on and self.mouse_mode == 'abs':
                # seed from the live cursor so the remote snaps to where we are
                try:
                    self.cur_x, self.cur_y = mouse.Controller().position
                    self.pos_dirty = True
                except Exception:
                    pass
            if not on:
                self.c.key_down(0, [])             # release all keys
                self.c._send_binary_mouse(0, 0, 0, 0)  # release all buttons
        print(f"[capture] forwarding {'ON' if on else 'OFF'}"
              f"{'  (local input SUPPRESSED)' if (on and self.args.grab) else ''}",
              flush=True)

    def quit(self):
        self.running = False
        with self.lock:
            try:
                self.c.key_down(0, [])
                self.c._send_binary_mouse(0, 0, 0, 0)
            except Exception:
                pass
        print("[capture] quitting.", flush=True)

    # ── darwin_intercept hooks (only used with --grab) ───────────────────────
    def kbd_intercept(self, event_type, event):
        try:
            import Quartz
            kc = Quartz.CGEventGetIntegerValueField(event, Quartz.kCGKeyboardEventKeycode)
        except Exception:
            kc = None
        if kc is not None and kc in self.reserved_vks:
            return event                       # never trap toggle/quit
        if self.forwarding:
            return None                        # suppress local while forwarding
        return event

    def mouse_intercept(self, event_type, event):
        # absolute mode needs the live cursor position, so never suppress the mouse
        if self.mouse_mode == 'abs':
            return event
        return None if self.forwarding else event


def main():
    ap = argparse.ArgumentParser(description="Capture this Mac's input and forward to NetHID.")
    ap.add_argument('host', help='NetHID device host/IP')
    ap.add_argument('--port', type=int, default=None, help='default 9000, or 9443 with --tls')
    ap.add_argument('--password', default=os.environ.get('NETHID_PASSWORD'),
                    help='device password (or set NETHID_PASSWORD; else prompted)')
    ap.add_argument('--user', default='admin')
    ap.add_argument('--protocol', choices=['binary', 'json'], default='binary')
    ap.add_argument('--tls', action='store_true', help='use the TLS control socket')
    ap.add_argument('--no-verify', action='store_true', help='skip TLS cert verification (IP)')
    ap.add_argument('--cafile', default=None, help='CA bundle for TLS verification')
    ap.add_argument('--server-hostname', default=None, help='name for SNI / cert check')
    ap.add_argument('--toggle', default='f12', help='forwarding on/off key (default f12)')
    ap.add_argument('--quit-key', default='f10', help='quit-from-anywhere key (default f10)')
    ap.add_argument('--grab', action='store_true', help='suppress local input while forwarding (true KVM)')
    ap.add_argument('--cmd-is-ctrl', action='store_true', help='map ⌘ to Ctrl (controlling a PC)')
    ap.add_argument('--mouse', choices=['abs', 'relative'], default='abs',
                    help="'abs' maps cursor position 1:1 over the screen (fixes edge "
                         "drift; default); 'relative' sends deltas")
    ap.add_argument('--screen', default=None,
                    help='override Mac screen size for --mouse abs, e.g. 2560x1440')
    ap.add_argument('--flip-y', action='store_true',
                    help='invert vertical mapping in --mouse abs (if up/down are reversed)')
    ap.add_argument('--mouse-scale', type=float, default=1.0,
                    help='sensitivity multiplier (--mouse relative only)')
    ap.add_argument('--no-keyboard', action='store_true')
    ap.add_argument('--no-mouse', action='store_true')
    args = ap.parse_args()

    if args.port is None:
        args.port = 9443 if args.tls else 9000
    pw = args.password
    if pw is None:
        pw = getpass.getpass(f"Password for {args.user}@{args.host}: ")

    if args.grab and sys.platform != 'darwin':
        sys.exit("--grab (input suppression) is implemented for macOS only.")

    # Resolve the Mac screen size used to map cursor position → 0.0..1.0 fraction.
    args.screen_size = None
    if args.mouse == 'abs' and not args.no_mouse:
        if args.screen:
            try:
                w, h = args.screen.lower().split('x')
                args.screen_size = (float(w), float(h))
            except Exception:
                sys.exit("--screen must look like WIDTHxHEIGHT, e.g. 2560x1440")
        else:
            args.screen_size = get_main_screen_size()
            if not args.screen_size:
                sys.exit("Couldn't detect the Mac screen size — pass --screen "
                         "WIDTHxHEIGHT (e.g. --screen 2560x1440), or use --mouse relative.")

    # Pass only the kwargs this client.py actually accepts — the firmware and
    # docs copies have diverged (e.g. `user` exists only in one), so adapt to
    # whichever is imported rather than assuming a fixed signature.
    supported = set(inspect.signature(NetHIDClient).parameters)
    if args.tls and 'tls' not in supported:
        sys.exit("This client.py has no TLS support (no `tls` argument). Update it "
                 "to the copy with tls=/tls_verify= params, or drop --tls.")
    if args.user != 'admin' and 'user' not in supported:
        print("[capture] note: this client.py has no `user` argument; ignoring --user.",
              flush=True)
    candidate = {
        'port': args.port, 'password': pw, 'protocol': args.protocol,
        'user': args.user, 'tls': args.tls, 'tls_verify': not args.no_verify,
        'cafile': args.cafile, 'server_hostname': args.server_hostname,
    }
    kwargs = {k: v for k, v in candidate.items() if k in supported}
    client = NetHIDClient(args.host, **kwargs)
    print(f"[capture] connecting to {args.host}:{args.port} "
          f"({'TLS' if args.tls else 'plain'}, {args.protocol})…", flush=True)
    client.connect()
    print("[capture] connected.", flush=True)

    cap = Capture(client, args)

    if args.no_mouse:
        mouse_desc = 'off'
    elif args.mouse == 'abs':
        mouse_desc = "absolute (%dx%d)" % (args.screen_size[0], args.screen_size[1])
    else:
        mouse_desc = 'relative'
    print("\n  TOGGLE forwarding:  %s        QUIT: %s   (or Ctrl-C in mirror mode)"
          % (args.toggle.upper(), args.quit_key.upper()))
    print("  mode: %s | keyboard: %s | mouse: %s\n"
          % ('GRAB (local suppressed)' if args.grab else 'mirror',
             'off' if args.no_keyboard else 'on', mouse_desc), flush=True)
    if args.grab and args.mouse == 'abs' and not args.no_mouse:
        print("  note: absolute mouse tracks your live cursor, so the local cursor "
              "still moves;\n        --grab suppresses the keyboard only in this mode.\n",
              flush=True)

    kb_kwargs = dict(on_press=cap.on_press, on_release=cap.on_release)
    ms_kwargs = dict(on_move=cap.on_move, on_click=cap.on_click, on_scroll=cap.on_scroll)
    if args.grab:
        kb_kwargs['darwin_intercept'] = cap.kbd_intercept
        if args.mouse == 'relative':       # abs must not suppress the mouse
            ms_kwargs['darwin_intercept'] = cap.mouse_intercept

    listeners = []
    if not args.no_keyboard:
        listeners.append(keyboard.Listener(**kb_kwargs))
    if not args.no_mouse:
        listeners.append(mouse.Listener(**ms_kwargs))
    if not listeners:
        sys.exit("nothing to capture (both --no-keyboard and --no-mouse).")

    flush_t = threading.Thread(target=cap.flusher, daemon=True)
    flush_t.start()
    for l in listeners:
        l.start()
    try:
        for l in listeners:
            l.join()
    except KeyboardInterrupt:
        cap.quit()
    finally:
        cap.running = False
        for l in listeners:
            l.stop()
        try:
            client.close()
        except Exception:
            pass


if __name__ == '__main__':
    main()
