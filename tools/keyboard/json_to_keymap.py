#!/usr/bin/env python3
"""
json_to_keymap.py — turn an exported keymap JSON into a keymap.cpp.

The same conversion the web editor's "Export C" button does, for a file you
already saved:

    python3 tools/keyboard/json_to_keymap.py nethid-keymap-modular.json > keymap.cpp

Unlike the browser, this has the source tree — so it reads the board's LAYOUT()
macro and writes the keymap in that readable, physical form rather than as a raw
[row][col] array. It also keeps the original line breaks of the argument list,
so the output looks like the board rather than like a matrix.

The keycode names are read out of NetHID.html at run time rather than being
duplicated here. Two hand-maintained copies of a 200-entry table drift, and the
drift is silent: an export that names the wrong key still compiles.
"""

import argparse
import json
import os
import re
import sys
from datetime import date

ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))
HTML = os.path.join(ROOT, "NetHID.html")

QK = {"MODS": 0x0100, "RMOD": 0x1000, "MOD_TAP": 0x2000, "LAYER_TAP": 0x4000,
      "MO": 0x5100, "DF": 0x5200, "TG": 0x5300, "OSL": 0x5400, "TO": 0x5500,
      "OSM": 0x5600, "CAPSWRD": 0x5700, "BOOT": 0x5800, "MOUSE": 0x5900,
      "CONSUMER": 0x5A00, "AUTOCLICK": 0x5B00, "MACRO": 0x7E00}


def load_tables():
    """Pull KC / MS / CC out of the page's own JavaScript."""
    js = open(HTML, encoding="utf-8").read()
    kc = {}

    # KC.A .. KC.Z and the digits are built with loops; mirror them.
    for i, ch in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
        kc[ch] = 0x04 + i
    for i, ch in enumerate("1234567890"):
        kc[ch] = 0x1E + i
    for i in range(1, 13):
        kc["F%d" % i] = 0x39 + i
    for i in range(13, 25):
        kc["F%d" % i] = 0x68 + i - 13
    for i in range(1, 10):
        kc["P%d" % i] = 0x58 + i
    kc["P0"] = 0x62
    kc["NO"] = 0x00
    kc["TRNS"] = 0x01

    # EVERY Object.assign(KC, {...}) block, not just the first. The page grew a
    # second one for the rest of the usage page (keypad extras, ISO, editing and
    # international keys); a re.search would have stopped at the first and this
    # exporter would then have written raw hex for keys the editor offers by
    # name, silently.
    for m in re.finditer(r"Object\.assign\(KC,\s*\{(.*?)\}\);", js, re.S):
        for name, val in re.findall(r"(\w+)\s*:\s*(0x[0-9A-Fa-f]+)", m.group(1)):
            kc[name] = int(val, 16)

    def name_list(var):
        mm = re.search(r"\[\s*((?:'[A-Z_0-9]+',?\s*)+)\]\s*\n?\s*\.forEach\(\(n,i\)\s*=>\s*"
                       + var, js)
        return re.findall(r"'([A-Z_0-9]+)'", mm.group(1)) if mm else []

    ms = {n: QK["MOUSE"] | i for i, n in enumerate(name_list("MS"))}
    cc = {n.replace("KC_", ""): QK["CONSUMER"] | i
          for i, n in enumerate(name_list("CC"))}
    return kc, ms, cc


KC, MS, CC = load_tables()
KC_BY_VAL = {}
for n, v in KC.items():
    KC_BY_VAL.setdefault(v, n)
MS_BY_VAL = {v: n for n, v in MS.items()}
CC_BY_VAL = {v: n for n, v in CC.items()}


def mod_expr(bits, right):
    p = "QK_R" if right else "QK_L"
    out = [p + s for b, s in ((1, "CTL"), (2, "SFT"), (4, "ALT"), (8, "GUI")) if bits & b]
    return " | ".join(out) if out else "0"


