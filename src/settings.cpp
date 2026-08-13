/* settings.cpp — see include/settings.h */

#include "settings.h"
#include "config.h"
#include "hid_layout.h"   /* HID_LAYOUT_NAMES / _COUNT for the layout field */
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

#define ST_MAGIC 0x5453484Eu     /* "NHST" */

/* Fourth sector from the end: keymap (last), macros, wifi, settings. */
#define ST_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)

#ifndef KB_DEBUG_MATRIX
#define KB_DEBUG_MATRIX 0
#endif
#ifndef TAPPING_TERM
#define TAPPING_TERM 200
#endif
#ifndef AP_MODE_AUTO_FALLBACK
#define AP_MODE_AUTO_FALLBACK 0
#endif

/*
 * One table drives everything: defaults, ranges, the setter, the JSON the web
 * page renders from, and which bit in `set_mask` tracks the override. Adding a
 * setting is one row here and one line of UI — there is no second place to
 * forget to update.
 */
typedef enum { T_BOOL, T_U8, T_U16, T_ENUM } stype_t;

typedef struct {
    const char *name;
    stype_t     type;
    size_t      offset;
    long        min, max;
    long        dflt;
    const char *help;
    /* T_ENUM only: NULL-terminated names, index = value. The page renders a
     * list of these instead of a number box, because "keyboard_layout: 1" is
     * not a thing anyone can answer without reading the source. */
    const char *const *opts;
} sfield_t;

#define F(name, type, member, mn, mx, df, help) \
    { name, type, offsetof(settings_t, member), mn, mx, df, help, NULL },
#define FE(name, member, mx, df, opts_, help) \
    { name, T_ENUM, offsetof(settings_t, member), 0, mx, df, help, opts_ },

static const sfield_t FIELDS[] = {
    F("quiet_boot",        T_BOOL, quiet_boot,        0, 1,     QUIET_BOOT,
      "Stop typing boot diagnostics into the host")
    F("debug_matrix",      T_BOOL, debug_matrix,      0, 1,     KB_DEBUG_MATRIX,
      "Log every matrix edge to the serial console")
    F("type_delay_ms",     T_U8,   type_delay_ms,     0, 100,   TYPE_DELAY_MS,
      "Delay between typed characters; raise it if a host drops keys")
    F("ap_auto_fallback",  T_BOOL, ap_auto_fallback,  0, 1,     AP_MODE_AUTO_FALLBACK,
      "Start setup mode automatically when no known network is in range")
    F("session_timeout_s", T_U16,  session_timeout_s, 30, 86400, SESSION_TIMEOUT_S,
      "Idle time before a web session expires")
    F("lockout_s",         T_U16,  lockout_s,         5, 3600,  LOCKOUT_S,
      "Lockout after too many failed logins")
    F("max_auth_attempts", T_U8,   max_auth_attempts, 1, 50,    MAX_AUTH_ATTEMPTS,
      "Failed logins before the lockout applies")
    F("tapping_term_ms",   T_U16,  tapping_term_ms,   50, 1000, TAPPING_TERM,
      "How long a dual-role key must be held to count as a hold")
    F("autoclick_ms",      T_U16,  autoclick_ms,       0, 5000, 0,
      "Autoclick interval in ms; 0 uses each key's own rate")
    FE("keyboard_layout",  keyboard_layout, HID_LAYOUT_COUNT - 1, KEYBOARD_LAYOUT,
       HID_LAYOUT_NAMES,
       "Layout the HOST is set to; a wrong value mistypes at, hash, tilde, "
       "quote, backslash and pipe")
};
#undef F
#undef FE

#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))
static_assert(NFIELDS <= 32, "set_mask is 32 bits");

typedef struct {
    uint32_t   magic;
    uint16_t   version;
    uint16_t   nfields;      /* guards against a rebuild that added a field */
    uint32_t   crc;
    uint32_t   set_mask;     /* bit i = FIELDS[i] has been overridden */
    settings_t values;
} st_blob_t;

#define ST_PROG_LEN (((int)sizeof(st_blob_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)
static_assert(ST_PROG_LEN <= FLASH_SECTOR_SIZE, "settings blob does not fit one sector");

static settings_t    live;
static uint32_t      set_mask;
static volatile bool save_pending;
static volatile bool dirty;
static bool          forced_quiet;
static bool          forced_loud;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static void field_write(const sfield_t *f, long v) {
    uint8_t *base = (uint8_t *)&live + f->offset;
    switch (f->type) {
    case T_BOOL:
    case T_ENUM:
    case T_U8:  *(uint8_t  *)base = (uint8_t)v;  break;
    case T_U16: *(uint16_t *)base = (uint16_t)v; break;
    }
}

static long field_read(const sfield_t *f) {
    const uint8_t *base = (const uint8_t *)&live + f->offset;
    switch (f->type) {
    case T_BOOL:
    case T_ENUM:
    case T_U8:  return *(const uint8_t  *)base;
    case T_U16: return *(const uint16_t *)base;
    }
    return 0;
}

/* Any field without an override tracks config.h. That is the point of the
 * mask: without it, saving once would freeze every setting at whatever the
 * defaults happened to be that day, and a later change to config.h would
 * silently do nothing. */
static void apply_defaults(void) {
    for (int i = 0; i < NFIELDS; i++)
        if (!(set_mask & (1u << i))) field_write(&FIELDS[i], FIELDS[i].dflt);
}

