#!/usr/bin/env python3
"""
check_layout_tables.py — the keyboard layout is described twice. Hold the two
descriptions to the same story.

    python3 tools/check/check_layout_tables.py

  1. src/hid.cpp + src/hid_layout.cpp   which key the firmware PRESSES to type
                                        a character
  2. NetHID.html KC_FACE_US/_UK         which key the editor SAYS prints it

They are separate because they answer for different sides of the wire, and
neither can import the other. When they disagree the result is uniquely
horrible: the picker tells you QUOT is the @ key, you assign it, and the typer
goes on sending shift-2 — so the page and the device are both confidently
wrong in opposite directions and nothing anywhere logs a thing.

Also checks the two smaller ways a layout setting can go quietly wrong:
the settings field's help text has to survive being pasted into JSON, and
the compiled default has to name a layout that exists.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The typer never presses a keypad key: whether the pad emits digits at all is
# the host's NumLock state, which the device cannot see. The pad's faces are
# still shown, because you can map a key to one.
KEYPAD = {"NUM", "PSLS", "PAST", "PMNS", "PPLS", "PENT", "PEQL", "PCMM",
          "PDOT"} | {"P%d" % i for i in range(10)}

# Keys belonging to layouts other than the two here. Their faces describe a JIS
# or Hangul board and have nothing to say about US or UK.
OTHER_LAYOUT = {"RO", "KANA", "JYEN", "HENK", "MHEN", "HAEN", "HANJ"}

# A character the page shows on a key the firmware does NOT use for it. Every
# one of these needs a reason, because the default reading is "one of the two
# is wrong".
ALLOWED_DUPLICATES = {
    # ISO keys. A US board does not have them, so the US typer reaches the
    # character by another route — but an ISO board wired to a US host still
    # has the physical keys and you must be able to map them.
    ("US", "NUHS", "#"), ("US", "NUHS", "~"),
    ("US", "NUBS", "\\"), ("US", "NUBS", "|"),
    # Windows maps usage 0x31 to the same scan code as the ISO hash key, so on
    # UK the ANSI backslash key really does type # and ~. The typer uses the
    # ISO key (0x32) for both, which is the one an ISO board actually has.
    ("UK", "BSLS", "#"), ("UK", "BSLS", "~"),
}

C_ENTRY = re.compile(r"\{\s*'((?:\\.|[^'\\]))'\s*,\s*(\w+)\s*,\s*(KEY_\w+)\s*\}")
UNESCAPE = {"\\\\": "\\", "\\'": "'", "\\n": "\n", "\\t": "\t", "\\0": "\0"}


def unescape(tok):
    return UNESCAPE.get(tok, tok)


def key_values():
    """KEY_* -> usage, and KC_* -> usage, so the two tables can be compared."""
    src = (ROOT / "include" / "nethid.h").read_text()
    keys = {n: int(v, 16)
            for n, v in re.findall(r"#define\s+(KEY_\w+)\s+(0x[0-9A-Fa-f]+)", src)}

    kc = {}
    src = (ROOT / "include" / "kb" / "keycodes.h").read_text()
    for name, rhs in re.findall(r"#define\s+(KC_\w+)\s+(\S+)", src):
        rhs = rhs.strip()
        if rhs in keys:
            kc[name[3:]] = keys[rhs]
        elif rhs.startswith("0x"):
            kc[name[3:]] = int(rhs, 16)
    # The page builds the letter/digit/keypad names in a loop rather than
    # naming them, and keycodes.h does the same, so fill in what both imply.
    for i, ch in enumerate("1234567890"):
        kc.setdefault(ch, 0x1E + i)
    return keys, kc


def firmware_map(keys):
    """layout -> {char: (shift, usage)}, exactly as ascii_to_hid resolves it."""
    us = {}
    body = (ROOT / "src" / "hid.cpp").read_text()
    m = re.search(r"struct \{ char ch; uint8_t mod; uint8_t kc; \} table\[\] = \{(.*?)\n    \};",
                  body, re.S)
    if not m:
        return None
    for ch, mod, kc in C_ENTRY.findall(m.group(1)):
        if kc not in keys:
            continue
        us[unescape(ch)] = (mod != "0", keys[kc])

    # Letters and 1-9 are range tests above the table, not rows in it. Read the
    # ranges rather than assuming them, so deleting one is a failure here and
    # not a checker that quietly stops covering a third of the alphabet.
    for lo, hi, base, shift in (("a", "z", "KEY_A", False),
                                ("A", "Z", "KEY_A", True),
                                ("1", "9", "KEY_1", False)):
        pat = r"c >= '%s' && c <= '%s'.*?keycode = %s" % (lo, hi, base)
        if not re.search(pat, body, re.S):
            return None
        for i in range(ord(lo), ord(hi) + 1):
            us[chr(i)] = (shift, keys[base] + i - ord(lo))

    out = {"US": us, "UK": dict(us)}
    body = (ROOT / "src" / "hid_layout.cpp").read_text()
    m = re.search(r"UK\[\] = \{(.*?)\n\};", body, re.S)
    if not m:
        return None
    for ch, mod, kc in C_ENTRY.findall(m.group(1)):
        if kc not in keys:
            continue
        out["UK"][unescape(ch)] = (mod != "0", keys[kc])
    return out


def page_faces():
    """layout -> {kc_name: [unshifted, shifted]}, merged the way the page does."""
    js = (ROOT / "NetHID.html").read_text()

    def table(name):
        m = re.search(r"const %s = \{(.*?)\n\};" % name, js, re.S)
        if not m:
            return None
        body = re.sub(r"//[^\n]*", "", m.group(1))
        return dict(re.findall(
            r"(\w+)\s*:\s*\[\s*'((?:[^'\\]|\\.)*)'\s*,\s*'(?:[^'\\]|\\.)*'\s*\]",
            body))

    us, uk = table("KC_FACE_US"), table("KC_FACE_UK")
    if us is None or uk is None:
        return None

    def split(face):
        # Source text: '\' "' is apostrophe then double quote. Un-escape first,
        # then the two faces are separated by a space.
        f = face.replace("\\\\", "\x00").replace("\\'", "'").replace("\x00", "\\")
        return f.split(" ")

    # A key whose NAME is already the character it prints needs no face on the
    # page — the button says "A". Fill those in, or this check would read the
    # absence as the picker having nothing to say about the letter A.
    letters = {c: [c.lower(), c] for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"}

    base = {**letters, **{k: split(v) for k, v in us.items()}}
    return {"US": base,
            "UK": {**base, **{k: split(v) for k, v in uk.items()}}}


def check_settings_help():
    """Help strings are interpolated into JSON with no escaping at all."""
    bad = []
    src = (ROOT / "src" / "settings.cpp").read_text()
    m = re.search(r"static const sfield_t FIELDS\[\] = \{(.*?)\n\};", src, re.S)
    if not m:
        return ["FIELDS table not found in src/settings.cpp"]
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
        if "\\" in lit:
            bad.append("settings field text %r contains a backslash — it is "
                       "printed into JSON with %%s and no escaping, so the "
                       "settings page would fail to parse and render nothing"
                       % lit)
    return bad


def check_default(names):
    bad = []
    src = (ROOT / "include" / "config.h").read_text()
    m = re.search(r"#define\s+KEYBOARD_LAYOUT\s+(\d+)", src)
    if not m:
        return ["config.h has no KEYBOARD_LAYOUT default"]
    v = int(m.group(1))
    if v >= len(names):
        bad.append("config.h KEYBOARD_LAYOUT is %d but only %d layouts exist "
                   "(%s) — a build with the settings store off would index off "
                   "the end" % (v, len(names), ", ".join(names)))
    return bad


def main() -> int:
    bad = []
    keys, kc = key_values()
    fw = firmware_map(keys)
    faces = page_faces()

    src = (ROOT / "src" / "hid_layout.cpp").read_text()
    m = re.search(r"HID_LAYOUT_NAMES\[HID_LAYOUT_COUNT \+ 1\] = \{(.*?)\}", src, re.S)
    names = re.findall(r'"(\w+)"', m.group(1)) if m else []

    if fw is None:
        bad.append("could not read the ASCII tables out of src/hid.cpp or "
                   "src/hid_layout.cpp — they were renamed or reshaped")
    if faces is None:
        bad.append("KC_FACE_US / KC_FACE_UK not found in NetHID.html")
    if not names:
        bad.append("HID_LAYOUT_NAMES not found in src/hid_layout.cpp")

    if fw and faces and names:
        if sorted(names) != sorted(fw):
            bad.append("HID_LAYOUT_NAMES is %s but this check knows %s — a new "
                       "layout needs a face table in NetHID.html and a row here"
                       % (names, sorted(fw)))

        for lay in names:
            fmap, fc = fw[lay], faces[lay]

            # 1. Every character the firmware can type must be advertised on
            #    the key it actually presses. Otherwise the picker cannot
            #    answer "which key types this", which is the whole point.
            for ch, (shift, usage) in sorted(fmap.items()):
                if not ch.strip() or ch in "\n\t":
                    continue
                found = [n for n, f in fc.items()
                         if n not in KEYPAD and n not in OTHER_LAYOUT
                         and len(f) > int(shift) and f[int(shift)] == ch
                         and kc.get(n) == usage]
                if not found:
                    owner = next((n for n, v in kc.items() if v == usage), hex(usage))
                    bad.append("%s: the typer sends %s%s for %r, but the picker "
                               "does not show %r on it — search the picker for "
                               "that character and you get the wrong key or no "
                               "key" % (lay, "shift+" if shift else "", owner,
                                        ch, ch))

            # 2. And the reverse: a face that claims a character the firmware
            #    reaches by another key is the page lying about the device.
            for n, f in sorted(fc.items()):
                if n in KEYPAD or n in OTHER_LAYOUT or n not in kc:
                    continue
                for i, ch in enumerate(f):
                    if not ch or ord(ch[0]) > 0x7E:
                        continue          # £ ¬ ¥ — real, but not ASCII
                    if (lay, n, ch) in ALLOWED_DUPLICATES:
                        continue
                    want = fmap.get(ch)
                    if want is None:
                        bad.append("%s: the picker shows %r on %s but the typer "
                                   "cannot type %r at all" % (lay, ch, n, ch))
                    elif want != (bool(i), kc[n]):
                        owner = next((k for k, v in kc.items()
                                      if v == want[1]), hex(want[1]))
                        bad.append("%s: the picker shows %r on %s, but the typer "
                                   "sends %s%s for it — one of the two is wrong, "
                                   "and neither says so"
                                   % (lay, ch, n,
                                      "shift+" if want[0] else "", owner))

    bad += check_settings_help()
    if names:
        bad += check_default(names)

    for b in bad:
        print("  " + b)
    if bad:
        print("\nThe firmware types by pressing a key; the editor explains which\n"
              "key that is. When those two disagree the user is told one thing\n"
              "and sent another, and the device looks broken rather than\n"
              "misconfigured.")
        return 1

    print("layout tables agree (%s; %d chars checked)"
          % ("/".join(names), sum(len(v) for v in fw.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