def kc_name(kc):
    if kc == 0:
        return "KC_NO"
    if kc == 1:
        return "KC_TRNS"
    if kc <= 0xFF:
        n = KC_BY_VAL.get(kc)
        return "KC_" + n if n else "0x%02X /* unnamed usage */" % kc

    hi, lo = kc & 0xFF00, kc & 0xFF
    if hi == QK["MOUSE"]:
        return MS_BY_VAL.get(kc, "0x%04X" % kc)
    if hi == QK["CONSUMER"]:
        n = CC_BY_VAL.get(kc)
        return "KC_" + n if n else "0x%04X" % kc
    if kc == QK["CAPSWRD"]:
        return "CAPSWRD"
    if kc == QK["BOOT"]:
        return "QK_BOOT"
    for tag in ("MO", "DF", "TG", "OSL", "TO"):
        if hi == QK[tag]:
            return "%s(%d)" % (tag, lo)
    if hi == QK["MACRO"]:
        return "KB_MACRO(%d)" % lo
    if hi == QK["AUTOCLICK"]:
        return "AUTOCLK(%d)" % lo
    if hi == QK["OSM"]:
        right = bool(lo & 0xF0)
        return "OSM(%s)" % mod_expr(lo >> 4 if right else lo, right)

    basic = kc_name(lo)
    if QK["LAYER_TAP"] <= kc <= 0x4FFF:
        return "LT(%d, %s)" % ((kc >> 8) & 0xF, basic)

    right = bool(kc & QK["RMOD"])
    bits = (kc >> 8) & 0xF
    if QK["MOD_TAP"] <= kc <= 0x3FFF:
        return "MT(%s, %s)" % (mod_expr(bits, right), basic)
    if QK["MODS"] <= kc <= 0x1FFF:
        one = {1: "CTL", 2: "SFT", 4: "ALT", 8: "GUI"}.get(bits)
        if one:
            return "%s%s(%s)" % ("R" if right else "L", one, basic)
        return "(%s | %s)" % (mod_expr(bits, right), basic)
    return "0x%04X /* unrecognised */" % kc


def step_kc(st):
    key = kc_name(st.get("key", 0))
    mod = st.get("mod", 0)
    if not mod:
        return key
    right = bool(mod & 0xF0)
    return "(%s | %s)" % (mod_expr(mod >> 4 if right else mod & 0x0F, right), key)


# ── The board's LAYOUT() macro ───────────────────────────────────────────────

def read_layout(board_dir):
    """Parse LAYOUT() into (arg_lines, {arg: (row, col)}).

    arg_lines preserves the macro's own line breaks. That grouping is the only
    thing that makes a split or staggered layout readable, and it is free to
    keep — throwing it away would produce a technically correct file that nobody
    wants to edit.

    Returns None if the board has no LAYOUT() or it cannot be parsed, in which
    case the caller falls back to a raw array rather than guessing.
    """
    hdr = os.path.join(board_dir, "keyboard.h")
    if not os.path.exists(hdr):
        return None
    text = open(hdr, encoding="utf-8").read()

    m = re.search(r"#\s*define\s+LAYOUT\s*\(", text)
    if not m:
        return None

    # Balance parentheses to find the end of the argument list.
    i = m.end()
    depth, start = 1, i
    while i < len(text) and depth:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
        i += 1
    if depth:
        return None
    params_src = text[start:i - 1]

    # Body: the brace block that follows.
    b = text.index("{", i)
    depth, j = 0, b
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    body = text[b:j + 1]

    # Keep the argument list's line structure, minus continuations.
    arg_lines = []
    for line in params_src.split("\n"):
        names = re.findall(r"\b([A-Za-z_]\w*)\b", line.replace("\\", " "))
        if names:
            arg_lines.append(names)
    if not arg_lines:
        return None

    # Body rows: each inner { ... } is one matrix row, in order.
    rows = re.findall(r"\{([^{}]*)\}", body)
    pos = {}
    for r, row in enumerate(rows):
        for c, tok in enumerate([t.strip() for t in row.split(",")]):
            if re.fullmatch(r"[A-Za-z_]\w*", tok):
                pos.setdefault(tok, (r, c))

    flat = [a for line in arg_lines for a in line]
    missing = [a for a in flat if a not in pos]
    if missing:
        print("note: LAYOUT() args not found in its body: %s - using a raw array"
              % ", ".join(missing[:4]), file=sys.stderr)
        return None
    return arg_lines, pos


def emit_layout_layer(layer, cols, arg_lines, pos, indent="        "):
    """One layer, written through LAYOUT(), keeping the macro's line breaks."""
    out = []
    total = sum(len(l) for l in arg_lines)
    seen = 0
    # Column-align within each line so the result stays readable.
    width = 0
    for line in arg_lines:
        for a in line:
            r, c = pos[a]
            width = max(width, len(kc_name(layer[r * cols + c])))
    for line in arg_lines:
        cells = []
        for a in line:
            r, c = pos[a]
            cells.append(kc_name(layer[r * cols + c]).ljust(width))
            seen += 1
        sep = "," if seen < total else ""
        out.append(indent + ", ".join(x.rstrip() if i == len(cells) - 1 else x
                                      for i, x in enumerate(cells)) + sep)
    return out


