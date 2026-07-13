"""
client.py — NetHID Python client (runs on the REMOTE machine, not the Pico)
────────────────────────────────────────────────────────────────────────────
Connects to the Pico W TCP socket server and sends keyboard/mouse events.
Supports both the binary protocol (low-latency) and JSON protocol.

Usage examples:
    from client import NetHIDClient, AuthError

    # With password (recommended)
    c = NetHIDClient("192.168.1.50", password="changeme")
    c.connect()            # connects AND authenticates
    c.type("Hello, World!")
    c.combo(ctrl=True, key='c')
    c.mouse_move(50, -20)
    c.mouse_click('left')
    c.close()

    # Keep the password off the wire — JSON mode uses HMAC challenge-response:
    with NetHIDClient("192.168.1.50", password="changeme",
                      protocol="json", user="admin") as c:
        c.type("Hello!\n")
        c.type("${WIN_PASS}\n")   # types a firmware secret by reference

    # Context manager form:
    with NetHIDClient("192.168.1.50", password="changeme") as c:
        c.type("Hello!\n")

    # No password (only if auth is disabled on the Pico):
    with NetHIDClient("192.168.1.50") as c:
        c.type("ping\n")
"""

import socket
import ssl
import struct
import json
import time
import hmac
import hashlib


# ── Exceptions ────────────────────────────────────────────────────────────────

class AuthError(Exception):
    """Raised when authentication fails or the session expires."""
    pass

class LockedError(AuthError):
    """Raised when the server is locked out due to too many failed attempts."""
    def __init__(self, retry_after: int = 0):
        self.retry_after = retry_after
        super().__init__(f"Server locked — retry after {retry_after}s")


# ── HID key code table ────────────────────────────────────────────────────────

KEY_CODES = {
    'a':0x04,'b':0x05,'c':0x06,'d':0x07,'e':0x08,'f':0x09,'g':0x0A,'h':0x0B,
    'i':0x0C,'j':0x0D,'k':0x0E,'l':0x0F,'m':0x10,'n':0x11,'o':0x12,'p':0x13,
    'q':0x14,'r':0x15,'s':0x16,'t':0x17,'u':0x18,'v':0x19,'w':0x1A,'x':0x1B,
    'y':0x1C,'z':0x1D,'1':0x1E,'2':0x1F,'3':0x20,'4':0x21,'5':0x22,'6':0x23,
    '7':0x24,'8':0x25,'9':0x26,'0':0x27,
    'enter':0x28,'esc':0x29,'backspace':0x2A,'tab':0x2B,'space':0x2C,
    '-':0x2D,'=':0x2E,'[':0x2F,']':0x30,'\\':0x31,';':0x33,"'":0x34,
    '`':0x35,',':0x36,'.':0x37,'/':0x38,'capslock':0x39,
    'f1':0x3A,'f2':0x3B,'f3':0x3C,'f4':0x3D,'f5':0x3E,'f6':0x3F,
    'f7':0x40,'f8':0x41,'f9':0x42,'f10':0x43,'f11':0x44,'f12':0x45,
    'insert':0x49,'home':0x4A,'pageup':0x4B,'delete':0x4C,'end':0x4D,
    'pagedown':0x4E,'right':0x4F,'left':0x50,'down':0x51,'up':0x52,
    'numlock':0x53,'printscreen':0x46,'scrolllock':0x47,'pause':0x48,
}

MOD_BITS = {
    'lctrl':0x01,'lshift':0x02,'lalt':0x04,'lgui':0x08,
    'rctrl':0x10,'rshift':0x20,'ralt':0x40,'rgui':0x80,
    'ctrl':0x01,'shift':0x02,'alt':0x04,'gui':0x08,'win':0x08,'meta':0x08,
}

ASCII_MAP: dict[str, tuple[int, int]] = {}

