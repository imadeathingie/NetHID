# Runtime settings

Options that can be changed from the web UI and persisted, without a reflash.
The **SETTINGS** tab is available in normal operation and in AP setup mode.

Every field has a `#define` in `config.h` as its default. The store keeps an
override *plus* a bit recording whether you ever set it — so a setting you never
touch keeps tracking `config.h` across rebuilds instead of freezing at whatever
it happened to be the first time you pressed Save. Fields you have set show as
**set**, with their default alongside and a reset button.

| field | default from | |
| --- | --- | --- |
| `quiet_boot` | `QUIET_BOOT` | stop typing **all** boot output into the host |
| `debug_matrix` | `KB_DEBUG_MATRIX` | log every matrix edge to the serial console |
| `type_delay_ms` | `TYPE_DELAY_MS` | per-character typing delay |
| `ap_auto_fallback` | `AP_MODE_AUTO_FALLBACK` | start setup mode when no known network is in range |
| `session_timeout_s` | `SESSION_TIMEOUT_S` | web session idle timeout |
| `lockout_s` | `LOCKOUT_S` | lockout after failed logins |
| `max_auth_attempts` | `MAX_AUTH_ATTEMPTS` | failed logins before lockout |
| `tapping_term_ms` | `TAPPING_TERM` | dual-role hold threshold |

`debug_matrix` and `tapping_term_ms` are the two that change the loop most:
working out a matrix and finding a tapping term that feels right are both
iterative, and reflashing between 180 ms and 200 ms to find out which is better
is a miserable way to spend an evening.

Out-of-range values are **rejected, not clamped**. A silently corrected value
looks like it worked and behaves like something else.

## What is deliberately not here

**`ENABLE_WEB`, `ENABLE_TCP`, `ENABLE_HTTPS`, `ENABLE_REMOTES`,
`ENABLE_KEYBOARD`** decide what gets compiled and which listeners bind at boot.
A runtime toggle cannot un-link code, and a button that disables the web server
from inside the web server has exactly one effect.

**`AUTH_REQUIRED` and `PASSWORD`** stay compile-time on purpose. Authentication
should not be weakenable by an authenticated session — if someone obtains one
session, they should not be able to remove the need for the next one.

**Matrix pins, `MATRIX_ROWS`/`COLS`, diode direction** are hardware. Wrong values
do not misbehave, they stop the keyboard scanning entirely — and in AP mode the
page you would fix them from needs a working keyboard to reach.

## Adding a field

One row in the `FIELDS` table in `src/settings.cpp`:

```c
F("my_option", T_U16, my_option, 1, 500, MY_OPTION_DEFAULT, "What it does")
```

plus the struct member in `settings.h`. That table drives the default, the
range, the setter, the override bit and the JSON the page renders from, so
there is no second place to forget.

Bump `SETTINGS_VERSION` if you change or remove an existing field. Adding one is
already handled: the blob records its field count and falls back to defaults on
a mismatch rather than reinterpreting old bytes as new fields.

## Boot keys

All four boot gestures are configured together in `config.h`:

| | default | |
| --- | --- | --- |
| `BOOTMAGIC_ROW/COL` | (0,0) | land in the RP2350 bootloader |
| `QUIET_BOOT_ROW/COL` | (0,1) | suppress boot diagnostics, this boot only |
| `LOUD_BOOT_ROW/COL` | (0,2) | force diagnostics ON, this boot only |
| `AP_MODE_ROW/COL` | (0,5) | serve our own WiFi network for setup |

All three are `#ifndef`-guarded so a board can override them in its
`keyboard.h`, and all three are sampled the same way — the key must read down
across every scan of a ~32 ms window, so a bouncing switch on a rail that has
only just come up cannot trigger one by accident.

The quiet and loud keys are **per-boot and write nothing to flash**: gestures
about this boot, not configuration changes. The persistent version is the
`quiet_boot` setting.

