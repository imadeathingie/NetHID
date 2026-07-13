/*
 * main.cpp — NetHID entry point for Raspberry Pi Pico 2 W
 *
 * Edit config.h for WiFi credentials, password, and tuning.
 * Debug output: UART0 at 115200 baud (GP0=TX, GP1=RX).
 */

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "tusb.h"

#include "nethid.h"
#include "auth.h"
#include "config.h"
#if ENABLE_TCP
#include "server.h"
#endif
#if ENABLE_WEB
#include "web.h"
#endif
#if ENABLE_REMOTES
#include "remotes.h"
#endif

#include <stdio.h>
#include <string.h>

// Forward declarations (defined later in the file)
bool usb_is_mounted(void);

// ── LED helpers ───────────────────────────────────────────────────────────────

static inline void led_set(bool on) {
#if USE_LED
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#endif
}

static void __attribute__((unused)) led_blink(int times, int on_ms, int off_ms) {
    for (int i = 0; i < times; i++) {
        led_set(true);  sleep_ms(on_ms);
        led_set(false); sleep_ms(off_ms);
    }
}

// Blink a status code: N short blinks, then a long pause, repeated `repeat`
// times. Lets you read how far boot progressed without a serial console.
//
//   1 = cyw43 (WiFi chip + LED) initialised OK
//   2 = USB stack started but host NEVER enumerated (descriptor/cable issue)
//   3 = USB ENUMERATED by host  ← seen briefly on every healthy boot
//   4 = USB OK but WiFi FAILED to connect (repeats forever)
//   heartbeat (single 30ms flash every 3s) = running normally, all up
//
// Healthy boot looks like: code 1, then code 3, then (if WiFi connects) a
// steady 3-second heartbeat. If it lands on repeating 4, USB is fine but
// WiFi didn't connect — check the typed message on the host.
static void led_status(int code, int repeat) {
    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i < code; i++) {
            led_set(true);  sleep_ms(150);
            led_set(false); sleep_ms(200);
        }
        sleep_ms(900);   // long gap between repeats
    }
}

// ── Typed debug log ───────────────────────────────────────────────────────────
// Mirrors printf debug output to the USB keyboard, so boot progress is visible
// on a focused text editor when no UART console is available. Runs on core 0:
// it pushes the line to the HID queue and waits while core 1 types it out.
// Never calls hid_task()/tud_task() (those belong to core 1 only).
//
// When QUIET_BOOT is set, dbg() still printf's (harmless, for UART) but does
// NOT type onto the host — useful once your setup is stable and the boot
// chatter is just noise.
void dbg(const char *line) {
    printf("%s\n", line);
#if QUIET_BOOT
    return;
#else
    if (!usb_is_mounted()) return;

    size_t len = strlen(line);
    char buf[120];
    if (len > 110) len = 110;
    memcpy(buf, line, len);
    buf[len]   = '\n';
    buf[len+1] = '\0';

    hid_push_type_string(buf, (uint8_t)(len + 1), TYPE_DELAY_MS);

    // Wait (bounded) while core 1 drains the queue and types the line.
    sleep_ms((int)(len + 1) * 2 * TYPE_DELAY_MS + 400);
#endif
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
// Values from cyw43.h: DOWN=0, JOIN=1, NOIP=2, UP=3, FAIL=-1, NONET=-2, BADAUTH=-3
static const char *wifi_err_str(int code) {
    switch (code) {
        case 3:    return "up (connected with IP)";
        case 2:    return "joined but no IP yet (DHCP pending)";
        case 1:    return "joining";
        case 0:    return "link down";
        case -1:   return "connection failed";
        case -2:   return "no matching network - check SSID, 2.4GHz, range";
        case -3:   return "bad auth - check password";
        case -100: return "joined AP but DHCP gave no IP address";
        default:   return "unknown";
    }
}

// Map an auth-mode config value to the SDK auth constant.
// WPA3 constants only exist on newer SDKs, so guard them.
static uint32_t wifi_auth_value(int mode) {
    switch (mode) {
        case 0: return CYW43_AUTH_WPA2_AES_PSK;
        case 1: return CYW43_AUTH_WPA2_MIXED_PSK;
#ifdef CYW43_AUTH_WPA3_WPA2_AES_PSK
        case 2: return CYW43_AUTH_WPA3_WPA2_AES_PSK;
#endif
#ifdef CYW43_AUTH_WPA3_SAE_AES_PSK
        case 3: return CYW43_AUTH_WPA3_SAE_AES_PSK;
#endif
        case 4: return CYW43_AUTH_OPEN;
        default: return CYW43_AUTH_WPA2_MIXED_PSK;
    }
}

// ── Network scan + selection ──────────────────────────────────────────────────
// Scan for available APs and choose the best configured network (the one from
// WIFI_NETWORKS that is present with the strongest signal). Returns the index
// into WIFI_NETWORKS, or -1 if none of the configured networks are in range.

static struct {
    int     best_idx;     // index into WIFI_NETWORKS, -1 = none
    int16_t best_rssi;
    int     total_seen;
} g_scan;

static int scan_match_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r) return 0;
    g_scan.total_seen++;
    // Compare this AP's SSID against each configured network.
    for (size_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
        const char *cfg = WIFI_NETWORKS[i].ssid;
        size_t clen = strlen(cfg);
        if (clen == r->ssid_len &&
            memcmp(cfg, r->ssid, clen) == 0) {
            // Match. Keep it if it's the strongest match so far.
            if (g_scan.best_idx < 0 || r->rssi > g_scan.best_rssi) {
                g_scan.best_idx  = (int)i;
                g_scan.best_rssi = r->rssi;
            }
        }
    }
    return 0;
}