def _build_ascii_map():
    for i, ch in enumerate('abcdefghijklmnopqrstuvwxyz'):
        ASCII_MAP[ch] = (0, 0x04 + i)
    for i, ch in enumerate('ABCDEFGHIJKLMNOPQRSTUVWXYZ'):
        ASCII_MAP[ch] = (0x02, 0x04 + i)
    for i, ch in enumerate('1234567890'):
        ASCII_MAP[ch] = (0, 0x1E + i if i < 9 else 0x27)
    simple = {' ':0x2C,'\n':0x28,'\t':0x2B,'-':0x2D,'=':0x2E,'[':0x2F,
              ']':0x30,'\\':0x31,';':0x33,"'":0x34,'`':0x35,',':0x36,
              '.':0x37,'/':0x38}
    for ch, kc in simple.items():
        ASCII_MAP[ch] = (0, kc)
    shifted = {'!':0x1E,'@':0x1F,'#':0x20,'$':0x21,'%':0x22,'^':0x23,
               '&':0x24,'*':0x25,'(':0x26,')':0x27,'_':0x2D,'+':0x2E,
               '{':0x2F,'}':0x30,'|':0x31,':':0x33,'"':0x34,'~':0x35,
               '<':0x36,'>':0x37,'?':0x38}
    for ch, kc in shifted.items():
        ASCII_MAP[ch] = (0x02, kc)

_build_ascii_map()


# ── Client ────────────────────────────────────────────────────────────────────

