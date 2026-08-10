# NetHID — test checklist

Things built recently that need confirming on real hardware. Much of this is
already verified in software (logic/round-trips), but IR/RF and timing-sensitive
behaviour can only really be proven on the device. Work top-down: the early
sections need no extra hardware; the IR/RF sections need the receiver/blaster
modules.

Build letter is printed in the boot log and `/api/ping` debug — confirm you
flashed the build you think you did. Current target: **build X**.

---

## 0. Prerequisites

### Parts to acquire
- [ ] **IR LED + NPN transistor** (2N2222 or similar) + ~68Ω and ~1k resistors — for IR *transmit*.
- [ ] **IR receiver, demodulating**: TSOP382 / TSOP384 / VS1838B (38 kHz) — for IR *capture*. 3V3-safe.
- [ ] **433 MHz OOK transmitter**: MX-FS-03V (or FS1000A) — for RF *transmit*.
- [ ] **433 MHz OOK receiver**: prefer a **superheterodyne** module (RXB6, RXB8). The cheap green
      "RX-B" ones are very noisy and capture poorly.
- [ ] Hook-up wire; ~17.3 cm straight wire as a 433 MHz antenna for the TX (and RX if it has an ANT pad).

### Wiring (defaults; change in `config.h`)
- [ ] IR LED → GPIO16 via transistor (GPIO16─1k─base; 5V─IRLED─68Ω─collector; emitter─GND).
- [ ] IR receiver: VCC→3V3, GND→GND, OUT→**GPIO18**.
- [ ] 433 TX (MX-FS-03V): VCC→5V, DATA→GPIO17, GND→GND, ANT→whip.
- [ ] 433 RX: VCC→**3V3**, GND→GND, DATA→**GPIO19**.  ⚠ Power from 3V3, *not* 5V — at 5V the
      DATA line can exceed the GPIO's 3V3 limit (needs a level shifter otherwise).

### Build & flash
- [ ] `cp include/env.h.example include/env.h` and edit WiFi creds, users, secrets, tab grants.
- [ ] Pico 2 W: `rm -rf build && mkdir build && cd build && cmake .. -DPICO_BOARD=pico2_w && make -j`
- [ ] (Optional) Pico W (RP2040): same but `-DPICO_BOARD=pico_w`. Confirm it builds, links, and runs.
- [ ] Flash `build/nethid.uf2` via BOOTSEL. Confirm boot log shows the build letter and "READY".

---

## 1. Core regression (no extra hardware)

- [ ] Device joins WiFi; web UI loads at the device IP / `nethid.local`.
- [ ] Login page appears when logged out; correct password logs in; wrong password rejected.
- [ ] USB HID works: type via the Keyboard tab, move via Mouse/trackpad, macros fire.
- [ ] TCP socket (port 9000) still works (run `nethid_remote_test.sh` or a client example).

## 2. Session keep-alive fix (build R)

The bug was logout ~5 min after login despite activity (cookie `Max-Age` never refreshed).
- [ ] Log in, then **keep using the UI** (move the trackpad / press keys) past the 5-minute mark.
      Expect: still logged in (no bounce to the login screen).
- [ ] Log in, then **go idle**. Expect: session expires after the inactivity window and the
      login overlay returns.
- [ ] Watch the heartbeat: `/api/ping` should be hit periodically and return 200 while active.

## 3. Per-user secrets — strict isolation (build P)

With the sample `env.h` (admin/alice/bob; alice & bob each have their own `WIN_PASS`, shared `EMAIL`):
- [ ] As **alice**, a text shortcut with `${WIN_PASS}` types *alice's* value; `${EMAIL}` types the shared one.
- [ ] As **bob**, `${WIN_PASS}` types *bob's* value (different from alice's).
- [ ] As **admin** (no `WIN_PASS` of its own, no global one), `${WIN_PASS}` expands to nothing.
- [ ] Confirm the secret value never appears in any HTTP response, only gets typed on the USB host.

## 4. Per-user web tabs (build Q)

