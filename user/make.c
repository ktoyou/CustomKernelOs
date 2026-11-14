/* make — чтение Makefile.
 *
 * Правила, переменные и рецепты живут в куче. Понимает то, чем пользуется
 * Makefile Git 2.47: присваивания = := += ?= и override, вложенные
 * ifdef/ifeq, include, define, $(call)/$(eval) и функции текста, шаблонные
 * и статические шаблонные правила, несколько целей, ::, order-only,
 * переменные конкретной цели и $@ $< $^ $* $+ $?. $(shell) запускает
 * $(SHELL) -c. Цель устарела, если файла нет, он старше зависимости,
 * зависимость пересобралась в этом запуске, или цель .PHONY.
 * -jN по-прежнему собирает независимые цели параллельно.
 */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "dirent.h"
#include "sys/stat.h"
#include "sys/wait.h"
#include "build_jobs.h"

#define MAX_JOBS BUILD_JOBS_MAX
extern char **environ;

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "make: out of memory\n"); exit(2); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "make: out of memory\n"); exit(2); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *p = xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static char *xstrndup(const char *s, int n) {
    char *p = xmalloc((size_t)n + 1);
    if (n > 0) memcpy(p, s, (size_t)n);
    p[n > 0 ? n : 0] = 0;
    return p;
}

struct buf { char *s; int n, cap; };
static void buf_init(struct buf *b) { b->s = xmalloc(32); b->s[0] = 0; b->n = 0; b->cap = 32; }
static void buf_add(struct buf *b, const char *p, int n) {
    if (n <= 0) return;
    if (b->n + n + 1 > b->cap) {
        int c = b->cap ? b->cap : 32;
        while (b->n + n + 1 > c) c *= 2;
        b->s = xrealloc(b->s, (size_t)c);
        b->cap = c;
    }
    memcpy(b->s + b->n, p, (size_t)n);
    b->n += n;
    b->s[b->n] = 0;
}
static void buf_puts(struct buf *b, const char *p) { if (p) buf_add(b, p, (int)strlen(p)); }
static void buf_putc(struct buf *b, char c) { buf_add(b, &c, 1); }

#define GROW(ptr, count, cap) do { \
    if ((count) >= (cap)) { \
        int _c = (cap) ? (cap) * 2 : 8; \
        void *_p = realloc((ptr), (size_t)_c * sizeof *(ptr)); \
        if (!_p) { fprintf(stderr, "make: out of memory\n"); exit(2); } \
        (ptr) = _p; (cap) = _c; \
    } \
} while (0)

static char *trim_dup(const char *s) {
    if (!s) s = "";
    while (*s == ' ' || *s == '\t') s++;
    int n = (int)strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return xstrndup(s, n);
}
static void rtrim(char *s) {
    int n = (int)strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
}
static char **split_words(const char *s, int *n) {
    char **a = NULL; int c = 0, cap = 0;
    if (!s) s = "";
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        GROW(a, c, cap);
        a[c++] = xstrndup(s, (int)(e - s));
        s = e;
    }
    *n = c;
    return a;
}
static void free_words(char **w, int n) {
    for (int i = 0; i < n; i++) free(w[i]);
    free(w);
}

static const char *src_name, *src_dir;
static int src_line;
static void note(const char *msg) {
    fprintf(stderr, "make: %s:%d: %s\n", src_name ? src_name : "make", src_line, msg);
}

/* ---------- variables ---------- */

struct var { char *name, *val; int simple, exported, locked; };
static struct var *vars; static int nvars, varcap;
static struct var *var_find(const char *name) {
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) return &vars[i];
    return NULL;
}
static void var_remove(const char *name) {
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) {
        free(vars[i].name); free(vars[i].val);
        vars[i] = vars[--nvars];
        return;
    }
}
/* force overrides a command-line lock. lock keeps later assignments out. */
static void var_set(const char *name, const char *val, int simple, int exported, int lock, int force) {
    struct var *v = var_find(name);
    if (v && v->locked && !force) return;
    if (!v) {
        GROW(vars, nvars, varcap);
        v = &vars[nvars++];
        memset(v, 0, sizeof *v);
        v->name = xstrdup(name);
    }
    free(v->val);
    v->val = xstrdup(val ? val : "");
    v->simple = simple;
    if (exported) v->exported = 1;
    if (lock) v->locked = 1;
}
static char *expand(const char *s);
static void var_append(const char *name, const char *text, int force) {
    struct var *v = var_find(name);
    if (v && v->locked && !force) return;
    if (!v) { var_set(name, text, 0, 0, 0, force); return; }
    struct buf b; buf_init(&b);
    if (v->simple) {
        char *e = expand(text);
        if (v->val[0] && e && e[0]) { buf_puts(&b, v->val); buf_putc(&b, ' '); buf_puts(&b, e); }
        else buf_puts(&b, v->val[0] ? v->val : (e ? e : ""));
        free(e);
    } else {
        if (v->val[0] && text && text[0]) { buf_puts(&b, v->val); buf_putc(&b, ' '); buf_puts(&b, text); }
        else buf_puts(&b, v->val[0] ? v->val : (text ? text : ""));
    }
    free(v->val); v->val = b.s;
}

/* ---------- automatic variables and expansion ---------- */

struct actx { const char *at, *lt, *hat, *plus, *star, *qm, *bar; };
static struct actx *cur_auto;
static int exp_depth;

static char *auto_dir(const char *s) {
    const char *sl = strrchr(s, '/');
    if (!sl) return xstrdup(".");
    if (sl == s) return xstrdup("/");
    return xstrndup(s, (int)(sl - s));
}
static char *auto_file(const char *s) {
    const char *sl = strrchr(s, '/');
    return xstrdup(sl ? sl + 1 : s);
}
static char *auto_get(const char *n) {
    if (!cur_auto) return NULL;
    const char *src = NULL; int kind = 0;
    if (!strcmp(n, "@")) src = cur_auto->at;
    else if (!strcmp(n, "<")) src = cur_auto->lt;
    else if (!strcmp(n, "^")) src = cur_auto->hat;
    else if (!strcmp(n, "+")) src = cur_auto->plus;
    else if (!strcmp(n, "*")) src = cur_auto->star;
    else if (!strcmp(n, "?")) src = cur_auto->qm;
    else if (!strcmp(n, "|")) src = cur_auto->bar;
    else if (!strcmp(n, "@D")) { src = cur_auto->at; kind = 1; }
    else if (!strcmp(n, "<D")) { src = cur_auto->lt; kind = 1; }
    else if (!strcmp(n, "*D")) { src = cur_auto->star; kind = 1; }
    else if (!strcmp(n, "@F")) { src = cur_auto->at; kind = 2; }
    else if (!strcmp(n, "<F")) { src = cur_auto->lt; kind = 2; }
    else if (!strcmp(n, "*F")) { src = cur_auto->star; kind = 2; }
    else return NULL;
    if (!src) src = "";
    if (kind == 1) return auto_dir(src);
    if (kind == 2) return auto_file(src);
    return xstrdup(src);
}
static char *expand_name(const char *name) {
    char *n = trim_dup(name);
    char *a = auto_get(n);
    if (a) { free(n); return a; }
    struct var *v = var_find(n);
    free(n);
    if (!v) return xstrdup("");
    if (v->simple) return xstrdup(v->val);
    return expand(v->val);
}

static int percent_match(const char *pat, const char *word, const char **stem, int *slen) {
    const char *pct = strchr(pat, '%');
    if (!pct) {
        if (strcmp(pat, word)) return 0;
        if (stem) *stem = word;
        if (slen) *slen = 0;
        return 1;
    }
    int pre = (int)(pct - pat), post = (int)strlen(pct + 1), n = (int)strlen(word);
    if (n < pre + post) return 0;
    if (pre && memcmp(word, pat, (size_t)pre)) return 0;
    if (post && strcmp(word + (n - post), pct + 1)) return 0;
    if (stem) *stem = word + pre;
    if (slen) *slen = n - pre - post;
    return 1;
}
static char *patsubst_all(const char *pat, const char *repl, const char *text) {
    char **w; int n; w = split_words(text, &n);
    struct buf b; buf_init(&b);
    int has = strchr(pat, '%') != NULL;
    for (int i = 0; i < n; i++) {
        const char *stem = NULL; int sl = 0;
        if (i) buf_putc(&b, ' ');
        if (percent_match(pat, w[i], &stem, &sl)) {
            if (!has) buf_puts(&b, repl);
            else for (const char *r = repl; *r; r++) {
                if (*r == '%') buf_add(&b, stem, sl);
                else buf_putc(&b, *r);
            }
        } else buf_puts(&b, w[i]);
    }
    free_words(w, n);
    return b.s;
}
static char *subst_all(const char *from, const char *to, const char *text) {
    struct buf b; buf_init(&b);
    if (!text) text = "";
    if (!from || !from[0]) { buf_puts(&b, text); return b.s; }
    int fl = (int)strlen(from);
    for (const char *p = text; *p; ) {
        if (!strncmp(p, from, (size_t)fl)) { buf_puts(&b, to ? to : ""); p += fl; }
        else buf_putc(&b, *p++);
    }
    return b.s;
}
static char **split_top(const char *s, int *out, int maxn) {
    char **a = NULL; int n = 0, cap = 0;
    const char *start = s; int depth = 0;
    for (const char *p = s; ; ) {
        int split = 0;
        if (!*p) split = 1;
        else if (*p == '(' || *p == '{') depth++;
        else if ((*p == ')' || *p == '}') && depth) depth--;
        else if (*p == ',' && depth == 0 && !(maxn && n >= maxn - 1)) split = 1;
        if (!split) { p++; continue; }
        GROW(a, n, cap);
        a[n++] = xstrndup(start, (int)(p - start));
        if (!*p) break;
        p++;
        start = p;
    }
    *out = n;
    return a;
}
static void free_args(char **a, int n) { free_words(a, n); }

