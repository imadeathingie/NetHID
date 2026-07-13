#!/usr/bin/env python3
"""
html_to_cstring.py — convert an HTML file into a C string literal for web.cpp.

WHY THIS EXISTS
---------------
web.cpp embeds the web UI as C string literals (MAIN_HTML / LOGIN_HTML).
Hand-escaping HTML into C strings is error-prone: a single mis-escaped
backslash or quote produces a page that works as a standalone .html file but
breaks when served from the Pico (the browser receives invalid JS/HTML).

The reliable workflow is:
  1. Edit the UI as a normal standalone .html file, test it in a browser.
  2. Regenerate the C literal from it with this script.
  3. Paste the output into web.cpp (replacing the old literal).

This guarantees the served bytes are identical to the file you tested.

USAGE
-----
  python3 tools/html_to_cstring.py NetHID.html                 # -> stdout
  python3 tools/html_to_cstring.py NetHID.html -n MAIN_HTML    # name the var
  python3 tools/html_to_cstring.py NetHID.html -o main.txt     # to a file

Then copy the output over the existing `static const char MAIN_HTML[] = ... ;`
block in src/web.cpp.

ROUND-TRIP
----------
cstring_to_html.py reverses this: it extracts a named literal out of web.cpp
and decodes it back to plain HTML, so you can pull the live UI out for editing.
"""

import argparse
import sys


def escape_line(line: str) -> str:
    """Escape one source line for inclusion in a C double-quoted string.
    Order matters: backslash FIRST, then double-quote. Tabs are converted to
    two spaces so stray tabs can't cause confusion; remove that if you rely on
    literal tabs in the page."""
    line = line.replace('\\', '\\\\')   # \  -> \\
    line = line.replace('"', '\\"')     # "  -> \"
    line = line.replace('\t', '  ')     # tab -> 2 spaces (cosmetic)
    return line


def html_to_cstring(html: str, var_name: str) -> str:
    # Normalise line endings so output is deterministic across platforms.
    html = html.replace('\r\n', '\n').replace('\r', '\n')

    # Split into lines. If the file ends with a trailing newline, split() leaves
    # a final empty element; we drop it so we don't emit a spurious extra "\n"
    # in the served output (keeps the round-trip with cstring_to_html.py exact).
    lines = html.split('\n')
    if lines and lines[-1] == '':
        lines.pop()

    out = [f'static const char {var_name}[] =']
    for ln in lines:
        out.append(f'"{escape_line(ln)}\\n"')
    # Terminate the literal (the last "...\n" line carries the semicolon).
    out[-1] = out[-1] + ';'
    return '\n'.join(out) + '\n'


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert HTML to a C string literal.")
    ap.add_argument("html", help="Path to the .html file")
    ap.add_argument("-n", "--name", default="MAIN_HTML",
                    help="C variable name (default: MAIN_HTML)")
    ap.add_argument("-o", "--out", help="Write to this file instead of stdout")
    args = ap.parse_args()

    try:
        with open(args.html, "r", encoding="utf-8") as f:
            html = f.read()
    except OSError as e:
        print(f"error: cannot read {args.html}: {e}", file=sys.stderr)
        return 1

    literal = html_to_cstring(html, args.name)

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(literal)
        print(f"wrote {args.out} ({len(literal)} bytes, "
              f"{html.count(chr(10)) + 1} source lines)", file=sys.stderr)
    else:
        sys.stdout.write(literal)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
