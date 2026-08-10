# NetHID C++ — Pico 2 W / Pico SDK

> **Continuing this work?** Read [`docs/`](docs/) first — one file per feature,
> each covering how it works and what is verified on hardware. Start with
> [docs/SETTINGS.md](docs/SETTINGS.md): it carries the HID post-mortems, and
> the reasoning there is the fastest way to understand why the rest is shaped
> as it is.

USB HID keyboard + mouse over WiFi for the **Raspberry Pi Pico 2 W** (RP2350).
Full feature parity with the Python version, plus **USB Remote Wakeup**.

---

## What's new vs the original C++ port

| Feature | Original C++ | This version |
|---|---|---|
| Authentication | ✗ | ✓ Password + session timeout + lockout |
| Web login page | ✗ | ✓ |
| Session cookies | ✗ | ✓ HttpOnly, SameSite=Strict |
| Remote Wakeup | ✗ | ✓ `tud_suspend_cb` / `tud_remote_wakeup()` |
| F13–F24 keys | ✗ | ✓ |
| PRTSC/SCRLK/PAUSE | ✗ | ✓ |
| NUMLK / APP key | ✗ | ✓ |
| Double-tap sticky mods | ✗ | ✓ |
| Legend update on Shift/Caps | ✗ | ✓ |
| Nav row (PGUP/↑/PGDN) | ✗ | ✓ |
| Wake button in web UI | ✗ | ✓ |
| `client.py wake()` method | ✗ | ✓ |
| Socket auth handshake | ✗ | ✓ |

---

## Prerequisites

```bash
git clone https://github.com/raspberrypi/pico-sdk.git --recurse-submodules
export PICO_SDK_PATH=/path/to/pico-sdk
# Ubuntu/Debian:
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi
```

## Editing the web UI

The web UI lives in **`include/web_html.h`** as C string literals (`MAIN_HTML`,
`LOGIN_HTML`), which `src/web.cpp` pulls in with `#include "web_html.h"`. This
keeps the large, frequently-edited markup out of the server logic. Do **not**
hand-edit the escaped literals — a single mis-escaped backslash or quote
produces a page that works standalone but breaks when served. Instead:

`NetHID.html` in the repo root **is** that standalone copy and is the source of
truth — edit it, then regenerate. (The header no longer holds a single
`MAIN_HTML`: `build_web_html.py` splits the page into `MAIN_HEAD`, one
`MAIN_TAB_*` per tab and `MAIN_FOOTER`, so the per-user tab permissions can serve
a subset. Extracting `-n MAIN_HTML` will not find anything.)

```bash
# 1. If you need to re-extract a single literal (e.g. the login page)
python3 tools/web/cstring_to_html.py include/web_html.h -n LOGIN_HTML -o Login.html

# 2. Edit NetHID.html and test it in a browser until it works

# 3. Regenerate the header in one step (rewrites MAIN_HTML, keeps LOGIN_HTML):
python3 tools/web/build_web_html.py --main NetHID.html

# 4. Rebuild + flash.
```

To also regenerate the login page, pass `--login Login.html`. The scripts are
exact inverses, so what you test in the browser is exactly what the Pico serves.
`web.cpp` never needs editing for UI changes — only `include/web_html.h` does.

## HTTPS

HTTPS is available using letsencrypt. Point the FQDN to your local IP address.

```bash
export CF_Token="your-cloudflare-api-token"
export CF_Account_ID="your-account-id"
./tools/cert/make-cert-letsencrypt.sh nethid.example.com
```

## Build

```bash
mkdir build && cd build
cmake .. -DPICO_BOARD=pico2_w -DENABLE_HTTPS=ON   # or pico_w for original Pico W (untested)

# Linux:
make -j$(nproc)
# macOS:
make -j$(sysctl -n hw.logicalcpu)
# Either platform (simplest, single-threaded):
# make
```

Produces `build/nethid.uf2`.

After changing the SDK, toolchain, or board, delete the build directory and
re-run cmake (`rm -rf build`) — cmake caches the compiler and board on first
configure and won't pick up the change otherwise.

## Configure

Edit **`include/config.h`** before building:

