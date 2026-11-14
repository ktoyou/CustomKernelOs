/* tls.c — TLS 1.3 (RFC 8446), клиентская сторона, всё с нуля.
 *
 *   SHA-256, HMAC, HKDF          — хеш и вывод ключей
 *   X25519 (RFC 7748)            — общий секрет с сервером (по мотивам TweetNaCl)
 *   ChaCha20-Poly1305 (RFC 8439) — шифр записей (предпочитаем: быстрый без AES-NI)
 *   AES-128-GCM                  — второй шифр (если сервер не умеет ChaCha20)
 *
 * Как идёт рукопожатие:
 *   мы  -> ClientHello (наш открытый ключ X25519, имя сайта SNI)
 *   сервер -> ServerHello (его ключ) — дальше всё зашифровано
 *   сервер -> EncryptedExtensions, Certificate, CertificateVerify, Finished
 *   мы  -> Finished
 * Из общего секрета X25519 и хеша всех сообщений выводятся ключи (HKDF).
 * Finished сервера проверяется — значит, никто не подменил сообщения по пути.
 * Сертификат сервера проверяет x509.c (см. tls.h). */
#ifdef TLS_HOST                       /* сборка на Linux для тестов */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
static unsigned ticks(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static unsigned get_seed_extra(void) { return (unsigned)time(NULL) ^ (unsigned)getpid(); }
#else
#include "myos.h"
static unsigned get_seed_extra(void) {
    struct datetime d;
    datetime_get(&d);
    return ((d.year * 13 + d.month) * 32 + d.day) * 86400 + d.hour * 3600 + d.min * 60 + d.sec + getpid() * 7919;
}
#endif
#include "tls.h"
#include "x509.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put_be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put_le32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void wipe(void *p, int n) { volatile u8 *v = p; while (n--) *v++ = 0; }

/* ================================================================ SHA-256 */

typedef struct { u32 h[8]; u8 buf[64]; u32 n; u64 len; } sha256_ctx;

static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(sha256_ctx *c, const u8 *p) {
    u32 w[64], a, b, d, e, f, g, h, cc, t1, t2;
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c) {
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof(iv));
    c->n = 0;
    c->len = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, int n) {
    const u8 *p = data;
    c->len += n;
    while (n > 0) {
        if (c->n == 0 && n >= 64) { sha256_block(c, p); p += 64; n -= 64; continue; }
        int k = 64 - c->n;
        if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(sha256_ctx *c, u8 out[32]) {          /* c не портится — можно продолжать */
    sha256_ctx t = *c;
    u64 bits = t.len * 8;
    u8 pad = 0x80, zero = 0, lenb[8];
    sha256_update(&t, &pad, 1);
    while (t.n != 56) sha256_update(&t, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = bits >> (56 - 8 * i);
    sha256_update(&t, lenb, 8);
    for (int i = 0; i < 8; i++) put_be32(out + 4 * i, t.h[i]);
}

void sha256(const void *data, int n, unsigned char out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, out);
}

/* ================================================================ HMAC, HKDF */

void hmac_sha256(const void *key, int klen, const void *data, int n, unsigned char out[32]) {
    u8 k[64], pad[64], inner[32];
    memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k);
    else memcpy(k, key, klen);
    sha256_ctx c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, data, n); sha256_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

/* HKDF-Expand-Label(secret, "tls13 " + label, context) длиной len (len <= 255) */
static void hkdf_label(const u8 secret[32], const char *label, const u8 *ctx, int ctxlen, u8 *out, int len) {
    u8 info[2 + 1 + 64 + 1 + 64 + 1 + 32];
    int ll = strlen(label), n = 0;
    info[n++] = len >> 8; info[n++] = len;
    info[n++] = 6 + ll;
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = ctxlen;
    memcpy(info + n, ctx, ctxlen); n += ctxlen;
    /* T(i) = HMAC(secret, T(i-1) | info | i) */
    u8 t[32 + sizeof(info)], blk[32];
    int done = 0, tl = 0;
    for (u8 i = 1; done < len; i++) {
        memcpy(t + tl, info, n);
        t[tl + n] = i;
        hmac_sha256(secret, 32, t, tl + n + 1, blk);
        int k = len - done < 32 ? len - done : 32;
        memcpy(out + done, blk, k);
        done += k;
        memcpy(t, blk, 32);
        tl = 32;
    }
}

/* ================================================================ X25519 */

typedef i64 gf[16];

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (i64)1 << 16;
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int b) {
    i64 c = ~((i64)b - 1);
    for (int i = 0; i < 16; i++) { i64 t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack25519(u8 *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = t[i] & 0xff; o[2 * i + 1] = t[i] >> 8; }
}

static void unpack25519(gf o, const u8 *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fmul(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void finv(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) {
        fmul(c, c, c);
        if (a != 2 && a != 4) fmul(c, c, in);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(unsigned char q[32], const unsigned char n[32], const unsigned char p[32]) {
    static const gf k121665 = { 0xDB41, 1 };
    u8 z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fmul(d, e, e);
        fmul(f, a, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fmul(b, a, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fmul(b, e, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    finv(c, c);
    fmul(a, a, c);
    pack25519(q, a);
    wipe(z, 32);
}

/* ================================================================ ChaCha20 */

#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROL(d, 16); c += d; b ^= c; b = ROL(b, 12); \
    a += b; d ^= a; d = ROL(d, 8);  c += d; b ^= c; b = ROL(b, 7);

static void chacha_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]) {
    u32 s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    memcpy(x, s, sizeof(s));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + 4 * i, x[i] + s[i]);
}

static void chacha_xor(const u8 key[32], u32 counter, const u8 nonce[12], u8 *data, int n) {
    u8 ks[64];
    while (n > 0) {
        chacha_block(key, counter++, nonce, ks);
        int k = n < 64 ? n : 64;
        for (int i = 0; i < k; i++) data[i] ^= ks[i];
        data += k; n -= k;
    }
}

/* ================================================================ Poly1305 */

typedef struct { u32 r[5], h[5], pad[4]; } poly1305;

static void poly_init(poly1305 *p, const u8 key[32]) {
    p->r[0] = le32(key + 0) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + 4 * i);
}

static void poly_block(poly1305 *p, const u8 *m, u32 hibit) {
    u32 r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    h0 += le32(m + 0) & 0x3ffffff;
    h1 += (le32(m + 3) >> 2) & 0x3ffffff;
    h2 += (le32(m + 6) >> 4) & 0x3ffffff;
    h3 += (le32(m + 9) >> 6) & 0x3ffffff;
    h4 += (le32(m + 12) >> 8) | hibit;
    u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
    u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
    u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
    u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
    u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;
    u32 c;
    c = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffff; d1 += c;
    c = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffff; d2 += c;
    c = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffff; d3 += c;
    c = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffff; d4 += c;
    c = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

/* данные кусками, каждый дополняется нулями до 16 байт (так в AEAD RFC 8439) */
static void poly_padded(poly1305 *p, const u8 *m, int n) {
    while (n >= 16) { poly_block(p, m, 1 << 24); m += 16; n -= 16; }
    if (n > 0) {
        u8 b[16];
        memset(b, 0, 16);
        memcpy(b, m, n);
        poly_block(p, b, 1 << 24);
    }
}

static void poly_finish(poly1305 *p, u8 mac[16]) {
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    u32 g4 = h4 + c - (1 << 26);
    u32 mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);
    u64 f;
    f = (u64)h0 + p->pad[0];             put_le32(mac + 0, (u32)f);
    f = (u64)h1 + p->pad[1] + (f >> 32); put_le32(mac + 4, (u32)f);
    f = (u64)h2 + p->pad[2] + (f >> 32); put_le32(mac + 8, (u32)f);
    f = (u64)h3 + p->pad[3] + (f >> 32); put_le32(mac + 12, (u32)f);
}

/* ChaCha20-Poly1305: шифрует/расшифровывает data на месте, tag — 16 байт */
static void chapoly_tag(const u8 key[32], const u8 nonce[12], const u8 *aad, int alen,
                        const u8 *ct, int n, u8 tag[16]) {
    u8 otk[64], lens[16];
    chacha_block(key, 0, nonce, otk);
    poly1305 p;
    poly_init(&p, otk);
    poly_padded(&p, aad, alen);
    poly_padded(&p, ct, n);
    memset(lens, 0, 16);
    put_le32(lens, alen);
    put_le32(lens + 8, n);
    poly_block(&p, lens, 1 << 24);
    poly_finish(&p, tag);
}

/* ================================================================ AES-128-GCM */

static u8 sbox[256];
static u32 te0[256];

static void aes_tables(void) {
    if (sbox[0]) return;
    u8 p = 1, q = 1;
    do {                                       /* p пробегает всё поле, q = 1/p */
        p = p ^ (u8)(p << 1) ^ (p & 0x80 ? 0x1B : 0);
        q ^= q << 1; q ^= q << 2; q ^= q << 4;
        if (q & 0x80) q ^= 0x09;
        u8 x = q ^ (u8)((q << 1) | (q >> 7)) ^ (u8)((q << 2) | (q >> 6)) ^
               (u8)((q << 3) | (q >> 5)) ^ (u8)((q << 4) | (q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) {
        u32 s = sbox[i], s2 = (u8)((s << 1) ^ (s & 0x80 ? 0x1B : 0)), s3 = s2 ^ s;
        te0[i] = s2 << 24 | s << 16 | s << 8 | s3;
    }
}

typedef struct { u32 rk[44]; u64 hl[16], hh[16]; } aesgcm;

static void aes_encrypt(const u32 *rk, const u8 in[16], u8 out[16]) {
    u32 s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1], s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
    u32 t0, t1, t2, t3;
#define TE(a, b, c, d, k) (te0[(a) >> 24] ^ ROR(te0[((b) >> 16) & 0xff], 8) ^ \
                           ROR(te0[((c) >> 8) & 0xff], 16) ^ ROR(te0[(d) & 0xff], 24) ^ (k))
    for (int r = 1; r < 10; r++) {
        const u32 *k = rk + 4 * r;
        t0 = TE(s0, s1, s2, s3, k[0]);
        t1 = TE(s1, s2, s3, s0, k[1]);
        t2 = TE(s2, s3, s0, s1, k[2]);
        t3 = TE(s3, s0, s1, s2, k[3]);
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
#define SB(a, b, c, d, k) (((u32)sbox[(a) >> 24] << 24 | (u32)sbox[((b) >> 16) & 0xff] << 16 | \
                            (u32)sbox[((c) >> 8) & 0xff] << 8 | sbox[(d) & 0xff]) ^ (k))
    put_be32(out, SB(s0, s1, s2, s3, rk[40]));
    put_be32(out + 4, SB(s1, s2, s3, s0, rk[41]));
    put_be32(out + 8, SB(s2, s3, s0, s1, rk[42]));
    put_be32(out + 12, SB(s3, s0, s1, s2, rk[43]));
}

static void gcm_init(aesgcm *g, const u8 key[16]) {
    aes_tables();
    static const u8 rcon[10] = { 1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36 };
    for (int i = 0; i < 4; i++) g->rk[i] = be32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        u32 t = g->rk[i - 1];
        if ((i & 3) == 0) {
            t = (u32)sbox[(t >> 16) & 0xff] << 24 | (u32)sbox[(t >> 8) & 0xff] << 16 |
                (u32)sbox[t & 0xff] << 8 | sbox[t >> 24];
            t ^= (u32)rcon[i / 4 - 1] << 24;
        }
        g->rk[i] = g->rk[i - 4] ^ t;
    }
    /* таблица для умножения на H в GF(2^128) по 4 бита (метод Шоупа) */
    u8 h[16];
    memset(h, 0, 16);
    aes_encrypt(g->rk, h, h);
    u64 vh = (u64)be32(h) << 32 | be32(h + 4), vl = (u64)be32(h + 8) << 32 | be32(h + 12);
    g->hl[8] = vl; g->hh[8] = vh;
    g->hl[0] = g->hh[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        u32 t = (u32)(vl & 1) * 0xe1000000;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((u64)t << 32);
        g->hl[i] = vl; g->hh[i] = vh;
    }
    for (int i = 2; i <= 8; i *= 2)
        for (int j = 1; j < i; j++) {
            g->hh[i + j] = g->hh[i] ^ g->hh[j];
            g->hl[i + j] = g->hl[i] ^ g->hl[j];
        }
}

static void gcm_mult(const aesgcm *g, u8 x[16]) {
    static const u32 last4[16] = { 0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
                                   0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0 };
    int lo = x[15] & 0xf, hi, rem;
    u64 zh = g->hh[lo], zl = g->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 0xf;
        hi = x[i] >> 4;
        if (i != 15) {
            rem = (int)zl & 0xf;
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4) ^ ((u64)last4[rem] << 48);
            zh ^= g->hh[lo]; zl ^= g->hl[lo];
        }
        rem = (int)zl & 0xf;
        zl = (zh << 60) | (zl >> 4);
        zh = (zh >> 4) ^ ((u64)last4[rem] << 48);
        zh ^= g->hh[hi]; zl ^= g->hl[hi];
    }
    put_be32(x, zh >> 32); put_be32(x + 4, (u32)zh);
    put_be32(x + 8, zl >> 32); put_be32(x + 12, (u32)zl);
}

