/*
 * web.cpp — NetHID HTTP server with authentication
 *
 * Matches the Python web.py exactly:
 *   GET  /           → login page (unauthenticated) or keyboard/mouse UI
 *   POST /api/auth   → {"password":"..."} → set session cookie
 *   POST /api/logout → clear session cookie + global logout
 *   GET  /api/ping   → {"ok":true} — 401 when session expired
 *   POST /api/combo  → tap a key report (press+release); alias: /api/key
 *   POST /api/mouse  → mouse report
 *   POST /api/text   → type string
 *   POST /api/wake   → assert USB Remote Wakeup
 *   OPTIONS *        → CORS preflight
 */

#include "tusb.h"
#include "web.h"
#include "nethid.h"
#include "auth.h"
#include "auth_store.h"
#include "secrets.h"
#include "tabs.h"
#include "config.h"
#if ENABLE_REMOTES
#include "remotes.h"
#endif
#include "net_compat.h"
#if ENABLE_KEYBOARD && KB_FEATURE_DYNAMIC_KEYMAP
#include "kb/kb.h"
#include "kb/keymap_store.h"
#include "kb/layout.h"
#include "kb/autoclick.h"   /* NUM_AUTOCLICKS, for /api/keymap/info */
#if SPLIT_ENABLE
#include "split/split.h"
#endif
#endif
#if ENABLE_KEYBOARD && KB_FEATURE_MACRO_STORE
#include "kb/macro_store.h"
#endif
#if ENABLE_KEYBOARD && KB_FEATURE_AUTOCLICK
#include "kb/autoclick.h"   /* includes the board's keyboard.h for NUM_AUTOCLICKS */
#endif
#if ENABLE_AP_MODE
#include "wifi_store.h"
#include "ap_mode.h"
#endif
#if ENABLE_SETTINGS
#include "settings.h"
#endif
#include "hid_layout.h"   /* the keymap editor is told which layout to draw */
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// snprintf returns the length it WOULD have written, so an `o += snprintf()`
// chain can run past the end of the buffer; `sizeof(jb) - o` then wraps to a
// huge size_t and the next call writes off the end. Clamp after every step, so
// the response is truncated rather than the stack being scribbled on.
//
// File scope rather than inside a handler: several JSON builders need it and
// they do not all live under the same feature guard.
#define JB_CLAMP(o) do { \
    if ((o) < 0) (o) = 0; \
    if ((size_t)(o) >= sizeof(jb)) (o) = (int)sizeof(jb) - 1; \
} while (0)

// ── Embedded HTML ─────────────────────────────────────────────────────────────

#include "web_html.h"

// ── JSON helpers (same minimal parser as server.cpp) ─────────────────────────

static int web_json_int(const char *json, const char *key, int def) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    for (;;) {                              /* match the key as an object key, not a value */
        p = strstr(p, pat); if (!p) return def;
        const char *q = p + strlen(pat);
        while (*q==' ') q++;
        if (*q==':') { p = q + 1; break; }  /* "key": ... -> this is the real key */
        p += strlen(pat);                   /* key text appeared as a value; keep looking */
    }
    while (*p==' ') p++;
    bool neg=(*p=='-'); if(neg)p++;
    if(*p<'0'||*p>'9') return def;
    int v=0; while(*p>='0'&&*p<='9') v=v*10+(*p++-'0');
    return neg?-v:v;
}

static size_t web_json_str(const char *json, const char *key, char *buf, size_t blen) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    for (;;) {                              /* match the key as an object key, not a value */
        p = strstr(p, pat); if (!p) return 0;
        const char *q = p + strlen(pat);
        while (*q==' ') q++;
        if (*q==':') { p = q + 1; break; }  /* "key": ... -> this is the real key */
        p += strlen(pat);                   /* key text appeared as a value; keep looking */
    }
    while (*p==' ') p++;
    if (*p++!='"') return 0;
    size_t n=0;
    while (*p && *p!='"' && n<blen-1) {
        if (*p=='\\'){p++;switch(*p){case 'n':buf[n++]='\n';break;case 't':buf[n++]='\t';break;default:buf[n++]=*p;}}
        else buf[n++]=*p;
        p++;
    }
    buf[n]='\0'; return n;
}

static void web_json_int_arr(const char *json, const char *key, int *arr, int count) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat); if (!p) return;
    p += strlen(pat);
    while(*p==' ')p++;
    if(*p++!=':')return;
    while(*p==' ')p++;
    if(*p++!='[')return;
    for(int i=0;i<count;i++){
        while(*p==' '||*p==',')p++;
        if(*p==']'||!*p)break;
        bool neg=(*p=='-');if(neg)p++;
        if(*p>='0'&&*p<='9'){int v=0;while(*p>='0'&&*p<='9')v=v*10+(*p++-'0');arr[i]=neg?-v:v;}
    }
}

#if ENABLE_REMOTES
// Parse an unsigned 32-bit value (decimal). NEC codes can exceed INT_MAX, so a
// plain web_json_int would overflow; this reads the full magnitude.
static uint32_t web_json_u32(const char *json, const char *key, uint32_t def) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat); if (!p) return def;
    p += strlen(pat);
    while(*p==' ')p++;
    if(*p++!=':')return def;
    while(*p==' ')p++;
    if(*p<'0'||*p>'9')return def;
    uint32_t v=0; while(*p>='0'&&*p<='9') v=v*10u+(uint32_t)(*p++-'0');
    return v;
}

// Variable-length unsigned array parser. Reads up to `maxn` values into `arr`
// and returns how many were parsed. Used for IR/RF raw timing arrays whose
// length isn't known ahead of time. Negative values are clamped to 0.
static int web_json_u16_arr(const char *json, const char *key, uint16_t *arr, int maxn) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat); if (!p) return 0;
    p += strlen(pat);
    while(*p==' ')p++;
    if(*p++!=':')return 0;
    while(*p==' ')p++;
    if(*p++!='[')return 0;
    int n=0;
    while(n<maxn){
        while(*p==' '||*p==',')p++;
        if(*p==']'||!*p)break;
        if(*p=='-'){ p++; while(*p>='0'&&*p<='9')p++; arr[n++]=0; continue; }
        if(*p>='0'&&*p<='9'){
            long v=0; while(*p>='0'&&*p<='9'){ v=v*10+(*p++-'0'); if(v>65535)v=65535; }
            arr[n++]=(uint16_t)v;
        } else break;
    }
    return n;
}
#endif // ENABLE_REMOTES

// ── Cookie extraction ─────────────────────────────────────────────────────────

static void get_cookie(const char *headers, const char *name, char *buf, size_t blen) {
    buf[0] = '\0';
    const char *p = strstr(headers, "Cookie:");
    if (!p) { p = strstr(headers, "cookie:"); if (!p) return; }
    p += 7;
    const char *end = strstr(p, "\r\n");
    if (!end) end = p + strlen(p);

    char cookie_line[512] = {};
    size_t clen = (size_t)(end - p);
    if (clen >= sizeof(cookie_line)) clen = sizeof(cookie_line) - 1;
    memcpy(cookie_line, p, clen);

    // Parse name=value pairs
    char *tok = strtok(cookie_line, ";");
    while (tok) {
        while (*tok == ' ') tok++;
        size_t nlen = strlen(name);
        if (strncmp(tok, name, nlen) == 0 && tok[nlen] == '=') {
            const char *val = tok + nlen + 1;
            size_t vlen = strlen(val);
            if (vlen >= blen) vlen = blen - 1;
            memcpy(buf, val, vlen);
            buf[vlen] = '\0';
            return;
        }
        tok = strtok(nullptr, ";");
    }
}

// ── HTTP connection state ─────────────────────────────────────────────────────

#define HTTP_RECV_BUF 4096
#define MAX_HTTP_CONN 4

#define MAX_TX_FRAGS 16   // HEAD + up to N tab fragments + FOOTER

typedef struct {
    uint8_t  buf[HTTP_RECV_BUF];
    uint16_t len;
    // Chunked HTML send state (streams the current fragment, then advances
    // through `frags` — lets GET / serve HEAD + only the granted tabs + FOOTER
    // straight from flash with no RAM copy).
    const char *tx_ptr;
    size_t      tx_remaining;
    const char *frags[MAX_TX_FRAGS];
    uint8_t     frag_count;
    uint8_t     frag_idx;
    bool        keep_alive;   // reuse this connection for the next request?
    uint8_t     idle;         // idle poll ticks (reaped when too high)
} http_conn_t;

static http_conn_t   http_conns[MAX_HTTP_CONN];
static net_pcb *http_pcbs[MAX_HTTP_CONN];

static int   http_alloc(net_pcb *p){for(int i=0;i<MAX_HTTP_CONN;i++){if(!http_pcbs[i]){http_pcbs[i]=p;memset(&http_conns[i],0,sizeof(http_conns[i]));return i;}}return -1;}
static void  http_free(net_pcb *p) {for(int i=0;i<MAX_HTTP_CONN;i++) if(http_pcbs[i]==p){http_pcbs[i]=nullptr;return;}}
static http_conn_t *http_get(net_pcb *p){for(int i=0;i<MAX_HTTP_CONN;i++) if(http_pcbs[i]==p) return &http_conns[i]; return nullptr;}

static void http_close(net_pcb *pcb){
    http_free(pcb);
    net_arg(pcb,nullptr);net_recv(pcb,nullptr);
    net_err(pcb,nullptr);net_sent(pcb,nullptr);
    net_poll(pcb,nullptr,0);
    net_close(pcb);
}

// Case-insensitive substring search bounded to `n` bytes (for header scanning).
static inline char http_lc(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool ci_find(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m == 0 || n < m) return false;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        while (j < m && http_lc(hay[i+j]) == http_lc(needle[j])) j++;
        if (j == m) return true;
    }
    return false;
}

