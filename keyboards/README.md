# `keyboards/`

One directory per physical keyboard, the same split QMK uses: the *board*
describes hardware, the *keymap* describes behaviour, and neither knows about
the other.

```
keyboards/<name>/
    keyboard.h              matrix size, pins, diode direction, LAYOUT macro
    rules.cmake             which features this board compiles in
    keymaps/
        default/keymap.cpp  the layers, combos and macros
        <other>/keymap.cpp
```

Build one:

```sh
cmake .. -DPICO_BOARD=pico2_w -DKEYBOARD=proto2x2 -DKEYMAP=advanced
```

Omit `-DKEYBOARD` entirely and NetHID builds exactly as it did before — no
matrix code compiled, no GPIO claimed.

## Adding a keyboard

`keyboard.h` needs `MATRIX_ROWS`, `MATRIX_COLS`, `MATRIX_ROW_PINS`,
`MATRIX_COL_PINS` and `DIODE_DIRECTION`, plus a `LAYOUT()` macro that maps the
physical key order you want to write in to the `[row][col]` array the scanner
reads. Everything else has a default: `DEBOUNCE_MS` (5) and
`MATRIX_IO_DELAY_US` (10).

Check `include/config.h` before choosing pins. GP0/GP1 are the UART console,
GP16/GP17 drive the IR and 433 MHz transmitters, GP19/GP21/GP22 are the
receivers, and GP23–25/GP29 belong to the CYW43. That leaves roughly 21 free —
about a 6×15 matrix. Past that, drive the columns through a 74HC595 chain or a
74HC138 demultiplexer and only spend pins on the read side.

Fit a 1N4148 in series with every switch. Without diodes the matrix ghosts on
any third simultaneous key and none of the tap-hold or combo behaviour can be
trusted.

## Sample boards

| | |
| --- | --- |
| `proto2x2` | four switches on a breadboard; the first thing to build |
| `oledpad` | 12 keys, an encoder and an SSD1306 on one Pico 2 W |
| `mystery6x6` | a 6x6 with an eight-key thumb cluster |
| `modular` | three modules on a serial bus, two of them with displays |

## Modular boards

List modules with `SPLIT_MODULES` in `keyboard.h` and the build emits one small
executable per module — no TinyUSB, no lwIP, no cyw43, so a module runs on any
Pico. Modules can have different pins, different matrix sizes and different key
counts; their rows are laid end to end so the keymap stays one array. See
**[../docs/SPLIT.md](../docs/SPLIT.md)**; `keyboards/modular/` is a worked
example with two 4x6 sides and a 4-key macropad.

### Editing a modular board in the browser

The keymap editor shows a **MODULE** row above the grid when a board has more
than one: `All`, then one chip per module with its size, encoder count and a
status dot. Selecting one narrows the view to that module's rows.

Two behaviours worth knowing, because they differ deliberately:

- In **grid view** the other modules' rows are omitted. The grid has no spatial
  meaning to preserve, so showing them would only be noise.
- In **layout view** they are dimmed instead. That view exists to show where
  keys physically are, and deleting half the board makes the remaining half
  unrecognisable. Clicking a dimmed key selects its module rather than editing
  it blind.

A module that is not answering the bus gets a red dot and a warning. Edits are
still saved to the keymap — that is the right behaviour, since you may be
configuring a module before plugging it in — but the warning is there because
"my changes did nothing" is otherwise a very confusing five minutes.

Checked by `tools/check/check_keymap_ui.sh` (needs `npm i jsdom`), which drives the
real page against a simulated three-module board.

### Exporting a keymap

**Export** on the keymap tab writes a complete JSON snapshot: keys, encoder maps
*and* macros. Import reads it back.

To turn that into a `keymap.cpp`:

```sh
python3 tools/keyboard/json_to_keymap.py nethid-keymap-modular.json -o keymap.cpp
```

The conversion is offline only, and deliberately so: it writes the keymap
through the board's own **`LAYOUT()`** macro, which means reading
`keyboards/<board>/keyboard.h`. The browser has no access to the source tree, so
a C export from the web UI could only ever emit a raw `[row][col]` array —
correct, and nobody wants to edit it. The tool even keeps the macro's original
line breaks, so a split board comes out looking like a split board:

