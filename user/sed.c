/* sed [-n] [-e SCRIPT] — s, d, p, x и адреса /регэкс/. */
#include "myos.h"

enum { C_S, C_D, C_P, C_X, C_BRA, C_KET };
struct cmd { int op, g, pflag, line; char *addr, *pat, *rep; };
static struct cmd *cmds;
static int nc, ncap, opt_n;

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { dprintf(STDERR, "sed: out of memory\n"); exit(1); }
    return p;
}
static char *xstrdup(const char *s) {
    char *p = xmalloc(strlen(s) + 1);
    memcpy(p, s, strlen(s) + 1);
    return p;
}
static void add_cmd(struct cmd c) {
    if (nc == ncap) { ncap = ncap ? ncap * 2 : 8; cmds = realloc(cmds, (size_t)ncap * sizeof *cmds); }
    cmds[nc++] = c;
}

static int atom_ok(const char *pat, const char *s, const char **next) {
    if (*pat == '.') { *next = pat + 1; return *s != 0; }
    if (*pat == '[') {
        int neg = 0, ok = 0;
        pat++;
        if (*pat == '^') { neg = 1; pat++; }
        if (*pat == ']') { if (*s == ']') ok = 1; pat++; }
        while (*pat && *pat != ']') {
            if (pat[1] == '-' && pat[2] && pat[2] != ']') {
                if (*s && (unsigned char)*s >= (unsigned char)*pat && (unsigned char)*s <= (unsigned char)pat[2]) ok = 1;
                pat += 3;
            } else { if (*s && *s == *pat) ok = 1; pat++; }
        }
        if (*pat == ']') pat++;
        *next = pat;
        return *s && ok != neg;
    }
    if (*pat == '\\' && pat[1]) { *next = pat + 2; return *s == pat[1]; }
    *next = pat + 1;
    return *s && *s == *pat;
}
static const char *atom_end(const char *pat) {
    if (*pat == '\\' && pat[1]) return pat + 2;
    if (*pat == '[') {
        pat++;
        if (*pat == '^') pat++;
        if (*pat == ']') pat++;
        while (*pat && *pat != ']') pat++;
        if (*pat == ']') pat++;
        return pat;
    }
    return pat + 1;
}
static int re_len(const char *pat, const char *s) {
    if (!*pat) return 0;
    if (*pat == '$') {
        const char *rest = pat + 1;
        while (rest[0] == '\\' && rest[1] == ')') rest += 2;
        if (!*rest) return *s ? -1 : 0;
    }
    if (*pat == '^') return -1;
    /* \( \) — границы группы. Для сценариев сборки группа совпадает со всем совпадением. */
    if (*pat == '\\' && (pat[1] == '(' || pat[1] == ')')) return re_len(pat + 2, s);
    const char *ae = atom_end(pat);
    int star = *ae == '*';
    if (!star) {
        const char *np;
        if (!atom_ok(pat, s, &np)) return -1;
        int r = re_len(np, s + 1);
        return r < 0 ? -1 : r + 1;
    }
    const char *rest = ae + 1;
    int max = 0;
    const char *q = s, *dummy;
    while (*q && atom_ok(pat, q, &dummy)) { q++; max++; }
    for (int n = max; n >= 0; n--) {
        int r = re_len(rest, s + n);
        if (r >= 0) return n + r;
    }
    return -1;
}
static int re_find(const char *pat, const char *s, int *mb, int *ml) {
    int anchor = pat[0] == '^';
    if (anchor) pat++;
    for (int i = 0; ; i++) {
        int n = re_len(pat, s + i);
        if (n >= 0) { *mb = i; *ml = n; return 1; }
        if (anchor || !s[i]) break;
    }
    return 0;
}

static char *space, *hs;
static void setstr(char **d, const char *s) { free(*d); *d = xstrdup(s ? s : ""); }

static int subst(struct cmd *c) {
    struct buf { char *s; int n, cap; } b;
    b.s = xmalloc(64); b.s[0] = 0; b.n = 0; b.cap = 64;
    int pos = 0, any = 0;
    while (space[pos]) {
        int mb, ml;
        if (!re_find(c->pat, space + pos, &mb, &ml) || (any && !c->g)) {
            /* дописать хвост */
            int len = (int)strlen(space + pos);
            while (b.n + len + 1 > b.cap) { b.cap *= 2; b.s = realloc(b.s, (size_t)b.cap); }
            memcpy(b.s + b.n, space + pos, (size_t)len); b.n += len; b.s[b.n] = 0;
            break;
        }
        while (b.n + mb + 1 > b.cap) { b.cap *= 2; b.s = realloc(b.s, (size_t)b.cap); }
        memcpy(b.s + b.n, space + pos, (size_t)mb); b.n += mb;
        const char *m = space + pos + mb;
        for (const char *r = c->rep; *r; r++) {
            const char *add = NULL; int an = 0; char one;
            if (*r == '&') { add = m; an = ml; }
            else if (*r == '\\' && r[1] >= '1' && r[1] <= '9') { add = m; an = ml; r++; }
            else if (*r == '\\' && r[1] == 'n') { one = '\n'; add = &one; an = 1; r++; }
            else if (*r == '\\' && r[1]) { one = r[1]; add = &one; an = 1; r++; }
            else { one = *r; add = &one; an = 1; }
            while (b.n + an + 1 > b.cap) { b.cap *= 2; b.s = realloc(b.s, (size_t)b.cap); }
            memcpy(b.s + b.n, add, (size_t)an); b.n += an; b.s[b.n] = 0;
        }
        any = 1;
        int step = ml > 0 ? ml : 1;
        pos += mb + step;
        if (!c->g) {
            int len = (int)strlen(space + pos);
            while (b.n + len + 1 > b.cap) { b.cap *= 2; b.s = realloc(b.s, (size_t)b.cap); }
            memcpy(b.s + b.n, space + pos, (size_t)len); b.n += len; b.s[b.n] = 0;
            break;
        }
        if (ml == 0 && !space[pos]) break;
    }
    if (!any && space[0] == 0) { free(b.s); return 0; }
    free(space); space = b.s;
    return any;
}