static bool load_from_flash(void) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + ST_FLASH_OFFSET);
    st_blob_t b;
    memcpy(&b, base, sizeof(b));
    if (b.magic != ST_MAGIC || b.version != SETTINGS_VERSION) return false;
    if (b.nfields != NFIELDS) {
        printf("[settings] stored blob has %u fields, firmware has %d — using defaults\n",
               b.nfields, NFIELDS);
        return false;
    }
    if (crc32((const uint8_t *)&b.values, sizeof(b.values)) != b.crc) {
        printf("[settings] stored settings failed CRC — using defaults\n");
        return false;
    }
    live     = b.values;
    set_mask = b.set_mask;
    return true;
}

static void __no_inline_not_in_flash_func(st_flash_commit)(const uint8_t *blob) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(ST_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(ST_FLASH_OFFSET, blob, ST_PROG_LEN);
    restore_interrupts(ints);
}

void settings_commit_poll(void) {
    if (!save_pending) return;
    save_pending = false;

    static uint8_t raw[ST_PROG_LEN];
    memset(raw, 0xFF, sizeof(raw));
    st_blob_t b = {};
    b.magic    = ST_MAGIC;
    b.version  = SETTINGS_VERSION;
    b.nfields  = NFIELDS;
    b.set_mask = set_mask;
    b.values   = live;
    b.crc      = crc32((const uint8_t *)&b.values, sizeof(b.values));
    memcpy(raw, &b, sizeof(b));

    printf("[settings] committing to flash\n");
    multicore_lockout_start_blocking();
    st_flash_commit(raw);
    multicore_lockout_end_blocking();
    dirty = false;
    printf("[settings] saved\n");
}

void settings_init(void) {
    memset(&live, 0, sizeof(live));
    set_mask = 0;
    if (!load_from_flash()) set_mask = 0;
    apply_defaults();
    dirty = false;
    save_pending = false;
    printf("[settings] %d field(s), %d overridden\n",
           NFIELDS, __builtin_popcount(set_mask));
}

/* Reports STORED values. The boot keys are deliberately NOT folded in here:
 * this is what the settings page renders, and showing quiet_boot as "set"
 * because someone held a key would be a lie about the saved configuration. */
const settings_t *settings(void) { return &live; }

bool settings_quiet_boot_effective(void) {
    if (forced_loud)  return false;   /* the override, and it beats everything */
    if (forced_quiet) return true;
    return live.quiet_boot != 0;
}

void settings_force_quiet_boot(void) { forced_quiet = true; }
void settings_force_loud_boot(void)  { forced_loud  = true; }

static const sfield_t *find(const char *name, int *idx_out) {
    for (int i = 0; i < NFIELDS; i++)
        if (strcmp(FIELDS[i].name, name) == 0) {
            if (idx_out) *idx_out = i;
            return &FIELDS[i];
        }
    return NULL;
}

bool settings_is_overridden(const char *name) {
    int i;
    if (!find(name, &i)) return false;
    return (set_mask & (1u << i)) != 0;
}

bool settings_set(const char *name, long value) {
    int i;
    const sfield_t *f = find(name, &i);
    if (!f) return false;
    /* Clamp-and-accept would be friendlier and wronger: a silently corrected
     * value looks like it worked and behaves like something else. */
    if (value < f->min || value > f->max) return false;
    field_write(f, value);
    set_mask |= (1u << i);
    dirty = true;
    return true;
}

bool settings_reset(const char *name) {
    int i;
    const sfield_t *f = find(name, &i);
    if (!f) return false;
    set_mask &= ~(1u << i);
    field_write(f, f->dflt);
    dirty = true;
    return true;
}

void settings_reset_all(void) {
    set_mask = 0;
    apply_defaults();
    dirty = true;
}

void settings_save_request(void) { save_pending = true; }
bool settings_dirty(void)        { return dirty; }

/* Serialise the whole table for the settings page: value, default, range, type
 * and help text, so the UI is generated rather than hand-maintained. */
int settings_to_json(char *buf, size_t cap) {
    int o = snprintf(buf, cap, "{\"ok\":true,\"dirty\":%s,\"fields\":[",
                     dirty ? "true" : "false");
    int written = 0;
    for (int i = 0; i < NFIELDS; i++) {
        const sfield_t *f = &FIELDS[i];
        if ((size_t)o > cap - 300) break;
        o += snprintf(buf + o, cap - o,
                      "%s{\"name\":\"%s\",\"type\":\"%s\",\"value\":%ld,"
                      "\"default\":%ld,\"min\":%ld,\"max\":%ld,"
                      "\"overridden\":%s,\"help\":\"%s\"",
                      i ? "," : "", f->name,
                      f->type == T_BOOL ? "bool" :
                      f->type == T_ENUM ? "enum" : "int",
                      field_read(f), f->dflt, f->min, f->max,
                      (set_mask & (1u << i)) ? "true" : "false",
                      f->help);
        if (f->type == T_ENUM && f->opts) {
            o += snprintf(buf + o, cap - o, ",\"options\":[");
            for (int k = 0; f->opts[k]; k++)
                o += snprintf(buf + o, cap - o, "%s\"%s\"", k ? "," : "",
                              f->opts[k]);
            o += snprintf(buf + o, cap - o, "]");
        }
        o += snprintf(buf + o, cap - o, "}");
        written++;
    }
    /* Running out of buffer used to drop the remaining fields and still look
     * like a complete, valid answer — the page would render nine settings and
     * simply not have a tenth, with nothing anywhere saying why. Say so. */
    o += snprintf(buf + o, cap - o, "],\"count\":%d,\"total\":%d,"
                  "\"truncated\":%s}", written, NFIELDS,
                  written < NFIELDS ? "true" : "false");
    return o;
}