```c
    [0] = LAYOUT(
        KC_TAB , KC_Q , KC_W , KC_E , KC_R , KC_T , KC_Y , KC_U , KC_I , KC_O , KC_P , KC_BSPC,
        KC_ESC , KC_A , KC_S , KC_D , KC_F , KC_G , KC_H , KC_J , KC_K , KC_L , ...
        ...
    ),
```

The board directory is found from the file's `board` field; override with
`--board-dir`. A board with no `LAYOUT()`, or one that cannot be parsed, falls
back to a raw array with row comments rather than guessing.

Keycode names are read out of `NetHID.html` at run time rather than duplicated
in the tool — two hand-maintained tables of 200 entries drift, and the drift is
silent, since an export that names the wrong key still compiles.

Trailing layers that are entirely `KC_TRNS` are left out — the store keeps eight
in RAM so you can add one from the web editor without a reflash, but writing six
empty ones down is noise. Three things are never dropped:

- **Layer 0**, which has nothing to fall through to.
- **An empty layer with a populated one above it.** Layer indices are
  positional, so removing a middle layer shifts everything above it down and the
  keymap silently means something else.
- **A layer something refers to.** `MO(3)` with no layer 3 in the file is not a
  smaller keymap, it is a broken one: the lookup skips layers past
  `keymap_layer_count` and the key quietly does nothing. An empty layer you can
  switch to is a normal thing to have — it is how you make a layer that blocks
  keys.

A layer counts as empty only when its keys *and* its encoder entries are
transparent, so a layer whose only job is to remap a knob survives.

Two things about the generated C worth knowing:

- Macro **delay** steps become a comment, not code. A compiled `kb_macro_user()`
  runs inline on the scan loop, where a blocking wait would stall the matrix.
  Timed sequences want the stored macro, which has a non-blocking interpreter.
- A compiled `kb_macro_user()` only runs for ids with **no stored macro**. Flash
  the exported file while those ids are still saved and the stored versions win.
  Clear them first, or use different ids.

An older version-1 export carried only `layers`. Importing one leaves macros and
encoders alone rather than clearing them: a partial backup should not silently
wipe things it never knew about.

`tools/check/check_export.sh <board>` runs the whole path — the real Export button
under jsdom, the converter, then a compile against the real headers.

### Previewing the editor without a board

```sh
python3 tools/web/preview.py --serve
```

Writes `preview.html` — `NetHID.html` with a mock API layer injected — and
serves it on :8080. The page runs exactly as it would against a Pico: same code
paths, same render functions, same bugs if there are any. From the browser
console:

```js
mock.boards()                    // modular, oledpad, mystery6x6
mock.board('oledpad')            // switch fixture
mock.online(2, false)            // take a module off the bus
mock.set('tapping_term_ms', 250)
mock.apMode(true)                // pretend to be in WiFi setup mode
mock.log()                       // every request the page has made
```

The mock lives in `tools/web/preview/mock.js` and is injected into a **copy**, never
into `NetHID.html` itself: every byte of that file is embedded in the firmware as
a C string literal, and a preview harness has no business taking up flash on a
keyboard. `preview.html` is generated output and is gitignored.

The fixtures are only as good as their fidelity — a response shape that has
drifted from the firmware shows you a UI that cannot exist. `tools/check/check_preview.sh`
asserts the page loads, the fixtures differ, and the console helpers reach the
real render code rather than a parallel implementation.

## Adding a keymap

Copy `keymaps/default/` and edit. A keymap must define:

```c
const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = { ... };
const uint8_t      keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);
```

and, when `KB_FEATURE_COMBO` is on, `kb_combos[]` and `kb_combo_count` (both
may be empty).

**Include `kb/kb.h` first.** In C++ a `const` at namespace scope has internal
linkage, so without the extern declaration that header provides, your keymap
compiles cleanly and then fails to link.

## Features

Each feature is one file under `src/kb/features/` and one flag. A disabled
feature is not compiled, not linked, and absent from the dispatch table in
`include/kb/features.h` — no dead branch survives.