static int file_info(const char *path, unsigned *mtime);
static char *shell_out(const char *cmd);
static int parse_text(const char *text, const char *name);

static char *map_words(const char *text, char *(*fn)(const char *, void *), void *ctx) {
    char **w; int n; w = split_words(text, &n);
    struct buf b; buf_init(&b);
    for (int i = 0; i < n; i++) {
        if (i) buf_putc(&b, ' ');
        char *one = fn(w[i], ctx);
        buf_puts(&b, one);
        free(one);
    }
    free_words(w, n);
    return b.s;
}
static char *word_dir(const char *w, void *c) {
    (void)c; const char *sl = strrchr(w, '/');
    if (!sl) return xstrdup("./");
    return xstrndup(w, (int)(sl - w) + 1);
}
static char *word_notdir(const char *w, void *c) {
    (void)c; const char *sl = strrchr(w, '/'); return xstrdup(sl ? sl + 1 : w);
}
static char *word_suffix(const char *w, void *c) {
    (void)c; const char *d = strrchr(w, '.'); return xstrdup(d ? d : "");
}
static char *word_base(const char *w, void *c) {
    (void)c; const char *d = strrchr(w, '.');
    if (!d) return xstrdup(w);
    return xstrndup(w, (int)(d - w));
}
struct affix { const char *fix; int pre; };
static char *word_affix(const char *w, void *c) {
    struct affix *a = c; struct buf b; buf_init(&b);
    if (a->pre) { buf_puts(&b, a->fix); buf_puts(&b, w); }
    else { buf_puts(&b, w); buf_puts(&b, a->fix); }
    return b.s;
}
static char *filter_words(const char *pats, const char *text, int drop) {
    char **ps, **ws; int np, nw;
    ps = split_words(pats, &np); ws = split_words(text, &nw);
    struct buf b; buf_init(&b); int first = 1;
    for (int i = 0; i < nw; i++) {
        int hit = 0;
        for (int j = 0; j < np; j++) if (percent_match(ps[j], ws[i], NULL, NULL)) { hit = 1; break; }
        if (hit == drop) continue;
        if (!first) buf_putc(&b, ' ');
        first = 0; buf_puts(&b, ws[i]);
    }
    free_words(ps, np); free_words(ws, nw);
    return b.s;
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char *sort_words(const char *text) {
    char **w; int n; w = split_words(text, &n);
    if (n) qsort(w, (size_t)n, sizeof *w, cmpstr);
    struct buf b; buf_init(&b);
    for (int i = 0; i < n; i++) {
        if (i && !strcmp(w[i], w[i - 1])) continue;
        if (b.n) buf_putc(&b, ' ');
        buf_puts(&b, w[i]);
    }
    free_words(w, n);
    return b.s;
}
static char *strip_words(const char *text) {
    struct buf b; buf_init(&b); int sp = 0, any = 0;
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n') { sp = any; continue; }
        if (sp) buf_putc(&b, ' ');
        buf_putc(&b, *p); any = 1; sp = 0;
    }
    return b.s;
}
static char *nth_word(const char *text, int index) {
    char **w; int n; w = split_words(text, &n);
    char *r = (index >= 1 && index <= n) ? xstrdup(w[index - 1]) : xstrdup("");
    free_words(w, n); return r;
}
static char *word_range(const char *text, int from, int to) {
    char **w; int n; w = split_words(text, &n);
    struct buf b; buf_init(&b);
    if (from < 1) from = 1;
    for (int i = from; i <= to && i <= n; i++) {
        if (b.n) buf_putc(&b, ' ');
        buf_puts(&b, w[i - 1]);
    }
    free_words(w, n); return b.s;
}
static char *join_lists(const char *a, const char *b) {
    char **wa, **wb; int na, nb;
    wa = split_words(a, &na); wb = split_words(b, &nb);
    int m = na > nb ? na : nb;
    struct buf o; buf_init(&o);
    for (int i = 0; i < m; i++) {
        if (i) buf_putc(&o, ' ');
        if (i < na) buf_puts(&o, wa[i]);
        if (i < nb) buf_puts(&o, wb[i]);
    }
    free_words(wa, na); free_words(wb, nb);
    return o.s;
}
static int glob_match(const char *pat, const char *s) {
    while (*pat == '*') {
        pat++;
        if (!*pat) return 1;
        for (; *s; s++) if (glob_match(pat, s)) return 1;
        return 0;
    }
    if (!*pat) return !*s;
    if (!*s || (*pat != '?' && *pat != *s)) return 0;
    return glob_match(pat + 1, s + 1);
}
static int has_meta(const char *s) { return strpbrk(s, "*?[") != NULL; }
static void glob_one(struct buf *out, const char *pat) {
    const char *sl = strrchr(pat, '/');
    char *dir, *basepat; const char *prefix;
    if (sl) {
        dir = sl == pat ? xstrdup("/") : xstrndup(pat, (int)(sl - pat));
        basepat = xstrdup(sl + 1);
        prefix = pat;
    } else { dir = xstrdup("."); basepat = xstrdup(pat); prefix = NULL; }
    if (!has_meta(basepat) && !has_meta(dir)) {
        if (file_info(pat, NULL)) {
            if (out->n) buf_putc(out, ' ');
            buf_puts(out, pat);
        }
        free(dir); free(basepat); return;
    }
    DIR *d = opendir(dir);
    if (!d) { free(dir); free(basepat); return; }
    struct dirent *de;
    int dot = basepat[0] == '.';
    while ((de = readdir(d))) {
        if (!dot && de->d_name[0] == '.') continue;
        if (!glob_match(basepat, de->d_name)) continue;
        if (out->n) buf_putc(out, ' ');
        if (prefix) {
            buf_add(out, pat, (int)(sl - pat) + 1);
            buf_puts(out, de->d_name);
        } else buf_puts(out, de->d_name);
    }
    closedir(d);
    free(dir); free(basepat);
}
static char *wildcard_of(const char *text) {
    char **w; int n; w = split_words(text, &n);
    struct buf b; buf_init(&b);
    for (int i = 0; i < n; i++) glob_one(&b, w[i]);
    free_words(w, n);
    return b.s;
}

