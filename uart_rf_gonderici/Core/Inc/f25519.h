/* Arithmetic mod p = 2^255-19
 * Daniel Beer <dlbeer@gmail.com>, 8 Jan 2014
 *
 * This file is in the public domain.
 */

#ifndef F25519_H_
#define F25519_H_

#include <stdint.h>
#include <string.h>

#define F25519_SIZE 32

extern const uint8_t f25519_zero[F25519_SIZE];
extern const uint8_t f25519_one[F25519_SIZE];

void f25519_load(uint8_t *x, uint32_t c);

static inline void f25519_copy(uint8_t *x, const uint8_t *a)
{
    memcpy(x, a, F25519_SIZE);
}

void f25519_normalize(uint8_t *x);
uint8_t f25519_eq(const uint8_t *x, const uint8_t *y);
void f25519_select(uint8_t *dst,
                   const uint8_t *zero, const uint8_t *one,
                   uint8_t condition);
void f25519_add(uint8_t *r, const uint8_t *a, const uint8_t *b);
void f25519_sub(uint8_t *r, const uint8_t *a, const uint8_t *b);
void f25519_neg(uint8_t *r, const uint8_t *a);
void f25519_mul(uint8_t *r, const uint8_t *a, const uint8_t *b);
void f25519_mul__distinct(uint8_t *r, const uint8_t *a, const uint8_t *b);
void f25519_mul_c(uint8_t *r, const uint8_t *a, uint32_t b);
void f25519_inv(uint8_t *r, const uint8_t *x);
void f25519_inv__distinct(uint8_t *r, const uint8_t *x);

#endif /* F25519_H_ */
