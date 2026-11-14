/* awk -f script [file]. Сборка Git вызывает один сценарий: clar-generate.awk. */
#include "myos.h"
#include "stdio.h"
#include "string.h"

struct buf { char *s; int n, cap; };
static void binit(struct buf *b) { b->s = malloc(64); b->s[0] = 0; b->n = 0; b->cap = 64; }
static void badd(struct buf *b, const char *s) {
    int k = (int)strlen(s);
    while (b->n + k + 1 > b->cap) { b->cap *= 2; b->s = realloc(b->s, (size_t)b->cap); }
    memcpy(b->s + b->n, s, (size_t)k + 1);
    b->n += k;
}
static void bfmt(struct buf *b, const char *s) { badd(b, s); }

static int suite_count, callback_count;
static struct buf suites, callbacks;
static char initialize[256], cleanup[256], prev_suite[128];
static int count;

static void add_suite(const char *suite) {
    char line[512];
    if (!suite || !suite[0]) return;
    suite_count++;
    callback_count += count;
    snprintf(line, sizeof line, "    {\n        \"%s\",\n        %s,\n        %s,\n        _clar_cb_%s, %d, 1\n    },\n",
             suite, initialize, cleanup, suite, count);
    badd(&suites, line);
}

static void field3(const char *line, char *out, int cap) {
    int f = 0, n = 0;
    const char *p = line;
    out[0] = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        f++;
        if (f == 3) {
            while (*p && *p != ' ' && *p != '\t' && n + 1 < cap) out[n++] = *p++;
            out[n] = 0;
            return;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
}

static void clar_line(const char *line) {
    char third[256], name[256], suite[256], short_name[256], cb[512];
    char *lp, *us;
    if (!line[0]) return;
    printf("%s\n", line);
    field3(line, third, (int)sizeof third);
    snprintf(name, sizeof name, "%s", third);
    lp = strchr(name, '(');
    if (lp) *lp = 0;
    snprintf(suite, sizeof suite, "%s", name);
    if (!strncmp(suite, "test_", 5)) memmove(suite, suite + 5, strlen(suite + 5) + 1);
    us = strstr(suite, "__");
    if (us) *us = 0;
    us = strrchr(name, '_');
    if (us && us > name && us[-1] == '_') snprintf(short_name, sizeof short_name, "%s", us + 1);
    else snprintf(short_name, sizeof short_name, "%s", name);
    snprintf(cb, sizeof cb, "{ \"%s\", &%s }", short_name, name);
    if (strcmp(suite, prev_suite)) {
        add_suite(prev_suite);
        if (callbacks.n) badd(&callbacks, "};\n");
        bfmt(&callbacks, "static const struct clar_func _clar_cb_");
        badd(&callbacks, suite);
        badd(&callbacks, "[] = {\n");
        snprintf(initialize, sizeof initialize, "{ NULL, NULL }");
        snprintf(cleanup, sizeof cleanup, "{ NULL, NULL }");
        count = 0;
        snprintf(prev_suite, sizeof prev_suite, "%s", suite);
    }
    if (!strcmp(short_name, "initialize")) snprintf(initialize, sizeof initialize, "%s", cb);
    else if (!strcmp(short_name, "cleanup")) snprintf(cleanup, sizeof cleanup, "%s", cb);
    else {
        badd(&callbacks, "    ");
        badd(&callbacks, cb);
        badd(&callbacks, ",\n");
        count++;
    }
}

static int clar_file(const char *path) {
    FILE *f;
    char line[1024];
    char tail[64];
    f = path ? fopen(path, "r") : stdin;
    if (!f) { dprintf(STDERR, "awk: %s: %s\n", path ? path : "stdin", strerror(errno)); return 1; }
    binit(&suites); binit(&callbacks);
    badd(&suites, "static struct clar_suite _clar_suites[] = {\n");
    initialize[0] = cleanup[0] = prev_suite[0] = 0;
    while (fgets(line, (int)sizeof line, f)) {
        int n = (int)strlen(line);
        if (n && line[n - 1] == '\n') line[n - 1] = 0;
        clar_line(line);
    }
    if (path) fclose(f);
    add_suite(prev_suite[0] ? prev_suite : "");
    badd(&suites, "};");
    if (callbacks.n) badd(&callbacks, "};");
    printf("%s\n%s\n", callbacks.s, suites.s);
    snprintf(tail, sizeof tail, "static const size_t _clar_suite_count = %d;\n", suite_count);
    printf("%s", tail);
    snprintf(tail, sizeof tail, "static const size_t _clar_callback_count = %d;\n", callback_count);
    printf("%s", tail);
    return 0;
}

int main(int argc, char **argv) {
    const char *script = NULL, *file = NULL;
    char *text = NULL;
    int i, fd, n = 0, cap = 0;
    char ch;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) script = argv[++i];
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!script) { dprintf(STDERR, "usage: awk -f script [file]\n"); return 1; }
    fd = open(script, O_RDONLY);
    if (fd < 0) { dprintf(STDERR, "awk: %s: %s\n", script, strerror(errno)); return 1; }
    while (read(fd, &ch, 1) == 1) {
        if (n + 2 > cap) { cap = cap ? cap * 2 : 256; text = realloc(text, (size_t)cap); }
        text[n++] = ch;
    }
    close(fd);
    if (text) text[n] = 0;
    if (!text || !strstr(text, "add_suite")) {
        dprintf(STDERR, "awk: unsupported script %s\n", script);
        return 1;
    }
    return clar_file(file);
}