KC_TRNS = 1

# Keycodes that name a layer, and where that layer number sits in the encoding.
_LAYER_OPS = ("MO", "DF", "TG", "OSL", "TO")


def highest_referenced_layer(d):
    """The largest layer number any keycode switches to, or -1 for none.

    Covers MO/DF/TG/OSL/TO, which carry the layer in the low byte, and LT(),
    which packs it into bits 11..8.
    """
    top = -1

    def scan(kc):
        nonlocal top
        hi, lo = kc & 0xFF00, kc & 0xFF
        for tag in _LAYER_OPS:
            if hi == QK[tag]:
                top = max(top, lo)
                return
        if QK["LAYER_TAP"] <= kc <= 0x4FFF:
            top = max(top, (kc >> 8) & 0xF)

    for layer in d.get("layers", []):
        for kc in layer:
            scan(kc)
    for layer in (d.get("encoder_map") or []):
        for e in (layer or []):
            for kc in e:
                scan(kc)
    return top


def used_layer_count(d):
    """How many layers to emit.

    The store always holds KB_MAX_LAYERS (8) so a layer can be added from the
    web UI without a reflash, but exporting eight layers when six are entirely
    transparent produces a file that is mostly noise.

    Only TRAILING empties are dropped. Layer indices are positional — MO(3)
    means the fourth entry — so an empty layer with a populated one above it has
    to stay or everything above it shifts down and the keymap silently means
    something else.

    A layer counts as empty only when its keys AND its encoder entries are all
    KC_TRNS. Checking the keys alone would throw away a layer whose only job is
    to remap a knob, which is a perfectly reasonable thing for a layer to do.

    A layer that something REFERS to is kept even when empty. MO(1) on the base
    layer with no layer 1 in the file is not a smaller keymap, it is a broken
    one: kb_layer_lookup() skips layers past keymap_layer_count, so the key
    silently does nothing. An empty layer that a key switches to is a normal
    thing to have — it is how you make a layer that only blocks keys.

    Layer 0 is always kept: KC_TRNS on the base layer has nothing to fall
    through to, and a keymap with no layers does not compile.
    """
    layers = d["layers"]
    enc = d.get("encoder_map") or []
    n = len(layers)
    floor = max(1, highest_referenced_layer(d) + 1)
    while n > floor:
        i = n - 1
        keys_empty = all(k == KC_TRNS for k in layers[i])
        enc_empty = True
        if i < len(enc):
            for e in (enc[i] or []):
                if any(a != KC_TRNS for a in e):
                    enc_empty = False
                    break
        if not (keys_empty and enc_empty):
            break
        n -= 1
    return n


