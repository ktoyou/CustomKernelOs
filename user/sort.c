/* sort [-u] [file...] — побайтовая сортировка, -u убирает повторы. */
#include "myos.h"

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

static int rev;
static int cmpstr(const void *a, const void *b) {
    int c = strcmp(*(char *const *)a, *(char *const *)b);
    return rev ? -c : c;
}

struct buf { char *s; int n, cap; };
static char **lines;
static int nlines, cap;

static void add_line(char *s) {
    if (nlines == cap) {
        cap = cap ? cap * 2 : 64;
        lines = realloc(lines, (size_t)cap * sizeof *lines);
        if (!lines) { dprintf(STDERR, "sort: out of memory\n"); exit(1); }
    }
    lines[nlines++] = s;
}

static char *take_line(struct buf *b) {
    char *s = b->s;
    b->s = NULL;
    return s;
}
static void binit(struct buf *b) {
    b->s = malloc(64); if (!b->s) exit(1);
    b->s[0] = 0; b->n = 0; b->cap = 64;
}
static void bputc(struct buf *b, char c) {
    if (b->n + 2 > b->cap) {
        b->cap *= 2;
        b->s = realloc(b->s, (size_t)b->cap);
        if (!b->s) exit(1);
    }
    b->s[b->n++] = c;
    b->s[b->n] = 0;
}

static void read_fd(int fd) {
    struct buf b; binit(&b);
    char c; int got = 0;
    while (read(fd, &c, 1) == 1) {
        got = 1;
        if (c == '\n') { add_line(take_line(&b)); binit(&b); got = 0; continue; }
        bputc(&b, c);
    }
    if (got) add_line(take_line(&b));
    else free(b.s);
}

int main(int argc, char **argv) {
    int uniq = 0, i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-u")) uniq = 1;
        else if (!strcmp(argv[i], "-r")) rev = 1;
        else if (argv[i][0] == '-') { dprintf(STDERR, "usage: sort [-u] [-r] [file...]\n"); return 2; }
        else break;
    }
    if (i == argc) read_fd(STDIN);
    for (; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) { dprintf(STDERR, "sort: %s: %s\n", argv[i], strerror(errno)); return 1; }
        read_fd(fd);
        close(fd);
    }
    if (nlines) qsort(lines, (size_t)nlines, sizeof *lines, cmpstr);
    for (int k = 0; k < nlines; k++) {
        if (uniq && k && !strcmp(lines[k], lines[k - 1])) continue;
        printf("%s\n", lines[k]);
    }
    return 0;
}
