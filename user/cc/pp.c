/* Препроцессор: #include, #define (с параметрами, # и ##), #if/#ifdef/#else/#endif.
 * Работает на списке токенов. Чтобы макрос не раскрывался сам в себя бесконечно,
 * у каждого токена есть «hideset» — имена макросов, из которых он уже получился. */
#include "cc.h"

typedef struct MacroParam { struct MacroParam *next; char *name; } MacroParam;
typedef struct MacroArg { struct MacroArg *next; char *name; Token *tok; } MacroArg;

typedef struct Macro {
    struct Macro *next;
    char *name;
    bool  is_objlike;
    MacroParam *params;
    char *va_name;              /* имя для ... (__VA_ARGS__) */
    Token *body;
    int    builtin;             /* 1 __FILE__, 2 __LINE__ */
} Macro;

struct Hideset { Hideset *next; char *name; };

typedef struct CondIncl {
    struct CondIncl *next;
    int  ctx;                   /* 0 в #if, 1 в #elif, 2 в #else */
    Token *tok;
    bool included;
} CondIncl;

#define NBUCKET 256
static Macro *macros[NBUCKET];
static CondIncl *cond_incl;
static int include_depth;

/* path → имя стража. Повторный #include не токенизируется, если страж уже
 * определён: иначе OpenSSL (ssl.h тянет obj_mac.h десятки раз) съедает сотни МБ. */
typedef struct IncGuard { struct IncGuard *next; char *path; char *macro; } IncGuard;
static IncGuard *inc_guards[NBUCKET];

static unsigned hash(const char *s, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h % NBUCKET;
}

static Macro *find_macro(Token *tok) {
    if (tok->kind != TK_IDENT) return NULL;
    for (Macro *m = macros[hash(tok->loc, tok->len)]; m; m = m->next)
        if ((int)strlen(m->name) == tok->len && !strncmp(m->name, tok->loc, tok->len)) return m;
    return NULL;
}

static Macro *add_macro(char *name, bool objlike, Token *body) {
    unsigned h = hash(name, strlen(name));
    Macro *m = xcalloc(sizeof(Macro));
    m->name = name;
    m->is_objlike = objlike;
    m->body = body;
    m->next = macros[h];
    macros[h] = m;
    return m;
}

static void undef_macro(char *name) {
    unsigned h = hash(name, strlen(name));
    for (Macro **pp = &macros[h]; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, name)) { *pp = (*pp)->next; return; }
}

/* ------------------------------------------------------------ hideset */

static Hideset *new_hideset(char *name) { Hideset *h = xcalloc(sizeof(Hideset)); h->name = name; return h; }

static Hideset *hideset_union(Hideset *a, Hideset *b) {
    Hideset head = {0}, *cur = &head;
    for (; a; a = a->next) cur = cur->next = new_hideset(a->name);
    cur->next = b;
    return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len) {
    for (; hs; hs = hs->next)
        if ((int)strlen(hs->name) == len && !strncmp(hs->name, s, len)) return true;
    return false;
}

static Token *add_hideset(Token *tok, Hideset *hs) {
    Token head = {0}, *cur = &head;
    for (; tok; tok = tok->next) {
        Token *t = copy_token(tok);
        t->hideset = hideset_union(t->hideset, hs);
        cur = cur->next = t;
    }
    return head.next;
}

/* ------------------------------------------------------------ списки */

static Token *append(Token *a, Token *b) {           /* копия a (без EOF) + b */
    if (a->kind == TK_EOF) return b;
    Token head = {0}, *cur = &head;
    for (; a->kind != TK_EOF; a = a->next) cur = cur->next = copy_token(a);
    cur->next = b;
    return head.next;
}

/* Список a свежий (файл только что прочитан) и больше нигде не нужен. */
static Token *splice(Token *a, Token *b) {
    Token *cur;
    if (a->kind == TK_EOF) return b;
    for (cur = a; cur->next->kind != TK_EOF; cur = cur->next);
    cur->next = b;
    return a;
}

static bool is_hash(Token *tok) { return tok->at_bol && equal(tok, "#"); }