static void ghash(const aesgcm *g, u8 y[16], const u8 *d, int n) {
    while (n > 0) {
        int k = n < 16 ? n : 16;
        for (int i = 0; i < k; i++) y[i] ^= d[i];
        gcm_mult(g, y);
        d += k; n -= k;
    }
}

static void gcm_ctr(const aesgcm *g, const u8 iv[12], u8 *data, int n) {
    u8 ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    u32 c = 2;                                          /* 1 — для тега */
    while (n > 0) {
        put_be32(ctr + 12, c++);
        aes_encrypt(g->rk, ctr, ks);
        int k = n < 16 ? n : 16;
        for (int i = 0; i < k; i++) data[i] ^= ks[i];
        data += k; n -= k;
    }
}

static void gcm_tag(const aesgcm *g, const u8 iv[12], const u8 *aad, int alen, const u8 *ct, int n, u8 tag[16]) {
    u8 y[16], lens[16], j0[16];
    memset(y, 0, 16);
    ghash(g, y, aad, alen);
    ghash(g, y, ct, n);
    memset(lens, 0, 16);
    put_be32(lens + 4, alen * 8);
    put_be32(lens + 12, n * 8);
    ghash(g, y, lens, 16);
    memcpy(j0, iv, 12);
    put_be32(j0 + 12, 1);
    aes_encrypt(g->rk, j0, j0);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ j0[i];
}