// Decide whether to keep the connection open after the response. HTTP/1.1
// defaults to keep-alive; "Connection: close" forces close; an explicit
// "Connection: keep-alive" forces reuse (e.g. on HTTP/1.0). Reusing the
// connection is what avoids a fresh (expensive) TLS handshake per request.
static bool req_keepalive(const char *req) {
    const char *he = strstr(req, "\r\n\r\n");
    size_t hlen = he ? (size_t)(he - req) : strlen(req);
    if (ci_find(req, hlen, "connection: close"))      return false;
    if (ci_find(req, hlen, "connection: keep-alive")) return true;
    return ci_find(req, hlen, "http/1.1");
}

// Finish a response: either reuse the connection for the next request
// (keep-alive — no new handshake) or close it.
static void http_done(net_pcb *pcb) {
    http_conn_t *hc = http_get(pcb);
    if (hc && hc->keep_alive) {
        hc->len = 0;
        hc->tx_ptr = ""; hc->tx_remaining = 0;
        hc->frag_count = 0; hc->frag_idx = 0;
        hc->idle = 0;
        net_sent(pcb, nullptr);   // nothing more to stream; recv stays armed
    } else {
        http_close(pcb);
    }
}

// ── Response helpers ──────────────────────────────────────────────────────────

static void http_send_full(net_pcb *pcb,
                            const char *hdr, size_t hlen,
                            const char *body, size_t blen) {
    net_write(pcb, hdr, (u16_t)hlen,
              TCP_WRITE_FLAG_COPY | (blen ? TCP_WRITE_FLAG_MORE : 0));
    if (body && blen)
        net_write(pcb, body, (u16_t)blen, TCP_WRITE_FLAG_COPY);
    net_output(pcb);
    http_done(pcb);
}

static void http_json(net_pcb *pcb, int code,
                      const char *body, const char *extra_hdr = nullptr) {
    http_conn_t *hc = http_get(pcb);
    bool ka = hc && hc->keep_alive;
    static char hdr[256];
    size_t blen = strlen(body);
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "%s"
        "\r\n",
        code,
        code==200?"OK":code==401?"Unauthorized":code==403?"Forbidden":"Not Found",
        blen,
        ka ? "keep-alive" : "close",
        extra_hdr ? extra_hdr : "");
    http_send_full(pcb, hdr, (size_t)hlen, body, blen);
}

// Chunked send for large HTML pages
static err_t http_sent_cb(void *arg, net_pcb *pcb, u16_t len);

// Send as much of the pending HTML as the stack will accept right now.
// Critically: only advance tx_ptr/tx_remaining for bytes tcp_write ACCEPTS.
// tcp_write can return ERR_MEM when the segment/pbuf pool is momentarily
// exhausted even if net_sndbuf() shows space; in that case we must NOT skip
// those bytes — we leave the state untouched and retry on the next sent_cb.
static void http_pump(net_pcb *pcb, http_conn_t *hc) {
    bool wrote_any = false;
    for (;;) {
        // Current fragment drained? advance to the next queued one.
        if (hc->tx_remaining == 0) {
            if (hc->frag_idx >= hc->frag_count) break;     // nothing left
            hc->tx_ptr       = hc->frags[hc->frag_idx++];
            hc->tx_remaining = hc->tx_ptr ? strlen(hc->tx_ptr) : 0;
            if (hc->tx_remaining == 0) continue;           // skip empty fragment
        }
        u16_t space = net_sndbuf(pcb);
        if (space == 0) break;                 // no room now; wait for sent_cb

        // Cap each write at 1024 bytes. Smaller writes are far more likely to
        // succeed when the TCP segment pool is under pressure, which keeps the
        // large page streaming reliably instead of stalling on ERR_MEM.
        u16_t chunk = (u16_t)(hc->tx_remaining < space ? hc->tx_remaining : space);
        if (chunk > 1024) chunk = 1024;

        err_t err = net_write(pcb, hc->tx_ptr, chunk, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            hc->tx_ptr       += chunk;
            hc->tx_remaining -= chunk;
            wrote_any = true;
        } else {
            // ERR_MEM or other transient error: stop, keep state, retry on the
            // next sent_cb (which fires as in-flight data is ACKed).
            break;
        }
    }
    if (wrote_any) net_output(pcb);
}

// True once every queued fragment has been fully written.
static bool http_tx_done(http_conn_t *hc) {
    return hc->tx_remaining == 0 && hc->frag_idx >= hc->frag_count;
}

// Send a response whose body is the concatenation of `n` flash fragments,
// streamed in order with no RAM copy. Content-Length is the sum of their lengths.
static void http_send_fragments(net_pcb *pcb, const char *const *frags, int n) {
    http_conn_t *hc = http_get(pcb);
    if (!hc) { http_close(pcb); return; }

    if (n > MAX_TX_FRAGS) n = MAX_TX_FRAGS;
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        hc->frags[i] = frags[i];
        total += frags[i] ? strlen(frags[i]) : 0;
    }
    hc->frag_count   = (uint8_t)n;
    hc->frag_idx     = 0;
    hc->tx_ptr       = "";
    hc->tx_remaining = 0;     // pump loads the first fragment

    char hdr[160];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Connection: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        (unsigned)total,
        (hc->keep_alive ? "keep-alive" : "close"));

    net_sent(pcb, http_sent_cb);

    if (net_write(pcb, hdr, (u16_t)hlen, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        http_close(pcb);
        return;
    }
    http_pump(pcb, hc);
}

static void http_send_html(net_pcb *pcb, const char *html) {
    const char *one[1] = { html };
    http_send_fragments(pcb, one, 1);
}

static err_t http_sent_cb(void *arg, net_pcb *pcb, u16_t len) {
    (void)arg; (void)len;
    http_conn_t *hc = http_get(pcb);
    if (!hc) return ERR_OK;
    hc->idle = 0;
    if (http_tx_done(hc)) { http_done(pcb); return ERR_OK; }
    http_pump(pcb, hc);
    return ERR_OK;
}

// ── Request dispatch ──────────────────────────────────────────────────────────