static Token *skip_line(Token *tok) {
    if (tok->at_bol) return tok;
    warn_tok(tok, "extra token");
    while (!tok->at_bol) tok = tok->next;
    return tok;
}

/* токены до конца строки (новым списком с EOF) */
static Token *copy_line(Token **rest, Token *tok) {
    Token head = {0}, *cur = &head;
    for (; !tok->at_bol; tok = tok->next) cur = cur->next = copy_token(tok);
    cur->next = new_eof(tok);
    *rest = tok;
    return head.next;
}

static char *tok_str(Token *t) { return strndup2(t->loc, t->len); }

static bool macro_defined_name(const char *name) {
    int n = strlen(name);
    for (Macro *m = macros[hash(name, n)]; m; m = m->next)
        if ((int)strlen(m->name) == n && !strcmp(m->name, name)) return true;
    return false;
}

static char *guard_of(const char *path) {
    int n = strlen(path);
    for (IncGuard *g = inc_guards[hash(path, n)]; g; g = g->next)
        if (!strcmp(g->path, path)) return g->macro;
    return NULL;
}

static void remember_guard(char *path, char *macro) {
    unsigned h = hash(path, strlen(path));
    IncGuard *g = xcalloc(sizeof(IncGuard));
    g->path = path;
    g->macro = macro;
    g->next = inc_guards[h];
    inc_guards[h] = g;
}

/* Файл целиком обёрнут в #ifndef NAME / #define NAME / ... / #endif.
 * #else на внешнем уровне или текст после #endif — стража нет: такой файл
 * нельзя пропускать, когда NAME уже определён. */
static char *detect_include_guard(Token *tok) {
    Token *name, *def, *dir;
    int depth;
    if (!tok || !is_hash(tok) || !equal(tok->next, "ifndef")) return NULL;
    name = tok->next->next;
    if (!name || name->kind != TK_IDENT) return NULL;
    tok = name->next;
    if (!tok || !is_hash(tok) || !equal(tok->next, "define")) return NULL;
    def = tok->next->next;
    if (!def || def->kind != TK_IDENT || def->len != name->len ||
        strncmp(def->loc, name->loc, name->len)) return NULL;
    tok = def->next;
    depth = 1;
    while (tok->kind != TK_EOF) {
        if (!is_hash(tok)) { tok = tok->next; continue; }
        dir = tok->next;
        if (!dir || dir->kind == TK_EOF) break;
        if (equal(dir, "if") || equal(dir, "ifdef") || equal(dir, "ifndef")) {
            depth++;
            tok = dir->next;
            continue;
        }
        if (equal(dir, "elif") || equal(dir, "else")) {
            if (depth == 1) return NULL;
            tok = dir->next;
            continue;
        }
        if (equal(dir, "endif")) {
            depth--;
            tok = dir->next;
            while (tok->kind != TK_EOF && !tok->at_bol) tok = tok->next;
            if (depth == 0) return tok->kind == TK_EOF ? tok_str(name) : NULL;
            continue;
        }
        tok = tok->next;
    }
    return NULL;
}

/* ------------------------------------------------------------ #define */

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_name) {
    MacroParam head = {0}, *cur = &head;
    while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");
        if (equal(tok, "...")) { *va_name = "__VA_ARGS__"; *rest = skip(tok->next, ")"); return head.next; }
        if (tok->kind != TK_IDENT) error_tok(tok, "expected an identifier");
        if (equal(tok->next, "...")) { *va_name = tok_str(tok); *rest = skip(tok->next->next, ")"); return head.next; }
        MacroParam *m = xcalloc(sizeof(MacroParam));
        m->name = tok_str(tok);
        cur = cur->next = m;
        tok = tok->next;
    }
    *rest = tok->next;
    return head.next;
}

