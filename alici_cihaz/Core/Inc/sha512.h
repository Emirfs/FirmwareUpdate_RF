/*
 * sha512.h — SHA-512 (FIPS 180-4) minimal implementasyonu
 *
 * Amaç: Ed25519 doğrulama için gerekli SHA-512 primitifi.
 *       Ed25519 algoritması (RFC 8032) dahili olarak SHA-512 kullanır.
 *
 * Kısıtlar:
 *   - Dinamik bellek tahsisi yok
 *   - Tüm durum yapıları stack veya statik alanlarda tutulur
 *   - GCC uint64_t → Cortex-M0'da 2×32-bit emülasyon (doğru çalışır)
 *
 * Flash tahmini: ~3.5 KB (@-Os)
 * RAM (bağlam): sizeof(SHA512_CTX) = 8*8 + 2*8 + 128 = 208 byte
 */

#ifndef SHA512_H
#define SHA512_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t state[8];   /* H0..H7 — FIPS 180-4 Section 6.4.1 */
    uint64_t count[2];   /* Bit sayacı: count[0] = düşük, count[1] = yüksek */
    uint8_t  buf[128];   /* Tam olmayan blok arabelleği */
} SHA512_CTX;

void SHA512_Init  (SHA512_CTX *ctx);
void SHA512_Update(SHA512_CTX *ctx, const uint8_t *data, size_t len);
void SHA512_Final (uint8_t digest[64], SHA512_CTX *ctx);

#endif /* SHA512_H */