static void handle_request(net_pcb *pcb, const char *req, size_t len) {
    (void)len;

    http_conn_t *hc0 = http_get(pcb);
    if (hc0) hc0->keep_alive = req_keepalive(req);

    char method[8]={}, path[64]={};
    sscanf(req, "%7s %63s", method, path);
    char *q = strchr(path, '?'); if (q) *q='\0';

    // Find headers (everything before the body)
    const char *hdr_end = strstr(req, "\r\n\r\n");
    const char *body    = hdr_end ? hdr_end + 4 : "";

    // Extract session cookie (parse directly from the request — no big copy)
    char sid[64] = {};
    if (hdr_end) {
        get_cookie(req, "sid", sid, sizeof(sid));
    }

#if ENABLE_AP_MODE
    // Hiding a tab is cosmetic — the endpoints behind it would still answer.
    // In setup mode the only paths that exist are the ones needed to log in and
    // provision WiFi. Anything that can reach the HID queue is refused outright,
    // so an attacker who joins the setup network and skips the UI gets a 403
    // rather than your host's keyboard.
    if (ap_mode_active() && strcmp(method, "OPTIONS") != 0) {
        static const char *allowed[] = {
            "/", "/favicon.ico",
            // Login is a three-step handshake: authinfo, challenge, auth.
            // Omitting /api/challenge 403s the middle step and the page reports
            // "Cannot start login" with no way to get in at all.
            "/api/authinfo", "/api/challenge", "/api/auth", "/api/logout",
            // Read-only session and status, used by the page shell on every
            // load. None of these touch the HID queue.
            "/api/whoami", "/api/ping", "/api/hostinfo",
            "/api/wifi", "/api/wifi/scan", "/api/wifi/forget",
            "/api/wifi/save", "/api/wifi/apply",
            "/api/keymap", "/api/keymap/save", "/api/keymap/reset",
            "/api/keymap/encoders",
            "/api/macro", "/api/macro/save", "/api/macro/clear",
            "/api/autoclick", "/api/autoclick/save", "/api/autoclick/reset",
            "/api/settings", "/api/settings/save", "/api/settings/reset",
        };
        // /api/keymap/info, /api/keymap/layout and /api/keymap/<n> are all
        // GET-only reads under one prefix, so match the prefix rather than
        // enumerating every layer index.
        static const char *allowed_prefix[] = { "/api/keymap/" };

        bool ok = false;
        for (size_t i = 0; i < sizeof(allowed)/sizeof(allowed[0]) && !ok; i++)
            if (strcmp(path, allowed[i]) == 0) ok = true;
        for (size_t i = 0; i < sizeof(allowed_prefix)/sizeof(allowed_prefix[0]) && !ok; i++)
            if (strncmp(path, allowed_prefix[i], strlen(allowed_prefix[i])) == 0) ok = true;

        if (!ok) {
            http_json(pcb, 403, "{\"error\":\"setup_mode\","
                                "\"detail\":\"setup mode serves WiFi and keymap configuration only\"}");
            return;
        }
    }
#endif

    // ── CORS preflight ────────────────────────────────────────────────────────
    if (strcmp(method, "OPTIONS") == 0) {
        const char *r = "HTTP/1.0 204 No Content\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type\r\n\r\n";
        net_write(pcb, r, (u16_t)strlen(r), TCP_WRITE_FLAG_COPY);
        net_output(pcb);
        http_close(pcb);
        return;
    }

    // ── GET /api/authinfo — tells the login page whether to show a user field ─
    if (strcmp(method,"GET")==0 && strcmp(path,"/api/authinfo")==0) {
        http_json(pcb, 200, auth_is_multiuser() ? "{\"multiuser\":true}"
                                                 : "{\"multiuser\":false}");
        return;
    }

    // ── GET /api/challenge — issue a one-time nonce (no session required) ─────
    if (strcmp(method,"GET")==0 && strcmp(path,"/api/challenge")==0) {
        char nonce[AUTH_NONCE_HEX + 1];
        if (auth_make_challenge(nonce)) {
            char jb[64];
            snprintf(jb, sizeof(jb), "{\"nonce\":\"%s\"}", nonce);
            http_json(pcb, 200, jb);
        } else {
            http_json(pcb, 503, "{\"error\":\"busy\"}");
        }
        return;
    }

    // ── POST /api/auth — no session required ─────────────────────────────────
    //   HMAC:      {"user":"..","nonce":"..","response":"<hmac-sha256 hex>"}
    //   Plaintext: {"user":"..","password":".."}  (only if ALLOW_PLAINTEXT_AUTH)
    // "user" may be omitted to mean the first configured user.
    if (strcmp(method,"POST")==0 && strcmp(path,"/api/auth")==0) {
        char user[64] = {}, nonce[AUTH_NONCE_HEX + 1] = {}, resp[80] = {};
        web_json_str(body, "user",     user,  sizeof(user));
        web_json_str(body, "nonce",    nonce, sizeof(nonce));
        web_json_str(body, "response", resp,  sizeof(resp));
        uint32_t retry = 0;
        auth_result_t r;
        if (resp[0]) {
            r = auth_respond(user, nonce, resp, &retry);
        } else if (auth_plaintext_allowed()) {
            char pw[64] = {};
            web_json_str(body, "password", pw, sizeof(pw));
            r = auth_authenticate(user, pw, &retry);
        } else {
            http_json(pcb, 400, "{\"error\":\"hmac_required\"}");
            return;
        }
        if (r == AUTH_OK || r == AUTH_DISABLED) {
            char token[64] = {};
            auth_create_token(user, token);
            char cookie_hdr[192];
            snprintf(cookie_hdr, sizeof(cookie_hdr),
                     "Set-Cookie: sid=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%d\r\n",
                     token, SESSION_TIMEOUT_S);
            http_json(pcb, 200, "{\"ok\":true}", cookie_hdr);
        } else if (r == AUTH_LOCKED) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf),
                     "{\"error\":\"locked\",\"retry_after\":%u}", (unsigned)retry);
            http_json(pcb, 403, errbuf);
        } else {
            http_json(pcb, 401, "{\"error\":\"wrong_password\"}");
        }
        return;
    }

    // ── GET / — serve login page or the per-user main UI ──────────────────────
    if (strcmp(method,"GET")==0 && strcmp(path,"/")==0) {
        if (auth_is_enabled() && !auth_validate_token(sid)) {
            http_send_html(pcb, LOGIN_HTML);
            return;
        }
        // Assemble HEAD + only the tabs this user may see + FOOTER. Admin (and
        // the ungated case) gets all; others get their TAB_FOR grants.
        const char *who  = auth_user_name(auth_token_user_index(sid));
        bool        gate = auth_is_enabled();
        const char *frags[MAIN_TAB_COUNT + 2];
        int n = 0;
        frags[n++] = MAIN_HEAD;
        for (int i = 0; i < MAIN_TAB_COUNT; i++) {
#if !ENABLE_REMOTES
            if (strcmp(MAIN_TABS[i].id, "irdb") == 0) continue;   // IR not built in
            if (strcmp(MAIN_TABS[i].id, "learn") == 0) continue;  // capture not built in
#endif
#if !(ENABLE_KEYBOARD && KB_FEATURE_DYNAMIC_KEYMAP)
            if (strcmp(MAIN_TABS[i].id, "keymap") == 0) continue;  // no runtime keymap
#endif
#if !ENABLE_SETTINGS
            if (strcmp(MAIN_TABS[i].id, "settings") == 0) continue;
#endif
#if !ENABLE_AP_MODE
            if (strcmp(MAIN_TABS[i].id, "wifi") == 0) continue;    // no credential store
#else
            // Setup mode serves configuration, not control. WIFI and KEYMAP
            // change what the device will do later; the other tabs type and
            // click on the host RIGHT NOW, and this page is reachable by anyone
            // in radio range holding the AP password. See the note on the API
            // allowlist above — hiding these is only half of it.
            if (ap_mode_active() &&
                strcmp(MAIN_TABS[i].id, "wifi") != 0 &&
                strcmp(MAIN_TABS[i].id, "keymap") != 0 &&
                strcmp(MAIN_TABS[i].id, "settings") != 0) continue;
#endif
            bool allowed = !gate || tab_user_allowed(who, MAIN_TABS[i].id);
#if ENABLE_AP_MODE
            // Setup mode ignores per-user tab grants for the tabs it serves.
            //
            // TAB_GRANTS lives in env.h, so it can only be changed by
            // reflashing — and gating the recovery path behind a list you need
            // a reflash to edit is precisely the trap AP mode exists to avoid.
            // A non-admin user with grants configured would otherwise log into
            // setup mode and find a page with no tabs on it at all.
            //
            // Nothing is widened by this: the session is already authenticated,
            // and the API allowlist above still decides what is reachable.
            if (ap_mode_active()) allowed = true;
#endif
            if (allowed) frags[n++] = MAIN_TABS[i].html;
        }
        // n == 1 means HEAD only: every tab was filtered out and the user gets a
        // header and a log box. Say so, because from the browser it looks like
        // the page simply failed to load.
        if (n == 1) {
            printf("[web] WARNING: no tabs served to '%s'"
                   " — check TAB_GRANTS in env.h\n",
                   (who && who[0]) ? who : "(anonymous)");
        }
        frags[n++] = MAIN_FOOTER;
        http_send_fragments(pcb, frags, n);
        return;
    }

#if ENABLE_REMOTES
    // ── GET /irdb — the IR code finder page (gated; loaded by the IR DB tab) ───
    if (strcmp(method,"GET")==0 && strcmp(path,"/irdb")==0) {
        if (auth_is_enabled() && !auth_validate_token(sid))
            http_send_html(pcb, LOGIN_HTML);
        else
            http_send_html(pcb, IRDB_HTML);
        return;
    }
    // ── GET /learn — the capture/replay page (gated; loaded by the Learn tab) ──
    if (strcmp(method,"GET")==0 && strcmp(path,"/learn")==0) {
        if (auth_is_enabled() && !auth_validate_token(sid))
            http_send_html(pcb, LOGIN_HTML);
        else
            http_send_html(pcb, LEARN_HTML);
        return;
    }
#endif

    // ── GET /api/ping ─────────────────────────────────────────────────────────
    if (strcmp(method,"GET")==0 && strcmp(path,"/api/ping")==0) {
        if (auth_is_enabled()) {
            if (!auth_validate_token(sid)) {
                http_json(pcb, 401, "{\"error\":\"session_expired\"}");
                return;
            }
            // Slide the cookie's expiry forward on every heartbeat. Without this
            // the cookie carries a fixed Max-Age from login and the BROWSER drops
            // it ~5 min after sign-in regardless of activity — so the next ping
            // arrives with no cookie and the user is logged out mid-use. The
            // heartbeat fires well inside the window, so re-issuing the same sid
            // with a fresh Max-Age keeps the cookie alive exactly as long as the
            // server-side session (i.e. until SESSION_TIMEOUT_S of inactivity).
            char cookie_hdr[192];
            snprintf(cookie_hdr, sizeof(cookie_hdr),
                     "Set-Cookie: sid=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%d\r\n",
                     sid, SESSION_TIMEOUT_S);
            http_json(pcb, 200, "{\"ok\":true}", cookie_hdr);
        } else {
            http_json(pcb, 200, "{\"ok\":true}");
        }
        return;
    }

    // ── GET /api/hostinfo — detected host OS (from USB enumeration) ────────────
    if (strcmp(method,"GET")==0 && strcmp(path,"/api/hostinfo")==0) {
        if (auth_is_enabled() && !auth_validate_token(sid)) {
            http_json(pcb, 401, "{\"error\":\"session_expired\"}");
        } else {
            const char *os = (usb_host_os() == HOST_OS_WINDOWS) ? "windows" : "unknown";
            char body[48];
            snprintf(body, sizeof(body), "{\"os\":\"%s\"}", os);
            http_json(pcb, 200, body);
        }
        return;
    }

    // ── POST /api/logout ──────────────────────────────────────────────────────
    if (strcmp(method,"POST")==0 && strcmp(path,"/api/logout")==0) {
        auth_invalidate_token(sid);
        auth_logout();
        http_json(pcb, 200, "{\"ok\":true}",
                  "Set-Cookie: sid=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0\r\n");
        return;
    }

    // ── All remaining routes require auth ─────────────────────────────────────
    if (auth_is_enabled() && !auth_validate_token(sid)) {
        http_json(pcb, 401, "{\"error\":\"session_expired\"}");
        return;
    }

#if ENABLE_PASSWORD_STORE
    // ── Login password ────────────────────────────────────────────────────────
    //   GET  /api/password         status only — never a password, not even a hash
    //   POST /api/password         {"current":"..","new":".."}
    //   POST /api/password/reset   {"current":".."} back to the compiled password
    //
    // Deliberately absent from the AP-mode allowlist above: setup mode exists to
    // get a device onto a network, and letting it change the login would make
    // "join the setup AP" a way to take the device over.
    //
    // The change is applied to RAM immediately and the flash write is queued;
    // there is no separate save step, because a password change you have to
    // remember to save is one that silently reverts at the next power cycle.
    // ALLOW_PLAINTEXT_AUTH=0 is a deliberate statement that no password may
    // cross this network, and login honours it by never sending one. Changing a
    // password cannot: the new one has to reach the device somehow. Rather than
    // quietly making that setting a half-truth, refuse over plain HTTP and say
    // why — HTTPS satisfies both, since the password is then encrypted in
    // transit and the operator's requirement is met.
    #define PW_CHANGE_ALLOWED (ALLOW_PLAINTEXT_AUTH || ENABLE_HTTPS)

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/password") == 0) {
        int uidx = auth_token_user_index(sid);
        if (uidx < 0) uidx = 0;
        // 512, and the user name clamped: the fixed part plus the longest `why`
        // is already ~300 bytes, so a long USER_LIST name would have truncated
        // the JSON mid-string. snprintf would not overflow, but the page would
        // get unparseable JSON and hide the panel with nothing to explain it.
        char jb[512];
        snprintf(jb, sizeof(jb),
            "{\"ok\":true,\"user\":\"%.64s\",\"multiuser\":%s,\"min_len\":%u,"
            "\"max_len\":%u,\"changed\":%s,\"stored\":%s,\"saving\":%s,"
            "\"build_id\":\"%s\",\"secure\":%s,\"can_change\":%s,\"why\":\"%s\"}",
            auth_user_name(uidx),
            auth_is_multiuser() ? "true" : "false",
            (unsigned)WEB_PASSWORD_MIN_LEN, (unsigned)AUTH_PW_MAX,
            auth_store_password(uidx) ? "true" : "false",
            auth_store_stored() ? "true" : "false",
            auth_store_save_pending() ? "true" : "false",
            auth_store_build_id_str(),
            ENABLE_HTTPS ? "true" : "false",
            PW_CHANGE_ALLOWED ? "true" : "false",
            PW_CHANGE_ALLOWED ? "" :
                "this firmware was built with ALLOW_PLAINTEXT_AUTH=0, so no "
                "password may cross the network - setting one requires "
                "ENABLE_HTTPS");
        http_json(pcb, 200, jb);
        return;
    }