static void read_macro_definition(Token **rest, Token *tok) {
    if (tok->kind != TK_IDENT) error_tok(tok, "macro name must be an identifier");
    char *name = tok_str(tok);
    tok = tok->next;
    if (!tok->has_space && equal(tok, "(")) {
        char *va = NULL;
        MacroParam *params = read_macro_params(&tok, tok->next, &va);
        Macro *m = add_macro(name, false, copy_line(rest, tok));
        m->params = params;
        m->va_name = va;
    } else {
        add_macro(name, true, copy_line(rest, tok));
    }
}

/* ------------------------------------------------------------ аргументы макроса */

static MacroArg *read_macro_arg_one(Token **rest, Token *tok, bool read_rest) {
    Token head = {0}, *cur = &head;
    int level = 0;
    for (;;) {
        if (level == 0 && equal(tok, ")")) break;
        if (level == 0 && !read_rest && equal(tok, ",")) break;
        if (tok->kind == TK_EOF) error_tok(tok, "premature end of input");
        if (equal(tok, "(")) level++;
        else if (equal(tok, ")")) level--;
        cur = cur->next = copy_token(tok);
        tok = tok->next;
    }
    cur->next = new_eof(tok);
    MacroArg *arg = xcalloc(sizeof(MacroArg));
    arg->tok = head.next;
    *rest = tok;
    return arg;
}

static MacroArg *read_macro_args(Token **rest, Token *tok, MacroParam *params, char *va_name) {
    Token *start = tok;
    tok = tok->next->next;                           /* имя ( */
    MacroArg head = {0}, *cur = &head;
    MacroParam *pp = params;
    for (; pp; pp = pp->next) {
        if (cur != &head) tok = skip(tok, ",");
        cur = cur->next = read_macro_arg_one(&tok, tok, false);
        cur->name = pp->name;
    }
    if (va_name) {
        MacroArg *arg;
        if (equal(tok, ")")) { arg = xcalloc(sizeof(MacroArg)); arg->tok = new_eof(tok); }
        else { if (pp != params) tok = skip(tok, ","); arg = read_macro_arg_one(&tok, tok, true); }
        arg->name = va_name;
        cur = cur->next = arg;
    } else if (pp) {
        error_tok(start, "too few arguments");
    }
    if (!equal(tok, ")")) error_tok(start, "too many arguments");
    *rest = tok;
    return head.next;
}

static MacroArg *find_arg(MacroArg *args, Token *tok) {
    for (MacroArg *a = args; a; a = a->next)
        if (tok->len == (int)strlen(a->name) && !strncmp(tok->loc, a->name, tok->len)) return a;
    return NULL;
}

/* #x -> "x" */
static Token *stringize(Token *hash_tok, Token *arg) {
    int len = 1;
    for (Token *t = arg; t->kind != TK_EOF; t = t->next) len += t->len + 1;
    char *buf = xcalloc(len * 2 + 3);
    int pos = 0;
    buf[pos++] = '"';
    for (Token *t = arg; t->kind != TK_EOF; t = t->next) {
        if (t != arg && t->has_space) buf[pos++] = ' ';
        for (int i = 0; i < t->len; i++) {
            if (t->kind == TK_STR && (t->loc[i] == '"' || t->loc[i] == '\\')) buf[pos++] = '\\';
            buf[pos++] = t->loc[i];
        }
    }
    buf[pos++] = '"';
    buf[pos] = 0;
    File *f = xcalloc(sizeof(File));
    f->name = hash_tok->file->name;
    f->contents = buf;
    Token *t = tokenize(f);
    t->line = hash_tok->line;
    return t;
}

/* a ## b -> ab */
static Token *paste(Token *lhs, Token *rhs) {
    char *buf = format("%.*s%.*s", lhs->len, lhs->loc, rhs->len, rhs->loc);
    File *f = xcalloc(sizeof(File));
    f->name = lhs->file->name;
    f->contents = buf;
    Token *t = tokenize(f);
    if (t->next->kind != TK_EOF) error_tok(lhs, "pasting forms '%s', an invalid token", buf);
    t->line = lhs->line;
    return t;
}

static Token *preprocess2(Token *tok);

