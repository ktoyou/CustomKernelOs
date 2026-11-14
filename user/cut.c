/* cut -d DELIM -f LIST — поля строки. LIST: 2, 1-3, 2-, -3. */
#include "myos.h"

static int from[64], to[64], nr;

static int add_range(const char *s) {
    if (nr >= 64) return -1;
    int a = 1, b = -1; /* -1 = до конца */
    if (*s == '-') {
        a = 1;
        s++;
        b = atoi(s);
    } else {
        a = atoi(s);
        while (*s && *s != '-') s++;
        if (*s == '-') {
            s++;
            b = *s ? atoi(s) : -1;
        } else b = a;
    }
    if (a < 1) return -1;
    from[nr] = a;
    to[nr] = b;
    nr++;
    return 0;
}

static int selected(int field) {
    for (int i = 0; i < nr; i++)
        if (field >= from[i] && (to[i] < 0 || field <= to[i])) return 1;
    return 0;
}

static void cut_line(char *line, char delim) {
    int field = 1, any = 0;
    char *p = line;
    while (*p) {
        char *start = p;
        while (*p && *p != delim) p++;
        int take = selected(field);
        if (take) {
            if (any) write(STDOUT, &delim, 1);
            write(STDOUT, start, (size_t)(p - start));
            any = 1;
        }
        if (*p == delim) p++;
        field++;
        if (!*p) break;
    }
    write(STDOUT, "\n", 1);
}

int main(int argc, char **argv) {
    char delim = '\t';
    const char *list = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) delim = argv[++i][0];
        else if (!strncmp(argv[i], "-d", 2) && argv[i][2]) delim = argv[i][2];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) list = argv[++i];
        else if (!strncmp(argv[i], "-f", 2) && argv[i][2]) list = argv[i] + 2;
        else { dprintf(STDERR, "usage: cut -d DELIM -f LIST\n"); return 2; }
    }
    if (!list) { dprintf(STDERR, "usage: cut -d DELIM -f LIST\n"); return 2; }
    const char *q = list;
    while (*q) {
        char piece[32]; int n = 0;
        while (*q && *q != ',' && n < 31) piece[n++] = *q++;
        piece[n] = 0;
        if (add_range(piece) < 0) return 2;
        if (*q == ',') q++;
    }
    char buf[8192];
    int len = 0;
    char c;
    while (read(STDIN, &c, 1) == 1) {
        if (c == '\n') { buf[len] = 0; cut_line(buf, delim); len = 0; continue; }
        if (len < (int)sizeof buf - 1) buf[len++] = c;
    }
    if (len) { buf[len] = 0; cut_line(buf, delim); }
    return 0;
}