class NetHIDClient:
    """TCP client for the NetHID Pico W device."""

    def __init__(self, host: str, port: int = 9000, password: str = '',
                 protocol: str = 'binary', timeout: float = 5.0,
                 user: str = 'admin',
                 tls: bool = False, tls_verify: bool = True,
                 cafile: str | None = None, server_hostname: str | None = None):
        """
        host     : IP address or hostname of the Pico W or mock server
        port     : TCP port (default 9000; use 9443 for the TLS listener)
        password : device password — never sent over the wire when protocol
                   is 'json' (challenge-response HMAC). Leave '' if auth is
                   disabled on the device.
        protocol : 'binary' (lower latency, plaintext auth) or 'json'
                   (human-readable, HMAC challenge-response auth)
        timeout  : socket timeout in seconds
        user     : which configured login to use (default 'admin')
        tls      : wrap the connection in TLS (the device's :9443 port), so the
                   command stream is encrypted, not just authenticated.
        tls_verify : verify the device certificate (default True). Connect by the
                   cert's FQDN to verify; for raw IP use tls_verify=False
                   (encryption without identity) or set cafile/server_hostname.
        cafile   : optional CA bundle to trust (else the system trust store).
        server_hostname : name for SNI + cert check (defaults to host).
        """
        self.host     = host
        self.port     = port
        self.password = password
        self.protocol = protocol
        self.timeout  = timeout
        self.user     = user
        self.tls      = tls
        self.tls_verify = tls_verify
        self.cafile   = cafile
        self.server_hostname = server_hostname or host
        self._sock: socket.socket | None = None

    # ── Connection management ──────────────────────────────────────────────

    def connect(self):
        """Open the connection (TLS-wrapped if tls=True) and authenticate."""
        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw.settimeout(self.timeout)
        raw.connect((self.host, self.port))
        if self.tls:
            ctx = ssl.create_default_context(cafile=self.cafile)
            if not self.tls_verify:
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
            self._sock = ctx.wrap_socket(raw, server_hostname=self.server_hostname)
        else:
            self._sock = raw
        if self.password:
            self._authenticate()
        return self

    def _authenticate(self):
        """Authenticate; raise on failure. JSON uses HMAC challenge-response so
        the password never crosses the network; binary uses the plaintext 0xA0
        frame (the binary protocol has no HMAC variant)."""
        if self.protocol == 'binary':
            self._send_binary_auth(self.password)
        else:
            self._send_json_hmac_auth(self.user, self.password)

    def _send_binary_auth(self, password: str):
        """
        Binary auth handshake (plaintext — the binary socket has no HMAC variant):
          → 0xA0 <len:u8> <password utf-8>
          ← 0xA0 <status:u8>  0x01=ok 0x00=wrong 0x02=locked 0x05=hmac_required
        """
        pw_bytes = password.encode('utf-8')[:255]
        self._send(bytes([0xA0, len(pw_bytes)]) + pw_bytes)
        resp = self._recv_exact(2)
        if resp[0] != 0xA0:
            raise AuthError(f"Unexpected auth response: {resp.hex()}")
        status = resp[1]
        if status == 0x01:
            return   # ok
        elif status == 0x02:
            raise LockedError()
        elif status == 0x05:
            raise AuthError("Device requires HMAC (ALLOW_PLAINTEXT_AUTH=0) — "
                            "use protocol='json' for challenge-response auth")
        else:
            raise AuthError("Wrong password")

    def _send_json_hmac_auth(self, user: str, password: str):
        """
        JSON challenge-response — the password never crosses the network:
          → {"type":"challenge"}\n
          ← {"nonce":"<hex>"}\n
          → {"type":"auth","user":..,"nonce":..,"response":HMAC-SHA256(pw,nonce)}\n
          ← {"ok":true}\n   or error
        """
        self._send((json.dumps({"type": "challenge"}) + "\n").encode())
        reply = self._recv_line()
        try:
            nonce = json.loads(reply)["nonce"]
        except Exception:
            raise AuthError(f"Bad challenge response: {reply!r}")

        response = hmac.new(password.encode('utf-8'),
                            nonce.encode('ascii'),
                            hashlib.sha256).hexdigest()
        msg = json.dumps({"type": "auth", "user": user,
                          "nonce": nonce, "response": response}) + "\n"
        self._send(msg.encode())

        reply = self._recv_line()
        try:
            j = json.loads(reply)
        except Exception:
            raise AuthError(f"Bad auth response: {reply!r}")
        if j.get("ok"):
            return
        err = j.get("error", "unknown")
        if err == "locked":
            raise LockedError(j.get("retry_after", 0))
        raise AuthError(f"Auth failed: {err}")

    def _recv_exact(self, n: int) -> bytes:
        """Receive exactly n bytes."""
        buf = b""
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("Connection closed during recv")
            buf += chunk
        return buf

    def _drain(self):
        """Discard any buffered reply lines without blocking.
        Most commands send a {"ok":true} reply that the API doesn't read;
        they accumulate in the socket buffer. Drain them before a query
        whose specific reply we actually need."""
        if not self._sock:
            return
        self._sock.setblocking(False)
        try:
            while True:
                chunk = self._sock.recv(4096)
                if not chunk:
                    break
        except (BlockingIOError, OSError):
            pass
        finally:
            self._sock.setblocking(True)

    def _recv_line(self) -> str:
        """Receive bytes until newline."""
        buf = b""
        while True:
            ch = self._sock.recv(1)
            if not ch:
                break
            if ch == b'\n':
                break
            buf += ch
        return buf.decode('utf-8', 'ignore').strip()

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *_):
        self.close()

    # ── Low-level send ─────────────────────────────────────────────────────

    def _send(self, data: bytes):
        if not self._sock:
            raise RuntimeError("Not connected. Call connect() first.")
        self._sock.sendall(data)

    def _send_binary_key(self, modifier: int, keys: list[int]):
        keys = (list(keys) + [0] * 6)[:6]
        self._send(struct.pack('9B', 0x01, modifier, 0, *keys))

    def _send_binary_mouse(self, buttons: int, x: int, y: int, wheel: int):
        self._send(struct.pack('BBbbb', 0x02, buttons,
                               max(-127, min(127, x)),
                               max(-127, min(127, y)),
                               max(-127, min(127, wheel))))

    def _send_binary_mouse_abs(self, buttons: int, x: int, y: int, wheel: int):
        # 0x06 <buttons> <x:u16 LE> <y:u16 LE> <wheel:s8>
        x = max(0, min(32767, x))
        y = max(0, min(32767, y))
        self._send(struct.pack('<BBHHb', 0x06, buttons, x, y,
                               max(-127, min(127, wheel))))

    def _send_binary_text(self, text: str):
        enc = text.encode('utf-8')[:255]
        self._send(bytes([0x03, len(enc)]) + enc)

    def _send_binary_combo(self, modifier: int, key: int):
        self._send(bytes([0x04, modifier, key]))

    def _send_json(self, msg: dict):
        self._send((json.dumps(msg) + '\n').encode())

    # ── Keyboard API ───────────────────────────────────────────────────────

    def key_down(self, modifier: int = 0, keys: list[int] = ()):
        """Send a raw key-down report (no auto-release)."""
        if self.protocol == 'binary':
            self._send_binary_key(modifier, keys)
        else:
            self._send_json({"type": "key", "modifier": modifier,
                             "keys": (list(keys) + [0]*6)[:6]})

    def key_up(self):
        """Release all keys."""
        if self.protocol == 'binary':
            self._send_binary_key(0, [])
        else:
            self._send_json({"type": "key_release"})

    def key_press(self, modifier: int = 0, keys: list[int] = ()):
        """Press and release keys."""
        self.key_down(modifier, keys)
        self.key_up()

    def combo(self, key: str, ctrl=False, shift=False, alt=False,
              gui=False, win=False, meta=False, delay_ms: int = 0):
        """
        Send a keyboard shortcut.
        Example: c.combo('c', ctrl=True)   → Ctrl+C
                 c.combo('F4', alt=True)   → Alt+F4
        """
        mod = 0
        if ctrl:            mod |= 0x01
        if shift:           mod |= 0x02
        if alt:             mod |= 0x04
        if gui or win or meta: mod |= 0x08
        kc = KEY_CODES.get(key.lower(), 0)
        if not kc:
            raise ValueError(f"Unknown key name: '{key}'")
        if self.protocol == 'binary':
            self._send_binary_combo(mod, kc)
        else:
            self._send_json({"type": "combo", "modifier": mod, "key": kc})
        if delay_ms:
            time.sleep(delay_ms / 1000)

    def type(self, text: str, delay_ms: int = 20):
        """Type a string character by character."""
        if self.protocol == 'binary':
            for i in range(0, len(text), 200):
                self._send_binary_text(text[i:i+200])
        else:
            self._send_json({"type": "text", "text": text,
                             "delay_ms": delay_ms})

    def press_key(self, key: str, shift=False, ctrl=False, alt=False):
        """Press a named key with optional modifiers."""
        mod = 0
        if shift: mod |= 0x02
        if ctrl:  mod |= 0x01
        if alt:   mod |= 0x04
        kc = KEY_CODES.get(key.lower(), 0)
        if kc:
            self.key_press(mod, [kc])

    def unlock(self, password: str, *,
               settle_s: float = 1.0,
               type_delay_ms: int = 40,
               submit: bool = True):
        """
        Dismiss the Windows lock screen and enter the password.

        The lock screen runs on a "secure desktop"; keystrokes sent too fast
        or too soon after it appears get dropped. This paces the sequence:
          1. press a key to dismiss the lock-screen image (reveals the field)
          2. wait `settle_s` for the secure desktop / password box to settle
          3. type the password slowly (`type_delay_ms` per key)
          4. optionally press Enter to submit

        Notes:
          - This assumes the machine is already AWAKE. Waking from sleep is a
            separate step (see wake()); call that first if needed.
          - If the first character of your password sometimes goes missing,
            increase settle_s.
        """
        import time
        # 1. Dismiss the lock-screen wallpaper to reveal the password field.
        #    Escape is safe — it won't add characters to the field.
        self.press_key('esc')
        time.sleep(0.3)
        # A second nudge in case the first was consumed waking the display.
        self.press_key('esc')
        # 2. Let the secure desktop settle and take focus.
        time.sleep(settle_s)
        # 3. Type the password slowly so no keys are dropped.
        self.type(password, delay_ms=type_delay_ms)
        # 4. Submit.
        if submit:
            time.sleep(0.3)
            self.press_key('enter')

    # ── Mouse API ──────────────────────────────────────────────────────────

    def mouse_move(self, x: int, y: int):
        """Move mouse relatively."""
        if self.protocol == 'binary':
            self._send_binary_mouse(0, x, y, 0)
        else:
            self._send_json({"type": "mouse", "buttons": 0,
                             "x": x, "y": y, "wheel": 0})

    def mouse_move_abs(self, x, y, *, buttons: int = 0):
        """
        Move the cursor to an ABSOLUTE screen position.

        Coordinates may be given two ways:
          - floats in 0.0..1.0  → fraction of screen (0,0=top-left, 1,1=bottom-right)
                                   e.g. mouse_move_abs(0.5, 0.5) → centre
          - ints in 0..32767    → raw HID logical units across the screen

        This presents as a separate absolute-pointer HID report (Report ID 3),
        so the cursor jumps to the position regardless of OS mouse speed or
        acceleration — unlike mouse_move(), which is relative and acceleration
        -dependent. Useful for reliably clicking a known on-screen location.
        """
        # Normalise to 0..32767 logical units.
        #   float  → treated as a 0.0..1.0 fraction of the screen
        #   int    → treated as a raw 0..32767 logical coordinate
        def to_units(v):
            if isinstance(v, float):
                return int(round(max(0.0, min(1.0, v)) * 32767))
            return int(max(0, min(32767, v)))
        ux, uy = to_units(x), to_units(y)
        if self.protocol == 'binary':
            self._send_binary_mouse_abs(buttons, ux, uy, 0)
        else:
            self._send_json({"type": "mouse_abs", "buttons": buttons,
                             "x": ux, "y": uy, "wheel": 0})

    def click_abs(self, x, y, button: str = 'left'):
        """Move to an absolute position and click there."""
        import time
        b = {'left': 1, 'right': 2, 'middle': 4}.get(button.lower(), 1)
        self.mouse_move_abs(x, y, buttons=0)      # position first
        time.sleep(0.02)
        self.mouse_move_abs(x, y, buttons=b)      # press at that position
        time.sleep(0.02)
        self.mouse_move_abs(x, y, buttons=0)      # release

    def mouse_click(self, button: str = 'left'):
        """Click a mouse button ('left', 'right', 'middle')."""
        b = {'left': 1, 'right': 2, 'middle': 4}.get(button.lower(), 1)
        if self.protocol == 'binary':
            self._send_binary_mouse(b, 0, 0, 0)
            self._send_binary_mouse(0, 0, 0, 0)
        else:
            self._send_json({"type": "mouse_click", "button": b})

    def mouse_scroll(self, amount: int):
        """Scroll wheel. Positive = up, negative = down."""
        if self.protocol == 'binary':
            self._send_binary_mouse(0, 0, 0, amount)
        else:
            self._send_json({"type": "mouse", "buttons": 0,
                             "x": 0, "y": 0, "wheel": amount})

    def mouse_drag(self, dx: int, dy: int, button: str = 'left'):
        """Click-drag relative to current position."""
        b = {'left': 1, 'right': 2, 'middle': 4}.get(button.lower(), 1)
        if self.protocol == 'binary':
            self._send_binary_mouse(b, 0, 0, 0)
            while abs(dx) > 0 or abs(dy) > 0:
                sx = max(-127, min(127, dx))
                sy = max(-127, min(127, dy))
                self._send_binary_mouse(b, sx, sy, 0)
                dx -= sx; dy -= sy
            self._send_binary_mouse(0, 0, 0, 0)

    # ── Misc ───────────────────────────────────────────────────────────────

    def ping(self) -> bool:
        """Send a ping. Returns True if the device responds."""
        try:
            self._send(bytes([0xFF]))
            resp = self._recv_exact(2)
            return resp == bytes([0xFF, 0x00])
        except Exception:
            return False

    def logout(self):
        """Tell the device to lock (requires re-auth on next connection)."""
        if self.protocol == 'binary':
            self.close()
        else:
            self._send_json({"type": "logout"})

    def wake(self):
        """
        Ask the Pico to assert the USB Remote Wakeup signal.
        The host must have granted Remote Wakeup permission (BIOS setting
        + OS power management) for this to actually wake a sleeping system.
        Has no effect if the host is already awake.
        """
        if self.protocol == 'binary':
            self._send(bytes([0x05]))
        else:
            self._send_json({"type": "wake"})

    def status(self) -> dict:
        """
        Query device status. Returns a dict including auth state plus wake
        diagnostics:
          suspended   - is the USB host currently suspended (as the Pico sees it)
          wake_diag   - "suspends=N wakeups_en=X wake_cmds=M last_rc=R"
            suspends   : how many times the host has suspended the device
            wakeups_en : did the host grant Remote Wakeup permission?
                         1=yes 0=no -1=never suspended
            wake_cmds  : how many wake commands the device received while asleep
            last_rc    : result of the last tud_remote_wakeup() call
                         1=signal sent OK, 0=call failed,
                         -2=permission not granted, -1=no wake attempted yet
        This uses the JSON protocol regardless of the binary/JSON setting.
        """
        import json
        # status is only implemented on the JSON path
        if self.protocol == 'binary':
            raise RuntimeError("status() requires the JSON protocol "
                               "(create the client with protocol='json')")
        # Clear any unread {"ok":true} replies from prior commands so we read
        # the status reply and not a stale acknowledgment.
        self._drain()
        self._send_json({"type": "status"})
        # Read lines until we get one that looks like the status object.
        for _ in range(10):
            line = self._recv_line()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict) and ("wake_diag" in obj or "timeout" in obj):
                return obj
        raise RuntimeError("No status reply received (is the firmware "
                           "current? wake_diag requires the latest build)")


