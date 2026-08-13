/*
 * settings.h — options you can change without a reflash.
 *
 * Every field here has a compile-time #define in config.h as its default. The
 * store holds an override plus a "was this ever set" bit, so a value you never
 * touch keeps tracking config.h across rebuilds instead of freezing at whatever
 * it happened to be the first time you saved.
 *
 * ── What is NOT here, and why ───────────────────────────────────────────────
 * Not everything in config.h belongs on a settings page:
 *
 *   ENABLE_WEB / ENABLE_TCP / ENABLE_HTTPS / ENABLE_REMOTES / ENABLE_KEYBOARD
 *       These decide what gets compiled and which listeners bind at boot. A
 *       runtime toggle cannot un-link code, and "disable the web server" served
 *       from the web server is a button whose only function is to lock you out.
 *
 *   AUTH_REQUIRED / PASSWORD
 *       Authentication must not be weakenable by an authenticated session. If
 *       someone gets one session they should not be able to remove the need for
 *       the next one. Compile-time only, deliberately.
 *
 *   Matrix pins, MATRIX_ROWS/COLS, diode direction
 *       Hardware. Wrong values here do not misbehave, they stop the keyboard
 *       scanning at all — and the page you would fix them from needs the
 *       keyboard working to reach in AP mode.
 *
 * Sector: fourth from the end (keymap, macros, wifi own the three above it).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Bump when the layout changes; a mismatch falls back to compiled defaults
 * rather than reinterpreting old bytes as new fields. */
#define SETTINGS_VERSION 2

typedef struct {
    uint8_t  quiet_boot;         /* suppress boot diagnostics typed to the host */
    uint8_t  debug_matrix;       /* log every debounced matrix edge to UART      */
    uint8_t  type_delay_ms;      /* per-character delay for the string typer     */
    uint8_t  ap_auto_fallback;   /* start AP when no known network is in range   */
    uint16_t session_timeout_s;
    uint16_t lockout_s;
    uint8_t  max_auth_attempts;
    uint8_t  keyboard_layout;    /* hid_layout_t: which layout the HOST is set to */
    uint16_t tapping_term_ms;    /* dual-role hold threshold                     */
    uint16_t autoclick_ms;       /* 0 = use each slot's own interval             */
} settings_t;

void settings_init(void);

/* Live values. Always safe to call; fall back to the config.h default for any
 * field that has never been set. */
const settings_t *settings(void);

/* Which fields have been overridden, so the UI can show "default" honestly
 * rather than pretending config.h's value was a choice someone made. */
bool settings_is_overridden(const char *field);

/* Set one field by name. Returns false for an unknown field or a value outside
 * its sane range — the ranges exist so a typo cannot brick the device from the
 * page you would fix it from. */
bool settings_set(const char *field, long value);

/* Drop an override and go back to the compiled default. */
bool settings_reset(const char *field);
void settings_reset_all(void);

void settings_save_request(void);
void settings_commit_poll(void);
bool settings_dirty(void);

/* Serialise every field — value, default, range, type, help — so the settings
 * page is generated from the table rather than hand-maintained alongside it. */
int settings_to_json(char *buf, size_t cap);

/* Per-boot overrides taken from the boot keys. Neither is persisted: they are
 * gestures about this boot, not configuration changes.
 *
 * Loud wins over quiet, and over the stored setting. Quiet boot is the one
 * setting that can hide the evidence of its own consequences, so there has to
 * be a key that makes the device talk regardless of what it has been told. */
void settings_force_quiet_boot(void);
void settings_force_loud_boot(void);

/* Quiet boot after the boot keys are applied. Use this rather than reading
 * settings()->quiet_boot, which reports the STORED value — what the settings
 * page should display, not what this boot is doing. */
bool settings_quiet_boot_effective(void);
