/*
 * ap_mode.h — bring up NetHID's own network so it can be configured.
 *
 * Entered deliberately, not automatically: hold AP_MODE_ROW/COL while plugging
 * the board in, exactly like bootmagic. That choice matters. A device that
 * silently starts broadcasting because the router rebooted is a different
 * thing from one you asked to — and what is behind this AP types into the
 * host computer.
 *
 * Set AP_MODE_AUTO_FALLBACK to 1 if you want it entered automatically when no
 * known network is in range. It is off by default for the reason above.
 *
 * ── Security ────────────────────────────────────────────────────────────────
 * The AP is WPA2 and refuses to start without a password of at least 8
 * characters. Session auth stays on regardless of AUTH_REQUIRED — on your own
 * LAN you might reasonably relax it, but here the only thing between someone
 * in radio range and your host's keyboard is the login page.
 *
 * HTTP, not HTTPS, on this interface: the captive-portal DNS answers every
 * lookup with our own address, and no certificate validates for
 * connectivitycheck.gstatic.com. A portal that actually pops means plaintext.
 * The link has exactly one client on it — you — but it is a real trade.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* First, so env.h has already had its say by the time the #ifndef fallbacks
 * below are evaluated. Without this, a translation unit that includes this
 * header before config.h picks up the empty default and then warns when env.h
 * redefines it. */
#include "config.h"

/* AP_MODE_ROW / AP_MODE_COL and the other boot-key positions are configured in
 * include/config.h, which this header includes above. */

#ifndef AP_SSID
#define AP_SSID "NetHID-Setup"
#endif
/* No default password on purpose: a shipped default is the same as no
 * password. Left empty here so ap_mode_start() can refuse at runtime with a
 * message, rather than failing the build for anyone who never uses AP mode. */
#ifndef AP_PASSWORD
#define AP_PASSWORD ""
#endif

#ifndef AP_IP
#define AP_IP "192.168.4.1"
#endif

/* True once ap_mode_start() has succeeded. */
bool ap_mode_active(void);

/* Bring up the AP, DHCP server and captive-portal DNS. Returns false if the
 * password is unusable or the chip refuses. */
bool ap_mode_start(void);

/* The "join this network, open this address" message, as plain ASCII. Typed
 * into the host on a repeating cadence by the setup-mode loop — in setup mode
 * there is no network to reach the device on, so this address is the only way
 * in and one dropped message would strand you. */
void ap_mode_banner(char *buf, size_t cap);

/* Start the AP-side listeners: plain HTTP only, no control socket. */
void ap_start_services(void);

/* Poll from core 0's main loop; handles a queued reboot. */
void ap_mode_poll(void);

/* Reboot into normal (station) mode. Used after provisioning: switching live
 * would drop the client that is mid-configuration, which is a bad moment to
 * lose the connection. */
void ap_mode_request_reboot(uint32_t delay_ms);
