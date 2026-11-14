/* tar — распаковка ustar и чтение оглавления.
 * Флаги как у GNU tar: tar xf a.tar, tar xzf a.tar.gz, -C, -v, -k,
 * --strip-components, длинные имена GNU и заголовки pax (path, linkpath, size).
 * .tar.gz разжимает libz потоком и сразу разбирает, без файла в /tmp.
 * Обычный tar читается потоком. Жёстких ссылок нет: содержимое копируется.
 * Устройства и FIFO пропускаются с сообщением. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "utime.h"
#include "dirent.h"
#include "zlib.h"

#define NAME_CAP    1024
#define PAX_MAX     (64 * 1024)

static int opt_list, opt_create, opt_gzip, opt_verbose, opt_keep, opt_absolute, opt_strip;
static const char *opt_file, *opt_dir;
static int run_create(void);
static char **opt_members;
static int opt_nmem;

/* ------------------------------------------------------- gzip (inflate через libz) */

/* Окно deflate живёт внутри libz и выделяется только для .tar.gz.
 * Сюда — небольшой вход и выход, чтобы не держать архив целиком. */
struct gz_in {
    int fd;
    z_stream zs;
    int started, eof, finished;
    unsigned char pre[512];
    int pre_len, pre_pos;
    unsigned char in[4096];
    unsigned char out[4096];
    int out_len, out_pos;
    unsigned reported;
};

static const char *gz_msg;
static int gz_msg_printed;

static void gz_fail(const char *m) { if (!gz_msg) gz_msg = m; }

static const char *gz_why(const char *msg) {
    if (!msg) return "invalid compressed data";
    if (!strcmp(msg, "incorrect data check")) return "crc error";
    if (!strcmp(msg, "incorrect gzip header")) return "not in gzip format";
    return msg;
}

static void gz_progress(struct gz_in *g) {
    unsigned mb = (unsigned)(g->zs.total_out >> 20);
    if (!opt_verbose || mb < 4 || (mb & 3) || mb == g->reported) return;
    g->reported = mb;
    fprintf(stderr, "tar: decompressed %u MiB\n", mb);
}

static int gz_fill(struct gz_in *g) {
    int n, r;
    if (g->zs.avail_in) return 0;
    if (g->pre_pos < g->pre_len) {
        n = g->pre_len - g->pre_pos;
        if (n > (int)sizeof g->in) n = (int)sizeof g->in;
        memcpy(g->in, g->pre + g->pre_pos, (size_t)n);
        g->pre_pos += n;
        g->zs.next_in = g->in;
        g->zs.avail_in = (uInt)n;
        return 0;
    }
    if (g->eof) return 0;
    r = read(g->fd, g->in, sizeof g->in);
    if (r < 0) return -1;
    if (r == 0) { g->eof = 1; return 0; }
    g->zs.next_in = g->in;
    g->zs.avail_in = (uInt)r;
    return 0;
}

/* 1 — есть ещё член gzip, 0 — поток кончился (хвост после архива тоже). */
static int gz_next_member(struct gz_in *g) {
    if (!g->zs.avail_in && gz_fill(g) < 0) return -1;
    if (!g->zs.avail_in) return 0;
    if (g->zs.next_in[0] != 31) return 0;
    if (inflateReset(&g->zs) != Z_OK) { gz_fail("invalid compressed data"); return -1; }
    return 1;
}

static int gz_pump(struct gz_in *g) {
    int ret, more;
    if (g->finished) return 0;
    if (!g->started) {
        if (inflateInit2(&g->zs, 16 + MAX_WBITS) != Z_OK) { gz_fail("out of memory"); return -1; }
        g->started = 1;
    }
    g->zs.next_out = g->out;
    g->zs.avail_out = (uInt)sizeof g->out;
    for (;;) {
        if (!g->zs.avail_in && gz_fill(g) < 0) { gz_fail("unexpected end of file"); return -1; }
        ret = inflate(&g->zs, Z_NO_FLUSH);
        gz_progress(g);
        if (ret == Z_STREAM_END) {
            g->out_len = (int)sizeof g->out - (int)g->zs.avail_out;
            g->out_pos = 0;
            more = gz_next_member(g);
            if (more < 0) return -1;
            if (!more) g->finished = 1;
            if (g->out_len || g->finished) return 0;
            g->zs.next_out = g->out;
            g->zs.avail_out = (uInt)sizeof g->out;
            continue;
        }
        if (ret == Z_OK) {
            if (!g->zs.avail_out) {
                g->out_len = (int)sizeof g->out;
                g->out_pos = 0;
                return 0;
            }
            if (!g->zs.avail_in && g->eof) { gz_fail("unexpected end of file"); return -1; }
            continue;
        }
        if (ret == Z_BUF_ERROR && !g->zs.avail_in && !g->eof) continue;
        if (ret == Z_BUF_ERROR) { gz_fail("unexpected end of file"); return -1; }
        gz_fail(gz_why(g->zs.msg));
        return -1;
    }
}

static void gz_close(struct gz_in *g) {
    if (!g) return;
    if (g->started) inflateEnd(&g->zs);
    free(g);
}


/* ------------------------------------------------------- чтение архива */

struct src {
    int fd;
    struct gz_in *gz;
    const unsigned char *mem;
    unsigned memlen, mempos;
    unsigned char peek[512];
    int peek_len, peek_off;
};