#endif

    // Who am I? (used by the UI to tailor what it shows per user)
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/whoami") == 0) {
        const char *who = auth_user_name(auth_token_user_index(sid));
        char jb[96];
        snprintf(jb, sizeof(jb), "{\"user\":\"%s\"}", who);
        http_json(pcb, 200, jb);
        return;
    }


#if ENABLE_KEYBOARD && KB_FEATURE_AUTOCLICK
    // ── Autoclick slots ───────────────────────────────────────────────────────
    //   GET  /api/autoclick        every slot
    //   POST /api/autoclick        {"slot":n,"target":kc,"interval_ms":ms,"trigger":bits}
    //   POST /api/autoclick/save   persist to flash (queued)
    //   POST /api/autoclick/reset  {"erase":bool} back to the compiled slots
    //
    // `target` is an ordinary keycode, so a slot can repeat a mouse button, a
    // letter, LCTL(KC_V), a macro — anything the feature chain understands.
    //
    // Guarded on AUTOCLICK alone, not on DYNAMIC_KEYMAP: editing a slot has
    // nothing to do with editing the keymap, and tying them together would make
    // a board that wants one have to take the other.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/autoclick") == 0) {
        static char jb[640];
        int o = snprintf(jb, sizeof(jb),
            "{\"ok\":true,\"count\":%u,\"min_ms\":%u,\"max_ms\":%u,"
            "\"tap_window_ms\":%u,\"stored\":%s,\"dirty\":%s,\"saving\":%s,"
            "\"rate_override_ms\":%u,\"slots\":[",
            autoclick_count(), (unsigned)AC_MIN_INTERVAL_MS,
            (unsigned)AC_MAX_INTERVAL_MS, (unsigned)AC_TAP_WINDOW_MS,
            autoclick_stored() ? "true" : "false",
            autoclick_dirty()  ? "true" : "false",
            autoclick_save_pending() ? "true" : "false",
#if ENABLE_SETTINGS
            (unsigned)settings()->autoclick_ms);
#else
            0u);
#endif
        JB_CLAMP(o);
        for (uint8_t i = 0; i < autoclick_count(); i++) {
            const autoclick_slot_t *s = autoclick_slot(i);
            if (!s || (size_t)o > sizeof(jb) - 64) break;
            o += snprintf(jb + o, sizeof(jb) - o,
                          "%s{\"target\":%u,\"interval_ms\":%u,\"trigger\":%u}",
                          i ? "," : "", (unsigned)s->target,
                          (unsigned)s->interval_ms, (unsigned)s->trigger);
            JB_CLAMP(o);
        }
        snprintf(jb + o, sizeof(jb) - o, "]}");
        http_json(pcb, 200, jb);
        return;
    }
#endif

#if ENABLE_KEYBOARD && KB_FEATURE_DYNAMIC_KEYMAP
    // ── Runtime keymap ────────────────────────────────────────────────────────
    //   GET  /api/keymap/info   dimensions + whether flash holds a keymap
    //   GET  /api/keymap/layout physical key geometry, if the board defines any
    //   GET  /api/keymap/<n>    one layer's keycodes
    //   POST /api/keymap        {"layer":n,"keys":[...]}  — applies immediately
    //   POST /api/keymap/save   persist to flash (queued; core 0 does the write)
    //   POST /api/keymap/reset  {"erase":bool} back to the compiled keymap
    //
    // A layer at a time, not the whole keymap: HTTP_RECV_BUF is 4096 and one
    // layer of a full-size board is a few hundred bytes, so this stays well
    // inside the buffer on any matrix worth building.

    // Response buffer sized for one layer: 6 chars per keycode is enough for
    // "65535," and the rest is the fixed envelope.
    #define KM_JSON_MAX (MATRIX_ROWS * MATRIX_COLS * 7 + 160)

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/keymap/info") == 0) {
        static char jb[896];
        int o = snprintf(jb, sizeof(jb),
            "{\"ok\":true,\"board\":\"%s\",\"rows\":%d,\"cols\":%d,"
            "\"layers\":%u,\"compiled_layers\":%u,\"encoders\":%u,"
            "\"stored\":%s,\"dirty\":%s,\"saving\":%s",
            KB_BOARD_NAME, MATRIX_ROWS, MATRIX_COLS,
            kb_keymap_layers(), keymap_layer_count, kb_encoder_count(),
            kb_keymap_stored() ? "true" : "false",
            kb_keymap_dirty()  ? "true" : "false",
            kb_keymap_save_pending() ? "true" : "false");
        JB_CLAMP(o);

        // Which keycode families this firmware will actually act on. Without
        // this the editor can only guess: a feature compiled out still has its
        // keycodes in the picker, and setting one produces a key that is stored,
        // displayed, and completely inert — the failure mode that made autoclick
        // look absent rather than disabled.
        //
        // `autoclick` is a COUNT, not a flag, because the slot number in
        // AUTOCLK(n) has to be in range too; a board with 3 slots must not offer
        // slot 7.
        o += snprintf(jb + o, sizeof(jb) - o,
            ",\"features\":{\"consumer\":%s,\"mousekeys\":%s,\"macros\":%s,"
            "\"layers\":%s,\"oneshot\":%s,\"caps_word\":%s,\"autoclick\":%u}",
            KB_FEATURE_CONSUMER  ? "true" : "false",
            KB_FEATURE_MOUSEKEYS ? "true" : "false",
            KB_FEATURE_MACROS    ? "true" : "false",
            KB_FEATURE_LAYERS    ? "true" : "false",
            KB_FEATURE_ONESHOT   ? "true" : "false",
            KB_FEATURE_CAPS_WORD ? "true" : "false",
            (unsigned)(KB_FEATURE_AUTOCLICK ? NUM_AUTOCLICKS : 0));
        JB_CLAMP(o);

        // The layout the HOST is set to. The picker shows what each key PRINTS
        // and lets you search for it, and "the key that types @" is a different
        // key on US and UK — so the editor has to be told, exactly as the typer
        // is. Both read the same setting; see include/hid_layout.h.
        o += snprintf(jb + o, sizeof(jb) - o, ",\"layout\":%u,\"layout_name\":\"%s\"",
                      (unsigned)hid_layout_active(),
                      HID_LAYOUT_NAMES[hid_layout_active()]);
        JB_CLAMP(o);

#if SPLIT_ENABLE
        // The module table, so the editor can say which rows belong to which
        // physical board. Without it a modular keyboard is one undifferentiated
        // grid and you have to remember that rows 4-7 are the right hand.
        //
        // `online` is included because the most useful thing this view can tell
        // you is that the module you are editing is not currently answering.
        o += snprintf(jb + o, sizeof(jb) - o, ",\"modules\":[");
        JB_CLAMP(o);
        for (uint8_t i = 0; i < split_module_count; i++) {
            const split_module_t *m = &SPLIT_MODULE_TABLE_PTR[i];
            if ((size_t)o > sizeof(jb) - 96) break;
            o += snprintf(jb + o, sizeof(jb) - o,
                          "%s{\"id\":%u,\"rows\":%u,\"cols\":%u,\"row_offset\":%u,"
                          "\"encoders\":%u,\"primary\":%s,\"online\":%s}",
                          i ? "," : "", m->id, m->rows, m->cols, m->row_offset,
                          m->encoders, i == 0 ? "true" : "false",
                          split_module_online(m->id) ? "true" : "false");
            JB_CLAMP(o);
        }
        o += snprintf(jb + o, sizeof(jb) - o, "]");
        JB_CLAMP(o);
#endif
        snprintf(jb + o, sizeof(jb) - o, "}");
        http_json(pcb, 200, jb);
        return;
    }



#if ENABLE_AP_MODE
    // ── WiFi provisioning ─────────────────────────────────────────────────────
    //   GET  /api/wifi         known networks + whether we are in AP mode
    //   POST /api/wifi         {"ssid":"..","password":"..","auth":0}
    //   POST /api/wifi/forget  {"ssid":".."}
    //   POST /api/wifi/save    persist to flash (queued)
    //   POST /api/wifi/apply   save, then reboot into station mode
    //
    // Passwords are NEVER returned. The UI shows which networks are known and
    // lets you replace one, but cannot read a secret back out of the device —
    // an authenticated session should not be a credential dump.


#if ENABLE_SETTINGS
    //   GET  /api/settings         every field with value, default, range, help
    //   POST /api/settings         {"field":"name","value":n}
    //   POST /api/settings/reset   {"field":"name"} or {"all":true}
    //   POST /api/settings/save    persist to flash (queued)
    //
    // The response is generated from the table in settings.cpp, so the page
    // renders whatever the firmware actually has rather than a hand-maintained
    // copy that drifts.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/settings") == 0) {
        static char jb[2600];
        settings_to_json(jb, sizeof(jb));
        http_json(pcb, 200, jb);
        return;
    }
