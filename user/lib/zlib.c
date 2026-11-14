/* zlib.c — deflate/inflate для libz.
 *
 * Упаковка — LZ77 и фиксированные коды Хаффмана из gzip (если поток не стал
 * короче, пишется stored-блок). Распаковка потоковая, с окном 32 КиБ, как у tar:
 * динамические, фиксированные и несжатые блоки. Поверх — обёртки zlib и gzip.
 * Компрессор собирает весь вход до Z_FINISH и выдаёт один поток. Так устроен
 * и прежний gzip: файл целиком уже был в памяти. */
#ifdef IMG_HOST
#include <stdlib.h>
#include <string.h>
#include "../include/zlib.h"
#else
#include "myos.h"
#include "zlib.h"
#endif

#define WSIZE     32768
#define HASH_SIZE 32768
#define MAX_MATCH 258

#define PH_HEAD   1
#define PH_BLOCK  2
#define PH_STORED 3
#define PH_DYN    4
#define PH_CODES  5
#define PH_MATCH  6
#define PH_CHECK  7
#define PH_DONE   8
#define PH_BAD    9

struct internal_state {
    int kind;                 /* 1 — deflate, 2 — inflate */
    int wrap;                 /* 0 raw, 1 zlib, 2 gzip, 3 ещё не выбрано */
    int level;
    int phase, sub, bfinal, btype;
    int sym, length, dist, match_left, stored_left;
    int nlit, ndist, nclen, i, what, rep_left;
    unsigned acc;
    int bits;
    unsigned char window[WSIZE];
    unsigned wpos, s1, s2, crc;
    unsigned char hdr[10];
    int skip;
    short lcount[16], lsym[288];
    short dcount[16], dsym[32];
    unsigned char lens[320];
    unsigned char clen[19];
    const char *msg;
    unsigned char *inbuf;
    unsigned in_len, in_cap;
    unsigned char *outbuf;
    unsigned out_len, out_pos;
    int compressed;
    int gzip_os;
};

static const unsigned short len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const unsigned char clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static unsigned crc_table[256];
static int crc_ready;

