/*
 * wifi_store.h — WiFi credentials that survive a reflash and can be set from
 * the web UI.
 *
 * Compiled credentials (WIFI_NETWORKS[] from env.h) stay exactly as they were
 * and are always tried. This adds a second list, stored in its own flash
 * sector, that you can add to from a browser — which is the whole point of AP
 * mode, since a device you can reach but cannot reconfigure is not much use.
 *
 * Lookup order is stored-first: a network you provisioned at the device beats
 * one baked into the firmware with the same SSID, so fixing a changed password
 * does not need a rebuild.
 *
 * ── Storage note ────────────────────────────────────────────────────────────
 * Passwords are held in flash as plaintext. That is already true of anything
 * in env.h, which ends up in the binary — but it is worth stating plainly:
 * anyone with physical access and `picotool save` can read them. Do not
 * provision a network you would not also compile in.
 *
 * Sector: third from the end. keymap owns the last, macros the second.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#ifndef WIFI_STORE_MAX
#define WIFI_STORE_MAX 8
#endif

#define WIFI_SSID_MAX 32      /* 802.11 limit */
#define WIFI_PASS_MAX 63      /* WPA2 PSK ASCII limit */

typedef struct {
    char    ssid[WIFI_SSID_MAX + 1];
    char    password[WIFI_PASS_MAX + 1];
    uint8_t auth_mode;        /* same encoding as wifi_network_t */
    uint8_t in_use;
} wifi_cred_t;

void wifi_store_init(void);

uint8_t            wifi_store_count(void);
const wifi_cred_t *wifi_store_get(uint8_t i);

/* Add, or replace an existing entry with the same SSID. Returns false only if
 * the table is full or the SSID is empty. */
bool wifi_store_set(const char *ssid, const char *password, uint8_t auth_mode);
bool wifi_store_remove(const char *ssid);
void wifi_store_clear(void);

/* Total networks known to the device: stored plus compiled. Index 0..n-1 walks
 * stored entries first, then WIFI_NETWORKS[]. */
uint8_t wifi_known_count(void);
bool    wifi_known_get(uint8_t i, const char **ssid, const char **password, int *auth);

/* ── Scan cache ──────────────────────────────────────────────────────────────
 * Nearby networks, captured while the chip is still in station mode.
 *
 * This has to be a cache rather than a live call. Once cyw43_arch_enable_ap_mode()
 * has run the chip is an access point, and asking it to scan from there is at
 * best unreliable and at worst drops the client you are configuring it from.
 * So main() scans BEFORE bringing the AP up and parks the results here; the
 * setup page reads them back.
 */
#ifndef WIFI_SCAN_CACHE_MAX
#define WIFI_SCAN_CACHE_MAX 24
#endif

typedef struct {
    char    ssid[WIFI_SSID_MAX + 1];
    int16_t rssi;
    uint8_t secure;      /* 0 = open, 1 = encrypted */
} wifi_scan_entry_t;

void wifi_scan_cache_clear(void);
/* Ignores duplicates and empty/hidden SSIDs; keeps the strongest sighting. */
void wifi_scan_cache_add(const char *ssid, uint8_t ssid_len, int16_t rssi, uint8_t secure);
uint8_t wifi_scan_cache_count(void);
const wifi_scan_entry_t *wifi_scan_cache_get(uint8_t i);

void wifi_store_save_request(void);
void wifi_store_commit_poll(void);
bool wifi_store_dirty(void);
