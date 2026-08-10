#!/usr/bin/env python3
"""
check_keycode_tables.py — the keycode table exists in three places. Hold them
in step.

    python3 tools/check/check_keycode_tables.py

  1. NetHID.html            what the keymap editor OFFERS
  2. json_to_keymap.py      what the export WRITES ("KC_" + the editor's name)
  3. include/kb/keycodes.h  what the firmware can COMPILE

A name in 1 with no define in 3 is the worst of the three failures, because
nothing catches it until someone exports a keymap and their build breaks with
"KC_P1 was not declared in this scope". That is not hypothetical: the editor
offered the whole number pad, plus NUHS, for as long as it has existed, and the
firmware header defined none of them.

2 is a hand-written mirror of the page's JavaScript — it re-implements the same
loops in Python — so it drifts the moment the page grows a keycode the mirror
does not know. The export then writes a bare hex literal with a comment instead
of a name, which does compile, and is silently unreviewable.
"""

import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HTML = os.path.join(ROOT, "NetHID.html")
KEYCODES_H = os.path.join(ROOT, "include", "kb", "keycodes.h")
EXPORTER = os.path.join(ROOT, "tools", "keyboard", "json_to_keymap.py")

# KC_NO and KC_TRNS are quantum values, not usages, and the exporter special
# cases them before it ever looks at a table.
SPECIAL = {"NO", "TRNS"}


def page_kc():
    """The editor's KC table, evaluated the way the browser builds it."""
    js = open(HTML, encoding="utf-8").read()
    kc = {}
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
    for m in re.finditer(r"Object\.assign\(KC,\s*\{(.*?)\}\);", js, re.S):
        for name, val in re.findall(r"(\w+)\s*:\s*(0x[0-9A-Fa-f]+)", m.group(1)):
            kc[name] = int(val, 16)
    return kc


def header_kc():
    """KC_<name> -> value, resolving the KEY_* indirection in nethid.h."""
    keys = {}
    src = open(os.path.join(ROOT, "include", "nethid.h"), encoding="utf-8").read()
    for name, val in re.findall(r"#define\s+(KEY_\w+)\s+(0x[0-9A-Fa-f]+)", src):
        keys[name] = int(val, 16)

    out = {}
    src = open(KEYCODES_H, encoding="utf-8").read()
    for name, rhs in re.findall(r"#define\s+(KC_\w+)\s+(\S+)", src):
        rhs = rhs.strip()
        if rhs in keys:
            out[name] = keys[rhs]
        elif rhs.startswith("0x"):
            out[name] = int(rhs, 16)
        elif rhs in out:
            out[name] = out[rhs]              # alias of an earlier KC_
    return out


def exporter_kc():
    spec = importlib.util.spec_from_file_location("json_to_keymap", EXPORTER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.KC, mod


def page_groups():
    """Names listed in KC_GROUPS, to catch a typo that hides a key."""
    js = open(HTML, encoding="utf-8").read()
    m = re.search(r"const KC_GROUPS = \[(.*?)\n\];", js, re.S)
    if not m:
        return None
    # Only the third element of each entry — the name list. A flat scan for
    # quoted tokens also picks up the labels and keyword strings, and then
    # reports "KEYPAD is not a keycode", which is true and useless.
    names = []
    for entry in re.finditer(r"\[\s*'[^']*',\s*'[^']*',\s*\[(.*?)\]\s*\]",
                             m.group(1), re.S):
        names += re.findall(r"'([A-Z0-9_]+)'", entry.group(1))
    return names


def main() -> int:
    bad = []
    page = page_kc()
    hdr = header_kc()
    exp, mod = exporter_kc()

    # 1 vs 3 — the one that breaks a user's build.
    for name, val in sorted(page.items()):
        if name in SPECIAL:
            continue
        key = "KC_" + name
        if key not in hdr:
            bad.append("%s (0x%02X) is offered by the editor but "
                       "include/kb/keycodes.h has no %s — exporting a keymap "
                       "that uses it will not compile" % (name, val, key))
        elif hdr[key] != val:
            bad.append("%s is 0x%02X in the editor but %s is 0x%02X in "
                       "keycodes.h" % (name, val, key, hdr[key]))

    # 1 vs 2 — drift in the exporter's hand-written mirror.
    for name, val in sorted(page.items()):
        if name not in exp:
            bad.append("%s is in the editor's table but not the exporter's "
                       "mirror — export would write a bare hex literal" % name)
        elif exp[name] != val:
            bad.append("%s is 0x%02X in the editor but 0x%02X in the exporter"
                       % (name, val, exp[name]))
    for name in sorted(exp):
        if name not in page:
            bad.append("%s is in the exporter's mirror but not the editor"
                       % name)

    # Round-trip every usage the editor offers, which is what export actually
    # does. Catches a value two names share resolving to the wrong one.
    for name, val in sorted(page.items()):
        if name in SPECIAL:
            continue
        got = mod.kc_name(val)
        if got.startswith("0x"):
            bad.append("0x%02X (%s) exports as a bare literal: %s"
                       % (val, name, got))
        elif "KC_" + name != got and hdr.get(got) != val:
            bad.append("0x%02X exports as %s, which is not that usage" % (val, got))

    # A group listing a name that does not exist silently shows nothing.
    groups = page_groups()
    if groups is None:
        bad.append("KC_GROUPS not found in NetHID.html — the picker's grouping "
                   "is gone, or it was renamed")
    else:
        for n in groups:
            if n not in page:
                bad.append("KC_GROUPS lists %s, which is not in KC — that "
                           "button simply never appears" % n)

    for b in bad:
        print("  " + b)
    if bad:
        print("\nThe editor, the exporter and the firmware must agree on every "
              "keycode.\nA key you can assign but cannot export, or export but "
              "not compile, is\nworse than one that is missing: it looks like it "
              "worked.")
        return 1

    print("keycode tables agree (%d usages offered, %d names in keycodes.h)"
          % (len(page) - len(SPECIAL), len(hdr)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
