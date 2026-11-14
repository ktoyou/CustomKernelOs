/* cc — компилятор Си для MyOS. Запуск:
 *
 *   cc hello.c                 -> программа hello в текущей папке
 *   cc -o /bin/hi a.c b.c      -> несколько файлов в одну программу
 *   cc -E hello.c              -> показать текст после препроцессора
 *
 *   cc -c a.c && cc a.o -lz    -> объектный файл, потом программа с /lib/libz.a
 *   cc a.c -ldl                -> dlopen из /lib/libdl.so, состояние в ld.so
 *   cc a.c -lpthread           -> pthread_create из /lib/libpthread.so
 *
 * Заголовки (#include <myos.h>, <gfx.h>) берутся из /include. Вместе с программой
 * линкуются libm, libgfx и libc. Если архивов gfx и libc нет, компилируются
 * исходники /lib/libc.c, gfx.c, vec.c, font.c, win.c. */
#include "cc.h"

char **include_paths;
int    n_include_paths;
bool   opt_linux;
bool   opt_pic;
bool   opt_shared, opt_static;
char  *opt_soname;

static char *base_dir = "";         /* -B: где искать include/ и lib/ (по умолчанию корень MyOS) */

/* ------------------------------------------------------------ сообщения об ошибках */

static void vreport(const char *file, int line, char *loc, char *line_start, const char *kind, const char *fmt, va_list ap) {
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (file) dprintf(STDERR, "%s:%d: %s: %s\n", file, line, kind, msg);
    else dprintf(STDERR, "cc: %s: %s\n", kind, msg);
    if (loc && line_start) {
        char *end = line_start;
        while (*end && *end != '\n') end++;
        dprintf(STDERR, "    %.*s\n    ", (int)(end - line_start), line_start);
        for (char *p = line_start; p < loc; p++) dprintf(STDERR, "%c", *p == '\t' ? '\t' : ' ');
        dprintf(STDERR, "^\n");
    }
}

void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport(NULL, 0, NULL, NULL, "error", fmt, ap);
    va_end(ap);
    exit(1);
}

static char *line_start_of(Token *tok) {
    if (!tok->file || !tok->file->contents || !tok->loc) return NULL;
    char *base = tok->file->contents;
    if (tok->loc < base || tok->loc > base + strlen(base)) return NULL;    /* токен из макроса */
    char *p = tok->loc;
    while (p > base && p[-1] != '\n') p--;
    return p;
}

void error_tok(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport(tok->file ? tok->file->name : "?", tok->line, tok->loc, line_start_of(tok), "error", fmt, ap);
    va_end(ap);
    exit(1);
}

