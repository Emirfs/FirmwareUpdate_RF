/*
 * sha256.c — SHA-256 implementasyonu (FIPS PUB 180-4)
 *
 * Cortex-M0 uzerinde test edilmistir. Dinamik bellek kullanmaz.
 * Her SHA256_Update cagrisi aralikli gelebilir (streaming).
 */

#include "sha256.h"
#include <string.h>

/* SHA-256 sabit K tablosu */
static const uint32_t K[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

#define S0(x) (ROTR32(x,  2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define S1(x) (ROTR32(x,  6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define s0(x) (ROTR32(x,  7) ^ ROTR32(x, 18) ^ ((x) >>  3))
#define s1(x) (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* 64 baytlik tek blok isle */
static void sha256_block(SHA256_CTX *ctx, const uint8_t *blk)
{
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t T1, T2;
    int i;

    /* Mesaj cetveli olustur */
    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)blk[i*4    ] << 24)
             | ((uint32_t)blk[i*4 + 1] << 16)
             | ((uint32_t)blk[i*4 + 2] <<  8)
             |  (uint32_t)blk[i*4 + 3];
    }
    for (i = 16; i < 64; i++) {
        W[i] = s1(W[i-2]) + W[i-7] + s0(W[i-15]) + W[i-16];
    }

    /* Ara durumu yukle */
    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    /* 64 tur */
    for (i = 0; i < 64; i++) {
        T1 = h + S1(e) + CH(e,f,g) + K[i] + W[i];
        T2 = S0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    /* Durumu guncelle */
    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void SHA256_Init(SHA256_CTX *ctx)
{
    ctx->state[0] = 0x6A09E667;
    ctx->state[1] = 0xBB67AE85;
    ctx->state[2] = 0x3C6EF372;
    ctx->state[3] = 0xA54FF53A;
    ctx->state[4] = 0x510E527F;
    ctx->state[5] = 0x9B05688C;
    ctx->state[6] = 0x1F83D9AB;
    ctx->state[7] = 0x5BE0CD19;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
}

void SHA256_Update(SHA256_CTX *ctx, const uint8_t *data, size_t len)
{
    uint32_t pos = (ctx->count[0] >> 3) & 63u; /* tampondaki mevcut bayt sayisi */

    /* Bit sayacini guncelle (overflow icin ust kelimeyi de tasi) */
    ctx->count[0] += (uint32_t)(len << 3);
    if (ctx->count[0] < (uint32_t)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    while (len > 0) {
        ctx->buf[pos++] = *data++;
        len--;
        if (pos == 64) {
            sha256_block(ctx, ctx->buf);
            pos = 0;
        }
    }
}

void SHA256_Final(uint8_t digest[32], SHA256_CTX *ctx)
{
    uint32_t pos = (ctx->count[0] >> 3) & 63u;
    uint32_t i;

    /* Padding: 0x80 ekle */
    ctx->buf[pos++] = 0x80;

    /* Doldurmaya yer yoksa mevcut bloku isle, yeni blok ac */
    if (pos > 56) {
        while (pos < 64) ctx->buf[pos++] = 0x00;
        sha256_block(ctx, ctx->buf);
        pos = 0;
    }
    while (pos < 56) ctx->buf[pos++] = 0x00;

    /* Mesaj uzunlugunu bit cinsinden big-endian ekle (8 byte) */
    ctx->buf[56] = (uint8_t)(ctx->count[1] >> 24);
    ctx->buf[57] = (uint8_t)(ctx->count[1] >> 16);
    ctx->buf[58] = (uint8_t)(ctx->count[1] >>  8);
    ctx->buf[59] = (uint8_t)(ctx->count[1]      );
    ctx->buf[60] = (uint8_t)(ctx->count[0] >> 24);
    ctx->buf[61] = (uint8_t)(ctx->count[0] >> 16);
    ctx->buf[62] = (uint8_t)(ctx->count[0] >>  8);
    ctx->buf[63] = (uint8_t)(ctx->count[0]      );
    sha256_block(ctx, ctx->buf);

    /* Durumu big-endian byte dizisine donustur */
    for (i = 0; i < 8; i++) {
        digest[i*4    ] = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4 + 2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i*4 + 3] = (uint8_t)(ctx->state[i]      );
    }

    /* Guvenlik: hassas veriyi temizle */
    memset(ctx, 0, sizeof(*ctx));
}