#endif

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wifi/scan") == 0) {
        // Cached, not live: by the time anyone asks, the chip is an access point
        // and cannot survey the air. Captured in main() before the AP came up.
        static char jb[1200];
        int o = snprintf(jb, sizeof(jb), "{\"ok\":true,\"cached\":true,\"networks\":[");
        for (uint8_t i = 0; i < wifi_scan_cache_count(); i++) {
            const wifi_scan_entry_t *e = wifi_scan_cache_get(i);
            if (!e || (size_t)o > sizeof(jb) - 80) break;
            o += snprintf(jb + o, sizeof(jb) - o,
                          "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                          i ? "," : "", e->ssid, e->rssi,
                          e->secure ? "true" : "false");
        }
        snprintf(jb + o, sizeof(jb) - o, "]}");
        http_json(pcb, 200, jb);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wifi") == 0) {
        static char jb[1024];
        int o = snprintf(jb, sizeof(jb),
                         "{\"ok\":true,\"ap_mode\":%s,\"dirty\":%s,\"stored\":%u,"
                         "\"max\":%d,\"networks\":[",
                         ap_mode_active() ? "true" : "false",
                         wifi_store_dirty() ? "true" : "false",
                         wifi_store_count(), WIFI_STORE_MAX);
        for (uint8_t i = 0; i < wifi_store_count(); i++) {
            const wifi_cred_t *c = wifi_store_get(i);
            if (!c || (size_t)o > sizeof(jb) - 80) break;
            o += snprintf(jb + o, sizeof(jb) - o,
                          "%s{\"ssid\":\"%s\",\"auth\":%u,\"source\":\"stored\"}",
                          i ? "," : "", c->ssid, c->auth_mode);
        }
        for (size_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
            if ((size_t)o > sizeof(jb) - 80) break;
            o += snprintf(jb + o, sizeof(jb) - o,
                          "%s{\"ssid\":\"%s\",\"auth\":%d,\"source\":\"compiled\"}",
                          (i || wifi_store_count()) ? "," : "",
                          WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].auth_mode);
        }
        snprintf(jb + o, sizeof(jb) - o, "]}");
        http_json(pcb, 200, jb);
        return;
    }
#endif

#if ENABLE_KEYBOARD && KB_FEATURE_MACRO_STORE
    // ── Macro bodies ──────────────────────────────────────────────────────────
    //   GET  /api/macro          all macros as step lists
    //   POST /api/macro          {"id":n,"steps":[...]}  — live immediately
    //   POST /api/macro/save     persist to flash (queued)
    //   POST /api/macro/clear    delete every macro
    //
    // Steps use the same vocabulary as the custom-button tab:
    //   {"t":"tap","mod":2,"key":4} {"t":"down",...} {"t":"up",...}
    //   {"t":"delay","ms":250}      {"t":"text","value":"hi"}
    // The firmware assembles them into bytecode and verifies before storing, so
    // the interpreter on core 1 never has to bounds-check in its hot path.

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/macro") == 0) {
        static char jb[3200];
        int o = snprintf(jb, sizeof(jb),
                         "{\"ok\":true,\"count\":%d,\"used\":%u,\"size\":%u,"
                         "\"dirty\":%s,\"stored\":%s,\"macros\":[",
                         KB_MACRO_COUNT, kb_macro_pool_used(), kb_macro_pool_size(),
                         kb_macro_dirty() ? "true" : "false",
                         kb_macro_stored() ? "true" : "false");
        bool first = true;
        for (uint8_t i = 0; i < KB_MACRO_COUNT; i++) {
            uint16_t len = 0;
            const uint8_t *b = kb_macro_body(i, &len);
            if (!b) continue;
            if ((size_t)o > sizeof(jb) - 96) break;
            o += snprintf(jb + o, sizeof(jb) - o, "%s{\"id\":%u,\"steps\":[",
                          first ? "" : ",", i);
            first = false;
            bool fs = true;
            for (uint16_t k = 0; k < len && b[k] != MOP_END; ) {
                if ((size_t)o > sizeof(jb) - 96) break;
                const char *sep = fs ? "" : ","; fs = false;
                switch (b[k]) {
                case MOP_TAP: case MOP_DOWN: case MOP_UP:
                    o += snprintf(jb + o, sizeof(jb) - o,
                                  "%s{\"t\":\"%s\",\"mod\":%u,\"key\":%u}", sep,
                                  b[k] == MOP_TAP ? "tap" : b[k] == MOP_DOWN ? "down" : "up",
                                  b[k+1], b[k+2]);
                    k += 3;
                    break;
                case MOP_DELAY:
                    o += snprintf(jb + o, sizeof(jb) - o, "%s{\"t\":\"delay\",\"ms\":%u}",
                                  sep, (unsigned)(b[k+1] | (b[k+2] << 8)));
                    k += 3;
                    break;
                case MOP_TEXT: {
                    uint8_t n = b[k+1];
                    o += snprintf(jb + o, sizeof(jb) - o, "%s{\"t\":\"text\",\"value\":\"", sep);
                    for (uint8_t c = 0; c < n && (size_t)o < sizeof(jb) - 8; c++) {
                        char ch = (char)b[k+2+c];
                        if (ch == '"' || ch == '\\') jb[o++] = '\\';
                        if (ch == '\n') { jb[o++] = '\\'; ch = 'n'; }
                        jb[o++] = ch;
                    }
                    o += snprintf(jb + o, sizeof(jb) - o, "\"}");
                    k += 2 + n;
                    break;
                }
                default: k = len; break;
                }
            }
            o += snprintf(jb + o, sizeof(jb) - o, "]}");
        }
        snprintf(jb + o, sizeof(jb) - o, "]}");
        http_json(pcb, 200, jb);
        return;
    }
#endif

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/keymap/layout") == 0) {
        // Trailing fields are omitted when they hold their defaults, so an
        // ordinary 1u unrotated key costs "[r,c,x,y]," and a 100-key board fits
        // comfortably inside one TCP write (TCP_SND_BUF is 8*MSS).
        static char jb[MATRIX_ROWS * MATRIX_COLS * 26 + 128];
        int o = snprintf(jb, sizeof(jb),
                         "{\"ok\":true,\"unit\":%d,\"keys\":[", KB_LAYOUT_UNIT);
        for (uint16_t i = 0; i < kb_layout_count; i++) {
            const kb_layout_key_t *k = &kb_layout[i];
            if ((size_t)o > sizeof(jb) - 40) break;   // never overrun; truncate cleanly
            o += snprintf(jb + o, sizeof(jb) - o, "%s[%u,%u,%u,%u",
                          i ? "," : "", k->row, k->col, k->x, k->y);
            if (k->rot)
                o += snprintf(jb + o, sizeof(jb) - o, ",%u,%u,%d,%u,%u",
                              k->w, k->h, k->rot, k->rx, k->ry);
            else if (k->w != KB_LAYOUT_UNIT || k->h != KB_LAYOUT_UNIT)
                o += snprintf(jb + o, sizeof(jb) - o, ",%u,%u", k->w, k->h);
            o += snprintf(jb + o, sizeof(jb) - o, "]");
        }
        snprintf(jb + o, sizeof(jb) - o, "]}");
        http_json(pcb, 200, jb);
        return;
    }

    //   GET  /api/keymap/encoders/<n>  one layer's encoder actions
    if (strcmp(method, "GET") == 0 &&
        strncmp(path, "/api/keymap/encoders/", 21) == 0) {
        int layer = atoi(path + 21);
        if (layer < 0 || layer >= (int)kb_keymap_layers()) {
            http_json(pcb, 404, "{\"error\":\"bad_layer\"}");
            return;
        }
        static char jb[512];
        int o = snprintf(jb, sizeof(jb),
                         "{\"ok\":true,\"layer\":%d,\"encoders\":[", layer);
        for (uint8_t e = 0; e < kb_encoder_count(); e++) {
            if ((size_t)o > sizeof(jb) - 40) break;
            /* [CCW, CW, press] — the same order as encoder_map[][][3], so the
             * page, the store and the keymap all index it identically. */
            o += snprintf(jb + o, sizeof(jb) - o, "%s[%u,%u,%u]", e ? "," : "",
                          kb_encoder_at((uint8_t)layer, e, 0),
                          kb_encoder_at((uint8_t)layer, e, 1),
                          kb_encoder_at((uint8_t)layer, e, 2));
        }
        snprintf(jb + o, sizeof(jb) - o, "]}");
        http_json(pcb, 200, jb);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/keymap/", 12) == 0) {
        int layer = atoi(path + 12);
        if (layer < 0 || layer >= (int)kb_keymap_layers()) {
            http_json(pcb, 404, "{\"error\":\"bad_layer\"}");
            return;
        }
        static char jb[KM_JSON_MAX];
        int o = snprintf(jb, sizeof(jb), "{\"ok\":true,\"layer\":%d,\"keys\":[", layer);
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                o += snprintf(jb + o, sizeof(jb) - o, "%s%u",
                              (r || c) ? "," : "",
                              kb_keymap_at((uint8_t)layer, (uint8_t)r, (uint8_t)c));
        snprintf(jb + o, sizeof(jb) - o, "]}");
        http_json(pcb, 200, jb);
        return;
    }
#endif // ENABLE_KEYBOARD && KB_FEATURE_DYNAMIC_KEYMAP

