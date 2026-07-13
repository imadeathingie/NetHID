#!/usr/bin/env python3
"""nethid-rf-learn.py — learn a 433 MHz remote code through a noisy receiver.

A held-down 433 remote transmits the SAME frame over and over, separated by a
long "sync" gap. The firmware's capture buffer (256 edges) therefore fills with
several repeats, starting at an arbitrary point in the stream. So we:

  1. split the capture on the long sync gaps,
  2. throw away the ragged first/last partial frames,
  3. check the remaining whole frames against each other (a real fixed code
     repeats; noise and rolling codes do not),
  4. emit ONE clean frame, median-averaged across the repeats to cancel jitter.

This is what makes it robust: an idle OOK receiver's AGC chatters constantly, so
any single grab can look like a "frame". Consensus across repeats is the only
reliable discriminator.

  BASE=http://192.168.8.205 PASS=yourpw ./nethid-rf-learn.py
  BASE=https://192.168.8.151 PASS=yourpw INSECURE=1 ./nethid-rf-learn.py --rounds 2

Hold the button down for the whole capture window. Output is a ready-to-send
JSON body for POST /api/rf.

Note: RF learn is HTTP-only — the raw control socket speaks HID commands only.
"""
import argparse, http.cookiejar, json, os, ssl, statistics, sys, time, urllib.request

BASE = os.environ.get("BASE", "http://NETHID_IP").rstrip("/")
PASS = os.environ.get("PASS", "changeme")
INSECURE = os.environ.get("INSECURE", "") not in ("", "0")

ctx = ssl.create_default_context()
if INSECURE:
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
opener = urllib.request.build_opener(
    urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
    urllib.request.HTTPSHandler(context=ctx))


def call(path, body=None, method=None):
    data = json.dumps(body).encode() if body is not None else None
    m = method or ("POST" if data is not None else "GET")
    req = urllib.request.Request(BASE + path, data=data if data else (b"" if m == "POST" else None), method=m)
    if data:
        req.add_header("Content-Type", "application/json")
    with opener.open(req, timeout=10) as r:
        return json.loads(r.read().decode() or "{}")


# ── frame extraction ─────────────────────────────────────────────────────────
def find_gap_threshold(t):
    """The sync gap is far longer than any symbol. Use a big multiple of the
    median pulse width, floored at 3 ms."""
    med = statistics.median(t) if t else 0
    return max(3000, med * 4)


def segment(t):
    """Split on sync gaps -> list of frames (gap not included). Drops the first
    and last segments: they are almost always partial."""
    thr = find_gap_threshold(t)
    frames, cur = [], []
    for v in t:
        if v >= thr:
            frames.append(cur)
            cur = []
        else:
            cur.append(v)
    frames.append(cur)
    if len(frames) >= 3:
        frames = frames[1:-1]              # drop ragged ends
    return [f for f in frames if len(f) >= 16], thr


def two_symbols(frame):
    """A valid OOK frame has two dominant, well-separated pulse widths."""
    if len(frame) < 16:
        return False, "too short"
    buckets = {}
    for v in frame:
        buckets.setdefault(round(v / 150) * 150, []).append(v)
    ranked = sorted(buckets.items(), key=lambda kv: len(kv[1]), reverse=True)
    if len(ranked) < 2:
        return False, "one pulse width"
    (c1, m1), (c2, m2) = ranked[0], ranked[1]
    if len(m1) + len(m2) < len(frame) * 0.55:
        return False, "widths don't cluster"
    lo, hi = min(c1, c2), max(c1, c2)
    if lo == 0 or hi / lo < 1.5:
        return False, "short/long not distinct"
    return True, "ok"


def same_frame(a, b, tol=0.35):
    if abs(len(a) - len(b)) > 2:
        return False
    n = min(len(a), len(b))
    ok = sum(1 for i in range(n)
             if abs(a[i] - b[i]) <= max(200, tol * max(a[i], b[i])))
    return ok >= n * 0.85


def consensus(frames):
    """Return (best_frame, count) for the largest group of matching frames."""
    best, best_group = None, []
    for i, a in enumerate(frames):
        group = [b for b in frames if same_frame(a, b)]
        if len(group) > len(best_group):
            best, best_group = a, group
    if not best_group:
        return None, 0
    # median-average the matching frames to cancel receiver jitter
    n = min(len(f) for f in best_group)
    avg = [int(statistics.median(f[i] for f in best_group)) for i in range(n)]
    return avg, len(best_group)


def capture_once(timeout=12):
    call("/api/rf/learn", method="POST")
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = call("/api/rf/captured")
        if r.get("ready"):
            return r.get("timings", [])
        time.sleep(0.3)
    return None


def analyse(raw, verbose=True):
    glitches = sum(1 for v in raw if v < 100)
    frames, thr = segment(raw)
    if verbose:
        print(f"    {len(raw)} edges, {glitches} glitches, sync-gap threshold "
              f"{thr:.0f}us -> {len(frames)} whole frame(s)")
    valid = []
    for f in frames:
        ok, _ = two_symbols(f)
        if ok:
            valid.append(f)
    return valid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=2,
                    help="capture rounds; hold the button through each (default 2)")
    ap.add_argument("--repeat", type=int, default=8,
                    help="repeat count to emit in the send payload (default 8)")
    args = ap.parse_args()

    print(f"[*] {BASE} — logging in…")
    call("/api/auth", {"password": PASS})

    pool = []
    for i in range(1, args.rounds + 1):
        input(f"\n[{i}/{args.rounds}] Press ENTER, then HOLD the remote button "
              f"until this round reports back…")
        raw = capture_once()
        if raw is None:
            print("    …nothing captured (window timed out).")
            continue
        valid = analyse(raw)
        print(f"    {len(valid)} frame(s) with clean short/long symbols")
        pool.extend(valid)

    if not pool:
        sys.exit("\nNo clean frames. Hold the button down for the whole window, bring the\n"
                 "remote close to the receiver, and keep the RF transmitter disconnected.")

    frame, count = consensus(pool)
    if not frame or count < 2:
        print("\n⚠  Frames found, but none repeated identically. That means either a")
        print("   rolling code (cannot be replayed) or noise. Best candidate below.")
        frame = pool[0]
    else:
        print(f"\n✅ Fixed code confirmed — the same frame repeated {count}x "
              f"(median-averaged to cancel jitter).")

    body = {"timings": frame, "repeat": args.repeat}
    print(f"\n{len(frame)} edges. Send it back with:\n")
    print("  curl -sb /tmp/nethid.cookies -X POST -H 'Content-Type: application/json' \\")
    print(f"       -d '{json.dumps(body)}' \\")
    print('       "$BASE/api/rf"')


if __name__ == "__main__":
    main()