// Returns index of the chosen network in WIFI_NETWORKS, or -1 if none found.
static int wifi_scan_select(void) {
    g_scan.best_idx   = -1;
    g_scan.best_rssi  = -32768;
    g_scan.total_seen = 0;

    dbg("[" NETHID_BUILD "] Scanning for known networks...");
    cyw43_wifi_scan_options_t opts = {};
    int err = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_match_cb);
    if (err != 0) {
        char d[64]; snprintf(d,sizeof(d),"[" NETHID_BUILD "] scan start failed: %d", err); dbg(d);
        return -1;
    }

    // Wait for the scan to finish, with a hard 10s cap so it can never hang boot.
    absolute_time_t deadline = make_timeout_time_ms(10000);
    bool led_on = false;
    absolute_time_t next = make_timeout_time_ms(150);
    while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(deadline)) {
        if (time_reached(next)) { led_on=!led_on; led_set(led_on); next=make_timeout_time_ms(150); }
        sleep_ms(10);
    }

    if (g_scan.best_idx >= 0) {
        char d[96];
        snprintf(d, sizeof(d), "[" NETHID_BUILD "] Chose '%s' (rssi %d, %d APs seen)",
                 WIFI_NETWORKS[g_scan.best_idx].ssid,
                 g_scan.best_rssi, g_scan.total_seen);
        dbg(d);
    } else {
        char d[64];
        snprintf(d, sizeof(d), "[" NETHID_BUILD "] No known network in range (%d APs seen)",
                 g_scan.total_seen);
        dbg(d);
    }
    return g_scan.best_idx;
}

// Connect to the network at WIFI_NETWORKS[idx].
// Returns 0 on success, or the last non-zero connect status on failure.
// Uses the ASYNC connect API + polling so it can never silently block.
static int wifi_connect(int idx) {
    if (idx < 0 || (size_t)idx >= WIFI_NETWORK_COUNT) return -1;
    const wifi_network_t *net = &WIFI_NETWORKS[idx];
    printf("[wifi] Connecting to '%s'...\n", net->ssid);

    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];

#if STATIC_IP_SET
    ip4_addr_t ip, mask, gw;
    ip4addr_aton(STATIC_IP,   &ip);
    ip4addr_aton(STATIC_MASK, &mask);
    ip4addr_aton(STATIC_GW,   &gw);
    cyw43_arch_lwip_begin();
    dhcp_stop(nif);
    netif_set_addr(nif, &ip, &mask, &gw);
    cyw43_arch_lwip_end();
    printf("[wifi] Static IP: %s\n", STATIC_IP);
