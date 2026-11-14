/* ls [-lhaA1] [path...] — содержимое каталогов.
 * Короткие флаги склеиваются, как в GNU ls: ls -lha.
 * -l длинный список, -h размеры в К/М/Г (1024), -a включая «.» «..» и скрытые,
 * -A скрытые без «.» и «..». Без -a/-A имена на точку не показываются.
 * Файл, переданный аргументом, показывается всегда. -- кончает флаги. */
#include "myos.h"
#include "sys/stat.h"

static int opt_long, opt_human, opt_all, opt_almost, opt_one;

static int join_path(char *dst, int cap, const char *dir, const char *name) {
    int nd = (int)strlen(dir), nn = (int)strlen(name), slash;
    slash = !(nd == 1 && dir[0] == '/') && nd > 0 && dir[nd - 1] != '/';
    if (nd + slash + nn + 1 > cap) return -1;
    memcpy(dst, dir, (size_t)nd);
    if (slash) dst[nd++] = '/';
    memcpy(dst + nd, name, (size_t)nn + 1);
    return 0;
}

/* Как GNU ls -h в локали C: 1023, 1.0K, 1.5K, 10K. */
static void format_size(char *buf, int cap, unsigned size) {
    static const char unit[] = "KMGTPE";
    unsigned whole, rem, tenths;
    int scale;
    if (!opt_human || size < 1024u) {
        snprintf(buf, (size_t)cap, "%u", size);
        return;
    }
    whole = size;
    rem = 0;
    scale = -1;
    while (whole >= 1024u && scale < (int)sizeof unit - 2) {
        rem = whole % 1024u;
        whole /= 1024u;
        scale++;
    }
    tenths = (rem * 10u + 512u) / 1024u;
    if (tenths >= 10u) {
        whole++;
        tenths = 0;
    }
    if (whole >= 1024u && scale < (int)sizeof unit - 2) {
        whole = 1;
        tenths = 0;
        scale++;
    }
    if (scale < 0) {
        snprintf(buf, (size_t)cap, "%u", size);
        return;
    }
    if (whole < 10u)
        snprintf(buf, (size_t)cap, "%u.%u%c", whole, tenths, unit[scale]);
    else
        snprintf(buf, (size_t)cap, "%u%c", whole, unit[scale]);
}

static void entry(const struct dirent_info *e, const char *shown, const char *path) {
    /* В конвейере и с -1 — одно имя на строку, как ждёт рецепт шаблонов Git. */
    if (opt_one && !opt_long) {
        printf("%s\n", shown);
        return;
    }
    if (opt_long) {
        struct stat st;
        char bits[11], sz[16];
        int i;
        if (lstat(path, &st) < 0) {
            dprintf(STDERR, "ls: %s: %s\n", path, strerror(errno));
            return;
        }
        bits[0] = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' :
                  S_ISCHR(st.st_mode) ? 'c' : S_ISBLK(st.st_mode) ? 'b' : '-';
        for (i = 0; i < 9; i++)
            bits[i + 1] = (st.st_mode & (1u << (8 - i))) ? "rwx"[i % 3] : '-';
        if (st.st_mode & S_ISVTX) bits[9] = bits[9] == 'x' ? 't' : 'T';
        bits[10] = 0;
        format_size(sz, (int)sizeof sz, st.st_size);
        printf("%s %u %u %8s %s\n", bits, st.st_uid, st.st_gid, sz, shown);
        return;
    }
    if (e->type == 2) printf("   <DIR>  " ANSI_BLUE ANSI_BOLD "%s/" ANSI_RESET "\n", shown);
    else if (e->type == 3) printf(" <BLOCK>  %s\n", shown);
    else if (e->type == 4) printf("  <CHAR>  %s\n", shown);
    else if (e->type == 5) printf("   <LNK>  %s\n", shown);
    else {
        char sz[16];
        format_size(sz, (int)sizeof sz, e->size);
        printf("%8s  %s\n", sz, shown);
    }
}

