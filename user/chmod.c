#include "myos.h"
#include "sys/stat.h"

static int parse_octal(const char *s, unsigned *mode) {
    unsigned m = 0;
    if (!s[0]) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '7' || m > 07777 / 8) return -1;
        m = m * 8 + (unsigned)(*p - '0');
    }
    if (m > 07777) return -1;
    *mode = m;
    return 0;
}

/* a+rx, u=rw, go-w. Пустая «кто» — все биты. */
static int apply_sym(unsigned cur, const char *spec) {
    const char *p = spec;
    while (*p) {
        unsigned who = 0, perm = 0;
        char op;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            if (*p == 'u') who |= 0700;
            else if (*p == 'g') who |= 0070;
            else if (*p == 'o') who |= 0007;
            else who |= 0777;
            p++;
        }
        if (!who) who = 0777;
        if (*p != '+' && *p != '-' && *p != '=') return -1;
        op = *p++;
        if (*p != 'r' && *p != 'w' && *p != 'x') return -1;
        while (*p == 'r' || *p == 'w' || *p == 'x') {
            if (*p == 'r') perm |= 0444;
            else if (*p == 'w') perm |= 0222;
            else perm |= 0111;
            p++;
        }
        perm &= who;
        if (op == '+') cur |= perm;
        else if (op == '-') cur &= ~perm;
        else { cur = (cur & ~who) | perm; }
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return (int)(cur & 07777);
}

int main(int argc, char **argv) {
    unsigned octal = 0;
    int symbolic, status = 0, i;
    if (argc < 3) { dprintf(STDERR, "Usage: chmod MODE FILE...\n"); return 1; }
    symbolic = parse_octal(argv[1], &octal) < 0;
    if (symbolic && apply_sym(0, argv[1]) < 0) { dprintf(STDERR, "chmod: invalid mode\n"); return 1; }
    for (i = 2; i < argc; i++) {
        unsigned mode = octal;
        if (symbolic) {
            struct stat st;
            int next;
            if (stat(argv[i], &st) < 0) {
                dprintf(STDERR, "chmod: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
            next = apply_sym(st.st_mode & 07777, argv[1]);
            if (next < 0) { dprintf(STDERR, "chmod: invalid mode\n"); return 1; }
            mode = (unsigned)next;
        }
        if (chmod(argv[i], mode) < 0) {
            dprintf(STDERR, "chmod: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
