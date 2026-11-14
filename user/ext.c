/* ext — минимальные порты.
 *
 *   ext list
 *   ext install <имя>
 *   ext uninstall <имя>
 *
 * Пакет — каталог /ext/instructions/<имя>/. Имя каталога и есть имя пакета.
 *   setup.inst              установка
 *   uninstall.inst          снятие; ext uninstall не трогает зависимости
 *   остальные файлы         патчи и заготовки вроде config.h
 *
 * Команды, одна на строку:
 *   name <id>
 *   descr <текст>
 *   workdir <путь>          создать каталог и перейти в него
 *   mkdir <путь>            создать каталог, в том числе вложенный
 *   depend <имя>            сначала поставить этот пакет, если его ещё нет
 *   fetch [-r N] <url> [куда]
 *   git <аргументы git>     /ext/bin/git или /bin/git. Если файла нет,
 *                           сначала выполняется ext install git, как depend git
 *   tar <аргументы tar>     /bin/tar, в том числе xzf для tar.gz
 *   make [аргументы]        /bin/make
 *   cc <аргументы cc>
 *   copy <файл> <куда>      файл из каталога пакета в дерево сборки
 *   patch [-p N] <файл>     unified diff из каталога пакета, по умолчанию -p1
 *   rm <путь>               удалить файл
 * # до конца строки — комментарий.
 * fetch вызывает /bin/download. Без -r повторов два.
 * Поставленный пакет отмечается в /ext/installed/<имя>. Пакет с отметкой
 * пропускается и при прямом запросе, и как зависимость. Цикл — ошибка.
 * Отметка снимается только после успешного uninstall.inst.
 */
#include "myos.h"
#include "sys/wait.h"
#include "stdio.h"
#include "dirent.h"
#include "build_jobs.h"

#define INST_DIR "/ext/instructions"
#define MAX_ARG  64
#define MAX_PKG  64

static int build_jobs_override=-1;
static int selected_jobs(void) {
    int jobs=build_jobs_override;
    if(jobs<0) {
        FILE *f=fopen("/etc/ext/build-jobs","r");
        if(f) {
            char value[32]; int n=0,c;
            while((c=fgetc(f))!=EOF && n<(int)sizeof(value)-1) value[n++]=c;
            fclose(f); value[n]=0;
            while(n && (value[n-1]=='\n' || value[n-1]=='\r' || value[n-1]==' ' || value[n-1]=='\t')) value[--n]=0;
            jobs=c==EOF?build_jobs_parse(value):-1;
            if(jobs<0) { dprintf(STDERR,"ext: /etc/ext/build-jobs must contain auto or 1..%d\n",BUILD_JOBS_MAX); return -1; }
        } else if(errno!=ENOENT) {
            dprintf(STDERR,"ext: cannot read /etc/ext/build-jobs\n"); return -1;
        } else jobs=0;
    }
    return jobs?jobs:build_jobs_auto();
}

static int split_line(char *s, char **av) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (n == MAX_ARG) return -1;
        if (*s == '"') {
            av[n++] = ++s;
            while (*s && *s != '"') s++;
            if (*s == '"') *s++ = 0;
        } else {
            av[n++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '#') s++;
            if (*s == '#') { *s = 0; break; }
            if (*s) *s++ = 0;
        }
    }
    av[n] = NULL;
    return n;
}

static int mkdir_p(const char *path) {
    char buf[128];
    int n, i;
    if (!path || !path[0]) return -EINVAL;
    n = (int)strlen(path);
    if (n >= (int)sizeof buf) return -ENAMETOOLONG;
    memcpy(buf, path, (size_t)n + 1);
    for (i = 1; i <= n; i++) {
        int r;
        char save;
        if (buf[i] != '/' && buf[i] != 0) continue;
        save = buf[i];
        buf[i] = 0;
        if (!(buf[0] == '/' && buf[1] == 0)) {
            r = mkdir(buf);
            if (r < 0 && r != -EEXIST) return r;
        }
        buf[i] = save;
    }
    return 0;
}

static int run_prog(const char *path, char **argv) {
    int pid, st = 0;
    pid = fork();
    if (pid < 0) { dprintf(STDERR, "ext: fork: %s\n", strerror(-pid)); return 1; }
    if (pid == 0) {
        int r = exec(path, argv);
        dprintf(STDERR, "ext: %s: %s\n", path, strerror(-r));
        exit(127);
    }
    if (waitpid(pid, &st, 0) < 0) return 1;
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}

static int cmd_fetch(int argc, char **argv) {
    char *av[8];
    const char *retries = "2";
    const char *url, *dest = NULL;
    int i = 1, n = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) { retries = argv[++i]; i++; }
        else { dprintf(STDERR, "ext: fetch: bad option %s\n", argv[i]); return 1; }
    }
    if (i >= argc) { dprintf(STDERR, "ext: fetch: url required\n"); return 1; }
    url = argv[i++];
    if (i < argc) dest = argv[i++];
    if (i != argc) { dprintf(STDERR, "ext: fetch: extra arguments\n"); return 1; }
    av[n++] = "download";
    av[n++] = "-r";
    av[n++] = (char *)retries;
    if (dest) { av[n++] = "-o"; av[n++] = (char *)dest; }
    av[n++] = (char *)url;
    av[n] = NULL;
    printf("fetch %s\n", url);
    return run_prog("/bin/download", av);
}