static int atoi_word(const char *s) {
    int n = 0; if (!s || !*s) return 0;
    for (; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
    return n;
}

static char *fn_if(char **raw, int n) {
    char *c = expand(raw[0]);
    int yes = c && c[0];
    free(c);
    if (yes) return n > 1 ? expand(raw[1]) : xstrdup("");
    return n > 2 ? expand(raw[2]) : xstrdup("");
}
static char *fn_or(char **raw, int n) {
    for (int i = 0; i < n; i++) {
        char *e = expand(raw[i]);
        if (e && e[0]) return e;
        free(e);
    }
    return xstrdup("");
}
static char *fn_and(char **raw, int n) {
    char *last = xstrdup("");
    for (int i = 0; i < n; i++) {
        char *e = expand(raw[i]);
        if (!e || !e[0]) { free(e); free(last); return xstrdup(""); }
        free(last); last = e;
    }
    return last;
}
static char *fn_foreach(char **raw, int n) {
    if (n < 3) return xstrdup("");
    char *once = expand(raw[0]);
    if (!once) return NULL;
    char *vn = trim_dup(once);
    free(once);
    char *list = expand(raw[1]);
    if (!list) { free(vn); return NULL; }
    struct var *old = var_find(vn);
    int existed = old != NULL;
    char *oval = existed ? xstrdup(old->val) : NULL;
    int os = existed ? old->simple : 0, oe = existed ? old->exported : 0, ol = existed ? old->locked : 0;
    char **w; int nw; w = split_words(list, &nw);
    struct buf b; buf_init(&b);
    for (int i = 0; i < nw; i++) {
        var_set(vn, w[i], 1, 0, 0, 1);
        char *e = expand(raw[2]);
        if (!e) { free_words(w, nw); free(list); free(vn); free(oval); free(b.s); return NULL; }
        if (b.n && e[0]) buf_putc(&b, ' ');
        buf_puts(&b, e);
        free(e);
    }
    if (existed) {
        var_set(vn, oval, os, oe, ol, 1);
        struct var *back = var_find(vn);
        if (back) { back->locked = ol; back->exported = oe; }
    } else var_remove(vn);
    free(oval); free(vn); free(list); free_words(w, nw);
    return b.s;
}
static char *numname(int i, char *tmp) { sprintf(tmp, "%d", i); return tmp; }
static char *fn_call(char **raw, int n) {
    if (n < 1) return xstrdup("");
    char *vn0 = expand(raw[0]); if (!vn0) return NULL;
    char *vn = trim_dup(vn0); free(vn0);
    struct var *v = var_find(vn);
    if (!v) { free(vn); return xstrdup(""); }
    int simple = v->simple; char *body = xstrdup(v->val);
    enum { N = 16 };
    int m = n - 1; if (m > N) m = N;
    int ex[N]; char *ov[N]; int os[N], oe[N], ol[N];
    char nm[N][8];
    for (int i = 0; i < m; i++) {
        numname(i + 1, nm[i]);
        struct var *a = var_find(nm[i]);
        ex[i] = a != NULL;
        ov[i] = ex[i] ? xstrdup(a->val) : NULL;
        os[i] = ex[i] ? a->simple : 0; oe[i] = ex[i] ? a->exported : 0; ol[i] = ex[i] ? a->locked : 0;
        char *ev = expand(raw[i + 1]);
        var_set(nm[i], ev ? ev : "", 1, 0, 0, 1);
        free(ev);
    }
    char *r = simple ? xstrdup(body) : expand(body);
    for (int i = 0; i < m; i++) {
        if (ex[i]) {
            var_set(nm[i], ov[i], os[i], oe[i], ol[i], 1);
            struct var *a = var_find(nm[i]); if (a) { a->locked = ol[i]; a->exported = oe[i]; }
        } else var_remove(nm[i]);
        free(ov[i]);
    }
    free(body); free(vn);
    return r;
}
static int is_func(const char *n) {
    static const char *t[] = {
        "subst", "patsubst", "filter", "filter-out", "sort", "strip", "word", "words",
        "wordlist", "firstword", "dir", "notdir", "suffix", "basename", "addprefix",
        "addsuffix", "findstring", "if", "or", "and", "wildcard", "shell", "foreach",
        "error", "warning", "join", "call", "eval", NULL
    };
    for (int i = 0; t[i]; i++) if (!strcmp(n, t[i])) return 1;
    return 0;
}
static int comma_max(const char *name) {
    if (!strcmp(name, "subst") || !strcmp(name, "patsubst") || !strcmp(name, "if")
        || !strcmp(name, "wordlist") || !strcmp(name, "foreach")) return 3;
    if (!strcmp(name, "filter") || !strcmp(name, "filter-out") || !strcmp(name, "word")
        || !strcmp(name, "findstring") || !strcmp(name, "addprefix") || !strcmp(name, "addsuffix")
        || !strcmp(name, "join")) return 2;
    if (!strcmp(name, "call") || !strcmp(name, "or") || !strcmp(name, "and")) return 0;
    return 1;
}
static char *dispatch(const char *name, const char *rest) {
    int n = 0, maxn = comma_max(name);
    char **raw = split_top(rest, &n, maxn);
    char *r = NULL;
    if (!strcmp(name, "if")) r = fn_if(raw, n);
    else if (!strcmp(name, "or")) r = fn_or(raw, n);
    else if (!strcmp(name, "and")) r = fn_and(raw, n);
    else if (!strcmp(name, "foreach")) r = fn_foreach(raw, n);
    else if (!strcmp(name, "call")) r = fn_call(raw, n);
    else if (!strcmp(name, "eval")) {
        char *t = n ? expand(raw[0]) : xstrdup("");
        if (!t || parse_text(t, "eval") < 0) r = NULL;
        else r = xstrdup("");
        free(t);
    } else if (!strcmp(name, "shell")) {
        char *c = n ? expand(raw[0]) : xstrdup("");
        r = c ? shell_out(c) : NULL;
        free(c);
    } else if (!strcmp(name, "error") || !strcmp(name, "warning")) {
        char *m = n ? expand(raw[0]) : xstrdup("");
        fprintf(stderr, "make: %s%s\n", !strcmp(name, "error") ? "*** " : "", m ? m : "");
        if (!strcmp(name, "error")) { free(m); free_args(raw, n); exit(2); }
        free(m); r = xstrdup("");
    } else {
        char *ev[8]; int en = n > 8 ? 8 : n;
        for (int i = 0; i < en; i++) {
            ev[i] = expand(raw[i]);
            if (!ev[i]) { for (int j = 0; j < i; j++) free(ev[j]); free_args(raw, n); return NULL; }
        }
        const char *a0 = en > 0 ? ev[0] : "", *a1 = en > 1 ? ev[1] : "", *a2 = en > 2 ? ev[2] : "";
        if (!strcmp(name, "subst")) r = subst_all(a0, a1, a2);
        else if (!strcmp(name, "patsubst")) r = patsubst_all(a0, a1, a2);
        else if (!strcmp(name, "filter")) r = filter_words(a0, a1, 0);
        else if (!strcmp(name, "filter-out")) r = filter_words(a0, a1, 1);
        else if (!strcmp(name, "findstring")) r = strstr(a1, a0) ? xstrdup(a0) : xstrdup("");
        else if (!strcmp(name, "sort")) r = sort_words(a0);
        else if (!strcmp(name, "strip")) r = strip_words(a0);
        else if (!strcmp(name, "word")) r = nth_word(a1, atoi_word(a0));
        else if (!strcmp(name, "words")) {
            char **w; int k; w = split_words(a0, &k);
            char tmp[16]; sprintf(tmp, "%d", k); r = xstrdup(tmp); free_words(w, k);
        } else if (!strcmp(name, "wordlist")) r = word_range(a2, atoi_word(a0), atoi_word(a1));
        else if (!strcmp(name, "firstword")) r = nth_word(a0, 1);
        else if (!strcmp(name, "dir")) r = map_words(a0, word_dir, NULL);
        else if (!strcmp(name, "notdir")) r = map_words(a0, word_notdir, NULL);
        else if (!strcmp(name, "suffix")) r = map_words(a0, word_suffix, NULL);
        else if (!strcmp(name, "basename")) r = map_words(a0, word_base, NULL);
        else if (!strcmp(name, "addprefix") || !strcmp(name, "addsuffix")) {
            struct affix af = { a0, !strcmp(name, "addprefix") };
            r = map_words(a1, word_affix, &af);
        } else if (!strcmp(name, "wildcard")) r = wildcard_of(a0);
        else if (!strcmp(name, "join")) r = join_lists(a0, a1);
        else r = xstrdup("");
        for (int i = 0; i < en; i++) free(ev[i]);
    }
    free_args(raw, n);
    return r;
}
static int subst_ref_at(const char *s, int *colon) {
    int depth = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == ' ' || s[i] == '\t') return 0;
        if (s[i] == '(' || s[i] == '{') depth++;
        else if ((s[i] == ')' || s[i] == '}') && depth) depth--;
        else if (s[i] == ':' && depth == 0) {
            for (int j = i + 1; s[j]; j++) {
                if (s[j] == '(' || s[j] == '{') depth++;
                else if ((s[j] == ')' || s[j] == '}') && depth) depth--;
                else if (s[j] == '=' && depth == 0) { *colon = i; return 1; }
            }
            return 0;
        }
    }
    return 0;
}
static char *expand_inside(const char *inside) {
    const char *s = inside;
    while (*s == ' ' || *s == '\t') s++;
    const char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != ',' && *e != ':' && *e != '=') e++;
    char *word = xstrndup(s, (int)(e - s));
    if (is_func(word) && (*e == 0 || *e == ' ' || *e == '\t' || *e == ',')) {
        const char *rest = e;
        while (*rest == ' ' || *rest == '\t') rest++;
        char *r = dispatch(word, rest);
        free(word);
        return r;
    }
    free(word);
    int colon = 0;
    if (subst_ref_at(s, &colon)) {
        char *name = xstrndup(s, colon);
        const char *eq = strchr(s + colon + 1, '=');
        /* eq at depth 0: subst_ref_at already found one. A later '=' inside
         * a nested ref is unlikely in a substitution pattern. */
        char *pat = xstrndup(s + colon + 1, (int)(eq - (s + colon + 1)));
        char *repl = xstrdup(eq + 1);
        char *val = expand_name(name);
        char *r;
        if (!strchr(pat, '%')) {
            char *p2 = xmalloc(strlen(pat) + 2), *r2 = xmalloc(strlen(repl) + 2);
            p2[0] = '%'; memcpy(p2 + 1, pat, strlen(pat) + 1);
            r2[0] = '%'; memcpy(r2 + 1, repl, strlen(repl) + 1);
            r = val ? patsubst_all(p2, r2, val) : NULL;
            free(p2); free(r2);
        } else r = val ? patsubst_all(pat, repl, val) : NULL;
        free(name); free(pat); free(repl); free(val);
        return r;
    }
    return expand_name(inside);
}
static char *expand_ref(const char **pp) {
    const char *s = *pp;
    if (s[1] == '$') { *pp = s + 2; return xstrdup("$"); }
    if (s[1] != '(' && s[1] != '{') {
        char name[2] = { s[1] ? s[1] : 0, 0 };
        if (!s[1]) { *pp = s + 1; return xstrdup("$"); }
        *pp = s + 2;
        return expand_name(name);
    }
    char open = s[1], close = open == '(' ? ')' : '}';
    int d = 1; const char *p = s + 2;
    while (*p && d) {
        if (*p == open) d++;
        else if (*p == close) d--;
        if (d) p++;
    }
    if (!*p || *p != close) { note("unterminated variable reference"); return NULL; }
    char *inside = xstrndup(s + 2, (int)(p - (s + 2)));
    *pp = p + 1;
    char *r = expand_inside(inside);
    free(inside);
    return r;
}
static char *expand_body(const char *s) {
    struct buf b; buf_init(&b);
    for (const char *p = s; *p; ) {
        if (*p == '$') {
            char *part = expand_ref(&p);
            if (!part) { free(b.s); return NULL; }
            buf_puts(&b, part); free(part);
        } else buf_putc(&b, *p++);
    }
    return b.s;
}
static char *expand(const char *s) {
    if (!s) return xstrdup("");
    if (++exp_depth > 256) {
        fprintf(stderr, "make: variable expansion recursed too deeply\n");
        exp_depth--;
        return NULL;
    }
    char *r = expand_body(s);
    exp_depth--;
    return r;
}

/* ---------- rules ---------- */

struct prechunk { char **pre; int npre; char **ord; int nord; int primary; };
struct rule {
    char *target, *stem;
    struct prechunk *ch; int nch, chcap;
    char **rec; int nrec, reccap;
    int pattern, dcolon;
};
static struct rule **rules; static int nrules, rulecap;
static struct rule **batchv; static int nbatch, batchcap;
static int rule_open;
static char *default_goal;
static char **phonies; static int nphony, phonycap;
static int delete_on_error;