static int emit_named(const char *dir, const char *name) {
    char full[1024];
    struct dirent_info e;
    int r;
    if (join_path(full, (int)sizeof full, dir, name) < 0) {
        dprintf(STDERR, "ls: %s: file name too long\n", dir);
        return 1;
    }
    r = dirstat(full, &e);
    if (r < 0) {
        dprintf(STDERR, "ls: %s: %s\n", full, strerror(-r));
        return 1;
    }
    entry(&e, name, full);
    return 0;
}

static int dot_entry(const char *name) {
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

static int list_dir(const char *path, bool title) {
    struct dirent_info e;
    int status, i, rc = 0;
    if (title) printf("%s:\n", path);
    status = dirread(path, 0, &e);
    if (status < 0) {
        dprintf(STDERR, "ls: %s: %s\n", path, strerror(-status));
        return 1;
    }
    if (opt_all && (emit_named(path, ".") || emit_named(path, ".."))) rc = 1;
    for (i = 0; (status = dirread(path, i, &e)) == 1; i++) {
        char full[1024];
        if (dot_entry(e.name)) continue;
        if (e.name[0] == '.' && !opt_all && !opt_almost) continue;
        if (join_path(full, (int)sizeof full, path, e.name) < 0) {
            dprintf(STDERR, "ls: %s: file name too long\n", path);
            rc = 1;
            continue;
        }
        entry(&e, e.name, full);
    }
    if (status < 0) {
        dprintf(STDERR, "ls: %s: %s\n", path, strerror(-status));
        return 1;
    }
    return rc;
}

static void usage(void) {
    dprintf(STDERR, "usage: ls [-lhaA1] [path...]\n");
}

/* 0 — это уже операнд, 1 — флаг съеден, 2 — «--», -1 — ошибка. */
static int take_opt(const char *a) {
    const char *p;
    if (a[0] != '-' || a[1] == 0) return 0;
    if (a[1] == '-' && a[2] == 0) return 2;
    if (a[1] == '-') {
        if (!strcmp(a, "--all")) opt_all = 1;
        else if (!strcmp(a, "--almost-all")) opt_almost = 1;
        else if (!strcmp(a, "--human-readable")) opt_human = 1;
        else {
            dprintf(STDERR, "ls: unknown option '%s'\n", a);
            usage();
            return -1;
        }
        return 1;
    }
    for (p = a + 1; *p; p++) {
        if (*p == 'l') opt_long = 1;
        else if (*p == 'h') opt_human = 1;
        else if (*p == 'a') opt_all = 1;
        else if (*p == 'A') opt_almost = 1;
        else if (*p == '1') opt_one = 1;
        else {
            dprintf(STDERR, "ls: unknown option '%c'\n", *p);
            usage();
            return -1;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    int i, rc = 0, optend = 1;
    struct dirent_info e;
    for (i = 1; i < argc; i++) {
        int k = take_opt(argv[i]);
        if (k == 0) break;
        if (k < 0) return 1;
        optend = i + 1;
        if (k == 2) break;
    }
    if (!opt_long && !opt_one) {
        struct stat out;
        /* Каталог на экране остаётся цветным. Труба и файл — голые имена. */
        if (fstat(STDOUT, &out) != 0 || !S_ISCHR(out.st_mode)) opt_one = 1;
    }
    if (argc <= optend) return list_dir(".", false);
    /* сначала файлы, потом каталоги — как в GNU ls */
    for (i = optend; i < argc; i++) {
        int r = dirstat(argv[i], &e);
        if (r < 0) {
            dprintf(STDERR, "ls: %s: %s\n", argv[i], strerror(-r));
            rc = 1;
            continue;
        }
        if (e.type != 2) entry(&e, argv[i], argv[i]);
    }
    for (i = optend; i < argc; i++)
        if (dirstat(argv[i], &e) == 0 && e.type == 2)
            rc |= list_dir(argv[i], argc > optend + 1);
    return rc;
}