#if ENABLE_REMOTES
    // Diagnostic: ISR-independent probe of the IR RX pin. Hit this while pressing
    // a remote at the receiver. {idle,transitions,low_seen}.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ir/raw") == 0) {
        int tr = 0, low = 0;
        int idle = remotes_ir_raw_sample(250, &tr, &low);
        char b[96];
        snprintf(b, sizeof(b), "{\"idle\":%d,\"transitions\":%d,\"low_seen\":%s}",
                 idle, tr, low ? "true" : "false");
        http_json(pcb, 200, b);
        return;
    }

    // Poll a "learn" capture. {ready:false,armed:bool} until a frame arrives,
    // then {ready:true,count,proto,[code],[carrier],timings:[...]}.
    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/api/ir/captured") == 0 || strcmp(path, "/api/rf/captured") == 0)) {
        rx_kind_t kind = (path[5] == 'i') ? RX_IR : RX_RF;   // "/api/ir.." vs "/api/rf.."
        static uint16_t t[256];
        int n = 0;
        if (!remotes_rx_get(kind, t, 256, &n)) {
            char b[64];
            snprintf(b, sizeof(b), "{\"ready\":false,\"armed\":%s,\"edges\":%d}",
                     remotes_rx_armed(kind) ? "true" : "false", remotes_rx_edges(kind));
            http_json(pcb, 200, b);
            return;
        }
        static char body[2200];
        int o = snprintf(body, sizeof(body), "{\"ready\":true,\"count\":%d", n);
        if (kind == RX_IR) {
            uint32_t code;
            if (remotes_decode_nec(t, n, &code))
                o += snprintf(body+o, sizeof(body)-o,
                              ",\"proto\":\"nec\",\"code\":%u,\"carrier\":38000", (unsigned)code);
            else
                o += snprintf(body+o, sizeof(body)-o, ",\"proto\":\"raw\",\"carrier\":38000");
        } else {
            o += snprintf(body+o, sizeof(body)-o, ",\"proto\":\"raw\"");
        }
        o += snprintf(body+o, sizeof(body)-o, ",\"timings\":[");
        for (int i = 0; i < n && o < (int)sizeof(body) - 12; i++)
            o += snprintf(body+o, sizeof(body)-o, "%s%u", i ? "," : "", (unsigned)t[i]);
        o += snprintf(body+o, sizeof(body)-o, "]}");
        http_json(pcb, 200, body);
        return;
    }
#endif // ENABLE_REMOTES

    if (strcmp(method, "POST") != 0) {
        http_json(pcb, 404, "{\"error\":\"not_found\"}");
        return;
    }


#if ENABLE_PASSWORD_STORE
    if (strcmp(path, "/api/password") == 0 ||
        strcmp(path, "/api/password/reset") == 0) {

        if (!PW_CHANGE_ALLOWED) {
            http_json(pcb, 403, "{\"error\":\"plaintext_forbidden\",\"detail\":"
                                "\"ALLOW_PLAINTEXT_AUTH=0 and this is not HTTPS\"}");
            return;
        }

        const bool reset = (strcmp(path, "/api/password/reset") == 0);
        int uidx = auth_token_user_index(sid);
        if (uidx < 0) uidx = 0;

        // Buffers, not pointers into `body`: the request buffer is reused and
        // the password has to outlive the parse.
        char cur[AUTH_PW_MAX + 2] = {}, neu[AUTH_PW_MAX + 2] = {};
        web_json_str(body, "current", cur, sizeof(cur));
        if (!reset) web_json_str(body, "new", neu, sizeof(neu));

        // An authenticated session is not enough. A password change is the one
        // action that can lock the owner out, so it re-proves knowledge of the
        // current password — which also means a walked-up-to unlocked browser
        // cannot take the device away from you.
        //
        // Through auth_authenticate() rather than a bare strcmp, so wrong
        // guesses here count towards the same lockout as wrong guesses at the
        // login page. Otherwise this endpoint is an unthrottled oracle for the
        // password the login page carefully refuses to let you brute-force.
        //
        // AUTH_DISABLED means no user has a password at all, so there is no
        // current one to prove and nothing to protect — that is the case where
        // this endpoint turns auth ON, which it must not refuse to do.
        uint32_t retry = 0;
        auth_result_t ar = auth_authenticate(auth_user_name(uidx), cur, &retry);
        if (ar == AUTH_LOCKED) {
            char jb[96];
            snprintf(jb, sizeof(jb),
                     "{\"error\":\"locked\",\"retry_after_s\":%u}", (unsigned)retry);
            http_json(pcb, 429, jb);
            return;
        }
        if (ar != AUTH_OK && ar != AUTH_DISABLED) {
            http_json(pcb, 403, "{\"error\":\"wrong_password\",\"detail\":"
                                "\"the current password is required to change it\"}");
            return;
        }

        if (reset) {
            // Checked, not assumed: reporting ok:true on a store that refused
            // the write would tell someone their password had been reverted
            // when it had not.
            if (!auth_store_set(uidx, NULL)) {
                http_json(pcb, 400, "{\"error\":\"rejected\"}");
                return;
            }
            auth_invalidate_user_tokens(uidx, sid);
            auth_touch();
            http_json(pcb, 200, "{\"ok\":true,\"reset\":true}");
            return;
        }

        // The lower bound is also what stops an empty password arriving here: ""
        // disables auth device-wide, and reaching that state from a web form —
        // over the network, possibly by a slip — is not something to allow. The
        // deliberate way to run without a password is PASSWORD "" in env.h.
        size_t n = strlen(neu);
        if (n < WEB_PASSWORD_MIN_LEN || n > AUTH_PW_MAX) {
            char jb[128];
            snprintf(jb, sizeof(jb),
                     "{\"error\":\"bad_length\",\"min_len\":%u,\"max_len\":%u}",
                     (unsigned)WEB_PASSWORD_MIN_LEN, (unsigned)AUTH_PW_MAX);
            http_json(pcb, 400, jb);
            return;
        }
        if (strcmp(neu, cur) == 0) {
            http_json(pcb, 400, "{\"error\":\"unchanged\"}");
            return;
        }
        if (!auth_store_set(uidx, neu)) {
            http_json(pcb, 400, "{\"error\":\"rejected\"}");
            return;
        }

        // Every other session for this user was opened with the old password.
        // Keep this one: signing out the person who just changed it, on a device
        // whose recovery path is a reflash, is exactly the wrong reflex.
        int dropped = auth_invalidate_user_tokens(uidx, sid);
        auth_touch();

        char jb[128];
        snprintf(jb, sizeof(jb),
                 "{\"ok\":true,\"queued\":true,\"sessions_ended\":%d}", dropped);
        http_json(pcb, 200, jb);
        return;
    }
#endif

#if ENABLE_KEYBOARD && KB_FEATURE_AUTOCLICK
    if (strcmp(path, "/api/autoclick") == 0) {
        int slot    = web_json_int(body, "slot", -1);
        int target  = web_json_int(body, "target", -1);
        int ms      = web_json_int(body, "interval_ms", -1);
        int trigger = web_json_int(body, "trigger", -1);

        if (slot < 0 || slot >= (int)autoclick_count() ||
            target < 0 || target > 0xFFFF || ms < 0 || trigger < 0) {
            http_json(pcb, 400, "{\"error\":\"bad params\"}");
            return;
        }
        // Say WHICH constraint failed. "bad params" on a rate of 4 ms sends you
        // looking at the keycode, and the floor is not a number anyone guesses.
        if (!autoclick_trigger_valid((uint8_t)trigger)) {
            http_json(pcb, 400, "{\"error\":\"bad trigger\",\"detail\":"
                                "\"trigger must be a non-empty mix of 1=hold, "
                                "2=double-tap, 4=triple-tap, and not both tap counts\"}");
            return;
        }
        if (ms < AC_MIN_INTERVAL_MS || ms > AC_MAX_INTERVAL_MS) {
            char jb[128];
            snprintf(jb, sizeof(jb),
                     "{\"error\":\"bad interval\",\"min_ms\":%u,\"max_ms\":%u}",
                     (unsigned)AC_MIN_INTERVAL_MS, (unsigned)AC_MAX_INTERVAL_MS);
            http_json(pcb, 400, jb);
            return;
        }
        if (!autoclick_set_slot((uint8_t)slot, (kb_keycode_t)target,
                                (uint16_t)ms, (uint8_t)trigger)) {
            http_json(pcb, 400, "{\"error\":\"rejected\",\"detail\":"
                                "\"target cannot be KC_NO\"}");
            return;
        }
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");
        return;
    }

    if (strcmp(path, "/api/autoclick/save") == 0) {
        autoclick_save();
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"queued\":true}");
        return;
    }

    if (strcmp(path, "/api/autoclick/reset") == 0) {
        autoclick_reset(web_json_int(body, "erase", 0) != 0);
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");
        return;
    }
#endif

#if ENABLE_KEYBOARD && KB_FEATURE_DYNAMIC_KEYMAP



#if ENABLE_SETTINGS
    if (strcmp(path, "/api/settings") == 0) {
        char field[32] = {};
        web_json_str(body, "field", field, sizeof(field));
        long value = web_json_int(body, "value", 0);
        if (!field[0]) { http_json(pcb, 400, "{\"error\":\"no_field\"}"); return; }
        if (!settings_set(field, value)) {
            // Out of range is rejected rather than clamped: a silently corrected
            // value looks like it worked and behaves like something else.
            http_json(pcb, 400, "{\"error\":\"unknown_field_or_out_of_range\"}");
            return;
        }
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");
        return;
    }

    if (strcmp(path, "/api/settings/reset") == 0) {
        if (web_json_int(body, "all", 0)) {
            settings_reset_all();
        } else {
            char field[32] = {};
            web_json_str(body, "field", field, sizeof(field));
            if (!settings_reset(field)) {
                http_json(pcb, 400, "{\"error\":\"unknown_field\"}");
                return;
            }
        }
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");
        return;
    }

    if (strcmp(path, "/api/settings/save") == 0) {
        settings_save_request();
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"queued\":true}");
        return;
    }
#endif