| Flag | File | Keycodes |
| --- | --- | --- |
| `KB_FEATURE_LAYERS` | `layers.cpp` | `MO()` `TO()` `TG()` `DF()` |
| `KB_FEATURE_TAPPING` | `tapping.cpp` | `MT()` `LT()` `SFT_T()` `CTL_T()` … |
| `KB_FEATURE_ONESHOT` | `oneshot.cpp` | `OSM()` `OSL()` |
| `KB_FEATURE_COMBO` | `combo.cpp` | — (table-driven) |
| `KB_FEATURE_CAPS_WORD` | `caps_word.cpp` | `CAPSWRD` |
| `KB_FEATURE_MACROS` | `macros.cpp` | `KB_MACRO(n)` |
| `KB_FEATURE_MOUSEKEYS` | `mousekeys.cpp` | `MS_UP` `MS_BTN1` `MS_WHLD` `MS_ACL0` … |
| `KB_FEATURE_CONSUMER` | `consumer.cpp` | `KC_MUTE` `KC_VOLU` `KC_MPLY` … ([docs](../docs/MEDIA_KEYS.md)) |
| `KB_FEATURE_AUTOCLICK` | `autoclick.cpp` | `AUTOCLK(n)` ([docs](../docs/AUTOCLICK.md)) |
| `KB_FEATURE_MACRO_STORE` | `macro_store.cpp` | — (bodies for `KB_MACRO(n)`) |
| `KB_FEATURE_BOOTMAGIC` | `bootmagic.cpp` | `QK_BOOT` |
| `KB_FEATURE_DYNAMIC_KEYMAP` | `keymap_store.cpp` | — (web UI) |

Set defaults per board with `kb_default(...)` in `rules.cmake`; the command
line always wins over it.

**Every feature here defaults to OFF.** A board that does not name it in its
`rules.cmake` does not get it, and its keycodes then do nothing — they are still
offered by the web keymap editor, still stored, still drawn on the key. That is
what `features` in `/api/keymap/info` is for: the editor hides or warns about
keycodes this firmware cannot act on, so a dead key says so instead of being a
mystery. If you add a feature to a board, nothing else needs changing — the
editor picks it up on the next load.

`KB_FEATURE_AUTOCLICK` additionally needs the board to declare slots
(`NUM_AUTOCLICKS` and `AUTOCLICKS` in `keyboard.h`, see `include/kb/autoclick.h`).
Turning the feature on without them is a configure-time error rather than a
firmware that silently ignores `AUTOCLK(n)`. The declared slots are the
*defaults*: target, rate and trigger are editable at runtime from the AUTOCLICK
panel in the web UI and persist to flash — see [docs/AUTOCLICK.md](../docs/AUTOCLICK.md).

`BOOTMAGIC` and `DYNAMIC_KEYMAP` are the two entries that aren't part of the
process chain — it hooks
boot and a single keycode rather than key events, so it has no `_process` hook
and doesn't appear in `KB_FEATURE_LIST`.

Processing order is fixed and explicit in `include/kb/features.h` — combos
must see raw presses before anything defers them, one-shots must claim the
next key before the tapping machine can hold it back, and so on. Adding a
feature means writing the file, adding a guard block at the right point in
that list, and adding one line to `cmake/keyboard.cmake`.

## Mouse keys

`KB_FEATURE_MOUSEKEYS` drives the pointer from the matrix — relative motion,
wheel, and five buttons. Same HID report NetHID already sends over the network,
just from a different source.

| | |
| --- | --- |
| `MS_UP` `MS_DOWN` `MS_LEFT` `MS_RGHT` | cursor |
| `MS_WHLU` `MS_WHLD` `MS_WHLL` `MS_WHLR` | wheel and horizontal pan |
| `MS_BTN1` … `MS_BTN5` | left, right, middle, back, forward |
| `MS_ACL0` `MS_ACL1` `MS_ACL2` | hold for a fixed slow / medium / fast speed |

