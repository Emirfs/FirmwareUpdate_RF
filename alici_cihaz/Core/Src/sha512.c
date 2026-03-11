/*
 * sha512.c — SHA-512 (FIPS 180-4) orijinal implementasyonu
 *
 * Bu dosya standart FIPS 180-4 spesifikasyonu esas alınarak sıfırdan yazılmıştır.
 * Herhangi bir kütüphaneden kopyalanmış kod içermez.
 *
 * Cortex-M0 notları:
 *   uint64_t işlemleri GCC tarafından 32-bit çift komutlarla emüle edilir.
 *   Her ROTR64 yaklaşık 6-8 komuta dönüşür → yavaş ama doğru.
 *   Tek SHA-512 bloğu (1024-bit): ~80×8 = 640 64-bit işlem ≈ ~5000 komut.
 *   64-byte mesaj için: ~5000 komut @ 24 MHz = ~200 µs.
 */

#include "sha512.h"
#include <string.h>

/* ─── Yardımcı makrolar (FIPS 180-4 Bölüm 2.2.2) ─────────────────── */
#define ROTR64(x, n)  (((x) >> (n)) | ((x) << (64 - (n))))
#define SHR64(x, n)   ((x) >> (n))

#define CH(x,y,z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* SHA-512 büyük sigma ve küçük sigma fonksiyonları */
#define SIG0(x) (ROTR64(x,28) ^ ROTR64(x,34) ^ ROTR64(x,39))
#define SIG1(x) (ROTR64(x,14) ^ ROTR64(x,18) ^ ROTR64(x,41))
#define sig0(x) (ROTR64(x, 1) ^ ROTR64(x, 8) ^ SHR64(x, 7))
#define sig1(x) (ROTR64(x,19) ^ ROTR64(x,61) ^ SHR64(x, 6))

/* ─── Başlangıç hash değerleri H0-H7 (FIPS 180-4 Bölüm 5.3.5) ──── */
static const uint64_t SHA512_H0[8] = {
    UINT64_C(0x6a09e667f3bcc908), UINT64_C(0xbb67ae8584caa73b),
    UINT64_C(0x3c6ef372fe94f82b), UINT64_C(0xa54ff53a5f1d36f1),
    UINT64_C(0x510e527fade682d1), UINT64_C(0x9b05688c2b3e6c1f),
    UINT64_C(0x1f83d9abfb41bd6b), UINT64_C(0x5be0cd19137e2179)
};

/* ─── Yuvarlak sabitler K[0..79] (FIPS 180-4 Bölüm 4.2.3) ─────── */
static const uint64_t K[80] = {
    UINT64_C(0x428a2f98d728ae22), UINT64_C(0x7137449123ef65cd),
    UINT64_C(0xb5c0fbcfec4d3b2f), UINT64_C(0xe9b5dba58189dbbc),
    UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019),
    UINT64_C(0x923f82a4af194f9b), UINT64_C(0xab1c5ed5da6d8118),
    UINT64_C(0xd807aa98a3030242), UINT64_C(0x12835b0145706fbe),
    UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2),
    UINT64_C(0x72be5d74f27b896f), UINT64_C(0x80deb1fe3b1696b1),
    UINT64_C(0x9bdc06a725c71235), UINT64_C(0xc19bf174cf692694),
    UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3),
    UINT64_C(0x0fc19dc68b8cd5b5), UINT64_C(0x240ca1cc77ac9c65),
    UINT64_C(0x2de92c6f592b0275), UINT64_C(0x4a7484aa6ea6e483),
    UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5),
    UINT64_C(0x983e5152ee66dfab), UINT64_C(0xa831c66d2db43210),
    UINT64_C(0xb00327c898fb213f), UINT64_C(0xbf597fc7beef0ee4),
    UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725),
    UINT64_C(0x06ca6351e003826f), UINT64_C(0x142929670a0e6e70),
    UINT64_C(0x27b70a8546d22ffc), UINT64_C(0x2e1b21385c26c926),
    UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df),
    UINT64_C(0x650a73548baf63de), UINT64_C(0x766a0abb3c77b2a8),
    UINT64_C(0x81c2c92e47edaee6), UINT64_C(0x92722c851482353b),
    UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001),
    UINT64_C(0xc24b8b70d0f89791), UINT64_C(0xc76c51a30654be30),
    UINT64_C(0xd192e819d6ef5218), UINT64_C(0xd69906245565a910),
    UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8),
    UINT64_C(0x19a4c116b8d2d0c8), UINT64_C(0x1e376c085141ab53),
    UINT64_C(0x2748774cdf8eeb99), UINT64_C(0x34b0bcb5e19b48a8),
    UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb),
    UINT64_C(0x5b9cca4f7763e373), UINT64_C(0x682e6ff3d6b2b8a3),
    UINT64_C(0x748f82ee5defb2fc), UINT64_C(0x78a5636f43172f60),
    UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec),
    UINT64_C(0x90befffa23631e28), UINT64_C(0xa4506cebde82bde9),
    UINT64_C(0xbef9a3f7b2c67915), UINT64_C(0xc67178f2e372532b),
    UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207),
    UINT64_C(0xeada7dd6cde0eb1e), UINT64_C(0xf57d4f7fee6ed178),
    UINT64_C(0x06f067aa72176fba), UINT64_C(0x0a637dc5a2c898a6),
    UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b),
    UINT64_C(0x28db77f523047d84), UINT64_C(0x32caab7b40c72493),
    UINT64_C(0x3c9ebe0a15c9bebc), UINT64_C(0x431d67c49c100d4c),
    UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a),
    UINT64_C(0x5fcb6fab3ad6faec), UINT64_C(0x6c44198c4a475817)
};