```c
#define WIFI_SSID           "your_network"
#define WIFI_PASSWORD       "your_password"
#define PASSWORD            "changeme"      // NetHID device password
#define SESSION_TIMEOUT_S   300             // idle lock timeout
#define STATIC_IP           ""              // "" = DHCP
```

## Flash

Hold **BOOTSEL**, plug into USB, drag `nethid.uf2` onto the `RP2350` drive.

Debug output on **UART0** (GP0=TX, GP1=RX) at 115200 baud.

---

## Finding the Pico's IP address

There are two ways:

1. **UART console** — connect a USB-UART adapter to GP0/GP1 at 115200 baud.
   The boot log prints the IP, web UI URL, and socket address.

2. **Typed onto the host** (default, no adapter needed) — with
   `TYPE_IP_ON_BOOT 1` in `config.h` (the default), the Pico waits
   `TYPE_IP_DELAY_S` seconds after joining WiFi, then types a short usage
   synopsis directly onto whatever has keyboard focus on the host:

   ```
   NetHID ready.
   Web UI: http://192.168.1.42/
   Socket: 192.168.1.42 port 9000
   Auth: password required
   ```

   For a headless setup: open a text editor on the target machine, plug in
   the Pico, wait ~5 seconds, and the address appears. Set `TYPE_IP_ON_BOOT 0`
   to disable.

---

## Remote Wakeup

When the target PC sleeps, the Pico can wake it by asserting a USB resume
signal. Three things must all be true:

1. **BIOS/UEFI** — "USB Wake from S3/S4" must be enabled (often off by default)
2. **OS** — the OS must have granted Remote Wakeup permission; most do when
   the device is enumerated as a HID with the Remote Wakeup capability bit set
3. **USB port stays powered** — most ports do, some budget boards cut power

To send a wake signal:

```python
# Python client:
with NetHIDClient("192.168.1.50", password="changeme") as c:
    c.wake()

# Or tap the WAKE button in the web UI
```

The `tud_suspend_cb` callback tells the Pico whether the host granted
permission. If not, `hid_push_wakeup()` logs a warning and does nothing.

---

## Architecture

```
main.cpp
│
│  Core 0 — cooperative loop
│  ─────────────────────────
│  hid_task()        → tud_task() + drain HID queue
│  cyw43_arch_poll() → lwIP timers / deferred work
│
├── auth.cpp
│     Spin-lock protected state: authenticated flag, last-activity time,
│     failed-attempt counter, 8-slot web token table, lockout timer.
│     pico/rand.h for token generation.
│
├── hid.cpp
│     Spin-lock FIFO (32 entries).  TinyUSB callbacks:
│       tud_suspend_cb()            → set _suspended, record wakeup permission
│       tud_resume_cb()             → clear _suspended
│       tud_hid_report_complete_cb()→ gate next send
│     HID_CMD_WAKEUP: calls tud_remote_wakeup() while suspended.
│     String typer: one char per USB frame, expandable to full ASCII.
│
├── server.cpp   (TCP :9000)
│     lwIP raw API. Binary + JSON protocol with auth handshake.
│     New: 0x05 Wake (binary), "wake"/"logout"/"status" (JSON).
│
├── web.cpp      (HTTP :80)
│     lwIP raw API. Chunked HTML send via tcp_sent_cb.
│     New: /api/wake endpoint, /api/auth, /api/logout,
│          login page, session cookie (HttpOnly, SameSite=Strict).
│
└── usb_descriptors.c
      TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP already set.
      Composite HID: keyboard Report ID 1 + mouse Report ID 2.
```

---

## Wire protocol additions

### Binary (port 9000)

| Byte 0 | Meaning | Size |
|--------|---------|------|
| `0xA0` | Auth handshake | `2+N`: `0xA0 <len> <password>` |
| `0x01` | Keyboard report | 8 B |
| `0x02` | Relative mouse | 5 B: `0x02 <buttons> <dx:s8> <dy:s8> <wheel:s8>` |
| `0x03` | Type string | `2+N` |
| `0x04` | Key combo | 3 B |
| `0x05` | Wake | 1 B |
| `0x06` | Absolute mouse | 7 B: `0x06 <buttons> <x:u16 LE> <y:u16 LE> <wheel:s8>` |