**Loud boot is the way back.** `quiet_boot` can be saved to flash, and a device
that has been told to say nothing is exactly the device you cannot diagnose —
the setting hides the evidence of its own consequences. Holding the loud key
forces diagnostics on for one boot whatever is stored, so you can see what is
happening without a reflash and without reaching the web UI. It works in every
boot mode, including AP setup mode.

Precedence is `loud > quiet > stored`. If you somehow hold both keys, loud wins:
between "say nothing" and "say something", the recoverable choice has to be the
one that happens.

Note that `settings()->quiet_boot` reports the **stored** value — what the
settings page should render. Anything deciding whether to actually type must
call `settings_quiet_boot_effective()`, which resolves the boot keys on top.

Pick keys far apart. Ending up in the bootloader when you wanted a quiet boot is
annoying; the reverse is worse.

## What quiet boot covers

Everything the firmware types into the host at boot: the per-step diagnostics,
the "ready" banner with the URLs, the WiFi-failure message, and the AP setup
address. The serial console still gets all of it — a console you had to attach
on purpose is not the disruptive part.

It does **not** cover `/api/text` or macros. Quiet boot is about unrequested
boot noise, not about refusing work someone asked for.

One consequence worth knowing: with quiet boot on, AP setup mode no longer types
its address. The default is `http://192.168.4.1/` and the SSID is whatever
`AP_SSID` is set to, so nothing is unreachable — but if you rely on that banner,
leave quiet boot off. The serial console still prints it, prefixed `[quiet]
suppressed typed message`.

Both typed paths — `dbg()` for single lines and `type_to_host()` for multi-line
banners — go through one `boot_output_allowed()` gate, and
`tools/check/check_quiet_boot.py` fails the build if a function that types is missing
it. That check exists because the gate was originally applied to `dbg()` only,
so quiet boot silenced the per-step lines and left the banner typing anyway.

## If typed output drops characters

Symptom: boot diagnostics come out with characters missing, or with the wrong
case, intermittently — perhaps one boot in three, worse when WiFi is busy.

Root cause, now fixed: `tud_hid_n_report()` returns false when the USB endpoint
cannot take another report, and the typer advanced its state regardless. A
rejected report was silently discarded.

- A dropped **press** loses that character.
- A dropped **release** leaves the host believing the previous key is still
  held. If that key carried shift, everything typed afterwards — by the firmware
  or by the person at the keyboard — appears shifted until some other report
  clears it. That is what "incorrect shifting" was; the ASCII map was never
  wrong.

`typer_step()` now advances only when the send is confirmed, and retries the
same report otherwise. The same rule is applied to the auto-release and merged
key-state paths, which had the identical defect.

## If the web keyboard or the absolute mouse stops working

Two separate faults with a shared cause: a change that looked local was not.

**Web keyboard silent while physical keys work.** `hid_task()` handled the
auto-release *before* composing the merged key state, so a keypress from
`/api/key` was written into the network source by the queue drain and cleared on
the very next pass — before anything had sent it. Physical keys never set that
flag, which is exactly why the matrix kept working and made this look like a web
problem rather than a HID one. The release now runs after the compose and only
once `keystate_dirty()` has gone false, meaning the press has actually left.

**Absolute mouse: three attempts, and what each one taught.** Selected by
`ABS_MOUSE_MODE`; the coordinates sent are identical in all three, only the
descriptor differs.

Every failure so far had the same shape — **a host deciding for itself which of
several pointers behind one interface to believe.** Two Mouse Application
collections: the host bound the first and ignored the second. Both folded into
one collection with two report IDs: the host applied one interpretation of X/Y
to both report IDs. A Pen digitizer (`ABS_MOUSE_MODE=1`) finally escaped that,
because a digitizer is a different device class — and it works on macOS. But on
**Windows a Pen collection is Windows Ink**: the arrow cursor is replaced by the
pen cursor and then hidden altogether between reports. That is Windows behaving
correctly for a pen. The way to get an arrow back is to stop being a pen.

So the default is now `ABS_MOUSE_MODE=2`: an ordinary Generic Desktop **Mouse**
whose X/Y are absolute, on **a HID interface of its own**. That is what every
VM's "absolute pointing device" is (QEMU usb-tablet, VMware, VirtualBox), and
hosts drive it with a normal cursor. The separate interface is the point rather
than an implementation detail: a host binds each HID interface independently, so
the question that defeated the first two attempts never arises.