static struct rule *new_rule(const char *target) {
    struct rule *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->target = xstrdup(target);
    return r;
}
static void push_rule(struct rule *r) { GROW(rules, nrules, rulecap); rules[nrules++] = r; }
static int is_phony(const char *name) {
    for (int i = 0; i < nphony; i++) if (!strcmp(phonies[i], name)) return 1;
    return 0;
}
static void phony_add(const char *name) {
    if (is_phony(name)) return;
    GROW(phonies, nphony, phonycap);
    phonies[nphony++] = xstrdup(name);
}
static struct rule *find_single(const char *target) {
    for (int i = 0; i < nrules; i++)
        if (!rules[i]->pattern && !rules[i]->dcolon && !strcmp(rules[i]->target, target))
            return rules[i];
    return NULL;
}
static void add_chunk(struct rule *r, char **pre, int npre, char **ord, int nord) {
    GROW(r->ch, r->nch, r->chcap);
    struct prechunk *c = &r->ch[r->nch++];
    c->pre = pre; c->npre = npre; c->ord = ord; c->nord = nord; c->primary = 0;
}
static void batch_add(struct rule *r) {
    for (int i = 0; i < nbatch; i++) if (batchv[i] == r) return;
    GROW(batchv, nbatch, batchcap);
    batchv[nbatch++] = r;
}
static void remember_goal(const char *target) {
    if (!default_goal && target[0] != '.' && !strchr(target, '%')) default_goal = xstrdup(target);
}
static int match_pat(const char *pat, const char *name, char **stem) {
    const char *pct = strchr(pat, '%');
    if (!pct) {
        if (strcmp(pat, name)) return 0;
        if (stem) *stem = xstrdup("");
        return 1;
    }
    int pre = (int)(pct - pat), post = (int)strlen(pct + 1), n = (int)strlen(name);
    if (n < pre + post) return 0;
    if (pre && memcmp(name, pat, (size_t)pre)) return 0;
    if (post && strcmp(name + (n - post), pct + 1)) return 0;
    if (stem) *stem = xstrndup(name + pre, n - pre - post);
    return 1;
}
static char *subst_stem(const char *pat, const char *stem) {
    struct buf b; buf_init(&b);
    for (; *pat; pat++) {
        if (*pat == '%') buf_puts(&b, stem ? stem : "");
        else buf_putc(&b, *pat);
    }
    return b.s;
}

/* ---------- files, shell ---------- */

static int dry_run, silent, jobs = 1, any_ran, syncing;
static char **makefiles;
static int nmk, mkcap;
static void note_makefile(const char *path) {
    if (!path || !path[0]) return;
    for (int i = 0; i < nmk; i++) if (!strcmp(makefiles[i], path)) return;
    if (nmk == mkcap) {
        mkcap = mkcap ? mkcap * 2 : 8;
        makefiles = xrealloc(makefiles, (size_t)mkcap * sizeof *makefiles);
    }
    makefiles[nmk++] = xstrdup(path);
}
static int is_makefile(const char *name) {
    for (int i = 0; i < nmk; i++) if (!strcmp(makefiles[i], name)) return 1;
    return 0;
}
static int file_info(const char *path, unsigned *mtime) {
    struct stat st;
    if (!path || stat(path, &st) < 0) { if (mtime) *mtime = 0; return 0; }
    if (mtime) *mtime = (unsigned)st.st_mtime;
    return 1;
}
static char *shell_path(void) {
    struct var *v = var_find("SHELL");
    char *s = NULL;
    if (v) s = v->simple ? xstrdup(v->val) : expand(v->val);
    if (!s || !s[0]) { free(s); return xstrdup("/bin/sh"); }
    return s;
}
static void sync_env(void) {
    if (syncing) return;
    syncing = 1;
    for (int i = 0; i < nvars; i++) if (vars[i].exported) {
        char *v = vars[i].simple ? xstrdup(vars[i].val) : expand(vars[i].val);
        if (v) setenv(vars[i].name, v, 1);
        free(v);
    }
    syncing = 0;
}
static char *shell_out(const char *cmd) {
    int p[2];
    if (pipe(p) < 0) return xstrdup("");
    sync_env();
    char *sh = shell_path();
    int pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); free(sh); return xstrdup(""); }
    if (!pid) {
        dup2(p[1], 1); close(p[0]); close(p[1]);
        char *av[4] = { "sh", "-c", (char *)cmd, NULL };
        exec(sh, av); exit(127);
    }
    close(p[1]); free(sh);
    struct buf b; buf_init(&b); char tmp[512]; int n;
    while ((n = (int)read(p[0], tmp, sizeof tmp)) > 0) buf_add(&b, tmp, n);
    close(p[0]);
    int st = 0; while (waitpid(pid, &st, 0) == -EINTR) {}
    for (int i = 0; i < b.n; i++) if (b.s[i] == '\n' || b.s[i] == '\r') b.s[i] = ' ';
    while (b.n && b.s[b.n - 1] == ' ') b.s[--b.n] = 0;
    return b.s;
}
static int run_sh(const char *cmd) {
    sync_env();
    char *sh = shell_path();
    int pid = fork();
    if (pid < 0) { free(sh); return -1; }
    if (!pid) {
        char *av[4] = { "sh", "-c", (char *)cmd, NULL };
        exec(sh, av); exit(127);
    }
    free(sh);
    int st = 0, r; do { r = waitpid(pid, &st, 0); } while (r == -EINTR);
    return r < 0 ? -1 : st;
}

/* ---------- parser ---------- */

struct tspec { char *target, *name, *val; int simple, append; };
static struct tspec *tspecs; static int ntspec, tspeccap;
static struct frame { int parent, taken, else_done; } conds[64];
static int cond_sp, defining, def_simple, def_export, def_force, def_skip, parse_depth;
static char *def_name; static struct buf def_body;