/* ================================================================ случайные числа */

static u8 rng_state[32];
static int rng_ready;

static u64 rdtsc(void) {
#if defined(__i386__) || defined(__x86_64__)
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64)hi << 32 | lo;
#else
    return ticks();
#endif
}

/* Источник энтропии — счётчик тактов процессора (rdtsc), время, pid, адреса;
 * всё перемешивается SHA-256. Каждый вызов ещё и подмешивает свежий rdtsc. */
void random_bytes(void *out, int n) {
    u8 *o = out;
    if (!rng_ready) {
        sha256_ctx c;
        sha256_init(&c);
        for (int i = 0; i < 64; i++) {
            u64 t = rdtsc();
            unsigned tk = ticks(), ex = get_seed_extra();
            void *addr = &t;
            sha256_update(&c, &t, sizeof(t));
            sha256_update(&c, &tk, sizeof(tk));
            sha256_update(&c, &ex, sizeof(ex));
            sha256_update(&c, &addr, sizeof(addr));
            for (volatile int k = 0; k < (int)(t & 255); k++) ;      /* джиттер */
        }
        sha256_final(&c, rng_state);
        rng_ready = 1;
    }
    while (n > 0) {
        u8 buf[48], blk[32];
        u64 t = rdtsc();
        memcpy(buf, rng_state, 32);
        memcpy(buf + 32, &t, 8);
        buf[40] = 1;
        sha256(buf, 41, blk);                               /* выход */
        buf[40] = 2;
        sha256(buf, 41, rng_state);                         /* новое состояние */
        int k = n < 32 ? n : 32;
        memcpy(o, blk, k);
        o += k; n -= k;
    }
}

