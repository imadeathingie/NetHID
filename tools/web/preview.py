#!/usr/bin/env python3
"""
preview.py — open the NetHID web UI in a browser with no board attached.

Writes preview.html: NetHID.html with tools/web/preview/mock.js injected, which
replaces window.fetch with a table of canned API responses. The page then runs
exactly as it would against a real Pico — same code paths, same render
functions, same bugs if there are any.

    python3 tools/web/preview.py            # writes preview.html
    python3 tools/web/preview.py --serve    # ...and serves it on :8080

Then, from the browser console:

    mock.boards()               list the fixtures
    mock.board('oledpad')       switch to a different board
    mock.online(2, false)       take a module off the bus
    mock.set('tapping_term_ms', 250)
    mock.log()                  every request the page has made

The mock is a SEPARATE file injected into a COPY, never part of NetHID.html
itself. Every byte of that file is embedded in the firmware as a C string
literal, and a preview harness has no business taking up flash on a keyboard —
which also means preview.html is generated output and belongs in .gitignore,
not in the tree.
"""

import argparse
import os
import sys
import webbrowser

ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "NetHID.html")
MOCK = os.path.join(ROOT, "tools", "web", "preview", "mock.js")
OUT = os.path.join(ROOT, "preview.html")


def build() -> str:
    html = open(SRC, encoding="utf-8").read()
    mock = open(MOCK, encoding="utf-8").read()

    # Inject BEFORE the page's own scripts. The fetch shim has to be installed
    # before anything calls fetch, and the page's init runs at parse time.
    marker = "<script>"
    if marker not in html:
        sys.exit("no <script> tag in NetHID.html - has the file changed shape?")
    i = html.index(marker)
    out = html[:i] + "<script>\n" + mock + "\n</script>\n" + html[i:]

    open(OUT, "w", encoding="utf-8").write(out)
    return OUT


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serve", action="store_true",
                    help="serve on :8080 and open a browser")
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()

    path = build()
    print("wrote", os.path.relpath(path, ROOT))

    if not a.serve:
        print("open it directly, or re-run with --serve")
        return 0

    # A server rather than file:// because some browsers treat local files as
    # opaque origins and refuse things the page does quite reasonably.
    import http.server, socketserver, functools
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=ROOT)
    with socketserver.TCPServer(("", a.port), handler) as httpd:
        url = "http://localhost:%d/preview.html" % a.port
        print("serving", url, " (ctrl-c to stop)")
        try:
            webbrowser.open(url)
        except Exception:
            pass
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