static int skipping(void) { return cond_sp > 0 && !(conds[cond_sp - 1].parent && conds[cond_sp - 1].taken); }
static int key(const char *s, const char *k) {
    int n = (int)strlen(k);
    if (strncmp(s, k, (size_t)n)) return 0;
    return s[n] == 0 || s[n] == ' ' || s[n] == '\t' || s[n] == '(';
}
static const char *skip_key(const char *s) {
    while (*s && *s != ' ' && *s != '\t') s++;
    while (*s == ' ' || *s == '\t') s++;
    return s;
}
static void strip_comment(char *s) {
    int depth = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) {
            if (s[i + 1] == '#') memmove(s + i, s + i + 1, strlen(s + i + 1) + 1);
            else i++;
            continue;
        }
        if (s[i] == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) { depth++; continue; }
        if ((s[i] == '(' || s[i] == '{') && depth) depth++;
        else if ((s[i] == ')' || s[i] == '}') && depth) depth--;
        else if (s[i] == '#' && !depth) { s[i] = 0; return; }
    }
}
/* rule=1 finds a rule colon (`:` / `::`, not `:=`). rule=0 finds an assignment. */
static int find_op(const char *s, int *at, int *len, int rule) {
    for (int i = 0; s[i]; i++) {
        if (s[i] == '$' && (s[i + 1] == '(' || s[i + 1] == '{')) {
            char open = s[i + 1], close = open == '(' ? ')' : '}';
            int d = 1; i += 2;
            while (s[i] && d) { if (s[i] == open) d++; else if (s[i] == close) d--; i++; }
            i--;
            continue;
        }
        if (rule) {
            if (s[i] == ':' && s[i + 1] != '=') { *at = i; *len = s[i + 1] == ':' ? 2 : 1; return 1; }
        } else if (s[i] == '=') { *at = i; *len = 1; return 1; }
        else if ((s[i] == ':' || s[i] == '+' || s[i] == '?') && s[i + 1] == '=') {
            *at = i; *len = 2; return 1;
        }
    }
    return 0;
}
static char **glob_expand_words(char **in, int n, int *outn) {
    char **a = NULL; int c = 0, cap = 0;
    for (int i = 0; i < n; i++) {
        if (!has_meta(in[i])) {
            GROW(a, c, cap);
            a[c++] = xstrdup(in[i]);
            continue;
        }
        struct buf b; buf_init(&b);
        glob_one(&b, in[i]);
        char **g = NULL; int ng = 0;
        if (b.n) g = split_words(b.s, &ng);
        free(b.s);
        if (ng > 1) qsort(g, (size_t)ng, sizeof *g, cmpstr);
        if (!ng) {
            GROW(a, c, cap);
            a[c++] = xstrdup(in[i]);
        } else for (int j = 0; j < ng; j++) {
            GROW(a, c, cap);
            a[c++] = g[j];
            g[j] = NULL;
        }
        free(g);
    }
    *outn = c;
    return a;
}
static void split_order(char *exp, char ***pre, int *npre, char ***ord, int *nord) {
    char *bar = NULL; int depth = 0;
    for (char *p = exp; *p; p++) {
        if (*p == '(' || *p == '{') depth++;
        else if ((*p == ')' || *p == '}') && depth) depth--;
        else if (*p == '|' && !depth) { bar = p; break; }
    }
    char **rawp, **rawo; int np, no;
    if (bar) { *bar = 0; rawo = split_words(bar + 1, &no); }
    else { rawo = NULL; no = 0; }
    rawp = split_words(exp, &np);
    *pre = glob_expand_words(rawp, np, npre);
    *ord = glob_expand_words(rawo, no, nord);
    free_words(rawp, np);
    free_words(rawo, no);
}
static int special_target(const char *name, char **pre, int npre) {
    if (!strcmp(name, ".PHONY")) { for (int i = 0; i < npre; i++) phony_add(pre[i]); return 1; }
    if (!strcmp(name, ".DELETE_ON_ERROR")) { delete_on_error = 1; return 1; }
    if (!strcmp(name, ".SUFFIXES") || !strcmp(name, ".PRECIOUS") || !strcmp(name, ".SECONDARY")
        || !strcmp(name, ".IGNORE") || !strcmp(name, ".DEFAULT") || !strcmp(name, ".NOTPARALLEL"))
        return 1;
    return 0;
}
static void tspec_add(const char *target, const char *name, const char *val, int simple, int append) {
    GROW(tspecs, ntspec, tspeccap);
    struct tspec *t = &tspecs[ntspec++];
    t->target = xstrdup(target); t->name = xstrdup(name); t->val = xstrdup(val ? val : "");
    t->simple = simple; t->append = append;
}
static int do_assign(char *s, int force, int exported) {
    int at, len;
    if (!find_op(s, &at, &len, 0)) { note("assignment expected"); return -1; }
    char kind = s[at];
    s[at] = 0;
    char *name = trim_dup(s);
    char *val = trim_dup(s + at + len);
    if (!name[0]) { note("empty variable name"); free(name); free(val); return -1; }
    if (kind == '?') { if (!var_find(name)) var_set(name, val, 0, exported, 0, force); }
    else if (kind == '+') var_append(name, val, force);
    else if (kind == ':') {
        char *e = expand(val);
        if (!e) { free(name); free(val); return -1; }
        var_set(name, e, 1, exported, 0, force);
        free(e);
    } else var_set(name, val, 0, exported, 0, force);
    if (exported) { struct var *v = var_find(name); if (v) v->exported = 1; }
    free(name); free(val);
    return 0;
}
static int start_define(const char *rest, int force, int exported) {
    char *line = xstrdup(rest);
    int at, len, simple = 0;
    if (find_op(line, &at, &len, 0) && !strchr(trim_dup(line), ' ')) {
        simple = line[at] == ':';
        line[at] = 0;
    }
    def_name = trim_dup(line);
    free(line);
    if (!def_name[0]) { note("empty define name"); return -1; }
    def_simple = simple; def_force = force; def_export = exported; defining = 1;
    if (!def_body.s) buf_init(&def_body);
    def_body.n = 0; if (def_body.s) def_body.s[0] = 0;
    return 0;
}
static int finish_define(void) {
    int rc = 0;
    if (def_simple) {
        char *e = expand(def_body.s ? def_body.s : "");
        if (!e) rc = -1;
        else { var_set(def_name, e, 1, def_export, 0, def_force); free(e); }
    } else var_set(def_name, def_body.s ? def_body.s : "", 0, def_export, 0, def_force);
    free(def_name); def_name = NULL; defining = 0;
    if (def_body.s) { def_body.n = 0; def_body.s[0] = 0; }
    return rc;
}
static int cond_push(int parent_active, int taken) {
    if (cond_sp >= 64) { note("conditionals nested too deeply"); return -1; }
    conds[cond_sp].parent = parent_active;
    conds[cond_sp].taken = taken;
    conds[cond_sp].else_done = 0;
    cond_sp++;
    return 0;
}
static int handle_cond(char *s) {
    int parent = cond_sp == 0 || (conds[cond_sp - 1].parent && conds[cond_sp - 1].taken);
    if (key(s, "endif")) {
        if (!cond_sp) { note("endif without if"); return -1; }
        cond_sp--; return 0;
    }
    if (key(s, "else")) {
        if (!cond_sp || conds[cond_sp - 1].else_done) { note("else without if"); return -1; }
        conds[cond_sp - 1].else_done = 1;
        conds[cond_sp - 1].taken = !conds[cond_sp - 1].taken;
        return 0;
    }
    if (!parent) return cond_push(0, 0);
    if (key(s, "ifdef") || key(s, "ifndef")) {
        int neg = key(s, "ifndef");
        char *name = trim_dup(skip_key(s));
        char *en = expand(name); free(name);
        if (!en) return -1;
        char *tn = trim_dup(en); free(en);
        struct var *v = var_find(tn);
        /* GNU: ifdef смотрит на текст значения и не раскрывает ссылки. Пустое значение — ложь. */
        int on = v && v->val && v->val[0];
        free(tn);
        return cond_push(1, neg ? !on : on);
    }
    int neq = key(s, "ifneq");
    const char *rest = skip_key(s);
    char *lhs = NULL, *rhs = NULL;
    if (*rest == '(') {
        int depth = 1; const char *p = rest + 1, *comma = NULL, *end = NULL;
        for (; *p; p++) {
            if (*p == '(') depth++;
            else if (*p == ')') { if (--depth == 0) { end = p; break; } }
            else if (*p == ',' && depth == 1 && !comma) comma = p;
        }
        if (!comma || !end) { note("malformed ifeq"); return -1; }
        lhs = xstrndup(rest + 1, (int)(comma - (rest + 1)));
        rhs = xstrndup(comma + 1, (int)(end - (comma + 1)));
    } else if (*rest == '"' || *rest == '\'') {
        char q = *rest; const char *a = rest + 1;
        const char *ae = strchr(a, q); if (!ae) { note("malformed ifeq"); return -1; }
        const char *b = ae + 1; while (*b == ' ' || *b == '\t') b++;
        if (*b != '"' && *b != '\'') { note("malformed ifeq"); return -1; }
        q = *b; b++;
        const char *be = strchr(b, q); if (!be) { note("malformed ifeq"); return -1; }
        lhs = xstrndup(a, (int)(ae - a)); rhs = xstrndup(b, (int)(be - b));
    } else { note("malformed ifeq"); return -1; }
    char *le = expand(lhs), *re = expand(rhs);
    free(lhs); free(rhs);
    if (!le || !re) { free(le); free(re); return -1; }
    int eq = !strcmp(le, re); free(le); free(re);
    return cond_push(1, neq ? !eq : eq);
}
static int is_cond(const char *s) {
    return key(s, "ifeq") || key(s, "ifneq") || key(s, "ifdef") || key(s, "ifndef")
        || key(s, "else") || key(s, "endif");
}
static char *dirname_dup(const char *path) {
    const char *sl = strrchr(path, '/');
    if (!sl) return xstrdup(".");
    if (sl == path) return xstrdup("/");
    return xstrndup(path, (int)(sl - path));
}
static char *join_path(const char *dir, const char *name) {
    struct buf b; buf_init(&b);
    buf_puts(&b, dir);
    if (dir[0] && dir[strlen(dir) - 1] != '/') buf_putc(&b, '/');
    buf_puts(&b, name);
    return b.s;
}
static int slurp(const char *path, char **out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    struct buf b; buf_init(&b); char tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(&b, tmp, (int)n);
    fclose(f); *out = b.s; return 0;
}
static int load_file(const char *path, int optional);
static int parse_lines(const char *text);
static int add_rule_line(char *s, int force);
static int handle_body(char *s, int force) {
    if (key(s, "define")) return start_define(skip_key(s), force, 0);
    if (key(s, "include") || key(s, "-include") || key(s, "sinclude")) {
        int opt = s[0] == '-' || s[0] == 's';
        char *exp = expand(skip_key(s));
        if (!exp) return -1;
        char **w; int n; w = split_words(exp, &n);
        int rc = 0;
        for (int i = 0; i < n && !rc; i++) rc = load_file(w[i], opt);
        free_words(w, n); free(exp);
        return rc;
    }
    if (key(s, "export") || key(s, "unexport")) {
        int un = s[0] == 'u';
        const char *rest = skip_key(s);
        int at = 0, len = 0;
        if (!un && find_op(rest, &at, &len, 0)) return do_assign((char *)rest, force, 1);
        if (!*rest) return 0;
        char **w; int n; w = split_words(rest, &n);
        for (int i = 0; i < n; i++) {
            struct var *v = var_find(w[i]);
            if (un) { if (v) v->exported = 0; }
            else if (v) v->exported = 1;
        }
        free_words(w, n);
        return 0;
    }
    if (key(s, "vpath")) return 0;
    if (key(s, "override")) return handle_body((char *)skip_key(s), 1);
    int ca, cl, aa, al;
    int has_c = find_op(s, &ca, &cl, 1);
    int has_a = find_op(s, &aa, &al, 0);
    if (has_c && (!has_a || ca < aa)) return add_rule_line(s, force);
    if (has_a) return do_assign(s, force, 0);
    if (s[0] == '$') {
        char *e = expand(s);
        if (!e) return -1;
        if (e[0]) { note("missing separator"); fprintf(stderr, "make: leftover: %.120s\n", e); free(e); return -1; }
        free(e); return 0;
    }
    note("missing separator");
    fprintf(stderr, "make: %.120s\n", s);
    return -1;
}
static int add_rule_line(char *s, int force) {
    (void)force;
    int at, len;
    if (!find_op(s, &at, &len, 1)) return -1;
    int dcolon = len == 2;
    char *line = xstrdup(s);
    line[at] = 0;
    char *rhs = line + at + len;
    int ra, rl;
    if (!dcolon && find_op(rhs, &ra, &rl, 0)) {
        char saved = rhs[ra]; rhs[ra] = 0;
        char *tname = trim_dup(rhs);
        rhs[ra] = saved;
        if (tname[0] && !strchr(tname, ' ') && !strchr(tname, ':')) {
            char *val = trim_dup(rhs + ra + rl);
            char *tg = expand(line);
            if (!tg) { free(tname); free(val); free(line); return -1; }
            char **ts; int nt; ts = split_words(tg, &nt);
            int simple = saved == ':';
            int append = saved == '+';
            char *stored = val;
            if (simple) {
                stored = expand(val);
                if (!stored) { free_words(ts, nt); free(tg); free(tname); free(val); free(line); return -1; }
            }
            for (int i = 0; i < nt; i++) {
                if (saved == '?' && (var_find(tname))) continue;
                tspec_add(ts[i], tname, stored, simple, append);
            }
            free(stored == val ? NULL : stored);
            free(val); free_words(ts, nt); free(tg); free(tname); free(line);
            return 0;
        }
        free(tname);
    }
    char *tg = expand(line);
    char *pr = expand(rhs);
    if (!tg || !pr) { free(tg); free(pr); free(line); return -1; }
    char **ts; int nt; ts = split_words(tg, &nt);
    char **pre, **ord; int npre, nord;
    char *prcopy = xstrdup(pr);
    split_order(prcopy, &pre, &npre, &ord, &nord);
    int second = -1;
    if (!dcolon) {
        int depth = 0;
        for (int i = 0; rhs[i]; i++) {
            if (rhs[i] == '$' && (rhs[i + 1] == '(' || rhs[i + 1] == '{')) {
                char open = rhs[i + 1], close = open == '(' ? ')' : '}';
                int d = 1; i += 2;
                while (rhs[i] && d) { if (rhs[i] == open) d++; else if (rhs[i] == close) d--; i++; }
                i--; continue;
            }
            if (rhs[i] == '(' || rhs[i] == '{') depth++;
            else if ((rhs[i] == ')' || rhs[i] == '}') && depth) depth--;
            else if (rhs[i] == ':' && !depth) { second = i; break; }
        }
    }
    if (second >= 0) {
        /* static pattern: targets already expanded; pattern and prereqs too,
         * but the split above used the whole rhs. Re-split around the colon. */
        free_words(pre, npre); free_words(ord, nord); free(prcopy);
        char *pat = trim_dup(rhs);
        /* pat currently is the expanded rhs because rhs is unexpanded... */
        free(pat);
        char *erhs = pr; /* expanded rhs */
        char *colon = NULL; int depth = 0;
        for (char *p = erhs; *p; p++) {
            if (*p == '(' || *p == '{') depth++;
            else if ((*p == ')' || *p == '}') && depth) depth--;
            else if (*p == ':' && !depth) { colon = p; break; }
        }
        if (!colon) { note("static pattern needs a target pattern"); free(tg); free(pr); free(line); free_words(ts, nt); return -1; }
        *colon = 0;
        char *pattern = trim_dup(erhs);
        char *prest = xstrdup(colon + 1);
        split_order(prest, &pre, &npre, &ord, &nord);
        free(prest);
        nbatch = 0; rule_open = 1;
        for (int i = 0; i < nt; i++) {
            char *stem = NULL;
            if (!match_pat(pattern, ts[i], &stem)) {
                fprintf(stderr, "make: target '%s' doesn't match pattern '%s'\n", ts[i], pattern);
                free(stem); free(pattern); free(tg); free(pr); free(line); free_words(ts, nt);
                free_words(pre, npre); free_words(ord, nord); return -1;
            }
            char **sp = xmalloc(sizeof(char *) * (npre ? npre : 1));
            char **so = xmalloc(sizeof(char *) * (nord ? nord : 1));
            for (int j = 0; j < npre; j++) sp[j] = subst_stem(pre[j], stem);
            for (int j = 0; j < nord; j++) so[j] = subst_stem(ord[j], stem);
            struct rule *r = find_single(ts[i]);
            if (!r) { r = new_rule(ts[i]); push_rule(r); remember_goal(ts[i]); }
            if (!r->stem) r->stem = stem; else free(stem);
            add_chunk(r, sp, npre, so, nord);
            batch_add(r);
        }
        free(pattern); free_words(pre, npre); free_words(ord, nord);
        free_words(ts, nt); free(tg); free(pr); free(line);
        return 0;
    }
    nbatch = 0;
    for (int i = 0; i < nt; i++) {
        if (special_target(ts[i], pre, npre)) continue;
        if (strchr(ts[i], '%')) {
            struct rule *r = new_rule(ts[i]);
            r->pattern = 1; r->dcolon = dcolon;
            char **sp = xmalloc(sizeof(char *) * (npre ? npre : 1));
            char **so = xmalloc(sizeof(char *) * (nord ? nord : 1));
            for (int j = 0; j < npre; j++) sp[j] = xstrdup(pre[j]);
            for (int j = 0; j < nord; j++) so[j] = xstrdup(ord[j]);
            add_chunk(r, sp, npre, so, nord);
            push_rule(r); batch_add(r);
        } else {
            struct rule *r = (!dcolon) ? find_single(ts[i]) : NULL;
            if (!r) { r = new_rule(ts[i]); r->dcolon = dcolon; push_rule(r); remember_goal(ts[i]); }
            char **sp = xmalloc(sizeof(char *) * (npre ? npre : 1));
            char **so = xmalloc(sizeof(char *) * (nord ? nord : 1));
            for (int j = 0; j < npre; j++) sp[j] = xstrdup(pre[j]);
            for (int j = 0; j < nord; j++) so[j] = xstrdup(ord[j]);
            add_chunk(r, sp, npre, so, nord);
            batch_add(r);
        }
    }
    free_words(pre, npre); free_words(ord, nord);
    free_words(ts, nt); free(tg); free(pr); free(line); free(prcopy);
    /* GNU make still consumes following recipe lines when the target list
     * expands to nothing (e.g. empty $(CLAR_TEST_PROG):). */
    rule_open = 1;
    return 0;
}
static void add_recipe(const char *body) {
    for (int i = 0; i < nbatch; i++) {
        struct rule *r = batchv[i];
        GROW(r->rec, r->nrec, r->reccap);
        r->rec[r->nrec++] = xstrdup(body);
        if (r->nch) {
            for (int c = 0; c < r->nch; c++) r->ch[c].primary = 0;
            r->ch[r->nch - 1].primary = 1;
        }
    }
}
static char *next_logical(const char **pp) {
    const char *p = *pp;
    if (!*p) return NULL;
    struct buf b; buf_init(&b);
    int any = 0;
    for (;;) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len && p[len - 1] == '\r') len--;
        int cont = len && p[len - 1] == '\\';
        if (cont) len--;
        if (any) {
            /* GNU make заменяет «\» и перевод строки вместе с отступом следующей строки одним пробелом. */
            while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
            buf_putc(&b, ' ');
        }
        buf_add(&b, p, len);
        src_line++;
        any = 1;
        if (!nl) { *pp = p + len; break; }
        p = nl + 1;
        if (!cont) { *pp = p; break; }
    }
    return b.s;
}
static int take_line(char *raw) {
    if (defining) {
        char *t = trim_dup(raw);
        int end = !strcmp(t, "endef");
        free(t);
        if (end) return finish_define();
        if (def_body.n) buf_putc(&def_body, '\n');
        buf_puts(&def_body, raw);
        return 0;
    }
    if (raw[0] == '\t' && rule_open && !skipping() && !def_skip) { add_recipe(raw + 1); return 0; }
    char *s = trim_dup(raw);
    strip_comment(s); rtrim(s);
    if (!s[0]) { free(s); return 0; }
    if (def_skip) {
        if (key(s, "define")) def_skip++;
        else if (!strcmp(s, "endef")) def_skip--;
        free(s); return 0;
    }
    if (skipping()) {
        int rc = 0;
        if (key(s, "define")) def_skip = 1;
        else if (is_cond(s)) rc = handle_cond(s);
        free(s); return rc;
    }
    if (is_cond(s)) { int rc = handle_cond(s); free(s); return rc; }
    rule_open = 0; nbatch = 0;
    int rc = handle_body(s, 0);
    free(s);
    return rc;
}
static int parse_lines(const char *text) {
    const char *p = text;
    char *line;
    while ((line = next_logical(&p))) {
        int rc = take_line(line);
        free(line);
        if (rc < 0) return -1;
    }
    if (defining) { note("unterminated define"); return -1; }
    return 0;
}
static int parse_text(const char *text, const char *name) {
    const char *save_name = src_name; int save_line = src_line;
    int save_open = rule_open, save_n = nbatch, save_def = defining;
    struct rule **save_b = NULL;
    if (nbatch) {
        save_b = xmalloc((size_t)nbatch * sizeof *save_b);
        memcpy(save_b, batchv, (size_t)nbatch * sizeof *batchv);
    }
    src_name = name; src_line = 0; rule_open = 0; nbatch = 0; defining = 0;
    parse_depth++;
    int rc = parse_lines(text);
    parse_depth--;
    if (rc == 0 && parse_depth == 0 && cond_sp) { note("unterminated conditional"); rc = -1; }
    defining = save_def; rule_open = save_open;
    if (save_b) { memcpy(batchv, save_b, (size_t)save_n * sizeof *batchv); free(save_b); }
    nbatch = save_n; src_name = save_name; src_line = save_line;
    return rc;
}
static int load_file(const char *path, int optional) {
    char *text = NULL, *used = NULL;
    if (slurp(path, &text) == 0) used = xstrdup(path);
    else if (src_dir && path[0] != '/') {
        char *alt = join_path(src_dir, path);
        if (slurp(alt, &text) == 0) used = alt; else free(alt);
    }
    if (!text) {
        if (optional) return 0;
        fprintf(stderr, "make: %s: cannot open\n", path);
        return -1;
    }
    note_makefile(path);
    note_makefile(used);
    char *dir = dirname_dup(used);
    const char *saved = src_dir;
    src_dir = dir;
    int rc = parse_text(text, used);
    src_dir = saved;
    free(text); free(dir); free(used);
    return rc;
}