static void print_ps(void) { printf("%s\n", space); }

static int skip_bra(int i) {
    int d = 0;
    for (; i < nc; i++) {
        if (cmds[i].op == C_BRA) d++;
        else if (cmds[i].op == C_KET && --d == 0) return i;
    }
    return nc;
}

static void apply_line(int lineno) {
    int deleted = 0;
    for (int i = 0; i < nc && !deleted; i++) {
        if (cmds[i].line && cmds[i].line != lineno) {
            if (cmds[i].op == C_BRA) i = skip_bra(i);
            continue;
        }
        if (cmds[i].addr) {
            int mb, ml;
            if (!re_find(cmds[i].addr, space, &mb, &ml)) {
                if (cmds[i].op == C_BRA) i = skip_bra(i);
                continue;
            }
        }
        if (cmds[i].op == C_S) { if (subst(&cmds[i]) && cmds[i].pflag) print_ps(); }
        else if (cmds[i].op == C_D) deleted = 1;
        else if (cmds[i].op == C_P) print_ps();
        else if (cmds[i].op == C_X) { char *t = space; space = hs; hs = t; if (!space) space = xstrdup(""); if (!hs) hs = xstrdup(""); }
    }
    if (!deleted && !opt_n) print_ps();
}

static char *take_until(const char **pp, char delim) {
    struct { char *s; int n, cap; } b;
    b.s = xmalloc(32); b.n = 0; b.cap = 32; b.s[0] = 0;
    const char *p = *pp;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1]) {
            if (p[1] != delim) {
                if (b.n + 3 > b.cap) { b.cap *= 2; b.s = realloc(b.s, (size_t)b.cap); }
                b.s[b.n++] = '\\';
            }
            p++;
        }
        if (b.n + 2 > b.cap) { b.cap *= 2; b.s = realloc(b.s, (size_t)b.cap); }
        b.s[b.n++] = *p++;
        b.s[b.n] = 0;
    }
    if (*p == delim) p++;
    *pp = p;
    return b.s;
}

static void parse_script(const char *s) {
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;
        if (!*p) break;
        struct cmd c; memset(&c, 0, sizeof c);
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9') n = n * 10 + *p++ - '0';
            c.line = n;
        } else if (*p == '/') {
            p++;
            c.addr = take_until(&p, '/');
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '{') { c.op = C_BRA; add_cmd(c); p++; continue; }
        if (*p == '}') { c.op = C_KET; add_cmd(c); p++; continue; }
        if (*p == 's') {
            p++;
            if (!*p) break;
            char d = *p++;
            c.op = C_S;
            c.pat = take_until(&p, d);
            c.rep = take_until(&p, d);
            while (*p == 'g' || *p == 'p') { if (*p == 'g') c.g = 1; else c.pflag = 1; p++; }
            add_cmd(c);
            continue;
        }
        if (*p == 'd' || *p == 'p' || *p == 'x') {
            c.op = *p == 'd' ? C_D : *p == 'p' ? C_P : C_X;
            add_cmd(c); p++;
            continue;
        }
        dprintf(STDERR, "sed: unsupported command '%c'\n", *p);
        exit(1);
    }
}

static void run_fd(int fd) {
    char *buf = NULL; int n = 0, cap = 0, lines = 0;
    char ch;
    while (read(fd, &ch, 1) == 1) {
        if (ch == '\n') {
            if (n + 1 > cap) { cap = n + 64; buf = realloc(buf, (size_t)cap); }
            buf[n] = 0;
            setstr(&space, buf);
            apply_line(++lines);
            n = 0;
            continue;
        }
        if (n + 2 > cap) { cap = cap ? cap * 2 : 128; buf = realloc(buf, (size_t)cap); }
        buf[n++] = ch;
    }
    if (n) {
        buf[n] = 0;
        setstr(&space, buf);
        apply_line(++lines);
    }
    free(buf);
}

int main(int argc, char **argv) {
    hs = xstrdup(""); space = xstrdup("");
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) opt_n = 1;
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) parse_script(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) {
            int nflag = 0, eflag = 0, ok = 1;
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'n') nflag = 1;
                else if (*f == 'e') eflag = 1;
                else ok = 0;
            }
            if (!ok) { dprintf(STDERR, "sed: bad option %s\n", argv[i]); return 1; }
            if (nflag) opt_n = 1;
            if (eflag) {
                if (i + 1 >= argc) { dprintf(STDERR, "sed: -e needs a script\n"); return 1; }
                parse_script(argv[++i]);
            }
        }
        else break;
    }
    if (!nc && i < argc) parse_script(argv[i++]);
    if (!nc) { dprintf(STDERR, "usage: sed [-n] [-e script] [file...]\n"); return 1; }
    if (i == argc) run_fd(STDIN);
    for (; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) { dprintf(STDERR, "sed: %s: %s\n", argv[i], strerror(errno)); return 1; }
        run_fd(fd);
        close(fd);
    }
    return 0;
}
