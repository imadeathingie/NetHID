# `src/vendor/`

Third-party sources, unmodified, vendored so the tree builds without extra
checkouts. lwIP ships no DHCP *server* and no DNS server, and AP mode needs
both: without DHCP a client joins and never gets an address, and without DNS
the captive-portal sheet never appears.

| file | origin | licence |
| --- | --- | --- |
| `dhcpserver.c/.h` | MicroPython, via pico-examples | MIT — © 2018–2019 Damien P. George |
| `dnsserver.c/.h` | pico-examples | BSD-3-Clause — © 2022 Raspberry Pi (Trading) Ltd |

Both are GPL-3 compatible, so they can live in this tree. Licence headers are
intact in each file; leave them there.

Upstream: https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/access_point

Only compiled when `ENABLE_AP_MODE` is on.

## Warnings from these files

They build with unused-parameter warnings under this project's flags. Leave
them. Patching upstream code to silence cosmetic warnings makes the next
refresh a merge instead of a copy, and these are not our warnings to fix.

## If you refresh them

Check that `dhcp_server_init()` and `dns_server_init()` still take a
`struct netif *` as their second argument. An older three-argument form is
widely quoted and produces a confusing runtime failure rather than a compile
error.