Absolute X/Y are `0..32767` spanning the full screen (0,0 = top-left).

Auth reply: `0xA0 0x01` ok, `0xA0 0x00` wrong, `0xA0 0x02` locked, `0xA0 0x04` session expired.

### JSON (port 9000)

```json
{"type":"auth",   "password":"..."}
{"type":"mouse",     "buttons":0, "x":10, "y":0, "wheel":0}
{"type":"mouse_abs", "buttons":0, "x":16384, "y":16384, "wheel":0}
{"type":"wake"}
{"type":"logout"}
{"type":"status"}
```

`mouse_abs` X/Y are `0..32767` over the full screen.

### HTTP (port 80)

`POST /api/mouse_abs` with `{"buttons":0,"x":16384,"y":16384,"wheel":0}`
moves the cursor to an absolute screen position.

---

## Absolute cursor positioning

NetHID presents three HID reports: keyboard (ID 1), relative mouse (ID 2),
and an **absolute pointer (ID 3)**. The absolute pointer lets you place the
cursor at an exact screen location, independent of OS mouse speed/acceleration
— useful for reliably clicking a known position.

```python
# fraction of screen (0.0..1.0): centre
c.mouse_move_abs(0.5, 0.5)
# top-left, then bottom-right corner
c.mouse_move_abs(0.0, 0.0)
c.mouse_move_abs(1.0, 1.0)
# raw logical units (0..32767) also accepted
c.mouse_move_abs(16384, 16384)
# move to a position and click there
c.click_abs(0.5, 0.5, 'left')
```

Relative `mouse_move()` still works as before for incremental movement.

---

## Python client

```python
from client import NetHIDClient

with NetHIDClient("192.168.1.50", password="changeme") as c:
    c.wake()                      # attempt Remote Wakeup
    c.type("Hello from NetHID!")
    c.combo('c', ctrl=True)
    c.mouse_move(100, -50)
    c.mouse_click('left')
```

## Keeping secrets out of git

`include/env.h` holds your real WiFi credentials and web password. It is
**gitignored** — git never tracks it, so secrets cannot be committed. The repo
ships `include/env.h.example` as a template.

First-time setup:

```bash
cp include/env.h.example include/env.h   # create your local secrets file
# edit include/env.h with your real values
git init && git add -A && git commit -m "initial"
```

`include/env.h` will not appear in `git status` or any commit. On a fresh
clone, just `cp include/env.h.example include/env.h` and fill it in again —
no extra git commands needed, because the ignore rule is committed in
`.gitignore`.

If you previously tracked `env.h` and want to stop, untrack it once (keeps the
local file): `git rm --cached include/env.h`.

## Defining WiFi networks (env.h)

Networks live in `include/env.h` as a single list — one line per network, no
numbered macros, no count to maintain:

```c
#define WIFI_NETWORK_LIST \
    WIFI("Home",   "homepassword",   0) \
    WIFI("Phone",  "phonepassword",  0) \
    WIFI("CafeWiFi", "",             4)    // open network: empty pw, auth 4
```

