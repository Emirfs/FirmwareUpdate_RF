/* Arithmetic mod p = 2^255-19
 * Daniel Beer <dlbeer@gmail.com>, 5 Jan 2014
 *
 * This file is in the public domain.
 */

#include "f25519.h"

const uint8_t f25519_zero[F25519_SIZE] = {0};
const uint8_t f25519_one[F25519_SIZE]  = {1};

void f25519_load(uint8_t *x, uint32_t c)
{
    unsigned int i;
    for (i = 0; i < sizeof(c); i++) {
        x[i] = c;
        c >>= 8;
    }
    for (; i < F25519_SIZE; i++)
        x[i] = 0;
}

void f25519_normalize(uint8_t *x)
{
    uint8_t minusp[F25519_SIZE];
    uint16_t c;
    int i;

    c = (x[31] >> 7) * 19;
    x[31] &= 127;

    for (i = 0; i < F25519_SIZE; i++) {
        c += x[i];
        x[i] = c;
        c >>= 8;
    }

    c = 19;
    for (i = 0; i + 1 < F25519_SIZE; i++) {
        c += x[i];
        minusp[i] = c;
        c >>= 8;
    }
    c += ((uint16_t)x[i]) - 128;
    minusp[31] = c;

    f25519_select(x, minusp, x, (c >> 15) & 1);
}

uint8_t f25519_eq(const uint8_t *x, const uint8_t *y)
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < F25519_SIZE; i++)
        sum |= x[i] ^ y[i];
    sum |= (sum >> 4);
    sum |= (sum >> 2);
    sum |= (sum >> 1);
    return (sum ^ 1) & 1;
}

void f25519_select(uint8_t *dst,
                   const uint8_t *zero, const uint8_t *one,
                   uint8_t condition)
{
    const uint8_t mask = -condition;
    int i;
    for (i = 0; i < F25519_SIZE; i++)
        dst[i] = zero[i] ^ (mask & (one[i] ^ zero[i]));
}

void f25519_add(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    uint16_t c = 0;
    int i;

    for (i = 0; i < F25519_SIZE; i++) {
        c >>= 8;
        c += ((uint16_t)a[i]) + ((uint16_t)b[i]);
        r[i] = c;
    }

    r[31] &= 127;
    c = (c >> 7) * 19;

    for (i = 0; i < F25519_SIZE; i++) {
        c += r[i];
        r[i] = c;
        c >>= 8;
    }
}

void f25519_sub(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    uint32_t c = 0;
    int i;

    c = 218;
    for (i = 0; i + 1 < F25519_SIZE; i++) {
        c += 65280 + ((uint32_t)a[i]) - ((uint32_t)b[i]);
        r[i] = c;
        c >>= 8;
    }

    c += ((uint32_t)a[31]) - ((uint32_t)b[31]);
    r[31] = c & 127;
    c = (c >> 7) * 19;

    for (i = 0; i < F25519_SIZE; i++) {
        c += r[i];
        r[i] = c;
        c >>= 8;
    }
}

void f25519_neg(uint8_t *r, const uint8_t *a)
{
    uint32_t c = 0;
    int i;

    c = 218;
    for (i = 0; i + 1 < F25519_SIZE; i++) {
        c += 65280 - ((uint32_t)a[i]);
        r[i] = c;
        c >>= 8;
    }

    c -= ((uint32_t)a[31]);
    r[31] = c & 127;
    c = (c >> 7) * 19;

    for (i = 0; i < F25519_SIZE; i++) {
        c += r[i];
        r[i] = c;
        c >>= 8;
    }
}

void f25519_mul__distinct(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    uint32_t c = 0;
    int i;

    for (i = 0; i < F25519_SIZE; i++) {
        int j;
        c >>= 8;
        for (j = 0; j <= i; j++)
            c += ((uint32_t)a[j]) * ((uint32_t)b[i - j]);
        for (; j < F25519_SIZE; j++)
            c += ((uint32_t)a[j]) * ((uint32_t)b[i + F25519_SIZE - j]) * 38;
        r[i] = c;
    }

    r[31] &= 127;
    c = (c >> 7) * 19;

    for (i = 0; i < F25519_SIZE; i++) {
        c += r[i];
        r[i] = c;
        c >>= 8;
    }
}

void f25519_mul(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    uint8_t tmp[F25519_SIZE];
    f25519_mul__distinct(tmp, a, b);
    f25519_copy(r, tmp);
}

void f25519_mul_c(uint8_t *r, const uint8_t *a, uint32_t b)
{
    uint32_t c = 0;
    int i;

    for (i = 0; i < F25519_SIZE; i++) {
        c >>= 8;
        c += b * ((uint32_t)a[i]);
        r[i] = c;
    }

    r[31] &= 127;
    c >>= 7;
    c *= 19;

    for (i = 0; i < F25519_SIZE; i++) {
        c += r[i];
        r[i] = c;
        c >>= 8;
    }
}

void f25519_inv__distinct(uint8_t *r, const uint8_t *x)
{
    uint8_t s[F25519_SIZE];
    int i;

    /* x^(p-2) = x^(2^255-21) icin ikili zincir:
     * 11111111... 01011 (bit temsilinde) */
    f25519_mul__distinct(s, x, x);
    f25519_mul__distinct(r, s, x);

    for (i = 0; i < 248; i++) {
        f25519_mul__distinct(s, r, r);
        f25519_mul__distinct(r, s, x);
    }

    f25519_mul__distinct(s, r, r);

    f25519_mul__distinct(r, s, s);
    f25519_mul__distinct(s, r, x);

    f25519_mul__distinct(r, s, s);

    f25519_mul__distinct(s, r, r);
    f25519_mul__distinct(r, s, x);

    f25519_mul__distinct(s, r, r);
    f25519_mul__distinct(r, s, x);
}

void f25519_inv(uint8_t *r, const uint8_t *x)
{
    uint8_t tmp[F25519_SIZE];
    f25519_inv__distinct(tmp, x);
    f25519_copy(r, tmp);
}
