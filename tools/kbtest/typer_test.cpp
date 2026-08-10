/*
 * typer_test — drive the real typer state machine through a deliberately flaky
 * endpoint and check that what the host receives spells the input exactly.
 *
 * The bug this exists for: tud_hid_n_report() returns false when the endpoint
 * cannot take another report, and the typer used to advance its state anyway.
 * A dropped press lost a character; a dropped release left the host holding the
 * previous key, so if that key carried shift, everything after it came out
 * capitalised. Intermittent, and worse when USB is busy — about one boot in
 * three.
 *
 * "Reconstruct what the host would see" is the whole point: checking that we
 * called the API N times proves nothing, because the failure was that some of
 * those calls did not take.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tusb.h"
#include "nethid.h"

extern uint32_t fake_now_ms;

/* Reject rate, in percent, applied to every report. */
int  g_reject_pct = 0;
/* What the host has decoded so far, plus its view of the currently held key. */
static char host_text[512];
static int  host_len;
static uint8_t held_kc, held_mod;

static char decode(uint8_t mod, uint8_t kc) {
    bool shift = (mod & (MOD_LSHIFT | MOD_RSHIFT)) != 0;
    if (kc >= KEY_A && kc <= KEY_Z) return (char)((shift ? 'A' : 'a') + (kc - KEY_A));
    if (kc >= KEY_1 && kc <= KEY_9) return shift ? ")!@#$%^&*"[kc - KEY_1 + 1] : (char)('1' + (kc - KEY_1));
    switch (kc) {
    case KEY_0: return shift ? ')' : '0';
    case KEY_SPACE: return ' ';
    case KEY_ENTER: return '\n';
    case KEY_DOT:   return shift ? '>' : '.';
    case KEY_MINUS: return shift ? '_' : '-';
    case KEY_EQUAL: return shift ? '+' : '=';
    case KEY_TAB:   return '\t';
    case KEY_BACKSLASH: return shift ? '|' : '\\';
    case KEY_GRAVE:     return shift ? '~' : '`';
    case KEY_SLASH: return shift ? '?' : '/';
    case KEY_SEMICOLON:  return shift ? ':' : ';';
    case KEY_LEFTBRACE:  return shift ? '{' : '[';
    case KEY_RIGHTBRACE: return shift ? '}' : ']';
    case KEY_COMMA:      return shift ? '<' : ',';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    default: return '\0';
    }
}

/* The rest of the TinyUSB surface hid.cpp touches. Always ready and always
 * mounted, so the ONLY variable in this test is whether a report is accepted.
 *
 * Completion is DEFERRED to the next tud_task(), which is what real TinyUSB
 * does. Calling it inline from the send would clear send_pending before
 * hid_task() gets to set it, latching the flag on forever — a fake that is
 * synchronous where the real thing is asynchronous does not test the same
 * state machine. */
extern void tud_hid_report_complete_cb(uint8_t, uint8_t const *, uint16_t);
static bool completion_due;

void tud_task(void) {
    if (completion_due) {
        completion_due = false;
        tud_hid_report_complete_cb(0, NULL, 0);
    }
}
bool tud_mounted(void) { return true; }
bool tud_hid_n_ready(uint8_t) { return true; }
bool tud_remote_wakeup(void) { return true; }

/* Stands in for TinyUSB. Rejects at random, exactly as a busy endpoint does. */
bool tud_hid_n_report(uint8_t, uint8_t, void const *data, uint16_t len) {
    if (g_reject_pct && (rand() % 100) < g_reject_pct) return false;
    const hid_keyboard_report_t *r = (const hid_keyboard_report_t *)data;
    /* A key is "typed" on the transition from not-held to held. */
    if (r->keycode[0] && r->keycode[0] != held_kc) {
        char c = decode(r->modifier, r->keycode[0]);
        if (c && host_len < (int)sizeof(host_text) - 1) host_text[host_len++] = c;
    }
    held_kc  = r->keycode[0];
    held_mod = r->modifier;
    (void)len;
    completion_due = true;   // fires on the next tud_task(), as TinyUSB does
    return true;
}

void host_reset(void) { host_len = 0; host_text[0] = 0; held_kc = held_mod = 0; }
const char *host_seen(void) { host_text[host_len] = 0; return host_text; }
uint8_t host_held_mod(void) { return held_mod; }
uint8_t host_held_kc(void)  { return held_kc; }

static int fails = 0;