static void short_archive(void) {
    if (gz_msg) {
        fprintf(stderr, "tar: %s: %s\n", opt_file ? opt_file : "archive", gz_msg);
        gz_msg_printed = 1;
    } else {
        fprintf(stderr, "tar: unexpected end of file\n");
    }
}

static int src_read(struct src *s, void *buf, int n) {
    unsigned char *p = buf;
    int got = 0;
    while (got < n) {
        int r, take;
        if (s->peek_off < s->peek_len) {
            take = n - got;
            if (take > s->peek_len - s->peek_off) take = s->peek_len - s->peek_off;
            memcpy(p + got, s->peek + s->peek_off, (size_t)take);
            s->peek_off += take;
            got += take;
            continue;
        }
        if (s->mem) {
            unsigned left = s->memlen - s->mempos;
            take = n - got;
            if ((unsigned)take > left) take = (int)left;
            if (!take) break;
            memcpy(p + got, s->mem + s->mempos, (size_t)take);
            s->mempos += (unsigned)take;
            got += take;
            continue;
        }
        if (s->gz) {
            struct gz_in *g = s->gz;
            if (g->out_pos >= g->out_len) {
                if (gz_pump(g) < 0) return -1;
                if (g->out_pos >= g->out_len) break;
            }
            take = n - got;
            if (take > g->out_len - g->out_pos) take = g->out_len - g->out_pos;
            memcpy(p + got, g->out + g->out_pos, (size_t)take);
            g->out_pos += take;
            got += take;
            continue;
        }
        r = read(s->fd, p + got, (size_t)(n - got));
        if (r < 0) return -1;
        if (r == 0) break;
        got += r;
    }
    return got;
}

static int src_exact(struct src *s, void *buf, int n) {
    int g = src_read(s, buf, n);
    if (g < 0) return -1;
    return g == n ? 0 : -2;
}

/* ------------------------------------------------------- заголовок tar */

static int parse_u32(const char *p, int n, unsigned *out) {
    unsigned long long v = 0;
    int i = 0;
    if ((unsigned char)p[0] & 0x80) {
        v = (unsigned char)p[0] & 0x7f;
        for (i = 1; i < n; i++) {
            if (v > 0xffffffull) return -1;
            v = (v << 8) | (unsigned char)p[i];
        }
        if (v > 0xffffffffull) return -1;
        *out = (unsigned)v;
        return 0;
    }
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > 0xffffffffull / 8) return -1;
        v = (v << 3) | (unsigned)(p[i] - '0');
    }
    if (v > 0xffffffffull) return -1;
    *out = (unsigned)v;
    return 0;
}

