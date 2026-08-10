/* wifi_store.cpp — see include/wifi_store.h */

#include "wifi_store.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

#define WS_MAGIC   0x5346484Eu     /* "NHFS" */
#define WS_VERSION 1

typedef struct {
    uint32_t    magic;
    uint16_t    version;
    uint8_t     count;
    uint8_t     reserved;
    uint32_t    crc;
    wifi_cred_t creds[WIFI_STORE_MAX];
} ws_blob_t;

#define WS_PROG_LEN (((int)sizeof(ws_blob_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)

static_assert(WS_PROG_LEN <= FLASH_SECTOR_SIZE,
              "wifi credential blob does not fit one sector — lower WIFI_STORE_MAX");

/* Third sector from the end. keymap has the last, macros the second. */
#define WS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)

static wifi_cred_t   creds[WIFI_STORE_MAX];
static volatile bool save_pending;
static volatile bool dirty;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static bool load_from_flash(void) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + WS_FLASH_OFFSET);
    ws_blob_t b;
    memcpy(&b, base, sizeof(b));
    if (b.magic != WS_MAGIC || b.version != WS_VERSION) return false;
    if (b.count > WIFI_STORE_MAX) return false;
    if (crc32((const uint8_t *)b.creds, sizeof(b.creds)) != b.crc) {
        printf("[wifi-store] stored credentials failed CRC — ignoring\n");
        return false;
    }
    memcpy(creds, b.creds, sizeof(creds));
    /* Terminate defensively: a corrupt-but-CRC-valid blob must never hand an
     * unterminated string to cyw43_arch_wifi_connect_async(). */
    for (int i = 0; i < WIFI_STORE_MAX; i++) {
        creds[i].ssid[WIFI_SSID_MAX] = '\0';
        creds[i].password[WIFI_PASS_MAX] = '\0';
    }
    printf("[wifi-store] loaded %u stored network(s)\n", b.count);
    return true;
}

static void __no_inline_not_in_flash_func(ws_flash_commit)(const uint8_t *blob) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(WS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WS_FLASH_OFFSET, blob, WS_PROG_LEN);
    restore_interrupts(ints);
}

void wifi_store_commit_poll(void) {
    if (!save_pending) return;
    save_pending = false;

    static uint8_t raw[WS_PROG_LEN];
    memset(raw, 0xFF, sizeof(raw));

    ws_blob_t b = {};
    b.magic   = WS_MAGIC;
    b.version = WS_VERSION;
    b.count   = wifi_store_count();
    memcpy(b.creds, creds, sizeof(creds));
    b.crc     = crc32((const uint8_t *)b.creds, sizeof(b.creds));
    memcpy(raw, &b, sizeof(b));

    printf("[wifi-store] committing to flash\n");
    multicore_lockout_start_blocking();
    ws_flash_commit(raw);
    multicore_lockout_end_blocking();
    dirty = false;
    printf("[wifi-store] saved\n");
}

void wifi_store_init(void) {
    memset(creds, 0, sizeof(creds));
    load_from_flash();
    dirty = false;
    save_pending = false;
}

uint8_t wifi_store_count(void) {
    uint8_t n = 0;
    for (int i = 0; i < WIFI_STORE_MAX; i++) if (creds[i].in_use) n++;
    return n;
}

const wifi_cred_t *wifi_store_get(uint8_t idx) {
    uint8_t n = 0;
    for (int i = 0; i < WIFI_STORE_MAX; i++)
        if (creds[i].in_use && n++ == idx) return &creds[i];
    return NULL;
}