#if ENABLE_AP_MODE
    if (strcmp(path, "/api/wifi") == 0) {
        char ssid[WIFI_SSID_MAX + 1] = {0};
        char pass[WIFI_PASS_MAX + 1] = {0};
        web_json_str(body, "ssid", ssid, sizeof(ssid));
        if (!ssid[0]) {
            http_json(pcb, 400, "{\"error\":\"no_ssid\"}");
            return;
        }
        web_json_str(body, "password", pass, sizeof(pass));
        int auth = web_json_int(body, "auth", 0);
        if (auth < 0 || auth > 4) auth = 0;

        // An open network is the only case where an empty password is valid.
        if (auth != 4 && strlen(pass) < 8) {
            http_json(pcb, 400, "{\"error\":\"password_too_short\"}");
            return;
        }
        if (!wifi_store_set(ssid, pass, (uint8_t)auth)) {
            http_json(pcb, 400, "{\"error\":\"store_full\"}");
            return;
        }
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");
        return;
    }

    if (strcmp(path, "/api/wifi/forget") == 0) {
        char ssid[WIFI_SSID_MAX + 1] = {0};
        web_json_str(body, "ssid", ssid, sizeof(ssid));
        bool gone = wifi_store_remove(ssid);
        auth_touch();
        http_json(pcb, gone ? 200 : 404,
                  gone ? "{\"ok\":true}" : "{\"error\":\"not_stored\"}");
        return;
    }

    if (strcmp(path, "/api/wifi/save") == 0) {
        wifi_store_save_request();
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"queued\":true}");
        return;
    }

    if (strcmp(path, "/api/wifi/apply") == 0) {
        // Save, answer, THEN reboot. Rebooting inside the handler leaves the
        // browser on a dead socket with no idea whether it worked; the delay is
        // long enough for this response to actually land.
        wifi_store_save_request();
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"rebooting\":true}");
        ap_mode_request_reboot(1500);
        return;
    }
#endif

#if ENABLE_KEYBOARD && KB_FEATURE_MACRO_STORE
    if (strcmp(path, "/api/macro") == 0) {
        int id = web_json_int(body, "id", -1);
        if (id < 0 || id >= KB_MACRO_COUNT) {
            http_json(pcb, 400, "{\"error\":\"bad_id\"}");
            return;
        }

        // Assemble the step list into bytecode. Scanning for "t" markers rather
        // than parsing JSON properly is the same trade the rest of this file
        // makes: a full parser is not worth its flash here, and kb_macro_set()
        // verifies the result before anything is stored, so a malformed body is
        // rejected rather than executed.
        static uint8_t prog[KB_MACRO_POOL];
        uint16_t n = 0;
        const char *p = strstr(body, "\"steps\"");
        bool bad = false;

        if (p) {
            const char *q = p;
            while ((q = strstr(q, "\"t\"")) != NULL && !bad) {
                const char *v = strchr(q, ':');
                if (!v) break;
                v++;
                while (*v == ' ' || *v == '"') v++;
                const char *scope = strchr(q, '}');
                if (!scope) break;

                uint8_t op = 0;
                if      (!strncmp(v, "tap",   3)) op = MOP_TAP;
                else if (!strncmp(v, "down",  4)) op = MOP_DOWN;
                else if (!strncmp(v, "up",    2)) op = MOP_UP;
                else if (!strncmp(v, "delay", 5)) op = MOP_DELAY;
                else if (!strncmp(v, "text",  4)) op = MOP_TEXT;
                else { bad = true; break; }

                if (op == MOP_TEXT) {
                    const char *tv = strstr(q, "\"value\"");
                    if (!tv || tv > scope) { bad = true; break; }
                    tv = strchr(tv, ':');
                    if (!tv) { bad = true; break; }
                    tv = strchr(tv, '"');
                    if (!tv) { bad = true; break; }
                    tv++;
                    uint8_t len = 0;
                    static char txt[KB_MACRO_TEXT_MAX + 1];
                    while (*tv && *tv != '"' && len < KB_MACRO_TEXT_MAX) {
                        char c = *tv++;
                        if (c == '\\' && *tv) {          // \n and \" survive the trip
                            char e = *tv++;
                            c = (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
                        }
                        txt[len++] = c;
                    }
                    if (!len || n + 2 + len + 1 > (int)sizeof(prog)) { bad = true; break; }
                    prog[n++] = MOP_TEXT;
                    prog[n++] = len;
                    memcpy(prog + n, txt, len);
                    n = (uint16_t)(n + len);
                } else if (op == MOP_DELAY) {
                    int ms = web_json_int(q, "ms", 0);
                    if (ms < 0) ms = 0;
                    if (ms > 65535) ms = 65535;
                    if (n + 3 + 1 > (int)sizeof(prog)) { bad = true; break; }
                    prog[n++] = MOP_DELAY;
                    prog[n++] = (uint8_t)(ms & 0xFF);
                    prog[n++] = (uint8_t)(ms >> 8);
                } else {
                    int mod = web_json_int(q, "mod", 0);
                    int key = web_json_int(q, "key", 0);
                    if (n + 3 + 1 > (int)sizeof(prog)) { bad = true; break; }
                    prog[n++] = op;
                    prog[n++] = (uint8_t)(mod & 0xFF);
                    prog[n++] = (uint8_t)(key & 0xFF);
                }
                q = scope;
            }
        }

        if (bad) { http_json(pcb, 400, "{\"error\":\"bad_steps\"}"); return; }

        if (n == 0) {
            kb_macro_set((uint8_t)id, NULL, 0);          // empty list = delete
        } else {
            prog[n++] = MOP_END;
            if (!kb_macro_set((uint8_t)id, prog, n)) {
                http_json(pcb, 400, "{\"error\":\"rejected_or_full\"}");
                return;
            }
        }
        auth_touch();
        static char jb[96];
        snprintf(jb, sizeof(jb), "{\"ok\":true,\"bytes\":%u,\"used\":%u,\"size\":%u}",
                 n, kb_macro_pool_used(), kb_macro_pool_size());
        http_json(pcb, 200, jb);
        return;
    }

    if (strcmp(path, "/api/macro/save") == 0) {
        kb_macro_save_request();
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"queued\":true}");
        return;
    }

    if (strcmp(path, "/api/macro/clear") == 0) {
        kb_macro_clear_all();
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");
        return;
    }
#endif

    if (strcmp(path, "/api/keymap/encoders") == 0) {
        int layer = web_json_int(body, "layer", -1);
        int index = web_json_int(body, "index", -1);
        int action = web_json_int(body, "action", -1);
        int kc = web_json_int(body, "kc", -1);
        if (layer < 0 || layer >= (int)kb_keymap_layers() ||
            index < 0 || index >= (int)kb_encoder_count() ||
            action < 0 || action > 2 || kc < 0 || kc > 0xFFFF) {
            http_json(pcb, 400, "{\"error\":\"bad_encoder_request\"}");
            return;
        }
        /* One action at a time, unlike the keymap which is sent a whole layer:
         * there are three of these per encoder, so a partial write has nothing
         * to be inconsistent with. */
        if (!kb_encoder_set((uint8_t)layer, (uint8_t)index,
                            (uint8_t)action, (uint16_t)kc)) {
            http_json(pcb, 400, "{\"error\":\"rejected\"}");
            return;
        }
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"live\":true}");
        return;
    }

    if (strcmp(path, "/api/keymap") == 0) {
        int layer = web_json_int(body, "layer", -1);
        if (layer < 0 || layer >= (int)kb_keymap_layers()) {
            http_json(pcb, 400, "{\"error\":\"bad_layer\"}");
            return;
        }
        // Parse the keycode array inline rather than reusing web_json_u16_arr,
        // which lives inside the ENABLE_REMOTES block.
        static uint16_t keys[MATRIX_ROWS * MATRIX_COLS];
        int n = 0;
        const char *p = strstr(body, "\"keys\"");
        if (p) p = strchr(p, '[');
        if (p) {
            p++;
            while (n < MATRIX_ROWS * MATRIX_COLS) {
                while (*p == ' ' || *p == ',') p++;
                if (*p == ']' || !*p) break;
                if (*p < '0' || *p > '9') break;
                long v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); if (v > 65535) v = 65535; }
                keys[n++] = (uint16_t)v;
            }
        }
        if (n != MATRIX_ROWS * MATRIX_COLS) {
            // Refuse a partial layer outright. Applying what arrived would leave
            // the tail of the layer holding whatever it held before, which is a
            // far more confusing failure than a rejected request.
            static char jb[96];
            snprintf(jb, sizeof(jb), "{\"error\":\"expected_%d_keys\",\"got\":%d}",
                     MATRIX_ROWS * MATRIX_COLS, n);
            http_json(pcb, 400, jb);
            return;
        }
        int i = 0;
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                kb_keymap_set((uint8_t)layer, (uint8_t)r, (uint8_t)c, keys[i++]);
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"live\":true}");
        return;
    }

    if (strcmp(path, "/api/keymap/save") == 0) {
        // Queue it. The erase parks core 1 and disables interrupts for ~50-100 ms,
        // which must not happen inside this lwIP callback — core 0's main loop
        // picks it up on the next pass. Answer now so the client is not waiting
        // on a connection that is about to stall.
        kb_keymap_save_request();
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true,\"queued\":true}");
        return;
    }

    if (strcmp(path, "/api/keymap/reset") == 0) {
        bool erase = web_json_int(body, "erase", 0) != 0;
        kb_keymap_reset(erase);
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");
        return;
    }
