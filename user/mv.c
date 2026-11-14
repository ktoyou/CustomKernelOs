/* mv [-f] source... dest — переименовать файл или переложить в каталог. */
#include "myos.h"
#include "stdio.h"
#include "sys/stat.h"

static int isdir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *base_of(const char *path) {
    const char *s = path;
    for (const char *p = path; *p; p++) if (*p == '/') s = p + 1;
    return *s ? s : path;
}

static int move_one(const char *src, const char *dst) {
    char path[512];
    const char *to = dst;
    if (isdir(dst)) {
        if (snprintf(path, sizeof path, "%s/%s", dst, base_of(src)) >= (int)sizeof path) {
            fprintf(stderr, "mv: path too long\n");
            return 1;
        }
        to = path;
    }
    if (rename(src, to) == 0) return 0;
    fprintf(stderr, "mv: %s: %s\n", src, strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    int i = 1, n, rc = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (char *p = argv[i] + 1; *p; p++) {
            if (*p != 'f') {
                fprintf(stderr, "mv: bad option -%c\n", *p);
                return 1;
            }
        }
        i++;
    }
    n = argc - i;
    if (n < 2) {
        fprintf(stderr, "usage: mv [-f] source dest\n");
        return 1;
    }
    if (n > 2 && !isdir(argv[argc - 1])) {
        fprintf(stderr, "mv: target is not a directory\n");
        return 1;
    }
    for (; i < argc - 1; i++) rc |= move_one(argv[i], argv[argc - 1]);
    return rc;
}