# ── CLI ───────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse
    import getpass

    parser = argparse.ArgumentParser(
        description="NetHID client — connects to a Pico W or the mock server")
    parser.add_argument('host', nargs='?', default='127.0.0.1',
                        help='Host/IP (default: 127.0.0.1)')
    parser.add_argument('--port', '-p', type=int, default=9000,
                        help='TCP port (default: 9000)')
    parser.add_argument('--password', '-P', default='',
                        help='Device password (or set NETHID_PASSWORD env var)')
    parser.add_argument('--ask-password', '-a', action='store_true',
                        help='Prompt for password interactively')
    parser.add_argument('--protocol', choices=['binary', 'json'], default='binary',
                        help='Wire protocol (default: binary)')
    parser.add_argument('--demo', action='store_true',
                        help='Run the automated demo sequence')
    parser.add_argument('--interactive', '-i', action='store_true',
                        help='Start an interactive REPL')
    args = parser.parse_args()

    # Resolve password: CLI flag > env var > interactive prompt
    import os
    password = args.password or os.environ.get('NETHID_PASSWORD', '')
    if args.ask_password:
        password = getpass.getpass("NetHID password: ")

    print(f"Connecting to {args.host}:{args.port} (protocol={args.protocol})…")

    try:
        with NetHIDClient(args.host, port=args.port, password=password,
                          protocol=args.protocol) as c:

            ok = c.ping()
            print(f"Ping: {'✓ ok' if ok else '✗ no response'}")

            if args.demo or (not args.interactive):
                print("\n── Demo sequence ──────────────────────")
                print("  type: 'Hello from NetHID!'")
                c.type("Hello from NetHID!")
                time.sleep(0.3)
                print("  press: Enter")
                c.press_key('enter')
                time.sleep(0.2)
                print("  combo: Ctrl+A")
                c.combo('a', ctrl=True)
                time.sleep(0.2)
                print("  combo: Ctrl+C")
                c.combo('c', ctrl=True)
                time.sleep(0.2)
                print("  combo: Alt+F4")
                c.combo('F4', alt=True)
                time.sleep(0.2)
                print("  combo: Win+L")
                c.combo('l', gui=True)
                time.sleep(0.2)
                print("  mouse: move +100, -50")
                c.mouse_move(100, -50)
                time.sleep(0.1)
                print("  mouse: left click")
                c.mouse_click('left')
                time.sleep(0.1)
                print("  mouse: right click")
                c.mouse_click('right')
                time.sleep(0.1)
                print("  mouse: scroll down 3")
                c.mouse_scroll(3)
                time.sleep(0.1)
                print("  mouse: scroll up 3")
                c.mouse_scroll(-3)
                time.sleep(0.1)
                old = c.protocol
                c.protocol = 'binary'
                print("  type (binary): 'Binary protocol test\\n'")
                c.type("Binary protocol test\n")
                c.protocol = old
                print("\nDemo complete.")

            if args.interactive:
                print("\n── Interactive mode ───────────────────────────────────────")
                print("  type <text>                     type a string")
                print("  key <name> [ctrl] [shift] [alt] key combo")
                print("  mouse <x> <y>                   move mouse")
                print("  click [left|right|middle]       mouse click")
                print("  scroll <amount>                 scroll (+up/-down)")
                print("  ping                            ping server")
                print("  quit                            disconnect")
                print()
                while True:
                    try:
                        line = input("nethid> ").strip()
                    except (EOFError, KeyboardInterrupt):
                        break
                    if not line: continue
                    parts = line.split()
                    cmd = parts[0].lower()
                    try:
                        if cmd in ('quit','exit','q'):
                            break
                        elif cmd == 'ping':
                            print('pong ✓' if c.ping() else 'no response')
                        elif cmd == 'type':
                            text = line[len('type'):].strip()
                            c.type(text) if text else print("usage: type <text>")
                        elif cmd == 'key':
                            if len(parts) < 2: print("usage: key <name> [ctrl] ..."); continue
                            flags = {p.lower() for p in parts[2:]}
                            c.combo(parts[1], ctrl='ctrl' in flags,
                                    shift='shift' in flags, alt='alt' in flags,
                                    gui='gui' in flags or 'win' in flags)
                            print(f"  sent: {parts[1]} + {flags or 'no mods'}")
                        elif cmd == 'mouse':
                            if len(parts) < 3: print("usage: mouse <x> <y>"); continue
                            c.mouse_move(int(parts[1]), int(parts[2]))
                        elif cmd == 'click':
                            c.mouse_click(parts[1] if len(parts) > 1 else 'left')
                        elif cmd == 'scroll':
                            c.mouse_scroll(int(parts[1])) if len(parts) > 1 else print("usage: scroll <n>")
                        else:
                            print(f"  unknown: {cmd}")
                    except AuthError as e:
                        print(f"  auth error: {e}")
                        break
                    except Exception as e:
                        print(f"  error: {e}")
                print("Disconnected.")

    except LockedError as e:
        print(f"Error: {e}")
        raise SystemExit(1)
    except AuthError as e:
        print(f"Auth failed: {e}")
        raise SystemExit(1)
    except ConnectionRefusedError:
        print(f"Connection refused — is the server running on {args.host}:{args.port}?")
        raise SystemExit(1)