#endif // ENABLE_KEYBOARD && KB_FEATURE_DYNAMIC_KEYMAP

    if (strcmp(path, "/api/combo") == 0 || strcmp(path, "/api/key") == 0) {
        // Tap a key report: press AND release. /api/combo is the canonical name
        // (matching the socket, where combo = tap); /api/key is kept as an alias
        // for older clients. There is no HTTP "hold"; use the socket key/key_release.
        int modifier=web_json_int(body,"modifier",0), keys[6]={};
        web_json_int_arr(body,"keys",keys,6);
        hid_keyboard_report_t rep={};
        rep.modifier=(uint8_t)modifier;
        for(int i=0;i<6;i++) rep.keycode[i]=(uint8_t)keys[i];
        hid_push_key_report(&rep, true);
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");

    } else if (strcmp(path, "/api/mouse") == 0) {
        int buttons=web_json_int(body,"buttons",0);
        int x=web_json_int(body,"x",0), y=web_json_int(body,"y",0);
        int wheel=web_json_int(body,"wheel",0);
        hid_mouse_report_t rep={
            .buttons=(uint8_t)buttons,
            .x=(int8_t)(x>127?127:x<-127?-127:x),
            .y=(int8_t)(y>127?127:y<-127?-127:y),
            .wheel=(int8_t)(wheel>127?127:wheel<-127?-127:wheel),.pan=0
        };
        hid_push_mouse_report(&rep);
        if (buttons) { hid_mouse_report_t rel={}; hid_push_mouse_report(&rel); }
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");

    } else if (strcmp(path, "/api/mouse_abs") == 0) {
        int buttons=web_json_int(body,"buttons",0);
        int x=web_json_int(body,"x",0), y=web_json_int(body,"y",0);
        int wheel=web_json_int(body,"wheel",0);
        if (x < 0) x = 0;
        if (x > HID_ABS_MAX) x = HID_ABS_MAX;
        if (y < 0) y = 0;
        if (y > HID_ABS_MAX) y = HID_ABS_MAX;
        hid_push_abs_pointer((uint16_t)x, (uint16_t)y, (uint8_t)buttons,
                             (int8_t)(wheel > 127 ? 127 : wheel < -127 ? -127 : wheel));
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");

    } else if (strcmp(path, "/api/text") == 0) {
        char text[256]={};
        size_t tlen = web_json_str(body,"text",text,sizeof(text));
        int delay   = web_json_int(body,"delay_ms",TYPE_DELAY_MS);
        if (!tlen) { http_json(pcb, 400, "{\"error\":\"empty_text\"}"); return; }
        char expanded[256];
        const char *who = auth_user_name(auth_token_user_index(sid));
        int elen = secret_expand(who, text, (int)tlen, expanded, sizeof(expanded));
        hid_push_type_string(expanded,(uint8_t)elen,(uint8_t)delay);
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");

    } else if (strcmp(path, "/api/wake") == 0) {
        hid_push_wakeup();
        http_json(pcb, 200, "{\"ok\":true}");

#if ENABLE_REMOTES
    } else if (strcmp(path, "/api/ir") == 0) {
        // IR blaster. Two forms:
        //   {"proto":"nec","code":<u32>}                     -> NEC convenience
        //   {"proto":"raw","carrier":38000,"timings":[...]}  -> raw mark/space us
        char proto[16]={}; web_json_str(body,"proto",proto,sizeof(proto));
        if (strcmp(proto,"nec")==0) {
            // code may exceed int range; parse as unsigned 32-bit.
            uint32_t code = web_json_u32(body,"code",0);
            ir_send_nec(code);
            auth_touch();
            http_json(pcb, 200, "{\"ok\":true}");
        } else {
            static uint16_t t[256];
            int n = web_json_u16_arr(body,"timings",t,256);
            int carrier = web_json_int(body,"carrier",38000);
            if (n <= 0) { http_json(pcb, 400, "{\"error\":\"no_timings\"}"); return; }
            ir_send_raw(t, n, carrier);
            auth_touch();
            http_json(pcb, 200, "{\"ok\":true}");
        }

    } else if (strcmp(path, "/api/rf") == 0) {
        // 433 MHz OOK. {"timings":[mark,gap,...] in us, "repeat":N}
        static uint16_t t[256];
        int n = web_json_u16_arr(body,"timings",t,256);
        int repeat = web_json_int(body,"repeat",6);
        if (n <= 0) { http_json(pcb, 400, "{\"error\":\"no_timings\"}"); return; }
        rf_send_raw(t, n, repeat);
        auth_touch();
        http_json(pcb, 200, "{\"ok\":true}");

    } else if (strcmp(path, "/api/ir/learn") == 0) {
        remotes_rx_arm(RX_IR);
        auth_touch();
        http_json(pcb, 200, "{\"armed\":true}");

    } else if (strcmp(path, "/api/rf/learn") == 0) {
        remotes_rx_arm(RX_RF);
        auth_touch();
        http_json(pcb, 200, "{\"armed\":true}");
#endif // ENABLE_REMOTES

    } else {
        http_json(pcb, 404, "{\"error\":\"not_found\"}");
    }
}

// ── lwIP callbacks ────────────────────────────────────────────────────────────

static err_t web_recv_cb(void *arg, net_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p || err!=ERR_OK) { http_close(pcb); if(p) pbuf_free(p); return ERR_OK; }

    http_conn_t *hc = http_get(pcb);
    if (!hc) { pbuf_free(p); return ERR_OK; }
    hc->idle = 0;

    for (struct pbuf *q=p; q; q=q->next) {
        uint16_t space=HTTP_RECV_BUF-hc->len-1, copy=q->len<space?q->len:space;
        memcpy(hc->buf+hc->len,q->payload,copy); hc->len+=copy;
    }
    hc->buf[hc->len]='\0';
    net_recved(pcb,p->tot_len); pbuf_free(p);

    // Guard: headers that fill the buffer without a terminator means a
    // malformed or oversized request. Reject rather than stalling.
    if (hc->len >= HTTP_RECV_BUF - 1 &&
        !strstr((char*)hc->buf, "\r\n\r\n")) {
        static const char r[] =
            "HTTP/1.0 431 Request Header Fields Too Large\r\n"
            "Connection: close\r\n\r\n";
        net_write(pcb, r, sizeof(r)-1, TCP_WRITE_FLAG_COPY);
        net_output(pcb);
        http_close(pcb);
        return ERR_OK;
    }

    // Wait until the full request has arrived.
    char *hdr_end = strstr((char*)hc->buf, "\r\n\r\n");
    if (!hdr_end) return ERR_OK;   // headers not complete yet

    // For requests with a body (POST), make sure the whole body is present
    // before dispatching — a body can span several TCP segments.
    const char *cl = strstr((char*)hc->buf, "Content-Length:");
    if (!cl) cl = strstr((char*)hc->buf, "content-length:");
    if (cl) {
        int content_len = atoi(cl + 15);
        size_t body_start = (size_t)(hdr_end + 4 - (char*)hc->buf);
        size_t body_have  = hc->len - body_start;
        if ((int)body_have < content_len) {
            // Body incomplete — wait for more segments (unless buffer is full)
            if (hc->len < HTTP_RECV_BUF - 1) return ERR_OK;
        }
    }

    handle_request(pcb, (char*)hc->buf, hc->len);
    return ERR_OK;
}

static void web_err_cb(void *arg, err_t err) {
    (void)err;
    net_pcb *pcb = (net_pcb*)arg;
    if (pcb) http_free(pcb);
}

// Poll fires every WEB_POLL_INTERVAL coarse ticks (~0.5 s each). A kept-alive
// connection that sits idle (no request or tx) for WEB_IDLE_MAX polls is reaped
// so its slot and TLS state are freed instead of lingering.
#define WEB_POLL_INTERVAL 8   // ~4 s per poll
#define WEB_IDLE_MAX      6   // ~24 s idle -> close (must exceed the client's
                              // ~12 s keep-alive ping so an active session's
                              // connection isn't reaped between pings)
static err_t web_poll_cb(void *arg, net_pcb *pcb) {
    (void)arg;
    http_conn_t *hc = http_get(pcb);
    if (!hc) { http_close(pcb); return ERR_OK; }
    if (++hc->idle >= WEB_IDLE_MAX) http_close(pcb);
    return ERR_OK;
}

static err_t web_accept_cb(void *arg, net_pcb *newpcb, err_t err) {
    (void)arg;
    if (err!=ERR_OK||!newpcb) return ERR_VAL;
    if (http_alloc(newpcb)<0) { net_abort(newpcb); return ERR_ABRT; }
    net_setprio(newpcb,TCP_PRIO_MIN);
    net_arg(newpcb,newpcb);
    net_recv(newpcb,web_recv_cb);
    net_err(newpcb,web_err_cb);
    net_poll(newpcb,web_poll_cb,WEB_POLL_INTERVAL);
    return ERR_OK;
}

// ── Public init ───────────────────────────────────────────────────────────────

#if ENABLE_HTTPS
#if defined(MBEDTLS_USE_PSA_CRYPTO) || defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#define NETHID_HAVE_PSA 1
#endif
#endif

void web_init(void) {
    memset(http_pcbs,0,sizeof(http_pcbs));
    memset(http_conns,0,sizeof(http_conns));
#if defined(NETHID_HAVE_PSA)
    // mbedTLS 3.x PSA crypto path must be initialised once before any handshake;
    // the lwIP altcp port doesn't do it for us.
    psa_crypto_init();
#endif
    net_pcb *lpcb = net_http_listen(WEB_LISTEN_PORT, web_accept_cb, MAX_HTTP_CONN);
    char m[96];
    if (!lpcb) {
        snprintf(m, sizeof(m), "[" NETHID_BUILD "] [web] listen FAILED: %s",
                 net_listen_fail ? net_listen_fail : "unknown");
        dbg(m);
        return;
    }
    snprintf(m, sizeof(m), "[" NETHID_BUILD "] [web] %s server on port %d (auth %s)",
             ENABLE_HTTPS ? "HTTPS" : "HTTP", WEB_LISTEN_PORT,
             auth_is_enabled() ? "enabled" : "DISABLED");
    dbg(m);
}

#if ENABLE_AP_MODE
void web_init_plain(uint16_t port) {
    memset(http_pcbs, 0, sizeof(http_pcbs));
    memset(http_conns, 0, sizeof(http_conns));
    net_pcb *lpcb = net_listen_plaintext(port, web_accept_cb, MAX_HTTP_CONN);
    if (!lpcb) {
        printf("[web] AP-mode listen on :%u FAILED: %s\n",
               port, net_listen_fail ? net_listen_fail : "unknown");
        return;
    }
    printf("[web] AP setup server on port %u (HTTP, auth enforced)\n", port);
}
#endif