/* ================================================================ протокол TLS 1.3 */

#define REC_MAX (16384 + 256)
#define HS_MAX  (64 * 1024)

enum { SUITE_AES128 = 0x1301, SUITE_CHACHA = 0x1303 };

struct key {                                /* ключ одного направления */
    u8 secret[32], key[32], iv[12];
    u64 seq;
    aesgcm *gcm;
};

struct tls {
    int fd;
    int suite;
    int keys_on;                            /* записи уже шифруются */
    int eof;
    struct key rd, wr;
    u8 rec[5 + REC_MAX];                    /* последняя полученная запись */
    u8 *data; int dlen, dpos, dtype;        /* её расшифрованное содержимое */
    u8 *hs; int hslen;                      /* сообщения рукопожатия (могут быть разбиты на записи) */
    sha256_ctx th;                          /* хеш всех сообщений рукопожатия */
    char info[48];
    int cert_ok;                            /* 1 — сертификат проверен и годится */
    char cert_err[128];                     /* иначе — почему нет */
    char issuer[64];                        /* кто выдал (корневой центр) */
};

int tls_verify = 1;

static int seterr(char *err, int max, const char *s) {
    if (err && max > 0) {
        int n = strlen(s);
        if (n >= max) n = max - 1;
        memcpy(err, s, n);
        err[n] = 0;
    }
    return -1;
}

static void set_key(tls *t, struct key *k, const u8 secret[32]) {
    memcpy(k->secret, secret, 32);
    int klen = t->suite == SUITE_CHACHA ? 32 : 16;
    hkdf_label(secret, "key", 0, 0, k->key, klen);
    hkdf_label(secret, "iv", 0, 0, k->iv, 12);
    k->seq = 0;
    if (t->suite == SUITE_AES128) {
        if (!k->gcm) k->gcm = malloc(sizeof(aesgcm));
        gcm_init(k->gcm, k->key);
    }
}