# ── IR / RF helpers (HTTP, port 80) ──────────────────────────────────────────
# The IR blaster and 433 MHz transmitter are exposed as HTTP JSON endpoints
# (the same ones the web UI custom tabs call), not over the TCP binary protocol.
# These small helpers post to them directly.
import urllib.request


def _http_post_json(host: str, path: str, payload: dict, timeout: float = 5.0):
    """POST a JSON body to http://host{path} and return the parsed response."""
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        f"http://{host}{path}", data=data,
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode()
    try:
        return json.loads(body)
    except ValueError:
        return {"raw": body}


def ir_send_nec(host: str, code: int):
    """Send a 32-bit NEC IR code (e.g. 0xE0E040BF). host is the device IP."""
    return _http_post_json(host, "/api/ir", {"proto": "nec", "code": int(code) & 0xFFFFFFFF})


def ir_send_raw(host: str, timings_us, carrier: int = 38000):
    """Send a raw IR frame: timings_us = [mark, space, ...] in microseconds."""
    return _http_post_json(host, "/api/ir",
                           {"proto": "raw", "carrier": int(carrier),
                            "timings": [int(t) for t in timings_us]})


def rf_send_raw(host: str, timings_us, repeat: int = 6):
    """Send a 433 MHz OOK burst: timings_us = [mark, gap, ...], repeated `repeat`."""
    return _http_post_json(host, "/api/rf",
                           {"timings": [int(t) for t in timings_us],
                            "repeat": int(repeat)})


if __name__ == "__main__" and False:
    # Example (won't run; flip the guard to try):
    #   ir_send_nec("192.168.8.205", 0xE0E040BF)            # TV power (example)
    #   rf_send_raw("192.168.8.205", [300, 900]*12, repeat=8)  # screen up (example)
    pass
