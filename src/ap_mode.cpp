/* ap_mode.cpp — see include/ap_mode.h */

#include "ap_mode.h"
#include "config.h"
/* Deliberately NOT nethid.h: it declares the HID report queue in terms of
 * TinyUSB types and so must always follow tusb.h (see the note at the top of
 * that header). Nothing here touches HID, so the cleanest fix is not to pull
 * it in at all. */
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdio.h>
#if ENABLE_WEB
#include "web.h"
#endif

/* Writes to the serial console and, unless quiet boot is on, types the line
 * into the host. Defined in main.cpp. */
extern void dbg(const char *line);

extern "C" {
#include "vendor/dhcpserver.h"
#include "vendor/dnsserver.h"
}

static dhcp_server_t dhcp;
static dns_server_t  dns;
static bool          active;
static bool          reboot_armed;
static absolute_time_t reboot_at;

bool ap_mode_active(void) { return active; }

bool ap_mode_start(void) {
    const char *pw = AP_PASSWORD;

    /* Refuse rather than fall back to an open network. An AP anyone can join
     * is a keyboard anyone can reach. */
    if (strlen(pw) < 8) {
        printf("[ap] AP_PASSWORD is unset or under 8 characters — refusing to "
               "start an open access point. Set it in env.h.\n");
        return false;
    }

    printf("[ap] starting '%s'\n", AP_SSID);
    cyw43_arch_enable_ap_mode(AP_SSID, pw, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t gw, mask;
    ip4addr_aton(AP_IP, &gw);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    /* The AP interface needs a static address; there is no DHCP server to ask.
     * netif_set_addr under the lwIP lock, as with every other netif change. */
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_AP];
    cyw43_arch_lwip_begin();
    netif_set_addr(nif, &gw, &mask, &gw);
    cyw43_arch_lwip_end();

    /* Signature note: current pico-examples takes the netif explicitly. An
     * older three-argument form is widely quoted online; if you update
     * src/vendor/, check this call still matches the header you pulled. */
    dhcp_server_init(&dhcp, nif, &gw, &mask);

    /* Answers every lookup with our own address, which is what makes phones
     * pop the "Sign in to network" sheet instead of you typing an IP. */
    dns_server_init(&dns, nif, &gw);

    active = true;
    printf("[ap] up. SSID '%s', http://%s/\n", AP_SSID, AP_IP);

    return true;
}

void ap_mode_request_reboot(uint32_t delay_ms) {
    reboot_at = make_timeout_time_ms(delay_ms);
    reboot_armed = true;
    printf("[ap] reboot in %u ms\n", (unsigned)delay_ms);
}

void ap_mode_poll(void) {
    if (!reboot_armed || !time_reached(reboot_at)) return;
    reboot_armed = false;
    printf("[ap] rebooting into station mode\n");
    /* The delay lets the HTTP response actually reach the browser first —
     * rebooting inside the request handler leaves the client staring at a dead
     * socket with no idea whether it worked. */
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

/*
 * Start the listeners for AP mode.
 *
 * Plain HTTP on port 80, not the usual HTTPS. Two reasons, and the second is
 * the real one: the captive-portal DNS answers every lookup with our address,
 * and no certificate validates for connectivitycheck.gstatic.com — so a portal
 * that actually pops has to be plaintext. Beyond that, a self-signed cert on a
 * network you joined thirty seconds ago produces a browser warning that trains
 * exactly the wrong instinct.
 *
 * The control socket is deliberately NOT started here. AP mode exists to get
 * credentials in; nothing else needs to be reachable while it does that, and
 * every extra listener is another thing facing a network anyone in radio range
 * can attempt to join.
 */
/*
 * The line typed into the host while setup mode is running.
 *
 * Deliberately plain ASCII. The HID typer maps characters to keycodes and has
 * no entry for anything outside ASCII, so a stray em-dash or curly quote is
 * silently dropped from what the user sees — which is exactly the wrong place
 * to lose characters, since this address is the only way in.
 */
void ap_mode_banner(char *buf, size_t cap) {
    snprintf(buf, cap,
             "[NetHID] WiFi setup mode is running.\n"
             "1. Join the wifi network: %s\n"
             "2. Open this address:     http://%s/\n"
             "3. Add your network on the WIFI tab, then Save and reboot.\n",
             AP_SSID, AP_IP);
}

void ap_start_services(void) {
#if ENABLE_WEB
    web_init_plain(80);
#endif
}