static Token *subst(Token *tok, MacroArg *args) {
    Token head = {0}, *cur = &head;
    while (tok->kind != TK_EOF) {
        if (equal(tok, "#")) {                        /* #param */
            MacroArg *arg = find_arg(args, tok->next);
            if (!arg) error_tok(tok->next, "'#' is not followed by a macro parameter");
            cur = cur->next = stringize(tok, arg->tok);
            tok = tok->next->next;
            continue;
        }
        if (equal(tok, "##")) {
            if (cur == &head) error_tok(tok, "'##' cannot appear at start of macro expansion");
            if (tok->next->kind == TK_EOF) error_tok(tok, "'##' cannot appear at end of macro expansion");
            MacroArg *arg = find_arg(args, tok->next);
            if (arg) {
                if (arg->tok->kind != TK_EOF) {
                    *cur = *paste(cur, arg->tok);
                    for (Token *t = arg->tok->next; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
                }
                tok = tok->next->next;
                continue;
            }
            *cur = *paste(cur, tok->next);
            tok = tok->next->next;
            continue;
        }
        MacroArg *arg = find_arg(args, tok);
        if (arg && equal(tok->next, "##")) {
            Token *rhs = tok->next->next;
            if (arg->tok->kind == TK_EOF) {
                MacroArg *arg2 = find_arg(args, rhs);
                if (arg2) { for (Token *t = arg2->tok; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t); }
                else cur = cur->next = copy_token(rhs);
                tok = rhs->next;
                continue;
            }
            for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
            tok = tok->next;
            continue;
        }
        if (arg) {                                   /* параметр: сначала раскрыть сам аргумент */
            Token *t = preprocess2(arg->tok);
            if (t->kind != TK_EOF) { t->at_bol = tok->at_bol; t->has_space = tok->has_space; }
            for (; t->kind != TK_EOF; t = t->next) cur = cur->next = copy_token(t);
            tok = tok->next;
            continue;
        }
        cur = cur->next = copy_token(tok);
        tok = tok->next;
    }
    cur->next = tok;
    return head.next;
}

static bool expand_macro(Token **rest, Token *tok) {
    if (hideset_contains(tok->hideset, tok->loc, tok->len)) return false;
    Macro *m = find_macro(tok);
    if (!m) return false;

    if (m->builtin) {
        Token *t = copy_token(tok);
        if (m->builtin == 1) {
            char *s = format("\"%s\"", tok->file->name);
            File *f = xcalloc(sizeof(File)); f->name = tok->file->name; f->contents = s;
            t = tokenize(f);
        } else {
            t->kind = TK_NUM; t->val = tok->line; t->ival = (unsigned)tok->line; t->ty = ty_int;
        }
        t->at_bol = tok->at_bol; t->has_space = tok->has_space; t->line = tok->line;
        t->next = tok->next;
        *rest = t;
        return true;
    }

    if (m->is_objlike) {
        Hideset *hs = hideset_union(tok->hideset, new_hideset(m->name));
        Token *body = add_hideset(m->body, hs);
        for (Token *t = body; t->kind != TK_EOF; t = t->next) { t->line = tok->line; t->file = tok->file; }
        *rest = append(body, tok->next);
        if ((*rest) != tok->next) { (*rest)->at_bol = tok->at_bol; (*rest)->has_space = tok->has_space; }
        return true;
    }

    if (!equal(tok->next, "(")) return false;        /* имя функционального макроса без скобок */
    Token *macro_tok = tok;
    MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_name);
    Token *rparen = tok;
    Hideset *hs = hideset_union(macro_tok->hideset, new_hideset(m->name));
    (void)rparen;
    Token *body = subst(m->body, args);
    body = add_hideset(body, hs);
    for (Token *t = body; t && t->kind != TK_EOF; t = t->next) { t->line = macro_tok->line; t->file = macro_tok->file; }
    *rest = append(body, tok->next);
    if (*rest != tok->next) { (*rest)->at_bol = macro_tok->at_bol; (*rest)->has_space = macro_tok->has_space; }
    return true;
}

/* ------------------------------------------------------------ #include */

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return "";
    return strndup2(path, slash - path + 1);
}

