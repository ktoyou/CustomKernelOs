/* POSIX regcomp/regexec: BRE и ERE, поиск с возвратами.
 * Без long double и без чужих библиотек — собирается и gcc, и cc. */
#include "regex.h"
#include "stdlib.h"
#include "string.h"

enum {
    OP_CHAR = 1, OP_ANY, OP_CLASS, OP_BOL, OP_EOL,
    OP_SPLIT, OP_JMP, OP_SAVE, OP_BACK, OP_MATCH
};

typedef struct {
    int *code;
    int ncode;
    unsigned char *bits;
    int nclass;
    int nsub;
    int flags;
} Re;

typedef struct { int *p; int n, cap; } Frag;

typedef struct {
    const char *pat;
    int pos, len;
    int ext, flags, nsub, err;
    unsigned char *bits;
    int nclass, ccap;
} Comp;

static int peek(Comp *c) { return c->pos < c->len ? (unsigned char)c->pat[c->pos] : -1; }
static int getc_re(Comp *c) { return c->pos < c->len ? (unsigned char)c->pat[c->pos++] : -1; }

static int fold(int ch, int icase) {
    if (icase && ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

static void femit(Comp *c, Frag *f, int v) {
    if (c->err) return;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 16;
        int *p = malloc(sizeof(int) * (size_t)cap);
        if (!p) { c->err = REG_ESPACE; return; }
        if (f->n) memcpy(p, f->p, sizeof(int) * (size_t)f->n);
        free(f->p);
        f->p = p;
        f->cap = cap;
    }
    f->p[f->n++] = v;
}

static int oplen(int op) {
    if (op == OP_CHAR || op == OP_CLASS || op == OP_JMP || op == OP_SAVE || op == OP_BACK) return 2;
    if (op == OP_SPLIT) return 3;
    return 1;
}

static void fshift(Frag *f, int base) {
    int i = 0;
    if (!f->p) return;
    while (i < f->n) {
        int op = f->p[i];
        if (op == OP_JMP) f->p[i + 1] += base;
        else if (op == OP_SPLIT) { f->p[i + 1] += base; f->p[i + 2] += base; }
        i += oplen(op);
    }
}

static Frag fclone(Comp *c, Frag *a) {
    Frag b;
    int i;
    b.p = 0; b.n = b.cap = 0;
    for (i = 0; i < a->n; i++) femit(c, &b, a->p[i]);
    return b;
}

static void fcat(Comp *c, Frag *a, Frag *b) {
    int i;
    for (i = 0; i < b->n; i++) femit(c, a, b->p[i]);
    free(b->p);
    b->p = 0; b->n = b->cap = 0;
}

static void ffree(Frag *f) { free(f->p); f->p = 0; f->n = f->cap = 0; }

static Frag wrap_star(Comp *c, Frag atom) {
    Frag o;
    o.p = 0; o.n = o.cap = 0;
    femit(c, &o, OP_SPLIT);
    femit(c, &o, 3);
    femit(c, &o, 0);
    fshift(&atom, 3);
    fcat(c, &o, &atom);
    femit(c, &o, OP_JMP);
    femit(c, &o, 0);
    if (o.p) o.p[2] = o.n;
    return o;
}

static Frag wrap_opt(Comp *c, Frag atom) {
    Frag o;
    o.p = 0; o.n = o.cap = 0;
    femit(c, &o, OP_SPLIT);
    femit(c, &o, 3);
    femit(c, &o, 0);
    fshift(&atom, 3);
    fcat(c, &o, &atom);
    if (o.p) o.p[2] = o.n;
    return o;
}

static Frag wrap_plus(Comp *c, Frag atom) {
    Frag s = wrap_star(c, fclone(c, &atom));
    fshift(&s, atom.n);
    fcat(c, &atom, &s);
    return atom;
}

static Frag wrap_repeat(Comp *c, Frag atom, int n, int m) {
    Frag out;
    int i;
    out.p = 0; out.n = out.cap = 0;
    for (i = 0; i < n; i++) {
        Frag a = fclone(c, &atom);
        fshift(&a, out.n);
        fcat(c, &out, &a);
    }
    if (m < 0) {
        Frag s = wrap_star(c, fclone(c, &atom));
        fshift(&s, out.n);
        fcat(c, &out, &s);
    } else {
        for (i = n; i < m; i++) {
            Frag a = wrap_opt(c, fclone(c, &atom));
            fshift(&a, out.n);
            fcat(c, &out, &a);
        }
    }
    ffree(&atom);
    return out;
}

static void bit_set(unsigned char *b, int ch, int icase) {
    if (ch < 0 || ch > 255) return;
    b[ch >> 3] |= (unsigned char)(1u << (ch & 7));
    if (!icase) return;
    if (ch >= 'A' && ch <= 'Z') bit_set(b, ch - 'A' + 'a', 0);
    if (ch >= 'a' && ch <= 'z') bit_set(b, ch - 'a' + 'A', 0);
}

static int named_class(int ch, const char *name) {
    if (!strcmp(name, "digit")) return ch >= '0' && ch <= '9';
    if (!strcmp(name, "xdigit")) return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    if (!strcmp(name, "lower")) return ch >= 'a' && ch <= 'z';
    if (!strcmp(name, "upper")) return ch >= 'A' && ch <= 'Z';
    if (!strcmp(name, "alpha")) return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    if (!strcmp(name, "alnum")) return named_class(ch, "alpha") || named_class(ch, "digit");
    if (!strcmp(name, "space")) return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
    if (!strcmp(name, "blank")) return ch == ' ' || ch == '\t';
    if (!strcmp(name, "cntrl")) return ch < 32 || ch == 127;
    if (!strcmp(name, "graph")) return ch > 32 && ch < 127;
    if (!strcmp(name, "print")) return ch >= 32 && ch < 127;
    if (!strcmp(name, "punct")) return named_class(ch, "graph") && !named_class(ch, "alnum");
    return -1;
}

static int add_class(Comp *c, unsigned char bits[32]) {
    unsigned char *n;
    int cap;
    if (c->err) return 0;
    if (c->nclass == c->ccap) {
        cap = c->ccap ? c->ccap * 2 : 4;
        n = malloc((size_t)cap * 32);
        if (!n) { c->err = REG_ESPACE; return 0; }
        if (c->nclass) memcpy(n, c->bits, (size_t)c->nclass * 32);
        free(c->bits);
        c->bits = n;
        c->ccap = cap;
    }
    memcpy(c->bits + c->nclass * 32, bits, 32);
    return c->nclass++;
}

static int parse_class(Comp *c) {
    unsigned char bits[32];
    int neg = 0, first = 1, prev = -1, dash = 0, ch;
    memset(bits, 0, sizeof bits);
    if (peek(c) == '^') { neg = 1; getc_re(c); }
    if (peek(c) == ']') { bit_set(bits, ']', c->flags & REG_ICASE); getc_re(c); first = 0; prev = ']'; }
    while ((ch = peek(c)) >= 0 && ch != ']') {
        getc_re(c);
        if (ch == '[' && peek(c) == ':') {
            char name[16];
            int n = 0, k, kind;
            getc_re(c);
            while (peek(c) >= 0 && peek(c) != ':' && n < 15) name[n++] = (char)getc_re(c);
            name[n] = 0;
            if (getc_re(c) != ':' || getc_re(c) != ']') { c->err = REG_ECTYPE; return 0; }
            kind = named_class('a', name);
            if (kind < 0) { c->err = REG_ECTYPE; return 0; }
            for (k = 1; k < 256; k++) if (named_class(k, name) == 1) bit_set(bits, k, c->flags & REG_ICASE);
            prev = -1; dash = 0; first = 0;
            continue;
        }
        if (ch == '\\' && peek(c) >= 0) ch = getc_re(c);
        if (ch == '-' && !first && peek(c) >= 0 && peek(c) != ']') {
            dash = 1;
            continue;
        }
        if (dash) {
            if (prev < 0 || ch < prev) { c->err = REG_ERANGE; return 0; }
            while (prev <= ch) bit_set(bits, prev++, c->flags & REG_ICASE);
            dash = 0; prev = ch; first = 0;
            continue;
        }
        bit_set(bits, ch, c->flags & REG_ICASE);
        prev = ch;
        first = 0;
    }
    if (dash) bit_set(bits, '-', c->flags & REG_ICASE);
    if (getc_re(c) != ']') { c->err = REG_EBRACK; return 0; }
    if (neg) {
        int i;
        for (i = 0; i < 32; i++) bits[i] = (unsigned char)~bits[i];
        bits[0] &= (unsigned char)~1u;                 /* NUL в строке Си не бывает */
        if (c->flags & REG_NEWLINE) bits['\n' >> 3] &= (unsigned char)~(1u << ('\n' & 7));
    }
    return add_class(c, bits);
}

static int at_stop(Comp *c) {
    int ch = peek(c);
    if (ch < 0) return 1;
    if (c->ext) return ch == ')' || ch == '|';
    return ch == '\\' && c->pos + 1 < c->len && c->pat[c->pos + 1] == ')';
}

static int dollar_anchor(Comp *c) {
    int i = c->pos + 1;
    if (i >= c->len) return 1;
    if (c->ext) return c->pat[i] == ')' || c->pat[i] == '|';
    return c->pat[i] == '\\' && i + 1 < c->len && c->pat[i + 1] == ')';
}

static int is_quant(Comp *c) {
    int ch = peek(c), i;
    if (ch == '*') return 1;
    if (c->ext && (ch == '+' || ch == '?')) return 1;
    if (c->ext && ch == '{') i = c->pos + 1;
    else if (!c->ext && ch == '\\' && c->pos + 1 < c->len && c->pat[c->pos + 1] == '{') i = c->pos + 2;
    else return 0;
    return i < c->len && c->pat[i] >= '0' && c->pat[i] <= '9';
}

static Frag parse_alt(Comp *c);

static Frag parse_atom(Comp *c, int at_bol) {
    Frag o;
    int ch;
    o.p = 0; o.n = o.cap = 0;
    if (c->err || at_stop(c)) { c->err = c->err ? c->err : REG_BADPAT; return o; }
    ch = getc_re(c);
    if (ch == '^' && at_bol) { femit(c, &o, OP_BOL); return o; }
    if (ch == '.') { femit(c, &o, OP_ANY); return o; }
    if (ch == '[') {
        int cls = parse_class(c);
        femit(c, &o, OP_CLASS);
        femit(c, &o, cls);
        return o;
    }
    if (ch == '\\') {
        int e = getc_re(c);
        if (e < 0) { c->err = REG_EESCAPE; return o; }
        if (!c->ext && e == '(') {
            int id = ++c->nsub;
            Frag inner;
            if (id > 31) { c->err = REG_ESPACE; return o; }
            inner = parse_alt(c);
            if (getc_re(c) != '\\' || getc_re(c) != ')') { c->err = REG_EPAREN; ffree(&inner); return o; }
            femit(c, &o, OP_SAVE); femit(c, &o, id * 2);
            fshift(&inner, o.n);
            fcat(c, &o, &inner);
            femit(c, &o, OP_SAVE); femit(c, &o, id * 2 + 1);
            return o;
        }
        if (!c->ext && e >= '1' && e <= '9') {
            if (e - '0' > c->nsub) { c->err = REG_ESUBREG; return o; }
            femit(c, &o, OP_BACK); femit(c, &o, e - '0');
            return o;
        }
        ch = e;
    } else if (c->ext && ch == '(') {
        int id = ++c->nsub;
        Frag inner;
        if (id > 31) { c->err = REG_ESPACE; return o; }
        inner = parse_alt(c);
        if (getc_re(c) != ')') { c->err = REG_EPAREN; ffree(&inner); return o; }
        femit(c, &o, OP_SAVE); femit(c, &o, id * 2);
        fshift(&inner, o.n);
        fcat(c, &o, &inner);
        femit(c, &o, OP_SAVE); femit(c, &o, id * 2 + 1);
        return o;
    }
    femit(c, &o, OP_CHAR);
    femit(c, &o, ch);
    return o;
}

static int read_num(Comp *c, int *out) {
    int v = 0, any = 0;
    while (peek(c) >= '0' && peek(c) <= '9') {
        any = 1;
        v = v * 10 + (getc_re(c) - '0');
        if (v > 255) { c->err = REG_BADBR; return 0; }
    }
    if (!any) return 0;
    *out = v;
    return 1;
}

static Frag apply_quant(Comp *c, Frag atom) {
    int ch = getc_re(c), n = 0, m = 0, comma = 0, has_m = 0;
    if (!c->ext && ch == '\\') ch = getc_re(c);
    if (ch == '*') return wrap_star(c, atom);
    if (ch == '+') return wrap_plus(c, atom);
    if (ch == '?') return wrap_opt(c, atom);
    if (!read_num(c, &n)) { c->err = REG_BADBR; return atom; }
    if (peek(c) == ',') { comma = 1; getc_re(c); }
    if (peek(c) >= '0' && peek(c) <= '9') { if (!read_num(c, &m)) return atom; has_m = 1; }
    if (c->ext) { if (getc_re(c) != '}') c->err = REG_EBRACE; }
    else if (getc_re(c) != '\\' || getc_re(c) != '}') c->err = REG_EBRACE;
    if (!comma) m = n;
    else if (!has_m) m = -1;
    if (m >= 0 && m < n) c->err = REG_BADBR;
    return wrap_repeat(c, atom, n, m);
}

static Frag parse_concat(Comp *c) {
    Frag out;
    int at_bol = 1;
    out.p = 0; out.n = out.cap = 0;
    while (!c->err && !at_stop(c)) {
        Frag atom;
        if (peek(c) == '$' && dollar_anchor(c) && !is_quant(c)) {
            femit(c, &out, OP_EOL);
            getc_re(c);
            at_bol = 0;
            continue;
        }
        atom = parse_atom(c, at_bol);
        at_bol = 0;
        if (c->err) { ffree(&atom); break; }
        if (is_quant(c)) {
            if (atom.n == 0) { c->err = REG_BADRPT; ffree(&atom); break; }
            atom = apply_quant(c, atom);
        }
        fshift(&atom, out.n);
        fcat(c, &out, &atom);
    }
    return out;
}

static Frag parse_alt(Comp *c) {
    Frag left = parse_concat(c);
    if (!c->ext || c->err || peek(c) != '|') return left;
    getc_re(c);
    {
        Frag right = parse_alt(c);
        Frag o;
        int jmp;
        o.p = 0; o.n = o.cap = 0;
        femit(c, &o, OP_SPLIT);
        femit(c, &o, 3);
        femit(c, &o, 0);
        fshift(&left, 3);
        fcat(c, &o, &left);
        jmp = o.n;
        femit(c, &o, OP_JMP);
        femit(c, &o, 0);
        if (o.p) o.p[2] = o.n;
        fshift(&right, o.n);
        fcat(c, &o, &right);
        if (o.p) o.p[jmp + 1] = o.n;
        return o;
    }
}

static int class_has(Re *re, int idx, int ch) {
    unsigned char *b = re->bits + idx * 32;
    return (b[ch >> 3] >> (ch & 7)) & 1;
}

typedef struct { int pc, prev; } Undo;
typedef struct { int pc, pos, nmk; int *slots; } Choice;

static void undo_to(int *at, Undo *u, int *nmk, int target) {
    while (*nmk > target) {
        (*nmk)--;
        at[u[*nmk].pc] = u[*nmk].prev;
    }
}

/* Итеративный поиск: стек развилок на куче, чтобы длинная .* не росла стеком Си.
 * Повтор SPLIT в той же позиции — пустой цикл, берём другую ветку. */
static int run(Re *re, const char *s, int n, int pos, int *slots, int eflags, int bol) {
    int ns = (re->nsub + 1) * 2;
    int pc = 0, steps = 0, nmk = 0, mcap = 0, nch = 0, ccap = 0;
    int *at = malloc(sizeof(int) * (size_t)re->ncode);
    Undo *undo = 0;
    Choice *ch = 0;
    int i, result = 0;
    if (!at) return -1;
    for (i = 0; i < re->ncode; i++) at[i] = -1;
    for (;;) {
        int op;
        if (pc < 0 || pc >= re->ncode) goto fail;
        if (++steps > 2000000) { result = -1; goto out; }
        op = re->code[pc];
        if (op == OP_CHAR) {
            int want = re->code[pc + 1];
            if (pos >= n || fold((unsigned char)s[pos], re->flags & REG_ICASE) != fold(want, re->flags & REG_ICASE))
                goto fail;
            pos++; pc += 2;
        } else if (op == OP_ANY) {
            if (pos >= n) goto fail;
            if ((re->flags & REG_NEWLINE) && s[pos] == '\n') goto fail;
            pos++; pc++;
        } else if (op == OP_CLASS) {
            if (pos >= n || !class_has(re, re->code[pc + 1], (unsigned char)s[pos])) goto fail;
            pos++; pc += 2;
        } else if (op == OP_BOL) {
            if (pos == bol) { if (eflags & REG_NOTBOL) goto fail; }
            else if (!((re->flags & REG_NEWLINE) && s[pos - 1] == '\n')) goto fail;
            pc++;
        } else if (op == OP_EOL) {
            if (pos == n) { if (eflags & REG_NOTEOL) goto fail; }
            else if (!((re->flags & REG_NEWLINE) && s[pos] == '\n')) goto fail;
            pc++;
        } else if (op == OP_SAVE) {
            slots[re->code[pc + 1]] = pos;
            pc += 2;
        } else if (op == OP_BACK) {
            int g = re->code[pc + 1];
            int so = slots[g * 2], eo = slots[g * 2 + 1], len, k;
            if (so < 0 || eo < so) goto fail;
            len = eo - so;
            if (pos + len > n) goto fail;
            for (k = 0; k < len; k++)
                if (fold((unsigned char)s[so + k], re->flags & REG_ICASE) !=
                    fold((unsigned char)s[pos + k], re->flags & REG_ICASE)) goto fail;
            pos += len;
            pc += 2;
        } else if (op == OP_SPLIT) {
            int *save;
            if (at[pc] == pos) goto fail;
            if (nmk == mcap) {
                int cap = mcap ? mcap * 2 : 32;
                Undo *p = malloc(sizeof(Undo) * (size_t)cap);
                if (!p) { result = -1; goto out; }
                if (nmk) memcpy(p, undo, sizeof(Undo) * (size_t)nmk);
                free(undo);
                undo = p;
                mcap = cap;
            }
            undo[nmk].pc = pc;
            undo[nmk].prev = at[pc];
            at[pc] = pos;
            nmk++;
            if (nch == ccap) {
                int cap = ccap ? ccap * 2 : 32;
                Choice *p = malloc(sizeof(Choice) * (size_t)cap);
                if (!p) { result = -1; goto out; }
                if (nch) memcpy(p, ch, sizeof(Choice) * (size_t)nch);
                free(ch);
                ch = p;
                ccap = cap;
            }
            save = malloc(sizeof(int) * (size_t)ns);
            if (!save) { result = -1; goto out; }
            memcpy(save, slots, sizeof(int) * (size_t)ns);
            ch[nch].pc = re->code[pc + 2];
            ch[nch].pos = pos;
            ch[nch].nmk = nmk;
            ch[nch].slots = save;
            nch++;
            pc = re->code[pc + 1];
        } else if (op == OP_JMP) {
            pc = re->code[pc + 1];
        } else if (op == OP_MATCH) {
            result = 1;
            goto out;
        } else goto fail;
        continue;
    fail:
        if (nch == 0) { result = 0; goto out; }
        nch--;
        pc = ch[nch].pc;
        pos = ch[nch].pos;
        undo_to(at, undo, &nmk, ch[nch].nmk);
        memcpy(slots, ch[nch].slots, sizeof(int) * (size_t)ns);
        free(ch[nch].slots);
        ch[nch].slots = 0;
    }
out:
    while (nch > 0) { nch--; free(ch[nch].slots); }
    free(ch);
    free(undo);
    free(at);
    return result;
}

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    Comp c;
    Frag body, all;
    Re *re;
    memset(&c, 0, sizeof c);
    if (!preg) return REG_BADPAT;
    preg->re_nsub = 0;
    preg->re_p = 0;
    if (!pattern) return REG_BADPAT;
    c.pat = pattern;
    c.len = (int)strlen(pattern);
    c.ext = (cflags & REG_EXTENDED) != 0;
    c.flags = cflags;
    body = parse_alt(&c);
    if (!c.err && c.pos != c.len) c.err = REG_BADPAT;
    all.p = 0; all.n = all.cap = 0;
    if (!c.err) {
        femit(&c, &all, OP_SAVE); femit(&c, &all, 0);
        fshift(&body, all.n);
        fcat(&c, &all, &body);
        femit(&c, &all, OP_SAVE); femit(&c, &all, 1);
        femit(&c, &all, OP_MATCH);
    } else ffree(&body);
    if (c.err) { ffree(&all); free(c.bits); return c.err; }
    re = malloc(sizeof *re);
    if (!re) { ffree(&all); free(c.bits); return REG_ESPACE; }
    re->code = all.p;
    re->ncode = all.n;
    re->bits = c.bits;
    re->nclass = c.nclass;
    re->nsub = c.nsub;
    re->flags = cflags;
    preg->re_nsub = (size_t)c.nsub;
    preg->re_p = re;
    return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t *pmatch, int eflags) {
    Re *re;
    int n, start, ns, i;
    int slots[64];
    if (!preg || !preg->re_p || !string) return REG_BADPAT;
    re = preg->re_p;
    if (eflags & REG_STARTEND) {
        if (!pmatch || pmatch[0].rm_so < 0 || pmatch[0].rm_eo < pmatch[0].rm_so) return REG_BADPAT;
        start = (int)pmatch[0].rm_so;
        n = (int)pmatch[0].rm_eo;
    } else {
        start = 0;
        n = (int)strlen(string);
    }
    ns = (re->nsub + 1) * 2;
    if (ns > 64) return REG_ESPACE;
    for (; start <= n; start++) {
        int r;
        int bol = (eflags & REG_STARTEND) ? (int)pmatch[0].rm_so : 0;
        for (i = 0; i < ns; i++) slots[i] = -1;
        r = run(re, string, n, start, slots, eflags, bol);
        if (r < 0) return REG_ESPACE;
        if (r == 1) {
            if (pmatch && !(re->flags & REG_NOSUB)) {
                for (i = 0; i < (int)nmatch; i++) {
                    if (i <= re->nsub && slots[i * 2] >= 0) {
                        pmatch[i].rm_so = slots[i * 2];
                        pmatch[i].rm_eo = slots[i * 2 + 1];
                    } else pmatch[i].rm_so = pmatch[i].rm_eo = -1;
                }
            }
            return 0;
        }
        if (start == n) break;
    }
    return REG_NOMATCH;
}

