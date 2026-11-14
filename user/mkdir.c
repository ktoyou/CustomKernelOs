/* mkdir [-p] dir... — создать каталоги. -p создаёт промежуточные и не ругается, если каталог уже есть. */
#include "myos.h"
#include "sys/stat.h"

static int mkdir_p(const char *path) {
    char buf[1024];
    int n = (int)strlen(path), i;
    if (n <= 0 || n >= (int)sizeof buf) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, (size_t)n + 1);
    for (i = 1; i <= n; i++) {
        struct stat st;
        int r;
        if (buf[i] != '/' && buf[i] != 0) continue;
        buf[i] = 0;
        if (!buf[0]) { buf[i] = path[i]; continue; }
        {
            const char *base = buf, *sl;
            for (sl = buf; *sl; sl++) if (*sl == '/') base = sl + 1;
            if (!base[0] || !strcmp(base, ".") || !strcmp(base, "..")) {
                buf[i] = (i == n) ? 0 : path[i];
                continue;
            }
        }
        r = mkdir(buf);
        if (r < 0 && r != -EEXIST) return r;
        if (stat(buf, &st) < 0) return -1;
        if (!S_ISDIR(st.st_mode)) { errno = EEXIST; return -1; }
        buf[i] = (i == n) ? 0 : path[i];
    }
    return 0;
}

int main(int argc, char **argv) {
    int parents = 0, i = 1, rc = 0;
    if (argc >= 2 && !strcmp(argv[1], "-p")) { parents = 1; i = 2; }
    if (i >= argc) { dprintf(STDERR, "usage: mkdir [-p] dir...\n"); return 2; }
    for (; i < argc; i++) {
        int r = parents ? mkdir_p(argv[i]) : mkdir(argv[i]);
        if (r < 0) {
            dprintf(STDERR, "mkdir: %s: %s\n", argv[i], strerror(r < -1 ? -r : errno));
            rc = 1;
        }
    }
    return rc;
}