static int cmd_cc(int argc, char **argv) {
    int i;
    if (argc < 2) { dprintf(STDERR, "ext: cc: arguments required\n"); return 1; }
    argv[0] = "cc";
    printf("cc");
    for (i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");
    return run_prog("/bin/cc", argv);
}

static int cmd_tool(const char *path, int argc, char **argv) {
    int i;
    printf("%s", argv[0]);
    for (i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");
    return run_prog(path, argv);
}

static int cmd_make(int argc,char **argv) {
    int explicit_jobs=0;
    for(int i=1;i<argc;i++) if(!strncmp(argv[i],"-j",2) || !strncmp(argv[i],"--jobs",6)) explicit_jobs=1;
    if(explicit_jobs && build_jobs_override<0) return cmd_tool("/bin/make",argc,argv);
    int jobs=selected_jobs();
    if(jobs<1) return 1;
    char flag[16],*av[MAX_ARG+2];
    snprintf(flag,sizeof(flag),"-j%d",jobs);
    for(int i=0;i<argc;i++) av[i]=argv[i];
    av[argc]=flag; av[argc+1]=NULL;
    return cmd_tool("/bin/make",argc+1,av);
}

static int install_name(const char *dir, const char *name);
static int cmd_copy(const char *pkg, int argc, char **argv);
static int cmd_patch(const char *pkg, int argc, char **argv);
static int cmd_rm(int argc, char **argv);
static int cmd_git(const char *root, int argc, char **argv);

static int exec_args(const char *root, const char *pkg, int argc, char **argv) {
    int r;
    if (argc < 1) return 0;
    if (!strcmp(argv[0], "name") || !strcmp(argv[0], "descr")) return 0;
    if (!strcmp(argv[0], "depend")) {
        if (argc != 2) { dprintf(STDERR, "ext: depend needs one package\n"); return 1; }
        return install_name(root, argv[1]);
    }
    if (!strcmp(argv[0], "workdir")) {
        if (argc != 2) { dprintf(STDERR, "ext: workdir needs one path\n"); return 1; }
        r = mkdir_p(argv[1]);
        if (r < 0) { dprintf(STDERR, "ext: %s: %s\n", argv[1], strerror(-r)); return 1; }
        r = chdir(argv[1]);
        if (r < 0) { dprintf(STDERR, "ext: %s: %s\n", argv[1], strerror(-r)); return 1; }
        return 0;
    }
    if (!strcmp(argv[0], "mkdir")) {
        if (argc != 2) { dprintf(STDERR, "ext: mkdir needs one path\n"); return 1; }
        r = mkdir_p(argv[1]);
        if (r < 0) { dprintf(STDERR, "ext: %s: %s\n", argv[1], strerror(-r)); return 1; }
        return 0;
    }
    if (!strcmp(argv[0], "fetch")) return cmd_fetch(argc, argv);
    if (!strcmp(argv[0], "git")) return cmd_git(root, argc, argv);
    if (!strcmp(argv[0], "tar")) {
        if (argc < 2) { dprintf(STDERR, "ext: tar: arguments required\n"); return 1; }
        return cmd_tool("/bin/tar", argc, argv);
    }
    if (!strcmp(argv[0], "make")) return cmd_make(argc, argv);
    if (!strcmp(argv[0], "cc")) return cmd_cc(argc, argv);
    if (!strcmp(argv[0], "copy")) return cmd_copy(pkg, argc, argv);
    if (!strcmp(argv[0], "patch")) return cmd_patch(pkg, argc, argv);
    if (!strcmp(argv[0], "rm")) return cmd_rm(argc, argv);
    dprintf(STDERR, "ext: unknown command '%s'\n", argv[0]);
    return 1;
}

static int read_line(FILE *f, char *buf, int max) {
    int i = 0, c, overflow = 0;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (c == '\r') continue;
        if (i < max - 1) buf[i++] = (char)c;
        else overflow = 1;
    }
    buf[i] = 0;
    if (overflow) return -1;
    if (c == EOF && i == 0) return 0;
    return 1;
}

static int run_file(const char *root, const char *pkg, const char *path) {
    FILE *f = fopen(path, "r");
    char buf[1024];
    int line = 0, rc;
    if (!f) { dprintf(STDERR, "ext: %s: %s\n", path, strerror(errno)); return 1; }
    while ((rc = read_line(f, buf, (int)sizeof buf)) != 0) {
        char *av[MAX_ARG + 1];
        int n;
        line++;
        if (rc < 0) { dprintf(STDERR, "ext: %s:%d: line too long\n", path, line); fclose(f); return 1; }
        n = split_line(buf, av);
        if (n < 0) { dprintf(STDERR, "ext: %s:%d: too many words\n", path, line); fclose(f); return 1; }
        if (exec_args(root, pkg, n, av) != 0) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

static void copy_field(char *dst, int max, int argc, char **argv) {
    int i, pos = 0;
    dst[0] = 0;
    for (i = 1; i < argc && pos < max - 1; i++) {
        int k = 0;
        if (pos) dst[pos++] = ' ';
        while (argv[i][k] && pos < max - 1) dst[pos++] = argv[i][k++];
    }
    dst[pos] = 0;
}

static int read_meta(const char *path, char *name, int nmax, char *descr, int dmax) {
    FILE *f = fopen(path, "r");
    char buf[1024];
    int rc;
    name[0] = descr[0] = 0;
    if (!f) return -1;
    while ((rc = read_line(f, buf, (int)sizeof buf)) != 0) {
        char *av[MAX_ARG + 1];
        int n;
        if (rc < 0) break;
        n = split_line(buf, av);
        if (n < 1) continue;
        if (!strcmp(av[0], "name") && n >= 2 && !name[0]) copy_field(name, nmax, 2, av);
        if (!strcmp(av[0], "descr") && n >= 2 && !descr[0]) copy_field(descr, dmax, n, av);
    }
    fclose(f);
    return name[0] ? 0 : -1;
}

struct pkg {
    char name[32];
    char descr[80];
};

static int valid_pkg(const char *name);

static int list_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct pkg pk[MAX_PKG];
    int n = 0, i, j;
    if (!d) { dprintf(STDERR, "ext: %s: %s\n", dir, strerror(errno)); return 1; }
    for (;;) {
        struct dirent *e = readdir(d);
        char path[256], ignore[32];
        FILE *f;
        if (!e) break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!valid_pkg(e->d_name) || n == MAX_PKG) continue;
        snprintf(path, sizeof path, "%s/%s/setup.inst", dir, e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        fclose(f);
        memcpy(pk[n].name, e->d_name, strlen(e->d_name) + 1);
        pk[n].descr[0] = 0;
        read_meta(path, ignore, (int)sizeof ignore, pk[n].descr, (int)sizeof pk[n].descr);
        n++;
    }
    closedir(d);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (strcmp(pk[j].name, pk[i].name) < 0) {
                struct pkg t = pk[i];
                pk[i] = pk[j];
                pk[j] = t;
            }
    for (i = 0; i < n; i++) {
        if (pk[i].descr[0]) printf("%s\t%s\n", pk[i].name, pk[i].descr);
        else printf("%s\n", pk[i].name);
    }
    return 0;
}

static int script_path(const char *dir, const char *name, const char *file, char *out, int max) {
    int n = snprintf(out, (size_t)max, "%s/%s/%s", dir, name, file);
    if (n < 0 || n >= max) return -1;
    return 0;
}

#define MAX_DEPTH 32
static char dep_stack[MAX_DEPTH][32];
static int dep_depth;

static int valid_pkg(const char *name) {
    int i;
    if (!name || !name[0] || strlen(name) >= 32) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (i = 0; name[i]; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '+' || c == '.')
            continue;
        return 0;
    }
    return 1;
}

static int parent_dir(const char *path, char *out, int max) {
    int n = (int)strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    while (n > 0 && path[n - 1] != '/') n--;
    if (n <= 0) return -1;
    if (n == 1) {
        if (max < 2) return -1;
        out[0] = '/';
        out[1] = 0;
        return 0;
    }
    if (n >= max) return -1;
    memcpy(out, path, (size_t)(n - 1));
    out[n - 1] = 0;
    return 0;
}

#define PATCH_MAX (256 * 1024)
#define FILE_MAX  (1024 * 1024)

/* Относительный путь без абсолютного корня и без компонентов . и .. */
static int safe_rel(const char *rel) {
    const char *p;
    if (!rel || !rel[0] || rel[0] == '/') return 0;
    p = rel;
    while (*p) {
        if (p[0] == '.' && (p[1] == '/' || p[1] == 0)) return 0;
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return 0;
        while (*p && *p != '/') p++;
        if (*p == '/') p++;
    }
    return 1;
}

static int copy_bytes(const char *src, const char *dst) {
    FILE *in = fopen(src, "r");
    FILE *out;
    char buf[512];
    int n;
    if (!in) { dprintf(STDERR, "ext: copy: %s: %s\n", src, strerror(errno)); return 1; }
    out = fopen(dst, "w");
    if (!out) { dprintf(STDERR, "ext: copy: %s: %s\n", dst, strerror(errno)); fclose(in); return 1; }
    while ((n = (int)fread(buf, 1, sizeof buf, in)) > 0) {
        if ((int)fwrite(buf, 1, (size_t)n, out) != n) {
            dprintf(STDERR, "ext: copy: %s: %s\n", dst, strerror(errno));
            fclose(in);
            fclose(out);
            return 1;
        }
    }
    if (ferror(in)) {
        dprintf(STDERR, "ext: copy: %s: %s\n", src, strerror(errno));
        fclose(in);
        fclose(out);
        return 1;
    }
    fclose(in);
    fclose(out);
    return 0;
}

static char *read_all(const char *path, int *len, int max) {
    FILE *f = fopen(path, "r");
    char *buf, *nb;
    int n = 0, cap = 256, c;
    if (!f) return NULL;
    if (max < 2) { fclose(f); return NULL; }
    buf = malloc((size_t)cap);
    if (!buf) { fclose(f); return NULL; }
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= max) { free(buf); fclose(f); return NULL; }
        if (n + 1 >= cap) {
            cap *= 2;
            if (cap > max) cap = max;
            nb = realloc(buf, (size_t)cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        buf[n++] = (char)c;
    }
    buf[n] = 0;
    *len = n;
    fclose(f);
    return buf;
}

static int split_lines(char *buf, int n, char ***out, int *nout) {
    int cap = 32, count = 0, i = 0;
    char **lines = malloc((size_t)cap * sizeof(char *));
    if (!lines) return -1;
    while (i < n) {
        int start = i, end;
        char **nl;
        while (i < n && buf[i] != '\n') i++;
        end = i;
        if (i < n) buf[i++] = 0;
        if (end > start && buf[end - 1] == '\r') buf[end - 1] = 0;
        if (count >= cap) {
            cap *= 2;
            nl = realloc(lines, (size_t)cap * sizeof(char *));
            if (!nl) { free(lines); return -1; }
            lines = nl;
        }
        lines[count++] = buf + start;
    }
    *out = lines;
    *nout = count;
    return 0;
}

static int header_path(char *line, char *out, int max) {
    char *p = line;
    int n = 0;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return -1;
    while (p[n] && p[n] != '\t' && n < max - 1) {
        out[n] = p[n];
        n++;
    }
    out[n] = 0;
    return n ? 0 : -1;
}

static const char *strip_components(const char *path, int n) {
    while (n > 0 && *path) {
        const char *sl = strchr(path, '/');
        if (!sl) return path + strlen(path);
        path = sl + 1;
        n--;
    }
    return path;
}

static int parse_num(const char **ps, int *v) {
    const char *s = *ps;
    int x = 0;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        x = x * 10 + (*s - '0');
        s++;
    }
    *v = x;
    *ps = s;
    return 0;
}

