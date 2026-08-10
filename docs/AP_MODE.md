# AP setup mode

NetHID can serve its own WiFi network so you can reach it — and change its
credentials — with no router involved.

## Using it

Hold the **AP key** while plugging the board in. On `mystery6x6` that is
`(0,5)`, the top-right key of the main block; set `AP_MODE_ROW`/`AP_MODE_COL` in
`keyboard.h` to move it. Five LED blinks means setup mode is up.

The address is typed into the host **every 20 seconds** while setup mode runs,
not once at bring-up. In setup mode there is no network to reach the device on,
so this address is the only way in — and a single announcement is fragile: the
typer may still be draining an earlier line when the AP comes up (and
`hid_push_type_string()` silently drops a push while it is busy), and the host
may not be plugged in or focused anywhere useful at that exact moment.

The message is plain ASCII on purpose. See "Typed output must be ASCII" below.

Join `NetHID-Setup` from a phone or laptop with the password you set in `env.h`.
The captive-portal sheet should appear by itself; if it doesn't, open
`http://192.168.4.1/` and go to the **WIFI** tab.

Networks that were in range when the device last scanned appear as a pick-list
— tap one to fill in its name and security type, then just type the password.
Add your network, then **Save & reboot**. The device restarts, joins it, and
you find it at its normal address again.

## Three tabs: WIFI, KEYMAP and SETTINGS

Setup mode serves those three and nothing else, and the API is narrowed to match —
everything outside login, WiFi provisioning and keymap/macro editing returns 403.

Both halves matter. Hiding a tab is cosmetic: the endpoints behind it would still
answer, so someone who joined the setup network and skipped the UI entirely would
still reach `/api/key` and, through it, your host's keyboard. The allowlist in
`handle_request()` is the part that actually closes that.

The line those tabs sit on is **configuration versus control**. WIFI and
KEYMAP change what the device will do later. KEYBOARD, MOUSE, MACROS, MEDIA and
the IR tabs type, click and transmit *right now* — one HTTP request from anyone
in radio range holding the AP password.

Keymap editing is not zero risk, and it is worth being precise about the residual.
Someone with access to setup mode could rewrite your keymap, or point a
`KB_MACRO(n)` at a step list that types a shell command. But a stored macro only
runs when the matching key is **physically pressed** — there is no path from the
API to firing one. So the worst case is a booby-trapped keyboard that misbehaves
next time *you* type on it, which is real but categorically weaker than the
direct injection `/api/key` would allow. If that residual is more than you want,
drop `"keymap"` from the tab filter and the `/api/keymap*` and `/api/macro*`
entries from the allowlist, both in `src/web.cpp`.

### Serving a subset of tabs

Two things make a restricted tab set work, and both were needed before setup
mode rendered anything at all:

**Grants are bypassed in setup mode.** `TAB_GRANTS` lives in `env.h`, so it can
only be changed by reflashing — and gating the recovery path behind a list you
need a reflash to edit is precisely the trap AP mode exists to avoid. A
non-admin user with grants configured would otherwise log in and find a page
with no tabs on it. Nothing is widened: the session is already authenticated and
the API allowlist still decides what is reachable.

**The page's init tolerates missing panels.** The server omits panels a user is
not granted, so at any load an arbitrary subset of the DOM the init code expects
is absent. Run bare and back to back, one null dereference aborts the whole
top-level script: the page keeps its header and log box and loses the tab bar
entirely, which looks like the page failed to load rather than like a bug in one
function. That is exactly what `#customPanels` — inside the `customedit`
fragment, which setup mode does not serve — did.

Each init step now runs through `initStep()`, so a failure is logged and skipped
rather than fatal, and `tools/check/check_tab_subsets.py` loads the page under jsdom
with each subset the firmware can actually serve and checks the tab bar still
builds:

```sh
npm i jsdom            # once
python3 tools/check/check_tab_subsets.py         # the AP subset
python3 tools/check/check_tab_subsets.py --all   # other subsets too, slower
```

It skips cleanly if jsdom is not installed. If you add a DOM lookup for an
element inside a tab panel, guard it — `if (!el) return;` — the way the touchpad
and media widgets already do.

### Keeping the allowlist honest

Run `python3 tools/check/check_ap_allowlist.py` after touching that list. It checks
both directions, and the second is the one that bites:

- **Forward** — every allowlisted path is a real route. A typo fails silently:
  the path never matches, the endpoint 403s, and setup mode becomes a page that
  cannot save anything.
- **Reverse** — every path the UI calls in setup mode is allowlisted. Login is a
  three-step handshake (`/api/authinfo`, `/api/challenge`, `/api/auth`), and
  listing only two of them 403s the middle step. The login page reports
  "Cannot start login" and there is no way into setup mode at all.

The reverse check reads `NetHID_login.html` and the WIFI, KEYMAP and SETTINGS
tab fragments and requires every endpoint they call to be allowed. Control
endpoints the page shell also references — `/api/key`, `/api/text`, `/api/wake`,
the IR and RF routes — are correctly blocked and are excluded from that set.

If you add a tab to setup mode, add its name to `AP_TABS` in the checker too.