static void make_nonce(struct key *k, u8 nonce[12]) {
    memcpy(nonce, k->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (u8)(k->seq >> (8 * i));
}

static int write_all(int fd, const u8 *p, int n) {
    while (n > 0) {
        int w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= w;
    }
    return 0;
}

int (*tls_idle)(void);

static int read_all(int fd, u8 *p, int n) {                /* 1 — прочитали, 0 — конец, -1 ошибка */
    int got = 0;
    while (got < n) {
        int r = read(fd, p + got, n - got);
        if (r == 0) return got ? -1 : 0;
        if (r < 0 && errno == ETIMEDOUT && tls_idle && tls_idle()) continue;   /* сокет с тайм-аутом: спросить программу */
        if (r < 0) return -1;
        got += r;
    }
    return 1;
}

/* отправить запись типа type (22 рукопожатие, 23 данные, 21 alert) */
static int send_record(tls *t, int type, const u8 *data, int n) {
    static u8 out[5 + 16384 + 1 + 16];
    while (n > 0 || type != 23) {
        int k = n > 16384 ? 16384 : n;
        out[0] = t->keys_on ? 23 : type;
        out[1] = 3; out[2] = t->keys_on ? 3 : 1;
        int len = t->keys_on ? k + 1 + 16 : k;
        out[3] = len >> 8; out[4] = len;
        memcpy(out + 5, data, k);
        if (t->keys_on) {
            u8 nonce[12];
            out[5 + k] = type;                              /* настоящий тип — внутри шифра */
            make_nonce(&t->wr, nonce);
            if (t->suite == SUITE_CHACHA) {
                chacha_xor(t->wr.key, 1, nonce, out + 5, k + 1);
                chapoly_tag(t->wr.key, nonce, out, 5, out + 5, k + 1, out + 5 + k + 1);
            } else {
                gcm_ctr(t->wr.gcm, nonce, out + 5, k + 1);
                gcm_tag(t->wr.gcm, nonce, out, 5, out + 5, k + 1, out + 5 + k + 1);
            }
            t->wr.seq++;
        }
        if (write_all(t->fd, out, 5 + len) < 0) return -1;
        data += k; n -= k;
        if (type != 23) break;
    }
    return 0;
}

/* получить следующую запись: тип в t->dtype, содержимое в t->data/dlen.
 * 0 — ок, 1 — соединение закрыто, -1 — ошибка (текст в err) */
static int recv_record(tls *t, char *err, int emax) {
    for (;;) {
        int r = read_all(t->fd, t->rec, 5);
        if (r == 0) return 1;
        if (r < 0) return seterr(err, emax, "connection broken");
        int type = t->rec[0], len = t->rec[3] << 8 | t->rec[4];
        if (len > REC_MAX) return seterr(err, emax, "record too big");
        if (read_all(t->fd, t->rec + 5, len) != 1) return seterr(err, emax, "connection broken");
        u8 *body = t->rec + 5;
        if (type == 20) continue;                           /* ChangeCipherSpec — для совместимости, пропускаем */
        if (!t->keys_on || type != 23) {
            if (t->keys_on && type != 21) return seterr(err, emax, "unencrypted record");
            t->dtype = type; t->data = body; t->dlen = len; t->dpos = 0;
            return 0;
        }
        if (len < 17) return seterr(err, emax, "bad record");
        int n = len - 16;
        u8 nonce[12], tag[16];
        make_nonce(&t->rd, nonce);
        if (t->suite == SUITE_CHACHA) {
            chapoly_tag(t->rd.key, nonce, t->rec, 5, body, n, tag);
            if (memcmp(tag, body + n, 16)) return seterr(err, emax, "bad record MAC (decryption failed)");
            chacha_xor(t->rd.key, 1, nonce, body, n);
        } else {
            gcm_tag(t->rd.gcm, nonce, t->rec, 5, body, n, tag);
            if (memcmp(tag, body + n, 16)) return seterr(err, emax, "bad record MAC (decryption failed)");
            gcm_ctr(t->rd.gcm, nonce, body, n);
        }
        t->rd.seq++;
        while (n > 0 && body[n - 1] == 0) n--;              /* убрать нули-заполнитель */
        if (n == 0) return seterr(err, emax, "empty record");
        t->dtype = body[n - 1];
        t->data = body; t->dlen = n - 1; t->dpos = 0;
        return 0;
    }
}

static const char *alert_name(int a) {
    switch (a) {
    case 0: return "close_notify";
    case 10: return "unexpected_message";
    case 20: return "bad_record_mac";
    case 40: return "handshake_failure";
    case 42: return "bad_certificate";
    case 47: return "illegal_parameter";
    case 50: return "decode_error";
    case 51: return "decrypt_error";
    case 70: return "protocol_version (server has no TLS 1.3)";
    case 71: return "insufficient_security";
    case 80: return "internal_error";
    case 109: return "missing_extension";
    case 110: return "unsupported_extension";
    case 112: return "unrecognized_name";
    case 120: return "no_application_protocol";
    }
    return "alert";
}

/* следующее сообщение рукопожатия: *type, *body, *len (до следующего вызова) */
static int next_hs(tls *t, int *type, u8 **body, int *len, int *consumed, char *err, int emax) {
    if (*consumed) {                                        /* выкинуть прошлое сообщение */
        memmove(t->hs, t->hs + *consumed, t->hslen - *consumed);
        t->hslen -= *consumed;
        *consumed = 0;
    }
    while (t->hslen < 4 || t->hslen < 4 + (t->hs[1] << 16 | t->hs[2] << 8 | t->hs[3])) {
        if (t->hslen >= 4 && 4 + (t->hs[1] << 16 | t->hs[2] << 8 | t->hs[3]) > HS_MAX)
            return seterr(err, emax, "handshake message too big");
        int r = recv_record(t, err, emax);
        if (r == 1) return seterr(err, emax, "server closed the connection");
        if (r < 0) return -1;
        if (t->dtype == 21) {
            char m[96];
            snprintf(m, sizeof(m), "server alert: %s", t->dlen >= 2 ? alert_name(t->data[1]) : "?");
            return seterr(err, emax, m);
        }
        if (t->dtype != 22) return seterr(err, emax, "unexpected record during handshake");
        if (t->hslen + t->dlen > HS_MAX) return seterr(err, emax, "handshake too big");
        memcpy(t->hs + t->hslen, t->data, t->dlen);
        t->hslen += t->dlen;
        t->dlen = 0;
    }
    *type = t->hs[0];
    *len = t->hs[1] << 16 | t->hs[2] << 8 | t->hs[3];
    *body = t->hs + 4;
    *consumed = 4 + *len;
    return 0;
}

static const u8 hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

static int is_ip(const char *h) {
    for (; *h; h++) if (!((*h >= '0' && *h <= '9') || *h == '.')) return 0;
    return 1;
}

#define P16(v) (b[n++] = (v) >> 8, b[n++] = (u8)(v))

/* group 0x001d — ключ X25519 (32 байта), 0x0017 — P-256 (65 байт, после HelloRetryRequest) */
static int client_hello(u8 *b, const char *host, const u8 rnd[64], int group, const u8 *pub, const u8 *cookie, int clen) {
    int n = 4;                                              /* место под заголовок сообщения */
    P16(0x0303);
    memcpy(b + n, rnd, 32); n += 32;                        /* client random */
    b[n++] = 32; memcpy(b + n, rnd + 32, 32); n += 32;      /* legacy session id (для «коробок» по пути) */
    P16(4); P16(SUITE_CHACHA); P16(SUITE_AES128);
    b[n++] = 1; b[n++] = 0;                                 /* без сжатия */
    int ext = n; n += 2;
    int hl = strlen(host);
    if (hl > 0 && hl < 256 && !is_ip(host)) {               /* server_name: имя сайта */
        P16(0); P16(hl + 5); P16(hl + 3); b[n++] = 0; P16(hl);
        memcpy(b + n, host, hl); n += hl;
    }
    P16(10); P16(6); P16(4); P16(0x001d); P16(0x0017);      /* supported_groups: x25519, P-256 */
    static const unsigned short sig[] = { 0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601 };
    int ns = sizeof(sig) / sizeof(sig[0]);
    P16(13); P16(2 + 2 * ns); P16(2 * ns);                  /* signature_algorithms */
    for (int i = 0; i < ns; i++) P16(sig[i]);
    P16(43); P16(3); b[n++] = 2; P16(0x0304);               /* supported_versions: только TLS 1.3 */
    P16(45); P16(2); b[n++] = 1; b[n++] = 1;                /* psk_key_exchange_modes */
    int kl = group == 0x0017 ? 65 : 32;
    P16(51); P16(kl + 6); P16(kl + 4); P16(group); P16(kl); /* key_share: наш открытый ключ */
    memcpy(b + n, pub, kl); n += kl;
    if (cookie && clen > 0 && clen < 600) { P16(44); P16(clen); memcpy(b + n, cookie, clen); n += clen; }   /* cookie из HRR */
    b[ext] = (n - ext - 2) >> 8; b[ext + 1] = n - ext - 2;
    b[0] = 1; b[1] = 0; b[2] = (n - 4) >> 8; b[3] = n - 4;
    return n;
}

/* разобрать ServerHello: шифр и ключ сервера */
/* -> 0 обычный ServerHello (ключ сервера в spub, длина *sklen), 1 — HelloRetryRequest
 * (*hrr_group — какую группу хочет сервер, cookie — что вернуть) */
static int server_hello(tls *t, const u8 *p, int len, u8 spub[65], int *sklen, int group, int *hrr_group,
                        u8 *cookie, int *clen, char *err, int emax) {
    if (len < 38) return seterr(err, emax, "short ServerHello");
    int hrr = !memcmp(p + 2, hrr_random, 32);
    int i = 34, sid = p[i++];
    i += sid;
    if (i + 3 > len) return seterr(err, emax, "bad ServerHello");
    t->suite = p[i] << 8 | p[i + 1];
    i += 3;
    if (t->suite != SUITE_CHACHA && t->suite != SUITE_AES128) return seterr(err, emax, "server chose an unknown cipher");
    int ver = 0, got_key = 0;
    if (i + 2 <= len) {
        int end = i + 2 + (p[i] << 8 | p[i + 1]);
        i += 2;
        if (end > len) return seterr(err, emax, "bad ServerHello extensions");
        while (i + 4 <= end) {
            int et = p[i] << 8 | p[i + 1], el = p[i + 2] << 8 | p[i + 3];
            const u8 *e = p + i + 4;
            if (i + 4 + el > end) return seterr(err, emax, "bad ServerHello extension");
            if (et == 43 && el == 2) ver = e[0] << 8 | e[1];
            if (et == 51 && hrr && el == 2) *hrr_group = e[0] << 8 | e[1];
            if (et == 44 && hrr && el >= 2 && el - 2 < 600) { *clen = e[0] << 8 | e[1]; if (*clen > el - 2) *clen = el - 2; memcpy(cookie, e + 2, *clen); }
            if (et == 51 && !hrr && el >= 4) {
                int g = e[0] << 8 | e[1], kl = e[2] << 8 | e[3];
                int want = group == 0x0017 ? 65 : 32;
                if (g != group || kl != want || el < 4 + want) return seterr(err, emax, "server sent an unsupported key");
                memcpy(spub, e + 4, want);
                *sklen = want;
                got_key = 1;
            }
            i += 4 + el;
        }
    }
    if (ver != 0x0304) return seterr(err, emax, "server does not speak TLS 1.3 (only TLS 1.2 or older)");
    if (hrr) return 1;
    if (!got_key) return seterr(err, emax, "server sent no key share");
    return 0;
}

static void transcript(tls *t, u8 h[32]) { sha256_final(&t->th, h); }

tls *tls_open(int fd, const char *host, char *err, int emax) {
    u8 *certmsg = NULL;                                         /* копия сообщения Certificate */
    int certlen = 0;
    tls *t = calloc(1, sizeof(tls));
    u8 *hello = malloc(2048);
    if (t) t->hs = malloc(HS_MAX);
    if (!t || !hello || !t->hs) { seterr(err, emax, "out of memory"); goto fail; }
    t->fd = fd;
    sha256_init(&t->th);

    u8 priv[32], pub[65], spub[65], shared[32], rnd[64], cookie[600];
    int sklen = 0, hrr_group = 0, clen = 0, group = 0x001d;
    static const u8 nine[32] = { 9 };
    random_bytes(rnd, 64);
    random_bytes(priv, 32);
    x25519(pub, priv, nine);
    int hn = client_hello(hello, host, rnd, group, pub, NULL, 0);
    sha256_update(&t->th, hello, hn);
    if (send_record(t, 22, hello, hn) < 0) { seterr(err, emax, "cannot send"); goto fail; }

    int type, len, used = 0;
    u8 *m;
    if (next_hs(t, &type, &m, &len, &used, err, emax) < 0) goto fail;
    if (type != 2) { seterr(err, emax, "expected ServerHello"); goto fail; }
    int sh = server_hello(t, m, len, spub, &sklen, group, &hrr_group, cookie, &clen, err, emax);
    if (sh < 0) goto fail;
    if (sh == 1) {                                              /* HelloRetryRequest: серверу нужен P-256 */
        if (hrr_group != 0x0017) { seterr(err, emax, "server wants a key type we do not have"); goto fail; }
        u8 h1[32], mh[36] = { 254, 0, 0, 32 };
        transcript(t, h1);                                      /* хеш первого ClientHello -> message_hash */
        memcpy(mh + 4, h1, 32);
        sha256_init(&t->th);
        sha256_update(&t->th, mh, 36);
        sha256_update(&t->th, t->hs, used);                     /* сам HelloRetryRequest */
        if (t->hslen > used) { seterr(err, emax, "unexpected data after HelloRetryRequest"); goto fail; }
        t->hslen = 0; used = 0;
        group = 0x0017;
        random_bytes(priv, 32);
        priv[0] &= 0x7F;                                        /* меньше порядка группы */
        if (p256_keygen(priv, pub) < 0) { seterr(err, emax, "P-256 key error"); goto fail; }
        hn = client_hello(hello, host, rnd, group, pub, clen ? cookie : NULL, clen);
        sha256_update(&t->th, hello, hn);
        if (send_record(t, 22, hello, hn) < 0) { seterr(err, emax, "cannot send"); goto fail; }
        if (next_hs(t, &type, &m, &len, &used, err, emax) < 0) goto fail;
        if (type != 2) { seterr(err, emax, "expected ServerHello"); goto fail; }
        sh = server_hello(t, m, len, spub, &sklen, group, &hrr_group, cookie, &clen, err, emax);
        if (sh != 0) { if (sh > 0) seterr(err, emax, "second HelloRetryRequest"); goto fail; }
    }
    sha256_update(&t->th, t->hs, used);

    /* ключи рукопожатия */
    if (group == 0x0017) {
        if (p256_shared(priv, spub, shared) < 0) { seterr(err, emax, "bad server key"); goto fail; }
    } else x25519(shared, priv, spub);
    wipe(priv, 32);
    u8 zero[32], secret[32], derived[32], h[32], empty_h[32], c_hs[32], s_hs[32], fk[32], vd[32];
    int all0 = 1;
    for (int i = 0; i < 32; i++) if (shared[i]) all0 = 0;
    if (all0) { seterr(err, emax, "bad server key"); goto fail; }
    memset(zero, 0, 32);
    sha256("", 0, empty_h);
    hmac_sha256(zero, 32, zero, 32, secret);                    /* early secret (без PSK) */
    hkdf_label(secret, "derived", empty_h, 32, derived, 32);
    hmac_sha256(derived, 32, shared, 32, secret);               /* handshake secret */
    wipe(shared, 32);
    transcript(t, h);
    hkdf_label(secret, "c hs traffic", h, 32, c_hs, 32);
    hkdf_label(secret, "s hs traffic", h, 32, s_hs, 32);
    hkdf_label(secret, "derived", empty_h, 32, derived, 32);
    hmac_sha256(derived, 32, zero, 32, secret);                 /* master secret */
    set_key(t, &t->rd, s_hs);
    set_key(t, &t->wr, c_hs);
    t->keys_on = 1;
    /* после ServerHello в буфере не должно остаться открытых данных */
    if (t->hslen > used) { seterr(err, emax, "unexpected data after ServerHello"); goto fail; }
    t->hslen = 0; used = 0;

    int cert_req = 0, cv_ok = -1;
    for (;;) {
        if (next_hs(t, &type, &m, &len, &used, err, emax) < 0) goto fail;
        if (type == 11 && !certmsg && tls_verify) {              /* сертификаты сервера — проверим после Finished */
            certmsg = malloc(len > 0 ? len : 1);
            if (certmsg) { memcpy(certmsg, m, len); certlen = len; }
        }
        if (type == 15 && certmsg && tls_verify) {               /* CertificateVerify: подпись ключом сайта */
            u8 content[64 + 34 + 32];
            memset(content, 0x20, 64);
            memcpy(content + 64, "TLS 1.3, server CertificateVerify", 34);   /* с нулём в конце */
            transcript(t, content + 98);
            int scheme = len >= 4 ? m[0] << 8 | m[1] : 0, sl = len >= 4 ? m[2] << 8 | m[3] : 0;
            const u8 *leaf = NULL;
            int leaflen = 0;
            if (certlen >= 7) {                                 /* context, длина списка, первый сертификат */
                int ctx = certmsg[0];
                if (1 + ctx + 6 <= certlen) {
                    leaflen = certmsg[1 + ctx + 3] << 16 | certmsg[1 + ctx + 4] << 8 | certmsg[1 + ctx + 5];
                    leaf = certmsg + 1 + ctx + 6;
                    if (leaf + leaflen > certmsg + certlen) leaf = NULL;
                }
            }
            cv_ok = leaf && sl <= len - 4 ? x509_verify_tls(leaf, leaflen, scheme, m + 4, sl, content, sizeof(content)) == 0 : 0;
        }
        if (type == 20) {                                       /* Finished сервера */
            transcript(t, h);
            hkdf_label(s_hs, "finished", 0, 0, fk, 32);
            hmac_sha256(fk, 32, h, 32, vd);
            if (len != 32 || memcmp(vd, m, 32)) { seterr(err, emax, "server Finished is wrong"); goto fail; }
            sha256_update(&t->th, t->hs, used);
            break;
        }
        if (type == 13) cert_req = 1;                           /* серверу нужен наш сертификат — отдадим пустой */
        else if (type != 8 && type != 11 && type != 15) { seterr(err, emax, "unexpected handshake message"); goto fail; }
        sha256_update(&t->th, t->hs, used);
    }
    if (t->hslen > used) { seterr(err, emax, "unexpected data after Finished"); goto fail; }
    t->hslen = 0;

    /* сертификаты: цепочка до корня, имя сайта, сроки */
    if (!tls_verify) snprintf(t->cert_err, sizeof(t->cert_err), "certificate not checked");
    else if (!certmsg) snprintf(t->cert_err, sizeof(t->cert_err), "server sent no certificate");
    else if (cv_ok != 1) snprintf(t->cert_err, sizeof(t->cert_err), "server signature (CertificateVerify) is wrong");
    else {
        const u8 *cs[10];
        int cl[10], nc = 0;
        int ctx = certmsg[0], i = 1 + ctx + 3;
        while (i + 3 <= certlen && nc < 10) {
            int l = certmsg[i] << 16 | certmsg[i + 1] << 8 | certmsg[i + 2];
            if (i + 3 + l + 2 > certlen) break;
            cs[nc] = certmsg + i + 3; cl[nc] = l; nc++;
            i += 3 + l;
            i += 2 + (certmsg[i] << 8 | certmsg[i + 1]);         /* расширения записи */
        }
        if (x509_verify_chain(cs, cl, nc, host, t->cert_err, sizeof(t->cert_err), t->issuer, sizeof(t->issuer)) == 0) {
            t->cert_ok = 1;
            t->cert_err[0] = 0;
        }
    }
    free(certmsg);
    certmsg = NULL;

    /* ключи данных — от хеша по Finished сервера включительно */
    u8 c_ap[32], s_ap[32];
    transcript(t, h);
    hkdf_label(secret, "c ap traffic", h, 32, c_ap, 32);
    hkdf_label(secret, "s ap traffic", h, 32, s_ap, 32);
    set_key(t, &t->rd, s_ap);

    static const u8 ccs[6] = { 20, 3, 3, 0, 1, 1 };
    write_all(fd, ccs, 6);                                      /* «ChangeCipherSpec» для совместимости */
    if (cert_req) {
        static const u8 empty_cert[8] = { 11, 0, 0, 4, 0, 0, 0, 0 };
        sha256_update(&t->th, empty_cert, 8);
        send_record(t, 22, empty_cert, 8);
    }
    u8 fin[36] = { 20, 0, 0, 32 };
    transcript(t, h);
    hkdf_label(c_hs, "finished", 0, 0, fk, 32);
    hmac_sha256(fk, 32, h, 32, fin + 4);
    if (send_record(t, 22, fin, 36) < 0) { seterr(err, emax, "cannot send"); goto fail; }
    set_key(t, &t->wr, c_ap);
    wipe(secret, 32); wipe(c_hs, 32); wipe(s_hs, 32); wipe(c_ap, 32); wipe(s_ap, 32);
    free(hello);
    free(t->hs);
    t->hs = 0;
    t->dlen = 0;
    snprintf(t->info, sizeof(t->info), "TLS 1.3, %s", t->suite == SUITE_CHACHA ? "ChaCha20-Poly1305" : "AES-128-GCM");
    return t;
fail:
    free(certmsg);
    if (t) {
        if (t->keys_on) { static const u8 a[2] = { 2, 40 }; send_record(t, 21, a, 2); }
        free(t->hs);
        free(t->rd.gcm);
        free(t->wr.gcm);
        free(t);
    }
    free(hello);
    return 0;
}

const char *tls_info(tls *t) { return t->info; }
int tls_cert_ok(tls *t) { return t->cert_ok; }
const char *tls_cert_error(tls *t) { return t->cert_err; }
const char *tls_cert_issuer(tls *t) { return t->issuer; }

int tls_read(tls *t, void *buf, int len) {
    while (!t->eof) {
        if (t->dtype == 23 && t->dpos < t->dlen) {
            int k = t->dlen - t->dpos;
            if (k > len) k = len;
            memcpy(buf, t->data + t->dpos, k);
            t->dpos += k;
            return k;
        }
        int r = recv_record(t, 0, 0);
        if (r == 1) { t->eof = 1; return 0; }                   /* закрыли без close_notify — тоже конец */
        if (r < 0) return -1;
        if (t->dtype == 21) {                                   /* alert */
            t->eof = 1;
            return t->dlen >= 2 && t->data[1] == 0 ? 0 : -1;
        }
        if (t->dtype == 22) {                                   /* после рукопожатия: билеты сессий, KeyUpdate */
            int i = 0;
            while (i + 4 <= t->dlen) {
                int ty = t->data[i], l = t->data[i + 1] << 16 | t->data[i + 2] << 8 | t->data[i + 3];
                if (ty == 24) {                                 /* KeyUpdate: новый ключ чтения */
                    u8 ns[32];
                    hkdf_label(t->rd.secret, "traffic upd", 0, 0, ns, 32);
                    set_key(t, &t->rd, ns);
                    if (i + 4 < t->dlen && t->data[i + 4] == 1) {   /* просят обновить и наш */
                        static const u8 ku[5] = { 24, 0, 0, 1, 0 };
                        send_record(t, 22, ku, 5);
                        hkdf_label(t->wr.secret, "traffic upd", 0, 0, ns, 32);
                        set_key(t, &t->wr, ns);
                    }
                }
                i += 4 + l;
            }
            t->dlen = 0;
        }
    }
    return 0;
}

int tls_write(tls *t, const void *buf, int len) {
    return send_record(t, 23, buf, len) < 0 ? -1 : len;
}

void tls_close(tls *t) {
    if (!t) return;
    static const u8 a[2] = { 1, 0 };                            /* close_notify */
    if (!t->eof) send_record(t, 21, a, 2);
    free(t->rd.gcm);
    free(t->wr.gcm);
    wipe(t, sizeof(*t));
    free(t);
}
