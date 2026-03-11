/*
 * ed25519_verify.h — Ed25519 doğrulama (verify-only) arayüzü
 *
 * Algoritma kaynağı: RFC 8032, Bölüm 5.1.7
 * Eğri: Edwards25519 (Curve25519 twisted Edwards formu)
 * Hash: SHA-512 (dahili olarak, RFC 8032 gereği)
 * İmza boyutu: 64 byte (R[32] || S[32])
 * Açık anahtar boyutu: 32 byte (y koordinatı sıkıştırılmış)
 *
 * Güvenlik özellikleri:
 *   - Scalar çarpımı sabit-zamanlı (yan kanal saldırısına karşı)
 *   - Karşılaştırma sabit-zamanlı (zamanlama saldırısına karşı)
 *   - Dinamik bellek tahsisi yok
 *   - Tüm geçici değerler stack'te
 *
 * Flash tahmini: ~10-12 KB (@-Os, Cortex-M0)
 * RAM (stack): ~1.2 KB (doğrulama sırasında)
 *
 * Doğru kullanım:
 *   uint8_t pub[32] = { ... };    // Ed25519 public key
 *   uint8_t sig[64] = { ... };    // Alınan imza
 *   uint8_t msg[32] = { ... };    // İmzalanan veri (firmware SHA-256 hash)
 *   int ok = ed25519_verify(pub, msg, 32, sig);
 *   // ok == 0: imza geçerli; ok != 0: imza geçersiz
 */

#ifndef ED25519_VERIFY_H
#define ED25519_VERIFY_H

#include <stdint.h>
#include <stddef.h>

/*
 * ed25519_verify() — Ed25519 imzasını doğrula
 *
 * Parametreler:
 *   public_key[32] — Ed25519 açık anahtarı (sıkıştırılmış Edwards noktası)
 *   message[]      — İmzalanan mesaj (bu projede: firmware SHA-256 hash, 32 byte)
 *   msg_len        — Mesaj uzunluğu (byte)
 *   signature[64]  — Ed25519 imzası: R[32] || S[32]
 *
 * Dönüş değeri:
 *   0  — İmza geçerli
 *  -1  — İmza geçersiz (S aralık dışı, nokta çözülemiyor, doğrulama başarısız)
 *
 * Bu fonksiyon YALNIZCA doğrulama yapar; imzalama kodu içermez.
 * Private key hiçbir zaman bu cihaza girmez.
 */
int ed25519_verify(const uint8_t public_key[32],
                   const uint8_t *message,
                   size_t         msg_len,
                   const uint8_t  signature[64]);

#endif /* ED25519_VERIFY_H */
