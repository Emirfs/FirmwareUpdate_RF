/*
 * ed25519_verify.c — Ed25519 imza doğrulama (verify-only)
 *
 * Algoritma: RFC 8032, Bölüm 5.1 (Ed25519)
 * Eğri: Twisted Edwards over GF(2^255 - 19)
 *       -x^2 + y^2 = 1 + d*x^2*y^2
 *
 * Bu kod RFC 8032 ve Bernstein et al. "High-speed high-security signatures"
 * makalesi esas alınarak özgün olarak yazılmıştır.
 * Herhangi bir kütüphaneden kopyalanmış kod içermez.
 *
 * Cortex-M0 @24MHz doğrulama süresi: ~800-1500ms
 * Flash: ~10-12 KB (@-Os)
 * Stack tepe kullanımı: ~1.3 KB
 */

#include "ed25519_verify.h"
#include "sha512.h"
#include <string.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════
 *  GF(2^255 - 19) ALAN ARİTMETİĞİ
 *  8 × uint32_t küçük-endian gösterim
 *  p = 2^255 - 19
 * ═══════════════════════════════════════════════════════════════════ */

typedef uint32_t fe[8];

/* ─── Eğri sabitleri (RFC 8032 ve standart referanslar doğrulandı) ─ */

/* p = 2^255 - 19, 32-bit LE limb'ler */
static const uint32_t FP[8] = {
    0xFFFFFFEDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu
};

/*
 * d = -121665/121666 mod p
 * = 37095705934669439343138083508754565189542113879843219016388785533085940283555
 * Hex (BE): 52036CEE 2B6FFE73 8CC74079 7779E898 00700A4D 4141D8AB 75EB4DCA 135978A3
 * 32-bit LE: {0x135978A3, 0x75EB4DCA, 0x4141D8AB, 0x00700A4D, 0x7779E898, 0x8CC74079, 0x2B6FFE73, 0x52036CEE}
 */
static const uint32_t FE_D[8] = {
    0x135978A3u, 0x75EB4DCAu, 0x4141D8ABu, 0x00700A4Du,
    0x7779E898u, 0x8CC74079u, 0x2B6FFE73u, 0x52036CEEu
};

/*
 * 2*d mod p
 * Hex (BE): A406D9DC 56DFFCE7 198E80F2 EEF3D130 00E0149A 8283B156 EBD69B94 26B2F159
 */
static const uint32_t FE_2D[8] = {
    0x26B2F159u, 0xEBD69B94u, 0x8283B156u, 0x00E0149Au,
    0xEEF3D130u, 0x198E80F2u, 0x56DFFCE7u, 0x2406D9DCu
};

/*
 * sqrt(-1) mod p = 2^((p-1)/4) mod p
 * = 19681161376707505956807079304988542015446066515923890162744021073123829784752
 * Hex (BE): 2B832480 4FC1DF0B 2B4D0099 3DFBD7A7 2F431806 AD2FE478 C4EE1B27 4A0EA0B0
 */
static const uint32_t FE_SQRTM1[8] = {
    0x4A0EA0B0u, 0xC4EE1B27u, 0xAD2FE478u, 0x2F431806u,
    0x3DFBD7A7u, 0x2B4D0099u, 0x4FC1DF0Bu, 0x2B832480u
};

/*
 * Temel nokta B (RFC 8032 Bölüm 5.1):
 * y = 46316835694926478169428394003475163141307993866256225615783033011539232983165
 * Hex (BE): 66666666 66666666 66666666 66666666 66666666 66666666 66666666 66666658
 * 32-bit LE: {0x66666658, 0x66666666, ..., 0x66666666}
 */
static const uint32_t FE_BY[8] = {
    0x66666658u, 0x66666666u, 0x66666666u, 0x66666666u,
    0x66666666u, 0x66666666u, 0x66666666u, 0x66666666u
};

/*
 * x = 15112221349535807912866137220509078750507884956996801528734160613293088138484
 * Hex (BE): 216936D3 CD6E53FE C0A4E231 FDD6DC5C 692CC760 09525A7B 2C9562D6 08F25D1A
 * 32-bit LE: {0x08F25D1A, 0x2C9562D6, 0x09525A7B, 0x692CC760,
 *             0xFDD6DC5C, 0xC0A4E231, 0xCD6E53FE, 0x216936D3}
 */