## The scan is a cache, not a live call

The pick-list is captured **before** the access point comes up, during the same
scan `main()` already does to choose a network. `scan_match_cb()` files every AP
it sees, not only ones we have a password for.

It has to work that way. Once `cyw43_arch_enable_ap_mode()` has run the chip is
an access point and can no longer survey the air — asking it to is unreliable
and can drop the client you are configuring it from, which is the worst possible
moment to lose the connection.

Two consequences worth knowing:

- The list is a snapshot from boot. A network switched on afterwards will not be
  there; reboot into setup mode again to re-scan.
- **Hidden networks never appear**, because they broadcast an empty SSID. Type
  the name in by hand — that path is always available and is why the SSID field
  is a text box rather than a dropdown.

## Why it isn't automatic

The default trigger is a held key, not a failed connection.

Behind this access point is a device that types into whatever computer it is
plugged into. A held key means it starts because you asked; automatic fallback
means it starts because the router rebooted at 3am and then sits there
broadcasting until someone notices. If you would rather have the second
behaviour — and it is defensible, since the alternative is a device you cannot
reach at all — set `AP_MODE_AUTO_FALLBACK` to 1 and the previously terminal
"WiFi failed" path becomes an AP instead.

## What protects it

**WPA2, always.** `ap_mode_start()` refuses to run if `AP_PASSWORD` is under 8
characters rather than falling back to an open network. There is no default
value, because a shipped default password is the same as no password.

**Session auth, regardless of `AUTH_REQUIRED`.** On your own LAN you might
reasonably relax it. Here the login page is the only thing between someone in
radio range and your host's keyboard.

**One listener.** The control socket is not started in AP mode. Setup mode
exists to get credentials in; nothing else needs to be reachable while it does
that.

**Passwords are write-only over the API.** `GET /api/wifi` lists SSIDs and where
they came from, never a secret. An authenticated session should not be a
credential dump.

## The HTTPS trade

This interface is plain HTTP. The captive-portal DNS answers *every* lookup with
the Pico's address, and no certificate validates for
`connectivitycheck.gstatic.com` — so a portal that actually pops has to be
plaintext. A self-signed cert on a network you joined thirty seconds ago also
produces a browser warning that trains precisely the wrong instinct.

The link has one client on it, on a network you had to know a password to join,
for the couple of minutes it takes to type an SSID. That is the argument. It is
a real trade and the UI says so on the page.

## Credentials in flash

Provisioned networks live in their own sector (third from the end; keymap has
the last, macros the second) as **plaintext**. Anyone with the board and
`picotool save` can read them.

That is already true of anything in `env.h`, which ends up in the binary — but
it becomes true of passwords you typed rather than ones you compiled in, which
feels different even though it isn't. Don't provision a network you wouldn't
also be willing to compile in.

Stored networks are tried **before** compiled ones, so adding an entry with the
same SSID overrides a firmware one — which is how you fix a changed password
without a rebuild.

## API

| | |
| --- | --- |
| `GET /api/wifi` | known networks and their source; never passwords |
| `GET /api/wifi/scan` | cached survey of what was in range at boot |
| `POST /api/wifi` | `{"ssid","password","auth"}` — add or replace |
| `POST /api/wifi/forget` | `{"ssid"}` — stored entries only |
| `POST /api/wifi/save` | persist to flash (queued) |
| `POST /api/wifi/apply` | save, respond, then reboot |

`apply` answers *before* rebooting, with a 1.5 s delay, so the browser sees a
result instead of a dead socket.

## Vendored code

lwIP ships neither a DHCP server nor a DNS server and AP mode needs both. See
`src/vendor/README.md` — MIT and BSD-3-Clause, both GPL-3 compatible.

If you update those files, check `dhcp_server_init()` and `dns_server_init()`
still take a `struct netif *` as their second argument. An older three-argument
form is widely quoted and compiles into a confusing runtime failure.

## Typed output must be ASCII

`ascii_to_hid()` maps characters to keycodes and has no entry above `0x7E`. It
used to skip what it could not map, so an em-dash in a diagnostic simply
vanished — a message that read `AP key held ` and stopped, with nothing to
suggest three bytes had been dropped mid-sentence.

Two changes: `dbg()` now substitutes `-` for anything unmappable, so a slip is
visible rather than silent; and `tools/check/check_typed_ascii.py` flags non-ASCII in
strings that reach `dbg()` or `type_to_host()`, including ones assembled with
`snprintf` first. Prose in comments and `printf`-only output is not checked — a
serial console handles UTF-8 fine.

```sh
python3 tools/check/check_typed_ascii.py
```

## Build notes

`src/vendor/*.c` emit unused-parameter warnings under this project's flags.
That is expected — see `src/vendor/README.md` for why they are not patched.

`tools/check/check_includes.py` enforces one convention that is otherwise only
discoverable by reading an error inside a header you never edited: anything
including `nethid.h` must include `tusb.h` first, because `nethid.h` declares
the HID queue in terms of TinyUSB types without pulling them in. Run it before
a build; it needs no cross-compiler.