/* ---------- build graph ---------- */

enum { NODE_NEW, NODE_PLANNED, NODE_RUNNING, NODE_DONE };
struct build_node {
    char *name; struct rule **rs; int nrs;
    int *deps; int ndeps, depcap;
    int state, pid, built, phony;
    unsigned stamp; /* время файла после рецепта; 0, если файла нет */
};
static struct build_node *nodes; static int nnodes, nodecap;

static int has_explicit(const char *name) {
    for (int i = 0; i < nrules; i++)
        if (!rules[i]->pattern && !strcmp(rules[i]->target, name)) return 1;
    return 0;
}
static int pattern_ok(struct rule *r, const char *stem) {
    for (int c = 0; c < r->nch; c++) for (int i = 0; i < r->ch[c].npre; i++) {
        char *dep = subst_stem(r->ch[c].pre[i], stem);
        int ok = file_info(dep, NULL) || has_explicit(dep) || is_phony(dep);
        free(dep);
        if (!ok) return 0;
    }
    return 1;
}
static struct rule *apply_pattern(const char *name) {
    for (int i = 0; i < nrules; i++) {
        struct rule *r = rules[i];
        if (!r->pattern) continue;
        char *stem = NULL;
        if (!match_pat(r->target, name, &stem)) continue;
        if (!pattern_ok(r, stem)) { free(stem); continue; }
        struct rule *n = new_rule(name);
        n->stem = stem; n->dcolon = r->dcolon;
        for (int c = 0; c < r->nch; c++) {
            char **sp = xmalloc(sizeof(char *) * (r->ch[c].npre ? r->ch[c].npre : 1));
            char **so = xmalloc(sizeof(char *) * (r->ch[c].nord ? r->ch[c].nord : 1));
            for (int j = 0; j < r->ch[c].npre; j++) sp[j] = subst_stem(r->ch[c].pre[j], stem);
            for (int j = 0; j < r->ch[c].nord; j++) so[j] = subst_stem(r->ch[c].ord[j], stem);
            add_chunk(n, sp, r->ch[c].npre, so, r->ch[c].nord);
            n->ch[n->nch - 1].primary = r->ch[c].primary;
        }
        for (int k = 0; k < r->nrec; k++) {
            GROW(n->rec, n->nrec, n->reccap);
            n->rec[n->nrec++] = xstrdup(r->rec[k]);
        }
        return n;
    }
    return NULL;
}
static void absorb(struct rule *dst, struct rule *src) {
    int src_primary = 0;
    for (int c = 0; c < src->nch; c++) if (src->ch[c].primary) src_primary = 1;
    if (src_primary) for (int c = 0; c < dst->nch; c++) dst->ch[c].primary = 0;
    for (int c = 0; c < src->nch; c++) {
        add_chunk(dst, src->ch[c].pre, src->ch[c].npre, src->ch[c].ord, src->ch[c].nord);
        dst->ch[dst->nch - 1].primary = src->ch[c].primary;
        src->ch[c].pre = NULL; src->ch[c].ord = NULL;
    }
    for (int k = 0; k < src->nrec; k++) {
        GROW(dst->rec, dst->nrec, dst->reccap);
        dst->rec[dst->nrec++] = src->rec[k];
        src->rec[k] = NULL;
    }
    if (!dst->stem && src->stem) { dst->stem = src->stem; src->stem = NULL; }
    free(src->target); free(src->stem); free(src->ch); free(src->rec); free(src);
}
static int plan(const char *name, int depth) {
    for (int i = 0; i < nnodes; i++) if (!strcmp(nodes[i].name, name)) {
        if (nodes[i].state == NODE_NEW) {
            fprintf(stderr, "make: circular dependency: %s\n", name); return -1;
        }
        return i;
    }
    if (depth > 64) { fprintf(stderr, "make: dependency nesting is too deep\n"); return -1; }
    GROW(nodes, nnodes, nodecap);
    int idx = nnodes++;
    memset(&nodes[idx], 0, sizeof nodes[idx]);
    nodes[idx].name = xstrdup(name);
    nodes[idx].phony = is_phony(name);
    nodes[idx].state = NODE_NEW;
    struct rule *single = NULL;
    struct rule **dcol = NULL; int nd = 0, dcap = 0;
    for (int i = 0; i < nrules; i++) {
        if (rules[i]->pattern || strcmp(rules[i]->target, name)) continue;
        if (rules[i]->dcolon) { GROW(dcol, nd, dcap); dcol[nd++] = rules[i]; }
        else single = rules[i];
    }
    if (single && !single->nrec) {
        struct rule *p = apply_pattern(name);
        if (p) absorb(single, p);
    }
    if (!single && !nd) {
        struct rule *p = apply_pattern(name);
        if (p) single = p;
        else if (!nodes[idx].phony && !file_info(name, NULL)) {
            fprintf(stderr, "make: *** no rule to make target '%s'\n", name);
            free(dcol); return -1;
        }
    }
    int nrs = (single ? 1 : 0) + nd;
    if (nrs) {
        nodes[idx].rs = xmalloc((size_t)nrs * sizeof(struct rule *));
        if (single) nodes[idx].rs[nodes[idx].nrs++] = single;
        for (int i = 0; i < nd; i++) nodes[idx].rs[nodes[idx].nrs++] = dcol[i];
    }
    free(dcol);
    char **deps = NULL; int ndeps = 0, dcap2 = 0;
    for (int ri = 0; ri < nodes[idx].nrs; ri++) {
        struct rule *r = nodes[idx].rs[ri];
        for (int c = 0; c < r->nch; c++) {
            for (int j = 0; j < r->ch[c].npre; j++) {
                int seen = 0;
                for (int k = 0; k < ndeps; k++) if (!strcmp(deps[k], r->ch[c].pre[j])) seen = 1;
                if (!seen) { GROW(deps, ndeps, dcap2); deps[ndeps++] = r->ch[c].pre[j]; }
            }
            for (int j = 0; j < r->ch[c].nord; j++) {
                int seen = 0;
                for (int k = 0; k < ndeps; k++) if (!strcmp(deps[k], r->ch[c].ord[j])) seen = 1;
                if (!seen) { GROW(deps, ndeps, dcap2); deps[ndeps++] = r->ch[c].ord[j]; }
            }
        }
    }
    for (int i = 0; i < ndeps; i++) {
        int d = plan(deps[i], depth + 1);
        if (d < 0) { free(deps); return -1; }
        int seen = 0;
        for (int j = 0; j < nodes[idx].ndeps; j++) if (nodes[idx].deps[j] == d) seen = 1;
        if (!seen) {
            GROW(nodes[idx].deps, nodes[idx].ndeps, nodes[idx].depcap);
            nodes[idx].deps[nodes[idx].ndeps++] = d;
        }
    }
    free(deps);
    nodes[idx].state = NODE_PLANNED;
    return idx;
}