Motion emits one step immediately, waits `MOUSEKEY_DELAY` (12 ms), then repeats
every `MOUSEKEY_INTERVAL` (16 ms ≈ 60 Hz) with speed ramping from
`MOUSEKEY_BASE_SPEED` to `MOUSEKEY_MAX_SPEED` over `MOUSEKEY_TIME_TO_MAX`
(350 ms). The immediate first step is what makes a tap a single nudge instead of
a skate; the ramp is what makes the same key usable for both "one pixel left"
and "cross a 4K display". Every tunable is a `#define` you can override in
`keyboard.h`.

Diagonals are scaled by 1/√2 in integer arithmetic, so holding right and down
does not travel 1.41× faster than either axis alone. The wheel runs on its own
much slower timer, because it is discrete.

Holding several accelerator keys picks the *slowest*, on the grounds that you
reach for `MS_ACL0` when you are trying to land on something small.

`keymaps/default/` has a worked example: a `MOUSE` layer latched from `NAV` with
`TG(MOUSE)` — latched rather than momentary, because pointing takes both hands
and a while, and a held layer key means occupying a finger for the whole
gesture.

### One caveat

There is no source merge on the mouse endpoint, unlike the keyboard. Motion is
genuinely event-based so there is nothing to merge, but the button bits *are*
state — which means a network mouse report arriving mid-drag will clear a
physically held button. In practice you are not doing both at once; if that ever
matters, the fix is the same `keystate.cpp` treatment applied to the button
byte.

Absolute positioning (`HID_CMD_ABS_REPORT` already exists in the HID layer) is
not wired up yet.

## Building macros in the web UI

`KB_MACRO(n)` on its own needs C in your `keymap.cpp` and a reflash.
`KB_FEATURE_MACRO_STORE` gives those same keycodes a body you can build in the
browser — the same step vocabulary the custom-button tab uses, plus the two
halves of a tap that a physical key needs:

| step | |
| --- | --- |
| **tap** | press modifiers + key, release both |
| **hold** | press and keep held |
| **release** | let go |
| **text** | type a string, up to 64 characters |
| **delay** | wait, 0–65535 ms |

A **MACROS** section appears under the keymap grid: 16 slots, a step list per
slot with reorder and delete, and a live byte count against the pool. Edits are
live immediately; **Save macros to flash** persists them in their own sector,
one below the keymap's.

Assign one to a key with the `KB_MACRO(n)` type in the key picker.

### Resolution order

A macro id resolves to a stored body first, then to `kb_macro_user()`, then to
nothing. Stored wins so that editing in the browser takes effect without a
reflash, which is the whole point. An id you want permanently owned by C is
simply one you never create a stored macro for — and C remains the answer for
anything a step list cannot express, like firing an IR code or reading a sensor.

### How it runs

The interpreter is a state machine advanced once per scan on core 1, because a
`sleep()` there would stall USB and the matrix together. Three things it waits
on:

- **a tap needs two reports.** A press and its release inside one scan collapse
  into a single composed report and the host never sees the key at all, so a tap
  presses, waits for `hid_task()` to send, then releases. Same problem
  `tapping.cpp` solves the same way.
- **text goes through the shared typer**, which drives the endpoint directly
  while the state composer stands aside. So modifiers you are holding do *not*
  apply to a text step, and a physically held key is released for its duration.
- **delays** are just a timestamp.

Every wait has a timeout, so an unplugged or suspended host cannot wedge a
macro mid-run.

One macro runs at a time. A second macro key pressed mid-run is ignored rather
than queued or preempting — queueing invites a pile-up from a key that chatters,
and preempting strands the first macro's held keys.

Anything still held when a macro ends is released for you, so an unbalanced
**hold** cannot leave a key stuck down. A macro also never releases a modifier
you were already physically holding when it started: `keystate` derives its
modifier byte from held usages, so a blind release would steal your shift
mid-word.

### Bytecode

Steps are assembled into bytecode server-side and **verified before storing** —
opcodes known, operands in range, terminated, self-contained. That verification
is what lets the interpreter's hot path do no bounds checking at all. Bodies are
re-verified on load from flash too, since a truncated write that still passes
CRC would otherwise hand the interpreter a malformed program.