static void run_case(const char *label, const char *text, int reject_pct, int seed) {
    // Settle first, with rejection off. send_pending is a file-static in hid.cpp
    // that hid_init() does not reset, and the previous case ends with a report
    // in flight whose completion callback has not run yet — leaving it latched
    // would block every send in this case and the output would be empty.
    g_reject_pct = 0;
    for (int i = 0; i < 200; i++) { hid_task(); fake_now_ms++; }

    srand(seed);
    completion_due = false;
    host_reset();
    g_reject_pct = reject_pct;

    hid_push_type_string(text, (uint8_t)strlen(text), 8);

    // Run until the typer has started and then finished, rather than for a
    // fixed count: with retries the number of passes needed depends on the
    // reject rate, and a fixed loop would silently truncate the output.
    bool started = false;
    for (int i = 0; i < 2000000; i++) {
        hid_task();
        if (hid_typer_busy()) started = true;
        else if (started) break;
        if ((i % 4) == 0) fake_now_ms++;
    }
    g_reject_pct = 0;
    for (int i = 0; i < 200; i++) { hid_task(); fake_now_ms++; }   // flush the last release

    bool text_ok = strcmp(host_seen(), text) == 0;
    bool clean   = host_held_kc() == 0 && host_held_mod() == 0;
    bool ok = text_ok && clean;
    printf("%-42s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf("    want %s\n    got  %s\n", text, host_seen());
        if (!clean) printf("    endpoint left holding kc=0x%02X mod=0x%02X\n",
                           host_held_kc(), host_held_mod());
        fails++;
    }
}

/*
 * Several strings queued back to back, exactly as the boot sequence does. This
 * is the case the single-string tests above cannot see: hid_task() used to fall
 * through to the queue drain whenever typer_step() returned false — which it
 * does for every ordinary inter-character gap — and the drain would overwrite
 * the in-flight string with the next one. Output came out spliced together,
 * fragments of one line inside another with the tails piling up at the end.
 */
static void run_queue_case(const char *label, int reject_pct, int seed) {
    g_reject_pct = 0;
    for (int i = 0; i < 200; i++) { hid_task(); fake_now_ms++; }

    srand(seed);
    completion_due = false;
    host_reset();
    g_reject_pct = reject_pct;

    static const char *lines[] = {
        "[AO] Scanning for known networks.\n",
        "[AO] connect attempt 1.\n",
        "[AO] link status = 2\n",
        "[AO] Connected. IP=192.168.8.205\n",
        "[AO] Servers up. READY.\n",
    };
    const int n = (int)(sizeof(lines) / sizeof(lines[0]));

    char want[512] = "";
    for (int i = 0; i < n; i++) {
        // Queued with no gap, the way dbg() does when the wait is too short.
        hid_push_type_string(lines[i], (uint8_t)strlen(lines[i]), 8);
        strcat(want, lines[i]);
    }

    bool started = false;
    for (int i = 0; i < 4000000; i++) {
        hid_task();
        if (hid_typer_busy()) started = true;
        else if (started) break;
        if ((i % 4) == 0) fake_now_ms++;
    }
    g_reject_pct = 0;
    for (int i = 0; i < 200; i++) { hid_task(); fake_now_ms++; }

    bool ok = strcmp(host_seen(), want) == 0 &&
              host_held_kc() == 0 && host_held_mod() == 0;
    printf("%-42s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf("    want |%s|\n    got  |%s|\n", want, host_seen());
        fails++;
    }
}

int main(void) {
    hid_init();
    const char *banner =
        "[NetHID] WiFi setup mode is running.\n"
        "1. Join the wifi network: NetHID-Setup\n";

    run_case("clean endpoint types exactly",        banner, 0,  1);
    run_case("30% rejection still types exactly",   banner, 30, 2);
    run_case("60% rejection still types exactly",   banner, 60, 3);
    run_case("mixed case survives rejection",       "WiFi SSID Ok", 40, 4);
    /* The shift symptom specifically: a shifted char followed by unshifted. */
    run_case("no shift bleed after a capital",      "Wi Fi Ap Ok", 50, 5);

    run_queue_case("5 queued lines stay in order",       0,  6);
    run_queue_case("5 queued lines, 30% rejection",       30, 7);
    run_queue_case("5 queued lines, 60% rejection",       60, 8);

    printf("\n%s\n", fails ? "FAILURES" : "all green");
    return fails;
}