static void field_copy(char *dst, int cap, const char *src, int n) {
    int i = 0;
    while (i < n && i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int header_sum(const unsigned char *h, unsigned *sum, int *zero) {
    unsigned s = 0;
    int i, z = 1;
    for (i = 0; i < 512; i++) {
        unsigned char c = h[i];
        if (c) z = 0;
        if (i >= 148 && i < 156) c = ' ';
        s += c;
    }
    *sum = s;
    *zero = z;
    return 0;
}

static int header_ok(const unsigned char *h) {
    unsigned sum, stored;
    int zero;
    header_sum(h, &sum, &zero);
    if (zero) return 0;
    if (parse_u32((const char *)h + 148, 8, &stored) < 0) return -1;
    return stored == sum ? 1 : -1;
}

struct saved {
    char path[NAME_CAP];
    char link[NAME_CAP];
    int has_path, has_link, has_size, has_mtime;
    unsigned size, mtime;
};

static void saved_clear(struct saved *s) { memset(s, 0, sizeof *s); }

static int key_is(const char *k, int kn, const char *lit) {
    int n = (int)strlen(lit);
    return kn == n && memcmp(k, lit, (size_t)n) == 0;
}

static int pax_parse(const char *body, unsigned n, struct saved *s) {
    unsigned i = 0;
    while (i < n && body[i]) {
        unsigned start = i, rec = 0, end, ks, vs;
        int kn;
        while (i < n && body[i] >= '0' && body[i] <= '9') {
            if (rec > 10000000u) return -1;
            rec = rec * 10u + (unsigned)(body[i] - '0');
            i++;
        }
        if (i >= n || body[i] != ' ' || rec < 3 || start + rec > n) return -1;
        end = start + rec;
        i++;
        ks = i;
        while (i < end && body[i] != '=') i++;
        if (i >= end || body[i] != '=') return -1;
        kn = (int)(i - ks);
        vs = ++i;
        if (end == 0 || body[end - 1] != '\n') return -1;
        if (key_is(body + ks, kn, "path") || key_is(body + ks, kn, "linkpath")) {
            int len = (int)(end - 1 - vs);
            char *dst = key_is(body + ks, kn, "path") ? s->path : s->link;
            if (len <= 0 || len >= NAME_CAP) return -1;
            memcpy(dst, body + vs, (size_t)len);
            dst[len] = 0;
            if (dst == s->path) s->has_path = 1;
            else s->has_link = 1;
        } else if (key_is(body + ks, kn, "size")) {
            unsigned v = 0;
            unsigned j;
            for (j = vs; j + 1 < end; j++) {
                if (body[j] < '0' || body[j] > '9') return -1;
                if (v > 0x19999999u) return -1;
                v = v * 10u + (unsigned)(body[j] - '0');
            }
            s->size = v;
            s->has_size = 1;
        } else if (key_is(body + ks, kn, "mtime")) {
            unsigned v = 0, j;
            for (j = vs; j + 1 < end && body[j] >= '0' && body[j] <= '9'; j++) {
                if (v > 0x19999999u) return -1;
                v = v * 10u + (unsigned)(body[j] - '0');
            }
            s->mtime = v;
            s->has_mtime = 1;
        }
        i = end;
    }
    return 0;
}

/* 0 — путь готов, 1 — после strip ничего не осталось, -1 — «..» или слишком длинно. */
static int place_name(char *dst, int cap, const char *src, int strip, int absolute) {
    const char *p = src;
    int comp = 0, o = 0, lead = src[0] == '/';
    while (*p == '/') p++;
    while (comp < strip && *p) {
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
        comp++;
    }
    if (!*p) return 1;
    if (absolute && lead) {
        if (cap < 2) return -1;
        dst[o++] = '/';
    }
    while (*p) {
        const char *s = p;
        int n = 0;
        while (p[n] && p[n] != '/') n++;
        if (!(n == 1 && s[0] == '.')) {
            if (n == 2 && s[0] == '.' && s[1] == '.') return -1;
            if (o && dst[o - 1] != '/') {
                if (o + 1 >= cap) return -1;
                dst[o++] = '/';
            }
            if (o + n >= cap) return -1;
            memcpy(dst + o, s, (size_t)n);
            o += n;
        }
        p += n;
        while (*p == '/') p++;
    }
    if (!o || (o == 1 && dst[0] == '/')) return 1;
    dst[o] = 0;
    return 0;
}

static int name_wanted(const char *raw) {
    int i;
    const char *a = raw;
    if (!opt_nmem) return 1;
    while (a[0] == '.' && a[1] == '/') a += 2;
    for (i = 0; i < opt_nmem; i++) {
        const char *w = opt_members[i];
        int n;
        while (w[0] == '.' && w[1] == '/') w += 2;
        n = (int)strlen(w);
        while (n > 1 && w[n - 1] == '/') n--;
        if (strncmp(a, w, (size_t)n) == 0 && (a[n] == 0 || a[n] == '/')) return 1;
    }
    return 0;
}

static int err_code(int r) { return r == -1 ? errno : -r; }

static void warn_sys(const char *path, int r) {
    fprintf(stderr, "tar: %s: %s\n", path, strerror(err_code(r)));
}

static int is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* New directories stay private during extraction; apply modes inside-out. */
struct directory_mode {char *path;unsigned mode,mtime;struct directory_mode *next;};
static struct directory_mode *directory_modes;
static unsigned extraction_mask;
static void apply_mtime(const char *path, unsigned mtime) {
    struct utimbuf u;
    if (!mtime) return;
    u.actime = u.modtime = (time_t)mtime;
    utime(path, &u);
}
static void directory_header_mode(const char *path,unsigned mode,unsigned mtime) {
    for(struct directory_mode *d=directory_modes;d;d=d->next)
        if(!strcmp(d->path,path)){d->mode=mode&0777&~extraction_mask;d->mtime=mtime;return;}
}
static int finish_directory_modes(int success) {
    int rc=0;while(directory_modes){struct directory_mode *d=directory_modes;directory_modes=d->next;
        if(success && chmod(d->path,d->mode)<0){warn_sys(d->path,-1);rc=1;}
        if(success) apply_mtime(d->path,d->mtime);
        free(d->path);free(d);
    }return rc;
}
static int mkdir_one(const char *path) {
    int r=mkdir(path,0700);
    if(r>=0){struct directory_mode *d=malloc(sizeof(*d));if(!d)return -1;
        d->path=malloc(strlen(path)+1);if(!d->path){free(d);return -1;}strcpy(d->path,path);
        d->mode=0777&~extraction_mask;d->next=directory_modes;directory_modes=d;return 0;
    }
    if (r == -1) r = -errno;
    if (r == -EEXIST && is_dir(path)) return 0;
    warn_sys(path, r);
    return -1;
}

static int mkdir_p(const char *path) {
    char buf[NAME_CAP];
    int n = (int)strlen(path), i;
    if (n <= 0 || n >= NAME_CAP) return -1;
    memcpy(buf, path, (size_t)n + 1);
    i = buf[0] == '/' ? 1 : 0;
    for (; buf[i]; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        if (buf[0] && mkdir_one(buf) < 0) return -1;
        buf[i] = '/';
    }
    return mkdir_one(buf);
}

static int ensure_parent(const char *path) {
    char buf[NAME_CAP];
    char *slash;
    int n = (int)strlen(path);
    if (n >= NAME_CAP) return -1;
    memcpy(buf, path, (size_t)n + 1);
    slash = strrchr(buf, '/');
    if (!slash || slash == buf) return 0;
    *slash = 0;
    return mkdir_p(buf);
}

static int write_full(int fd, const void *buf, unsigned n);

static int copy_file(const char *src, const char *dst) {
    char buf[4096];
    int in, out, r;
    if (ensure_parent(dst) < 0) return -1;
    in = open(src, O_RDONLY);
    if (in < 0) return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { close(in); return -1; }
    for (;;) {
        r = read(in, buf, sizeof buf);
        if (r < 0) { close(in); close(out); return -1; }
        if (r == 0) break;
        if (write_full(out, buf, (unsigned)r) < 0) { close(in); close(out); return -1; }
    }
    close(in);
    close(out);
    return 0;
}

#define WRITE_CHUNK (32 * 1024)
static unsigned char io_buf[WRITE_CHUNK];

static int skip_bytes(struct src *s, unsigned size) {
    unsigned rest = (size + 511u) & ~511u;
    while (rest) {
        unsigned n = rest > WRITE_CHUNK ? WRITE_CHUNK : rest;
        if (src_exact(s, io_buf, (int)n) < 0) return -1;
        rest -= n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, unsigned n) {
    const unsigned char *p = buf;
    unsigned off = 0;
    while (off < n) {
        int w = write(fd, p + off, n - off);
        if (w < 0) return -1;
        if (w == 0) return -1;
        off += (unsigned)w;
    }
    return 0;
}

/* Данные члена всегда дочитываются до конца блока, чтобы следующий заголовок не съехал.
 * -2 — обрыв архива, -1 — файл не записан. */
static int write_member(struct src *s, const char *path, unsigned size,unsigned mode,unsigned mtime) {
    int fd = -1, err = 0;
    unsigned rest, filled = 0;
    if (ensure_parent(path) < 0) err = -1;
    else {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,mode&0777);
        if (fd < 0) err = -1;
    }
    rest = (size + 511u) & ~511u;
    while (rest) {
        unsigned n = rest > WRITE_CHUNK ? WRITE_CHUNK : rest;
        unsigned chunk;
        if (src_exact(s, io_buf, (int)n) < 0) { if (fd >= 0) close(fd); return -2; }
        if (!err && filled < size) {
            chunk = n;
            if (filled + chunk > size) chunk = size - filled;
            if (write_full(fd, io_buf, chunk) < 0) err = -1;
        }
        filled += n;
        rest -= n;
    }
    if (fd >= 0) close(fd);
    if (!err) apply_mtime(path, mtime);
    return err;
}

static int exists_keep(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static void show(char type, unsigned mode, unsigned size, const char *name, const char *link) {
    if (!opt_verbose && !opt_list) return;
    if (!opt_verbose) {
        printf("%s\n", name);
        return;
    }
    if (opt_list) {
        char m[11];
        const char *rwx = "rwxrwxrwx";
        int i;
        m[0] = type;
        for (i = 0; i < 9; i++) m[1 + i] = (mode & (1u << (8 - i))) ? rwx[i] : '-';
        m[10] = 0;
        printf("%s %u %s", m, size, name);
        if (link && type == 'l') printf(" -> %s", link);
        printf("\n");
        return;
    }
    if (link && type == 'l') printf("%s -> %s\n", name, link);
    else printf("%s\n", name);
}

static int take_override(struct src *s, unsigned size, char *dst, int cap, int nul) {
    unsigned char *buf;
    unsigned ncopy;
    int rc;
    if (size == 0 || size >= (unsigned)cap) {
        fprintf(stderr, "tar: name is too long\n");
        return skip_bytes(s, size) < 0 ? -1 : -2;
    }
    buf = malloc(size + 1);
    if (!buf) return -1;
    rc = 0;
    {
        unsigned rest = (size + 511u) & ~511u, filled = 0;
        while (rest) {
            unsigned char blk[512];
            if (src_exact(s, blk, 512) < 0) { free(buf); return -1; }
            if (filled < size) {
                unsigned n = 512;
                if (filled + n > size) n = size - filled;
                memcpy(buf + filled, blk, n);
            }
            filled += 512;
            rest -= 512;
        }
    }
    ncopy = size;
    if (nul && ncopy && buf[ncopy - 1] == 0) ncopy--;
    if (ncopy >= (unsigned)cap) { free(buf); fprintf(stderr, "tar: name is too long\n"); return -2; }
    memcpy(dst, buf, ncopy);
    dst[ncopy] = 0;
    free(buf);
    return rc;
}

static int run_archive(struct src *s) {
    struct saved global, local;
    char longname[NAME_CAP], longlink[NAME_CAP];
    int have_longname = 0, have_longlink = 0, rc = 0;
    saved_clear(&global);
    saved_clear(&local);
    for (;;) {
        unsigned char h[512];
        unsigned sum_unused, size, mode, mtime;
        int zero = 0, ok, kind, pr;
        char name[NAME_CAP], link[NAME_CAP], dest[NAME_CAP], raw[NAME_CAP];
        const char *np, *lp;
        (void)sum_unused;
        if (src_exact(s, h, 512) < 0) {
            short_archive();
            return 1;
        }
        header_sum(h, &sum_unused, &zero);
        if (zero) break;
        ok = header_ok(h);
        if (ok < 0) { fprintf(stderr, "tar: checksum error\n"); return 1; }
        kind = h[156];
        if (parse_u32((const char *)h + 124, 12, &size) < 0) {
            fprintf(stderr, "tar: file is too large\n");
            return 1;
        }
        if (kind == 'L' || kind == 'K') {
            char *dst = kind == 'L' ? longname : longlink;
            int r = take_override(s, size, dst, NAME_CAP, 1);
            if (r == -1) return 1;
            if (r == 0) {
                if (kind == 'L') have_longname = 1;
                else have_longlink = 1;
            }
            continue;
        }
        if (kind == 'x' || kind == 'g') {
            unsigned char *body;
            unsigned rest, filled = 0;
            if (size > PAX_MAX) {
                fprintf(stderr, "tar: pax header is too large\n");
                return 1;
            }
            body = malloc(size + 1);
            if (!body) return 1;
            rest = (size + 511u) & ~511u;
            while (rest) {
                unsigned char blk[512];
                if (src_exact(s, blk, 512) < 0) { free(body); short_archive(); return 1; }
                if (filled < size) {
                    unsigned n = filled + 512 > size ? size - filled : 512;
                    memcpy(body + filled, blk, n);
                }
                filled += 512;
                rest -= 512;
            }
            body[size] = 0;
            if (pax_parse((char *)body, size, kind == 'g' ? &global : &local) < 0) {
                free(body);
                fprintf(stderr, "tar: malformed pax header\n");
                return 1;
            }
            free(body);
            continue;
        }
        field_copy(name, (int)sizeof name, (const char *)h, 100);
        field_copy(link, (int)sizeof link, (const char *)h + 157, 100);
        if (memcmp(h + 257, "ustar", 5) == 0) {
            char prefix[156];
            field_copy(prefix, (int)sizeof prefix, (const char *)h + 345, 155);
            if (prefix[0] && !have_longname && !local.has_path && !global.has_path) {
                if (strlen(prefix) + strlen(name) + 2 > sizeof name) {
                    fprintf(stderr, "tar: name is too long\n");
                    rc = 1;
                    if (skip_bytes(s, size) < 0) return 1;
                    continue;
                }
                {
                    char joined[NAME_CAP];
                    snprintf(joined, sizeof joined, "%s/%s", prefix, name);
                    memcpy(name, joined, strlen(joined) + 1);
                }
            }
        }
        np = local.has_path ? local.path : global.has_path ? global.path : have_longname ? longname : name;
        lp = local.has_link ? local.link : global.has_link ? global.link : have_longlink ? longlink : link;
        if (local.has_size) size = local.size;
        mode = 0;
        parse_u32((const char *)h + 100, 8, &mode);
        mtime = 0;
        parse_u32((const char *)h + 136, 12, &mtime);
        if (local.has_mtime) mtime = local.mtime;
        else if (global.has_mtime) mtime = global.mtime;
        memcpy(raw, np, strlen(np) + 1);
        have_longname = have_longlink = 0;
        saved_clear(&local);
        if (!name_wanted(raw)) {
            if (skip_bytes(s, size) < 0) { short_archive(); return 1; }
            continue;
        }
        pr = place_name(dest, NAME_CAP, raw, opt_strip, opt_absolute);
        if (pr < 0) {
            fprintf(stderr, "tar: %s: member name contains '..' or is too long\n", raw);
            rc = 1;
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (pr > 0) {
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '5') {
            if (!opt_list) {
                if (mkdir_p(dest) < 0) rc = 1;
                else directory_header_mode(dest,mode,mtime);
            }
            show('d', mode, size, dest, NULL);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '2') {
            char target[NAME_CAP];
            if (strlen(lp) >= sizeof target) {
                fprintf(stderr, "tar: %s: link target is too long\n", dest);
                rc = 1;
                if (skip_bytes(s, size) < 0) return 1;
                continue;
            }
            memcpy(target, lp, strlen(lp) + 1);
            if (!opt_list) {
                if (!(opt_keep && exists_keep(dest))) {
                    if (ensure_parent(dest) < 0) rc = 1;
                    else {
                        if (exists_keep(dest)) unlink(dest);
                        if (symlink(target, dest) < 0) { warn_sys(dest, -1); rc = 1; }
                    }
                }
            }
            show('l', mode, size, dest, target);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '1') {
            char target[NAME_CAP];
            int tr = place_name(target, NAME_CAP, lp, opt_strip, opt_absolute);
            if (!opt_list && !(opt_keep && exists_keep(dest))) {
                if (tr != 0 || copy_file(target, dest) < 0) {
                    fprintf(stderr, "tar: %s: skipping hard link to %s\n", dest, lp);
                    rc = 1;
                }
            }
            show('h', mode, size, dest, lp);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '3' || kind == '4' || kind == '6') {
            fprintf(stderr, "tar: %s: skipping special file\n", dest);
            show(kind == '6' ? 'p' : 'c', mode, size, dest, NULL);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind != '0' && kind != 0 && kind != '7') {
            fprintf(stderr, "tar: %s: unknown file type '%c'\n", dest, kind ? kind : '0');
            rc = 1;
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (!opt_list) {
            if (opt_keep && exists_keep(dest)) {
                if (skip_bytes(s, size) < 0) return 1;
                continue;
            }
            if (exists_keep(dest)) unlink(dest);
            show('-', mode, size, dest, NULL);
            {
                int wr = write_member(s,dest,size,mode,mtime);
                if (wr == -2) { short_archive(); return 1; }
                if (wr < 0) { warn_sys(dest, -1); rc = 1; }
            }
        } else {
            show('-', mode, size, dest, NULL);
            if (skip_bytes(s, size) < 0) return 1;
        }
    }
    return rc;
}

/* ------------------------------------------------------- флаги GNU tar */

static void usage(void) {
    fprintf(stderr,
            "usage: tar -x|-t [-z] [-v] [-k] [-P] [-C DIR] [-f ARCHIVE]\n"
            "           [--strip-components=N] [--gzip] [MEMBER...]\n"
            "       tar xf ARCHIVE, tar xzf ARCHIVE.tgz\n");
}

static int need_op;
static int bad_op;

static int set_op(int list) {
    if (need_op && opt_list != list) { bad_op = 1; return -1; }
    need_op = 1;
    opt_list = list;
    return 0;
}

static int apply_short(int c, const char *arg) {
    if (c == 'x') return set_op(0);
    if (c == 't') return set_op(1);
    if (c == 'c') {
        if (need_op && !opt_create) { bad_op = 1; return -1; }
        need_op = 1;
        opt_create = 1;
        return 0;
    }
    if (c == 'r' || c == 'u' || c == 'd' || c == 'A') {
        fprintf(stderr, "tar: operation '%c' is not supported\n", c);
        return -1;
    }
    if (c == 'f') { if (!arg) return -1; opt_file = arg; return 0; }
    if (c == 'C') { if (!arg) return -1; opt_dir = arg; return 0; }
    if (c == 'z') { opt_gzip = 1; return 0; }
    if (c == 'v') { opt_verbose++; return 0; }
    if (c == 'k') { opt_keep = 1; return 0; }
    if (c == 'P') { opt_absolute = 1; return 0; }
    if (c == 'a' || c == 'm' || c == 'p' || c == 'o' || c == 'h') return 0;
    if (c == 'j' || c == 'J') {
        fprintf(stderr, "tar: option '%c' is not supported\n", c);
        return -1;
    }
    fprintf(stderr, "tar: unknown option '%c'\n", c);
    return -1;
}

static int parse_cluster(const char *s, int *i, int argc, char **argv) {
    int k = 0;
    while (s[k]) {
        char c = s[k++];
        const char *arg = NULL;
        if (c == 'f' || c == 'C') {
            if (s[k]) { arg = s + k; k += (int)strlen(s + k); }
            else if (*i + 1 < argc) arg = argv[++*i];
            else { fprintf(stderr, "tar: option '%c' requires an argument\n", c); return -1; }
        }
        if (apply_short(c, arg) < 0) return -1;
    }
    return 0;
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static int parse_long(const char *s, int *i, int argc, char **argv) {
    const char *val = NULL;
    const char *name = s;
    if (strcmp(s, "--") == 0) return 1;
    if (starts(s, "--") && strchr(s, '=')) {
        /* значение после «=» */
    }
    if (!strcmp(s, "--help")) { usage(); exit(0); }
    if (!strcmp(s, "--version")) { printf("tar (MyOS)\n"); exit(0); }
    if (!strcmp(s, "--extract") || !strcmp(s, "--get")) return set_op(0);
    if (!strcmp(s, "--list")) return set_op(1);
    if (!strcmp(s, "--create")) {
        if (need_op && !opt_create) { bad_op = 1; return -1; }
        need_op = 1;
        opt_create = 1;
        return 0;
    }
    if (!strcmp(s, "--append") || !strcmp(s, "--update") ||
        !strcmp(s, "--diff") || !strcmp(s, "--compare") || !strcmp(s, "--concatenate") ||
        !strcmp(s, "--catenate")) {
        fprintf(stderr, "tar: %s is not supported\n", s);
        return -1;
    }
    if (!strcmp(s, "--gzip") || !strcmp(s, "--gunzip") || !strcmp(s, "--ungzip")) { opt_gzip = 1; return 0; }
    if (!strcmp(s, "--verbose")) { opt_verbose++; return 0; }
    if (!strcmp(s, "--keep-old-files") || !strcmp(s, "--skip-old-files")) { opt_keep = 1; return 0; }
    if (!strcmp(s, "--absolute-names")) { opt_absolute = 1; return 0; }
    if (!strcmp(s, "--overwrite")) { opt_keep = 0; return 0; }
    if (!strcmp(s, "--auto-compress") || !strcmp(s, "--touch") ||
        !strcmp(s, "--preserve-permissions") || !strcmp(s, "--same-permissions") ||
        !strcmp(s, "--no-same-owner")) return 0;
    if (!strcmp(s, "--bzip2") || !strcmp(s, "--xz")) {
        fprintf(stderr, "tar: %s is not supported\n", s);
        return -1;
    }
    name = s;
    if (starts(s, "--file")) name = "--file";
    else if (starts(s, "--directory")) name = "--directory";
    else if (starts(s, "--strip-components")) name = "--strip-components";
    else { fprintf(stderr, "tar: unknown option '%s'\n", s); return -1; }
    if (s[strlen(name)] == '=') val = s + strlen(name) + 1;
    else if (s[strlen(name)] == 0) {
        if (*i + 1 >= argc) { fprintf(stderr, "tar: %s requires an argument\n", name); return -1; }
        val = argv[++*i];
    } else { fprintf(stderr, "tar: unknown option '%s'\n", s); return -1; }
    if (!strcmp(name, "--file")) { opt_file = val; return 0; }
    if (!strcmp(name, "--directory")) { opt_dir = val; return 0; }
    opt_strip = 0;
    if (!val || !*val) { fprintf(stderr, "tar: --strip-components requires a number\n"); return -1; }
    while (*val) {
        if (*val < '0' || *val > '9' || opt_strip > 100000) {
            fprintf(stderr, "tar: --strip-components requires a number\n");
            return -1;
        }
        opt_strip = opt_strip * 10 + (*val - '0');
        val++;
    }
    return 0;
}

static int old_style(const char *s) {
    int i, op = 0;
    if (!s || !s[0]) return 0;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (c == 'x' || c == 't' || c == 'c' || c == 'r' || c == 'u' || c == 'd' || c == 'A') op = 1;
        else if (!strchr("zvkaPmpohfCjJ", c)) return 0;
    }
    return op;
}

int main(int argc, char **argv) {
    int i = 1, fd = -1, nfirst, rc;
    unsigned char first[512];
    struct src src;
    struct gz_in *gz = NULL;
    if (argc > 1 && argv[1][0] != '-' && old_style(argv[1])) {
        if (parse_cluster(argv[1], &i, argc, argv) < 0) return 2;
        i++;
    }
    for (; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            int r;
            if (argv[i][1] == '-') {
                if (!argv[i][2]) { i++; break; }
                r = parse_long(argv[i], &i, argc, argv);
                if (r < 0) return 2;
                continue;
            }
            if (parse_cluster(argv[i] + 1, &i, argc, argv) < 0) return 2;
            continue;
        }
        break;
    }
    if (bad_op) { fprintf(stderr, "tar: you may not specify more than one operation\n"); return 2; }
    if (!need_op) { usage(); return 2; }
    opt_members = argv + i;
    opt_nmem = argc - i;
    if (opt_create) return run_create();
    if (!opt_file) { fprintf(stderr, "tar: option -f is required\n"); return 2; }
    if (!strcmp(opt_file, "-")) fd = 0;
    else {
        fd = open(opt_file, O_RDONLY);
        if (fd < 0) { warn_sys(opt_file, -1); return 2; }
    }
    memset(&src, 0, sizeof src);
    src.fd = fd;
    nfirst = src_read(&src, first, 512);
    if (nfirst < 0) { warn_sys(opt_file, -1); return 2; }
    if (nfirst >= 2 && first[0] == 0x1f && first[1] == 0x8b) opt_gzip = 1;
    else if (opt_gzip) { fprintf(stderr, "tar: %s: not in gzip format\n", opt_file); return 2; }
    if (opt_gzip) {
        gz = calloc(1, sizeof *gz);
        if (!gz) {
            fprintf(stderr, "tar: out of memory\n");
            if (fd > 0) close(fd);
            return 2;
        }
        gz->fd = fd;
        if (nfirst > (int)sizeof gz->pre) nfirst = (int)sizeof gz->pre;
        if (nfirst > 0) memcpy(gz->pre, first, (size_t)nfirst);
        gz->pre_len = nfirst;
        memset(&src, 0, sizeof src);
        src.gz = gz;
    } else if (nfirst == 512) {
        memcpy(src.peek, first, 512);
        src.peek_len = 512;
    } else if (nfirst == 0) {
        if (fd > 0) close(fd);
        return 0;
    } else {
        fprintf(stderr, "tar: unexpected end of file\n");
        if (fd > 0) close(fd);
        return 2;
    }
    if (opt_dir) {
        int cr = chdir(opt_dir);
        if (cr < 0) {
            warn_sys(opt_dir, cr);
            gz_close(gz);
            if (fd > 0) close(fd);
            return 2;
        }
    }
    extraction_mask=umask(0);umask(extraction_mask);
    rc = run_archive(&src);
    if (src.gz) {
        unsigned char sink[4096];
        int n = 0;
        while (!gz_msg && (n = src_read(&src, sink, (int)sizeof sink)) > 0) {}
        if (gz_msg || n < 0) rc = 1;
    }
    if (gz_msg && !gz_msg_printed) {
        fprintf(stderr, "tar: %s: %s\n", opt_file, gz_msg);
        rc = 1;
    }
    rc |= finish_directory_modes(!rc);
    gz_close(gz);
    if (fd > 0) close(fd);
    return rc ? 1 : 0;
}

/* ------------------------------------------------------- tar cf (ustar) */

static int create_fd;

static int create_write(const void *buf, unsigned n) {
    const unsigned char *p = buf;
    unsigned off = 0;
    while (off < n) {
        int w = write(create_fd, p + off, n - off);
        if (w <= 0) return -1;
        off += (unsigned)w;
    }
    return 0;
}

static void put_octal(char *dst, int n, unsigned long v) {
    int i;
    memset(dst, '0', (size_t)n);
    dst[n - 1] = 0;
    for (i = n - 2; i >= 0; i--) {
        dst[i] = (char)('0' + (v & 7ul));
        v >>= 3;
        if (!v) break;
    }
}

static int write_ustar(const char *name, unsigned mode, unsigned size, unsigned mtime,
                       int type, const char *link) {
    unsigned char h[512];
    unsigned sum = 0;
    int i, nlen = (int)strlen(name);
    const char *base = name, *pref = "";
    int plen = 0;
    memset(h, 0, sizeof h);
    if (nlen > 100) {
        const char *slash = NULL;
        for (i = 0; name[i] && i < nlen - 1; i++)
            if (name[i] == '/' && nlen - (i + 1) <= 100 && i <= 155) slash = name + i;
        if (!slash || slash - name > 155) {
            fprintf(stderr, "tar: %s: name is too long\n", name);
            return -1;
        }
        plen = (int)(slash - name);
        pref = name;
        base = slash + 1;
        nlen = (int)strlen(base);
    }
    memcpy(h, base, (size_t)nlen);
    if (plen) memcpy(h + 345, pref, (size_t)plen);
    put_octal((char *)h + 100, 8, mode & 07777);
    put_octal((char *)h + 108, 8, 0);
    put_octal((char *)h + 116, 8, 0);
    put_octal((char *)h + 124, 12, size);
    put_octal((char *)h + 136, 12, mtime);
    memset(h + 148, ' ', 8);
    h[156] = (unsigned char)type;
    if (link) {
        int ln = (int)strlen(link);
        if (ln > 100) { fprintf(stderr, "tar: %s: link target is too long\n", name); return -1; }
        memcpy(h + 157, link, (size_t)ln);
    }
    memcpy(h + 257, "ustar", 5);
    h[262] = 0; h[263] = '0'; h[264] = '0';
    for (i = 0; i < 512; i++) sum += h[i];
    put_octal((char *)h + 148, 7, sum);
    h[155] = ' ';
    return create_write(h, 512);
}

static int create_file(const char *path, const char *store, struct stat *st) {
    int fd, n;
    unsigned filled = 0, size = st->st_size;
    if (write_ustar(store, st->st_mode, size, (unsigned)st->st_mtime, '0', NULL) < 0) return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) { warn_sys(path, -1); return -1; }
    while (filled < size) {
        unsigned char buf[4096];
        unsigned want = size - filled;
        if (want > sizeof buf) want = (unsigned)sizeof buf;
        n = read(fd, buf, want);
        if (n <= 0) { close(fd); warn_sys(path, -1); return -1; }
        if (create_write(buf, (unsigned)n) < 0) { close(fd); return -1; }
        filled += (unsigned)n;
    }
    close(fd);
    if (size % 512u) {
        unsigned char pad[512];
        memset(pad, 0, sizeof pad);
        if (create_write(pad, 512u - (size % 512u)) < 0) return -1;
    }
    return 0;
}

static int create_add(const char *path, const char *store);

static int create_dir(const char *path, const char *store, struct stat *st) {
    DIR *d;
    struct dirent *de;
    char child[NAME_CAP], child_store[NAME_CAP];
    int slen = (int)strlen(store), rc = 0;
    if (write_ustar(store, st->st_mode, 0, (unsigned)st->st_mtime, '5', NULL) < 0) return -1;
    d = opendir(path);
    if (!d) { warn_sys(path, -1); return -1; }
    while ((de = readdir(d))) {
        int npath, nstore;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        npath = (int)strlen(path) + 1 + (int)strlen(de->d_name);
        nstore = slen + 1 + (int)strlen(de->d_name);
        if (npath >= NAME_CAP || nstore >= NAME_CAP) {
            fprintf(stderr, "tar: name is too long\n");
            rc = 1;
            continue;
        }
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        if (slen && store[slen - 1] == '/')
            snprintf(child_store, sizeof child_store, "%s%s", store, de->d_name);
        else
            snprintf(child_store, sizeof child_store, "%s/%s", store, de->d_name);
        if (create_add(child, child_store) < 0) rc = 1;
    }
    closedir(d);
    return rc;
}

static int create_add(const char *path, const char *store) {
    struct stat st;
    char link[NAME_CAP];
    if (lstat(path, &st) < 0) { warn_sys(path, -1); return -1; }
    if (S_ISDIR(st.st_mode)) return create_dir(path, store, &st);
    if (S_ISLNK(st.st_mode)) {
        int n = (int)readlink(path, link, sizeof link - 1);
        if (n < 0) { warn_sys(path, -1); return -1; }
        link[n] = 0;
        return write_ustar(store, st.st_mode, 0, (unsigned)st.st_mtime, '2', link);
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "tar: %s: skipping special file\n", path);
        return 0;
    }
    return create_file(path, store, &st);
}

static int run_create(void) {
    static unsigned char zblk[1024];
    int i, rc = 0, out;
    if (!opt_file) { fprintf(stderr, "tar: option -f is required\n"); return 2; }
    if (opt_nmem < 1) { fprintf(stderr, "tar: create requires a member\n"); return 2; }
    if (!strcmp(opt_file, "-")) out = 1;
    else {
        out = open(opt_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out < 0) { warn_sys(opt_file, -1); return 2; }
    }
    create_fd = out;
    if (opt_dir) {
        int cr = chdir(opt_dir);
        if (cr < 0) {
            warn_sys(opt_dir, cr);
            if (out > 1) close(out);
            return 2;
        }
    }
    for (i = 0; i < opt_nmem; i++) {
        const char *p = opt_members[i];
        if (create_add(p, p) < 0) rc = 1;
    }
    memset(zblk, 0, sizeof zblk);
    if (create_write(zblk, 1024) < 0) rc = 1;
    if (out > 1) close(out);
    return rc ? 1 : 0;
}