void warn_tok(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport(tok->file ? tok->file->name : "?", tok->line, tok->loc, line_start_of(tok), "warning", fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ мелочи */

void *xcalloc(int n) {
    void *p = calloc(1, n > 0 ? n : 1);
    if (!p) error("out of memory");
    return p;
}

char *strndup2(const char *s, int n) {
    char *p = xcalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *format(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strndup2(buf, strlen(buf));
}

char *read_file(const char *path, int *out_len) {
#ifdef CC_HOST
    int fd = open(path, O_RDONLY);
#else
    struct dirent_info st;
    if (dirstat(path, &st) < 0 || st.type != 1) return NULL;
    int fd = open(path, O_READ);
#endif
    if (fd < 0) return NULL;
    int cap = 4096, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 2048 > cap) {
            cap *= 2;
            char *nb = malloc(cap);
            if (!nb) error("out of memory");
            memcpy(nb, buf, len);
            free(buf);
            buf = nb;
        }
        int n = read(fd, buf + len, cap - len - 1);
        if (n < 0) { close(fd); free(buf); return NULL; }
        if (n == 0) break;
        len += n;
    }
    close(fd);
    if (len == 0 || buf[len - 1] != '\n') buf[len++] = '\n';  /* файл всегда кончается переводом строки */
    buf[len] = 0;
    *out_len = len;
    return buf;
}

static void add_include(char *path) {
    char **p = xcalloc(sizeof(char *) * (n_include_paths + 1));
    for (int i = 0; i < n_include_paths; i++) p[i] = include_paths[i];
    p[n_include_paths++] = path;
    include_paths = p;
}

static void usage(void) {
    dprintf(STDERR,
        "usage: cc [-c] [-fPIC] [-shared] [-static] [-soname name] [-o out] [-E]\n"
        "          [-I dir] [-L dir] [-D name] [-l name] [-nostdlib] file.c... file.o... lib.a... lib.so...\n"
        "  -c          only compile: each file.c -> file.o (ELF relocatable object)\n"
        "  -fPIC       position-independent code (GOT, PLT)\n"
        "  -shared     build a shared library (.so); implies -fPIC\n"
        "  -static     do not link shared libraries\n"
        "  -soname name  DT_SONAME of a shared library (default: the -o file name)\n"
        "  -o out      name of the program or library or, with -c and one file, of the object\n"
        "              (default: first file without .c or .o)\n"
        "  -E          only preprocess, print the result\n"
        "  -I dir      also look for #include files in dir\n"
        "  -D name     predefined macro, name or name=body\n"
        "  -L dir      also look for -l libraries in dir (before /lib and /usr/lib)\n"
        "  -l name     link libname.so if it exists, otherwise libname.a (-static: only .a)\n"
        "  -nostdlib   do not link libm, libgfx and libc\n"
        "              (without them on disk: /lib/gfx.c, vec.c, font.c, win.c, libc.c)\n"
        "Objects (.o) may come from cc -c or from gcc -m32 -fno-pic; libraries from ar or cc -shared.\n");
    exit(2);
}

static bool has_suffix(const char *s, const char *suf) {
    int n = strlen(s), m = strlen(suf);
    return n > m && !strcmp(s + n - m, suf);
}

static Obj *compile(const char *path) {
    Token *tok = tokenize_file(path);
    if (!tok) error("%s: no such file", path);
    return parse(preprocess(tok));
}

/* растущий список строк */
typedef struct { char **v; int n, cap; } StrList;

static void push(StrList *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        char **p = xcalloc(sizeof(char *) * l->cap);
        if (l->v) memcpy(p, l->v, sizeof(char *) * l->n);
        l->v = p;
    }
    l->v[l->n++] = s;
}

static bool file_exists(const char *path) {
#ifdef CC_HOST
    int fd = open(path, O_RDONLY);
#else
    struct dirent_info st;
    if (dirstat(path, &st) < 0 || st.type != 1) return false;
    int fd = open(path, O_READ);
#endif
    if (fd < 0) return false;
    close(fd);
    return true;
}

static StrList lib_dirs;

/* -lname: сначала libname.so (если не -static), потом libname.a.
 * Каталоги: -L, затем /lib и /usr/lib. */
static char *find_lib(const char *name) {
    int passes = opt_static ? 1 : 2;
    for (int pass = 0; pass < passes; pass++) {
        const char *ext = (!opt_static && pass == 0) ? ".so" : ".a";
        for (int i = 0; i < lib_dirs.n; i++) {
            char *p = format("%s/lib%s%s", lib_dirs.v[i], name, ext);
            if (file_exists(p)) return p;
        }
    }
    error("cannot find -l%s", name);
}

static Archive *open_lib(const char *path) {
    Archive *a = archive_read(path);
    if (!a) error("%s: no such file", path);
    return a;
}

static void print_tokens(Token *tok) {
    int line = 1;
    for (; tok->kind != TK_EOF; tok = tok->next) {
        if (line > 1 && tok->at_bol) dprintf(STDOUT, "\n");
        if (tok->has_space && !tok->at_bol) dprintf(STDOUT, " ");
        dprintf(STDOUT, "%.*s", tok->len, tok->loc);
        line++;
    }
    dprintf(STDOUT, "\n");
}