static int parse_hunk(const char *s, int *os, int *oc, int *ns, int *nc) {
    if (strncmp(s, "@@ -", 4)) return -1;
    s += 4;
    if (parse_num(&s, os) < 0) return -1;
    if (*s == ',') {
        s++;
        if (parse_num(&s, oc) < 0) return -1;
    } else *oc = 1;
    if (*s != ' ' || s[1] != '+') return -1;
    s += 2;
    if (parse_num(&s, ns) < 0) return -1;
    if (*s == ',') {
        s++;
        if (parse_num(&s, nc) < 0) return -1;
    } else *nc = 1;
    return 0;
}

static int push_line(char ***v, int *n, int *cap, char *s) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        char **nv = realloc(*v, (size_t)nc * sizeof(char *));
        if (!nv) { dprintf(STDERR, "ext: out of memory\n"); return -1; }
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = s;
    return 0;
}

static int write_lines(const char *path, char **lines, int n) {
    FILE *f;
    char parent[128];
    int i, r;
    if (strchr(path, '/')) {
        if (parent_dir(path, parent, (int)sizeof parent) == 0) {
            r = mkdir_p(parent);
            if (r < 0) return -1;
        }
    }
    f = fopen(path, "w");
    if (!f) return -1;
    for (i = 0; i < n; i++) {
        if (fputs(lines[i], f) < 0 || fputc('\n', f) < 0) { fclose(f); return -1; }
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

static int apply_section(char **plines, int nlines, int *index, int strip) {
    char oldp[160], newp[160];
    char *filebuf = NULL, **oldlines = NULL, **out = NULL, *s;
    const char *target, *raw;
    int oldn = 0, filelen = 0, outn = 0, outcap = 0, in_i, i, rc;
    int creating, deleting, os, oc, ns, nc, got_old, got_new, u;
    char kind;

    i = *index;
    if (header_path(plines[i], oldp, (int)sizeof oldp) < 0) return -1;
    i++;
    if (i >= nlines || strncmp(plines[i], "+++ ", 4) != 0) {
        dprintf(STDERR, "ext: patch: missing +++\n");
        return -1;
    }
    if (header_path(plines[i], newp, (int)sizeof newp) < 0) return -1;
    i++;
    creating = !strcmp(oldp, "/dev/null");
    deleting = !strcmp(newp, "/dev/null");
    if (creating && deleting) {
        dprintf(STDERR, "ext: patch: empty file header\n");
        return -1;
    }
    raw = deleting ? oldp : newp;
    target = strip_components(raw, strip);
    if (!safe_rel(target)) {
        dprintf(STDERR, "ext: patch: bad path %s\n", target[0] ? target : raw);
        return -1;
    }
    if (creating) {
        filebuf = malloc(1);
        if (!filebuf) { dprintf(STDERR, "ext: out of memory\n"); return -1; }
        filebuf[0] = 0;
        filelen = 0;
    } else {
        filebuf = read_all(target, &filelen, FILE_MAX);
        if (!filebuf) {
            dprintf(STDERR, "ext: patch: %s: cannot read\n", target);
            return -1;
        }
    }
    if (split_lines(filebuf, filelen, &oldlines, &oldn) < 0) {
        free(filebuf);
        dprintf(STDERR, "ext: out of memory\n");
        return -1;
    }
    in_i = 0;
    rc = 0;
    while (i < nlines && strncmp(plines[i], "@@ ", 3) == 0) {
        if (parse_hunk(plines[i], &os, &oc, &ns, &nc) < 0) {
            dprintf(STDERR, "ext: patch: bad hunk\n");
            rc = -1;
            goto done;
        }
        (void)ns;
        i++;
        while (in_i < os - 1) {
            if (in_i >= oldn) {
                dprintf(STDERR, "ext: patch: %s: hunk does not match\n", target);
                rc = -1;
                goto done;
            }
            if (push_line(&out, &outn, &outcap, oldlines[in_i]) < 0) { rc = -1; goto done; }
            in_i++;
        }
        got_old = 0;
        got_new = 0;
        while (got_old < oc || got_new < nc) {
            if (i >= nlines) {
                dprintf(STDERR, "ext: patch: %s: short hunk\n", target);
                rc = -1;
                goto done;
            }
            s = plines[i];
            if (s[0] == '\\') { i++; continue; }
            kind = s[0];
            if (kind != ' ' && kind != '+' && kind != '-') {
                dprintf(STDERR, "ext: patch: %s: hunk does not match\n", target);
                rc = -1;
                goto done;
            }
            i++;
            if (kind == ' ' || kind == '-') {
                if (in_i >= oldn || strcmp(oldlines[in_i], s + 1) != 0) {
                    dprintf(STDERR, "ext: patch: %s: hunk does not match\n", target);
                    rc = -1;
                    goto done;
                }
                if (kind == ' ') {
                    if (push_line(&out, &outn, &outcap, oldlines[in_i]) < 0) { rc = -1; goto done; }
                    got_new++;
                }
                in_i++;
                got_old++;
            } else {
                if (push_line(&out, &outn, &outcap, s + 1) < 0) { rc = -1; goto done; }
                got_new++;
            }
        }
    }
    while (in_i < oldn) {
        if (push_line(&out, &outn, &outcap, oldlines[in_i]) < 0) { rc = -1; goto done; }
        in_i++;
    }
    if (deleting) {
        u = unlink(target);
        if (u < 0) {
            dprintf(STDERR, "ext: patch: %s: %s\n", target, strerror(-u));
            rc = -1;
            goto done;
        }
    } else if (write_lines(target, out, outn) < 0) {
        dprintf(STDERR, "ext: patch: %s: %s\n", target, strerror(errno));
        rc = -1;
        goto done;
    }
    *index = i;
done:
    free(out);
    free(oldlines);
    free(filebuf);
    return rc;
}

static int apply_patch(const char *path, int strip) {
    char *buf, **lines = NULL;
    int len = 0, nlines = 0, i = 0, files = 0, rc = 0;
    buf = read_all(path, &len, PATCH_MAX);
    if (!buf) { dprintf(STDERR, "ext: patch: %s: cannot read\n", path); return 1; }
    if (split_lines(buf, len, &lines, &nlines) < 0) {
        free(buf);
        dprintf(STDERR, "ext: out of memory\n");
        return 1;
    }
    while (i < nlines) {
        if (strncmp(lines[i], "--- ", 4) != 0) { i++; continue; }
        files++;
        if (apply_section(lines, nlines, &i, strip) < 0) { rc = 1; break; }
    }
    if (!rc && files == 0) { dprintf(STDERR, "ext: patch: no files in %s\n", path); rc = 1; }
    free(lines);
    free(buf);
    return rc;
}

static int cmd_copy(const char *pkg, int argc, char **argv) {
    char src[256], parent[128];
    int n, r;
    if (argc != 3) { dprintf(STDERR, "ext: copy needs a source and a destination\n"); return 1; }
    if (!safe_rel(argv[1])) { dprintf(STDERR, "ext: copy: bad source %s\n", argv[1]); return 1; }
    n = snprintf(src, sizeof src, "%s/%s", pkg, argv[1]);
    if (n < 0 || n >= (int)sizeof src) { dprintf(STDERR, "ext: copy: path too long\n"); return 1; }
    if (strchr(argv[2], '/')) {
        if (parent_dir(argv[2], parent, (int)sizeof parent) == 0) {
            r = mkdir_p(parent);
            if (r < 0) { dprintf(STDERR, "ext: %s: %s\n", parent, strerror(-r)); return 1; }
        }
    }
    return copy_bytes(src, argv[2]);
}

static int cmd_patch(const char *pkg, int argc, char **argv) {
    char src[256];
    const char *num;
    int argi = 1, strip = 1, n;
    if (argi < argc && strncmp(argv[argi], "-p", 2) == 0) {
        if (argv[argi][2]) num = argv[argi] + 2;
        else {
            if (argi + 1 >= argc) { dprintf(STDERR, "ext: patch: -p needs a number\n"); return 1; }
            num = argv[++argi];
        }
        argi++;
        strip = 0;
        if (!num[0]) { dprintf(STDERR, "ext: patch: -p needs a number\n"); return 1; }
        while (*num) {
            if (*num < '0' || *num > '9') { dprintf(STDERR, "ext: patch: -p needs a number\n"); return 1; }
            strip = strip * 10 + (*num - '0');
            num++;
        }
    }
    if (argi + 1 != argc) { dprintf(STDERR, "ext: patch needs one file\n"); return 1; }
    if (!safe_rel(argv[argi])) { dprintf(STDERR, "ext: patch: bad source %s\n", argv[argi]); return 1; }
    n = snprintf(src, sizeof src, "%s/%s", pkg, argv[argi]);
    if (n < 0 || n >= (int)sizeof src) { dprintf(STDERR, "ext: patch: path too long\n"); return 1; }
    return apply_patch(src, strip);
}

#if defined(__LINUX_TEST__)
static int git_hide_system;
static const char *git_test_bin;
#endif

static int regular_file(const char *path) {
    struct dirent_info st;
    return path && dirstat(path, &st) == 0 && st.type == 1;
}

static const char *git_program(void) {
#if defined(__LINUX_TEST__)
    if (git_test_bin) {
        if (regular_file(git_test_bin)) return git_test_bin;
        if (git_hide_system) return NULL;
    } else if (git_hide_system) return NULL;
#endif
    if (regular_file("/ext/bin/git")) return "/ext/bin/git";
    if (regular_file("/bin/git")) return "/bin/git";
    return NULL;
}

static int cmd_git(const char *root, int argc, char **argv) {
    char cwd[512];
    const char *bin;
    int r;
    if (argc < 2) { dprintf(STDERR, "ext: git: arguments required\n"); return 1; }
    bin = git_program();
    if (!bin) {
        if (!getcwd(cwd, sizeof cwd)) {
            dprintf(STDERR, "ext: git: cannot read the working directory\n");
            return 1;
        }
        r = install_name(root, "git");
        if (chdir(cwd) < 0) {
            dprintf(STDERR, "ext: git: cannot restore the working directory\n");
            return 1;
        }
        if (r != 0) return 1;
        bin = git_program();
        if (!bin) {
            dprintf(STDERR, "ext: git: program is not installed\n");
            return 1;
        }
    }
    return cmd_tool(bin, argc, argv);
}

static int cmd_rm(int argc, char **argv) {
    int r;
    if (argc != 2) { dprintf(STDERR, "ext: rm needs one path\n"); return 1; }
    r = unlink(argv[1]);
    if (r < 0) { dprintf(STDERR, "ext: rm: %s: %s\n", argv[1], strerror(-r)); return 1; }
    return 0;
}

static int stamp_path(const char *dir, const char *name, char *out, int max) {
    char parent[128];
    if (parent_dir(dir, parent, (int)sizeof parent) < 0) return -1;
    if (snprintf(out, (size_t)max, "%s/installed/%s", parent, name) >= max) return -1;
    return 0;
}

static int stamp_exists(const char *dir, const char *name) {
    char path[160];
    FILE *f;
    if (stamp_path(dir, name, path, (int)sizeof path) < 0) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int write_stamp(const char *dir, const char *name) {
    char parent[128], instd[160], path[160];
    FILE *f;
    if (parent_dir(dir, parent, (int)sizeof parent) < 0) return -1;
    if (snprintf(instd, sizeof instd, "%s/installed", parent) >= (int)sizeof instd) return -1;
    if (mkdir_p(instd) < 0) return -1;
    if (stamp_path(dir, name, path, (int)sizeof path) < 0) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fputs("ok\n", f);
    fclose(f);
    return 0;
}

static int install_name(const char *dir, const char *name) {
    char path[160], pkg[160];
    FILE *sf;
    int i, rc, n;
    if (!valid_pkg(name)) {
        dprintf(STDERR, "ext: %s: bad package name\n", name ? name : "");
        return 1;
    }
    for (i = 0; i < dep_depth; i++)
        if (!strcmp(dep_stack[i], name)) {
            dprintf(STDERR, "ext: dependency cycle:");
            for (i = 0; i < dep_depth; i++) dprintf(STDERR, " %s", dep_stack[i]);
            dprintf(STDERR, " %s\n", name);
            return 1;
        }
    if (stamp_exists(dir, name)) {
        printf("already installed %s\n", name);
        return 0;
    }
    if (dep_depth >= MAX_DEPTH) {
        dprintf(STDERR, "ext: dependency chain is too long\n");
        return 1;
    }
    n = snprintf(pkg, sizeof pkg, "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof pkg || script_path(dir, name, "setup.inst", path, (int)sizeof path) < 0) {
        dprintf(STDERR, "ext: %s: path too long\n", name);
        return 1;
    }
    sf = fopen(path, "r");
    if (!sf) {
        dprintf(STDERR, "ext: %s: no instruction in %s\n", name, dir);
        return 1;
    }
    fclose(sf);
    memcpy(dep_stack[dep_depth], name, strlen(name) + 1);
    dep_depth++;
    rc = run_file(dir, pkg, path);
    dep_depth--;
    if (rc != 0) return 1;
    if (write_stamp(dir, name) != 0) {
        dprintf(STDERR, "ext: %s: cannot record install\n", name);
        return 1;
    }
    printf("installed %s\n", name);
    return 0;
}

static int uninstall_name(const char *dir, const char *name) {
    char path[160], pkg[160], stamp[160];
    FILE *sf;
    int n, r;
    if (!valid_pkg(name)) {
        dprintf(STDERR, "ext: %s: bad package name\n", name ? name : "");
        return 1;
    }
    if (!stamp_exists(dir, name)) {
        dprintf(STDERR, "ext: %s: not installed\n", name);
        return 1;
    }
    n = snprintf(pkg, sizeof pkg, "%s/%s", dir, name);
    if (n < 0 || n >= (int)sizeof pkg || script_path(dir, name, "uninstall.inst", path, (int)sizeof path) < 0) {
        dprintf(STDERR, "ext: %s: path too long\n", name);
        return 1;
    }
    sf = fopen(path, "r");
    if (!sf) {
        dprintf(STDERR, "ext: %s: no uninstall script\n", name);
        return 1;
    }
    fclose(sf);
    if (run_file(dir, pkg, path) != 0) return 1;
    if (stamp_path(dir, name, stamp, (int)sizeof stamp) < 0 || (r = unlink(stamp)) < 0) {
        dprintf(STDERR, "ext: %s: cannot remove install record\n", name);
        return 1;
    }
    printf("removed %s\n", name);
    return 0;
}

#include "ext-auto.inc"

static void usage(void) {
    dprintf(STDERR, "usage: ext list | ext install [-j N|-jN] <name> | ext uninstall <name>\n       ext auto add|remove NAME... | ext auto list|status|run\n");
}

#if defined(__LINUX_TEST__)
static int file_is(const char *path, const char *expect) {
    char got[64];
    FILE *f = fopen(path, "r");
    int n = 0, c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF && n < (int)sizeof got - 1) got[n++] = (char)c;
    got[n] = 0;
    fclose(f);
    return strcmp(got, expect) == 0;
}

static int selftest(void) {
    int fails = 0;
    char *av[8];
    char line[128];
    char name[32], descr[80];
    struct dirent_info st;
    FILE *f;
    snprintf(line, sizeof line, "fetch -r 1 https://example.com/a.c a.c");
    if (split_line(line, av) != 5) { printf("FAIL split fetch\n"); fails++; }
    snprintf(line, sizeof line, "descr OpenBSD wc");
    if (split_line(line, av) != 3) { printf("FAIL split descr\n"); fails++; }
    snprintf(line, sizeof line, "  # only a comment");
    if (split_line(line, av) != 0) { printf("FAIL comment\n"); fails++; }
    if (mkdir_p("/tmp/myos-ext-test/work/sub") < 0) { printf("FAIL mkdir_p\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/work/sub", &st) != 0 || st.type != 2) { printf("FAIL dir type\n"); fails++; }
    mkdir_p("/tmp/myos-ext-test/instructions/demo");
    f = fopen("/tmp/myos-ext-test/instructions/demo/setup.inst", "w");
    if (!f) { printf("FAIL write inst\n"); return 1; }
    fputs("name demo\ndescr hello there\nworkdir /tmp/myos-ext-test/demo\nmkdir nested\n", f);
    fclose(f);
    if (read_meta("/tmp/myos-ext-test/instructions/demo/setup.inst", name, 32, descr, 80) != 0
        || strcmp(name, "demo") || strcmp(descr, "hello there")) {
        printf("FAIL meta '%s' '%s'\n", name, descr); fails++;
    }
    if (install_name("/tmp/myos-ext-test/instructions", "demo") != 0) { printf("FAIL install\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/demo/nested", &st) != 0 || st.type != 2) { printf("FAIL nested\n"); fails++; }
    mkdir_p("/tmp/myos-ext-test/instructions/bad");
    f = fopen("/tmp/myos-ext-test/instructions/bad/setup.inst", "w");
    if (f) { fputs("name bad\ndescr x\nnope\n", f); fclose(f); }
    if (install_name("/tmp/myos-ext-test/instructions", "bad") == 0) { printf("FAIL unknown accepted\n"); fails++; }
    if (install_name("/tmp/myos-ext-test/instructions", "missing") == 0) { printf("FAIL missing accepted\n"); fails++; }

    unlink("/tmp/myos-ext-test/base.log");
    unlink("/tmp/myos-ext-test/installed/base");
    unlink("/tmp/myos-ext-test/installed/mid");
    unlink("/tmp/myos-ext-test/installed/top");
    unlink("/tmp/myos-ext-test/installed/untar");
    unlink("/tmp/myos-ext-test/installed/c1");
    unlink("/tmp/myos-ext-test/installed/c2");
    mkdir_p("/tmp/myos-ext-test/base");
    f = fopen("/tmp/myos-ext-test/base/Makefile", "w");
    if (!f) { printf("FAIL makefile\n"); return 1; }
    fputs("all:\n\techo base >> /tmp/myos-ext-test/base.log\n", f);
    fclose(f);
    mkdir_p("/tmp/myos-ext-test/instructions/base");
    f = fopen("/tmp/myos-ext-test/instructions/base/setup.inst", "w");
    if (!f) { printf("FAIL base inst\n"); return 1; }
    fputs("name base\ndescr base\nworkdir /tmp/myos-ext-test/base\nmake\n", f);
    fclose(f);
    mkdir_p("/tmp/myos-ext-test/instructions/mid");
    f = fopen("/tmp/myos-ext-test/instructions/mid/setup.inst", "w");
    if (!f) { printf("FAIL mid inst\n"); return 1; }
    fputs("name mid\ndescr mid\ndepend base\nworkdir /tmp/myos-ext-test/mid\nmkdir middir\n", f);
    fclose(f);
    mkdir_p("/tmp/myos-ext-test/instructions/top");
    f = fopen("/tmp/myos-ext-test/instructions/top/setup.inst", "w");
    if (!f) { printf("FAIL top inst\n"); return 1; }
    fputs("name top\ndescr top\ndepend mid\nworkdir /tmp/myos-ext-test/top\nmkdir topdir\nmake\n", f);
    fclose(f);
    unlink("/tmp/myos-ext-test/top.log");
    mkdir_p("/tmp/myos-ext-test/top");
    f = fopen("/tmp/myos-ext-test/top/Makefile", "w");
    if (!f) { printf("FAIL top makefile\n"); return 1; }
    fputs("all:\n\techo top >> /tmp/myos-ext-test/top.log\n", f);
    fclose(f);
    if (install_name("/tmp/myos-ext-test/instructions", "top") != 0) { printf("FAIL chain\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/mid/middir", &st) != 0 || st.type != 2) { printf("FAIL middir\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/top/topdir", &st) != 0 || st.type != 2) { printf("FAIL topdir\n"); fails++; }
    {
        char got[64];
        FILE *lf = fopen("/tmp/myos-ext-test/base.log", "r");
        int n = 0, c;
        if (!lf) { printf("FAIL base log missing\n"); fails++; }
        else {
            while ((c = fgetc(lf)) != EOF && n < (int)sizeof got - 1) got[n++] = (char)c;
            got[n] = 0;
            fclose(lf);
            if (strcmp(got, "base\n")) { printf("FAIL base log '%s'\n", got); fails++; }
        }
    }
    if (install_name("/tmp/myos-ext-test/instructions", "top") != 0) { printf("FAIL chain again\n"); fails++; }
    {
        char got[64];
        FILE *lf = fopen("/tmp/myos-ext-test/base.log", "r");
        int n = 0, c;
        if (!lf) { printf("FAIL base log after skip\n"); fails++; }
        else {
            while ((c = fgetc(lf)) != EOF && n < (int)sizeof got - 1) got[n++] = (char)c;
            got[n] = 0;
            fclose(lf);
            if (strcmp(got, "base\n")) { printf("FAIL dep rebuilt '%s'\n", got); fails++; }
        }
        lf = fopen("/tmp/myos-ext-test/top.log", "r");
        n = 0;
        if (!lf) { printf("FAIL top log after skip\n"); fails++; }
        else {
            while ((c = fgetc(lf)) != EOF && n < (int)sizeof got - 1) got[n++] = (char)c;
            got[n] = 0;
            fclose(lf);
            if (strcmp(got, "top\n")) { printf("FAIL top rebuilt '%s'\n", got); fails++; }
        }
    }

    {
        static const unsigned char hello_gz[] = {
            31,139,8,0,0,0,0,0,2,255,237,205,49,10,131,64,20,4,208,95,231,20,57,65,88,204,98,206,99,33,40,8,
            66,178,129,28,223,141,149,216,43,132,188,215,204,48,205,12,253,52,205,183,242,41,113,156,84,181,57,175,89,237,
            179,186,111,250,119,127,180,169,137,107,138,19,188,95,165,123,214,203,248,79,195,120,9,0,0,0,0,0,0,0,0,0,126,
            207,2,97,63,34,243,0,40,0,0
        };
        char got[16];
        int n = 0, c;
        FILE *hf;
        f = fopen("/tmp/myos-ext-test/hello.tar.gz", "w");
        if (!f) { printf("FAIL write gz\n"); return 1; }
        if (fwrite(hello_gz, 1, sizeof hello_gz, f) != sizeof hello_gz) { printf("FAIL short gz\n"); fclose(f); return 1; }
        fclose(f);
        mkdir_p("/tmp/myos-ext-test/instructions/untar");
        f = fopen("/tmp/myos-ext-test/instructions/untar/setup.inst", "w");
        if (!f) { printf("FAIL untar inst\n"); return 1; }
        fputs("name untar\ndescr u\nworkdir /tmp/myos-ext-test/untar\ntar xf /tmp/myos-ext-test/hello.tar.gz\n", f);
        fclose(f);
        if (install_name("/tmp/myos-ext-test/instructions", "untar") != 0) { printf("FAIL untar\n"); fails++; }
        hf = fopen("/tmp/myos-ext-test/untar/hello.txt", "r");
        if (!hf) { printf("FAIL hello missing\n"); fails++; }
        else {
            while ((c = fgetc(hf)) != EOF && n < (int)sizeof got - 1) got[n++] = (char)c;
            got[n] = 0;
            fclose(hf);
            if (strcmp(got, "hi\n")) { printf("FAIL hello '%s'\n", got); fails++; }
        }
    }

    mkdir_p("/tmp/myos-ext-test/instructions/c1");
    f = fopen("/tmp/myos-ext-test/instructions/c1/setup.inst", "w");
    if (f) { fputs("name c1\ndescr c\ndepend c2\n", f); fclose(f); }
    mkdir_p("/tmp/myos-ext-test/instructions/c2");
    f = fopen("/tmp/myos-ext-test/instructions/c2/setup.inst", "w");
    if (f) { fputs("name c2\ndescr c\ndepend c1\n", f); fclose(f); }
    if (install_name("/tmp/myos-ext-test/instructions", "c1") == 0) { printf("FAIL cycle accepted\n"); fails++; }
    if (stamp_exists("/tmp/myos-ext-test/instructions", "c1") || stamp_exists("/tmp/myos-ext-test/instructions", "c2")) {
        printf("FAIL cycle stamp\n"); fails++;
    }

    unlink("/tmp/myos-ext-test/installed/boxed");
    unlink("/tmp/myos-ext-test/installed/badpatch");
    unlink("/tmp/myos-ext-test/installed/noscript");
    mkdir_p("/tmp/myos-ext-test/instructions/boxed");
    f = fopen("/tmp/myos-ext-test/instructions/boxed/note.txt", "w");
    if (!f) { printf("FAIL note\n"); return 1; }
    fputs("hi\n", f);
    fclose(f);
    f = fopen("/tmp/myos-ext-test/instructions/boxed/base.txt", "w");
    if (!f) { printf("FAIL base file\n"); return 1; }
    fputs("hello\n", f);
    fclose(f);
    f = fopen("/tmp/myos-ext-test/instructions/boxed/fix.diff", "w");
    if (!f) { printf("FAIL diff\n"); return 1; }
    fputs("--- a/base.txt\n+++ b/base.txt\n@@ -1 +1 @@\n-hello\n+world\n", f);
    fclose(f);
    f = fopen("/tmp/myos-ext-test/instructions/boxed/setup.inst", "w");
    if (!f) { printf("FAIL boxed inst\n"); return 1; }
    fputs("name boxed\ndescr box\nworkdir /tmp/myos-ext-test/boxed\n"
          "copy note.txt note.txt\ncopy base.txt base.txt\npatch fix.diff\n", f);
    fclose(f);
    f = fopen("/tmp/myos-ext-test/instructions/boxed/uninstall.inst", "w");
    if (!f) { printf("FAIL boxed uninstall\n"); return 1; }
    fputs("rm /tmp/myos-ext-test/boxed/note.txt\nrm /tmp/myos-ext-test/boxed/base.txt\n", f);
    fclose(f);
    if (install_name("/tmp/myos-ext-test/instructions", "boxed") != 0) { printf("FAIL boxed\n"); fails++; }
    if (!file_is("/tmp/myos-ext-test/boxed/note.txt", "hi\n")) { printf("FAIL copied note\n"); fails++; }
    if (!file_is("/tmp/myos-ext-test/boxed/base.txt", "world\n")) { printf("FAIL patched base\n"); fails++; }
    if (uninstall_name("/tmp/myos-ext-test/instructions", "boxed") != 0) { printf("FAIL uninstall\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/boxed/note.txt", &st) == 0 || dirstat("/tmp/myos-ext-test/boxed/base.txt", &st) == 0) {
        printf("FAIL files remain\n"); fails++;
    }
    if (stamp_exists("/tmp/myos-ext-test/instructions", "boxed")) { printf("FAIL stamp remains\n"); fails++; }
    if (uninstall_name("/tmp/myos-ext-test/instructions", "boxed") == 0) { printf("FAIL second uninstall\n"); fails++; }

    mkdir_p("/tmp/myos-ext-test/instructions/badpatch");
    f = fopen("/tmp/myos-ext-test/instructions/badpatch/base.txt", "w");
    if (f) { fputs("hello\n", f); fclose(f); }
    f = fopen("/tmp/myos-ext-test/instructions/badpatch/wrong.diff", "w");
    if (f) { fputs("--- a/base.txt\n+++ b/base.txt\n@@ -1 +1 @@\n-nope\n+world\n", f); fclose(f); }
    f = fopen("/tmp/myos-ext-test/instructions/badpatch/setup.inst", "w");
    if (f) {
        fputs("name badpatch\ndescr bad\nworkdir /tmp/myos-ext-test/badpatch\n"
              "copy base.txt base.txt\npatch wrong.diff\n", f);
        fclose(f);
    }
    if (install_name("/tmp/myos-ext-test/instructions", "badpatch") == 0) { printf("FAIL bad patch accepted\n"); fails++; }
    if (!file_is("/tmp/myos-ext-test/badpatch/base.txt", "hello\n")) { printf("FAIL bad patch wrote\n"); fails++; }
    if (stamp_exists("/tmp/myos-ext-test/instructions", "badpatch")) { printf("FAIL bad patch stamp\n"); fails++; }

    mkdir_p("/tmp/myos-ext-test/instructions/noscript");
    f = fopen("/tmp/myos-ext-test/instructions/noscript/setup.inst", "w");
    if (f) { fputs("name noscript\ndescr n\nworkdir /tmp/myos-ext-test/noscript\nmkdir kept\n", f); fclose(f); }
    if (install_name("/tmp/myos-ext-test/instructions", "noscript") != 0) { printf("FAIL noscript\n"); fails++; }
    if (uninstall_name("/tmp/myos-ext-test/instructions", "noscript") == 0) { printf("FAIL missing uninstall\n"); fails++; }
    if (!stamp_exists("/tmp/myos-ext-test/instructions", "noscript")) { printf("FAIL noscript stamp\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/noscript/kept", &st) != 0 || st.type != 2) { printf("FAIL kept\n"); fails++; }

    git_hide_system = 1;
    git_test_bin = "/tmp/myos-ext-test/fake-git";
    unlink("/tmp/myos-ext-test/fake-git");
    unlink("/tmp/myos-ext-test/git-dep.log");
    unlink("/tmp/myos-ext-test/git-args");
    unlink("/tmp/myos-ext-test/installed/git");
    unlink("/tmp/myos-ext-test/installed/app");
    unlink("/tmp/myos-ext-test/installed/app2");
    unlink("/tmp/myos-ext-test/app-src/git-ran");
    unlink("/tmp/myos-ext-test/app2-src/git-ran");
    mkdir_p("/tmp/myos-ext-test/gitpkg");
    f = fopen("/tmp/myos-ext-test/gitpkg/Makefile", "w");
    if (!f) { printf("FAIL git makefile\n"); return 1; }
    fputs("all:\n"
          "\techo gitdep >> /tmp/myos-ext-test/git-dep.log\n"
          "\techo '#!/bin/sh' > /tmp/myos-ext-test/fake-git\n"
          "\techo 'printf \"%s\\n\" \"$$@\" > /tmp/myos-ext-test/git-args' >> /tmp/myos-ext-test/fake-git\n"
          "\techo ': > git-ran' >> /tmp/myos-ext-test/fake-git\n"
          "\tchmod 755 /tmp/myos-ext-test/fake-git\n", f);
    fclose(f);
    mkdir_p("/tmp/myos-ext-test/instructions/git");
    f = fopen("/tmp/myos-ext-test/instructions/git/setup.inst", "w");
    if (!f) { printf("FAIL git inst\n"); return 1; }
    fputs("name git\ndescr git\nworkdir /tmp/myos-ext-test/gitpkg\nmake\n", f);
    fclose(f);
    mkdir_p("/tmp/myos-ext-test/instructions/app");
    f = fopen("/tmp/myos-ext-test/instructions/app/setup.inst", "w");
    if (!f) { printf("FAIL app inst\n"); return 1; }
    fputs("name app\ndescr app\nworkdir /tmp/myos-ext-test/app-src\n"
          "git clone --depth 1 https://example.com/s.git src\n", f);
    fclose(f);
    if (install_name("/tmp/myos-ext-test/instructions", "app") != 0) { printf("FAIL git dep\n"); fails++; }
    if (!file_is("/tmp/myos-ext-test/git-dep.log", "gitdep\n")) { printf("FAIL git not installed\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/app-src/git-ran", &st) != 0 || st.type != 1) { printf("FAIL git cwd\n"); fails++; }
    if (!file_is("/tmp/myos-ext-test/git-args", "clone\n--depth\n1\nhttps://example.com/s.git\nsrc\n")) {
        printf("FAIL git args\n"); fails++;
    }
    if (install_name("/tmp/myos-ext-test/instructions", "app") != 0) { printf("FAIL git dep again\n"); fails++; }
    if (!file_is("/tmp/myos-ext-test/git-dep.log", "gitdep\n")) { printf("FAIL git rebuilt\n"); fails++; }

    unlink("/tmp/myos-ext-test/installed/git");
    unlink("/tmp/myos-ext-test/git-dep.log");
    mkdir_p("/tmp/myos-ext-test/instructions/app2");
    f = fopen("/tmp/myos-ext-test/instructions/app2/setup.inst", "w");
    if (!f) { printf("FAIL app2 inst\n"); return 1; }
    fputs("name app2\ndescr app2\nworkdir /tmp/myos-ext-test/app2-src\ngit status\n", f);
    fclose(f);
    if (install_name("/tmp/myos-ext-test/instructions", "app2") != 0) { printf("FAIL git present\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/git-dep.log", &st) == 0) { printf("FAIL git reinstalled\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/app2-src/git-ran", &st) != 0 || st.type != 1) { printf("FAIL git present cwd\n"); fails++; }
    if (!file_is("/tmp/myos-ext-test/git-args", "status\n")) { printf("FAIL git status args\n"); fails++; }
    if (stamp_exists("/tmp/myos-ext-test/instructions", "git")) { printf("FAIL git stamp without install\n"); fails++; }

    unlink("/tmp/myos-ext-test/fake-git");
    unlink("/tmp/myos-ext-test/installed/git");
    f = fopen("/tmp/myos-ext-test/instructions/git/setup.inst", "w");
    if (!f) { printf("FAIL git cycle inst\n"); return 1; }
    fputs("name git\ndescr git\ngit status\n", f);
    fclose(f);
    if (install_name("/tmp/myos-ext-test/instructions", "git") == 0) { printf("FAIL git cycle accepted\n"); fails++; }
    if (stamp_exists("/tmp/myos-ext-test/instructions", "git")) { printf("FAIL git cycle stamp\n"); fails++; }
    git_hide_system = 0;
    git_test_bin = NULL;
    {
        struct auto_packages selected;
        FILE *af=fopen("/tmp/myos-ext-test/auto.conf","w");
        if(af){fputs("# selected\nbase\ntop # comment\nbase\n",af);fclose(af);}
        if(!af || auto_read("/tmp/myos-ext-test/auto.conf",&selected) || selected.count!=2 || strcmp(selected.names[1],"top")){printf("FAIL auto selection\n");fails++;}
        af=fopen("/tmp/myos-ext-test/auto.conf","w");
        if(af){fputs("base top\n",af);fclose(af);}
        if(!auto_read("/tmp/myos-ext-test/auto.conf",&selected)){printf("FAIL auto multiple names accepted\n");fails++;}
        af=fopen("/tmp/myos-ext-test/auto.conf","w");
        if(af){fputs("../base\n",af);fclose(af);}
        if(!auto_read("/tmp/myos-ext-test/auto.conf",&selected)){printf("FAIL auto traversal accepted\n");fails++;}
    }
    return fails != 0;
}
#endif

int main(int argc, char **argv) {
#if defined(__LINUX_TEST__)
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
#endif
    if (argc == 2 && !strcmp(argv[1], "list")) return list_dir(INST_DIR);
    if(argc>=2 && !strcmp(argv[1],"auto"))return auto_command(argc,argv);
    if(argc>=3 && !strcmp(argv[1],"install")) {
        const char *name=NULL;
        for(int i=2;i<argc;i++) {
            if(!strncmp(argv[i],"-j",2)) {
                const char *value=argv[i]+2;
                if(!*value) {
                    value="auto";
                    if(i+1<argc && ((argv[i+1][0]>='0' && argv[i+1][0]<='9') || !strcmp(argv[i+1],"auto"))) value=argv[++i];
                }
                build_jobs_override=build_jobs_parse(value);
                if(build_jobs_override<0) { dprintf(STDERR,"ext: jobs must be auto or 1..%d\n",BUILD_JOBS_MAX); return 2; }
            } else if(!name) name=argv[i];
            else { usage(); return 2; }
        }
        if(!name) { usage(); return 2; }
        return ext_lock()?1:install_name(INST_DIR,name);
    }
    if (argc == 3 && !strcmp(argv[1], "uninstall")) return ext_lock()?1:uninstall_name(INST_DIR, argv[2]);
    usage();
    return 2;
}