static char **rule_pres(struct rule *r, int *n, int unique) {
    char **a = NULL; int c = 0, cap = 0;
    for (int pass = 0; pass < 2; pass++) for (int ci = 0; ci < r->nch; ci++) {
        if ((pass == 0) != r->ch[ci].primary) continue;
        for (int i = 0; i < r->ch[ci].npre; i++) {
            if (unique) {
                int seen = 0;
                for (int j = 0; j < c; j++) if (!strcmp(a[j], r->ch[ci].pre[i])) seen = 1;
                if (seen) continue;
            }
            GROW(a, c, cap);
            a[c++] = r->ch[ci].pre[i];
        }
    }
    *n = c; return a;
}
static int pre_newer(const char *target, const char *pre) {
    unsigned tm = 0, dm = 0;
    int te = file_info(target, &tm);
    if (!te || is_phony(pre)) return 1;
    for (int i = 0; i < nnodes; i++) if (!strcmp(nodes[i].name, pre)) {
        if (nodes[i].phony) return 1;
        /* Рецепт FORCE часто ничего не переписывает. Тогда файл остаётся
         * старше цели, и зависимые объекты не надо собирать заново. */
        if (nodes[i].built) return nodes[i].stamp > tm;
        break;
    }
    if (file_info(pre, &dm) && dm > tm) return 1;
    return 0;
}
static int rule_stale(const char *name, struct rule *r, int phony) {
    if (phony || !file_info(name, NULL)) return 1;
    int n = 0; char **a = rule_pres(r, &n, 0);
    int stale = 0;
    for (int i = 0; i < n; i++) if (pre_newer(name, a[i])) { stale = 1; break; }
    free(a);
    return stale;
}
struct tsave { char *name, *val; int existed, simple, exported, locked; };
static struct tsave *apply_tspec(const char *target, int *nsave) {
    struct tsave *sv = NULL; int n = 0, cap = 0;
    for (int i = 0; i < ntspec; i++) {
        int match = 0;
        if (strchr(tspecs[i].target, '%')) match = match_pat(tspecs[i].target, target, NULL);
        else match = !strcmp(tspecs[i].target, target);
        if (!match) continue;
        int seen = 0;
        for (int j = 0; j < n; j++) if (!strcmp(sv[j].name, tspecs[i].name)) seen = 1;
        if (!seen) {
            GROW(sv, n, cap);
            struct var *v = var_find(tspecs[i].name);
            sv[n].name = xstrdup(tspecs[i].name);
            sv[n].existed = v != NULL;
            sv[n].val = v ? xstrdup(v->val) : NULL;
            sv[n].simple = v ? v->simple : 0;
            sv[n].exported = v ? v->exported : 0;
            sv[n].locked = v ? v->locked : 0;
            n++;
        }
        if (tspecs[i].append) var_append(tspecs[i].name, tspecs[i].val, 1);
        else var_set(tspecs[i].name, tspecs[i].val, tspecs[i].simple, 0, 0, 1);
    }
    *nsave = n; return sv;
}
static void restore_tspec(struct tsave *sv, int n) {
    for (int i = 0; i < n; i++) {
        if (!sv[i].existed) var_remove(sv[i].name);
        else {
            var_set(sv[i].name, sv[i].val, sv[i].simple, sv[i].exported, sv[i].locked, 1);
            struct var *v = var_find(sv[i].name);
            if (v) { v->exported = sv[i].exported; v->locked = sv[i].locked; }
        }
        free(sv[i].name); free(sv[i].val);
    }
    free(sv);
}
static int one_recipe(const char *target, const char *body) {
    char *exp = expand(body);
    if (!exp) return -1;
    char *cmd = exp;
    int quiet = 0, ignore = 0, plus = 0;
    while (*cmd == '@' || *cmd == '-' || *cmd == '+') {
        if (*cmd == '@') quiet = 1;
        else if (*cmd == '-') ignore = 1;
        else plus = 1;
        cmd++;
    }
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) { free(exp); return 0; }
    /* Включённый makefile пересобирается и при -n: так GNU обновляет GIT-VERSION-FILE. */
    int mk = is_makefile(target);
    if (!silent && ((dry_run && !mk) || !quiet)) { printf("%s\n", cmd); fflush(stdout); }
    if (dry_run && !plus && !mk) { any_ran = 1; free(exp); return 0; }
    any_ran = 1;
    int st = run_sh(cmd);
    free(exp);
    if (st != 0 && !ignore) {
        int code = st < 0 ? st : (WIFEXITED(st) ? WEXITSTATUS(st) : st);
        fprintf(stderr, "make: *** [%s] Error %d\n", target, code);
        if (delete_on_error) unlink(target);
        return -1;
    }
    return 0;
}
static int run_rule(const char *name, struct rule *r) {
    int nu = 0, na = 0;
    char **up = rule_pres(r, &nu, 1);
    char **ap = rule_pres(r, &na, 0);
    char *hat = NULL, *plus = NULL, *qm = NULL, *lt = NULL, *bar = NULL;
    struct buf hb, pb, qb, bb; buf_init(&hb); buf_init(&pb); buf_init(&qb); buf_init(&bb);
    for (int i = 0; i < nu; i++) { if (i) buf_putc(&hb, ' '); buf_puts(&hb, up[i]); }
    for (int i = 0; i < na; i++) { if (i) buf_putc(&pb, ' '); buf_puts(&pb, ap[i]); }
    for (int i = 0; i < na; i++) if (pre_newer(name, ap[i])) {
        if (qb.n) buf_putc(&qb, ' ');
        buf_puts(&qb, ap[i]);
    }
    for (int c = 0; c < r->nch; c++) for (int i = 0; i < r->ch[c].nord; i++) {
        if (bb.n) buf_putc(&bb, ' ');
        buf_puts(&bb, r->ch[c].ord[i]);
    }
    hat = hb.s; plus = pb.s; qm = qb.s; bar = bb.s;
    lt = nu ? xstrdup(up[0]) : xstrdup("");
    struct actx ax = { name, lt, hat, plus, r->stem ? r->stem : "", qm, bar };
    int nsave = 0; struct tsave *sv = apply_tspec(name, &nsave);
    cur_auto = &ax;
    int rc = 0;
    for (int i = 0; i < r->nrec && !rc; i++) rc = one_recipe(name, r->rec[i]);
    cur_auto = NULL;
    restore_tspec(sv, nsave);
    free(up); free(ap); free(hat); free(plus); free(qm); free(bar); free(lt);
    return rc;
}
static int node_recipes(struct build_node *node) {
    for (int i = 0; i < node->nrs; i++) {
        if (!rule_stale(node->name, node->rs[i], node->phony)) continue;
        if (!node->rs[i]->nrec) continue;
        if (run_rule(node->name, node->rs[i]) < 0) return -1;
    }
    return 0;
}
static int reap(int block, int *active, int *failed) {
    int status = 0, pid;
    do { pid = waitpid(-1, &status, block ? 0 : WNOHANG); } while (pid == -EINTR);
    if (pid < 0) { fprintf(stderr, "make: waitpid failed (%d)\n", pid); return -1; }
    if (!pid) return 0;
    for (int i = 0; i < nnodes; i++) if (nodes[i].state == NODE_RUNNING && nodes[i].pid == pid) {
        nodes[i].state = NODE_DONE;
        nodes[i].built = status == 0;
        if (nodes[i].built) {
            unsigned m = 0;
            nodes[i].stamp = file_info(nodes[i].name, &m) ? m : 0;
        }
        (*active)--;
        if (status) *failed = 1;
        return 1;
    }
    fprintf(stderr, "make: unexpected child %d\n", pid);
    return -1;
}
static int node_needed(struct build_node *node, int *recs) {
    int need = 0; *recs = 0;
    if (!node->nrs) return node->phony || !file_info(node->name, NULL);
    for (int i = 0; i < node->nrs; i++) if (rule_stale(node->name, node->rs[i], node->phony)) {
        need = 1;
        if (node->rs[i]->nrec) *recs = 1;
    }
    return need;
}
static int execute_graph(void) {
    int active = 0, failed = 0;
    for (;;) {
        while (active) {
            int result = reap(0, &active, &failed);
            if (result < 0) { failed = 1; break; }
            if (!result) break;
        }
        if (failed) break;
        int remaining = 0, progress = 0;
        for (int i = 0; i < nnodes; i++) {
            struct build_node *node = &nodes[i];
            if (node->state == NODE_DONE) continue;
            remaining++;
            if (node->state != NODE_PLANNED || active >= jobs) continue;
            int ready = 1;
            for (int j = 0; j < node->ndeps; j++) if (nodes[node->deps[j]].state != NODE_DONE) ready = 0;
            if (!ready) continue;
            int recs = 0, need = node_needed(node, &recs);
            if (!need || !recs) {
                node->built = need; node->state = NODE_DONE;
                if (need) {
                    unsigned m = 0;
                    node->stamp = file_info(node->name, &m) ? m : 0;
                }
                progress = 1; continue;
            }
            any_ran = 1;
            if (dry_run) {
                if (node_recipes(node) < 0) { failed = 1; break; }
                node->built = 1; node->state = NODE_DONE; progress = 1; continue;
            }
            fflush(stdout); fflush(stderr);
            int pid = fork();
            if (pid < 0) { fprintf(stderr, "make: fork failed (%d)\n", pid); failed = 1; break; }
            if (!pid) exit(node_recipes(node) < 0 ? 1 : 0);
            node->pid = pid; node->state = NODE_RUNNING; active++; progress = 1;
        }
        if (failed || !remaining) break;
        if (!progress) {
            if (!active) { fprintf(stderr, "make: graph stalled\n"); failed = 1; break; }
            if (reap(1, &active, &failed) < 0) { failed = 1; break; }
        }
    }
    while (active) if (reap(1, &active, &failed) < 0) break;
    return failed ? -1 : 0;
}