Sample grants: everyone gets keyboard+media; alice also mouse+macros; bob also macros; admin sees all.
- [ ] Log in as **admin** → all tabs present (keyboard, mouse, macros, media, IR DB, Learn, + CUSTOM).
- [ ] Log in as **alice** → keyboard, mouse, macros, media (no IR DB/Learn unless granted).
- [ ] Log in as **bob** → keyboard, macros, media (no mouse).
- [ ] View source / DOM as a restricted user: the ungranted tabs' markup is **absent**, not just hidden.
- [ ] `GET /api/whoami` returns the logged-in user.

## 5. IR transmit + the IR DB tab (builds S–V)  ← **Samsung testable now**

You can test these as soon as the IR LED is wired (no receiver needed).
- [ ] IR DB tab loads; "Load IRDB index" populates; search/browse works.
- [ ] **Samsung TV (priority):** open a Samsung TV remote, hit **Send** on Power.
      Expect the TV to toggle. (Samsung32 was verified in software to emit the canonical
      `E0E040BF` for IRDB device=7/function=2 — this confirms it on real hardware.)
- [ ] Samsung volume/channel/input also work from the same remote entry.
- [ ] NEC remote (if you have another NEC device): Send works.
- [ ] **Spec-derived encoders — confirm if you have the gear** (these were verified structurally,
      not on hardware): RC5, RC6, Sony (SIRC 12/15/20), JVC, Panasonic/Kaseikyo.
      - [ ] RC5/RC6 device (older Philips, some set-top boxes)
      - [ ] Sony device (40 kHz)
      - [ ] Panasonic device (37 kHz, "4004" vendor)
      Note: if one fails, likely culprits are the carrier or a toggle/bit-order convention —
      all one-liners in `renderRaw` in `NetHID_irdb.html`.

## 6. IR capture / "Learn" (build W+; needs the IR receiver)

- [ ] Wire the TSOP receiver to GPIO18. Open the **Learn** tab.
- [ ] Click **Learn IR**, point your Samsung remote at the receiver, press Power.
      Expect: a captured frame appears with `proto: nec` and a code, plus an edge count (~67).
- [ ] **Replay** the captured frame → the TV should toggle (this proves capture→replay round-trips).
- [ ] **Save** it with a name; confirm it persists across a page reload (localStorage).
- [ ] Capture a non-NEC remote → expect `proto: raw` with a sensible edge count; replay should work.
- [ ] Sanity: arming and not pressing anything for ~12 s should quietly disarm (no stuck state).
- [ ] (Curl path, if you prefer: `POST /api/ir/learn` then poll `GET /api/ir/captured`.)

## 7. RF 433 MHz transmit + capture (needs both 433 modules)

- [ ] **TX:** wire MX-FS-03V to GPIO17. If you have a known OOK remote's timings, send via `/api/rf`
      (`{"timings":[...],"repeat":8}`) and confirm the target device responds.
- [ ] **RX:** wire the 433 receiver to GPIO19 (3V3!). Learn tab → **Learn RF**, press a 433 remote
      (e.g. a socket/doorbell remote). Expect a captured burst (it rejects no-sync noise).
- [ ] **Round-trip:** Replay the captured RF frame → the original device should respond.
- [ ] If capture is flaky: this is expected with a noisy receiver. Try a superhet module, press
      a few times, or tune `RF_SYNC_MIN_US` / `RX_MIN_EDGES` / `RX_GAP_US` in `remotes.cpp`.

---

## 8. HTTPS / TLS on the device (build AB; experimental — needs a domain + cert)

This is **off by default**; the whole section only applies if you opt in. Expect
the firmware TLS build to need a round or two of on-hardware compile iteration
(mbedTLS/lwIP option + library names) — `check_config.h` will name anything
missing. The certificate tooling below is already tested host-side.

### Cert generation (host, no Pico needed)
- [ ] Pick an FQDN (e.g. `nethid.example.com`) and decide the cert path:
- [ ] **mkcert (recommended):** `./tools/cert/make-cert-mkcert.sh nethid.example.com` →
      confirm `include/server_cert.h` is created and `certs/server.{crt,key}` exist.