bool wifi_store_set(const char *ssid, const char *password, uint8_t auth_mode) {
    if (!ssid || !ssid[0]) return false;

    int slot = -1;
    for (int i = 0; i < WIFI_STORE_MAX; i++)
        if (creds[i].in_use && strcmp(creds[i].ssid, ssid) == 0) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < WIFI_STORE_MAX; i++)
            if (!creds[i].in_use) { slot = i; break; }
    if (slot < 0) return false;

    memset(&creds[slot], 0, sizeof(creds[slot]));
    strncpy(creds[slot].ssid, ssid, WIFI_SSID_MAX);
    if (password) strncpy(creds[slot].password, password, WIFI_PASS_MAX);
    creds[slot].auth_mode = auth_mode;
    creds[slot].in_use = 1;
    dirty = true;
    return true;
}

bool wifi_store_remove(const char *ssid) {
    for (int i = 0; i < WIFI_STORE_MAX; i++)
        if (creds[i].in_use && strcmp(creds[i].ssid, ssid) == 0) {
            memset(&creds[i], 0, sizeof(creds[i]));
            dirty = true;
            return true;
        }
    return false;
}

void wifi_store_clear(void) {
    memset(creds, 0, sizeof(creds));
    dirty = true;
}

/* Stored first, then compiled: a network provisioned at the device beats one
 * baked into the firmware with the same name, so a changed password is fixable
 * without a rebuild. */
uint8_t wifi_known_count(void) {
    return (uint8_t)(wifi_store_count() + WIFI_NETWORK_COUNT);
}

bool wifi_known_get(uint8_t i, const char **ssid, const char **password, int *auth) {
    uint8_t ns = wifi_store_count();
    if (i < ns) {
        const wifi_cred_t *c = wifi_store_get(i);
        if (!c) return false;
        *ssid = c->ssid;
        *password = c->password;
        *auth = c->auth_mode;
        return true;
    }
    i = (uint8_t)(i - ns);
    if (i >= WIFI_NETWORK_COUNT) return false;
    *ssid = WIFI_NETWORKS[i].ssid;
    *password = WIFI_NETWORKS[i].password;
    *auth = WIFI_NETWORKS[i].auth_mode;
    return true;
}

void wifi_store_save_request(void) { save_pending = true; }
bool wifi_store_dirty(void)        { return dirty; }

/* ── Scan cache ───────────────────────────────────────────────────────────── */

static wifi_scan_entry_t scan_cache[WIFI_SCAN_CACHE_MAX];
static uint8_t           scan_n;

void wifi_scan_cache_clear(void) { scan_n = 0; memset(scan_cache, 0, sizeof(scan_cache)); }

void wifi_scan_cache_add(const char *ssid, uint8_t ssid_len, int16_t rssi, uint8_t secure) {
    /* A hidden network broadcasts an empty SSID. Listing a row you cannot
     * click is worse than not listing it — you would still have to type the
     * name, and now you are also wondering which blank row is yours. */
    if (!ssid_len || !ssid[0]) return;
    if (ssid_len > WIFI_SSID_MAX) ssid_len = WIFI_SSID_MAX;

    for (uint8_t i = 0; i < scan_n; i++) {
        if (strncmp(scan_cache[i].ssid, ssid, ssid_len) == 0 &&
            scan_cache[i].ssid[ssid_len] == '\0') {
            /* Same network on another band or another AP in a mesh. Keep the
             * strongest, since that is the one we would actually join. */
            if (rssi > scan_cache[i].rssi) {
                scan_cache[i].rssi = rssi;
                scan_cache[i].secure = secure;
            }
            return;
        }
    }
    if (scan_n >= WIFI_SCAN_CACHE_MAX) return;

    memcpy(scan_cache[scan_n].ssid, ssid, ssid_len);
    scan_cache[scan_n].ssid[ssid_len] = '\0';
    scan_cache[scan_n].rssi = rssi;
    scan_cache[scan_n].secure = secure;
    scan_n++;
}

uint8_t wifi_scan_cache_count(void) { return scan_n; }

const wifi_scan_entry_t *wifi_scan_cache_get(uint8_t i) {
    return (i < scan_n) ? &scan_cache[i] : NULL;
}