#endif

    for (int attempt = 0; attempt < WIFI_RETRIES; attempt++) {
        printf("[wifi] Attempt %d/%d...\n", attempt+1, WIFI_RETRIES);
        { char d[64]; snprintf(d,sizeof(d),"[" NETHID_BUILD "] connect attempt %d...",attempt+1); dbg(d); }

        // For an OPEN network the SDK wants a NULL password.
        const char *pw = (net->auth_mode == 4 || net->password[0] == '\0')
                         ? NULL : net->password;
        int rc = cyw43_arch_wifi_connect_async(net->ssid, pw,
                                               wifi_auth_value(net->auth_mode));
        { char d[64]; snprintf(d,sizeof(d),"[" NETHID_BUILD "] async kickoff rc=%d",rc); dbg(d); }
        if (rc != 0) {
            sleep_ms(1000);
            continue;
        }

        // Poll link status up to 30s.
        absolute_time_t deadline = make_timeout_time_ms(30000);
        bool led_on = false;
        absolute_time_t next = make_timeout_time_ms(200);
        absolute_time_t next_log = make_timeout_time_ms(2000);
        int last_st = 99;
        bool probed = false;

        while (!time_reached(deadline)) {
            // One-time probe around the very first status call, to see if THIS
            // call is what blocks.
            if (!probed) { dbg("[" NETHID_BUILD "] before link_status call"); }

            int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

            if (!probed) { dbg("[" NETHID_BUILD "] after link_status call"); probed = true; }

            // Log status changes immediately, and a heartbeat every 2s.
            if (st != last_st || time_reached(next_log)) {
                char d[64];
                snprintf(d, sizeof(d), "[" NETHID_BUILD "] link status = %d", st);
                dbg(d);
                last_st  = st;
                next_log = make_timeout_time_ms(2000);
            }

            if (st == CYW43_LINK_UP) {
                dbg("[" NETHID_BUILD "] Joined, waiting for DHCP IP...");
                absolute_time_t dhcp_deadline = make_timeout_time_ms(15000);
                while (!time_reached(dhcp_deadline)) {
                    const ip4_addr_t *a = netif_ip4_addr(nif);
                    if (a && !ip4_addr_isany(a) && ip4_addr_get_u32(a) != 0) {
                        led_set(true);
                        printf("[wifi] DHCP OK. IP: %s\n", ip4addr_ntoa(a));
                        return 0;
                    }
                    sleep_ms(50);
                }
                dbg("[" NETHID_BUILD "] Joined but DHCP gave no IP");
                return -100;
            }
            if (st < 0) {
                { char d[64]; snprintf(d,sizeof(d),"[" NETHID_BUILD "] link status %d (fail)",st); dbg(d); }
                break;   // hard failure — retry the attempt
            }

            if (time_reached(next)) {
                led_on = !led_on; led_set(led_on);
                next = make_timeout_time_ms(200);
            }
            sleep_ms(50);   // let the cyw43 background context run
        }

        led_set(false);
        { char d[80]; snprintf(d,sizeof(d),"[" NETHID_BUILD "] attempt %d no connect",attempt+1); dbg(d); }
        sleep_ms(1500);
    }

    return -1;
}

// ── Type a string onto the host ───────────────────────────────────────────────

// Type a string onto the host. Runs on core 0: pushes to the HID queue and
// waits while core 1 (USB) types it. Never calls tud_task()/hid_task().
static void type_to_host(const char *msg, int settle_ms) {
    if (!usb_is_mounted()) return;
    // Give the user a moment to focus a text field
    sleep_ms(settle_ms);

    // The HID queue holds one type-string command of up to 255 chars; longer
    // messages are split into chunks so none is dropped.
    size_t total = strlen(msg);
    size_t off = 0;
    while (off < total) {
        size_t chunk = total - off;
        if (chunk > 200) chunk = 200;
        hid_push_type_string(msg + off, (uint8_t)chunk, TYPE_DELAY_MS);
        // wait for core 1 to type this chunk before queuing the next
        sleep_ms((int)chunk * 2 * TYPE_DELAY_MS + 300);
        off += chunk;
    }
}

// ── USB HID on Core 1 ─────────────────────────────────────────────────────────
// USB must run on its own core, separate from cyw43/lwIP, because servicing
// TinyUSB in the same loop as the cyw43 background context deadlocks on RP2350.
// Core 1 does nothing but init TinyUSB and pump tud_task() + the HID queue.
// Core 0 (cyw43 + lwIP + servers) communicates only via the spinlock-safe
// HID queue, never touching USB directly.

static volatile bool _usb_mounted = false;

bool usb_is_mounted(void) { return _usb_mounted; }

static void core1_usb_main(void) {
    hid_init();
    tusb_init();
    printf("[usb] (core1) tusb_init done\n");

    for (;;) {
        hid_task();          // services tud_task() + drains the HID queue
        _usb_mounted = tud_mounted();
        // no sleep — tight USB servicing loop on this dedicated core
    }
}