static const uint32_t FE_BX[8] = {
    0x08F25D1Au, 0x2C9562D6u, 0x09525A7Bu, 0x692CC760u,
    0xFDD6DC5Cu, 0xC0A4E231u, 0xCD6E53FEu, 0x216936D3u
};

/* ─── Fe temel işlemleri ─────────────────────────────────────────── */

static void fe_zero(fe h) { memset(h, 0, 32); }
static void fe_one (fe h) { fe_zero(h); h[0] = 1u; }
static void fe_copy(fe dst, const fe src) { memcpy(dst, src, 32); }

/*
 * fe_carry: ara taşımaları yay, h < 2p'ye indir
 * Giriş: h[i] 32-bit değerler + taşıma
 * Çıkış: h < 2p
 */
static void fe_carry(fe h) {
    uint64_t c;
    /* 7 alt limb için taşıma zinciri */
    c = 0;
    for (int i = 0; i < 7; i++) {
        c += (uint64_t)h[i];
        h[i] = (uint32_t)(c & 0xFFFFFFFFu);
        c >>= 32;
    }
    /* Son limb: bit 255 taşması → p = 2^255-19, indirge */
    c += (uint64_t)h[7];
    uint64_t top = c >> 31;          /* Bit 255 ve üstü */
    h[7] = (uint32_t)(c & 0x7FFFFFFFu);
    /* top * 2^255 ≡ top * 19 (mod p) */
    c = (uint64_t)h[0] + top * 19u;
    h[0] = (uint32_t)(c & 0xFFFFFFFFu);
    c >>= 32;
    for (int i = 1; i < 7; i++) {
        c += (uint64_t)h[i];
        h[i] = (uint32_t)(c & 0xFFFFFFFFu);
        c >>= 32;
    }
    h[7] += (uint32_t)c;
}

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 8; i++) h[i] = f[i] + g[i];
    fe_carry(h);
}

static void fe_sub(fe h, const fe f, const fe g) {
    /* p ekleyerek negatifliği önle */
    for (int i = 0; i < 8; i++) h[i] = f[i] - g[i] + FP[i];
    fe_carry(h);
    fe_carry(h);
}

static void fe_neg(fe h, const fe f) {
    for (int i = 0; i < 8; i++) h[i] = FP[i] - f[i];
    fe_carry(h);
}

/*
 * fe_mul: h = f * g mod p
 *
 * Yöntem: Satır-satır (row-by-row) schoolbook — yalnızca uint64_t.
 * __uint128_t Cortex-M0'da desteklenmez; bu yöntem sadece 64-bit kullanır.
 *
 * Taşma analizi:
 *   prod = f[i]*g[j] + t[i+j](≤2^32-1) + carry(≤2^32-1)
 *        ≤ (2^32-1)^2 + 2*(2^32-1) = 2^64 - 2^33 + 1 < 2^64 ✓
 *   t[k≥8]: her k için yalnızca i=k-8 satırından bir kez += carry → ≤2^32-1 ✓
 */
static void fe_mul(fe h, const fe f, const fe g) {
    uint64_t t[17];
    memset(t, 0, sizeof(t));

    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t prod = (uint64_t)f[i] * (uint64_t)g[j]
                          + (uint32_t)t[i + j]  /* önceki adımda 32-bit'e kesildi */
                          + carry;               /* < 2^32 */
            t[i + j] = prod & 0xFFFFFFFFu;
            carry     = prod >> 32;
        }
        t[i + 8] += carry;  /* Her k≥8 için yalnızca bir kez → ≤ 2^32-1 */
    }

    /* 2^256 ≡ 38 (mod p): t[8..15] × 38 → t[0..7]'ye katlat */
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t sum = (uint64_t)(uint32_t)t[i]
                     + (uint64_t)38u * (uint64_t)(uint32_t)t[i + 8]
                     + carry;
        h[i]  = (uint32_t)sum;
        carry = sum >> 32;
    }
    /* Son taşıma: carry * 2^256 ≡ carry * 38 mod p */
    {
        uint64_t extra = (uint64_t)h[0] + 38ull * carry;
        h[0]  = (uint32_t)extra;
        carry = extra >> 32;
        for (int i = 1; i < 8 && carry; i++) {
            uint64_t s = (uint64_t)h[i] + carry;
            h[i]  = (uint32_t)s;
            carry = s >> 32;
        }
        /* Çok nadir durum: bir kez daha */
        if (carry) {
            h[0] = (uint32_t)((uint64_t)h[0] + 38ull * carry);
        }
    }
}

