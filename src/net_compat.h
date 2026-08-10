// net_compat.h — thin TCP/TLS shim for the web server.
//
// The whole point: the web server's connection code is written against net_*
// names and a net_pcb type. With ENABLE_HTTPS off, these map 1:1 to the plain
// lwIP raw tcp_* API (zero behavioural change from before). With ENABLE_HTTPS
// on, they map to lwIP's altcp_* layer and net_http_listen() wraps the listener
// in TLS using the embedded server certificate. Only the listen-socket setup
// differs between the two paths — every per-connection call is identical.
//
// Used by web.cpp and (for the control socket) server.cpp: net_listen_plaintext()
// gives a plain listener and net_listen_tls() a TLS one, sharing all per-conn code.
#pragma once
#include <stdio.h>
#include "config.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"

// Set by net_http_listen() to a short reason on failure; web_init() reports it
// via dbg() (which is HID-typed and therefore visible without a serial console).
__attribute__((unused)) static const char *net_listen_fail = 0;

#if ENABLE_HTTPS
// ── TLS path: application-layered TCP over mbedTLS ────────────────────────────
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "server_cert.h"          // SERVER_CERT / SERVER_KEY (+ _LEN). GENERATED.

typedef struct altcp_pcb net_pcb;
typedef altcp_recv_fn     net_recv_fn;
typedef altcp_sent_fn     net_sent_fn;
typedef altcp_accept_fn   net_accept_fn;
typedef altcp_err_fn      net_err_fn;

#define net_arg      altcp_arg
#define net_recv     altcp_recv
#define net_sent     altcp_sent
#define net_err      altcp_err
#define net_recved   altcp_recved
#define net_write    altcp_write
#define net_output   altcp_output
#define net_close    altcp_close
#define net_abort    altcp_abort
#define net_sndbuf   altcp_sndbuf
#define net_setprio  altcp_setprio
#define net_poll     altcp_poll

// Build the server TLS config (cert + key) once and cache it; shared by every
// TLS listener (web :443 and the control socket's TLS port).
static inline struct altcp_tls_config *net_tls_server_cfg(void) {
    static struct altcp_tls_config *cfg = NULL;
    if (!cfg) {
        cfg = altcp_tls_create_config_server_privkey_cert(
                  (const u8_t *)SERVER_KEY,  SERVER_KEY_LEN,
                  NULL, 0,
                  (const u8_t *)SERVER_CERT, SERVER_CERT_LEN);
        if (!cfg) { net_listen_fail = "tls-config (cert/key parse?)"; printf("[net] TLS config build failed (cert/key parse?)\n"); }
    }
    return cfg;
}

// TLS-wrapped listening socket on `port`. Returns the listen pcb, or NULL.
static inline net_pcb *net_listen_tls(u16_t port, net_accept_fn accept_fn, u8_t backlog) {
    struct altcp_tls_config *cfg = net_tls_server_cfg();
    if (!cfg) return NULL;
    net_pcb *pcb = altcp_tls_new(cfg, IPADDR_TYPE_ANY);
    if (!pcb) { net_listen_fail = "altcp_tls_new (out of memory?)"; printf("[net] altcp_tls_new :%d failed\n", port); return NULL; }
    if (altcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) { net_listen_fail = "bind"; printf("[net] bind :%d failed\n", port); altcp_close(pcb); return NULL; }
    net_pcb *lpcb = altcp_listen_with_backlog(pcb, backlog);
    if (!lpcb) { net_listen_fail = "listen"; printf("[net] listen :%d failed\n", port); altcp_close(pcb); return NULL; }
    altcp_accept(lpcb, accept_fn);
    return lpcb;
}

// Plaintext listening socket on `port` (altcp over raw TCP — same wire bytes as
// tcp_*, just through the altcp layer so it shares per-connection code with TLS).
static inline net_pcb *net_listen_plaintext(u16_t port, net_accept_fn accept_fn, u8_t backlog) {
    net_pcb *pcb = altcp_tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return NULL;
    if (altcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) { altcp_close(pcb); return NULL; }
    net_pcb *lpcb = altcp_listen_with_backlog(pcb, backlog);
    if (!lpcb) { altcp_close(pcb); return NULL; }
    altcp_accept(lpcb, accept_fn);
    return lpcb;
}

#else
// ── Plain path: raw lwIP TCP (identical to the pre-TLS server) ────────────────
#include "lwip/tcp.h"

typedef struct tcp_pcb net_pcb;
typedef tcp_recv_fn    net_recv_fn;
typedef tcp_sent_fn    net_sent_fn;
typedef tcp_accept_fn  net_accept_fn;
typedef tcp_err_fn     net_err_fn;

#define net_arg      tcp_arg
#define net_recv     tcp_recv
#define net_sent     tcp_sent
#define net_err      tcp_err
#define net_recved   tcp_recved
#define net_write    tcp_write
#define net_output   tcp_output
#define net_close    tcp_close
#define net_abort    tcp_abort
#define net_sndbuf   tcp_sndbuf
#define net_setprio  tcp_setprio
#define net_poll     tcp_poll

static inline net_pcb *net_listen_plaintext(u16_t port, net_accept_fn accept_fn, u8_t backlog) {
    net_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return NULL;
    if (tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) return NULL;
    net_pcb *lpcb = tcp_listen_with_backlog(pcb, backlog);
    if (!lpcb) return NULL;
    tcp_accept(lpcb, accept_fn);
    return lpcb;
}

#endif

// Web server listener: TLS when built with HTTPS, plaintext otherwise.
static inline net_pcb *net_http_listen(u16_t port, net_accept_fn accept_fn, u8_t backlog) {
#if ENABLE_HTTPS
    return net_listen_tls(port, accept_fn, backlog);
#else
    return net_listen_plaintext(port, accept_fn, backlog);
#endif
}

// Port the web server listens on, depending on build.
#if ENABLE_HTTPS
#define WEB_LISTEN_PORT  HTTPS_PORT
#else
#define WEB_LISTEN_PORT  HTTP_PORT
#endif