- [ ] **Let's Encrypt / Cloudflare:** export `CF_Token`, run
      `./tools/cert/make-cert-letsencrypt.sh nethid.example.com` → confirm the header is created.
- [ ] Sanity-check the header compiles: it's pure C strings; `gen_cert_header.py`
      prints the cert/key sizes and a fingerprint.

### Local DNS
- [ ] Point the FQDN at the Pico's LAN IP via **local** DNS (router / Pi-hole /
      `hosts`). Confirm `ping nethid.example.com` resolves to the LAN IP (public
      DNS may strip private IPs — rebinding protection).

### Build & flash (both switches must agree)
- [ ] Set `#define ENABLE_HTTPS 1` in `include/config.h`.
- [ ] `rm -rf build && mkdir build && cd build && cmake .. -DPICO_BOARD=pico2_w -DENABLE_HTTPS=ON && make -j`
- [ ] If CMake errors that `pico_mbedtls` / `pico_lwip_mbedtls` aren't found, check
      the library names your SDK uses in `pico-examples/pico_w/wifi/tls_*`.
- [ ] If mbedTLS `check_config.h` errors, add the named module(s) to `include/mbedtls_config.h`.
- [ ] Flash; boot log should now say **`[web] HTTPS server on port 443`**.

### Verify TLS
- [ ] `curl -v https://nethid.example.com/` succeeds (mkcert: add `--cacert $(mkcert -CAROOT)/rootCA.pem`
      or trust the root; LE: should be trusted already). Validate **before** the browser.
- [ ] `openssl s_client -connect nethid.example.com:443 -servername nethid.example.com` shows the
      expected cert chain and a completed handshake.
- [ ] Browser loads `https://nethid.example.com/` with a padlock (after the mkcert root is
      imported on that machine). Login, typing, mouse, IR/RF all work over HTTPS.
- [ ] Confirm the raw socket on **9000 is still plain TCP** (existing shell clients keep working).
- [ ] **Regression:** rebuild with `-DENABLE_HTTPS=OFF` (and `ENABLE_HTTPS 0` in config.h) and confirm
      the plain-HTTP build is byte-for-byte the same behavior as before (this is the safety net).

### Board-specific
- [ ] RP2350 (Pico 2 W): 16 KB/8 KB TLS buffers, 2 concurrent connections — confirm two browser tabs work.
- [ ] RP2040 (Pico W): 8 KB/4 KB buffers, 1 connection — confirm a single session is stable; watch for
      out-of-memory in the boot log under load. EC (P-256) cert strongly preferred here.

## Known caveats / things to watch

- **TLS firmware is unverified on hardware:** the cert tooling is tested, but the mbedTLS/lwIP/CMake
  glue (option names, library names, buffer sizes) is best-effort from the SDK examples and should be
  expected to need iteration. It's all behind `ENABLE_HTTPS` (default off), so the normal build is safe.
- **Private key in flash:** no secure element — anyone with physical access can read the key. Treat
  physical access as trust.
- **Let's Encrypt renewal:** 90-day certs mean rebuild + reflash on renewal. mkcert avoids this.

- **IR carrier on capture:** the demodulated signal doesn't carry its frequency, so replay assumes
  38 kHz. Fine for ~all remotes; a 36/40 kHz remote captures fine and usually still replays at 38 kHz.
- **Pico W (RP2040) entropy:** no hardware TRNG — session tokens/nonces use software entropy. Fine for
  a LAN device; weaker than the Pico 2 W's TRNG. (Functionality identical.)
- **Kaseikyo vendor:** the Panasonic encoder hard-codes vendor `0x2002`. A non-Panasonic Kaseikyo
  remote would need its own vendor bytes.
- **PIO usage on the W boards:** WiFi uses one PIO state machine; IR+RF TX use two more — fine on both
  RP2040 (8 SMs) and RP2350.

## Possible follow-ups (after testing)
- [ ] "Save to IRDB"/custom-tab integration so learned codes become one-tap buttons.
- [ ] More IR protocols on the "no encoder yet" list, if you hit a device that needs one.
- [ ] Tighten the inactivity window if the current ~"4 min ping + 5 min" feels too long.