Worth knowing if you extend the format: `MOP_END` is `0x00`, and `0x00` is also
an ordinary operand — "no key", "no modifiers", the low byte of a sub-256 ms
delay. Program length must be found by *walking* opcodes, never by scanning for
the first zero byte.

### API

| | |
| --- | --- |
| `GET /api/macro` | every macro as a step list, plus pool usage |
| `POST /api/macro` | `{"id":n,"steps":[…]}` — live immediately |
| `POST /api/macro/save` | persist to flash (queued) |
| `POST /api/macro/clear` | delete every macro |

## Bootloader access without the BOOTSEL button

Two routes in, both from `KB_FEATURE_BOOTMAGIC`:

**Bootmagic** — hold a key while plugging the board in. Set the position in
`keyboard.h`:

```c
#define BOOTMAGIC_ROW 0
#define BOOTMAGIC_COL 0
/* both must be held, if you want two: */
/* #define BOOTMAGIC_ROW_2 3 */
/* #define BOOTMAGIC_COL_2 5 */
```

The check runs on core 0 as the very first thing `main()` does — before USB,
before WiFi, before `auth_init()`. That ordering is the point: a firmware that
hangs during WiFi association is still recoverable. The key must read as down
across all `BOOTMAGIC_SCANS` (8) scans spread over ~32 ms, so a bouncing switch
or a floating line on a cold rail can't trigger it by accident.

**`QK_BOOT`** — a keycode you can put on a layer, usually behind a momentary
layer so it can't be hit by mistake. Core 1 only raises a flag; the reset
itself happens in core 0's loop via `kb_bootloader_poll()`, because
`rom_reset_usb_boot()` tears the chip down under the calling core and doing
that from core 1 mid-way through a cyw43 SPI transaction hangs instead of
rebooting.

Neither path needs the LED: it hangs off the CYW43, which isn't powered yet at
bootmagic time.

## Working out an unknown matrix

`keymaps/debug/` makes every switch type its own coordinates — press a key, get
`r2c3 `. Walk the board in physical order and the output *is* the layout.

```sh
cmake .. -DPICO_BOARD=pico2_w -DKEYBOARD=mystery6x6 -DKEYMAP=debug \
         -DKB_DEBUG_MATRIX=1
```

`-DKB_DEBUG_MATRIX=1` additionally logs every debounced edge to the UART as
`[matrix] r2 c3 down`. Prefer the UART log where you have it: it happens
*before* the keymap, so it reports positions that are mapped to nothing, and
one press producing several edges is a missing diode announcing itself.

Two things the debug keymap does deliberately:

- It writes the `[row][col]` array directly instead of going through
  `LAYOUT()`. `LAYOUT()` is a *claim* about how physical positions map to the
  matrix, and that claim is what's under test — routing the debug keymap
  through it would let a wrong macro conceal the wiring it exists to reveal.
- It maps all `MATRIX_ROWS × MATRIX_COLS` positions, including ones you believe
  are unpopulated. A key that types `r5c4` when row 5 was supposed to hold two
  keys is exactly the discovery you want, and a keymap that omits those slots
  can never make it.

Once you know the truth, fix `LAYOUT()` in `keyboard.h` and lock it in:

```sh
cd tools/kbtest && make layout_check KB=mystery6x6 && ./layout_check
```

That fills the macro with one sentinel per position and prints where each
lands, so a later edit can't silently scramble every keymap at once.

## Editing the keymap from the web UI

With `KB_FEATURE_DYNAMIC_KEYMAP` the compiled keymap becomes a *default*: it is
copied into RAM at boot, the scanner reads the RAM copy, and a **KEYMAP** tab
appears in the web UI with a layer picker and a grid of keys. Click a key, pick
a keycode, and it is live on the next press — no recompile, no reflash, no host
application. This is the part QMK needs VIA or Vial for; the server is already
running, so the editor is just another tab.

The tab and the API disappear entirely when the feature is off.

### What the picker offers

The whole Keyboard/Keypad usage page, so every key on a full-size board can be
assigned — grouped, because a flat list of 140 entries is not a list anyone
reads:

