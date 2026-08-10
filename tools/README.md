# `tools/`

| | |
| --- | --- |
| [`check/`](check/) | static checks and host-side test harnesses |
| [`web/`](web/) | building and previewing the embedded web UI |
| [`keyboard/`](keyboard/) | keymaps, layouts and the OLED font |
| [`cert/`](cert/) | TLS certificates for HTTPS |
| [`kbtest/`](kbtest/) | the host test suite (C++, run with `make check`) |

## `check/`

```sh
tools/check/run-all.sh            # everything
tools/check/run-all.sh --fast     # skip the jsdom and compile-heavy ones
```

Each of these exists because something went wrong once in a way that did **not**
fail the build:

| | catches |
| --- | --- |
| `check_includes.py` | `nethid.h` included without `tusb.h` first — the error surfaces inside a header you never edited |
| `check_typed_ascii.py` | non-ASCII in strings typed to the host; the typer drops what it cannot map, so a message silently truncates |
| `check_quiet_boot.py` | a typed-output path that skips the quiet-boot gate, and reads of the stored rather than effective value |
| `check_ap_allowlist.py` | AP setup mode's API allowlist, both directions — a missing path 403s and locks you out of the page you would fix it from |
| `check_pins.py` | two features on one GPIO; produces hardware that half works |
| `check_keycode_tables.py` | the keycode table lives in three places — what the editor offers, what the export writes, what the firmware compiles. A name in the first with no `#define` in the third is a keymap that exports and then fails to build; the editor offered the whole number pad for as long as it existed while `keycodes.h` defined none of it |
| `check_modal_css.py` | a full-screen overlay whose card cannot fit or scroll on a phone. The settings modal outgrew a phone screen with no `max-height` and no `overflow`, and `align-items:center` put half the overflow above the top of the screen where scrolling cannot reach — jsdom does not lay out, so no UI check could measure it |
| `check_tab_subsets.py` | page init crashing when a tab it expects is not served (its driver runs from a temp dir, so it needs `NODE_PATH` — without it every case died with "Cannot find module 'jsdom'") |
| `check_keymap_ui.sh` | the editor's module mapping on a modular board |
| `check_preview.sh` | the mock preview still runs, fixtures still differ, and **every endpoint the page calls is actually modelled** — the mock answers `{ok:true}` to anything it does not, which turned a missing `/api/autoclick` route into an editor that looked absent |
| `check_export.sh` | an exported keymap converts, compiles, and keeps every referenced layer |
| `test_layer_trim.py` | which layers survive the export trim |
| `test_cond_eval.py` | the `#if`/`#elif`/`#else` evaluation inside `dump_hid_descriptor.py`. That mini-preprocessor has been wrong twice — an unterminated `#if` decoded as "structurally OK", and `#elif` was treated as `#else` so two mutually exclusive branches decoded at once and the tool blamed the descriptor |
| `check_usb_descriptor.py` | the CONFIGURATION descriptor in a linked firmware: `wTotalLength` vs the descriptors present (a host reads exactly that many bytes), `bNumInterfaces` vs the interfaces that follow, two interfaces sharing an endpoint. `check_build.sh` runs it for all three `ABS_MOUSE_MODE` values |
| `dump_hid_descriptor.py` | decodes the HID report descriptor and checks collection balance, `#if` balance, and **global items leaking forward** (a Physical range declared for absolute X/Y was still in force for the wheel below it, so Windows scrolled on every report) — in every branch (`--all`) |
| `check_picker_ui.sh` | the keycode picker offers autoclick in the *searchable* list, warns instead of silently accepting a keycode whose feature was compiled out, and the autoclick slot editor validates before POSTing (it caught a fire-and-forget `onchange` whose late re-render wiped the warning the next edit had just shown) |
| `check_password_ui.sh` | the change-password panel: hidden when the firmware has no store or setup mode refuses it, a mistyped confirmation never reaching the wire, the device's specific rejection shown rather than a generic failure, and the hint still naming the reflash recovery path |
| `check_build.sh` | **links a real firmware for every board**, plus all three `ABS_MOUSE_MODE` forms and the no-web build, plus every per-module image and the no-keyboard build; skips cleanly without `PICO_SDK_PATH`/`arm-none-eabi-gcc`. The only check that would have noticed the tree not compiling at all — `check_export.sh` compiles an exported keymap but never links an image |

`check_layer_refs.py` is called by `check_export.sh` rather than run directly.

The jsdom-based ones need `npm i jsdom` and skip cleanly without it. Pin the
major version — `npm i jsdom@24` — because jsdom 25+ pulls an ESM-only
transitive dependency that these CommonJS checks cannot `require()` on Node 22.
The wrappers treat that as "not installed" and skip, so a bad version looks like
the checks passing rather than failing.

## `web/`

| | |
| --- | --- |
| `build_web_html.py` | `NetHID.html` → `include/web_html.h`. Run after editing the page. |
| `cstring_to_html.py` | the reverse, to recover an editable page from the header |
| `html_to_cstring.py` | the underlying escaper |
| `preview.py` | writes `preview.html` with a mock API layer; `--serve` opens it |
| `preview/mock.js` | the mock itself, injected into a copy — never into `NetHID.html`, whose every byte lives in flash |
| `export_capture.js` | drives the Export button under jsdom, for `check_export.sh` |

## `keyboard/`

| | |
| --- | --- |
| `json_to_keymap.py` | an exported keymap JSON → `keymap.cpp`, written through the board's `LAYOUT()` |
| `kle_to_layout.py` | keyboard-layout-editor.com ↔ `LAYOUT_GEOMETRY`, both directions |
| `mkfont.py` | the OLED 5x7 font, from pixel art; `--check` renders glyphs back |

## `cert/`

| | |
| --- | --- |
| `make-cert-mkcert.sh` | a locally trusted certificate for development |
| `make-cert-letsencrypt.sh` | a real one |
| `gen_cert_header.py` | PEM → `include/server_cert.h` |
