/* Curve25519 (Montgomery form) — X25519 key exchange
 * Daniel Beer <dlbeer@gmail.com>, 18 Apr 2014
 *
 * This file is in the public domain.
 *
 * Sadece X25519 (scalar multiply) korundu; Ed25519 ve morph25519
 * cikarildi — Flash tasarrufu icin.
 */

#ifndef C25519_H_
#define C25519_H_

#include <stdint.h>
#include "f25519.h"

/* Curve25519: y^2 = x^3 + 486662x^2 + x, GF(2^255-19) */

/* Exponent (private key) boyutu */
#define C25519_KEY_SIZE 32

/* Base point X koordinati (9) */
extern const uint8_t c25519_base_x[F25519_SIZE];

/*
 * c25519_prepare — Private key'i Curve25519 icin hazirla (clamp)
 *
 * 32 random byte olusturulduktan sonra MUTLAKA bu fonksiyon cagirilmali.
 * Bit maskeleme: alt 3 bit sifirla, bit 255 sifirla, bit 254 set et.
 */
static inline void c25519_prepare(uint8_t *key)
{
    key[0]  &= 0xF8; /* Alt 3 bit sifirla (8'in katlari) */
    key[31] &= 0x7F; /* Bit 255 sifirla */
    key[31] |= 0x40; /* Bit 254 set et */
}

/*
 * c25519_smult — X25519 scalar multiplication
 *
 * result = e * q  (sadece X koordinati)
 *
 * result, q : 32 byte field element (little-endian)
 * e         : 32 byte scalar (private key, onceden c25519_prepare ile hazir)
 *
 * ECDH kullanimi:
 *   Kendi public key'ini uret:
 *     c25519_smult(pub, c25519_base_x, priv)
 *   Shared secret hesapla:
 *     c25519_smult(shared, partner_pub, own_priv)
 */
void c25519_smult(uint8_t *result, const uint8_t *q, const uint8_t *e);

#endif /* C25519_H_ */
