// mbedtls_config.h — TLS configuration for NetHID's on-device HTTPS server.
//
// Selected for a TLS 1.2 *server* presenting a single EC (preferred) or RSA
// certificate. The Pico needs no *accurate* clock — it presents its own cert and
// never validates anyone's dates — but MBEDTLS_HAVE_TIME is still enabled below
// because the SDK's altcp_tls_mbedtls.c references the session's `start`
// timestamp, which only exists under HAVE_TIME. The clock may read zero.
//
// VERIFY ON HARDWARE: module names are from upstream mbedTLS and the Pico SDK's
// TLS examples; the exact set depends on the mbedTLS version in your SDK. mbedTLS
// runs its own check_config.h at the right point — do NOT include it here (that
// runs the checks too early and they fail spuriously).
#pragma once
#include "config.h"   // NETHID_TLS_*_CONTENT_LEN, board tuning

// ── SDK altcp_tls compatibility ──────────────────────────────────────────────
// The SDK's altcp_tls_mbedtls.c reaches into private mbedTLS struct members
// (ssl_context.out_left, session.start) and calls the session-resumption
// functions, so it needs private access, time support, and the client module
// compiled in — even though we only act as a server.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS         // expose private struct members (out_left, session.start)
#define MBEDTLS_HAVE_TIME                     // session.start field; clock need not be accurate
#define MBEDTLS_PLATFORM_MS_TIME_ALT          // we supply mbedtls_ms_time() (src/mbedtls_pico_time.c)
#define MBEDTLS_SSL_CLI_C                     // declares mbedtls_ssl_get/set_session used by altcp

// ── Platform + entropy ───────────────────────────────────────────────────────
#define MBEDTLS_PLATFORM_C
// NOTE: deliberately NOT defining MBEDTLS_PLATFORM_MEMORY. With it set, lwIP's
// altcp_tls mem layer (altcp_tls_mbedtls_mem.c) redirects ALL mbedTLS allocations
// to lwIP's small MEM_SIZE heap, which can't satisfy the per-connection 8K+8K TLS
// record buffers — the handshake then fails to allocate and aborts before the
// ServerHello (RST, no alert). Without it, mbedTLS uses the C heap (PICO_HEAP_SIZE,
// 192 KB here), which has the room.
#define MBEDTLS_NO_PLATFORM_ENTROPY          // no /dev/urandom on the MCU
#define MBEDTLS_ENTROPY_HARDWARE_ALT         // pico_mbedtls provides mbedtls_hardware_poll()
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// ── TLS protocol: 1.2 server ─────────────────────────────────────────────────
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   // SNI (harmless with a single cert)

// ── Key exchange: ECDHE with EC or RSA certs (covers mkcert + Let's Encrypt) ──
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

// ── Public key / curves ──────────────────────────────────────────────────────
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED     // P-256 (our cert scripts' leaf key)
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED     // P-384 — Let's Encrypt ECDSA intermediates / ISRG Root X2
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED     // P-521 — extra headroom for other chains
#define MBEDTLS_RSA_C                        // for RSA certs (e.g. default mkcert)
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

// ── X.509 (parse our own server cert) ────────────────────────────────────────
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

// Our cert + key are embedded as PEM text, so PEM decoding is required at
// runtime (it is NOT a compile-time prerequisite for X.509, which is why
// omitting these still compiles but then fails to parse the cert at boot).
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

// ── Ciphers / AEAD / hashes ──────────────────────────────────────────────────
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C                        // AES-GCM (preferred suites)
#define MBEDTLS_CIPHER_MODE_CBC              // AES-CBC fallback for broad client compat
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

// ── Misc plumbing ────────────────────────────────────────────────────────────
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_ERROR_C                      // human-readable error strings (drop to save flash)

// ── Record buffers — the dominant per-connection RAM cost (board-tuned) ──────
// These halve per-connection RAM vs mbedTLS's 16384 default. Fallbacks guard the
// (shouldn't-happen) case where config.h's ENABLE_HTTPS gating skips this TU, and
// the #undef forces our value to win over any default already in scope.
#ifndef NETHID_TLS_IN_CONTENT_LEN
#define NETHID_TLS_IN_CONTENT_LEN     8192
#endif
#ifndef NETHID_TLS_OUT_CONTENT_LEN
#define NETHID_TLS_OUT_CONTENT_LEN    8192
#endif
#undef  MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN    NETHID_TLS_IN_CONTENT_LEN
#undef  MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN   NETHID_TLS_OUT_CONTENT_LEN

// NOTE: do NOT #include "mbedtls/check_config.h" here. The Pico SDK's
// pico_mbedtls_config.h includes this file, and mbedTLS runs check_config.h
// itself at the correct point (after config_adjust derives the *_CAN_* flags).
// Including it manually runs the prerequisite checks too early, so they fail
// spuriously (MBEDTLS_ENTROPY_C / GCM_C / SSL_TLS_C "not all prerequisites").
