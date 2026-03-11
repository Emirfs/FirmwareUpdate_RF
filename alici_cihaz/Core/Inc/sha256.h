#ifndef SHA256_H
#define SHA256_H

/*
 * sha256.h — Kompakt SHA-256 implementasyonu (FIPS PUB 180-4)
 *
 * Cortex-M0 uyumlu, dinamik bellek yok, ~2KB flash.
 * Kullanim:
 *   SHA256_CTX ctx;
 *   SHA256_Init(&ctx);
 *   SHA256_Update(&ctx, data, len);   // birden fazla cagrilabilir
 *   SHA256_Final(digest, &ctx);       // 32 byte hash cikti
 */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8]; /* A..H ara durumu          */
    uint32_t count[2]; /* Islenen bit sayisi (64b)  */
    uint8_t  buf[64];  /* Dolmayan blok tamponu     */
} SHA256_CTX;

void SHA256_Init(SHA256_CTX *ctx);
void SHA256_Update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void SHA256_Final(uint8_t digest[32], SHA256_CTX *ctx);

#endif /* SHA256_H */
