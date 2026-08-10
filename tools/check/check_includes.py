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

def main() -> int:
    bad = []
    for f in sources():
        if "vendor" in f.parts:
            continue                      # third-party, not ours to reorder
        order = INC.findall(f.read_text(errors="ignore"))
        if "nethid.h" not in order:
            continue
        before = order[:order.index("nethid.h")]
        if "tusb.h" not in before:
            bad.append(f.relative_to(ROOT))

    for f in bad:
        print(f"{f}: includes nethid.h without tusb.h before it")
    if bad:
        print("\nnethid.h needs TinyUSB's report types already declared.")
        print("Either add #include \"tusb.h\" above it, or — if the file does not")
        print("actually use the HID queue — drop the nethid.h include entirely.")
        return 1

    print("include order OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