`auth_mode`: 0=WPA2-AES, 1=WPA/WPA2, 2=WPA3/WPA2, 3=WPA3, 4=open. Each line ends
with a backslash (`\`) except the last. `config.h` expands this list into the
`WIFI_NETWORKS[]` array automatically (an "X-macro"), so adding a network is a
single new line here. On boot the device connects to the in-range network with
the strongest signal among those listed.

## Example custom tabs (examples/)

The `examples/` folder has ready-made custom-tab configs you can import from the
CUSTOM editor (create a tab, then use its "+panels" button to import into it):

- `system-tab.json` — Sleep / Lock / Screenshot with per-OS chains, plus Copy/Paste
- `macros-tab.json` — common Ctrl shortcuts (Copy, Paste, Save, etc.)
- `media-tab.json` — a YouTube control tab (search, playback, volume, display)
- `netflix-nethid.json` — a Netflix genre picker

These are plain JSON in the custom-tab format, so you can edit them freely.

## IR blaster + 433 MHz screen control

The firmware can drive an IR LED (TV power/volume, etc.) and a 433 MHz OOK
transmitter (MX-FS-03V — projector screen up/down/stop). Both use the RP2350's
PIO so the waveform timing is generated in hardware and is immune to WiFi/USB
jitter. It can also **capture** ("learn") codes from your existing remotes given
receiver modules — see *Learning codes* below.

### Wiring (default pins; change in config.h)

IR LED — GPIO 16, through a transistor (a bare GPIO can't drive it for range):

```
  GPIO16 ──[1k]── B
                  │  2N2222 (NPN)
        5V ──[IR LED]──[68Ω]── C
                            E ── GND
```

433 MHz MX-FS-03V — GPIO 17:

```
  MX-FS-03V VCC  -> 5V   (use 5V, not 3V3, for usable range)
  MX-FS-03V DATA -> GPIO17
  MX-FS-03V GND  -> GND
  ANT terminal   -> ~17.3 cm of straight wire (quarter-wave whip for 433 MHz)
```

Receivers for capture (default pins; both optional):

```
  IR demod (TSOP382 / VS1838B): VCC->3V3, GND->GND, OUT->GPIO18   (3V3-safe)
  433 OOK receiver (RXB6 superhet preferred): VCC->3V3, GND->GND, DATA->GPIO19
    Power the RF receiver from 3V3 so DATA stays 3V3-safe. At 5V its DATA can
    reach 5V and needs a level shifter/divider. The cheap green receivers are
    very noisy — a superheterodyne module captures far more reliably.
```

### Learning codes (capture)

Capture is edge-timed on a GPIO IRQ and gap-framed; a captured frame is a raw
microsecond array — the exact format the transmit API takes, so learn → replay
needs no conversion. Arm a channel, point the source remote at the receiver and
press a button, then poll for the frame:

```
POST /api/ir/learn       -> {"armed":true}        (then press the remote button)
GET  /api/ir/captured    -> {"ready":false,"armed":true}            (keep polling)
                         -> {"ready":true,"count":67,"proto":"nec",
                             "code":3208707585,"carrier":38000,"timings":[...]}
POST /api/rf/learn  / GET /api/rf/captured  — same, for 433 MHz OOK.
```

IR decodes NEC for a friendly label; everything else returns `proto:"raw"`.
Replay is just the existing transmit call with the captured array:

```bash
# learn a button, then replay it
curl -b cookies.txt -X POST http://nethid.local/api/ir/learn
curl -b cookies.txt http://nethid.local/api/ir/captured        # poll until ready
curl -b cookies.txt -X POST http://nethid.local/api/ir \
  -H 'Content-Type: application/json' \
  -d '{"proto":"raw","carrier":38000,"timings":[9000,4500,560,560, ...]}'
```

Notes: the demodulated IR signal doesn't carry its carrier frequency, so capture
assumes 38 kHz for replay (fine for almost all remotes). RF capture is
best-effort — it rejects bursts with no long sync gap to filter receiver noise,
but a noisy module may need a few presses or threshold tuning in `remotes.cpp`.

### Endpoints

- `POST /api/ir`  — `{"proto":"nec","code":<u32>}` or
  `{"proto":"raw","carrier":38000,"timings":[mark,space,…]}` (microseconds)
- `POST /api/rf`  — `{"timings":[mark,gap,…],"repeat":6}` (microseconds)

### Using it from the custom tabs

In the CUSTOM editor, a button step can now be **+ IR** or **+ RF**:

- IR step: choose *NEC code* (paste a decimal or 0x-hex code — many TVs publish
  these) or *Raw timings* (carrier + microsecond mark/space list).
- RF step: a repeat count plus a microsecond mark/gap list.

Until you've captured your remotes, you can paste codes from public databases
(e.g. Flipper IRDB / LIRC for IR). For the projector screen you'll most likely
need to capture (phase 2), since screen remotes are rarely published.

### Verifying the remote timing (do this before trusting it)

This code has **not** been verified on hardware. Before relying on it:

1. Put a scope or logic analyzer on GPIO16 (IR) — confirm a ~38 kHz square wave
   *during marks* and silence during spaces. An IR LED through a phone camera
   shows a faint flicker when transmitting.
2. Put a scope on GPIO17 (433) — confirm the pulse/gap widths match what you
   sent (1 µs resolution).
3. If timings are off, the likely culprit is the PIO clock divider or the
   per-loop tick math in `src/rf_ook.pio` / `src/ir_tx.pio` and `src/remotes.cpp`.

### Turning the feature off

IR/RF is a compile-time option. To leave it out entirely — `remotes.cpp` and the
PIO programs aren't compiled, the `/api/ir` and `/api/rf` endpoints are removed,
and no PIO state machines are claimed — disable it at configure time:

```bash
cmake .. -DPICO_BOARD=pico2_w -DENABLE_REMOTES=OFF
```

(The default is ON.) The `ENABLE_REMOTES` define in `include/config.h` mirrors
this for the firmware code; CMake passes the matching value to the compiler, so
the build and the source stay in sync. The web UI's IR/RF step types still
appear in the editor, but the device returns 404 for those endpoints when the
feature is compiled out.

### Physical keyboard (matrix scanning, keymaps, layers)

NetHID can also drive a real key matrix — its own switches, diodes, keymaps
and QMK-style behaviour — while remaining a network HID device. Both sources
are merged, so a key held on the matrix is not released by an incoming network
report and vice versa.

```
cmake .. -DPICO_BOARD=pico2_w -DKEYBOARD=proto2x2 -DKEYMAP=advanced
```

Omit `-DKEYBOARD` and nothing changes: no matrix code is compiled and no GPIO
is claimed.

Boards live under `keyboards/<name>/` (hardware in `keyboard.h`, behaviour in
`keymaps/<keymap>/keymap.cpp`), exactly the split QMK uses. Features — layers,
mod-tap/layer-tap, one-shots, combos, caps word, macros — are one file each
under `src/kb/features/` and are selected per board in `rules.cmake` or
overridden on the command line. A disabled feature is not compiled, not
linked, and absent from the dispatch table.

Scanning runs on core 1 next to `hid_task()`, the loop that does nothing but
USB, so scan timing is unaffected by lwIP or the cyw43 background work.

With `KB_FEATURE_DYNAMIC_KEYMAP` the compiled keymap becomes a default that is
copied into RAM at boot, and a **KEYMAP** tab appears in the web UI: pick a
layer, click a key, choose a keycode, and it is live on the next press. Saving
to flash is a separate, explicit step. No recompile, no reflash, no host
application — which is the part QMK needs VIA or Vial for.

A board can be **modular**: any number of Picos on one polled serial bus, each
with its own pins, matrix size and key count. The build emits a minimal firmware
per module that needs no WiFi or USB and runs on any Pico, while the keymap stays
a single array. See **[docs/SPLIT.md](docs/SPLIT.md)**.

Development tooling lives under **[tools/](tools/README.md)**, grouped into
`check/`, `web/`, `keyboard/` and `cert/`. `tools/check/run-all.sh` runs every
static check and the host test suite in one go.

The web UI can be previewed with no hardware attached — `python3 tools/web/preview.py
--serve` injects a mock API layer into a copy of the page and serves it locally,
with console helpers for switching boards and taking modules off the bus.

An **SSD1306/SH1106 status display** can sit on the primary or on any module,
showing layer, modifiers, link state, WPM and which modules are answering. Only
changed pages are flushed and never more than one per call, so it cannot stall
the matrix scan. See **[docs/OLED.md](docs/OLED.md)**.

Rotary encoders get their own pins — two quadrature lines plus a push switch —
and map per layer to any keycode. Encoders can live on any module. See
**[docs/ENCODERS.md](docs/ENCODERS.md)**.

**Media keys** — volume, transport, brightness and the browser keys — travel on
their own Consumer Control report, so they are not keyboard usages and need
`KB_FEATURE_CONSUMER`. See **[docs/MEDIA_KEYS.md](docs/MEDIA_KEYS.md)**.

An **autoclicker** repeats any keycode at a fixed rate — hold for a burst, or
double-tap to latch it on. Target, rate and trigger are editable from the web UI
and persist to flash, so finding a rate you like costs no reflashes. See
**[docs/AUTOCLICK.md](docs/AUTOCLICK.md)**.

Hold the AP key at boot and NetHID serves its own WiFi network instead of
joining one, with a captive portal serving the WiFi and keymap tabs —
so a device that can't reach your router is still configurable without a
reflash. Provisioned networks are stored in flash and tried ahead of the
compiled list. See **[docs/AP_MODE.md](docs/AP_MODE.md)**, particularly the
notes on why it isn't automatic and why that interface is plain HTTP.

A **SETTINGS** tab exposes the `config.h` options that are safe to change at
runtime — quiet boot, matrix debug logging, tapping term, typing delay, session
timeouts — persisted to flash, available in both normal and setup mode. See
**[docs/SETTINGS.md](docs/SETTINGS.md)**, including what is deliberately left
compile-time and why.

The **login password** can be changed from the device, under the header cog, so
it does not need to live only in `env.h`. Because a keyboard has no reset hole,
the stored password is tagged with the build id of the firmware that wrote it
and is ignored by the next firmware flashed — forgetting it costs a reflash, not
a board. See **[docs/PASSWORDS.md](docs/PASSWORDS.md)**, including why it is
stored in plaintext and why setup mode refuses to change it.

Macros for `KB_MACRO(n)` can be built in the same tab — tap, hold, release, text
and delay steps, stored as verified bytecode and run by a non-blocking
interpreter on the keyboard itself, so a macro key works with no browser open.

Hold a chosen key while plugging the board in to land in the RP2350 bootloader
(`KB_FEATURE_BOOTMAGIC`), or put `QK_BOOT` on a layer. The bootmagic check is
the first thing `main()` does, before USB and WiFi, so a firmware that wedges
during association is still recoverable without reaching BOOTSEL.

`tools/kbtest/` fakes the GPIO layer and the clock and runs the real pipeline
on a PC — taps, holds, chords, one-shots and the matrix/network merge are
asserted byte for byte without flashing anything:

```
cd tools/kbtest && make && ./kbtest
```

See **[keyboards/README.md](keyboards/README.md)** for the full layout, pin
budget, wiring notes and how to add a board.

## Compile-time feature toggles (summary)

Three independent build options, each defaulting ON. Set any to `OFF` at
configure time to omit that subsystem entirely (its source isn't compiled and
it's never started):

| Option | What it builds | Disable with |
|---|---|---|
| `ENABLE_WEB` | HTTP server on port 80 (web UI + `/api/*`) | `-DENABLE_WEB=OFF` |
| `ENABLE_TCP` | Raw TCP server on port 9000 (binary + JSON) | `-DENABLE_TCP=OFF` |
| `ENABLE_REMOTES` | IR blaster + 433 MHz transmit | `-DENABLE_REMOTES=OFF` |
| `ENABLE_ABS_MOUSE` | absolute pointer (HID report 3) | `-DENABLE_ABS_MOUSE=OFF` |

The absolute pointer's *form* is a separate setting. By default it is an
ordinary mouse with absolute coordinates on a **HID interface of its own** —
the shape a virtual machine's "absolute pointing device" uses, which every host
drives with a normal arrow cursor:

```bash
cmake .. -DPICO_BOARD=pico2_w -DABS_MOUSE_MODE=1   # digitizer/Pen (Windows: pen cursor)
cmake .. -DPICO_BOARD=pico2_w -DABS_MOUSE_MODE=0   # legacy, shares the mouse collection
```

Mode 1 works on macOS but on Windows a Pen collection is Windows Ink: the arrow
is replaced by a pen cursor and then hidden between reports. See
[docs/SETTINGS.md](docs/SETTINGS.md) for why the interface is separate.

Example — a minimal web-only build with no TCP socket and no IR/RF:

```bash
cmake .. -DPICO_BOARD=pico2_w -DENABLE_TCP=OFF -DENABLE_REMOTES=OFF
```

Dependency: `ENABLE_REMOTES` requires `ENABLE_WEB` (the IR/RF endpoints live in
the HTTP API). If you turn the web server off, remotes are forced off too — both
CMake and `config.h` enforce this. With both servers off the device still works
as a plain USB HID keyboard/mouse; it just has no network control surface.

Each option is mirrored as a `#ifndef`-guarded define in `include/config.h`;
CMake passes the matching `-D` value so the compiled code and the build agree.
