# Autoclick

Repeat a key or a mouse button at a fixed rate. `KB_FEATURE_AUTOCLICK`.

```c
/* keyboard.h */
#define NUM_AUTOCLICKS 3
#define AUTOCLICKS                                  \
    AUTOCLICK(0, MS_BTN1, 100, AC_HOLD)             \
    AUTOCLICK(1, KC_SPC,   50, AC_TAP2)             \
    AUTOCLICK(2, MS_BTN1,  25, AC_TAP3 | AC_HOLD)
```

`AUTOCLK(0)` in the keymap selects a slot. In the web editor it is both an
**AUTOCLICK** group in the key picker's searchable list and an **autoclick slot**
type in the advanced dropdown; it exports as `AUTOCLK(n)`.

The table above is the **default**, not the definition. Slots are editable at
runtime from the **AUTOCLICK** panel under the keymap editor — target, rate and
trigger per slot — and persist to flash as a separate explicit step, exactly
like the dynamic keymap. See [Editing slots at runtime](#editing-slots-at-runtime).

Turning the feature on without declaring slots is a **configure-time error**, not
a firmware that silently ignores `AUTOCLK(n)`:

```cmake
# keyboards/<board>/rules.cmake
kb_default(KB_FEATURE_AUTOCLICK ON)
```

The target is an ordinary keycode, so anything the feature chain already
understands works: a mouse button, a letter, `LCTL(KC_V)`, a macro — including
the [media keys](MEDIA_KEYS.md). Autoclick re-enters the chain from the top to
emit it and does not know which, which is also why it sits *after* mousekeys and
consumer in the chain, and guards against seeing its own output.

One target it refuses: another `AUTOCLK(n)`. That is a loop with no exit, and the
`emitting` guard means it would simply never fire.

## Triggers

| | |
| --- | --- |
| `AC_HOLD` | runs while held, stops on release |
| `AC_TAP2` | double-tap latches it on |
| `AC_TAP3` | triple-tap latches it on |

They OR together, and `AC_TAP2 | AC_HOLD` is the useful pair: hold for a burst,
double-tap to leave it running. **Any tap stops a latched autoclicker** —
turning it off has to be at least as easy as turning it on.

A key with both a tap trigger and `AC_HOLD` waits `AC_TAP_WINDOW_MS` (250) before
starting a hold, or the second press of a double-tap would begin holding and the
toggle would never fire. That delay is only paid on keys that asked for both.

## Rate

Per slot in the table, but `autoclick_ms` on the **SETTINGS** tab overrides every
slot when non-zero — the rate is the thing you want to change by feel, and
reflashing to try 80 ms instead of 100 is a miserable loop.

Clamped to `AC_MIN_INTERVAL_MS` (8). A 1 ms autoclicker is not faster in any
useful sense: each click is a press and a release, each needing its own USB
frame, and asking for more than the endpoint can carry just fills the queue.

Press and release land on **separate passes**. Both in one scan collapse into a
single composed report and the host sees nothing at all — the same trap
`tapping.cpp` and the encoder path work around.

## Editing slots at runtime

The **AUTOCLICK** panel sits under the keymap grid, next to the macro builder.
Pick a slot chip, then set:

- **repeats** — opens the same key picker the keymap uses, so the target can be
  any keycode. A slot cannot target another `AUTOCLK(n)`; that is a loop with no
  exit, and the `emitting` guard means it would simply never fire.
- **every** — the slot's own rate, `AC_MIN_INTERVAL_MS`..`AC_MAX_INTERVAL_MS`.
- **trigger** — hold / double-tap / triple-tap. The two tap counts exclude each
  other: both set is not a richer setting but an ambiguous one, since the process
  loop resolves it to triple and the double bit silently does nothing.

Edits apply immediately; **Save slots to flash** makes them survive a power
cycle, and **Revert to compiled** goes back to the `AUTOCLICKS` table.

```
GET  /api/autoclick        every slot, plus the valid ranges
POST /api/autoclick        {"slot":n,"target":kc,"interval_ms":ms,"trigger":bits}
POST /api/autoclick/save   persist to flash (queued; core 0 does the write)
POST /api/autoclick/reset  {"erase":bool}
```

`trigger` is the same bitmask as the table: 1 hold, 2 double-tap, 4 triple-tap.

Stored slots live in the **fifth flash sector from the end** (keymap, macros,
wifi, settings, autoclick). A stored blob whose slot count does not match the
firmware is ignored in favour of the compiled defaults, rather than being
reinterpreted as a different number of slots.

Two ordering details that are easy to get wrong:

- Editing a running slot **stops it first**. Changing the target of a slot that
  is mid-repeat would otherwise release the *new* keycode and leave the old one
  held down on the host.
- The commit stops every slot before erasing. A flash erase parks core 1, so a
  slot left mid-press would have its release deferred for the whole write.

`autoclick_ms` on the SETTINGS tab still overrides every slot's rate when it is
non-zero — the panel says so when it is set, because otherwise the rate you type
here appears to do nothing.

## Testing

```sh
cd tools/kbtest && make autoclick_test && ./autoclick_test
```

Counts what the host would actually see: that one tap does not start a
double-tap trigger, that a latch survives release, that presses and releases
balance, that the rate is the one asked for, and that nothing is left held.