static void crc_init(void) {
    unsigned i, c, bit;
    if (crc_ready) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (bit = 0; bit < 8; bit++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

const char *zlibVersion(void) { return ZLIB_VERSION; }

uLong crc32_z(uLong crc, const Bytef *buf, uLong len) {
    if (!buf) return 0;
    crc_init();
    crc ^= 0xffffffffUL;
    while (len--) crc = crc_table[(crc ^ *buf++) & 255] ^ (crc >> 8);
    return crc ^ 0xffffffffUL;
}

uLong crc32(uLong crc, const Bytef *buf, uInt len) {
    return crc32_z(crc, buf, len);
}

uLong adler32_z(uLong adler, const Bytef *buf, uLong len) {
    unsigned s1, s2;
    if (!buf) return 1;
    s1 = (unsigned)(adler & 0xffffu);
    s2 = (unsigned)((adler >> 16) & 0xffffu);
    while (len--) {
        s1 += *buf++;
        if (s1 >= 65521u) s1 -= 65521u;
        s2 += s1;
        if (s2 >= 65521u) s2 -= 65521u;
    }
    return ((uLong)s2 << 16) | s1;
}

uLong adler32(uLong adler, const Bytef *buf, uInt len) {
    return adler32_z(adler, buf, len);
}

static int version_ok(const char *version, int stream_size) {
    if (!version || version[0] != ZLIB_VERSION[0]) return 0;
    return stream_size == (int)sizeof(z_stream);
}

static void *z_alloc(z_streamp s, unsigned n) {
    if (!n) return 0;
    if (s->zalloc) return s->zalloc(s->opaque, 1, n);
    return malloc(n);
}

static void z_free(z_streamp s, void *p) {
    if (!p) return;
    if (s->zfree) s->zfree(s->opaque, p);
    else free(p);
}

static int alloc_pair_ok(z_streamp s) {
    if (!s->zalloc && !s->zfree) return 1;
    if (s->zalloc && s->zfree) return 1;
    return 0;
}

static void free_bufs(struct internal_state *st) {
    free(st->inbuf);
    free(st->outbuf);
    st->inbuf = 0;
    st->outbuf = 0;
    st->in_len = st->in_cap = 0;
    st->out_len = st->out_pos = 0;
    st->compressed = 0;
}

static int parse_window(int windowBits, int *wrap) {
    *wrap = 1;
    if (windowBits < 0) {
        *wrap = 0;
        windowBits = -windowBits;
    } else if (windowBits > 15) {
        *wrap = 2;
        windowBits -= 16;
        if (windowBits > 15) {
            *wrap = 3;
            windowBits -= 16;
        }
    }
    if (windowBits < 8 || windowBits > 15) return -1;
    return windowBits;
}

static void inflate_clear(struct internal_state *st) {
    int kind = st->kind, wrap = st->wrap, level = st->level;
    free_bufs(st);
    memset(st, 0, sizeof *st);
    st->kind = kind;
    st->wrap = wrap;
    st->level = level;
    st->s1 = 1;
    st->sym = -1;
    st->phase = wrap == 0 ? PH_BLOCK : PH_HEAD;
}

/* ------------------------------------------------------- биты inflate */

static int pull_byte(struct internal_state *st, z_streamp strm) {
    unsigned char c;
    if (st->bits > 24) return -1;
    if (!strm->avail_in || !strm->next_in) return 0;
    c = *strm->next_in++;
    strm->avail_in--;
    strm->total_in++;
    st->acc |= (unsigned)c << st->bits;
    st->bits += 8;
    return 1;
}

static int take_bits(struct internal_state *st, z_streamp strm, int n, int *out) {
    int pr;
    if (n == 0) { *out = 0; return 1; }
    if (n < 0 || n > 16) return -1;
    while (st->bits < n) {
        pr = pull_byte(st, strm);
        if (pr <= 0) return pr;
    }
    *out = (int)(st->acc & ((1u << n) - 1u));
    st->acc >>= n;
    st->bits -= n;
    return 1;
}

static int need_bits(struct internal_state *st, z_streamp strm, int n) {
    int pr;
    while (st->bits < n) {
        pr = pull_byte(st, strm);
        if (pr <= 0) return pr;
    }
    return 1;
}

static int take_byte(struct internal_state *st, z_streamp strm, int *out) {
    if (st->bits >= 8) {
        *out = (int)(st->acc & 255u);
        st->acc >>= 8;
        st->bits -= 8;
        return 1;
    }
    if (!strm->avail_in || !strm->next_in) return 0;
    *out = *strm->next_in++;
    strm->avail_in--;
    strm->total_in++;
    return 1;
}

static void drop_to_byte(struct internal_state *st) {
    int rem = st->bits & 7;
    if (rem) {
        st->acc >>= rem;
        st->bits -= rem;
    }
}

/* Целые байты, захваченные сверх конца блока, возвращаются во вход. */
static int rewind_full_bytes(struct internal_state *st, z_streamp strm) {
    while (st->bits >= 8) {
        unsigned keep = (unsigned)st->bits - 8;
        unsigned char byte = (unsigned char)((st->acc >> keep) & 255u);
        if (!strm->total_in || !strm->next_in) return -1;
        if (strm->next_in[-1] != byte) return -1;
        if (keep == 0) st->acc = 0;
        else st->acc &= (1u << keep) - 1u;
        st->bits -= 8;
        strm->next_in--;
        strm->avail_in++;
        strm->total_in--;
    }
    return 0;
}

static int construct(short *count, short *symbol, const unsigned char *length, int n) {
    int len, sym, left;
    short offs[16];
    for (len = 0; len <= 15; len++) count[len] = 0;
    for (sym = 0; sym < n; sym++) {
        if (length[sym] > 15) return -1;
        count[length[sym]]++;
    }
    if (count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym]) symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int decode_sym(struct internal_state *st, z_streamp strm,
                      const short *count, const short *symbol, int nsym, int *sym) {
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= 15; len++) {
        int pr = need_bits(st, strm, len);
        int bit, count_len, pos;
        if (pr <= 0) return pr;
        bit = (int)((st->acc >> (len - 1)) & 1u);
        code |= bit;
        count_len = count[len];
        if (code - count_len < first) {
            pos = index + (code - first);
            if (pos < 0 || pos >= nsym) return -1;
            st->acc >>= len;
            st->bits -= len;
            *sym = symbol[pos];
            return 1;
        }
        index += count_len;
        first += count_len;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static short fix_lcount[16], fix_lsym[288], fix_dcount[16], fix_dsym[32];
static int fix_ready;

static int build_fixed(void) {
    unsigned char fl[288], fd[32];
    int i;
    if (fix_ready) return 0;
    for (i = 0; i < 144; i++) fl[i] = 8;
    for (; i < 256; i++) fl[i] = 9;
    for (; i < 280; i++) fl[i] = 7;
    for (; i < 288; i++) fl[i] = 8;
    for (i = 0; i < 32; i++) fd[i] = 5;
    if (construct(fix_lcount, fix_lsym, fl, 288) < 0) return -1;
    if (construct(fix_dcount, fix_dsym, fd, 32) < 0) return -1;
    fix_ready = 1;
    return 0;
}

static int emit_byte(struct internal_state *st, z_streamp strm, unsigned char c) {
    Bytef b;
    if (!strm->avail_out || !strm->next_out) return 0;
    st->window[st->wpos & (WSIZE - 1)] = c;
    st->wpos++;
    *strm->next_out++ = c;
    strm->avail_out--;
    strm->total_out++;
    st->s1 += c;
    if (st->s1 >= 65521u) st->s1 -= 65521u;
    st->s2 += st->s1;
    if (st->s2 >= 65521u) st->s2 -= 65521u;
    b = c;
    st->crc = (unsigned)crc32(st->crc, &b, 1);
    if (st->wrap == 2) strm->adler = st->crc;
    else strm->adler = ((uLong)st->s2 << 16) | st->s1;
    return 1;
}

static int finish_stream(struct internal_state *st, z_streamp strm) {
    drop_to_byte(st);
    if (rewind_full_bytes(st, strm) < 0) {
        st->msg = "invalid bit buffer";
        return -1;
    }
    st->sub = 0;
    st->i = 0;
    st->sym = -1;
    if (st->wrap == 0) st->phase = PH_DONE;
    else st->phase = PH_CHECK;
    return 0;
}

static int end_block(struct internal_state *st, z_streamp strm) {
    st->sub = 0;
    st->sym = -1;
    if (!st->bfinal) {
        st->phase = PH_BLOCK;
        return 0;
    }
    return finish_stream(st, strm);
}

static int gzip_header_byte(struct internal_state *st, int c) {
    if (st->sub < 10) {
        st->hdr[st->sub++] = (unsigned char)c;
        if (st->sub < 10) return 0;
        if (st->hdr[0] != 31 || st->hdr[1] != 139 || st->hdr[2] != 8 || (st->hdr[3] & 0xe0))
            return -1;
        if (st->hdr[3] & 4) { st->sub = 10; st->i = 0; return 0; }
        if (st->hdr[3] & 8) { st->sub = 11; return 0; }
        if (st->hdr[3] & 16) { st->sub = 12; return 0; }
        if (st->hdr[3] & 2) { st->sub = 13; st->i = 0; return 0; }
        st->phase = PH_BLOCK;
        st->sub = 0;
        st->sym = -1;
        return 0;
    }
    if (st->sub == 10) {
        if (st->i == 0) { st->skip = c; st->i = 1; return 0; }
        st->skip |= c << 8;
        st->i = 0;
        st->sub = st->skip ? 14 : 0;
        if (st->sub == 0) {
            /* длина extra нулевая — дальше как после extra */
            if (st->hdr[3] & 8) st->sub = 11;
            else if (st->hdr[3] & 16) st->sub = 12;
            else if (st->hdr[3] & 2) { st->sub = 13; st->i = 0; }
            else { st->phase = PH_BLOCK; st->sub = 0; st->sym = -1; }
        }
        return 0;
    }
    if (st->sub == 14) {
        if (!st->skip) return -1;
        st->skip--;
        if (st->skip) return 0;
        if (st->hdr[3] & 8) { st->sub = 11; return 0; }
        if (st->hdr[3] & 16) { st->sub = 12; return 0; }
        if (st->hdr[3] & 2) { st->sub = 13; st->i = 0; return 0; }
        st->phase = PH_BLOCK;
        st->sub = 0;
        st->sym = -1;
        return 0;
    }
    if (st->sub == 11) {
        if (c) return 0;
        if (st->hdr[3] & 16) { st->sub = 12; return 0; }
        if (st->hdr[3] & 2) { st->sub = 13; st->i = 0; return 0; }
        st->phase = PH_BLOCK;
        st->sub = 0;
        st->sym = -1;
        return 0;
    }
    if (st->sub == 12) {
        if (c) return 0;
        if (st->hdr[3] & 2) { st->sub = 13; st->i = 0; return 0; }
        st->phase = PH_BLOCK;
        st->sub = 0;
        st->sym = -1;
        return 0;
    }
    if (st->sub == 13) {
        st->i++;
        if (st->i < 2) return 0;
        st->phase = PH_BLOCK;
        st->sub = 0;
        st->sym = -1;
        return 0;
    }
    return -1;
}

static int inflate_body(struct internal_state *st, z_streamp strm) {
    for (;;) {
        int pr, v, c;
        if (st->phase == PH_DONE) return Z_STREAM_END;
        if (st->phase == PH_BAD) return Z_DATA_ERROR;

        if (st->phase == PH_HEAD) {
            if (st->wrap == 0) { st->phase = PH_BLOCK; continue; }
            if (st->wrap == 3) {
                if (!strm->avail_in || !strm->next_in) return 0;
                st->wrap = strm->next_in[0] == 0x1f ? 2 : 1;
                continue;
            }
            if (st->wrap == 1) {
                pr = take_byte(st, strm, &c);
                if (pr <= 0) return pr;
                if (st->sub == 0) { st->hdr[0] = (unsigned char)c; st->sub = 1; continue; }
                st->hdr[1] = (unsigned char)c;
                if ((st->hdr[0] & 15) != 8 || (st->hdr[1] & 0x20)) {
                    st->msg = "unknown compression method";
                    return -1;
                }
                if (((((unsigned)st->hdr[0] << 8) | st->hdr[1]) % 31u) != 0) {
                    st->msg = "incorrect header check";
                    return -1;
                }
                st->phase = PH_BLOCK;
                st->sub = 0;
                st->sym = -1;
                continue;
            }
            pr = take_byte(st, strm, &c);
            if (pr <= 0) return pr;
            if (gzip_header_byte(st, c) < 0) {
                st->msg = "incorrect gzip header";
                return -1;
            }
            continue;
        }

        if (st->phase == PH_BLOCK) {
            if (st->sub == 0) {
                pr = take_bits(st, strm, 1, &v);
                if (pr <= 0) return pr;
                st->bfinal = v;
                st->sub = 1;
            }
            pr = take_bits(st, strm, 2, &v);
            if (pr <= 0) return pr;
            st->btype = v;
            st->sub = 0;
            st->sym = -1;
            if (v == 0) { st->phase = PH_STORED; st->i = 0; continue; }
            if (v == 1) {
                if (build_fixed() < 0) return -1;
                memcpy(st->lcount, fix_lcount, sizeof st->lcount);
                memcpy(st->lsym, fix_lsym, sizeof st->lsym);
                memcpy(st->dcount, fix_dcount, sizeof st->dcount);
                memcpy(st->dsym, fix_dsym, sizeof st->dsym);
                st->phase = PH_CODES;
                continue;
            }
            if (v == 2) {
                memset(st->lens, 0, sizeof st->lens);
                memset(st->clen, 0, sizeof st->clen);
                st->phase = PH_DYN;
                st->i = 0;
                st->rep_left = 0;
                continue;
            }
            st->msg = "invalid block type";
            return -1;
        }

        if (st->phase == PH_STORED) {
            if (st->sub == 0) { drop_to_byte(st); st->sub = 1; }
            if (st->sub == 1) {
                pr = take_bits(st, strm, 16, &v);
                if (pr <= 0) return pr;
                st->stored_left = v;
                st->sub = 2;
            }
            if (st->sub == 2) {
                pr = take_bits(st, strm, 16, &v);
                if (pr <= 0) return pr;
                if (v != (st->stored_left ^ 0xffff)) {
                    st->msg = "invalid stored block lengths";
                    return -1;
                }
                st->sub = 3;
            }
            while (st->stored_left > 0) {
                if (!strm->avail_out) return 0;
                pr = take_byte(st, strm, &c);
                if (pr <= 0) return pr;
                if (!emit_byte(st, strm, (unsigned char)c)) return 0;
                st->stored_left--;
            }
            if (end_block(st, strm) < 0) return -1;
            continue;
        }

        if (st->phase == PH_DYN) {
            if (st->sub == 0) {
                pr = take_bits(st, strm, 5, &v);
                if (pr <= 0) return pr;
                st->nlit = v + 257;
                st->sub = 1;
            }
            if (st->sub == 1) {
                pr = take_bits(st, strm, 5, &v);
                if (pr <= 0) return pr;
                st->ndist = v + 1;
                st->sub = 2;
            }
            if (st->sub == 2) {
                pr = take_bits(st, strm, 4, &v);
                if (pr <= 0) return pr;
                st->nclen = v + 4;
                if (st->nlit > 288 || st->ndist > 32 || st->nclen > 19) {
                    st->msg = "too many length codes";
                    return -1;
                }
                st->sub = 3;
                st->i = 0;
            }
            if (st->sub == 3) {
                while (st->i < st->nclen) {
                    pr = take_bits(st, strm, 3, &v);
                    if (pr <= 0) return pr;
                    st->clen[clen_order[st->i]] = (unsigned char)v;
                    st->i++;
                }
                if (construct(st->lcount, st->lsym, st->clen, 19) < 0) {
                    st->msg = "invalid code lengths";
                    return -1;
                }
                st->sub = 4;
                st->i = 0;
                st->sym = -1;
                st->rep_left = 0;
            }
            if (st->sub == 4) {
                int tot = st->nlit + st->ndist;
                while (st->i < tot) {
                    int extra;
                    if (st->rep_left) {
                        if (st->i >= tot) return -1;
                        st->lens[st->i++] = (unsigned char)st->what;
                        st->rep_left--;
                        continue;
                    }
                    if (st->sym < 0) {
                        pr = decode_sym(st, strm, st->lcount, st->lsym, 19, &v);
                        if (pr <= 0) return pr;
                        if (v < 0 || v > 18) return -1;
                        st->sym = v;
                    }
                    if (st->sym < 16) {
                        st->lens[st->i++] = (unsigned char)st->sym;
                        st->sym = -1;
                        continue;
                    }
                    extra = st->sym == 16 ? 2 : st->sym == 17 ? 3 : 7;
                    pr = take_bits(st, strm, extra, &v);
                    if (pr <= 0) return pr;
                    if (st->sym == 16) {
                        if (st->i == 0) {
                            st->msg = "invalid code lengths";
                            return -1;
                        }
                        st->what = st->lens[st->i - 1];
                        st->rep_left = 3 + v;
                    } else if (st->sym == 17) {
                        st->what = 0;
                        st->rep_left = 3 + v;
                    } else {
                        st->what = 0;
                        st->rep_left = 11 + v;
                    }
                    st->sym = -1;
                    if (st->i + st->rep_left > tot) {
                        st->msg = "invalid code lengths";
                        return -1;
                    }
                }
                if (construct(st->lcount, st->lsym, st->lens, st->nlit) < 0 ||
                    construct(st->dcount, st->dsym, st->lens + st->nlit, st->ndist) < 0) {
                    st->msg = "invalid literal/lengths";
                    return -1;
                }
                st->phase = PH_CODES;
                st->sub = 0;
                st->sym = -1;
                continue;
            }
            continue;
        }

        if (st->phase == PH_CODES) {
            int extra, ds;
            if (st->sym < 0) {
                pr = decode_sym(st, strm, st->lcount, st->lsym, 288, &v);
                if (pr <= 0) return pr;
                st->sym = v;
                st->sub = 0;
            }
            if (st->sym < 256) {
                if (!strm->avail_out) return 0;
                if (!emit_byte(st, strm, (unsigned char)st->sym)) return 0;
                st->sym = -1;
                continue;
            }
            if (st->sym == 256) {
                if (end_block(st, strm) < 0) return -1;
                continue;
            }
            if (st->sym > 285) return -1;
            if (st->sub == 0) {
                extra = len_extra[st->sym - 257];
                pr = take_bits(st, strm, extra, &v);
                if (pr <= 0) return pr;
                st->length = len_base[st->sym - 257] + v;
                st->sub = 1;
            }
            if (st->sub == 1) {
                pr = decode_sym(st, strm, st->dcount, st->dsym, 32, &ds);
                if (pr <= 0) return pr;
                if (ds < 0 || ds > 29) {
                    st->msg = "invalid distance code";
                    return -1;
                }
                st->dist = ds;
                st->sub = 2;
            }
            if (st->sub == 2) {
                extra = dist_extra[st->dist];
                pr = take_bits(st, strm, extra, &v);
                if (pr <= 0) return pr;
                st->dist = dist_base[st->dist] + v;
                if (st->length < 3 || st->length > 258 || st->dist < 1 ||
                    (unsigned)st->dist > st->wpos || st->dist > WSIZE) {
                    st->msg = "invalid distance too far back";
                    return -1;
                }
                st->match_left = st->length;
                st->phase = PH_MATCH;
                st->sym = -1;
                st->sub = 0;
            }
            continue;
        }

        if (st->phase == PH_MATCH) {
            while (st->match_left > 0) {
                unsigned char b;
                if (!strm->avail_out) return 0;
                b = st->window[(st->wpos - (unsigned)st->dist) & (WSIZE - 1)];
                if (!emit_byte(st, strm, b)) return 0;
                st->match_left--;
            }
            st->phase = PH_CODES;
            st->sym = -1;
            continue;
        }

        if (st->phase == PH_CHECK) {
            int need = st->wrap == 2 ? 8 : 4;
            unsigned got, expect;
            while (st->i < need) {
                pr = take_byte(st, strm, &c);
                if (pr <= 0) return pr;
                st->hdr[st->i++] = (unsigned char)c;
            }
            if (st->wrap == 2) {
                got = (unsigned)st->hdr[0] | ((unsigned)st->hdr[1] << 8) |
                      ((unsigned)st->hdr[2] << 16) | ((unsigned)st->hdr[3] << 24);
                expect = (unsigned)st->hdr[4] | ((unsigned)st->hdr[5] << 8) |
                         ((unsigned)st->hdr[6] << 16) | ((unsigned)st->hdr[7] << 24);
                if (got != st->crc || expect != st->wpos) {
                    st->msg = "incorrect data check";
                    return -1;
                }
            } else {
                got = ((unsigned)st->hdr[0] << 24) | ((unsigned)st->hdr[1] << 16) |
                      ((unsigned)st->hdr[2] << 8) | (unsigned)st->hdr[3];
                expect = (st->s2 << 16) | st->s1;
                if (got != expect) {
                    st->msg = "incorrect data check";
                    return -1;
                }
            }
            st->phase = PH_DONE;
            continue;
        }

        st->msg = "stream error";
        return -1;
    }
}

int inflate(z_streamp strm, int flush) {
    struct internal_state *st;
    uInt in0, out0;
    int pr;
    (void)flush;
    if (!strm || !strm->state || strm->state->kind != 2) return Z_STREAM_ERROR;
    st = strm->state;
    if (st->phase == PH_DONE) return Z_STREAM_END;
    if (st->phase == PH_BAD) return Z_DATA_ERROR;
    in0 = strm->avail_in;
    out0 = strm->avail_out;
    pr = inflate_body(st, strm);
    if (pr < 0) {
        st->phase = PH_BAD;
        strm->msg = st->msg ? st->msg : "invalid compressed data";
        return Z_DATA_ERROR;
    }
    if (st->phase == PH_DONE || pr == Z_STREAM_END) return Z_STREAM_END;
    if (strm->avail_in == in0 && strm->avail_out == out0) return Z_BUF_ERROR;
    return Z_OK;
}

int inflateEnd(z_streamp strm) {
    if (!strm || !strm->state || strm->state->kind != 2) return Z_STREAM_ERROR;
    free_bufs(strm->state);
    z_free(strm, strm->state);
    strm->state = 0;
    return Z_OK;
}

static int inflate_init(z_streamp strm, int windowBits, const char *version, int stream_size) {
    struct internal_state *st;
    int wrap = 0;
    if (!strm || !version_ok(version, stream_size) ) return !strm ? Z_STREAM_ERROR : Z_VERSION_ERROR;
    if (!alloc_pair_ok(strm)) return Z_STREAM_ERROR;
    if (parse_window(windowBits, &wrap) < 0) return Z_STREAM_ERROR;
    st = z_alloc(strm, sizeof *st);
    if (!st) return Z_MEM_ERROR;
    memset(st, 0, sizeof *st);
    st->kind = 2;
    st->wrap = wrap;
    strm->state = st;
    strm->msg = 0;
    strm->total_in = strm->total_out = 0;
    strm->adler = wrap == 2 ? 0 : 1;
    inflate_clear(st);
    return Z_OK;
}

int inflateInit_(z_streamp strm, const char *version, int stream_size) {
    return inflate_init(strm, MAX_WBITS, version, stream_size);
}

int inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size) {
    return inflate_init(strm, windowBits, version, stream_size);
}

int inflateReset(z_streamp strm) {
    struct internal_state *st;
    if (!strm || !strm->state || strm->state->kind != 2) return Z_STREAM_ERROR;
    st = strm->state;
    inflate_clear(st);
    strm->total_in = strm->total_out = 0;
    strm->msg = 0;
    strm->adler = st->wrap == 2 ? 0 : 1;
    return Z_OK;
}

int inflateReset2(z_streamp strm, int windowBits) {
    int wrap;
    if (!strm || !strm->state || strm->state->kind != 2) return Z_STREAM_ERROR;
    if (parse_window(windowBits, &wrap) < 0) return Z_STREAM_ERROR;
    strm->state->wrap = wrap;
    return inflateReset(strm);
}

/* ------------------------------------------------------- упаковка */

static unsigned short lit_code[288];
static unsigned char lit_nbits[288];
static unsigned short dist_code[30];
static unsigned char dist_nbits[30];
static int codes_ready;

struct bout {
    unsigned char *p;
    int n, cap, bits, err;
    unsigned acc;
};

static void put_byte(struct bout *o, unsigned char c) {
    unsigned char *np;
    int nc;
    if (o->err) return;
    if (o->n >= o->cap) {
        nc = o->cap ? o->cap * 2 : 256;
        if (nc < o->n + 1) nc = o->n + 1;
        np = realloc(o->p, (size_t)nc);
        if (!np) { o->err = 1; return; }
        o->p = np;
        o->cap = nc;
    }
    o->p[o->n++] = c;
}

static void putbit(struct bout *o, int bit) {
    if (o->err) return;
    o->acc |= (unsigned)(bit & 1) << o->bits;
    if (++o->bits == 8) { put_byte(o, (unsigned char)o->acc); o->acc = 0; o->bits = 0; }
}

static void put_msb(struct bout *o, unsigned v, int n) {
    int i;
    for (i = n - 1; i >= 0; i--) putbit(o, (v >> i) & 1);
}

static void put_lsb(struct bout *o, unsigned v, int n) {
    int i;
    for (i = 0; i < n; i++) putbit(o, (v >> i) & 1);
}

static void bout_flush(struct bout *o) {
    if (o->bits) put_byte(o, (unsigned char)o->acc);
    o->acc = 0;
    o->bits = 0;
}

static void codes_from_lengths(unsigned short *code, unsigned char *nbits, const unsigned char *len, int n) {
    int count[16], nextc[16], i, bits, c;
    memset(count, 0, sizeof count);
    for (i = 0; i < n; i++) if (len[i]) count[len[i]]++;
    c = 0;
    for (bits = 1; bits <= 15; bits++) {
        c = (c + count[bits - 1]) << 1;
        nextc[bits] = c;
    }
    for (i = 0; i < n; i++) {
        nbits[i] = len[i];
        code[i] = 0;
        if (len[i]) code[i] = (unsigned short)nextc[len[i]]++;
    }
}

static void codes_init(void) {
    unsigned char llen[288], dlen[30];
    int i;
    if (codes_ready) return;
    for (i = 0; i < 144; i++) llen[i] = 8;
    for (; i < 256; i++) llen[i] = 9;
    for (; i < 280; i++) llen[i] = 7;
    for (; i < 288; i++) llen[i] = 8;
    for (i = 0; i < 30; i++) dlen[i] = 5;
    codes_from_lengths(lit_code, lit_nbits, llen, 288);
    codes_from_lengths(dist_code, dist_nbits, dlen, 30);
    codes_ready = 1;
}

static int emit_lit(struct bout *o, int sym) {
    if (sym < 0 || sym > 287 || !lit_nbits[sym]) return -1;
    put_msb(o, lit_code[sym], lit_nbits[sym]);
    return o->err ? -1 : 0;
}

static int emit_match(struct bout *o, int length, int dist) {
    int i, ls, ds;
    if (length < 3 || length > 258 || dist < 1 || dist > WSIZE) return -1;
    ls = 285;
    for (i = 0; i < 29; i++) {
        int max = len_base[i] + ((1 << len_extra[i]) - 1);
        if (length <= max) { ls = 257 + i; break; }
    }
    if (emit_lit(o, ls) < 0) return -1;
    put_lsb(o, (unsigned)(length - len_base[ls - 257]), len_extra[ls - 257]);
    ds = 29;
    for (i = 0; i < 30; i++) {
        int max = dist_base[i] + ((1 << dist_extra[i]) - 1);
        if (dist <= max) { ds = i; break; }
    }
    put_msb(o, dist_code[ds], dist_nbits[ds]);
    put_lsb(o, (unsigned)(dist - dist_base[ds]), dist_extra[ds]);
    return o->err ? -1 : 0;
}

static unsigned hash3(const unsigned char *s) {
    return (((unsigned)s[0] << 10) ^ ((unsigned)s[1] << 5) ^ (unsigned)s[2]) & (HASH_SIZE - 1);
}

static void insert_hash(const unsigned char *in, int n, int pos, unsigned *head, unsigned *prev) {
    unsigned h;
    if (pos < 0 || pos + 2 >= n) return;
    h = hash3(in + pos);
    prev[pos & (WSIZE - 1)] = head[h];
    head[h] = (unsigned)pos + 1;
}

static int deflate_huff(const unsigned char *in, int n, int level, struct bout *o) {
    static const int chains[10] = { 0, 4, 8, 16, 32, 64, 128, 256, 1024, 4096 };
    static const int nices[10] = { 0, 8, 16, 32, 48, 64, 128, 128, 258, 258 };
    unsigned *head, *prev;
    int pos = 0, chain, nice;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    chain = chains[level];
    nice = nices[level];
    codes_init();
    head = calloc(HASH_SIZE, sizeof(unsigned));
    prev = calloc(WSIZE, sizeof(unsigned));
    if (!head || !prev) { free(head); free(prev); return -1; }
    putbit(o, 1);
    putbit(o, 1);
    putbit(o, 0);
    while (pos < n) {
        int len = 0, dist = 0, k;
        if (pos + 3 <= n) {
            unsigned cur;
            int steps, max, best = 0, best_d = 0;
            insert_hash(in, n, pos, head, prev);
            cur = prev[pos & (WSIZE - 1)];
            max = n - pos;
            if (max > MAX_MATCH) max = MAX_MATCH;
            steps = chain;
            while (cur && steps--) {
                int p = (int)cur - 1;
                int d = pos - p;
                int m = 0;
                if (p < 0 || d <= 0 || d > WSIZE) break;
                while (m < max && in[p + m] == in[pos + m]) m++;
                if (m >= 3 && m > best) { best = m; best_d = d; }
                if (best >= nice || best == max) break;
                cur = prev[p & (WSIZE - 1)];
            }
            len = best;
            dist = best_d;
        }
        if (len >= 3) {
            if (emit_match(o, len, dist) < 0) { free(head); free(prev); return -1; }
            for (k = 1; k < len; k++) insert_hash(in, n, pos + k, head, prev);
            pos += len;
        } else {
            if (emit_lit(o, in[pos]) < 0) { free(head); free(prev); return -1; }
            pos++;
        }
    }
    free(head);
    free(prev);
    if (emit_lit(o, 256) < 0) return -1;
    bout_flush(o);
    return o->err ? -1 : 0;
}

static int deflate_stored(const unsigned char *in, int n, struct bout *o) {
    int off = 0;
    while (off < n) {
        int chunk = n - off;
        int last;
        unsigned char *np;
        if (chunk > 65535) chunk = 65535;
        last = off + chunk == n;
        putbit(o, last);
        putbit(o, 0);
        putbit(o, 0);
        bout_flush(o);
        put_byte(o, (unsigned char)(chunk & 255));
        put_byte(o, (unsigned char)((chunk >> 8) & 255));
        put_byte(o, (unsigned char)((~chunk) & 255));
        put_byte(o, (unsigned char)(((~chunk) >> 8) & 255));
        if (o->err) return -1;
        if (o->n > 0x7fffffff - chunk) return -1;
        if (o->n + chunk > o->cap) {
            np = realloc(o->p, (size_t)(o->n + chunk));
            if (!np) return -1;
            o->p = np;
            o->cap = o->n + chunk;
        }
        memcpy(o->p + o->n, in + off, (size_t)chunk);
        o->n += chunk;
        off += chunk;
    }
    if (n == 0) {
        putbit(o, 1);
        putbit(o, 0);
        putbit(o, 0);
        bout_flush(o);
        put_byte(o, 0); put_byte(o, 0); put_byte(o, 0xFF); put_byte(o, 0xFF);
    }
    return o->err ? -1 : 0;
}

/* Сырой deflate. Возвращает malloc-буфер. */
static unsigned char *pack_raw(const unsigned char *in, int n, int level, int *out_n) {
    struct bout huff, stored;
    memset(&huff, 0, sizeof huff);
    memset(&stored, 0, sizeof stored);
    if (level < 0) level = 6;
    if (n && level > 0 && deflate_huff(in, n, level, &huff) == 0 && !huff.err && huff.n < n) {
        *out_n = huff.n;
        return huff.p;
    }
    free(huff.p);
    if (deflate_stored(in, n, &stored) < 0 || stored.err) { free(stored.p); return 0; }
    *out_n = stored.n;
    return stored.p;
}

static unsigned char *wrap_out(int wrap, int level, int gzip_os, const unsigned char *raw, int n,
                               const unsigned char *def, int dn, int *out_n) {
    unsigned char *out;
    unsigned a;
    int i, total;
    (void)level;
    if (wrap == 0) {
        out = malloc((size_t)(dn ? dn : 1));
        if (!out) return 0;
        if (dn) memcpy(out, def, (size_t)dn);
        *out_n = dn;
        return out;
    }
    if (wrap == 2) {
        total = dn + 18;
        out = malloc((size_t)total);
        if (!out) return 0;
        out[0] = 0x1f; out[1] = 0x8b; out[2] = 8; out[3] = 0;
        out[4] = out[5] = out[6] = out[7] = out[8] = 0; out[9] = (unsigned char)gzip_os;
        memcpy(out + 10, def, (size_t)dn);
        a = (unsigned)crc32(0, raw ? raw : (const Bytef *)"", (uInt)n);
        for (i = 0; i < 4; i++) out[10 + dn + i] = (unsigned char)((a >> (8 * i)) & 255);
        for (i = 0; i < 4; i++) out[14 + dn + i] = (unsigned char)(((unsigned)n >> (8 * i)) & 255);
        *out_n = total;
        return out;
    }
    total = dn + 6;
    out = malloc((size_t)total);
    if (!out) return 0;
    out[0] = 0x78; out[1] = 0x9c;
    memcpy(out + 2, def, (size_t)dn);
    a = (unsigned)adler32(1, raw ? raw : (const Bytef *)"", (uInt)n);
    out[2 + dn] = (unsigned char)((a >> 24) & 255);
    out[3 + dn] = (unsigned char)((a >> 16) & 255);
    out[4 + dn] = (unsigned char)((a >> 8) & 255);
    out[5 + dn] = (unsigned char)(a & 255);
    *out_n = total;
    return out;
}

static int deflate_produce(z_streamp strm) {
    struct internal_state *st = strm->state;
    unsigned char *def, *out;
    int dn = 0, on = 0;
    if (st->in_len > 0x7fffffffu) return Z_MEM_ERROR;
    def = pack_raw(st->inbuf, (int)st->in_len, st->level, &dn);
    if (!def) return Z_MEM_ERROR;
    out = wrap_out(st->wrap == 2 ? 2 : st->wrap == 0 ? 0 : 1, st->level, st->gzip_os,
                   st->inbuf, (int)st->in_len, def, dn, &on);
    free(def);
    if (!out) return Z_MEM_ERROR;
    free(st->outbuf);
    st->outbuf = out;
    st->out_len = (unsigned)on;
    st->out_pos = 0;
    st->compressed = 1;
    if (st->wrap == 2) strm->adler = crc32(0, st->inbuf ? st->inbuf : (const Bytef *)"", (uInt)st->in_len);
    else strm->adler = adler32(1, st->inbuf ? st->inbuf : (const Bytef *)"", (uInt)st->in_len);
    return Z_OK;
}

int deflate(z_streamp strm, int flush) {
    struct internal_state *st;
    uInt in0, out0;
    unsigned room, n, copy;
    if (!strm || !strm->state || strm->state->kind != 1) return Z_STREAM_ERROR;
    st = strm->state;
    if (st->compressed && flush != Z_FINISH && strm->avail_in) return Z_STREAM_ERROR;
    in0 = strm->avail_in;
    out0 = strm->avail_out;
    if (strm->avail_in) {
        if (!strm->next_in) return Z_STREAM_ERROR;
        if (st->compressed) return Z_STREAM_ERROR;
        if (strm->avail_in > 0xffffffffu - st->in_len) return Z_MEM_ERROR;
        room = st->in_len + strm->avail_in;
        if (room > st->in_cap) {
            unsigned cap = st->in_cap ? st->in_cap : 256;
            unsigned char *np;
            while (cap < room) {
                if (cap > 0x7fffffffu / 2) { cap = room; break; }
                cap *= 2;
            }
            np = realloc(st->inbuf, cap);
            if (!np) return Z_MEM_ERROR;
            st->inbuf = np;
            st->in_cap = cap;
        }
        memcpy(st->inbuf + st->in_len, strm->next_in, strm->avail_in);
        st->in_len += strm->avail_in;
        strm->next_in += strm->avail_in;
        strm->total_in += strm->avail_in;
        strm->avail_in = 0;
    }
    if (flush == Z_FINISH && !st->compressed) {
        int pr = deflate_produce(strm);
        if (pr != Z_OK) return pr;
    }
    if (st->compressed && st->out_pos < st->out_len && strm->avail_out && strm->next_out) {
        n = st->out_len - st->out_pos;
        copy = n > strm->avail_out ? strm->avail_out : n;
        memcpy(strm->next_out, st->outbuf + st->out_pos, copy);
        strm->next_out += copy;
        strm->avail_out -= copy;
        strm->total_out += copy;
        st->out_pos += copy;
    }
    if (st->compressed && st->out_pos == st->out_len && flush == Z_FINISH) return Z_STREAM_END;
    if (strm->avail_in != in0 || strm->avail_out != out0) return Z_OK;
    if (st->compressed && st->out_pos < st->out_len) return Z_BUF_ERROR;
    return Z_BUF_ERROR;
}

int deflateSetHeader(z_streamp strm, gz_headerp head) {
    if (!strm || !strm->state || strm->state->kind != 1 || !head) return Z_STREAM_ERROR;
    if (strm->state->wrap != 2) return Z_STREAM_ERROR;
    strm->state->gzip_os = head->os;
    return Z_OK;
}
int deflateEnd(z_streamp strm) {
    if (!strm || !strm->state || strm->state->kind != 1) return Z_STREAM_ERROR;
    free_bufs(strm->state);
    z_free(strm, strm->state);
    strm->state = 0;
    return Z_OK;
}

static int deflate_init(z_streamp strm, int level, int method, int windowBits,
                        const char *version, int stream_size) {
    struct internal_state *st;
    int wrap = 0;
    if (!strm) return Z_STREAM_ERROR;
    if (!version_ok(version, stream_size)) return Z_VERSION_ERROR;
    if (!alloc_pair_ok(strm)) return Z_STREAM_ERROR;
    if (method != Z_DEFLATED) return Z_STREAM_ERROR;
    if (level == Z_DEFAULT_COMPRESSION) level = 6;
    if (level < 0 || level > 9) return Z_STREAM_ERROR;
    if (parse_window(windowBits, &wrap) < 0) return Z_STREAM_ERROR;
    if (wrap == 3) wrap = 1;
    st = z_alloc(strm, sizeof *st);
    if (!st) return Z_MEM_ERROR;
    memset(st, 0, sizeof *st);
    st->kind = 1;
    st->wrap = wrap;
    st->level = level;
    st->gzip_os = 255;
    strm->state = st;
    strm->msg = 0;
    strm->total_in = strm->total_out = 0;
    strm->adler = wrap == 2 ? 0 : 1;
    return Z_OK;
}

int deflateInit_(z_streamp strm, int level, const char *version, int stream_size) {
    return deflate_init(strm, level, Z_DEFLATED, MAX_WBITS, version, stream_size);
}

int deflateInit2_(z_streamp strm, int level, int method, int windowBits,
                  int memLevel, int strategy, const char *version, int stream_size) {
    (void)memLevel;
    (void)strategy;
    return deflate_init(strm, level, method, windowBits, version, stream_size);
}

int deflateReset(z_streamp strm) {
    struct internal_state *st;
    int wrap, level;
    if (!strm || !strm->state || strm->state->kind != 1) return Z_STREAM_ERROR;
    st = strm->state;
    wrap = st->wrap;
    level = st->level;
    free_bufs(st);
    memset(st, 0, sizeof *st);
    st->kind = 1;
    st->wrap = wrap;
    st->level = level;
    st->gzip_os = 255;
    strm->total_in = strm->total_out = 0;
    strm->msg = 0;
    strm->adler = wrap == 2 ? 0 : 1;
    return Z_OK;
}

uLong deflateBound(z_streamp strm, uLong sourceLen) {
    uLong chunks, stored;
    (void)strm;
    chunks = sourceLen ? (sourceLen + 65534ul) / 65535ul : 1ul;
    stored = sourceLen + chunks * 5ul + 64ul;
    return stored;
}

uLong compressBound(uLong sourceLen) { return deflateBound(0, sourceLen); }

int compress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen, int level) {
    z_stream s;
    int ret;
    uLong bound;
    if (!dest || !destLen) return Z_STREAM_ERROR;
    bound = deflateBound(0, sourceLen);
    if (*destLen < bound && sourceLen > *destLen) {
        /* всё равно пробуем: оценка с запасом, реальному выходу может хватить */
    }
    memset(&s, 0, sizeof s);
    ret = deflateInit(&s, level);
    if (ret != Z_OK) return ret;
    s.next_in = (Bytef *)source;
    s.avail_in = (uInt)sourceLen;
    s.next_out = dest;
    s.avail_out = (uInt)*destLen;
    ret = deflate(&s, Z_FINISH);
    *destLen = s.total_out;
    deflateEnd(&s);
    if (ret == Z_STREAM_END) return Z_OK;
    if (ret == Z_OK || ret == Z_BUF_ERROR) return Z_BUF_ERROR;
    return ret;
}

int compress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen) {
    return compress2(dest, destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
}

int uncompress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong *sourceLen) {
    z_stream s;
    int ret;
    if (!dest || !destLen || !sourceLen) return Z_STREAM_ERROR;
    memset(&s, 0, sizeof s);
    ret = inflateInit(&s);
    if (ret != Z_OK) return ret;
    s.next_in = (Bytef *)source;
    s.avail_in = (uInt)*sourceLen;
    s.next_out = dest;
    s.avail_out = (uInt)*destLen;
    ret = inflate(&s, Z_FINISH);
    *destLen = s.total_out;
    *sourceLen = s.total_in;
    inflateEnd(&s);
    if (ret == Z_STREAM_END) return Z_OK;
    if (ret == Z_OK || ret == Z_BUF_ERROR) return Z_BUF_ERROR;
    return ret;
}

int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen) {
    uLong n = sourceLen;
    return uncompress2(dest, destLen, source, &n);
}
