# Status display (SSD1306 / SH1106)

A 128×64 or 128×32 I2C panel showing layer, modifiers, connection state, WPM and
which modules are answering. Works on the primary and on any module.

```c
/* keyboard.h, or a module header so only modules with a panel pay for one */
#define OLED_ENABLE   1
#define OLED_I2C_INST i2c1
#define OLED_SDA_PIN  2
#define OLED_SCL_PIN  3
#define OLED_HEIGHT   64
#define OLED_TITLE    "MODULAR"
```

GP2/GP3 is a valid `i2c1` pair and is clear of the matrix, the split bus and the
encoder on the example board. Internal pull-ups are enabled; most panel modules
also have their own.

`OLED_ENABLE` is read by CMake at configure time — from `keyboard.h`, or from
`modules/module<id>.h` for a module — and passed to the compiler as `-D`. It is
not left to the header alone, because it guards the entire body of `oled.cpp`
and `oled_status.cpp`, and those files include `oled/oled.h` (which defaults it
to 0) long before anything pulls in a board header. When it depended on include
order the whole driver quietly compiled to nothing on every board. Configure
prints what it decided:

```
-- NetHID: OLED status display
-- NetHID: module targets: nethid_module1;nethid_module2
```

If that first line is missing, the panel is off no matter what the header says —
check the `#define OLED_ENABLE 1` is literally that, since CMake matches it with
a regex.

## Sample boards

**`keyboards/oledpad/`** — the simplest thing that exercises a display. One
Pico 2 W, twelve keys, one encoder, one SSD1306. Not split, so there is no bus,
no module headers and no second firmware:

```sh
cmake .. -DPICO_BOARD=pico2_w -DKEYBOARD=oledpad -DKEYMAP=default
make
```

Wiring is in the header: matrix rows GP4-6 and columns GP8-11, display SDA GP2 /
SCL GP3 on `i2c1`, encoder A/B/push on GP26/27/28. Nothing collides with the
IR/RF pins or the console UART, so the remote features can stay on.

The keymap puts `MO(FN)` on the bottom-right key. Hold it and watch the layer
indicator change — that is the quickest way to confirm the status snapshot is
actually reaching the renderer.

**`keyboards/modular/`** — displays on two of three modules. Module 0 (the
primary) has a 128x64 panel on `i2c1`; module 2, the macropad, has a 128x32
strip on `i2c0`; module 1 has none.

That last part matters and is why displays are declared in `modules/moduleN.h`
rather than the shared `keyboard.h`: a shared define would build the driver and
font into module 1 for nothing, and have it initialise an I2C bus on pins that
module does not have wired. Module 2 also shows that the renderer adapts —
`oled_render_status()` checks `OLED_HEIGHT` and drops the WPM and module lines
on a 32-pixel panel rather than drawing off the bottom.

## The timing problem, and what solves it

A 128×64 framebuffer is 1024 bytes — about **25 ms** of I2C at 400 kHz. Pushed
synchronously from the core that scans the matrix, that is 25 ms of no scanning
every time the screen changes. Dropped keystrokes that would look exactly like a
flaky USB problem, which this project has already chased once.

Three things prevent it:

- **Only changed pages are sent.** The panel is eight 128-byte pages. A status
  screen touches one or two, so a typical update is ~3 ms.
- **`oled_task()` sends at most one page, then returns.** It is a state machine,
  not a loop, so no single call can stall its caller.
- **`oled_pixel()` marks a page dirty only on an actual change.** A redraw that
  produces identical pixels costs *nothing* — which is what makes rendering on
  every pass of the main loop affordable rather than needing change detection
  above it.

On the primary it runs on **core 0** beside lwIP, which has slack. Core 1 does
nothing but USB and the matrix and is never asked to wait for a display. On a
module there is one core, but there is also nothing else on it — and the flush
happens *after* the reply to a poll, never before, because the primary is
waiting on that frame.

## How a module knows what to draw

A module has no keymap. It cannot work out what layer 2 means, so it can only
show what the primary tells it.

Shipping a framebuffer is not an option — 1 KB against a bus budgeting a few
dozen bytes per poll. Instead a packed 6-byte `kb_status_t` rides **in the
payload of the poll frame the primary already sends every cycle**. No extra
traffic, every module refreshed every poll, and both ends render through the
same `oled_render_status()`, so a module shows exactly what the primary shows.

```c
typedef struct {
    uint8_t  layer, mods, flags, wpm;
    uint16_t modules;      /* bit n = module n online */
} kb_status_t;
```

Keep it small: every byte added is paid on every poll to every module.

On the primary the same struct crosses cores instead — published by core 1,
read by core 0. No lock: every field is a byte or an aligned `uint16_t`, so no
reader can see a torn value, and the worst case is a frame rendered from a
snapshot one scan old. A spinlock on the scan hot path to stop a display being
1 ms stale would be the wrong trade.

## SH1106

Sold as SSD1306 constantly. It is 132 columns wide with the visible 128 offset
by two, so a driver that does not know produces a picture shifted two pixels
with a wrapped stripe down one edge. If that is what you see:

```c
#define OLED_SH1106 1
```

## Burn-in

`OLED_TIMEOUT_MS` (10 minutes) blanks the panel after no keypress. A static
layer indicator shown eight hours a day is exactly the pattern that burns in.
The framebuffer is untouched while blanked, and the first keypress redraws
everything. 0 disables.

## The font

`include/oled/oled_font.h` is generated by `tools/keyboard/mkfont.py` from pixel art, so
the glyphs are readable and checkable at source rather than a wall of hex nobody
can verify:

```sh
python3 tools/keyboard/mkfont.py --check     # render a few back as ASCII art
python3 tools/keyboard/mkfont.py             # regenerate the table
```

Lowercase folds onto uppercase — a status line reads fine in caps. Characters
with no glyph render blank, because a gap in the text is a legible bug and a
random block is not.

## Changing what is shown

Everything that decides content lives in `oled_render_status()` in
`src/oled/oled_status.cpp`. The driver knows how to put pixels on a panel and
nothing about keyboards; that file knows about keyboards and nothing about I2C.
Making the content configurable later means replacing one function rather than
unpicking the driver.

## Testing

```sh
cd tools/kbtest && make oled_test && ./oled_test
```

`make layout_check KB=oledpad && ./layout_check` separately validates a board's
`LAYOUT_GEOMETRY` — positions inside the matrix, none placed twice — and prints
the exact JSON the web editor will receive. It no longer checks `LAYOUT()`
argument counts: that varies per board and the compiler already rejects a short
row with a clear message, whereas a tool needing an edit for every new board is
a tool nobody runs.

`oled_test` counts what actually reaches the wire and checks the properties that matter: a
one-line change flushes one page not eight, an identical redraw sends nothing at
all, a single `oled_task()` call never sends more than one page, off-panel
pixels are clipped rather than wrapped, and the status struct survives a round
trip through the wire format — a mismatch there is a module confidently showing
the wrong layer.