static char *search_include(char *name, Token *tok, bool quoted) {
    if (name[0] == '/') return name;
    if (quoted) {
        char *p = format("%s%s", dir_of(tok->file->name), name);
        int len;
        char *s = read_file(p, &len);
        if (s) { free(s); return p; }
    }
    for (int i = 0; i < n_include_paths; i++) {
        char *p = format("%s/%s", include_paths[i], name);
        int len;
        char *s = read_file(p, &len);
        if (s) { free(s); return p; }
    }
    return NULL;
}

static Token *include_file(Token *tok, char *path, Token *filename_tok) {
    Token *t = tokenize_file(path);
    if (!t) error_tok(filename_tok, "%s: cannot open file", path);
    return splice(t, tok);
}

static char *read_include_filename(Token **rest, Token *tok, bool *quoted) {
    if (tok->kind == TK_STR) {
        *quoted = true;
        *rest = skip_line(tok->next);
        return strndup2(tok->loc + 1, tok->len - 2);
    }
    if (equal(tok, "<")) {
        Token *start = tok;
        for (; !equal(tok, ">"); tok = tok->next)
            if (tok->at_bol || tok->kind == TK_EOF) error_tok(tok, "expected '>'");
        *quoted = false;
        *rest = skip_line(tok->next);
        char *buf = xcalloc(256);
        int pos = 0;
        for (Token *t = start->next; t != tok; t = t->next) {
            if (t != start->next && t->has_space) buf[pos++] = ' ';
            if (pos + t->len >= 255) break;
            memcpy(buf + pos, t->loc, t->len);
            pos += t->len;
        }
        return buf;
    }
    error_tok(tok, "expected a filename");
}

/* ------------------------------------------------------------ #if */