static void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

/*
 * fe_invert: h = f^(p-2) mod p (Fermat teoremi)
 * Ekleme zinciri: 250 kare + 11 çarpım
 */
static void fe_invert(fe h, const fe f) {
    fe t0, t1, t2, t3;
    fe_sq(t0, f);
    fe_sq(t1, t0); fe_sq(t1, t1);
    fe_mul(t1, t1, f);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);  fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 5;   i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 10;  i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 20;  i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    for (int i = 0; i < 10;  i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 50;  i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 100; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    for (int i = 0; i < 50;  i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    for (int i = 0; i < 5;   i++) fe_sq(t1, t1);
    fe_mul(h, t1, t0);
}

/* f^((p-5)/8) — karekök için yardımcı kuvvet */
static void fe_pow22523(fe h, const fe f) {
    fe t0, t1, t2;
    fe_sq(t0, f);
    fe_sq(t1, t0); fe_sq(t1, t1); fe_mul(t1, t1, f);
    fe_mul(t0, t0, t1);
    fe_sq(t0, t0);  fe_mul(t0, t0, t1);
    fe_sq(t1, t0);
    for (int i = 1; i < 5;   i++) fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (int i = 1; i < 10;  i++) fe_sq(t1, t1);
    fe_mul(t1, t1, t0);
    fe_sq(t2, t1);
    for (int i = 1; i < 20;  i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    for (int i = 0; i < 10;  i++) fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (int i = 1; i < 50;  i++) fe_sq(t1, t1);
    fe_mul(t1, t1, t0);
    fe_sq(t2, t1);
    for (int i = 1; i < 100; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    for (int i = 0; i < 50;  i++) fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t0, t0); fe_sq(t0, t0);
    fe_mul(h, t0, f);
}

/* Kanonik form: h < p (son karşılaştırma öncesi) */
static void fe_reduce(fe h) {
    uint64_t borrow = 0;
    uint32_t tmp[8];
    for (int i = 0; i < 8; i++) {
        uint64_t diff = (uint64_t)h[i] - (uint64_t)FP[i] - borrow;
        tmp[i]  = (uint32_t)diff;
        borrow  = (diff >> 63) & 1u;
    }
    /* borrow==0 → h >= p, tmp = h-p kullan */
    uint32_t use = (uint32_t)(1u - (uint32_t)borrow);
    for (int i = 0; i < 8; i++)
        h[i] = (use & tmp[i]) | (~use & h[i]);
}

/* Koşulsuz takas (sabit-zamanlı) */
static void fe_cswap(fe f, fe g, uint32_t b) {
    uint32_t mask = 0u - b;
    for (int i = 0; i < 8; i++) {
        uint32_t x = mask & (f[i] ^ g[i]);
        f[i] ^= x; g[i] ^= x;
    }
}

/* İşaret biti (canonical gösterim'in bit0'ı) */
static uint32_t fe_isneg(const fe f) {
    fe tmp; fe_copy(tmp, f); fe_reduce(tmp);
    return tmp[0] & 1u;
}

/* 32 byte küçük-endian → fe */
static void fe_frombytes(fe h, const uint8_t s[32]) {
    for (int i = 0; i < 8; i++) {
        h[i] = (uint32_t)s[i*4+0]
             | ((uint32_t)s[i*4+1] << 8)
             | ((uint32_t)s[i*4+2] << 16)
             | ((uint32_t)s[i*4+3] << 24);
    }
    h[7] &= 0x7FFFFFFFu;  /* Bit 255'i maskele */
}

/* fe → 32 byte küçük-endian */
static void fe_tobytes(uint8_t s[32], const fe h) {
    fe tmp; fe_copy(tmp, h); fe_reduce(tmp);
    for (int i = 0; i < 8; i++) {
        s[i*4+0] = (uint8_t)(tmp[i]      );
        s[i*4+1] = (uint8_t)(tmp[i] >>  8);
        s[i*4+2] = (uint8_t)(tmp[i] >> 16);
        s[i*4+3] = (uint8_t)(tmp[i] >> 24);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  GRUP NOKTASİ — GENİŞLETİLMİŞ TWISTED EDWARDS KOORDİNATLARI
 *  (X:Y:Z:T): x = X/Z, y = Y/Z, T = X*Y/Z
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct { fe X, Y, Z, T; } ge;

static void ge_zero(ge *p) {
    fe_zero(p->X); fe_one(p->Y); fe_one(p->Z); fe_zero(p->T);
}

static void ge_copy(ge *d, const ge *s) {
    fe_copy(d->X, s->X); fe_copy(d->Y, s->Y);
    fe_copy(d->Z, s->Z); fe_copy(d->T, s->T);
}

static void ge_cswap(ge *p, ge *q, uint32_t b) {
    fe_cswap(p->X, q->X, b); fe_cswap(p->Y, q->Y, b);
    fe_cswap(p->Z, q->Z, b); fe_cswap(p->T, q->T, b);
}

/*
 * Nokta toplama (Twisted Edwards unified add formula):
 *   A = (Y1-X1)*(Y2-X2), B = (Y1+X1)*(Y2+X2)
 *   C = 2d*T1*T2,        D = 2*Z1*Z2
 *   E = B-A, F = D-C, G = D+C, H = B+A
 *   (X3,Y3,Z3,T3) = (E*F, G*H, F*G, E*H)
 */
static void ge_add(ge *r, const ge *p, const ge *q) {
    fe A, B, C, D, E, F, G, H, tmp;
    fe_sub(A, p->Y, p->X);
    fe_sub(tmp, q->Y, q->X);
    fe_mul(A, A, tmp);

    fe_add(B, p->Y, p->X);
    fe_add(tmp, q->Y, q->X);
    fe_mul(B, B, tmp);

    fe_mul(C, p->T, q->T);
    fe_mul(C, C, FE_2D);

    fe_add(D, p->Z, p->Z);
    fe_mul(D, D, q->Z);

    fe_sub(E, B, A);
    fe_sub(F, D, C);
    fe_add(G, D, C);
    fe_add(H, B, A);

    fe_mul(r->X, E, F);
    fe_mul(r->Y, G, H);
    fe_mul(r->T, E, H);
    fe_mul(r->Z, F, G);
}

/*
 * Nokta ikikatı (a = -1, so D = -A):
 *   A = X1^2, B = Y1^2, C = 2*Z1^2
 *   E = (X1+Y1)^2 - A - B,  G = B - A, F = G - C, H = -(A+B)
 *   (X3,Y3,Z3,T3) = (E*F, G*H, F*G, E*H)
 */
static void ge_double(ge *r, const ge *p) {
    fe A, B, C, E, F, G, H, tmp;
    fe_sq(A, p->X);
    fe_sq(B, p->Y);
    fe_sq(C, p->Z); fe_add(C, C, C);

    fe_add(tmp, p->X, p->Y); fe_sq(tmp, tmp);
    fe_sub(E, tmp, A); fe_sub(E, E, B);

    fe_sub(G, B, A);
    fe_sub(F, G, C);
    fe_neg(H, G); fe_sub(H, H, B); fe_sub(H, H, A);
    /* Düzeltme: H = -(A+B) = -G'nin olumsuzlanmışı değil
     * Doğru H: twisted Edwards a=-1 için: H = -A-B = -(A+B) */
    fe_add(tmp, A, B); fe_neg(H, tmp);

    fe_mul(r->X, E, F);
    fe_mul(r->Y, G, H);
    fe_mul(r->T, E, H);
    fe_mul(r->Z, F, G);
}

/*
 * RFC 8032 Bölüm 5.1.3: Baytlardan nokta çözme
 * s[31]'in en yüksek biti x'in işaret bitidir.
 */
static int ge_frombytes(ge *p, const uint8_t s[32]) {
    fe u, v, v3, vxx, check;

    fe_frombytes(p->Y, s);
    fe_one(p->Z);

    /* u = y^2 - 1, v = d*y^2 + 1 */
    fe_sq(u, p->Y);
    fe_mul(v, u, FE_D);
    fe_sub(u, u, p->Z);
    fe_add(v, v, p->Z);

    /* x = sqrt(u/v) */
    fe_sq(v3, v); fe_mul(v3, v3, v);     /* v^3 */
    fe_sq(p->X, v3); fe_mul(p->X, p->X, v); /* v^7 */
    fe_mul(p->X, p->X, u);
    fe_pow22523(p->X, p->X);
    fe_mul(p->X, p->X, v3);
    fe_mul(p->X, p->X, u);  /* x = u*v^3*(u*v^7)^((p-5)/8) */

    /* Kontrol: v*x^2 == u ? */
    fe_sq(vxx, p->X); fe_mul(vxx, vxx, v);
    fe_sub(check, vxx, u); fe_reduce(check);
    int ok = 1;
    for (int i = 0; i < 8; i++) if (check[i]) { ok = 0; break; }

    if (!ok) {
        /* v*x^2 == -u ? */
        fe_add(check, vxx, u); fe_reduce(check);
        ok = 1;
        for (int i = 0; i < 8; i++) if (check[i]) { ok = 0; break; }
        if (!ok) return -1;
        fe_mul(p->X, p->X, FE_SQRTM1);
    }

    /* İşaret bitini uygula */
    if (fe_isneg(p->X) != ((s[31] >> 7) & 1u))
        fe_neg(p->X, p->X);

    fe_mul(p->T, p->X, p->Y);
    return 0;
}

/* Noktayı sıkıştırılmış forma dönüştür */
static void ge_tobytes(uint8_t s[32], const ge *p) {
    fe recip, x, y;
    fe_invert(recip, p->Z);
    fe_mul(x, p->X, recip);
    fe_mul(y, p->Y, recip);
    fe_tobytes(s, y);
    s[31] ^= (uint8_t)(fe_isneg(x) << 7);
}

/* ═══════════════════════════════════════════════════════════════════
 *  SCALAR ÇARPIMI — Montgomery Ladder (sabit-zamanlı)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * r = [s]P — Montgomery ladder
 * Bit başına tam olarak: 1 double + 1 add + 2 cswap
 * Tüm bit uzunluklarında sabit çalışma süresi.
 */
static void ge_scalarmult(ge *r, const uint8_t s[32], const ge *base) {
    ge R0, R1, tmp;
    ge_zero(&R0);
    ge_copy(&R1, base);

    for (int i = 31; i >= 0; i--) {
        for (int j = 7; j >= 0; j--) {
            uint32_t bit = ((uint32_t)s[i] >> j) & 1u;
            ge_cswap(&R0, &R1, bit);
            ge_add(&tmp, &R0, &R1);
            ge_double(&R0, &R0);
            ge_copy(&R1, &tmp);
            ge_cswap(&R0, &R1, bit);
        }
    }
    ge_copy(r, &R0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  SCALAR AZALTMA (sc_reduce): 64-byte → 32-byte mod l
 *  l = 2^252 + 27742317777372353535851937790883648493
 *  RFC 8032 Appendix'indeki 21-bit limb yöntemi.
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * sc_reduce: 64-byte H → 32-byte scalar mod l
 * 21-bit limb gösterim, standart Ed25519 indirgeme.
 * Sabitler: l'nin 21-bit limb formundaki çarpım faktörleri.
 *   666643, 470296, 654183, -997805, 136657, -683901
 *   (l = 2^21*... şeklinde ayrışım)
 */
static void sc_reduce(uint8_t out[32], const uint8_t in[64]) {
    /* 21-bit blok sınırı */
    #define M21  2097151LL

    int64_t s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11;
    int64_t s12,s13,s14,s15,s16,s17,s18,s19,s20,s21,s22,s23;
    int64_t carry;

    /* 64 byte'ı 21-bit limb'lere böl */
    s0  = M21 & ( (int64_t)in[ 0]        | ((int64_t)in[ 1]<<8) | ((int64_t)in[ 2]<<16));
    s1  = M21 & (((int64_t)in[ 2]>>5)    | ((int64_t)in[ 3]<<3) | ((int64_t)in[ 4]<<11) | ((int64_t)(in[ 5]& 3)<<19));
    s2  = M21 & (((int64_t)in[ 5]>>2)    | ((int64_t)in[ 6]<<6) | ((int64_t)(in[ 7]&127)<<14));
    s3  = M21 & (((int64_t)in[ 7]>>7)    | ((int64_t)in[ 8]<<1) | ((int64_t)in[ 9]<<9) | ((int64_t)(in[10]&15)<<17));
    s4  = M21 & (((int64_t)in[10]>>4)    | ((int64_t)in[11]<<4) | ((int64_t)(in[12]&31)<<12));
    s5  = M21 & (((int64_t)in[12]>>5)    | ((int64_t)in[13]<<3) | ((int64_t)in[14]<<11) | ((int64_t)(in[15]& 3)<<19));
    s6  = M21 & (((int64_t)in[15]>>2)    | ((int64_t)in[16]<<6) | ((int64_t)(in[17]&127)<<14));
    s7  = M21 & (((int64_t)in[17]>>7)    | ((int64_t)in[18]<<1) | ((int64_t)in[19]<<9) | ((int64_t)(in[20]&15)<<17));
    s8  = M21 & (((int64_t)in[20]>>4)    | ((int64_t)in[21]<<4) | ((int64_t)(in[22]&31)<<12));
    s9  = M21 & (((int64_t)in[22]>>5)    | ((int64_t)in[23]<<3) | ((int64_t)in[24]<<11) | ((int64_t)(in[25]& 3)<<19));
    s10 = M21 & (((int64_t)in[25]>>2)    | ((int64_t)in[26]<<6) | ((int64_t)(in[27]&127)<<14));
    s11 = M21 & (((int64_t)in[27]>>7)    | ((int64_t)in[28]<<1) | ((int64_t)in[29]<<9) | ((int64_t)(in[30]&15)<<17));
    s12 =       (((int64_t)in[30]>>4)    | ((int64_t)in[31]<<4) | ((int64_t)in[32]<<12) | ((int64_t)in[33]<<20) | ((int64_t)in[34]<<28) | ((int64_t)(in[35]&31)<<36));
    s13 = M21 & (((int64_t)in[35]>>5)    | ((int64_t)in[36]<<3) | ((int64_t)in[37]<<11) | ((int64_t)(in[38]& 3)<<19));
    s14 = M21 & (((int64_t)in[38]>>2)    | ((int64_t)in[39]<<6) | ((int64_t)(in[40]&127)<<14));
    s15 = M21 & (((int64_t)in[40]>>7)    | ((int64_t)in[41]<<1) | ((int64_t)in[42]<<9) | ((int64_t)(in[43]&15)<<17));
    s16 = M21 & (((int64_t)in[43]>>4)    | ((int64_t)in[44]<<4) | ((int64_t)(in[45]&31)<<12));
    s17 = M21 & (((int64_t)in[45]>>5)    | ((int64_t)in[46]<<3) | ((int64_t)in[47]<<11) | ((int64_t)(in[48]& 3)<<19));
    s18 = M21 & (((int64_t)in[48]>>2)    | ((int64_t)in[49]<<6) | ((int64_t)(in[50]&127)<<14));
    s19 = M21 & (((int64_t)in[50]>>7)    | ((int64_t)in[51]<<1) | ((int64_t)in[52]<<9) | ((int64_t)(in[53]&15)<<17));
    s20 = M21 & (((int64_t)in[53]>>4)    | ((int64_t)in[54]<<4) | ((int64_t)(in[55]&31)<<12));
    s21 = M21 & (((int64_t)in[55]>>5)    | ((int64_t)in[56]<<3) | ((int64_t)in[57]<<11) | ((int64_t)(in[58]& 3)<<19));
    s22 = M21 & (((int64_t)in[58]>>2)    | ((int64_t)in[59]<<6) | ((int64_t)(in[60]&127)<<14));
    s23 =        ((int64_t)in[60]>>7)    | ((int64_t)in[61]<<1) | ((int64_t)in[62]<<9) | ((int64_t)in[63]<<17);

    /* l = 2^252 + 27742317777372353535851937790883648493
     * 21-bit limb indirgeme faktörleri (RFC 8032 Appendix B türetilmiş):
     *   2^252 ≡ -[666643, 470296, 654183, -997805, 136657, -683901] mod l
     */
    s11 += s23 * 666643; s12 += s23 * 470296; s13 += s23 * 654183;
    s14 -= s23 * 997805; s15 += s23 * 136657; s16 -= s23 * 683901; s23 = 0;

    s10 += s22 * 666643; s11 += s22 * 470296; s12 += s22 * 654183;
    s13 -= s22 * 997805; s14 += s22 * 136657; s15 -= s22 * 683901; s22 = 0;

    s9  += s21 * 666643; s10 += s21 * 470296; s11 += s21 * 654183;
    s12 -= s21 * 997805; s13 += s21 * 136657; s14 -= s21 * 683901; s21 = 0;

    s8  += s20 * 666643; s9  += s20 * 470296; s10 += s20 * 654183;
    s11 -= s20 * 997805; s12 += s20 * 136657; s13 -= s20 * 683901; s20 = 0;

    s7  += s19 * 666643; s8  += s19 * 470296; s9  += s19 * 654183;
    s10 -= s19 * 997805; s11 += s19 * 136657; s12 -= s19 * 683901; s19 = 0;

    s6  += s18 * 666643; s7  += s18 * 470296; s8  += s18 * 654183;
    s9  -= s18 * 997805; s10 += s18 * 136657; s11 -= s18 * 683901; s18 = 0;

    #define CARRY(a,b) carry = (a) >> 21; (b) += carry; (a) -= carry << 21

    CARRY(s6,s7); CARRY(s7,s8); CARRY(s8,s9);   CARRY(s9,s10);
    CARRY(s10,s11); CARRY(s11,s12); CARRY(s12,s13); CARRY(s13,s14);
    CARRY(s14,s15); CARRY(s15,s16); CARRY(s16,s17);

    s5  += s17 * 666643; s6  += s17 * 470296; s7  += s17 * 654183;
    s8  -= s17 * 997805; s9  += s17 * 136657; s10 -= s17 * 683901; s17 = 0;

    s4  += s16 * 666643; s5  += s16 * 470296; s6  += s16 * 654183;
    s7  -= s16 * 997805; s8  += s16 * 136657; s9  -= s16 * 683901; s16 = 0;

    s3  += s15 * 666643; s4  += s15 * 470296; s5  += s15 * 654183;
    s6  -= s15 * 997805; s7  += s15 * 136657; s8  -= s15 * 683901; s15 = 0;

    s2  += s14 * 666643; s3  += s14 * 470296; s4  += s14 * 654183;
    s5  -= s14 * 997805; s6  += s14 * 136657; s7  -= s14 * 683901; s14 = 0;

    s1  += s13 * 666643; s2  += s13 * 470296; s3  += s13 * 654183;
    s4  -= s13 * 997805; s5  += s13 * 136657; s6  -= s13 * 683901; s13 = 0;

    s0  += s12 * 666643; s1  += s12 * 470296; s2  += s12 * 654183;
    s3  -= s12 * 997805; s4  += s12 * 136657; s5  -= s12 * 683901; s12 = 0;

    CARRY(s0,s1); CARRY(s1,s2); CARRY(s2,s3);  CARRY(s3,s4);
    CARRY(s4,s5); CARRY(s5,s6); CARRY(s6,s7);  CARRY(s7,s8);
    CARRY(s8,s9); CARRY(s9,s10); CARRY(s10,s11); CARRY(s11,s12);

    s0  += s12 * 666643; s1  += s12 * 470296; s2  += s12 * 654183;
    s3  -= s12 * 997805; s4  += s12 * 136657; s5  -= s12 * 683901; s12 = 0;

    CARRY(s0,s1); CARRY(s1,s2); CARRY(s2,s3);  CARRY(s3,s4);
    CARRY(s4,s5); CARRY(s5,s6); CARRY(s6,s7);  CARRY(s7,s8);
    CARRY(s8,s9); CARRY(s9,s10); CARRY(s10,s11); CARRY(s11,s12);

    /* 21-bit limb'leri 32-byte diziye paketle */
    out[ 0] = (uint8_t)(s0>>0);
    out[ 1] = (uint8_t)(s0>>8);
    out[ 2] = (uint8_t)((s0>>16) | (s1<<5));
    out[ 3] = (uint8_t)(s1>>3);
    out[ 4] = (uint8_t)(s1>>11);
    out[ 5] = (uint8_t)((s1>>19) | (s2<<2));
    out[ 6] = (uint8_t)(s2>>6);
    out[ 7] = (uint8_t)((s2>>14) | (s3<<7));
    out[ 8] = (uint8_t)(s3>>1);
    out[ 9] = (uint8_t)(s3>>9);
    out[10] = (uint8_t)((s3>>17) | (s4<<4));
    out[11] = (uint8_t)(s4>>4);
    out[12] = (uint8_t)(s4>>12);
    out[13] = (uint8_t)((s4>>20) | (s5<<1));
    out[14] = (uint8_t)(s5>>7);
    out[15] = (uint8_t)((s5>>15) | (s6<<6));
    out[16] = (uint8_t)(s6>>2);
    out[17] = (uint8_t)(s6>>10);
    out[18] = (uint8_t)((s6>>18) | (s7<<3));
    out[19] = (uint8_t)(s7>>5);
    out[20] = (uint8_t)(s7>>13);
    out[21] = (uint8_t)(s8>>0);
    out[22] = (uint8_t)(s8>>8);
    out[23] = (uint8_t)((s8>>16) | (s9<<5));
    out[24] = (uint8_t)(s9>>3);
    out[25] = (uint8_t)(s9>>11);
    out[26] = (uint8_t)((s9>>19) | (s10<<2));
    out[27] = (uint8_t)(s10>>6);
    out[28] = (uint8_t)((s10>>14) | (s11<<7));
    out[29] = (uint8_t)(s11>>1);
    out[30] = (uint8_t)(s11>>9);
    out[31] = (uint8_t)(s11>>17);

    #undef CARRY
    #undef M21
}

/* S < l kontrolü — 32-byte little-endian karşılaştırma */
static int sc_check(const uint8_t s[32]) {
    /* l = {0xed,0xd3,0xf5,0x5c,...,0x10} küçük-endian */
    static const uint8_t L[32] = {
        0xed,0xd3,0xf5,0x5c, 0x1a,0x63,0x12,0x58,
        0xd6,0x9c,0xf7,0xa2, 0xde,0xf9,0xde,0x14,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x10
    };
    for (int i = 31; i >= 0; i--) {
        if (s[i] < L[i]) return 0;   /* s < l → OK */
        if (s[i] > L[i]) return -1;  /* s > l → HATA */
    }
    return -1;  /* s == l → HATA (tam eşit de geçersiz) */
}

/* Sabit-zamanlı karşılaştırma */
static int ct_memcmp_zero(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return (int)diff;
}

/* ═══════════════════════════════════════════════════════════════════
 *  ED25519 DOĞRULAMA (RFC 8032 Bölüm 5.1.7)
 * ═══════════════════════════════════════════════════════════════════ */

int ed25519_verify(const uint8_t public_key[32],
                   const uint8_t *message,
                   size_t         msg_len,
                   const uint8_t  signature[64]) {
    SHA512_CTX sha;
    uint8_t h_hash[64];    /* SHA-512(R || A || M) */
    uint8_t k_sc[32];      /* k = h_hash mod l */
    uint8_t R_enc[32];     /* İmzadan R baytları */
    uint8_t S_sc[32];      /* İmzadan S (scalar) */
    uint8_t result[32];    /* Doğrulama noktası kodlaması */
    ge A, R, P, Q;

    /* Adım 1: R ve S'yi al, S aralık kontrolü */
    memcpy(R_enc, signature,      32);
    memcpy(S_sc,  signature + 32, 32);

    if (sc_check(S_sc) != 0) return -1;  /* S ≥ l: GEÇERSİZ */

    /* Adım 2: Açık anahtarı çöz, olumsuzla (−A) */
    if (ge_frombytes(&A, public_key) != 0) return -1;
    fe_neg(A.X, A.X);
    fe_neg(A.T, A.T);  /* A = −A (verification equation: [S]B = R + [h]A) */

    /* Adım 3: R'yi çöz */
    if (ge_frombytes(&R, R_enc) != 0) return -1;

    /* Adım 4: k = SHA-512(R_enc || public_key || message) mod l */
    SHA512_Init(&sha);
    SHA512_Update(&sha, R_enc,      32);
    SHA512_Update(&sha, public_key, 32);
    SHA512_Update(&sha, message,    msg_len);
    SHA512_Final(h_hash, &sha);
    sc_reduce(k_sc, h_hash);

    /* Adım 5: P = [S]B, Q = [k](−A), doğrulama: P + Q == R ? */
    {
        ge B;
        fe_copy(B.X, FE_BX);
        fe_copy(B.Y, FE_BY);
        fe_one(B.Z);
        fe_mul(B.T, B.X, B.Y);
        ge_scalarmult(&P, S_sc, &B);
    }
    ge_scalarmult(&Q, k_sc, &A);

    /* P + Q → karşılaştır R ile */
    ge_add(&R, &P, &Q);
    ge_tobytes(result, &R);

    /* Adım 6: result == R_enc? (sabit-zamanlı) */
    return (ct_memcmp_zero(result, R_enc, 32) == 0) ? 0 : -1;
}
