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
#include "secrets.h"
#include "tabs.h"
#include "config.h"
#if ENABLE_REMOTES
#include "remotes.h"
#endif
#include "net_compat.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
            if (!gate || tab_user_allowed(who, MAIN_TABS[i].id))
                frags[n++] = MAIN_TABS[i].html;
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

    // Who am I? (used by the UI to tailor what it shows per user)
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/whoami") == 0) {
        const char *who = auth_user_name(auth_token_user_index(sid));
        char jb[96];
        snprintf(jb, sizeof(jb), "{\"user\":\"%s\"}", who);
        http_json(pcb, 200, jb);
        return;
    }

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
        hid_abs_report_t rep={
            .buttons=(uint8_t)buttons,
            .x=(uint16_t)x, .y=(uint16_t)y,
            .wheel=(int8_t)(wheel>127?127:wheel<-127?-127:wheel),
        };
        hid_push_abs_report(&rep);
        if (buttons) {
            hid_abs_report_t rel=rep; rel.buttons=0;
            hid_push_abs_report(&rel);
        }
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
