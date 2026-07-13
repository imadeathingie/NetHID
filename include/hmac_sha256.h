#pragma once
// ============================================================
//  hmac_sha256.h — self-contained SHA-256 + HMAC-SHA256.
//
//  No external crypto dependency (does not pull in mbedTLS). Used by the
//  challenge-response login: the device verifies HMAC-SHA256(password, nonce)
//  so the password itself never crosses the network. SHA-256 core is the
//  well-known public-domain implementation; HMAC follows RFC 2104.
// ============================================================
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LEN 32

void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen,
                 uint8_t out[SHA256_DIGEST_LEN]);

// HMAC-SHA256 then lowercase-hex encode. out_hex must hold >= 65 bytes
// (64 hex chars + NUL).
void hmac_sha256_hex(const uint8_t *key, size_t keylen,
                     const uint8_t *msg, size_t msglen,
                     char out_hex[65]);

#ifdef __cplusplus
}
#endif