def generate(d, board_dir=None):
    rows, cols = d["rows"], d["cols"]
    macros = {k: v for k, v in (d.get("macros") or {}).items() if v}
    layout = read_layout(board_dir) if board_dir else None
    nlayers = used_layer_count(d)
    dropped = len(d["layers"]) - nlayers
    out = []
    w = out.append

    w("/*")
    w(" * %s :: exported keymap" % d.get("board", "?"))
    w(" *")
    w(" * Generated by tools/keyboard/json_to_keymap.py on %s." % date.today().isoformat())
    w(" * Drop this in as keyboards/%s/keymaps/<name>/keymap.cpp" % d.get("board", "?"))
    w(" *")
    if dropped:
        w(" * %d trailing layer(s) were entirely KC_TRNS and have been left out." % dropped)
        w(" * The store keeps %d in RAM regardless, so they are still there to use" % len(d["layers"]))
        w(" * from the web editor; they are simply not worth writing down.")
        w(" *")
    if layout:
        w(" * Written through the board's own LAYOUT() macro, with its line breaks")
        w(" * preserved, so this reads the way the keyboard is laid out.")
    else:
        w(" * Written as a raw [row][col] array: this board defines no LAYOUT() macro,")
        w(" * or it could not be parsed. The row comments show the matrix positions.")
    w(" */")
    w("")
    w('#include "kb/kb.h"')
    if d.get("encoders"):
        w('#include "kb/encoder.h"')
    if macros:
        w('#include "kb/macros.h"')
    w('#include "kb/layers.h"')
    w("")
    w("const kb_keycode_t keymaps[][MATRIX_ROWS][MATRIX_COLS] = {")
    for li, layer in enumerate(d["layers"][:nlayers]):
        if layout:
            arg_lines, pos = layout
            w("    [%d] = LAYOUT(" % li)
            for line in emit_layout_layer(layer, cols, arg_lines, pos):
                w(line)
            w("    ),")
        else:
            w("    [%d] = {" % li)
            for r in range(rows):
                cells = [kc_name(layer[r * cols + c]) for c in range(cols)]
                width = max(len(x) for x in cells)
                w("        { %s },  /* row %d */"
                  % (", ".join(x.ljust(width) for x in cells), r))
            w("    },")
    w("};")
    w("")
    w("const uint8_t keymap_layer_count = sizeof(keymaps) / sizeof(keymaps[0]);")

    if d.get("encoders") and d.get("encoder_map"):
        w("")
        w("/* [layer][encoder][CCW, CW, press] */")
        w("const kb_keycode_t encoder_map[][NUM_ENCODERS][3] = {")
        for li, layer in enumerate(d["encoder_map"][:nlayers]):
            entries = ["{ %s }" % ", ".join(kc_name(e[a]) for a in range(3))
                       for e in (layer or [])]
            w("    [%d] = { %s }," % (li, ", ".join(entries)))
        w("};")
        w("")
        w("const uint8_t encoder_map_layers =")
        w("    (uint8_t)(sizeof(encoder_map) / sizeof(encoder_map[0]));")

    if macros:
        w("")
        w("/*")
        w(" * Macros, as built in the web editor.")
        w(" *")
        w(" * NOTE: a compiled kb_macro_user() only runs for ids that have NO stored")
        w(" * macro. If you flash this while these ids are still saved in flash, the")
        w(" * stored versions win. Clear them first, or use different ids.")
        w(" */")
        w("bool kb_macro_user(uint8_t id, keyrecord_t *rec) {")
        w("    if (!rec->event.pressed) return false;")
        w("    switch (id) {")
        for mid in sorted(macros, key=int):
            w("    case %s:" % mid)
            for st in macros[mid]:
                t = st.get("t")
                if t == "text":
                    esc = (str(st.get("value", ""))
                           .replace("\\", "\\\\").replace('"', '\\"')
                           .replace("\n", "\\n"))
                    w('        kb_macro_string("%s");' % esc)
                elif t == "delay":
                    w("        /* delay %s ms - a compiled macro runs inline, so a"
                      % st.get("ms", 0))
                    w("           blocking wait here would stall the scan loop. Use the")
                    w("           stored macro for timed sequences. */")
                elif t == "tap":
                    w("        kb_register(%s);" % step_kc(st))
                    w("        kb_unregister(%s);" % step_kc(st))
                elif t == "down":
                    w("        kb_register(%s);" % step_kc(st))
                elif t == "up":
                    w("        kb_unregister(%s);" % step_kc(st))
            w("        return false;")
        w("    }")
        w("    return false;")
        w("}")
    w("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("json")
    ap.add_argument("-o", "--output")
    ap.add_argument("--board-dir",
                    help="keyboards/<board>/ (default: derived from the file's "
                         "`board` field). Its LAYOUT() macro is used if present.")
    a = ap.parse_args()

    d = json.load(open(a.json, encoding="utf-8"))
    for k in ("rows", "cols", "layers"):
        if k not in d:
            sys.exit("not a NetHID keymap export: no '%s'" % k)
    if d.get("version", 1) < 2:
        print("note: version 1 export - it carries no macros or encoders",
              file=sys.stderr)

    board_dir = a.board_dir
    if not board_dir and d.get("board"):
        cand = os.path.join(ROOT, "keyboards", d["board"])
        if os.path.isdir(cand):
            board_dir = cand
    if board_dir and not os.path.isdir(board_dir):
        sys.exit("no such board directory: " + board_dir)
    if not board_dir:
        print("note: no board directory found - emitting a raw [row][col] array",
              file=sys.stderr)

    n = used_layer_count(d)
    if n < len(d["layers"]):
        print("note: dropped %d trailing all-transparent layer(s), kept %d"
              % (len(d["layers"]) - n, n), file=sys.stderr)

    text = generate(d, board_dir)
    if a.output:
        open(a.output, "w", encoding="utf-8").write(text)
        print("wrote", a.output, file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