/* последний компонент пути без .c / .o, плюс ext */
static char *default_output(const char *src, const char *ext) {
    const char *b = strrchr(src, '/');
    b = b ? b + 1 : src;
    int n = strlen(b);
    if (n > 2 && b[n - 2] == '.' && (b[n - 1] == 'c' || b[n - 1] == 'o')) n -= 2;
    if (n == 0) return *ext ? "a.o" : "a.out";
    return format("%s%s", strndup2(b, n), ext);
}

int main(int argc, char **argv) {
    char *out = NULL, *first = NULL;
    bool opt_E = false, opt_c = false, nostdlib = false, saw_nopic = false;
    StrList srcs = {0}, objs = {0}, libs = {0}, lnames = {0}, sos = {0};
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) { out = argv[++i]; continue; }
        if (!strcmp(a, "-c")) { opt_c = true; continue; }
        if (!strcmp(a, "-E")) { opt_E = true; continue; }
        if (!strcmp(a, "-linux")) { opt_linux = true; continue; }
        if (!strcmp(a, "-shared")) { opt_shared = true; continue; }
        if (!strcmp(a, "-static")) { opt_static = true; continue; }
        if (!strcmp(a, "-soname") && i + 1 < argc) { opt_soname = argv[++i]; continue; }
        if (!strcmp(a, "-fPIC") || !strcmp(a, "-fpic")) { opt_pic = true; continue; }
        if (!strcmp(a, "-fno-pic") || !strcmp(a, "-fno-PIC")) { opt_pic = false; saw_nopic = true; continue; }
        if (!strcmp(a, "-nostdlib")) { nostdlib = true; continue; }
        if (!strcmp(a, "-B") && i + 1 < argc) { base_dir = argv[++i]; continue; }
        if (!strcmp(a, "-I") && i + 1 < argc) { add_include(argv[++i]); continue; }
        if (!strncmp(a, "-I", 2) && a[2]) { add_include(a + 2); continue; }
        if (!strcmp(a, "-D") && i + 1 < argc) { pp_define(argv[++i]); continue; }
        if (!strncmp(a, "-D", 2) && a[2]) { pp_define(a + 2); continue; }
        if (!strcmp(a, "-L") && i + 1 < argc) { push(&lib_dirs, argv[++i]); continue; }
        if (!strncmp(a, "-L", 2) && a[2]) { push(&lib_dirs, a + 2); continue; }
        /* -l ищется после разбора всех -L: место в списке библиотек запоминаем сразу */
        if (!strcmp(a, "-l") && i + 1 < argc) { push(&lnames, argv[++i]); push(&libs, NULL); continue; }
        if (!strncmp(a, "-l", 2) && a[2]) { push(&lnames, a + 2); push(&libs, NULL); continue; }
        /* Флаги gcc, которые Makefile Git передаёт и которые на код не влияют. */
        if (!strcmp(a, "-g") || !strncmp(a, "-g", 2) || !strncmp(a, "-O", 2) || !strncmp(a, "-W", 2) ||
            !strncmp(a, "-m", 2) || !strncmp(a, "-std=", 5) || !strncmp(a, "-pedantic", 9) ||
            !strcmp(a, "-pthread") || !strncmp(a, "-f", 2)) continue;
        if (a[0] == '-') usage();
        if (!first) first = a;
        if (has_suffix(a, ".o")) push(&objs, a);
        else if (has_suffix(a, ".a")) push(&libs, a);
        else if (has_suffix(a, ".so")) push(&sos, a);
        else push(&srcs, a);
    }
    if (!first) usage();
    if (opt_shared && opt_static) error("-shared and -static cannot be used together");
    if (opt_shared && saw_nopic) error("-shared requires -fPIC");
    if (opt_shared && !saw_nopic) opt_pic = true;
    add_include(format("%s/include", base_dir));
    push(&lib_dirs, format("%s/lib", base_dir));
    push(&lib_dirs, format("%s/usr/lib", base_dir));

    if (opt_E) {
        for (int i = 0; i < srcs.n; i++) {
            Token *tok = tokenize_file(srcs.v[i]);
            if (!tok) error("%s: no such file", srcs.v[i]);
            print_tokens(preprocess(tok));
        }
        return 0;
    }

    /* -c: каждый .c — в свой .o, без библиотеки и без компоновки */
    if (opt_c) {
        if (opt_shared) error("-shared cannot be used with -c");
        if (!srcs.n) error("-c: no C files to compile");
        if (out && srcs.n > 1) error("-o with -c needs exactly one C file");
        for (int i = 0; i < srcs.n; i++)
            obj_write(codegen(compile(srcs.v[i]), srcs.v[i], NULL, 0, true), out ? out : default_output(srcs.v[i], ".o"));
        return 0;
    }

    int lw = 0, lk = 0;
    for (int i = 0; i < libs.n; i++) {
        char *p = libs.v[i] ? libs.v[i] : find_lib(lnames.v[lk++]);
        if (has_suffix(p, ".so")) { push(&sos, p); continue; }
        libs.v[lw++] = p;
    }
    libs.n = lw;
    if (opt_static && sos.n) error("cannot link a shared library with -static");

    /* Стандартные библиотеки: готовые .a, а если их нет — исходники */
    StrList libsrcs = {0};
    if (!nostdlib) {
        int have_m = false;
        for (int i = 0; i < libs.n; i++)
            if (has_suffix(libs.v[i], "libm.a")) have_m = true;
        for (int i = 0; i < sos.n; i++)
            if (has_suffix(sos.v[i], "libm.so")) have_m = true;
        if (!have_m) {
            char *so = format("%s/lib/libm.so", base_dir), *a = format("%s/lib/libm.a", base_dir);
            if (!opt_static && file_exists(so)) push(&sos, so);
            else if (file_exists(a)) push(&libs, a);
        }
        char *gfx_a = format("%s/lib/libgfx.a", base_dir), *libc_a = format("%s/lib/libc.a", base_dir);
        if (file_exists(gfx_a)) push(&libs, gfx_a);
        else {
            static const char *extra[] = { "gfx.c", "vec.c", "font.c", "win.c" };  /* есть на диске — добавляем */
            for (int k = 0; k < 4; k++) {
                char *g = format("%s/lib/%s", base_dir, extra[k]);
                if (file_exists(g)) push(&libsrcs, g);
            }
        }
        if (file_exists(libc_a)) push(&libs, libc_a);
        else push(&libsrcs, format("%s/lib/libc.c", base_dir));
    }
    if (!out) out = default_output(first, "");

    /* Все .c программы компилируются в один объект, исходники библиотеки — в
     * другой. Объекты содержат все определения: на них могут сослаться другие
     * файлы (библиотека — на main или на свой malloc вместо библиотечного), а
     * лишнее отсеет компоновщик. */
    ObjFile **files = xcalloc(sizeof(ObjFile *) * (objs.n + 2));
    int n_files = 0;
    for (int i = 0; i < objs.n; i++) {
        ObjFile *f = obj_read(objs.v[i]);
        if (!f) error("%s: no such file", objs.v[i]);
        files[n_files++] = f;
    }
    Archive **archives = xcalloc(sizeof(Archive *) * (libs.n + 1));
    for (int i = 0; i < libs.n; i++) archives[i] = open_lib(libs.v[i]);

    for (int pass = 0; pass < 2; pass++) {
        StrList *list = pass ? &libsrcs : &srcs;
        Obj *prog = NULL, **tail = &prog;
        for (int i = 0; i < list->n; i++) {
            *tail = compile(list->v[i]);
            while (*tail) tail = &(*tail)->next;
        }
        if (!prog) continue;
        ObjFile *f = codegen(prog, pass ? "standard library" : "", NULL, 0, true);
        f->from_lib = pass;
        files[n_files++] = f;
    }
    link_program(files, n_files, archives, libs.n, sos.v, sos.n, out);
    return 0;
}