// Launch core 1 and wait (on core 0) for USB to enumerate.
static bool usb_start_core1(void) {
    multicore_launch_core1(core1_usb_main);
    printf("[usb] core1 launched, waiting for enumeration...\n");
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 8000;
    while (!_usb_mounted) {
        if (to_ms_since_boot(get_absolute_time()) > deadline) {
            printf("[usb] NOT enumerated within 8s\n");
            return false;
        }
        sleep_ms(10);
    }
    printf("[usb] HID device enumerated and ready\n");
    return true;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    stdio_init_all();
    sleep_ms(200);

    printf("\n");
    printf("================================================\n");
    printf("  NetHID — Network Attached Keyboard & Mouse\n");
    printf("  Pico 2 W / RP2350 / C++ / Pico SDK\n");
    printf("================================================\n");

    // ── 1. Auth init ──────────────────────────────────────────────────────────
    auth_init();
    if (auth_is_enabled()) {
        printf("  Auth    : enabled  (timeout=%ds, lockout=%ds after %d attempts)\n",
               SESSION_TIMEOUT_S, LOCKOUT_S, MAX_AUTH_ATTEMPTS);
    } else {
        printf("  Auth    : DISABLED — set PASSWORD in config.h\n");
    }

    // ── 2. WiFi chip (brings up the onboard LED) ──────────────────────────────
    // The onboard LED is wired through the CYW43 chip, so we init it first to
    // make the LED available for status codes. This does NOT connect to WiFi
    // yet — just powers up the chip. The country code affects which channels
    // are allowed and can be the difference between connecting and failing.
    uint32_t country = (WIFI_COUNTRY[0] == 'X')
                       ? PICO_CYW43_ARCH_DEFAULT_COUNTRY_CODE
                       : CYW43_COUNTRY(WIFI_COUNTRY[0], WIFI_COUNTRY[1], 0);
    if (cyw43_arch_init_with_country(country) != 0) {
        // No LED available (it needs cyw43). Nothing we can do but halt.
        printf("[diag] cyw43_arch_init FAILED — LED unavailable\n");
        for (;;) tight_loop_contents();
    }
    // CODE 1: cyw43 OK. If you only ever see ONE blink repeating, the firmware
    // is running but stalled here (shouldn't happen).
    led_status(1, 2);

    // ── 3. USB HID on Core 1 ──────────────────────────────────────────────────
    bool usb_ok = usb_start_core1();
    if (usb_ok) {
        // CODE 3: USB enumerated by host. This is the key success signal.
        led_status(3, 2);
    } else {
        // CODE 2 repeating forever: USB stack started but host never
        // enumerated the device. Points to a USB descriptor/stack issue
        // (or a host/cable problem), NOT a crash and NOT a WiFi problem.
        printf("[diag] USB did not enumerate — signalling code 2\n");
        cyw43_arch_enable_sta_mode();  // still try WiFi so web UI may work
        { int idx = wifi_scan_select(); if (idx >= 0) wifi_connect(idx); }
        for (;;) {
            led_status(2, 1);
        }
    }

    cyw43_arch_enable_sta_mode();

    dbg("[" NETHID_BUILD "] USB ok (core1). Scanning for known networks...");

    // ── 4. WiFi ───────────────────────────────────────────────────────────────
    // Scan for available networks and pick the best one we have credentials
    // for, so the device can move between networks without reflashing.
    int net_idx = wifi_scan_select();
    const char *chosen_ssid = (net_idx >= 0) ? WIFI_NETWORKS[net_idx].ssid : "(none)";

    int wifi_err;
    if (net_idx < 0) {
        // No known network in range. Fall back to trying the first configured
        // entry blind (in case the scan missed a hidden SSID).
        dbg("[" NETHID_BUILD "] No known SSID scanned; trying first configured network blind");
        net_idx = 0;
        chosen_ssid = WIFI_NETWORKS[0].ssid;
        wifi_err = wifi_connect(0);
    } else {
        wifi_err = wifi_connect(net_idx);
    }

    {
        char d[96];
        snprintf(d, sizeof(d), "[" NETHID_BUILD "] wifi_connect('%s') returned %d",
                 chosen_ssid, wifi_err);
        dbg(d);
    }

    if (wifi_err != 0) {
        printf("[wifi] No network — USB HID only\n");

        // Type the failure reason onto the host so it's visible without a
        // serial console. Repeat every ~8s so you can't miss it.
        char msg[320];
        if (wifi_err == -100) {
            snprintf(msg, sizeof(msg),
                     "[NetHID build " NETHID_BUILD "] Joined WiFi but NO IP (DHCP failed).\n"
                     "SSID: %s\n"
                     "The AP accepted us but never gave an address. "
                     "Check the router DHCP server is enabled, or set a static "
                     "IP (STATIC_IP_SET 1 in config.h) on the router's subnet.\n",
                     chosen_ssid);
        } else {
            snprintf(msg, sizeof(msg),
                     "[NetHID build " NETHID_BUILD "] WiFi connection FAILED.\n"
                     "Tried SSID: %s\n"
                     "Status %d: %s\n"
                     "No known network connected. Check credentials in "
                     "config.h/env.h, or that a configured 2.4GHz network "
                     "is in range.\n",
                     chosen_ssid, wifi_err, wifi_err_str(wifi_err));
        }

        for (;;) {
            led_status(4, 1);             // 4 blinks = USB ok, WiFi failed
            type_to_host(msg, 2000);      // re-type every loop (~8s apart)
        }
    }

    // ── 5. Print URLs ─────────────────────────────────────────────────────────
    const char *ip = ip4addr_ntoa(
        netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
    printf("\n");
#if ENABLE_WEB
    printf("  Web UI : http://%s/\n", ip);
#endif
#if ENABLE_TCP
    printf("  Socket : %s:%d  (TCP — binary or JSON, auth %s)\n",
           ip, TCP_PORT, auth_is_enabled() ? "required" : "disabled");
#endif
#if !ENABLE_WEB && !ENABLE_TCP
    printf("  (no network control interface compiled in — USB HID only)\n");
#endif
    printf("  Remote Wakeup: host must enable in BIOS and OS power settings\n");
    printf("\n");

    {
        char d[80];
        snprintf(d, sizeof(d), "[" NETHID_BUILD "] Connected. IP=%s", ip);
        dbg(d);
    }

    // ── 5b. Type a usage synopsis onto the host ───────────────────────────────
#if TYPE_IP_ON_BOOT && !QUIET_BOOT
    if (usb_is_mounted()) {
        char synopsis[256];
        char ifaces[160];
        ifaces[0] = '\0';
#if ENABLE_WEB
        { char l[80]; snprintf(l, sizeof(l), "Web UI: http://%s/\n", ip);
          strncat(ifaces, l, sizeof(ifaces)-strlen(ifaces)-1); }
#endif
#if ENABLE_TCP
        { char l[80]; snprintf(l, sizeof(l), "Socket: %s port %d\n", ip, TCP_PORT);
          strncat(ifaces, l, sizeof(ifaces)-strlen(ifaces)-1); }
#endif
#if !ENABLE_WEB && !ENABLE_TCP
        strncat(ifaces, "USB HID only (no network control)\n",
                sizeof(ifaces)-strlen(ifaces)-1);
#endif
        snprintf(synopsis, sizeof(synopsis),
                 "[NetHID build " NETHID_BUILD "] ready. Connected to '%s'.\n"
                 "%s"
                 "Auth: %s\n",
                 chosen_ssid, ifaces,
                 auth_is_enabled() ? "password required" : "disabled");
        printf("[usb] Typing synopsis to host in %d s...\n", TYPE_IP_DELAY_S);
        type_to_host(synopsis, TYPE_IP_DELAY_S * 1000);
    }
#endif

    dbg("[" NETHID_BUILD "] Starting servers...");

    // ── 6. Servers ────────────────────────────────────────────────────────────
    cyw43_arch_lwip_begin();
#if ENABLE_TCP
    server_init();
#endif
#if ENABLE_WEB
    web_init();
#endif
    cyw43_arch_lwip_end();

    // ── 6b. IR blaster + 433 MHz transmitter (PIO) ────────────────────────────
    // PIO generates the waveform timing in hardware, so WiFi/USB CPU activity
    // cannot jitter it. Init after the radio so PIO/clock state is settled.
#if ENABLE_REMOTES
    if (remotes_init()) {
        dbg("[" NETHID_BUILD "] Remotes (IR/RF) ready.");
    } else {
        dbg("[" NETHID_BUILD "] Remotes (IR/RF) init FAILED (PIO unavailable).");
    }
#endif

    dbg("[" NETHID_BUILD "] Servers up. Entering main loop. READY.");

    // ── 7. Main loop (core 0) ─────────────────────────────────────────────────
    // USB runs on core 1; core 0 just keeps the cyw43/lwIP context alive and
    // blinks a heartbeat. No hid_task() here — that's core 1's job.
    led_set(false);
    absolute_time_t next_beat = make_timeout_time_ms(2000);
    for (;;) {
        if (time_reached(next_beat)) {
            led_set(true);  sleep_ms(60); led_set(false);
            next_beat = make_timeout_time_ms(2000);
        }
#if ENABLE_REMOTES
        remotes_rx_poll();   // finalize any armed IR/RF capture (gap-framed)
#endif
        sleep_ms(5);
    }
}