| | |
| --- | --- |
| KEYBOARD | letters, digits, punctuation, F1-F24 |
| KEYPAD | the number pad as its own usages — `P0`-`P9`, `PSLS`, `PAST`, `PMNS`, `PPLS`, `PENT`, `PDOT`, `PEQL`, `PCMM` |
| NAVIGATION | arrows, Home/End, Page Up/Down, Insert/Delete, PrtSc, ScrLk, Pause |
| MODIFIERS | both sides of Ctrl/Shift/Alt/GUI |
| EDIT & SYSTEM | Undo, Cut, Copy, Paste, Find, Again, Select, Stop, Execute, Help, Menu, App, and the keyboard-page power/volume usages |
| INTERNATIONAL | `NUHS` and `NUBS` for ISO, and `RO`/`KANA`/`JYEN`/`HENK`/`MHEN`/`HAEN`/`HANJ` for JIS and Hangul boards |

Search matches the words you would actually type, not just the name: "numpad"
finds the keypad group, "hangul" and "iso" find the international one, "paste"
finds `PSTE`.

**The number pad is not the digit row.** `P5` (usage 0x5D) and `5` (0x22) are
different keys to a host, and software that cares — spreadsheets, CAD, remote
desktop, anything binding "numpad 5" — sees them as such.

**`KB_MUTE`/`KB_VOLU`/`KB_VOLD` are not the media keys.** Those are the
Keyboard-page volume usages, which most modern hosts ignore. The MEDIA group's
`KC_MUTE`/`KC_VOLU`/`KC_VOLD` are Consumer Control usages on report 4 and are
what actually changes the volume. Both exist because the usage table has both;
the names are deliberately distinct so one cannot quietly shadow the other.

Three tables have to agree for a key to work end to end: what the editor
offers, what Export C writes, and what `include/kb/keycodes.h` compiles. They
are checked against each other by `tools/check/check_keycode_tables.py`,
because a key you can assign but cannot export — or export but not compile — is
worse than one that is missing. It looks like it worked.

### Drawing your actual keyboard

By default the editor shows a rows×cols grid, which is honest about the wiring
and useless for finding a key with your eyes. Give the board a physical layout
and it draws the real thing instead.

In `keyboard.h`, optionally:

```c
#define LAYOUT_GEOMETRY                              \
    KB_KEY(0,0,     0,   0)                          \
    KB_KEY(0,1,   100,   0)                          \
    KB_KEY_S(3,0,   0, 300, 225, 100)                \  /* 2.25u */
    KB_KEY_R(4,2, 500, 400, 100, 100, -15, 450, 400)     /* rotated thumb */
```

Hundredths of a key unit, origin top-left, y downward, rotation in whole degrees
about `(rx, ry)` — deliberately the same conventions as
keyboard-layout-editor.com. It compiles to a `const` table, so it lives in
flash, which is the right place for something that stops changing the moment the
board is screwed together.

Don't type that by hand past a macropad:

```sh
# lay the board out at keyboard-layout-editor.com, put "row,col" in each key's
# TOP-LEFT legend, copy the Raw Data to layout.json, then:
python3 tools/keyboard/kle_to_layout.py layout.json
```

Paste the result into `keyboard.h`. The `row,col` legend convention is the one
VIA and Vial use, so an existing VIA layout for your board works unmodified.

It runs backwards too, which is how you edit an existing layout:

```sh
python3 tools/keyboard/kle_to_layout.py --to-kle keyboards/mystery6x6/keyboard.h
# paste into the Raw data box, drag keys around, copy back out, run forwards
```

The round trip is lossless — worth knowing, because KLE's raw format is a
*cursor* rather than absolute coordinates (y advances by one per row, `x`/`y` are
deltas applied to that cursor), and an emitter that treats it as absolute
produces output that looks plausible while every key drifts a little further
than the last.

Two behaviours worth knowing:

- Positions that are wired but absent from `LAYOUT_GEOMETRY` appear in a
  separate strip below the board, flagged. They stay editable — which matters
  most exactly while you are still getting the geometry right.
- The layout is optional and the grid remains one button away, which is still
  the better view for spotting an unassigned position.