static Token *skip_cond_incl2(Token *tok) {
    while (tok->kind != TK_EOF) {
        if (is_hash(tok) && (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef"))) {
            tok = skip_cond_incl2(tok->next->next);
            continue;
        }
        if (is_hash(tok) && equal(tok->next, "endif")) return tok->next->next;
        tok = tok->next;
    }
    return tok;
}

/* пропустить до #else/#elif/#endif своего уровня */
static Token *skip_cond_incl(Token *tok) {
    while (tok->kind != TK_EOF) {
        if (is_hash(tok) && (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef"))) {
            tok = skip_cond_incl2(tok->next->next);
            continue;
        }
        if (is_hash(tok) && (equal(tok->next, "elif") || equal(tok->next, "else") || equal(tok->next, "endif"))) break;
        tok = tok->next;
    }
    return tok;
}

static Token *read_const_expr(Token **rest, Token *tok) {
    tok = copy_line(rest, tok);
    Token head = {0}, *cur = &head;
    while (tok->kind != TK_EOF) {
        if (equal(tok, "defined")) {                 /* defined(X) или defined X */
            Token *start = tok;
            bool paren = consume(&tok, tok->next, "(");
            if (tok->kind != TK_IDENT) error_tok(start, "macro name must be an identifier");
            Macro *m = find_macro(tok);
            tok = tok->next;
            if (paren) tok = skip(tok, ")");
            Token *t = copy_token(start);
            t->kind = TK_NUM; t->val = m ? 1 : 0; t->ival = t->val; t->ty = ty_int;
            cur = cur->next = t;
            continue;
        }
        cur = cur->next = tok;
        tok = tok->next;
    }
    cur->next = tok;
    return head.next;
}

static int eval_const_expr(Token **rest, Token *tok) {
    Token *start = tok;
    Token *expr = read_const_expr(rest, tok->next);
    expr = preprocess2(expr);
    if (expr->kind == TK_EOF) error_tok(start, "no expression");
    for (Token *t = expr; t->kind != TK_EOF; t = t->next) {
        if (t->kind == TK_IDENT) { t->kind = TK_NUM; t->val = 0; t->ival = 0; t->ty = ty_int; }   /* неизвестное имя = 0 */
        else if (t->kind == TK_PP_NUM) convert_pp_number(t);
    }
    Token *rest2;
    int val = const_expr(&rest2, expr);
    if (rest2->kind != TK_EOF) error_tok(rest2, "extra token");
    return val;
}

static CondIncl *push_cond_incl(Token *tok, bool included) {
    CondIncl *ci = xcalloc(sizeof(CondIncl));
    ci->next = cond_incl;
    ci->tok = tok;
    ci->included = included;
    cond_incl = ci;
    return ci;
}

/* ------------------------------------------------------------ главный цикл */

static Token *preprocess2(Token *tok) {
    Token head = {0}, *cur = &head;
    while (tok->kind != TK_EOF) {
        /* st->open( — open это макрос с аргументами, но член структуры. */
        if (!(cur != &head && (equal(cur, ".") || equal(cur, "->")) && equal(tok->next, "("))
            && expand_macro(&tok, tok)) continue;
        if (!is_hash(tok)) {
            cur = cur->next = tok;
            tok = tok->next;
            continue;
        }
        Token *start = tok;
        tok = tok->next;

        if (equal(tok, "include")) {
            bool quoted;
            Token *arg = tok->next;
            char *name;
            if (arg->kind == TK_IDENT) {
                Macro *m = find_macro(arg);
                Token *b = m ? m->body : NULL;
                if (!m || m->builtin || !m->is_objlike || !b || b->kind != TK_STR)
                    error_tok(arg, "expected a filename");
                quoted = true;
                name = strndup2(b->loc + 1, b->len - 2);
                tok = skip_line(arg->next);
            } else name = read_include_filename(&tok, arg, &quoted);
            char *path = search_include(name, start, quoted);
            char *guard;
            if (!path) error_tok(start, "%s: file not found", name);
            guard = guard_of(path);
            if (guard && macro_defined_name(guard)) continue;
            /* Счётчик — число реально вставленных #include. Повтор одного и того же
             * стража сюда не доходит: OpenSSL иначе легко переваливает за тысячу. */
            if (++include_depth > 8192) error_tok(start, "#include nested too deeply");
            if (!guard) {
                Token *fresh = tokenize_file(path);
                char *found;
                if (!fresh) error_tok(start->next->next, "%s: cannot open file", path);
                found = detect_include_guard(fresh);
                if (found) remember_guard(path, found);
                tok = splice(fresh, tok);
            } else tok = include_file(tok, path, start->next->next);
            continue;
        }
        if (equal(tok, "define")) { read_macro_definition(&tok, tok->next); continue; }
        if (equal(tok, "undef")) {
            tok = tok->next;
            if (tok->kind != TK_IDENT) error_tok(tok, "macro name must be an identifier");
            undef_macro(tok_str(tok));
            tok = skip_line(tok->next);
            continue;
        }
        if (equal(tok, "if")) {
            int val = eval_const_expr(&tok, tok);
            push_cond_incl(start, val);
            if (!val) tok = skip_cond_incl(tok);
            continue;
        }
        if (equal(tok, "ifdef") || equal(tok, "ifndef")) {
            bool want = equal(tok, "ifdef");
            bool defined = find_macro(tok->next) != NULL;
            push_cond_incl(tok, defined == want);
            tok = skip_line(tok->next->next);
            if (defined != want) tok = skip_cond_incl(tok);
            continue;
        }
        if (equal(tok, "elif")) {
            if (!cond_incl || cond_incl->ctx == 2) error_tok(start, "stray #elif");
            cond_incl->ctx = 1;
            if (!cond_incl->included && eval_const_expr(&tok, tok)) cond_incl->included = true;
            else tok = skip_cond_incl(tok);
            continue;
        }
        if (equal(tok, "else")) {
            if (!cond_incl || cond_incl->ctx == 2) error_tok(start, "stray #else");
            cond_incl->ctx = 2;
            tok = skip_line(tok->next);
            if (cond_incl->included) tok = skip_cond_incl(tok);
            continue;
        }
        if (equal(tok, "endif")) {
            if (!cond_incl) error_tok(start, "stray #endif");
            cond_incl = cond_incl->next;
            tok = skip_line(tok->next);
            continue;
        }
        if (equal(tok, "error")) error_tok(tok, "#error");
        if (equal(tok, "pragma") || equal(tok, "line") || equal(tok, "warning")) {
            do tok = tok->next; while (!tok->at_bol);
            continue;
        }
        if (tok->at_bol) continue;                   /* пустая директива «#» */
        error_tok(tok, "invalid preprocessor directive");
    }
    cur->next = tok;
    return head.next;
}

#define MAX_USER_DEFS 128
static char *user_defs[MAX_USER_DEFS];
static int n_user_defs;

void pp_define(const char *spec) {
    if (!spec || !spec[0]) error("missing macro name after -D");
    if (n_user_defs == MAX_USER_DEFS) error("too many -D options");
    user_defs[n_user_defs++] = strndup2(spec, strlen(spec));
}

void define_macro(const char *name, const char *body) {
    File *f = xcalloc(sizeof(File));
    f->name = "<built-in>";
    f->contents = strndup2(body, strlen(body));
    add_macro(strndup2(name, strlen(name)), true, tokenize(f));
}

static void init_macros(void) {
    define_macro("__MYOS_CC__", "1");
    define_macro("__MYOS__", "1");
    define_macro("__STDC__", "1");
    define_macro("__STDC_VERSION__", "199901L");
    define_macro("__STDC_HOSTED__", "1");
    define_macro("__i386__", "1");
    define_macro("__LITTLE_ENDIAN__", "1");
    define_macro("__BYTE_ORDER__", "1234");
    define_macro("__ORDER_LITTLE_ENDIAN__", "1234");
    define_macro("__ORDER_BIG_ENDIAN__", "4321");
    define_macro("__BYTE_ORDER", "__BYTE_ORDER__");
    define_macro("__LITTLE_ENDIAN", "1234");
    define_macro("__BIG_ENDIAN", "4321");
    define_macro("__SIZEOF_INT__", "4");
    define_macro("__SIZEOF_POINTER__", "4");
    if (opt_linux) define_macro("__LINUX_TEST__", "1");
    if (opt_pic) { define_macro("__PIC__", "2"); define_macro("__pic__", "2"); }
    for (int i = 0; i < n_user_defs; i++) {
        char *s = user_defs[i];
        char *eq = strchr(s, '=');
        char *name = s;
        const char *body = "1";
        if (eq) {
            name = strndup2(s, (int)(eq - s));
            body = eq[1] ? eq + 1 : "1";
        }
        if (!name[0]) error("empty macro name after -D");
        define_macro(name, body);
    }
    add_macro("__FILE__", true, NULL)->builtin = 1;
    add_macro("__LINE__", true, NULL)->builtin = 2;
}

/* соседние строки "a" "b" -> "ab" */
static void join_strings(Token *tok) {
    for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
        if (t->kind != TK_STR || t->next->kind != TK_STR) continue;
        Token *end = t->next;
        int len = t->str_len - 1;
        while (end->kind == TK_STR) { len += end->str_len - 1; end = end->next; }
        char *buf = xcalloc(len + 1);
        int pos = 0;
        for (Token *u = t; u != end; u = u->next) { memcpy(buf + pos, u->str, u->str_len - 1); pos += u->str_len - 1; }
        t->str = buf;
        t->str_len = len + 1;
        t->ty = array_of(ty_char, len + 1);
        t->next = end;
    }
}

Token *preprocess(Token *tok) {
    memset(macros, 0, sizeof(macros));             /* каждый файл — с чистого листа */
    memset(inc_guards, 0, sizeof(inc_guards));
    cond_incl = NULL;
    include_depth = 0;
    init_macros();
    tok = preprocess2(tok);
    if (cond_incl) error_tok(cond_incl->tok, "unterminated conditional directive");
    convert_keywords(tok);
    for (Token *t = tok; t->kind != TK_EOF; t = t->next)
        if (t->kind == TK_PP_NUM) convert_pp_number(t);
    join_strings(tok);
    return tok;
}
