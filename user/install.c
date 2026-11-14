/* install [-m MODE] [-d] path... — копирование и каталоги для make install. */
#include "myos.h"
#include "sys/stat.h"

static int opt_dir;
static unsigned opt_mode = 0755;
static int have_mode;

static int parse_mode(const char *s, unsigned *mode) {
    unsigned m = 0;
    if (!s || !s[0]) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '7' || m > 07777 / 8) return -1;
        m = m * 8 + (unsigned)(*p - '0');
    }
    if (m > 07777) return -1;
    *mode = m;
    return 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

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
        if (buf[0]) {
            r = mkdir(buf);
            if (r < 0 && r != -EEXIST) return -1;
            if (stat(buf, &st) < 0 || !S_ISDIR(st.st_mode)) { errno = EEXIST; return -1; }
        }
        buf[i] = (i == n) ? 0 : path[i];
    }
    if (have_mode && chmod(path, opt_mode) < 0) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    char buf[4096];
    int in, out, n;
    in = open(src, O_RDONLY);
    if (in < 0) return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { close(in); return -1; }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        int off = 0;
        while (off < n) {
            int w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { close(in); close(out); return -1; }
            off += w;
        }
    }
    close(in);
    close(out);
    if (n < 0) return -1;
    if (chmod(dst, opt_mode) < 0) return -1;
    return 0;
}

static const char *base_of(const char *path) {
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/') s = p + 1;
    return s[0] ? s : path;
}

int main(int argc, char **argv) {
    int i = 1, nsrc, status = 0;
    const char *dest;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "-d")) { opt_dir = 1; i++; continue; }
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            if (parse_mode(argv[i + 1], &opt_mode) < 0) {
                dprintf(STDERR, "install: invalid mode\n");
                return 1;
            }
            have_mode = 1;
            i += 2;
            continue;
        }
        dprintf(STDERR, "usage: install [-m MODE] [-d] path...\n");
        return 1;
    }
    if (i >= argc) { dprintf(STDERR, "usage: install [-m MODE] [-d] path...\n"); return 1; }
    if (opt_dir) {
        for (; i < argc; i++) if (mkdir_p(argv[i]) < 0) {
            dprintf(STDERR, "install: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        return status;
    }
    nsrc = argc - i - 1;
    if (nsrc < 1) { dprintf(STDERR, "install: missing destination\n"); return 1; }
    dest = argv[argc - 1];
    if (nsrc > 1 && !is_dir(dest)) { dprintf(STDERR, "install: %s: not a directory\n", dest); return 1; }
    for (; i < argc - 1; i++) {
        char path[1024];
        const char *to = dest;
        if (is_dir(dest)) {
            const char *base = base_of(argv[i]);
            int nd = (int)strlen(dest), nb = (int)strlen(base);
            int slash = !(nd == 1 && dest[0] == '/') && nd > 0 && dest[nd - 1] != '/';
            if (nd + slash + nb + 1 > (int)sizeof path) { errno = ENAMETOOLONG; status = 1; continue; }
            memcpy(path, dest, (size_t)nd);
            if (slash) path[nd++] = '/';
            memcpy(path + nd, base, (size_t)nb + 1);
            to = path;
        }
        if (copy_file(argv[i], to) < 0) {
            dprintf(STDERR, "install: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}