`tools/kbtest`'s `layout_check` prints the exact JSON the endpoint will serve,
along with how much of its buffer it used:

```sh
cd tools/kbtest && make layout_check KB=mystery6x6 && ./layout_check
```

### API

| | |
| --- | --- |
| `GET /api/keymap/info` | dimensions, layer counts, whether flash holds a keymap |
| `GET /api/keymap/layout` | physical geometry, empty if the board defines none |
| `GET /api/keymap/<n>` | one layer's keycodes as a flat row-major array |
| `POST /api/keymap` | `{"layer":n,"keys":[…]}` — applies immediately |
| `POST /api/keymap/save` | persist to flash (queued) |
| `POST /api/keymap/reset` | `{"erase":bool}` back to the compiled keymap |

All of it sits behind the existing session auth. A layer at a time, not the
whole keymap: `HTTP_RECV_BUF` is 4096 bytes and one layer of a full-size board
is a few hundred, so this stays comfortably inside the buffer on any matrix
worth building. A partial layer is rejected outright rather than half-applied.

`KB_MAX_LAYERS` (8 by default) is how many layers exist in RAM, independent of
how many the compiled keymap defines. The extra ones start fully transparent, so
adding a layer in the UI can't silently mask the one underneath.

### Two things worth understanding before you rely on it

**Editing is free; saving is not.** Edits only touch RAM. Persisting means
erasing a flash sector, and on RP2350 that takes XIP down — so core 1, which is
running USB and the scanner out of flash, has to be parked in RAM first.
`kb_init()` calls `multicore_lockout_victim_init()` for exactly this, and the
write is performed from core 0's main loop rather than from the lwIP callback
that requested it. For the ~50–100 ms it takes, USB isn't serviced and
interrupts are off: the host sees the keyboard go quiet and a WiFi packet or two
may be dropped. TCP and TinyUSB both recover. It only happens when someone
presses Save.

**There is no lock on the keymap.** Core 1 reads it every scan while core 0
writes it from the web handler. A `uint16_t` store to an aligned address is a
single instruction on Cortex-M33, so no reader can see half a keycode; the worst
case is a keypress landing in the same microsecond as an edit resolving to
either the old or the new value — which is what a lock would have given you
anyway. Taking a spinlock on the scan hot path to buy nothing would be the wrong
trade.

### If a stored keymap gets in your way

The stored blob carries a fingerprint of the compiled keymap it was derived
from, and a mismatch discards it. This matters more than it sounds: without it,
you edit `keymap.cpp`, rebuild, flash — and the blob still matches on dimensions
and still passes CRC, so it silently wins and half your keys are wrong with
nothing on screen to explain why. A freshly flashed keymap beating a remembered
edit is the safe default. `-DKB_KEYMAP_KEEP_ON_REFLASH=1` inverts it if you
prefer, and `-DKB_KEYMAP_IGNORE_STORED=1` ignores flash entirely for a build.

Either way the UART says which one is live on every boot:

```
[keymap] active source: COMPILED (keymap.cpp) (6x6, 8 layers in RAM)
```

The stored blob also carries the matrix dimensions and a CRC32, both checked on load.
Flashing a different board over the top leaves a valid-looking sector behind,
and reinterpreting those bytes as a keymap of the wrong shape would be a
memorable afternoon.

### Editing the UI

`include/web_html.h` is generated — don't hand-edit it. The keymap tab lives in
`NetHID.html` between its `<!--@@TAB keymap@@-->` markers:

```sh
# edit NetHID.html, test it in a browser, then:
python3 tools/web/build_web_html.py --main NetHID.html
```

## Testing without hardware

`tools/kbtest/` fakes the GPIO layer and the clock and runs the real pipeline
on your PC:

```sh
cd tools/kbtest && make && ./kbtest
```

It asserts the exact byte sequence the host would receive for taps, holds,
chords, one-shots, and the matrix/network merge, and — with a fake flash sector
— that a runtime keymap edit takes effect, survives a simulated reboot, and that
a blob of the wrong shape is refused. Run it before flashing.

`make layout_check KB=<name>` is a separate, much smaller check of a single
board's `LAYOUT()` macro — see above.
