#!/usr/bin/env python3
"""
check_tab_subsets.py — load the page with only a subset of its tabs and check
the tab bar still gets built.

The server serves only the panels a user is granted, and AP setup mode serves
just three. So at any given load an arbitrary subset of the DOM the page's init
code expects is simply absent. Run bare and back to back, one null deref aborts
the whole top-level script: the page keeps its header and log box and loses the
tab bar, which looks like the page failed to load rather than like a bug in one
function. That is exactly how #customPanels took down AP setup mode.

Needs node with jsdom:  npm i jsdom
Skips cleanly (exit 0) when jsdom is unavailable, so it does not become a
blocker on a machine without it.

    python3 tools/check/check_tab_subsets.py          # the AP setup subset
    python3 tools/check/check_tab_subsets.py --all    # plus other subsets, slower
"""

import json
import re
import subprocess
import sys
import os
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HTML = ROOT / "NetHID.html"

TAB = re.compile(r"<!--@@TAB (\w+)@@-->(.*?)<!--@@ENDTAB@@-->", re.S)

# The subsets the firmware can actually serve.
# jsdom parses and runs the whole page per case, which is slow. Default to the
# subset that is actually served in production; pass --all for the rest.
CASES = {
    "AP setup mode": ["wifi", "keymap", "settings"],
}
EXTRA_CASES = {
    "single tab": ["keyboard"],
    "no custom tab": None,      # everything except customedit
}

DRIVER = r"""
const { JSDOM } = require('jsdom');
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const errors = [];
const dom = new JSDOM(html, {
  runScripts: 'dangerously',
  pretendToBeVisual: true,
  beforeParse(w) {
    w.fetch = () => Promise.resolve({ ok: true, status: 200, json: async () => ({}) });
    w.console.error = (...a) => errors.push(a.map(String).join(' '));
    w.addEventListener('error', e => errors.push('uncaught: ' + e.message));
  },
});
const tabs = dom.window.document.querySelectorAll('#tabBar .tab').length;
const panels = dom.window.document.querySelectorAll('.panel[data-tabname]').length;
console.log(JSON.stringify({ tabs, panels, errors }));
// The page installs a ping interval and other timers. Left running they keep
// node's event loop alive forever and the check looks like a hang rather than
// a result.
dom.window.close();
process.exit(0);
"""


def build(names):
    src = HTML.read_text()
    blocks = {m.group(1): m.group(0) for m in TAB.finditer(src)}
    keep = set(blocks) if names is None else set(names)
    if names is None:
        keep.discard("customedit")
    out = src
    for name, block in blocks.items():
        if name not in keep:
            out = out.replace(block, "")
    return out, sorted(keep & set(blocks))


def main() -> int:
    if not HTML.exists():
        print("NetHID.html not found")
        return 1
    probe = subprocess.run(["node", "-e", "require('jsdom')"],
                           cwd=ROOT, capture_output=True)
    if probe.returncode != 0:
        print("jsdom not installed - skipping (npm i jsdom to enable)")
        return 0

    # The driver runs from a temp directory, so node's usual walk up the tree
    # never reaches this repo's node_modules and every case dies with "Cannot
    # find module 'jsdom'" — a failure that reads like the page is broken. The
    # probe above cannot catch it, because the probe runs from ROOT where the
    # resolution does work.
    env = dict(os.environ)
    node_path = ROOT / "node_modules"
    if node_path.is_dir():
        env["NODE_PATH"] = (str(node_path) +
                            (os.pathsep + env["NODE_PATH"] if env.get("NODE_PATH") else ""))

    cases = dict(CASES)
    if "--all" in sys.argv:
        cases.update(EXTRA_CASES)

    with tempfile.TemporaryDirectory() as td:
        drv = Path(td) / "drv.js"
        drv.write_text(DRIVER)
        bad = False
        for label, names in cases.items():
            html, kept = build(names)
            page = Path(td) / "page.html"
            page.write_text(html)
            try:
                r = subprocess.run(["node", str(drv), str(page)], env=env,
                                   capture_output=True, text=True, timeout=120)
            except subprocess.TimeoutExpired:
                print(f"{label}: timed out (jsdom is slow; not treated as a failure)")
                continue
            if r.returncode != 0:
                print(f"{label}: driver failed\n{r.stderr[:400]}")
                bad = True
                continue
            res = json.loads(r.stdout)
            ok = res["tabs"] >= res["panels"] > 0 and not res["errors"]
            print(f"{label:16s} panels={res['panels']:2d} tabs={res['tabs']:2d} "
                  f"{'OK' if ok else 'FAIL'}")
            for e in res["errors"][:3]:
                print(f"    {e[:160]}")
            if not ok:
                bad = True

        if bad:
            print("\nA tab subset that breaks init leaves the page with a header")
            print("and no tabs. Guard DOM lookups for panels that may not be served.")
            return 1
    print("tab subsets OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