In digitizer mode only, two things are not optional: **IN RANGE must be set** on
anything carrying a position, because a host will not track a pointer it does
not believe is near the surface, and buttons are **not** sent on the digitizer —
digitizer button semantics vary by host, so the web UI sends position on report
3 and clicks on report 2 with a zero movement delta. Modes 0 and 2 carry their
own buttons and need none of that.

No Physical Minimum/Maximum anywhere in the pointer descriptors. They are
**global** items — in force for every main item that follows until changed — so
declaring a physical range of 0..32767 for absolute X/Y also applied it to the
Wheel underneath, whose logical range is -127..127. A host converting to
physical units read a wheel of 0 as the midpoint of that range and scrolled on
every single report; Windows does that conversion, macOS does not, so it looked
like a Windows-specific bug. Absent (or both zero) means "physical units are the
logical units", which is all this device ever wants.

Because the interface *count* changes with the mode, `bcdDevice` is bumped when
it does: macOS and Windows both cache parsed descriptors keyed by
VID/PID/bcdDevice, and a host reusing a stale parse looks for an endpoint that
no longer means what it did.

```sh
python3 tools/check/dump_hid_descriptor.py            # what the descriptor says
python3 tools/check/dump_hid_descriptor.py --all      # every mode, structure checked
python3 tools/check/check_usb_descriptor.py build/nethid.elf   # interfaces/endpoints
cmake .. -DABS_MOUSE_MODE=1                           # back to the digitizer
# and build with -DHID_DEBUG_ABS=1 to log every abs report as it leaves
```

The earlier attempt, for the record:

**Duplicate mouse collections.** The descriptor had two separate
Application collections both declaring `Usage (Mouse)`. Hosts commonly bind only
the first one they see, so the second was parsed and then ignored, and the two
appeared to fight over button presses. They are now one collection containing
both report IDs — a single Application collection may hold several, so this
costs nothing and removes the ambiguity: there is one mouse, and it can be told
to move by a delta or to jump to a coordinate.

Consumer Control also moved to the end of the descriptor. Sitting between the
keyboard and the mouse is not illegal, but keeping the pointing devices next to
the keyboard is what every host is used to parsing.

## If typed output comes out spliced together

Symptom: fragments of one boot line appearing inside another, with a run of
trailing characters piling up at the end.

`hid_task()` used to fall through to the command queue whenever `typer_step()`
returned false — and it returns false for every ordinary inter-character gap,
not just on failure. The drain would then dequeue the next `TYPE_STRING` and
overwrite `typer.text/len/pos` while the previous string was still being typed.

It now returns unconditionally while a string is in flight. This was always
latent; making `typer_step()` also return false on a rejected report widened the
window enormously, which turned it from occasional into constant.

A related trap: `hid_typer_busy()` originally reported only whether the typer
was *running*, not whether strings were queued. A caller that pushed and then
waited saw "idle" for the whole window between the push and core 1 dequeuing,
concluded the line was finished, and queued the next one immediately. It now
counts queued strings too.

`dbg()` also used to sleep a fixed estimate of how long a line ought to take.
With retries a congested endpoint takes longer than that, so it now waits for
`hid_typer_busy()` to clear, with a generous timeout so an unresponsive host
cannot wedge the boot sequence.

Raising `type_delay_ms` on the settings page gives the endpoint more room per
character and is worth trying on a host that is still fussy.

```sh
cd tools/kbtest && make typer_test && ./typer_test
```

drives the real typer through an endpoint that rejects a set percentage of
reports at random and checks that what the host decodes spells the input
exactly, and that no key is left held at the end. Reverting the fix makes it
fail with precisely the reported symptoms.

## Testing

```sh
cd tools/kbtest && make check
```

Runs the keyboard, settings and typer suites — defaults, range rejection,
override tracking, persistence across a simulated reboot, the field-count
guard, and that the boot key does not persist.
