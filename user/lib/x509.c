/* x509.c — проверка сертификатов (см. x509.h).
 *
 *   SHA-384/512        — для подписей sha384WithRSA, ecdsa-with-SHA384 и т.п.
 *   большие числа       — по 32 бита в «цифре», умножение Монтгомери (CIOS);
 *                         RSA: s^e mod n, e обычно 65537 — 17 умножений;
 *   кривые P-256, P-384 — точки в якобиевых координатах, a = -3;
 *                         ECDSA: u1*G + u2*Q одним проходом (трюк Шамира);
 *   ASN.1 DER           — только чтение: тег, длина, значение, с проверкой границ;
 *   X.509               — кто выдал, кому, сроки, ключ, subjectAltName, basicConstraints;
 *   цепочка             — от сертификата сайта вверх по «issuer», пока не встретим
 *                         корень из /etc/certs.der, каждую подпись проверяем.
 * Проверка подписи — не секретная операция, поэтому без защиты от атак по времени. */
#ifdef TLS_HOST
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include "myos.h"
#endif
#include "x509.h"
#include "tls.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

/* ====================================================================== SHA-512 / SHA-384 */

typedef struct { u64 h[8]; u8 buf[128]; int n; u64 len; } sha512_ctx;

static const u64 K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

#define R64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(sha512_ctx *c, const u8 *p) {
    u64 w[80], a, b, d, e, f, g, h, cc;
    for (int i = 0; i < 16; i++) {
        u64 v = 0;
        for (int k = 0; k < 8; k++) v = v << 8 | p[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        u64 s0 = R64(w[i - 15], 1) ^ R64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = R64(w[i - 2], 19) ^ R64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 80; i++) {
        u64 t1 = h + (R64(e, 14) ^ R64(e, 18) ^ R64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        u64 t2 = (R64(a, 28) ^ R64(a, 34) ^ R64(a, 39)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha512_init(sha512_ctx *c, int is384) {
    static const u64 iv512[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                  0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    static const u64 iv384[8] = { 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                  0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL };
    memcpy(c->h, is384 ? iv384 : iv512, sizeof(iv512));
    c->n = 0;
    c->len = 0;
}

static void sha512_update(sha512_ctx *c, const void *data, int n) {
    const u8 *p = data;
    c->len += n;
    while (n > 0) {
        int k = 128 - c->n;
        if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}

static void sha512_final(sha512_ctx *c, u8 *out, int outlen) {
    u64 bits = c->len * 8;
    u8 pad = 0x80, zero = 0, lenb[16];
    sha512_update(c, &pad, 1);
    while (c->n != 112) sha512_update(c, &zero, 1);
    memset(lenb, 0, 8);
    for (int i = 0; i < 8; i++) lenb[8 + i] = bits >> (56 - 8 * i);
    sha512_update(c, lenb, 16);
    for (int i = 0; i < outlen / 8; i++)
        for (int k = 0; k < 8; k++) out[i * 8 + k] = c->h[i] >> (56 - 8 * k);
}

void sha512(const void *data, int n, unsigned char out[64]) { sha512_ctx c; sha512_init(&c, 0); sha512_update(&c, data, n); sha512_final(&c, out, 64); }
void sha384(const void *data, int n, unsigned char out[48]) { sha512_ctx c; sha512_init(&c, 1); sha512_update(&c, data, n); sha512_final(&c, out, 48); }

/* хеш по номеру: 1 SHA-256, 2 SHA-384, 3 SHA-512 -> длина */
static int hash_n(int h, const u8 *d, int n, u8 *out) {
    if (h == 1) { sha256(d, n, out); return 32; }
    if (h == 2) { sha384(d, n, out); return 48; }
    if (h == 3) { sha512(d, n, out); return 64; }
    return 0;
}

/* ====================================================================== большие числа */

#define BN 132                                  /* до 4096 бит (+запас) */

typedef struct {
    int n;                                      /* сколько 32-битных цифр */
    u32 m[BN], rr[BN], one[BN];                 /* модуль, R^2 mod m, R mod m (единица в форме Монтгомери) */
    u32 m0inv;                                  /* -m^-1 mod 2^32 */
} mont;

/* байты (старшие первыми) -> n цифр (младшие первыми); -1 — не влезло */
static int bn_read(u32 *a, int n, const u8 *b, int len) {
    while (len > 0 && !*b) { b++; len--; }
    if (len > n * 4) return -1;
    memset(a, 0, n * 4);
    for (int i = 0; i < len; i++) a[i / 4] |= (u32)b[len - 1 - i] << (8 * (i % 4));
    return 0;
}

static void bn_write(const u32 *a, int n, u8 *b, int len) {
    for (int i = 0; i < len; i++) b[len - 1 - i] = i / 4 < n ? a[i / 4] >> (8 * (i % 4)) : 0;
}

static int bn_cmp(const u32 *a, const u32 *b, int n) {
    for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static int bn_zero(const u32 *a, int n) { for (int i = 0; i < n; i++) if (a[i]) return 0; return 1; }

static u32 bn_sub(u32 *r, const u32 *a, const u32 *b, int n) {
    u64 borrow = 0;
    for (int i = 0; i < n; i++) {
        u64 d = (u64)a[i] - b[i] - borrow;
        r[i] = (u32)d;
        borrow = (d >> 32) & 1;
    }
    return (u32)borrow;
}

static u32 bn_add(u32 *r, const u32 *a, const u32 *b, int n) {
    u64 c = 0;
    for (int i = 0; i < n; i++) { c += (u64)a[i] + b[i]; r[i] = (u32)c; c >>= 32; }
    return (u32)c;
}

/* r = a * b * R^-1 mod m (a, b < m); r может совпадать с a или b */
static void mont_mul(const mont *M, u32 *r, const u32 *a, const u32 *b) {
    u32 t[BN + 2];
    int n = M->n;
    memset(t, 0, sizeof(u32) * (n + 2));
    for (int i = 0; i < n; i++) {
        u64 c = 0;
        u32 bi = b[i];
        for (int j = 0; j < n; j++) { c += (u64)a[j] * bi + t[j]; t[j] = (u32)c; c >>= 32; }
        c += t[n]; t[n] = (u32)c; t[n + 1] = (u32)(c >> 32);
        u32 q = t[0] * M->m0inv;
        c = ((u64)q * M->m[0] + t[0]) >> 32;
        for (int j = 1; j < n; j++) { c += (u64)q * M->m[j] + t[j]; t[j - 1] = (u32)c; c >>= 32; }
        c += t[n]; t[n - 1] = (u32)c; t[n] = t[n + 1] + (u32)(c >> 32);
    }
    if (t[n] || bn_cmp(t, M->m, n) >= 0) bn_sub(t, t, M->m, n);
    memcpy(r, t, n * 4);
}

/* модуль из байтов (старшие первыми) */
static int mont_init(mont *M, const u8 *mod, int len) {
    while (len > 0 && !*mod) { mod++; len--; }
    int n = (len + 3) / 4;
    if (n < 1 || n > BN - 1) return -1;
    M->n = n;
    bn_read(M->m, n, mod, len);
    if (!(M->m[0] & 1)) return -1;                                  /* чётный модуль — не бывает */
    u32 x = 1;                                                      /* m^-1 mod 2^32 (Ньютон) */
    for (int i = 0; i < 5; i++) x *= 2 - M->m[0] * x;
    M->m0inv = -x;
    /* R mod m и R^2 mod m — удвоениями: x = 2x mod m */
    u32 v[BN + 1];
    memset(v, 0, sizeof(v));
    v[0] = 1;
    for (int i = 0; i < 2 * 32 * n; i++) {
        u32 top = 0;
        for (int k = 0; k < n; k++) { u32 nt = v[k] >> 31; v[k] = v[k] << 1 | top; top = nt; }
        if (top || bn_cmp(v, M->m, n) >= 0) bn_sub(v, v, M->m, n);
        if (i == 32 * n - 1) memcpy(M->one, v, n * 4);
    }
    memcpy(M->rr, v, n * 4);
    return 0;
}

static void mont_to(const mont *M, u32 *r, const u32 *a) { mont_mul(M, r, a, M->rr); }
static void mont_from(const mont *M, u32 *r, const u32 *a) {
    u32 one[BN];
    memset(one, 0, M->n * 4);
    one[0] = 1;
    mont_mul(M, r, a, one);
}

/* r = a^e (всё в форме Монтгомери; e — обычное число из ebytes байт) */
static void mont_pow(const mont *M, u32 *r, const u32 *a, const u8 *e, int elen) {
    u32 x[BN];
    memcpy(x, M->one, M->n * 4);
    for (int i = 0; i < elen; i++)
        for (int b = 7; b >= 0; b--) {
            mont_mul(M, x, x, x);
            if ((e[i] >> b) & 1) mont_mul(M, x, x, a);
        }
    memcpy(r, x, M->n * 4);
}

/* ====================================================================== RSA */

static const u8 di256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const u8 di384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const u8 di512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };

static mont rsa_m;                                                 /* большая — не на стеке */

/* s^e mod n -> em (k байт, k — длина модуля); 0 — ок */
static int rsa_public(const u8 *n, int nlen, const u8 *e, int elen, const u8 *sig, int slen, u8 *em, int *k) {
    while (nlen > 0 && !*n) { n++; nlen--; }
    if (nlen < 128 || mont_init(&rsa_m, n, nlen) < 0) return -1;   /* RSA меньше 1024 бит — нет */
    while (elen > 0 && !*e) { e++; elen--; }
    if (elen < 1 || elen > 8) return -1;
    u32 s[BN];
    if (slen > nlen + 1 || bn_read(s, rsa_m.n, sig, slen) < 0 || bn_cmp(s, rsa_m.m, rsa_m.n) >= 0) return -1;
    mont_to(&rsa_m, s, s);
    mont_pow(&rsa_m, s, s, e, elen);
    mont_from(&rsa_m, s, s);
    *k = nlen;
    bn_write(s, rsa_m.n, em, nlen);
    return 0;
}

/* PKCS#1 v1.5: em = 00 01 FF..FF 00 DigestInfo H */
static int rsa_pkcs1(const u8 *n, int nlen, const u8 *e, int elen, const u8 *sig, int slen, int hash, const u8 *msg, int mlen) {
    u8 em[520], h[64];
    int k;
    if (rsa_public(n, nlen, e, elen, sig, slen, em, &k) < 0) return -1;
    int hl = hash_n(hash, msg, mlen, h);
    const u8 *di = hash == 1 ? di256 : hash == 2 ? di384 : di512;
    int dl = sizeof(di256);
    int tl = dl + hl;
    if (!hl || k < tl + 11) return -1;
    if (em[0] != 0 || em[1] != 1) return -1;
    for (int i = 2; i < k - tl - 1; i++) if (em[i] != 0xFF) return -1;
    if (em[k - tl - 1] != 0) return -1;
    if (memcmp(em + k - tl, di, dl) || memcmp(em + k - hl, h, hl)) return -1;
    return 0;
}

/* RSASSA-PSS (MGF1 тем же хешем, соль длиной в хеш — как в TLS 1.3) */
static int rsa_pss(const u8 *n, int nlen, const u8 *e, int elen, const u8 *sig, int slen, int hash, const u8 *msg, int mlen) {
    u8 em[520], mh[64], db[520], tmp[64 + 8 + 64 + 4];
    int k;
    if (rsa_public(n, nlen, e, elen, sig, slen, em, &k) < 0) return -1;
    while (nlen > 0 && !*n) { n++; nlen--; }
    int bits = (nlen - 1) * 8;
    for (int b = n[0]; b; b >>= 1) bits++;
    int embits = bits - 1, emlen = (embits + 7) / 8;
    const u8 *E = em + (k - emlen);                                /* em длиной k, EM — последние emlen */
    if (k - emlen > 0 && em[0]) return -1;
    int hl = hash_n(hash, msg, mlen, mh);
    if (!hl || emlen < 2 * hl + 2 || E[emlen - 1] != 0xBC) return -1;
    int dbl = emlen - hl - 1;
    const u8 *H = E + dbl;
    for (int o = 0, cnt = 0; o < dbl; cnt++) {                     /* маска MGF1(H) */
        u8 c[64 + 4], out[64];
        memcpy(c, H, hl);
        c[hl] = cnt >> 24; c[hl + 1] = cnt >> 16; c[hl + 2] = cnt >> 8; c[hl + 3] = cnt;
        hash_n(hash, c, hl + 4, out);
        for (int i = 0; i < hl && o < dbl; i++, o++) db[o] = E[o] ^ out[i];
    }
    db[0] &= 0xFF >> (8 * emlen - embits);
    int i = 0;
    while (i < dbl - hl - 1 && !db[i]) i++;
    if (db[i] != 1) return -1;
    int salt = dbl - i - 1;
    if (salt != hl) return -1;
    memset(tmp, 0, 8);
    memcpy(tmp + 8, mh, hl);
    memcpy(tmp + 8 + hl, db + i + 1, salt);
    u8 h2[64];
    hash_n(hash, tmp, 8 + hl + salt, h2);
    return memcmp(h2, H, hl) ? -1 : 0;
}

/* ====================================================================== кривые P-256, P-384 */

typedef struct {
    int bytes;                                                     /* 32 или 48 */
    mont p, n;                                                     /* поле и порядок группы */
    u32 b[BN], gx[BN], gy[BN];                                     /* в форме Монтгомери по p */
    int ok;
} curve;

static curve c256, c384;

static void hex2(const char *h, u8 *out, int len) {
    for (int i = 0; i < len; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char c = h[i * 2 + k]; v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10); }
        out[i] = v;
    }
}

static curve *get_curve(int bytes) {
    curve *c = bytes == 32 ? &c256 : bytes == 48 ? &c384 : NULL;
    if (!c || c->ok) return c;
    static const char *P[2][5] = {
        { "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
          "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
          "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
          "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
          "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5" },
        { "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
          "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
          "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
          "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
          "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f" },
    };
    const char **h = P[bytes == 48];
    u8 buf[48];
    c->bytes = bytes;
    hex2(h[0], buf, bytes); mont_init(&c->p, buf, bytes);
    hex2(h[1], buf, bytes); mont_init(&c->n, buf, bytes);
    int n = c->p.n;
    hex2(h[2], buf, bytes); bn_read(c->b, n, buf, bytes); mont_to(&c->p, c->b, c->b);
    hex2(h[3], buf, bytes); bn_read(c->gx, n, buf, bytes); mont_to(&c->p, c->gx, c->gx);
    hex2(h[4], buf, bytes); bn_read(c->gy, n, buf, bytes); mont_to(&c->p, c->gy, c->gy);
    c->ok = 1;
    return c;
}

/* поле: сложение и вычитание по модулю p */
static void fp_add(const mont *M, u32 *r, const u32 *a, const u32 *b) {
    u32 c = bn_add(r, a, b, M->n);
    if (c || bn_cmp(r, M->m, M->n) >= 0) bn_sub(r, r, M->m, M->n);
}
static void fp_sub(const mont *M, u32 *r, const u32 *a, const u32 *b) {
    if (bn_sub(r, a, b, M->n)) bn_add(r, r, M->m, M->n);
}

typedef struct { u32 x[16], y[16], z[16]; int inf; } jpoint;      /* до 384 бит = 12 цифр */

static void pt_double(const curve *c, jpoint *r, const jpoint *p) {
    const mont *M = &c->p;
    if (p->inf || bn_zero(p->y, M->n)) { r->inf = 1; return; }
    u32 delta[16], gamma[16], beta[16], alpha[16], t1[16], t2[16];
    mont_mul(M, delta, p->z, p->z);
    mont_mul(M, gamma, p->y, p->y);
    mont_mul(M, beta, p->x, gamma);
    fp_sub(M, t1, p->x, delta);
    fp_add(M, t2, p->x, delta);
    mont_mul(M, alpha, t1, t2);
    fp_add(M, t1, alpha, alpha); fp_add(M, alpha, t1, alpha);      /* alpha = 3 (x-d)(x+d) */
    u32 x3[16], y3[16], z3[16];
    mont_mul(M, x3, alpha, alpha);
    fp_add(M, t1, beta, beta); fp_add(M, t1, t1, t1); fp_add(M, t2, t1, t1);   /* t1 = 4 beta, t2 = 8 beta */
    fp_sub(M, x3, x3, t2);
    fp_add(M, z3, p->y, p->z);
    mont_mul(M, z3, z3, z3);
    fp_sub(M, z3, z3, gamma);
    fp_sub(M, z3, z3, delta);
    fp_sub(M, t1, t1, x3);
    mont_mul(M, y3, alpha, t1);
    mont_mul(M, t2, gamma, gamma);
    fp_add(M, t2, t2, t2); fp_add(M, t2, t2, t2); fp_add(M, t2, t2, t2);       /* 8 gamma^2 */
    fp_sub(M, y3, y3, t2);
    memcpy(r->x, x3, sizeof(x3)); memcpy(r->y, y3, sizeof(y3)); memcpy(r->z, z3, sizeof(z3));
    r->inf = 0;
}

static void pt_add(const curve *c, jpoint *r, const jpoint *p, const jpoint *q) {
    const mont *M = &c->p;
    if (p->inf) { *r = *q; return; }
    if (q->inf) { *r = *p; return; }
    u32 z1z1[16], z2z2[16], u1[16], u2[16], s1[16], s2[16], h[16], i[16], j[16], rr[16], v[16], t[16];
    mont_mul(M, z1z1, p->z, p->z);
    mont_mul(M, z2z2, q->z, q->z);
    mont_mul(M, u1, p->x, z2z2);
    mont_mul(M, u2, q->x, z1z1);
    mont_mul(M, s1, p->y, q->z); mont_mul(M, s1, s1, z2z2);
    mont_mul(M, s2, q->y, p->z); mont_mul(M, s2, s2, z1z1);
    fp_sub(M, h, u2, u1);
    fp_sub(M, rr, s2, s1);
    if (bn_zero(h, M->n)) {
        if (bn_zero(rr, M->n)) { pt_double(c, r, p); return; }
        r->inf = 1;
        return;
    }
    fp_add(M, i, h, h); mont_mul(M, i, i, i);
    mont_mul(M, j, h, i);
    fp_add(M, rr, rr, rr);
    mont_mul(M, v, u1, i);
    u32 x3[16], y3[16], z3[16];
    mont_mul(M, x3, rr, rr); fp_sub(M, x3, x3, j); fp_sub(M, x3, x3, v); fp_sub(M, x3, x3, v);
    fp_sub(M, t, v, x3); mont_mul(M, y3, rr, t);
    mont_mul(M, t, s1, j); fp_add(M, t, t, t); fp_sub(M, y3, y3, t);
    fp_add(M, z3, p->z, q->z); mont_mul(M, z3, z3, z3); fp_sub(M, z3, z3, z1z1); fp_sub(M, z3, z3, z2z2);
    mont_mul(M, z3, z3, h);
    memcpy(r->x, x3, sizeof(x3)); memcpy(r->y, y3, sizeof(y3)); memcpy(r->z, z3, sizeof(z3));
    r->inf = 0;
}

/* обратное по модулю (Ферма: a^(m-2)), всё в форме Монтгомери */
static void mont_inv(const mont *M, u32 *r, const u32 *a, int bytes) {
    u8 e[64];
    u32 m2[BN], two[BN];
    memset(two, 0, sizeof(two));
    two[0] = 2;
    bn_sub(m2, M->m, two, M->n);
    bn_write(m2, M->n, e, bytes);
    mont_pow(M, r, a, e, bytes);
}

/* точка 04|X|Y -> якобиева (z = 1); проверить, что на кривой */
static int pt_load(const curve *c, jpoint *p, const u8 *pub, int len) {
    int b = c->bytes, n = c->p.n;
    if (len != 1 + 2 * b || pub[0] != 4) return -1;
    memset(p, 0, sizeof(*p));
    bn_read(p->x, n, pub + 1, b);
    bn_read(p->y, n, pub + 1 + b, b);
    if (bn_cmp(p->x, c->p.m, n) >= 0 || bn_cmp(p->y, c->p.m, n) >= 0) return -1;
    mont_to(&c->p, p->x, p->x);
    mont_to(&c->p, p->y, p->y);
    memcpy(p->z, c->p.one, n * 4);
    u32 l[16], r[16], t[16];                                       /* y^2 = x^3 - 3x + b */
    mont_mul(&c->p, l, p->y, p->y);
    mont_mul(&c->p, r, p->x, p->x);
    mont_mul(&c->p, r, r, p->x);
    fp_add(&c->p, t, p->x, p->x); fp_add(&c->p, t, t, p->x);
    fp_sub(&c->p, r, r, t);
    fp_add(&c->p, r, r, c->b);
    return bn_cmp(l, r, n) ? -1 : 0;
}

/* x аффинной точки (обычное число) */
static void pt_affine_x(const curve *c, const jpoint *p, u32 *x, u32 *y) {
    u32 zi[16], z2[16];
    mont_inv(&c->p, zi, p->z, c->bytes);
    mont_mul(&c->p, z2, zi, zi);
    mont_mul(&c->p, x, p->x, z2);
    mont_from(&c->p, x, x);
    if (y) { mont_mul(&c->p, z2, z2, zi); mont_mul(&c->p, y, p->y, z2); mont_from(&c->p, y, y); }
}

/* k1*G + k2*Q (k — обычные числа, n цифр); Q может быть NULL */
static void pt_mul2(const curve *c, jpoint *r, const u32 *k1, const u32 *k2, const jpoint *q) {
    jpoint g, gq;
    int n = c->p.n;
    memset(&g, 0, sizeof(g));
    memcpy(g.x, c->gx, n * 4); memcpy(g.y, c->gy, n * 4); memcpy(g.z, c->p.one, n * 4);
    if (q) pt_add(c, &gq, &g, q);
    r->inf = 1;
    for (int i = n * 32 - 1; i >= 0; i--) {
        pt_double(c, r, r);
        int b1 = (k1[i / 32] >> (i % 32)) & 1, b2 = q ? (k2[i / 32] >> (i % 32)) & 1 : 0;
        if (b1 && b2) pt_add(c, r, r, &gq);
        else if (b1) pt_add(c, r, r, &g);
        else if (b2) pt_add(c, r, r, q);
    }
}

/* ECDSA: подпись (r, s) хеша h ключом pub (04|X|Y) */
static int ecdsa_verify(int bytes, const u8 *pub, int plen, const u8 *h, int hl, const u8 *rb, int rl, const u8 *sb, int sl) {
    curve *c = get_curve(bytes);
    if (!c) return -1;
    const mont *N = &c->n;
    int n = N->n;
    jpoint q;
    if (pt_load(c, &q, pub, plen) < 0) return -1;
    u32 r[BN], s[BN], e[BN], w[BN], u1[BN], u2[BN];
    if (bn_read(r, n, rb, rl) < 0 || bn_read(s, n, sb, sl) < 0) return -1;
    if (bn_zero(r, n) || bn_zero(s, n) || bn_cmp(r, N->m, n) >= 0 || bn_cmp(s, N->m, n) >= 0) return -1;
    if (hl > bytes) hl = bytes;                                    /* хеш длиннее порядка — берём старшие биты */
    bn_read(e, n, h, hl);
    if (bn_cmp(e, N->m, n) >= 0) bn_sub(e, e, N->m, n);
    mont_to(N, s, s);
    mont_inv(N, w, s, bytes);                                      /* w = s^-1 (Монтгомери) */
    mont_to(N, e, e); mont_mul(N, u1, e, w); mont_from(N, u1, u1);
    u32 rm[BN];
    mont_to(N, rm, r); mont_mul(N, u2, rm, w); mont_from(N, u2, u2);
    jpoint x;
    pt_mul2(c, &x, u1, u2, &q);
    if (x.inf) return -1;
    u32 ax[16];
    pt_affine_x(c, &x, ax, NULL);
    if (bn_cmp(ax, N->m, n) >= 0) bn_sub(ax, ax, N->m, n);
    return bn_cmp(ax, r, n) ? -1 : 0;
}

int p256_keygen(const unsigned char priv[32], unsigned char pub[65]) {
    curve *c = get_curve(32);
    u32 k[BN];
    bn_read(k, 8, priv, 32);
    if (bn_zero(k, 8) || bn_cmp(k, c->n.m, 8) >= 0) return -1;
    jpoint r;
    pt_mul2(c, &r, k, k, NULL);
    u32 x[16], y[16];
    pt_affine_x(c, &r, x, y);
    pub[0] = 4;
    bn_write(x, 8, pub + 1, 32);
    bn_write(y, 8, pub + 33, 32);
    return 0;
}

int p256_shared(const unsigned char priv[32], const unsigned char peer[65], unsigned char out[32]) {
    curve *c = get_curve(32);
    jpoint q, r;
    if (pt_load(c, &q, peer, 65) < 0) return -1;
    u32 k[BN], zero[BN];
    bn_read(k, 8, priv, 32);
    memset(zero, 0, sizeof(zero));
    /* k*Q: тот же цикл, но без G */
    r.inf = 1;
    for (int i = 255; i >= 0; i--) {
        pt_double(c, &r, &r);
        if ((k[i / 32] >> (i % 32)) & 1) pt_add(c, &r, &r, &q);
    }
    if (r.inf) return -1;
    u32 x[16];
    pt_affine_x(c, &r, x, NULL);
    bn_write(x, 8, out, 32);
    (void)zero;
    return 0;
}

/* ====================================================================== ASN.1 DER */

typedef struct { const u8 *p; int len, tag; const u8 *start; int total; } asn;

/* следующий элемент из [*p, end); 0 — ок */
static int asn_next(const u8 **p, const u8 *end, asn *a) {
    const u8 *s = *p;
    if (end - s < 2) return -1;
    a->start = s;
    a->tag = *s++;
    if ((a->tag & 0x1F) == 0x1F) return -1;                        /* длинные теги не нужны */
    int l = *s++;
    if (l & 0x80) {
        int k = l & 0x7F;
        if (k < 1 || k > 3 || end - s < k) return -1;
        l = 0;
        while (k--) l = l << 8 | *s++;
    }
    if (l < 0 || end - s < l) return -1;
    a->p = s;
    a->len = l;
    a->total = (int)(s + l - a->start);
    *p = s + l;
    return 0;
}

static int asn_expect(const u8 **p, const u8 *end, asn *a, int tag) { return asn_next(p, end, a) || a->tag != tag ? -1 : 0; }

static int oid_is(const asn *a, const u8 *o, int ol) { return a->tag == 6 && a->len == ol && !memcmp(a->p, o, ol); }

#define OID(name, ...) static const u8 name[] = { __VA_ARGS__ }
OID(o_rsa,        0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01);
OID(o_rsa256,     0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B);
OID(o_rsa384,     0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C);
OID(o_rsa512,     0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D);
OID(o_ecpub,      0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01);
OID(o_ecdsa256,   0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02);
OID(o_ecdsa384,   0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03);
OID(o_ecdsa512,   0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04);
OID(o_p256,       0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07);
OID(o_p384,       0x2B, 0x81, 0x04, 0x00, 0x22);
OID(o_san,        0x55, 0x1D, 0x11);
OID(o_basic,      0x55, 0x1D, 0x13);
OID(o_cn,         0x55, 0x04, 0x03);
OID(o_org,        0x55, 0x04, 0x0A);

enum { K_NONE, K_RSA, K_EC256, K_EC384 };

typedef struct {
    const u8 *tbs; int tbslen;                                     /* что подписано (весь TBSCertificate) */
    int sigalg;                                                    /* 1..3 RSA SHA-256/384/512, 11..13 ECDSA */
    const u8 *sig; int siglen;
    const u8 *issuer; int ilen;                                    /* имена — как есть (DER) */
    const u8 *subject; int slen;
    char nb[16], na[16];                                           /* срок: "YYYYMMDDHHMMSS" */
    int key;                                                       /* K_... */
    const u8 *kn; int knlen; const u8 *ke; int kelen;              /* RSA: n, e */
    const u8 *kp; int kplen;                                       /* EC: 04|X|Y */
    const u8 *san; int sanlen;
    int ca, has_basic;
} cert;

static int sig_alg(const asn *a) {
    if (oid_is(a, o_rsa256, sizeof(o_rsa256))) return 1;
    if (oid_is(a, o_rsa384, sizeof(o_rsa384))) return 2;
    if (oid_is(a, o_rsa512, sizeof(o_rsa512))) return 3;
    if (oid_is(a, o_ecdsa256, sizeof(o_ecdsa256))) return 11;
    if (oid_is(a, o_ecdsa384, sizeof(o_ecdsa384))) return 12;
    if (oid_is(a, o_ecdsa512, sizeof(o_ecdsa512))) return 13;
    return 0;
}

static int get_time_str(const asn *a, char *out) {
    if (a->tag == 0x17 && a->len >= 12) {                          /* UTCTime YYMMDDHHMMSSZ */
        int yy = (a->p[0] - '0') * 10 + a->p[1] - '0';
        out[0] = yy < 50 ? '2' : '1'; out[1] = yy < 50 ? '0' : '9';
        memcpy(out + 2, a->p, 12);
    } else if (a->tag == 0x18 && a->len >= 14) memcpy(out, a->p, 14);   /* GeneralizedTime */
    else return -1;
    out[14] = 0;
    return 0;
}

static int cert_parse(const u8 *d, int len, cert *c) {
    memset(c, 0, sizeof(*c));
    const u8 *p = d, *end = d + len;
    asn crt, tbs, a, alg;
    if (asn_expect(&p, end, &crt, 0x30)) return -1;
    p = crt.p; end = crt.p + crt.len;
    if (asn_expect(&p, end, &tbs, 0x30)) return -1;
    c->tbs = tbs.start; c->tbslen = tbs.total;
    if (asn_expect(&p, end, &alg, 0x30)) return -1;                /* signatureAlgorithm */
    const u8 *q = alg.p;
    if (asn_next(&q, alg.p + alg.len, &a)) return -1;
    c->sigalg = sig_alg(&a);
    if (asn_expect(&p, end, &a, 0x03) || a.len < 1) return -1;     /* BIT STRING */
    c->sig = a.p + 1; c->siglen = a.len - 1;
    /* TBSCertificate */
    p = tbs.p; end = tbs.p + tbs.len;
    if (asn_next(&p, end, &a)) return -1;
    if (a.tag == 0xA0 && asn_next(&p, end, &a)) return -1;         /* [0] version — дальше serial */
    if (a.tag != 0x02) return -1;
    if (asn_expect(&p, end, &a, 0x30)) return -1;                  /* signature */
    if (asn_expect(&p, end, &a, 0x30)) return -1;                  /* issuer */
    c->issuer = a.start; c->ilen = a.total;
    if (asn_expect(&p, end, &a, 0x30)) return -1;                  /* validity */
    q = a.p;
    asn t1, t2;
    if (asn_next(&q, a.p + a.len, &t1) || asn_next(&q, a.p + a.len, &t2) || get_time_str(&t1, c->nb) || get_time_str(&t2, c->na)) return -1;
    if (asn_expect(&p, end, &a, 0x30)) return -1;                  /* subject */
    c->subject = a.start; c->slen = a.total;
    asn spki, kalg, kbits, ka, kp2;
    if (asn_expect(&p, end, &spki, 0x30)) return -1;
    q = spki.p;
    if (asn_expect(&q, spki.p + spki.len, &kalg, 0x30) || asn_expect(&q, spki.p + spki.len, &kbits, 0x03) || kbits.len < 1) return -1;
    const u8 *r = kalg.p;
    if (asn_next(&r, kalg.p + kalg.len, &ka)) return -1;
    if (oid_is(&ka, o_rsa, sizeof(o_rsa))) {
        const u8 *k = kbits.p + 1, *kend = kbits.p + kbits.len;
        asn ks, kn, ke;
        if (asn_expect(&k, kend, &ks, 0x30)) return -1;
        k = ks.p;
        if (asn_expect(&k, ks.p + ks.len, &kn, 2) || asn_expect(&k, ks.p + ks.len, &ke, 2)) return -1;
        c->key = K_RSA; c->kn = kn.p; c->knlen = kn.len; c->ke = ke.p; c->kelen = ke.len;
    } else if (oid_is(&ka, o_ecpub, sizeof(o_ecpub))) {
        if (asn_next(&r, kalg.p + kalg.len, &kp2)) return -1;
        if (oid_is(&kp2, o_p256, sizeof(o_p256))) c->key = K_EC256;
        else if (oid_is(&kp2, o_p384, sizeof(o_p384))) c->key = K_EC384;
        c->kp = kbits.p + 1; c->kplen = kbits.len - 1;
    }
    /* расширения: [3] { SEQUENCE { Extension... } } */
    while (p < end) {
        if (asn_next(&p, end, &a)) return -1;
        if (a.tag != 0xA3) continue;
        const u8 *e = a.p;
        asn exts, ext, id, v;
        if (asn_expect(&e, a.p + a.len, &exts, 0x30)) return -1;
        e = exts.p;
        while (e < exts.p + exts.len) {
            if (asn_expect(&e, exts.p + exts.len, &ext, 0x30)) return -1;
            const u8 *x = ext.p, *xend = ext.p + ext.len;
            if (asn_next(&x, xend, &id) || asn_next(&x, xend, &v)) return -1;
            if (v.tag == 0x01 && asn_next(&x, xend, &v)) return -1;   /* critical */
            if (v.tag != 0x04) continue;
            if (oid_is(&id, o_san, sizeof(o_san))) {
                const u8 *s = v.p;
                asn gn;
                if (!asn_expect(&s, v.p + v.len, &gn, 0x30)) { c->san = gn.p; c->sanlen = gn.len; }
            } else if (oid_is(&id, o_basic, sizeof(o_basic))) {
                const u8 *s = v.p;
                asn bc, ca;
                c->has_basic = 1;
                if (!asn_expect(&s, v.p + v.len, &bc, 0x30)) {
                    const u8 *b = bc.p;
                    if (bc.len && !asn_next(&b, bc.p + bc.len, &ca) && ca.tag == 0x01 && ca.len == 1 && ca.p[0]) c->ca = 1;
                }
            }
        }
    }
    return 0;
}

/* подпись сертификата c ключом issuer */
static int cert_check_sig(const cert *c, const cert *issuer) {
    int h = c->sigalg % 10;
    if (c->sigalg >= 1 && c->sigalg <= 3) {
        if (issuer->key != K_RSA) return -1;
        return rsa_pkcs1(issuer->kn, issuer->knlen, issuer->ke, issuer->kelen, c->sig, c->siglen, h, c->tbs, c->tbslen);
    }
    if (c->sigalg >= 11 && c->sigalg <= 13) {
        if (issuer->key != K_EC256 && issuer->key != K_EC384) return -1;
        u8 hash[64];
        int hl = hash_n(h, c->tbs, c->tbslen, hash);
        const u8 *p = c->sig, *end = c->sig + c->siglen;
        asn seq, r, s;
        if (asn_expect(&p, end, &seq, 0x30)) return -1;
        p = seq.p;
        if (asn_expect(&p, seq.p + seq.len, &r, 2) || asn_expect(&p, seq.p + seq.len, &s, 2)) return -1;
        return ecdsa_verify(issuer->key == K_EC256 ? 32 : 48, issuer->kp, issuer->kplen, hash, hl, r.p, r.len, s.p, s.len);
    }
    return -1;
}

/* ====================================================================== имя сайта */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int name_match(const u8 *pat, int pl, const char *host) {
    int hl = strlen(host);
    if (pl >= 3 && pat[0] == '*' && pat[1] == '.') {               /* *.example.com — ровно одна метка */
        const char *dot = strchr(host, '.');
        if (!dot || dot == host) return 0;
        int rest = hl - (int)(dot - host);
        if (rest != pl - 1) return 0;
        for (int i = 0; i < rest; i++) if (lower(dot[i]) != lower(pat[1 + i])) return 0;
        return 1;
    }
    if (pl != hl) return 0;
    for (int i = 0; i < hl; i++) if (lower(pat[i]) != lower(host[i])) return 0;
    return 1;
}

static int ip4(const char *h, u8 out[4]) {
    int v = 0, k = 0, digits = 0;
    for (;; h++) {
        if (*h >= '0' && *h <= '9') { v = v * 10 + *h - '0'; digits++; if (v > 255) return 0; continue; }
        if ((*h == '.' || !*h) && digits && k < 4) { out[k++] = v; v = 0; digits = 0; if (!*h) break; continue; }
        return 0;
    }
    return k == 4;
}

static int host_ok(const cert *c, const char *host) {
    u8 ip[4];
    int is_ip = ip4(host, ip), any_dns = 0;
    const u8 *p = c->san, *end = c->san + c->sanlen;
    asn g;
    while (p && p < end && !asn_next(&p, end, &g)) {
        if (g.tag == 0x82) { any_dns = 1; if (!is_ip && name_match(g.p, g.len, host)) return 1; }
        if (g.tag == 0x87 && is_ip && g.len == 4 && !memcmp(g.p, ip, 4)) return 1;
    }
    if (any_dns || is_ip) return 0;
    /* старые сертификаты без subjectAltName: CN в subject */
    p = c->subject; end = c->subject + c->slen;
    asn name, rdn, atv, oid, val;
    if (asn_expect(&p, end, &name, 0x30)) return 0;
    p = name.p;
    while (p < name.p + name.len && !asn_expect(&p, name.p + name.len, &rdn, 0x31)) {
        const u8 *r = rdn.p;
        while (r < rdn.p + rdn.len && !asn_expect(&r, rdn.p + rdn.len, &atv, 0x30)) {
            const u8 *v = atv.p;
            if (asn_next(&v, atv.p + atv.len, &oid) || asn_next(&v, atv.p + atv.len, &val)) break;
            if (oid_is(&oid, o_cn, sizeof(o_cn)) && name_match(val.p, val.len, host)) return 1;
        }
    }
    return 0;
}

/* человеку: «O=...» или «CN=...» из имени */
static void name_str(const u8 *n, int nl, char *out, int max) {
    const u8 *p = n, *end = n + nl;
    asn name, rdn, atv, oid, val;
    out[0] = 0;
    if (max < 2 || asn_expect(&p, end, &name, 0x30)) return;
    p = name.p;
    char cn[64] = "", org[64] = "";
    while (p < name.p + name.len && !asn_expect(&p, name.p + name.len, &rdn, 0x31)) {
        const u8 *r = rdn.p;
        while (r < rdn.p + rdn.len && !asn_expect(&r, rdn.p + rdn.len, &atv, 0x30)) {
            const u8 *v = atv.p;
            if (asn_next(&v, atv.p + atv.len, &oid) || asn_next(&v, atv.p + atv.len, &val)) break;
            char *dst = oid_is(&oid, o_cn, sizeof(o_cn)) ? cn : oid_is(&oid, o_org, sizeof(o_org)) ? org : NULL;
            if (!dst) continue;
            int k = val.len < 63 ? val.len : 63;
            for (int i = 0; i < k; i++) dst[i] = val.p[i] >= 32 && val.p[i] < 127 ? val.p[i] : '?';
            dst[k] = 0;
        }
    }
    snprintf(out, max, "%s", org[0] ? org : cn);
}

/* ====================================================================== корневые центры */

const char *x509_roots_file = "/etc/certs.der";
static u8 *roots;
static int roots_len = -1;
static cert *rc;
static int n_roots;

static void load_roots(void) {
    if (roots_len >= 0) return;
    roots_len = 0;
#ifdef TLS_HOST
    FILE *f = fopen(x509_roots_file, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    roots = malloc(sz + 1);
    if (!roots) { fclose(f); return; }
    roots_len = fread(roots, 1, sz, f);
    fclose(f);
#else
    int fd = open(x509_roots_file, O_READ);
    if (fd < 0) return;
    int cap = 256 * 1024, n;
    roots = malloc(cap);
    if (!roots) { close(fd); return; }
    while ((n = read(fd, roots + roots_len, cap - roots_len)) > 0) {
        roots_len += n;
        if (roots_len == cap) {
            u8 *b = realloc(roots, cap * 2);
            if (!b) break;
            roots = b; cap *= 2;
        }
    }
    close(fd);
#endif
    int cap2 = 64;
    rc = malloc(sizeof(cert) * cap2);
    for (int p = 0; rc && p + 4 < roots_len;) {
        const u8 *s = roots + p;
        asn a;
        if (asn_next(&s, roots + roots_len, &a) || a.tag != 0x30) break;
        if (n_roots == cap2) {
            cert *nc = realloc(rc, sizeof(cert) * cap2 * 2);
            if (!nc) break;
            rc = nc; cap2 *= 2;
        }
        if (!cert_parse(roots + p, a.total, &rc[n_roots])) n_roots++;
        p += a.total;
    }
}

/* ====================================================================== цепочка */

static void now_str(char *out) {
#ifdef TLS_HOST
    time_t t = time(NULL);
    struct tm *g = gmtime(&t);
    snprintf(out, 16, "%04d%02d%02d%02d%02d%02d", g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
#else
    struct datetime d;
    datetime_get(&d);
    snprintf(out, 16, "%04d%02d%02d%02d%02d%02d", d.year, d.month, d.day, d.hour, d.min, d.sec);
#endif
}

/* сравнить даты с запасом в сутки (часы компьютера — местные, а не UTC) */
static int date_cmp_slack(const char *a, const char *b, int slack_days) {
    int ya = (a[0] - '0') * 1000 + (a[1] - '0') * 100 + (a[2] - '0') * 10 + a[3] - '0';
    int yb = (b[0] - '0') * 1000 + (b[1] - '0') * 100 + (b[2] - '0') * 10 + b[3] - '0';
    int da = ya * 372 + ((a[4] - '0') * 10 + a[5] - '0') * 31 + (a[6] - '0') * 10 + a[7] - '0';
    int db = yb * 372 + ((b[4] - '0') * 10 + b[5] - '0') * 31 + (b[6] - '0') * 10 + b[7] - '0';
    if (da - db > slack_days) return 1;
    if (db - da > slack_days) return -1;
    return 0;
}

int x509_verify_chain(const unsigned char *const *certs, const int *lens, int n, const char *host,
                      char *err, int emax, char *issuer, int imax) {
    static cert chain[10];
    if (issuer && imax) issuer[0] = 0;
    if (n < 1) { snprintf(err, emax, "server sent no certificate"); return -1; }
    if (n > 10) n = 10;
    for (int i = 0; i < n; i++)
        if (cert_parse(certs[i], lens[i], &chain[i])) { snprintf(err, emax, "cannot read certificate %d", i + 1); return -1; }
    char now[16];
    now_str(now);
    const cert *leaf = &chain[0];
    if (!host_ok(leaf, host)) { snprintf(err, emax, "certificate is for another site, not %.40s", host); return -1; }
    load_roots();
    if (!n_roots) { snprintf(err, emax, "no root certificates (%s)", x509_roots_file); return -1; }
    const cert *cur = leaf;
    int used[10] = { 0 };
    for (int depth = 0; depth < 10; depth++) {
        if (date_cmp_slack(now, cur->nb, 1) < 0 || date_cmp_slack(now, cur->na, 1) > 0) {
            char who[64];
            name_str(cur->subject, cur->slen, who, sizeof(who));
            snprintf(err, emax, "certificate %s (%.40s) expired or not yet valid: %.8s..%.8s, now %.8s",
                     depth ? "of CA" : "of the site", who, cur->nb, cur->na, now);
            return -1;
        }
        if (depth && (!cur->has_basic || !cur->ca)) { snprintf(err, emax, "intermediate certificate is not a CA"); return -1; }
        /* выдан корнем, которому мы доверяем? */
        for (int r = 0; r < n_roots; r++) {
            const cert *root = &rc[r];
            if (root->slen != cur->ilen || memcmp(root->subject, cur->issuer, cur->ilen)) continue;
            if (cert_check_sig(cur, root)) continue;
            if (issuer) name_str(root->subject, root->slen, issuer, imax);
            return 0;
        }
        /* сам корень (сервер прислал и его) */
        for (int r = 0; r < n_roots; r++)
            if (rc[r].tbslen == cur->tbslen && !memcmp(rc[r].tbs, cur->tbs, cur->tbslen)) {
                if (issuer) name_str(rc[r].subject, rc[r].slen, issuer, imax);
                return 0;
            }
        /* промежуточный из присланных */
        int next = -1;
        for (int i = 1; i < n; i++) {
            if (used[i] || chain[i].slen != cur->ilen || memcmp(chain[i].subject, cur->issuer, cur->ilen)) continue;
            if (cert_check_sig(cur, &chain[i])) continue;
            next = i;
            break;
        }
        if (next < 0) {
            char who[64];
            name_str(cur->issuer, cur->ilen, who, sizeof(who));
            if (cur->sigalg == 0) snprintf(err, emax, "unsupported signature algorithm in the certificate chain");
            else snprintf(err, emax, "issuer is not trusted: %.60s", who[0] ? who : "?");
            return -1;
        }
        used[next] = 1;
        cur = &chain[next];
    }
    snprintf(err, emax, "certificate chain is too long");
    return -1;
}

int x509_verify_tls(const unsigned char *der, int clen, int scheme, const unsigned char *sig, int slen,
                    const unsigned char *msg, int mlen) {
    static cert c;
    if (cert_parse(der, clen, &c)) return -1;
    int h = 0;
    switch (scheme) {
    case 0x0403: case 0x0804: case 0x0401: h = 1; break;
    case 0x0503: case 0x0805: case 0x0501: h = 2; break;
    case 0x0603: case 0x0806: case 0x0601: h = 3; break;
    default: return -1;
    }
    if ((scheme & 0xFF) == 0x03) {                                 /* ECDSA */
        if ((scheme == 0x0403 && c.key != K_EC256) || (scheme == 0x0503 && c.key != K_EC384) || scheme == 0x0603) return -1;
        u8 hash[64];
        int hl = hash_n(h, msg, mlen, hash);
        const u8 *p = sig, *end = sig + slen;
        asn seq, r, s;
        if (asn_expect(&p, end, &seq, 0x30)) return -1;
        p = seq.p;
        if (asn_expect(&p, seq.p + seq.len, &r, 2) || asn_expect(&p, seq.p + seq.len, &s, 2)) return -1;
        return ecdsa_verify(c.key == K_EC256 ? 32 : 48, c.kp, c.kplen, hash, hl, r.p, r.len, s.p, s.len);
    }
    if (c.key != K_RSA) return -1;
    if ((scheme >> 8) == 0x08) return rsa_pss(c.kn, c.knlen, c.ke, c.kelen, sig, slen, h, msg, mlen);
    return rsa_pkcs1(c.kn, c.knlen, c.ke, c.kelen, sig, slen, h, msg, mlen);
}
