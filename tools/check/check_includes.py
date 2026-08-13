#!/usr/bin/env python3
"""
check_includes.py — enforce header-ordering conventions that only show up as
confusing errors deep inside someone else's header.

Currently one rule, learned the hard way:

  nethid.h declares the HID command queue in terms of TinyUSB's
  hid_keyboard_report_t / hid_mouse_report_t and does NOT include tusb.h
  itself. Any file including it must include tusb.h first, or the error
  surfaces as "'hid_keyboard_report_t' does not name a type" pointing at
  nethid.h — a file the author never touched.

The rule applies TRANSITIVELY, which is the part that is easy to miss. A
header of our own that includes nethid.h silently inherits the obligation and
passes it to everyone who includes it — so a new header can break files that
never mentioned nethid.h and whose authors have no reason to suspect it. The
fix is nearly always to take nethid.h out of the intermediate header (forward
declare, or move the type) rather than to sprinkle tusb.h around.

Run it before a build; it is a second of work and it does not require the
cross-compiler.

    python3 tools/check/check_includes.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INC = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]', re.M)


def sources():
    for d in ("src", "include", "keyboards"):
        for ext in ("*.c", "*.cpp", "*.h"):
            yield from (ROOT / d).rglob(ext)


def includes_of(path):
    return INC.findall(path.read_text(errors="ignore"))


def first_unsafe(order, unsafe):
    """The first include that arrives needing tusb.h and does not have it.

    A file is fine if tusb.h comes first — and a HEADER that does that also
    settles the matter for everyone who includes it, which is why kb/kb.h can
    reach nethid.h through kb/keycodes.h and break nothing: keycodes.h puts
    tusb.h in front of it. Only a header that reaches nethid.h with nothing in
    front passes the obligation on.
    """
    for i, name in enumerate(order):
        if name == "tusb.h":
            return None
        if name in unsafe:
            return name
    return None


def unsafe_headers():
    """Our headers that oblige their includers to include tusb.h first.

    Keyed by the basename an #include names, which is how the tree refers to
    them — "kb/foo.h" is matched on its tail, so both spellings resolve.
    """
    by_name = {}
    for f in sources():
        if f.suffix != ".h" or "vendor" in f.parts:
            continue
        by_name[f.name] = f

    unsafe = {"nethid.h"}               # includes no tusb.h of its own
    changed = True
    while changed:                      # transitive closure, tiny tree
        changed = False
        for name, f in by_name.items():
            if name in unsafe:
                continue
            order = [Path(i).name for i in includes_of(f)]
            if first_unsafe(order, unsafe):
                unsafe.add(name)
                changed = True
    return unsafe


def main() -> int:
    unsafe = unsafe_headers()
    bad = []
    for f in sources():
        if "vendor" in f.parts:
            continue                      # third-party, not ours to reorder
        if f.name in unsafe and f.name != "nethid.h":
            continue                      # reported against its includers
        via = first_unsafe([Path(i).name for i in includes_of(f)], unsafe)
        if via:
            bad.append((f.relative_to(ROOT), via))

    for f, via in bad:
        how = "nethid.h" if via == "nethid.h" else f"{via}, which reaches nethid.h"
        print(f"{f}: includes {how} without tusb.h before it")
    if bad:
        print("\nnethid.h needs TinyUSB's report types already declared.")
        print("Either add #include \"tusb.h\" above it, or — if the file does not")
        print("actually use the HID queue — drop the nethid.h include entirely.")
        print("If the culprit is a header of ours, prefer taking nethid.h OUT of")
        print("it: an intermediate header hands this rule to every file that")
        print("includes it, including files that never name a HID type.")
        return 1

    print("include order OK (%d header(s) pass the nethid.h obligation on)"
          % (len(unsafe) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
