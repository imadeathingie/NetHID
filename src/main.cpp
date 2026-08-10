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
#include "auth_store.h"
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
#if ENABLE_AP_MODE
#include "ap_mode.h"
#include "wifi_store.h"
#endif
#if ENABLE_SETTINGS
#include "settings.h"
#endif
#if OLED_ENABLE
#include "oled/oled.h"
#include "oled/kb_status.h"
#endif
#if ENABLE_KEYBOARD
#include "kb/kb.h"
#include "kb/bootmagic.h"
#if KB_FEATURE_DYNAMIC_KEYMAP
#include "kb/keymap_store.h"
#endif
#if KB_FEATURE_MACRO_STORE
#include "kb/macro_store.h"
#endif
#if KB_FEATURE_AUTOCLICK
#include "kb/autoclick.h"
#endif
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
/*
 * Is this build/boot allowed to type diagnostics into the host?
 *
 * THE single gate for quiet boot. Both typed-output paths — dbg() for one-line
 * diagnostics and type_to_host() for the multi-line banners — go through here,
 * because a gate applied in only one of them is a setting that half works:
 * quiet boot silenced the per-step lines and left the "ready" banner and the
 * WiFi-failure message typing into whatever you had focused.
 *
 * This deliberately does NOT gate /api/text or the macro typer. Quiet boot is
 * about unrequested boot noise, not about refusing work someone asked for.
 */
static bool boot_output_allowed(void) {
#if ENABLE_SETTINGS
    // Effective, not stored: this resolves the loud/quiet boot keys on top of
    // whatever is saved.
    return !settings_quiet_boot_effective();
#elif QUIET_BOOT
    return false;
#else
    return true;
#endif
}

void dbg(const char *line) {
    // The serial line is always written. Quiet boot is about not typing into
    // whatever the host has focused, which is the part that is actually
    // disruptive; a console you had to attach on purpose is not.
    printf("%s\n", line);
    if (!boot_output_allowed()) return;
    if (!usb_is_mounted()) return;

    // Transliterate to plain ASCII before typing.
    //
    // ascii_to_hid() has no entry for anything above 0x7E and SKIPS what it
    // cannot map, so a stray em-dash or curly quote simply vanishes from what
    // the user sees — a diagnostic that reads "AP key held " and stops, with no
    // hint that three bytes were dropped mid-sentence. Since these strings are
    // written by hand in source, that will keep happening; substituting a
    // visible character is more honest than silently losing them.
    size_t len = strlen(line);
    char buf[120];
    if (len > 110) len = 110;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];
        buf[i] = (c == '\n' || c == '\t' || (c >= 0x20 && c <= 0x7E))
                 ? (char)c : '-';
    }
    buf[len]   = '\n';
    buf[len+1] = '\0';

    uint8_t delay = TYPE_DELAY_MS;
#if ENABLE_SETTINGS
    delay = settings()->type_delay_ms;
#endif
    if (!hid_push_type_string(buf, (uint8_t)(len + 1), delay)) return;   // queue full

    // Wait for the typer to actually finish, not for an estimate of how long it
    // ought to take.
    //
    // The old fixed sleep was computed from length x delay, which is only right
    // when every report is accepted first time. Now that typer_step() retries a
    // rejected report instead of dropping it, a congested endpoint makes a line
    // take longer than the estimate — and returning early meant the next line
    // was queued while this one was still going, so boot output interleaved and
    // looked like characters had been lost.
    //
    // The timeout is the estimate plus generous headroom, so a host that stops
    // accepting reports entirely cannot wedge the boot sequence.
    absolute_time_t deadline = make_timeout_time_ms((uint32_t)(len + 1) * 4 * delay + 2000);
    while (hid_typer_busy() && !time_reached(deadline)) sleep_ms(2);
    sleep_ms(delay * 2);   // let the final release land before the next line
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

