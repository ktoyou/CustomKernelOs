/* expr — арифметика и оператор «:» (базовое регулярное выражение). */
#include "myos.h"

static const char *caps, *cape;
static int have_cap;

static int atom_ok(const char *pat, const char *s, const char **next) {
    if (*pat == '.') { *next = pat + 1; return *s != 0; }
    if (*pat == '\\' && pat[1]) { *next = pat + 2; return *s == pat[1]; }
    if (*pat == '[') {
        int neg = 0, ok = 0;
        pat++;
        if (*pat == '^' || *pat == '!') { neg = 1; pat++; }
        while (*pat && *pat != ']') {
            if (pat[1] == '-' && pat[2] && pat[2] != ']') {
                if (*s && *s >= *pat && *s <= pat[2]) ok = 1;
                pat += 3;
            } else { if (*s && *s == *pat) ok = 1; pat++; }
        }
        if (*pat == ']') pat++;
        *next = pat;
        return *s && ok != neg;
    }
    *next = pat + 1;
    return *s && *s == *pat;
}

/* Длина совпадения с начала строки или -1. Одна пара \( \) запоминается. */
static int match_len(const char *pat, const char *s) {
    if (*pat == 0) return 0;
    if (*pat == '$' && pat[1] == 0) return *s ? -1 : 0;
    if (*pat == '\\' && pat[1] == '(') {
        const char *start = s;
        int n = match_len(pat + 2, s);
        if (n < 0) return -1;
        if (!have_cap) { caps = start; cape = start + n; have_cap = 1; }
        return n;
    }
    if (*pat == '\\' && pat[1] == ')') return match_len(pat + 2, s);
    const char *restp;
    int star = 0;
    if (*pat == '\\' && pat[1] && pat[1] != '(' && pat[1] != ')' && pat[2] == '*') {
        star = 1; restp = pat + 3;
    } else if (*pat != '\\' && pat[1] == '*') {
        star = 1; restp = pat + 2;
    } else {
        const char *np;
        if (!atom_ok(pat, s, &np)) return -1;
        int r = match_len(np, s + 1);
        return r < 0 ? -1 : r + 1;
    }
    int max = 0;
    const char *q = s, *dummy;
    while (*q && atom_ok(pat, q, &dummy)) { q++; max++; }
    for (int n = max; n >= 0; n--) {
        int r = match_len(restp, s + n);
        if (r >= 0) return n + r;
    }
    (void)star;
    return -1;
}

static int colon(const char *str, const char *pat) {
    have_cap = 0; caps = cape = NULL;
    int n = match_len(pat, str);
    if (n < 0) { printf("0\n"); return 1; }
    if (have_cap) {
        int len = (int)(cape - caps);
        if (len == 0) { printf("\n"); return 1; }
        printf("%.*s\n", len, caps);
        return 0;
    }
    if (n == 0) { printf("0\n"); return 1; }
    printf("%d\n", n);
    return 0;
}

static int num(const char *s, int *v) {
    if (!s || !*s) return 0;
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (!*p) return 0;
    while (*p) { if (*p < '0' || *p > '9') return 0; p++; }
    *v = atoi(s);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: expr ARG...\n"); return 2; }
    if (argc == 4 && !strcmp(argv[2], ":")) return colon(argv[1], argv[3]);
    if (argc == 4) {
        int a, b;
        if (!num(argv[1], &a) || !num(argv[3], &b)) { dprintf(STDERR, "expr: non-numeric argument\n"); return 2; }
        int r = 0;
        if (!strcmp(argv[2], "+")) r = a + b;
        else if (!strcmp(argv[2], "-")) r = a - b;
        else if (!strcmp(argv[2], "*")) r = a * b;
        else if (!strcmp(argv[2], "/")) { if (!b) return 2; r = a / b; }
        else if (!strcmp(argv[2], "%")) { if (!b) return 2; r = a % b; }
        else { dprintf(STDERR, "expr: unknown operator\n"); return 2; }
        printf("%d\n", r);
        return r == 0;
    }
    if (argc == 2) {
        printf("%s\n", argv[1]);
        return argv[1][0] == 0 || (argv[1][0] == '0' && !argv[1][1]);
    }
    dprintf(STDERR, "usage: expr ARG...\n");
    return 2;
}