/* ─── Büyük-küçük endian dönüşüm (SHA-512 big-endian çalışır) ──── */
static uint64_t load64_be(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] <<  8) |  (uint64_t)p[7];
}

static void store64_be(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >>  8); p[7] = (uint8_t)v;
}

/* ─── Tek blok (1024-bit = 128-byte) işleme (FIPS 180-4 Bölüm 6.4.2) */
static void sha512_block(SHA512_CTX *ctx, const uint8_t block[128]) {
    uint64_t W[80];
    uint64_t a, b, c, d, e, f, g, h;
    uint64_t T1, T2;
    int t;

    /* Mesaj programı hazırla */
    for (t = 0;  t < 16; t++) W[t] = load64_be(block + t * 8);
    for (t = 16; t < 80; t++) W[t] = sig1(W[t-2]) + W[t-7] + sig0(W[t-15]) + W[t-16];

    /* Çalışma değişkenlerini başlat */
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    /* 80 tur */
    for (t = 0; t < 80; t++) {
        T1 = h + SIG1(e) + CH(e,f,g) + K[t] + W[t];
        T2 = SIG0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    /* Ara karma değerlerini güncelle */
    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

/* ─── Kamuya açık API ────────────────────────────────────────────── */

void SHA512_Init(SHA512_CTX *ctx) {
    for (int i = 0; i < 8; i++) ctx->state[i] = SHA512_H0[i];
    ctx->count[0] = ctx->count[1] = 0;
}

void SHA512_Update(SHA512_CTX *ctx, const uint8_t *data, size_t len) {
    size_t bufpos = (size_t)(ctx->count[0] >> 3) & 127u;  /* buffer içindeki mevcut offset */

    /* Bit sayacını güncelle */
    uint64_t bits = (uint64_t)len << 3;
    ctx->count[0] += bits;
    if (ctx->count[0] < bits) ctx->count[1]++;  /* taşma */
    ctx->count[1] += (uint64_t)len >> 61;

    while (len > 0) {
        size_t space = 128 - bufpos;
        size_t take  = (len < space) ? len : space;
        memcpy(ctx->buf + bufpos, data, take);
        data   += take;
        len    -= take;
        bufpos += take;
        if (bufpos == 128) {
            sha512_block(ctx, ctx->buf);
            bufpos = 0;
        }
    }
}

void SHA512_Final(uint8_t digest[64], SHA512_CTX *ctx) {
    uint8_t pad[128];
    size_t bufpos = (size_t)(ctx->count[0] >> 3) & 127u;

    /* Dolgu: 0x80 baytı, sıfır byte'lar, 16-byte uzunluk (big-endian) */
    memset(pad, 0, 128);
    pad[0] = 0x80;
    size_t padlen = (bufpos < 112) ? (112 - bufpos) : (240 - bufpos);
    SHA512_Update(ctx, pad, padlen);

    /* Mesaj uzunluğunu ekle (128-bit big-endian, count[1]:count[0] bit cinsinden) */
    uint8_t len_block[16];
    store64_be(len_block + 0, ctx->count[1]);
    store64_be(len_block + 8, ctx->count[0]);
    SHA512_Update(ctx, len_block, 16);

    /* Hash değerini big-endian olarak yaz */
    for (int i = 0; i < 8; i++) store64_be(digest + i * 8, ctx->state[i]);

    /* Bağlamı sil */
    memset(ctx, 0, sizeof(*ctx));
}