// SSID at a merged-list index, for logging. Never returns NULL.
static const char *wifi_ssid_at(int idx) {
#if ENABLE_AP_MODE
    const char *ssid, *pw; int am;
    if (idx >= 0 && wifi_known_get((uint8_t)idx, &ssid, &pw, &am)) return ssid;
#else
    if (idx >= 0 && (size_t)idx < WIFI_NETWORK_COUNT) return WIFI_NETWORKS[idx].ssid;
#endif
    return "(unknown)";
}

static int scan_match_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r) return 0;
    g_scan.total_seen++;
#if ENABLE_AP_MODE
    // Park every AP we see, not just ones we have credentials for. AP setup
    // mode reads this back to offer a pick-list, and by then the chip is an
    // access point and can no longer scan.
    wifi_scan_cache_add((const char *)r->ssid, r->ssid_len, (int16_t)r->rssi,
                        r->auth_mode ? 1 : 0);
#endif
    // Compare this AP's SSID against each known network — stored credentials
    // first, then compiled ones, so a network you provisioned at the device
    // wins over a firmware entry of the same name.
#if ENABLE_AP_MODE
    size_t known = wifi_known_count();
#else
    size_t known = WIFI_NETWORK_COUNT;
#endif
    for (size_t i = 0; i < known; i++) {
#if ENABLE_AP_MODE
        const char *cfg, *pw; int am;
        if (!wifi_known_get((uint8_t)i, &cfg, &pw, &am)) continue;
#else
        const char *cfg = WIFI_NETWORKS[i].ssid;
#endif
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
                 wifi_ssid_at(g_scan.best_idx),
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
    // Index space is the MERGED list: stored credentials first, then compiled
    // ones. wifi_known_get() owns that ordering so the scan, the connect and
    // the web UI cannot disagree about which network index 3 is.
    const char *ssid, *password;
    int auth_mode;
#if ENABLE_AP_MODE
    if (idx < 0 || !wifi_known_get((uint8_t)idx, &ssid, &password, &auth_mode)) return -1;
#else
    if (idx < 0 || (size_t)idx >= WIFI_NETWORK_COUNT) return -1;
    ssid = WIFI_NETWORKS[idx].ssid;
    password = WIFI_NETWORKS[idx].password;
    auth_mode = WIFI_NETWORKS[idx].auth_mode;
#endif
    printf("[wifi] Connecting to '%s'...\n", ssid);

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
        const char *pw = (auth_mode == 4 || password[0] == '\0') ? NULL : password;
        int rc = cyw43_arch_wifi_connect_async(ssid, pw, wifi_auth_value(auth_mode));
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
    // Same gate as dbg(). This is the path the "ready" banner, the AP setup
    // address and the WiFi-failure message all take, and it had no quiet-boot
    // check at all.
    if (!boot_output_allowed()) {
        printf("[quiet] suppressed typed message:\n%s\n", msg);
        return;
    }
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
        uint8_t delay = TYPE_DELAY_MS;
#if ENABLE_SETTINGS
        delay = settings()->type_delay_ms;
#endif
        hid_push_type_string(msg + off, (uint8_t)chunk, delay);
        // Wait for core 1 to finish this chunk before queuing the next. Same
        // reasoning as dbg(): a fixed estimate under-runs once typer_step()
        // starts retrying rejected reports, and the chunks then interleave.
        absolute_time_t dl = make_timeout_time_ms((uint32_t)chunk * 4 * delay + 2000);
        while (hid_typer_busy() && !time_reached(dl)) sleep_ms(2);
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
#if ENABLE_KEYBOARD
    // The matrix lives here, not on core 0: this loop has nothing in it but
    // USB, so scan timing is unaffected by lwIP or the cyw43 background work.
    kb_init();
#endif
    printf("[usb] (core1) tusb_init done\n");

    for (;;) {
        hid_task();          // services tud_task() + drains the HID queue
#if ENABLE_KEYBOARD
        kb_task();           // scan → debounce → features → merged key state
#endif
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

    // ── 0. Bootmagic ──────────────────────────────────────────────────────────
    // Before USB, before WiFi, before anything that can fail: if the magic key
    // is held we jump straight to the bootloader. This is deliberately the
    // first thing that happens, so a wedged firmware is still recoverable
    // without reaching the BOOTSEL button.
#if ENABLE_KEYBOARD && KB_FEATURE_BOOTMAGIC
    kb_bootmagic();
#endif
#if ENABLE_SETTINGS
    // Before anything can type: settings decide whether dbg() speaks at all.
    settings_init();
#endif
#if ENABLE_SETTINGS && ENABLE_KEYBOARD && KB_FEATURE_BOOTMAGIC
    // Third boot gesture. Unlike the stored setting this is per-boot and is not
    // written to flash — "be quiet this once" after a noisy build, without
    // having to remember to turn it back on.
    // Loud first, and it wins: it is the way back from a stored quiet_boot on a
    // device that has therefore stopped telling you anything.
    if (kb_matrix_held_at_boot(LOUD_BOOT_ROW, LOUD_BOOT_COL)) {
        settings_force_loud_boot();
        printf("[settings] loud-boot key held: diagnostics forced ON for this boot\n");
    } else if (kb_matrix_held_at_boot(QUIET_BOOT_ROW, QUIET_BOOT_COL)) {
        settings_force_quiet_boot();
        printf("[settings] quiet-boot key held: diagnostics stay off the host\n");
    }
#endif
#if ENABLE_AP_MODE && ENABLE_KEYBOARD
    // Same idea as bootmagic and checked in the same breath: hold a key while
    // plugging in to come up as an access point instead of joining a network.
    // Deliberate rather than automatic — see include/ap_mode.h.
    bool want_ap = kb_matrix_held_at_boot(AP_MODE_ROW, AP_MODE_COL);
#elif ENABLE_AP_MODE
    bool want_ap = false;
#endif

    // ── 1. Auth init ──────────────────────────────────────────────────────────
    // Before auth_init(): auth_is_enabled() consults the override, so a
    // firmware whose env.h password is "" but which has one stored must come
    // up with auth ON, not off for the first request.
    auth_store_init();
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

#if ENABLE_AP_MODE
    // Credentials provisioned at the device, tried ahead of the compiled ones.
    wifi_store_init();
#endif

    cyw43_arch_enable_sta_mode();

#if ENABLE_AP_MODE
    // Say what is about to happen BEFORE doing it. The scan below runs either
    // way, but in setup mode it is a survey, not an attempt to join anything.
    // A log that reads "Scanning for known networks... Chose 'Foo'" and only
    // then mentions AP mode describes a device doing the opposite of what it is
    // actually about to do.
    if (want_ap) {
        dbg("[" NETHID_BUILD "] AP key held at boot: setup mode.");
        dbg("[" NETHID_BUILD "] Surveying nearby networks (not joining one)...");
    } else
#endif
    dbg("[" NETHID_BUILD "] USB ok (core1). Scanning for known networks...");

    // ── 4. WiFi ───────────────────────────────────────────────────────────────
    // Scan for available networks and pick the best one we have credentials
    // for, so the device can move between networks without reflashing.
    int net_idx = wifi_scan_select();

#if ENABLE_AP_MODE
    // The scan has to run first even in setup mode: once
    // cyw43_arch_enable_ap_mode() has run the chip is an access point and can no
    // longer survey the air, so the pick-list the setup page offers is captured
    // here or not at all. scan_match_cb() files every AP it sees, not only the
    // ones we have a password for.
    if (want_ap) {
        char d[72];
        snprintf(d, sizeof(d), "[" NETHID_BUILD "] %u networks cached for the setup page",
                 wifi_scan_cache_count());
        dbg(d);
        if (ap_mode_start()) {
            ap_start_services();
            char banner[220];
            ap_mode_banner(banner, sizeof(banner));
            absolute_time_t next_banner = get_absolute_time();
            for (;;) {
                cyw43_arch_poll();
                ap_mode_poll();
                wifi_store_commit_poll();
#if ENABLE_SETTINGS
                settings_commit_poll();
#endif
                // Repeating, not once at bring-up. Two things made the one-shot
                // version unreliable: the typer may still be draining an earlier
                // line when the AP comes up, and hid_push_type_string() silently
                // drops a push while it is busy; and the host may not be plugged
                // in, or focused anywhere useful, at that exact moment. In setup
                // mode this address is the only way in, so it is worth repeating.
                if (time_reached(next_banner)) {
                    type_to_host(banner, 1500);
                    next_banner = make_timeout_time_ms(20000);
                }
                led_status(5, 1);      // 5 blinks = AP setup mode
            }
        }
        dbg("[" NETHID_BUILD "] AP mode refused to start, continuing normally");
    }
#endif

    const char *chosen_ssid = (net_idx >= 0) ? wifi_ssid_at(net_idx) : "(none)";

    int wifi_err;
    if (net_idx < 0) {
        // No known network in range. Fall back to trying the first configured
        // entry blind (in case the scan missed a hidden SSID).
        dbg("[" NETHID_BUILD "] No known SSID scanned; trying first configured network blind");
        net_idx = 0;
        chosen_ssid = wifi_ssid_at(0);
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

#if ENABLE_AP_MODE && (AP_MODE_AUTO_FALLBACK || ENABLE_SETTINGS)
      if (
#if ENABLE_SETTINGS
          settings()->ap_auto_fallback
#else
          true
#endif
      ) {
        // Off by default. When enabled, a WiFi failure is no longer terminal:
        // come up as our own network so the credentials can be fixed in place.
        dbg("[" NETHID_BUILD "] WiFi failed, falling back to AP setup mode");
        if (ap_mode_start()) {
            ap_start_services();
            char banner[220];
            ap_mode_banner(banner, sizeof(banner));
            absolute_time_t next_banner = get_absolute_time();
            for (;;) {
                cyw43_arch_poll();
                ap_mode_poll();
                wifi_store_commit_poll();
#if ENABLE_SETTINGS
                settings_commit_poll();
#endif
                if (time_reached(next_banner)) {
                    type_to_host(banner, 1500);
                    next_banner = make_timeout_time_ms(20000);
                }
                led_status(5, 1);
            }
        }
      }
#endif
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
#if ENABLE_KEYBOARD && KB_FEATURE_BOOTMAGIC
        // QK_BOOT sets a flag on core 1; the reset itself happens here, on the
        // core that owns lwIP and the cyw43 link.
        kb_bootloader_poll();
#endif
#if ENABLE_KEYBOARD && KB_FEATURE_DYNAMIC_KEYMAP
        // Flash writes park core 1 and disable interrupts, so they happen here
        // in the main loop rather than inside the lwIP callback that asked for
        // them. See src/kb/keymap_store.cpp.
        kb_keymap_commit_poll();
#endif
#if ENABLE_KEYBOARD && KB_FEATURE_MACRO_STORE
        kb_macro_commit_poll();
#endif
#if ENABLE_KEYBOARD && KB_FEATURE_AUTOCLICK
        autoclick_commit_poll();
#endif
#if ENABLE_AP_MODE
        wifi_store_commit_poll();
        ap_mode_poll();               // handles a queued reboot-to-apply
#endif
#if ENABLE_SETTINGS
        settings_commit_poll();
#endif
        // Same reason as every other store: the erase parks core 1.
        auth_store_commit_poll();
#if OLED_ENABLE
        /* Core 0, never core 1. Rendering is cheap and idempotent — pixels that
         * do not change mark nothing dirty — so it can run every pass, and
         * oled_task() sends at most one page before returning. */
        oled_render_status();
        oled_task();
#endif
        sleep_ms(5);
    }
}
