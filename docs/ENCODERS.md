# Encoders

Encoders get **dedicated pins**, not matrix positions: two quadrature lines and
optionally a push switch. That is how they are wired, and pretending an encoder
is three matrix positions means growing the matrix to fit hardware that is not
one.

In a board or module header:

```c
#define NUM_LOCAL_ENCODERS 1
#define ENCODERS \
    ENCODER(26, 27, 28)        /* A, B, switch */
```

Use `ENCODER_NO_SW` for the switch pin if there isn't one. All three pins get
internal pull-ups; wire the encoder's common pin to ground.

The count is stated rather than derived. The obvious trick — redefine `ENCODER`
as `+1`, expand, `#undef` — does not work: the resulting macro expands at its
*use* site, by which point the helper is gone, and you get "missing binary
operator before token ENCODER" pointing at a board header that is perfectly
correct.

## Mapping

Per layer, `[encoder][CCW, CW, press]`:

```c
const kb_keycode_t encoder_map[][NUM_ENCODERS][3] = {
    [BASE] = { { KC_VOLD, KC_VOLU, KC_MUTE },
               { KC_MPRV, KC_MNXT, KC_MPLY } },
    [NAV]  = { { KC_PGDN, KC_PGUP, KC_TRNS },
               { KC_BRID, KC_BRIU, KC_TRNS } },
};
const uint8_t encoder_map_layers =
    (uint8_t)(sizeof(encoder_map) / sizeof(encoder_map[0]));
```

All three actions of a knob on one line, because that is how you read a knob.
`KC_TRNS` falls through to the layer below exactly as it does for keys, so a
layer that says nothing about a knob leaves it working rather than dead.

The volume and brightness keycodes above need `KB_FEATURE_CONSUMER` — without it
the knob turns and nothing happens. See [MEDIA_KEYS.md](MEDIA_KEYS.md).

Rotation resolves through the **same feature chain** as keys, so a detent can
carry a mod-tap, fire a macro or be remapped from the web editor without any of
those features knowing an encoder exists. A detent is momentary — there is no
held state for a rotation — so it is emitted as a press and released on the
following scan. Doing both in one scan would collapse into a single composed
report and the host would never see it, the same trap `tapping.cpp` works
around. The push switch *is* held, because it is a real button.

## Editing from the browser

The keymap editor shows an **ENCODERS** strip below the keys: one card per knob
with three slots laid out the way a knob works — turn left, press, turn right.
Each opens the same key picker the matrix uses, so a detent can be assigned any
keycode, including [media keys](MEDIA_KEYS.md), [autoclick slots](AUTOCLICK.md)
and macros.

They are deliberately not in the grid: an encoder is not a matrix position, and
putting it there would mean inventing a fake row for hardware that does not have
one. On a modular board each card names the module its knob is on, since two
identical cards otherwise give no clue which physical device you are editing.

Encoder maps are stored in the **same flash blob** as the keymap and share its
dirty flag and Save button. They are edited in the same breath; separate stores
would mean two Save buttons and a way to persist half your changes. The stored
blob version was bumped when they were added, so an older blob falls back to the
compiled defaults rather than being read as encoder entries.

Endpoints, both behind the existing auth:

| | |
| --- | --- |
| `GET /api/keymap/encoders/<layer>` | `[[ccw, cw, press], …]` |
| `POST /api/keymap/encoders` | `{"layer","index","action","kc"}` |

One action per POST, unlike the keymap which is sent a whole layer at a time:
there are only three per encoder, so a partial write has nothing to be
inconsistent with.

## Getting the direction right

Which way is clockwise depends on which of A and B you soldered where, so there
is no correct default — only a convention and a way to flip it:

```c
#define ENCODER_REVERSED 1                 /* all encoders */
#define ENCODER_REVERSED_MASK 0b10         /* or per encoder, bit n */
```

Better than swapping wires or rewriting `encoder_map`.

`ENCODER_RESOLUTION` (4) is quadrature transitions per detent. Most encoders
click every four; a few every two. If one click sends two volume steps, that is
the knob to turn.

## Bounce

Decoding uses a transition table indexed by `(previous << 2) | current`, with
every invalid transition mapping to zero. Reading one edge instead turns a cheap
encoder's bounce into a burst of steps — and always in the same direction, so
volume jumps ten notches from one click. The push switch has an ordinary 5 ms
debounce, which rotation does not need because the table already rejects
bounces.

## Encoders on modules

A module declares its own, and the module table says how many each has:

```c
#define SPLIT_MODULES           \
    SPLIT_MODULE(0, 4, 6, 1)    /* id, rows, cols, encoders */ \
    SPLIT_MODULE(1, 4, 6, 0)    \
    SPLIT_MODULE(2, 1, 4, 1)

#define TOTAL_ENCODERS 2
```

`encoder_map` is indexed by the total, with each module's encoders starting at
its computed base, so a knob can move between modules without the keymap being
rewritten.

Rotation travels in its own `SPLIT_MSG_SENSOR` frame, not appended to the matrix
frame. Rotation is a **delta** — consumed once and gone — whereas the matrix is
state that can be resent harmlessly. One frame would mean either resending
deltas (double-counted clicks) or making the matrix consume-once (a dropped
frame becomes a stuck key). A sensor frame is only sent when there is something
to say, and if the send fails the deltas are put back rather than lost.

Losing those clicks is the right failure mode if it comes to it: a knob that
misses a step is better than a knob that jumps.

## Testing

```sh
cd tools/kbtest && make encoder_test && ./encoder_test
```

Drives the real decoder from a simulated encoder: single clicks each way, ten
clicks counting exactly ten, bounce on one line producing nothing at all, a
reversed partial turn netting to zero, and a bouncing push switch debouncing to
one press.
