#!/usr/bin/env bash
# make-cert-mkcert.sh — Option C: a long-lived cert from a local CA (mkcert).
#
# Best fit for a LAN-only device you reach from machines you control. mkcert
# creates a local root CA, installs it into your OS/browser trust stores, and
# signs a leaf cert for your NetHID's domain that is valid for ~2 years — so no
# 90-day renewal treadmill. Browsers that trust the CA show a normal padlock.
#
#   ./tools/cert/make-cert-mkcert.sh nethid.example.com
#
# Then rebuild with -DENABLE_HTTPS=ON and flash. To trust it on OTHER machines,
# copy "$(mkcert -CAROOT)/rootCA.pem" to them and import it (mkcert -install uses
# it locally; phones/other PCs need the root imported manually).
set -euo pipefail

FQDN="${1:-}"
if [ -z "$FQDN" ]; then echo "usage: $0 <fqdn>   e.g. $0 nethid.example.com" >&2; exit 1; fi
if ! command -v mkcert >/dev/null 2>&1; then
  echo "error: mkcert not found. Install it: https://github.com/FiloSottile/mkcert" >&2
  echo "  macOS:  brew install mkcert nss" >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
CERTS="$HERE/certs"
mkdir -p "$CERTS"

# Install the local CA into trust stores (idempotent).
mkcert -install

# Issue the leaf cert for the FQDN (add more SANs as extra args if you like).
mkcert -cert-file "$CERTS/server.crt" -key-file "$CERTS/server.key" "$FQDN"

echo "CA root is at: $(mkcert -CAROOT)/rootCA.pem  (import this on other devices)"

# Embed into the firmware header.
python3 "$HERE/tools/cert/gen_cert_header.py" \
  --cert "$CERTS/server.crt" --key "$CERTS/server.key" \
  -o "$HERE/include/server_cert.h"

echo
echo "Done. Next:"
echo "  1) Point $FQDN at the Pico's LAN IP (router DNS / Pi-hole / hosts file)."
echo "  2) Rebuild:  cmake .. -DPICO_BOARD=pico2_w -DENABLE_HTTPS=ON && make -j"
echo "  3) Flash, then browse to  https://$FQDN/"
