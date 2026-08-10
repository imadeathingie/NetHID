#!/usr/bin/env python3
"""
check_usb_descriptor.py — read the USB configuration descriptor out of a linked
firmware and check it against what the code intended.

    python3 tools/check/check_usb_descriptor.py build/nethid.elf [expected_interfaces]

dump_hid_descriptor.py checks the REPORT descriptor, from source. This checks
the CONFIGURATION descriptor, from the linked binary — the interface and
endpoint layout a host actually enumerates.

What it catches is internal inconsistency, all of which a host accepts and then
acts on wrongly, with no error anywhere: wTotalLength disagreeing with the
descriptors present (a host reads exactly that many bytes, so too small silently
truncates the tail and loses an interface), bNumInterfaces disagreeing with the
interfaces that follow, two interfaces sharing an endpoint address, or an
interface with no endpoint.

It does NOT check CFG_TUD_HID — that macro leaves no trace in the descriptor.
A TU_VERIFY_STATIC in usb_descriptors.c covers that one at compile time.
"""
import re
import subprocess
import sys


def rodata(elf):
    for tool in ("arm-none-eabi-objdump", "objdump"):
        try:
            out = subprocess.run([tool, "-s", "-j", ".rodata", elf],
                                 capture_output=True, text=True, check=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
        blob = bytearray()
        for m in re.finditer(r"^ ([0-9a-f]{8}) ((?:[0-9a-f]{2,8} ){1,4})", out, re.M):
            for grp in m.group(2).split():
                blob += bytes.fromhex(grp)
        return blob
    return None


def find_config(blob):
    """The configuration descriptor: bLength 9, bDescriptorType 2, sane fields.

    `09 02` is also "Usage (Mouse)" in a HID REPORT descriptor, which sits in the
    same .rodata — so the field checks are not belt-and-braces, they are what
    stops this decoding the report descriptor as a configuration and reporting
    confident nonsense. The clincher is that a real configuration descriptor is
    followed immediately by an interface descriptor (bLength 9, type 4).
    """
    i = blob.find(bytes([0x09, 0x02]))
    while i >= 0:
        total = blob[i + 2] | (blob[i + 3] << 8)
        nitf = blob[i + 4]
        if (9 <= total <= 512 and 1 <= nitf <= 8 and i + total <= len(blob)
                and blob[i + 9] == 9 and blob[i + 10] == 0x04):
            return blob[i:i + total], total, nitf
        i = blob.find(bytes([0x09, 0x02]), i + 1)
    return None, 0, 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[2])
        return 2
    elf = sys.argv[1]
    want_itf = int(sys.argv[2]) if len(sys.argv) > 2 else None

    blob = rodata(elf)
    if blob is None:
        print("objdump not available - skipping")
        return 0

    d, total, nitf = find_config(blob)
    if d is None:
        print("no configuration descriptor found in %s" % elf)
        return 1

    bad = []
    p, itf, eps, hid, walked = 0, [], [], [], 0
    ep_per_itf = []
    while p + 1 < len(d):
        ln, tp = d[p], d[p + 1]
        if ln == 0:
            bad.append("descriptor of length 0 at offset %d - a host stops here" % p)
            break
        if tp == 0x04:
            itf.append(d[p + 2])
            ep_per_itf.append(d[p + 4])       # bNumEndpoints
        elif tp == 0x21:
            hid.append(d[p + 7] | (d[p + 8] << 8))
        elif tp == 0x05:
            eps.append(d[p + 2])
        p += ln
        walked = p

    print("wTotalLength=%d bNumInterfaces=%d interfaces=%s endpoints=%s "
          "report descriptors=%s"
          % (total, nitf, itf, ["0x%02x" % e for e in eps], hid))

    if len(itf) != nitf:
        bad.append("bNumInterfaces says %d but %d interface descriptor(s) follow"
                   % (nitf, len(itf)))
    if sorted(itf) != list(range(len(itf))):
        bad.append("interface numbers are not 0..%d: %s" % (len(itf) - 1, itf))
    if len(eps) != sum(ep_per_itf):
        bad.append("interfaces declare %d endpoint(s) between them but %d "
                   "endpoint descriptor(s) follow" % (sum(ep_per_itf), len(eps)))
    if any(n == 0 for n in ep_per_itf):
        bad.append("an interface declares no endpoints - it can never send")
    if len(set(eps)) != len(eps):
        bad.append("two interfaces share an endpoint address: %s"
                   % ["0x%02x" % e for e in eps])
    if len(hid) != len(itf):
        bad.append("%d interface(s) but %d HID descriptor(s)" % (len(itf), len(hid)))
    if any(n == 0 for n in hid):
        bad.append("a HID descriptor claims a zero-length report descriptor")
    # Walked, not computed from an assumed shape: the old form hard-coded
    # "9 + interfaces * (9+9+7)", which silently assumes exactly one endpoint
    # and one HID descriptor per interface and would have to be edited for any
    # future interface rather than checking it.
    if walked != total:
        bad.append("wTotalLength %d but walking the descriptors consumed %d - a "
                   "host reads exactly wTotalLength bytes, so the tail is either "
                   "truncated or rubbish" % (total, walked))
    if want_itf is not None and nitf != want_itf:
        bad.append("expected %d interface(s), found %d" % (want_itf, nitf))

    for b in bad:
        print("  " + b)
    if bad:
        print("\nA descriptor a host accepts but that does not match the "
              "firmware's own idea of itself fails silently: reports go out "
              "and nothing happens.")
        return 1
    print("configuration descriptor OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
