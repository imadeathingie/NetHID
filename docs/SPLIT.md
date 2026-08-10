# Modular keyboards

One primary runs all of NetHID. Any number of **modules** each run a small
firmware that scans their own hardware and answer when polled over a serial bus.

Modules are not "halves" and need not resemble each other. A 4x6 left side, a
4x6 right side with different pins, and a 1x4 macropad are three modules on the
same bus.

```sh
cmake .. -DPICO_BOARD=pico2_w -DKEYBOARD=modular -DKEYMAP=default
make                    # nethid.uf2          -> primary, module 0 (Pico 2 W)
make nethid_module1     # nethid_module1.uf2  -> module 1 (any Pico)
make nethid_module2     # nethid_module2.uf2  -> module 2 (any Pico)
```

A module links no TinyUSB, no lwIP and no cyw43, so it is happy on a plain Pico
or a Pico 2. It has no keymap and does not know what any of its switches mean —
which is why it needs none of the firmware that does.

## Declaring the modules

In `keyboard.h`:

```c
#define SPLIT_MODULES        \
    SPLIT_MODULE(0, 4, 6)    /* id, rows, cols */ \
    SPLIT_MODULE(1, 4, 6)    \
    SPLIT_MODULE(2, 1, 4)

#define MATRIX_ROWS 9        /* 4 + 4 + 1 */
#define MATRIX_COLS 6        /* the widest module */
#define SPLIT_PRIMARY_ROWS 4
```

Rows are laid end to end in that order: module 0 owns rows 0-3, module 1 rows
4-7, module 2 row 8. The keymap stays one array and nothing above the matrix
layer — layers, combos, the web editor, `LAYOUT_GEOMETRY` — knows the keyboard
is in pieces. A combo can span modules and behaves exactly like a local one.

The build detects `SPLIT_MODULES` by reading the header and generates one target
per non-zero id, so the ids the primary polls and the firmwares you can build
come from the same list and cannot drift apart. Module 0 is always the primary
and is never polled — it scans its own GPIO.

A narrower module simply leaves its high columns unused. Every module speaks the
board's `SPLIT_ROW_BYTES`, so a 4-column module wastes a few bits per frame in
exchange for one wire format everywhere.

## Per-module hardware

Each module gets its own header under `modules/`, because different hardware is
the entire point:

```
keyboards/modular/modules/module0.h    pins for the primary
keyboards/modular/modules/module1.h    different pins, same shape
keyboards/modular/modules/module2.h    1 row, 4 columns
```

Each defines `MATRIX_ROW_PINS`, `MATRIX_COL_PINS` and `SPLIT_MODULE_ROWS`. The
build selects one via `SPLIT_MODULE_ID`.

Adding a module is one line in `SPLIT_MODULES`, one header, and a row in the
keymap.

## Wiring

Three conductors for the bus, however many modules you have: one line out from
the primary, one shared line back, and ground.

```
                         PRIMARY (Pico 2 W)
                    TX GP20 ----+       +---- GP21 RX
                                |       |
                                |       +--------------+------------+
                                |                      |            |
                                |       +--------------+            |
                                |       |     10k pull-up to 3V3    |
                                |       |     (fit ONE, anywhere)   |
        +-----------------------+       |                           |
        |  (broadcast: every module     |  (shared: every module    |
        |   listens to this line)       |   drives this one)        |
        |                               |                           |
   +----+----+----------------------+---+---+                       |
   |         |                      |       |                       |
 RX GP21   TX GP20               RX GP21  TX GP20 ------------------+
   MODULE 1                         MODULE 2
   (any Pico)                       (any Pico)

   GND ------------------- common to every board -------------------
```

Two things people get wrong here:

**TX goes to RX, not to TX.** The primary's TX is a broadcast — one driver, many
listeners — so every module's RX ties to it. The return path is the reverse:
every module's TX ties together into the primary's RX.

**Fit a pull-up on the shared return line.** 10k to 3V3, one of them, anywhere
on the line. Between transmissions no module drives it, and a floating UART
input reads noise as start bits — you get phantom frames rather than silence.
Modules idle their transmitter in high-Z and only take the line while answering,
which is what makes tying them together safe in the first place.

Defaults are `uart1`, GP20 for TX and GP21 for RX, on every board including the
modules. Override per board in `keyboard.h`.

### Pin budget

