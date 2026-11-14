/* grep [-v] [-i] [-c] [-n] [-h] pattern [file...] — базовые регулярные выражения. */
#include "myos.h"

static bool opt_v, opt_i, opt_c, opt_n, opt_h;

static int fold(int c) { return opt_i ? tolower(c) : c; }

static int atom_ok(const char *pat, const char *s, const char **next) {
    if (*pat == '.') { *next = pat + 1; return *s != 0; }
    if (*pat == '[') {
        int neg = 0, ok = 0;
        pat++;
        if (*pat == '^') { neg = 1; pat++; }
        if (*pat == ']') { if (fold(*s) == ']') ok = 1; pat++; }
        while (*pat && *pat != ']') {
            if (pat[1] == '-' && pat[2] && pat[2] != ']') {
                int ch = fold(*s), a = fold((unsigned char)*pat), b = fold((unsigned char)pat[2]);
                if (*s && ch >= a && ch <= b) ok = 1;
                pat += 3;
            } else { if (*s && fold(*s) == fold((unsigned char)*pat)) ok = 1; pat++; }
        }
        if (*pat == ']') pat++;
        *next = pat;
        return *s && ok != neg;
    }
    if (*pat == '\\' && pat[1]) { *next = pat + 2; return *s && fold(*s) == fold((unsigned char)pat[1]); }
    *next = pat + 1;
    return *s && fold(*s) == fold((unsigned char)*pat);
}
static int re_len(const char *pat, const char *s) {
    if (!*pat) return 0;
    if (*pat == '$' && !pat[1]) return *s ? -1 : 0;
    if (*pat == '^') return -1;
    int star = pat[0] && pat[0] != '\\' && pat[1] == '*';
    if (*pat == '\\' && pat[1] && pat[2] == '*') star = 1;
    if (!star) {
        const char *np;
        if (!atom_ok(pat, s, &np)) return -1;
        int r = re_len(np, s + 1);
        return r < 0 ? -1 : r + 1;
    }
    const char *rest = (*pat == '\\') ? pat + 3 : pat + 2;
    int max = 0;
    const char *q = s, *dummy;
    while (*q && atom_ok(pat, q, &dummy)) { q++; max++; }
    for (int n = max; n >= 0; n--) {
        int r = re_len(rest, s + n);
        if (r >= 0) return n + r;
    }
    return -1;
}
static int matches(const char *pat, const char *s) {
    if (!pat[0]) return 1;
    int anchor = pat[0] == '^';
    if (anchor) pat++;
    for (int i = 0; ; i++) {
        if (re_len(pat, s + i) >= 0) return 1;
        if (anchor || !s[i]) break;
    }
    return 0;
}

static int search(int fd, const char *pat, const char *name) {
    char *buf = NULL; int n = 0, cap = 0, count = 0, lineno = 0;
    char ch;
    while (read(fd, &ch, 1) == 1) {
        if (ch != '\n') {
            if (n + 2 > cap) { cap = cap ? cap * 2 : 128; buf = realloc(buf, (size_t)cap); }
            buf[n++] = ch;
            continue;
        }
        if (n + 1 > cap) { cap = n + 2; buf = realloc(buf, (size_t)cap); }
        buf[n] = 0;
        lineno++;
        int hit = matches(pat, buf);
        if (hit == opt_v) { n = 0; continue; }
        count++;
        if (!opt_c) {
            if (name && !opt_h) printf("%s:", name);
            if (opt_n) printf("%d:", lineno);
            printf("%s\n", buf);
        }
        n = 0;
    }
    free(buf);
    if (opt_c) {
        if (name && !opt_h) printf("%s:", name);
        printf("%d\n", count);
    }
    return count ? 0 : 1;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (char *o = argv[i] + 1; *o; o++) {
            if (*o == 'v') opt_v = true;
            else if (*o == 'i') opt_i = true;
            else if (*o == 'c') opt_c = true;
            else if (*o == 'n') opt_n = true;
            else if (*o == 'h') opt_h = true;
            else { dprintf(STDERR, "grep: unknown option -%c\n", *o); return 2; }
        }
    if (i >= argc) { dprintf(STDERR, "usage: grep [-vichn] pattern [file...]\n"); return 2; }
    const char *pat = argv[i++];
    if (i == argc) return search(STDIN, pat, NULL);
    bool names = argc - i > 1;
    int rc = 1;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) { dprintf(STDERR, "grep: %s: %s\n", argv[i], strerror(errno)); continue; }
        if (search(fd, pat, names ? argv[i] : NULL) == 0) rc = 0;
        close(fd);
    }
    return rc;
}
