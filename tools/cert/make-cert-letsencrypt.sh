#!/usr/bin/env bash
# make-cert-letsencrypt.sh — Option A: a publicly-trusted cert from Let's Encrypt
# using the Cloudflare DNS-01 challenge. No inbound connection is needed (DNS-01
# proves control by writing a TXT record), and Let's Encrypt does NOT care that
# your A record points at a private 192.168.x.x address — so you get a browser-
# trusted cert for a device that is only reachable on your LAN.
#
# Prerequisites:
#   * Your domain's DNS is hosted at Cloudflare.
#   * acme.sh installed:  https://github.com/acmesh-official/acme.sh
#   * A Cloudflare API token (Zone:DNS:Edit on the zone) exported as CF_Token,
#     plus CF_Account_ID. (See acme.sh's dns_cf docs.)
#
#   export CF_Token="..."  CF_Account_ID="..."
#   ./tools/cert/make-cert-letsencrypt.sh nethid.example.com
#
# NOTE ON RENEWAL: Let's Encrypt certs last 90 days. Running an ACME client on
# the Pico itself is impractical, so the model here is "issue/renew on a host,
# then rebuild + reflash". acme.sh installs a renewal cron; wire its --reloadcmd
# (or a periodic re-run of this script) to regenerate include/server_cert.h, then
# rebuild + flash. If that cadence is annoying for a LAN box, prefer mkcert
# (make-cert-mkcert.sh), which is long-lived.
set -euo pipefail

FQDN="${1:-}"
if [ -z "$FQDN" ]; then echo "usage: $0 <fqdn>   e.g. $0 nethid.example.com" >&2; exit 1; fi
# Locate acme.sh. The official installer puts the binary at ~/.acme.sh/acme.sh
# and only adds a shell *alias* (not a PATH entry), so it isn't visible to a
# script by name — look for the alias first, then the standard install path.
if command -v acme.sh >/dev/null 2>&1; then
  ACME="acme.sh"
elif [ -x "${HOME}/.acme.sh/acme.sh" ]; then
  ACME="${HOME}/.acme.sh/acme.sh"
else
  echo "error: acme.sh not found (looked on PATH and in ~/.acme.sh)." >&2
  echo "Install: https://github.com/acmesh-official/acme.sh" >&2
  exit 1
fi
: "${CF_Token:?set CF_Token (Cloudflare API token, Zone:DNS:Edit)}"

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
CERTS="$HERE/certs"
mkdir -p "$CERTS"

# Use an EC (P-256) key: fast handshakes and small footprint on the Pico.
"$ACME" --issue --dns dns_cf -d "$FQDN" --keylength ec-256

# Install the issued files where we can read them, and regenerate the header on
# each (re)issue/renewal via --reloadcmd.
"$ACME" --install-cert -d "$FQDN" --ecc \
  --key-file       "$CERTS/server.key" \
  --fullchain-file "$CERTS/server.crt" \
  --reloadcmd "python3 '$HERE/tools/cert/gen_cert_header.py' --cert '$CERTS/server.crt' --key '$CERTS/server.key' -o '$HERE/include/server_cert.h'"

echo
echo "Done. Next:"
echo "  1) Point $FQDN at the Pico's LAN IP via LOCAL DNS (router/Pi-hole/hosts)."
echo "     The public A record can stay unset or point anywhere; only your LAN"
echo "     resolver needs to map $FQDN -> the Pico. (Public DNS may strip private IPs.)"
echo "  2) Rebuild:  cmake .. -DPICO_BOARD=pico2_w -DENABLE_HTTPS=ON && make -j"
echo "  3) Flash, then browse to  https://$FQDN/"
echo "  Renewal: acme.sh's cron re-issues every ~60 days and regenerates the"
echo "  header; you still need to rebuild + reflash for the Pico to pick it up."