/* ---------- main ---------- */

static int job_count(const char *value) {
    int count = build_jobs_parse(value);
    return count < 0 ? 0 : count ? count : build_jobs_auto();
}
static void usage(void) {
    fprintf(stderr, "usage: make [-n] [-s] [-j N|-jN|--jobs=N] [-C dir] [-f file] [VAR=value] [target...]\n");
}
static void import_env(void) {
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char *name = xstrndup(*e, (int)(eq - *e));
        var_set(name, eq + 1, 0, 1, 0, 0);
        free(name);
    }
}
static void maybe(const char *name, const char *val) { if (!var_find(name)) var_set(name, val, 1, 0, 0, 0); }
static void cli_assign(const char *s) {
    int at, len;
    if (!find_op(s, &at, &len, 0)) return;
    char *copied = xstrndup(s, at);
    char *name = trim_dup(copied);
    free(copied);
    char *val = trim_dup(s + at + len);
    char kind = s[at];
    if (kind == '+') var_append(name, val, 1);
    else if (kind == '?') { if (!var_find(name)) var_set(name, val, 0, 1, 1, 1); }
    else if (kind == ':') {
        char *e = expand(val);
        var_set(name, e ? e : "", 1, 1, 1, 1);
        free(e);
    } else var_set(name, val, 0, 1, 1, 1);
    struct var *v = var_find(name); if (v) { v->exported = 1; v->locked = 1; }
    free(name); free(val);
}
static void flags_from_env(void) {
    const char *m = getenv("MAKEFLAGS");
    if (!m) return;
    for (; *m; m++) { if (*m == 'n') dry_run = 1; if (*m == 's') silent = 1; }
}

int main(int argc, char **argv) {
    const char *file = NULL, *dir = NULL;
    char **goals = NULL; int ngoals = 0, gcap = 0;
    flags_from_env();
    import_env();
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strncmp(a, "-j", 2) || !strncmp(a, "--jobs=", 7) || !strcmp(a, "--jobs")) {
            const char *value = "auto";
            if (!strncmp(a, "--jobs=", 7)) value = a + 7;
            else if (a[1] == 'j' && a[2]) value = a + 2;
            else if (i + 1 < argc && ((argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') || !strcmp(argv[i + 1], "auto")))
                value = argv[++i];
            jobs = job_count(value);
            if (!jobs) { fprintf(stderr, "make: jobs must be auto or 1..%d\n", MAX_JOBS); return 2; }
            continue;
        }
        if (!strcmp(a, "--help")) { usage(); return 0; }
        if (!strncmp(a, "--", 2)) continue;
        if (a[0] == '-' && a[1]) {
            if (a[1] == 'n' && !a[2]) { dry_run = 1; continue; }
            if (a[1] == 's' && !a[2]) { silent = 1; continue; }
            if (a[1] == 'C' || a[1] == 'f' || a[1] == 'I' || a[1] == 'k' || a[1] == 'w'
                || a[1] == 'r' || a[1] == 'R' || a[1] == 'e' || a[1] == 'd') {
                if ((a[1] == 'C' || a[1] == 'f' || a[1] == 'I') && !a[2]) {
                    if (i + 1 >= argc) { usage(); return 2; }
                    if (a[1] == 'C') dir = argv[++i];
                    else if (a[1] == 'f') file = argv[++i];
                    else i++;
                } else if (a[1] == 'C') dir = a + 2;
                else if (a[1] == 'f') file = a + 2;
                continue;
            }
            usage(); return 2;
        }
        if (strchr(a, '=')) { cli_assign(a); continue; }
        GROW(goals, ngoals, gcap);
        goals[ngoals++] = a;
    }
    if (dir && chdir(dir) < 0) { fprintf(stderr, "make: %s: cannot enter\n", dir); return 2; }
    maybe("CC", "cc"); maybe("AR", "ar"); maybe("RM", "rm -f");
    maybe("MAKE", argv[0] && argv[0][0] ? argv[0] : "make");
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd)) maybe("CURDIR", cwd);
    struct buf mf; buf_init(&mf);
    if (dry_run) buf_putc(&mf, 'n');
    if (silent) buf_putc(&mf, 's');
    var_set("MAKEFLAGS", mf.s, 1, 1, 1, 1);
    setenv("MAKEFLAGS", mf.s, 1);
    free(mf.s);
    if (!file) {
        if (file_info("Makefile", NULL)) file = "Makefile";
        else if (file_info("makefile", NULL)) file = "makefile";
        else { fprintf(stderr, "make: no Makefile\n"); return 2; }
    }
    if (load_file(file, 0) < 0) return 2;
    if (!ngoals) {
        if (!default_goal) { fprintf(stderr, "make: no targets\n"); return 2; }
        goals = xmalloc(sizeof *goals); goals[0] = default_goal; ngoals = 1;
    }
    for (int i = 0; i < ngoals; i++) if (plan(goals[i], 0) < 0) return 2;
    if (execute_graph() < 0) return 2;
    if (!any_ran) printf("make: '%s' is up to date\n", goals[0]);
    return 0;
}
