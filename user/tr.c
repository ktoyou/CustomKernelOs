/* tr SET1 SET2 — замена символов. Диапазоны a-z и восьмеричные \012. */
#include "myos.h"

static int parse_set(const char *s, unsigned char *out, int max) {
    int n = 0;
    while (*s && n < max) {
        unsigned char c;
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == 'n') c = '\n';
            else if (*s == 't') c = '\t';
            else if (*s == 'r') c = '\r';
            else if (*s == '\\') c = '\\';
            else if (*s >= '0' && *s <= '7') {
                int v = 0, k = 0;
                while (*s >= '0' && *s <= '7' && k < 3) { v = v * 8 + (*s - '0'); s++; k++; }
                c = (unsigned char)v;
                s--;
            } else c = (unsigned char)*s;
            s++;
        } else if (s[1] == '-' && s[2] && s[2] != '\\') {
            unsigned char a = (unsigned char)*s, b = (unsigned char)s[2];
            if (a > b) { unsigned char t = a; a = b; b = t; }
            for (int i = a; i <= b && n < max; i++) out[n++] = (unsigned char)i;
            s += 3;
            continue;
        } else c = (unsigned char)*s++;
        out[n++] = c;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc != 3) { dprintf(STDERR, "usage: tr SET1 SET2\n"); return 2; }
    unsigned char a[512], b[512];
    int na = parse_set(argv[1], a, 512);
    int nb = parse_set(argv[2], b, 512);
    if (!na || !nb) return 2;
    unsigned char map[256];
    for (int i = 0; i < 256; i++) map[i] = (unsigned char)i;
    for (int i = 0; i < na; i++) map[a[i]] = b[i < nb ? i : nb - 1];
    char buf[4096], out[4096];
    int n;
    while ((n = read(STDIN, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) out[i] = (char)map[(unsigned char)buf[i]];
        write(STDOUT, out, (size_t)n);
    }
    return 0;
}
