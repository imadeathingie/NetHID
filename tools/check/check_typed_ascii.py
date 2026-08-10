#!/usr/bin/env python3
"""
check_typed_ascii.py — find non-ASCII in strings that get typed into the host.

The HID typer maps characters to keycodes and has no entry above 0x7E. Until
recently it skipped what it could not map, so an em-dash in a diagnostic just
disappeared and the message read as truncated with nothing to explain it.
dbg() now substitutes '-', but a string full of substitutions is still worse
than one written in ASCII to begin with.

This flags literals passed to dbg() and type_to_host(), and those assembled
into the buffers those calls use, that contain non-ASCII.
Comments and printf-only output are not checked — a serial console handles
UTF-8 fine, and the prose in comments is not going anywhere near a keycode.

    python3 tools/check/check_typed_ascii.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# A string literal appearing anywhere inside a dbg(...) / type_to_host(...) call,
# including the concatenated "[" NETHID_BUILD "] ..." form.
CALL = re.compile(r'\b(?:dbg|type_to_host)\s*\(([^;]*?)\)\s*;', re.S)
# Messages assembled first and typed later. Matched by the buffer names actually
# used for that in this codebase; a new one needs adding here.
BUILT = re.compile(r'snprintf\s*\(\s*(?:msg|banner|d)\b([^;]*?)\)\s*;', re.S)
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def main() -> int:
    bad = []
    for f in list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.c")):
        if "vendor" in f.parts:
            continue
        text = f.read_text(errors="ignore")
        for m in list(CALL.finditer(text)) + list(BUILT.finditer(text)):
            for lit in LIT.finditer(m.group(1)):
                s = lit.group(1)
                if any(ord(ch) > 0x7E for ch in s):
                    line = text[: m.start()].count("\n") + 1
                    offenders = sorted({ch for ch in s if ord(ch) > 0x7E})
                    bad.append((f.relative_to(ROOT), line, "".join(offenders), s[:60]))

    for f, line, chars, snippet in bad:
        print(f"{f}:{line}: non-ASCII {chars!r} in typed string: {snippet!r}")
    if bad:
        print("\nThese are typed into the host one keycode at a time. Use ASCII:")
        print("  em dash / en dash -> '-' or ': '")
        print("  curly quotes      -> straight quotes")
        print("  ellipsis          -> '...'")
        return 1

    print("typed strings are ASCII")
    return 0


if __name__ == "__main__":
    sys.exit(main())
