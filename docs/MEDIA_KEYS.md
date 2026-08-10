# Media keys

Volume, transport, brightness and the browser keys. `KB_FEATURE_CONSUMER` adds a
Consumer Control report (HID report ID 4) to the descriptor. These are not
keyboard usages and could not be expressed at all before it:

`KC_MUTE` `KC_VOLU` `KC_VOLD` `KC_MPLY` `KC_MNXT` `KC_MPRV` `KC_MSTP`
`KC_MFFD` `KC_MRWD` `KC_EJCT` `KC_BRIU` `KC_BRID` `KC_WSCH` `KC_WHOM`
`KC_WBAK` `KC_WFWD` `KC_WREF` `KC_CALC` `KC_MYCM` `KC_MAIL`

They appear as a **MEDIA** group in the web editor's key picker and as a type in
the advanced builder.

## Enabling it

Off by default, like every keyboard feature. Turn it on per board:

```cmake
# keyboards/<board>/rules.cmake
kb_default(KB_FEATURE_CONSUMER ON)
```

With it off, `KC_MUTE` and friends are still offered by the web editor, stored,
and drawn on the key — and then do nothing, because `consumer.cpp` is not in the
process chain. `/api/keymap/info` reports `features.consumer` so the editor can
hide the group and say so instead; see [keyboards/README.md](../keyboards/README.md).

## One usage at a time

The report carries **one usage at a time**. That is a real limit of the
descriptor chosen — an array of one — so two media keys held together do not
both register. In practice nobody holds mute and next-track at once, and the
alternative is a fixed bitmap needing extension every time a new key is wanted.

A second media key pressed on top replaces the first. Releasing the first
afterwards does *not* cancel the second: `consumer.cpp` only sends the release
if the key going up is still the one the host thinks is down.

## The keycode is an index, not a usage

`KC_MUTE` carries an *index* into a table rather than the usage itself: consumer
usages are 10-bit and will not fit alongside the `0x5A00` tag in 16 bits.

`CONSUMER_USAGES[]` in `src/kb/features/consumer.cpp` must stay in the same order
as the `KC_*` list in `kb/keycodes.h`, and as the `CC` list in the web UI. There
is a `static_assert` on the count but **none on the order** — a reordering that
keeps the count compiles cleanly and silently sends the wrong usage.

## Descriptor notes

Consumer is a top-level Application collection on usage page `0x0C`, sibling to
the keyboard, mouse and digitizer collections — not nested inside any of them.
It was nested for a while, because the mouse collection's `End Collection` sat
inside an unterminated `#if`; the result did not compile, and when the same
branch was built with the legacy in-mouse XY form (now `ABS_MOUSE_MODE=0`) it produced a descriptor a host
would parse and then ignore. Check the structure rather than assuming it:

```sh
python3 tools/check/dump_hid_descriptor.py --all
```

Media keys are also reachable over the network API without a physical keyboard —
see the MEDIA tab in the web UI.