void regfree(regex_t *preg) {
    Re *re;
    if (!preg || !preg->re_p) return;
    re = preg->re_p;
    free(re->code);
    free(re->bits);
    free(re);
    preg->re_p = 0;
    preg->re_nsub = 0;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    const char *msg = "unknown regex error";
    size_t n;
    (void)preg;
    switch (errcode) {
    case 0: msg = "success"; break;
    case REG_NOMATCH: msg = "no match"; break;
    case REG_BADPAT: msg = "invalid pattern"; break;
    case REG_ECOLLATE: msg = "invalid collating element"; break;
    case REG_ECTYPE: msg = "invalid character class"; break;
    case REG_EESCAPE: msg = "trailing backslash"; break;
    case REG_ESUBREG: msg = "invalid back reference"; break;
    case REG_EBRACK: msg = "unmatched ["; break;
    case REG_EPAREN: msg = "unmatched parenthesis"; break;
    case REG_EBRACE: msg = "unmatched brace"; break;
    case REG_BADBR: msg = "invalid brace count"; break;
    case REG_ERANGE: msg = "invalid range"; break;
    case REG_ESPACE: msg = "out of memory"; break;
    case REG_BADRPT: msg = "repetition with nothing to repeat"; break;
    }
    n = strlen(msg) + 1;
    if (errbuf && errbuf_size) {
        size_t c = n < errbuf_size ? n : errbuf_size;
        memcpy(errbuf, msg, c - 1);
        errbuf[c - 1] = 0;
    }
    return n;
}
