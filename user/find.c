/* find path... выражение. Хватает вызова из Makefile Git: -name -type -prune -print, -o и скобки. */
#include "myos.h"
#include "string.h"
#include "sys/stat.h"
#include "dirent.h"
#include "fnmatch.h"

enum { N_AND, N_OR, N_NAME, N_TYPE, N_PRUNE, N_PRINT };
struct node { int op; char *arg; struct node *l, *r; };

static char **av;
static int ai, ac, saw_action, pruned;

static struct node *node(int op, struct node *l, struct node *r, const char *arg) {
    struct node *n = malloc(sizeof *n);
    if (!n) exit(1);
    n->op = op; n->l = l; n->r = r; n->arg = NULL;
    if (arg) {
        int k = (int)strlen(arg);
        n->arg = malloc((size_t)k + 1);
        if (n->arg) memcpy(n->arg, arg, (size_t)k + 1);
    }
    return n;
}
static int is_primary(const char *s) {
    return s && (s[0] != '-' || !strcmp(s, "-name") || !strcmp(s, "-type") || !strcmp(s, "-prune") || !strcmp(s, "-print"));
}
static int is_lpar(const char *s) { return s && (!strcmp(s, "(") || !strcmp(s, "\\(")); }
static int is_rpar(const char *s) { return s && (!strcmp(s, ")") || !strcmp(s, "\\)")); }

static struct node *parse_or(void);
static struct node *parse_prim(void) {
    const char *s;
    if (ai >= ac) return NULL;
    s = av[ai++];
    if (is_lpar(s)) {
        struct node *n = parse_or();
        if (ai < ac && is_rpar(av[ai])) ai++;
        return n;
    }
    if (!strcmp(s, "-name") && ai < ac) return node(N_NAME, NULL, NULL, av[ai++]);
    if (!strcmp(s, "-type") && ai < ac) return node(N_TYPE, NULL, NULL, av[ai++]);
    if (!strcmp(s, "-prune")) { saw_action = 1; return node(N_PRUNE, NULL, NULL, NULL); }
    if (!strcmp(s, "-print")) { saw_action = 1; return node(N_PRINT, NULL, NULL, NULL); }
    ai--;
    return NULL;
}
static struct node *parse_and(void) {
    struct node *l = parse_prim();
    while (l && ai < ac && !is_rpar(av[ai]) && strcmp(av[ai], "-o") && is_primary(av[ai])) {
        struct node *r = parse_prim();
        if (!r) break;
        l = node(N_AND, l, r, NULL);
    }
    return l;
}
static struct node *parse_or(void) {
    struct node *l = parse_and();
    while (l && ai < ac && !strcmp(av[ai], "-o")) {
        ai++;
        l = node(N_OR, l, parse_and(), NULL);
    }
    return l;
}

static const char *base_of(const char *path) {
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/') s = p + 1;
    return s;
}
static int eval(struct node *n, const char *path, const struct stat *st) {
    int dir, reg, lnk;
    if (!n) return 1;
    if (n->op == N_AND) return eval(n->l, path, st) && eval(n->r, path, st);
    if (n->op == N_OR) return eval(n->l, path, st) || eval(n->r, path, st);
    if (n->op == N_NAME) return fnmatch(n->arg, base_of(path), 0) == 0;
    dir = S_ISDIR(st->st_mode); reg = S_ISREG(st->st_mode); lnk = S_ISLNK(st->st_mode);
    if (n->op == N_TYPE) {
        char t = n->arg ? n->arg[0] : 0;
        if (t == 'd') return dir;
        if (t == 'f') return reg;
        if (t == 'l') return lnk;
        return 0;
    }
    if (n->op == N_PRUNE) { pruned = 1; return 1; }
    if (n->op == N_PRINT) { printf("%s\n", path); return 1; }
    return 0;
}

static void walk(const char *path, struct node *expr) {
    struct stat st;
    DIR *d;
    if (lstat(path, &st) < 0) return;
    pruned = 0;
    if (eval(expr, path, &st) && !saw_action) printf("%s\n", path);
    if (pruned || !S_ISDIR(st.st_mode)) return;
    d = opendir(path);
    if (!d) return;
    for (;;) {
        struct dirent *de = readdir(d);
        char child[1024];
        int nd, nn, slash;
        if (!de) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        nd = (int)strlen(path); nn = (int)strlen(de->d_name);
        slash = !(nd == 1 && path[0] == '/') && nd > 0 && path[nd - 1] != '/';
        if (nd + slash + nn + 1 > (int)sizeof child) continue;
        memcpy(child, path, (size_t)nd);
        if (slash) child[nd++] = '/';
        memcpy(child + nd, de->d_name, (size_t)nn + 1);
        walk(child, expr);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    int i = 1, paths;
    struct node *expr;
    if (argc < 2) { dprintf(STDERR, "usage: find path... [expression]\n"); return 1; }
    while (i < argc && argv[i][0] != '-' && !is_lpar(argv[i])) i++;
    paths = i;
    av = argv; ac = argc; ai = i;
    expr = ai < ac ? parse_or() : NULL;
    if (paths == 1) walk(".", expr);
    for (i = 1; i < paths; i++) walk(argv[i], expr);
    return 0;
}