On a Pico W, `uart1` can only use GP4/5, GP8/9, GP20/21 or GP24/25, and GP24/25
belong to the CYW43. That is a tighter constraint than it looks, and **GP21 is
`IR_RX_PIN`** in `include/config.h`. The `modular` example resolves it by
switching the IR/RF features off — a modular keyboard is not usually also a
remote control — which its `rules.cmake` says explicitly.

Run this after touching any pin assignment:

```sh
python3 tools/check/check_pins.py
```

A pin collision does not fail to build. It produces hardware that half works: a
display that dies the moment the firmware prints something, a matrix column that
reads as pressed whenever the IR receiver sees light. The checker knows which
pins the console UART, the CYW43 and each feature claim, and that a module
firmware links no IR/RF code so does not claim those pins at all. It found two
real collisions in this project's own example board.

### Power

Each module needs 3V3 and ground. Options, in order of preference:

1. **Its own USB cable.** Simplest, and the only option that is safe to hot-plug.
2. **A fourth conductor carrying 3V3** from the primary, if the modules are
   light and permanently connected.

**Do not put VBUS (5 V) on a connector you unplug while powered.** A TRRS jack
drags its contacts across each other as it slides in, briefly shorting whatever
is on the tip to whatever is on the sleeve. Shorting 5 V to ground through a
connector is the single most common way modular keyboards die. If you must
share power, share 3V3, and accept that hot-plugging is still a bad idea.

Ground must be common to every board regardless of how they are powered — a
UART with no shared ground reference does not work, and the failure is
intermittent rather than obvious.

### Cable

Any three-conductor cable. TRRS is traditional because the jacks are cheap and
the cables are everywhere; it gives four conductors, so one spare. Use the spare
as a second ground rather than for power.

If the modules are more than about half a metre apart, or the run passes
anything noisy, keep `SPLIT_UART_BAUD` at 115200 and use a shielded cable with
the shield on ground. Raising the baud rate is the last thing to try, not the
first.

## Why the bus is polled

## Why the bus is polled

The primary is bus master and a module transmits only in response to its own
address, so collisions are impossible by construction rather than by good luck.
The alternative on a shared wire is modules talking over each other, and a
collision does not look like silence — it looks like a corrupted matrix row.

The cost is latency: a full cycle is one poll per module. At 115200 a poll and
reply are well under a millisecond each, so a handful of modules is fine.
Raising `SPLIT_UART_BAUD` is where a long bus with many modules earns its keep —
measure before you do.

## Protocol

```
0xA5  addr  type  len  payload[len]  crc8
```

`addr` is the module the frame concerns. A reply carrying the wrong address is
discarded rather than applied to whichever slot is being polled — on a shared
line that is the difference between "a module is missing" and "the macropad's
keys appear on the left hand".

Sync byte because a UART just powered up, or a jack being pushed in, delivers
partial frames. CRC8 because a marginal cable corrupts bytes rather than losing
whole frames.

**Modules send complete state, never events.** With events, one lost frame
desyncs a module permanently: a key held until you unplug something. With state,
the next poll is a full correction. Same reasoning as the source merge in
`keystate.cpp`.

**A reply of the wrong size is refused.** That means a module is running firmware
built for a different shape, and applying it would scatter its keys across the
matrix. It stays offline until reflashed, and says so on the console.

**Losing a module releases its keys, and only its.** After
`SPLIT_MISSED_POLLS` (8) unanswered polls its rows are zeroed. Timing out in
missed polls rather than milliseconds matters: a fixed timeout generous with two
modules is trigger-happy with six.

## Sensors

`SPLIT_MSG_SENSOR` is reserved in the protocol with a typed payload, so encoders
and pointing devices can be added without changing the framing or touching the
matrix path. Not implemented yet.

## Debounce lives on the primary

Modules send **raw** scanned state. Debounce runs once, on the primary, over the
combined matrix — one implementation and one set of timings for every module.
Debouncing on each module as well would add its `DEBOUNCE_MS` to the primary's,
so modules would have measurably different latency from each other and only the
primary's would be tunable.

## Boot keys

`BOOTMAGIC`, `QUIET_BOOT`, `LOUD_BOOT` and `AP_MODE` are sampled before the bus
is up, so they must be on module 0. Keys on any other module cannot work: the
firmware that would read them has not started talking yet.

## Testing

```sh
cd tools/kbtest && make split_test && ./split_test
```

Runs the real primary against a simulated bus with two modules of different
shapes, configurable per-byte loss and corruption, and checks that rows land at
the right offsets, a wrong-addressed reply is never applied, a wrong-sized reply
is refused, losing one module leaves the others alone, and that a 5% lossy, 5%
corrupting bus recovers on its own.
