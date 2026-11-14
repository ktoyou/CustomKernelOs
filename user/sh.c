/* sh — командная оболочка MyOS.
 * Интерактивный ввод прежний. Скрипты и рецепты: переменные, if/for/while/case,
 * функции, $(...), ${VAR:-…} ${VAR#pat}, $(( )), test, перенаправления.
 */
#include "myos.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "sys/wait.h"

#define MAX_LINE 4096
#define HISTORY  32

static int  last_status;
static bool interactive = true;
static bool term_mode;

/* ------------------------------------------------------------ строка ввода
 * Терминал в сыром режиме: каждая клавиша приходит сразу, а рисуем строку мы
 * сами — так работают readline в bash и linenoise. Строка показывается в одной
 * экранной строке; если она длиннее, видимое окно сдвигается за курсором. */

static char history[HISTORY][MAX_LINE];
static int  hist_n;

static void add_history(const char *line) {
    if (!line[0] || (hist_n && !strcmp(history[(hist_n - 1) % HISTORY], line))) return;
    strncpy(history[hist_n % HISTORY], line, MAX_LINE - 1);
    hist_n++;
}

static int visible_len(const char *s) {          /* длина без escape-последовательностей */
    int n = 0;
    for (; *s; s++) {
        if (*s == 27) { while (*s && !isalpha(*s)) s++; if (!*s) break; continue; }
        n++;
    }
    return n;
}

/* длина в буквах (UTF-8: продолжения 10xxxxxx не считаем) */
static int u8len(const char *s, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) k++;
    return k;
}

static void render(const char *prompt, const char *buf, int len, int cur) {
    static char out[1024];
    int plen = visible_len(prompt);
    int avail = SCREEN_W - 1 - plen;
    if (avail < 10) avail = 10;
    int ccur = u8len(buf, cur), off = 0;
    for (int skip = ccur > avail ? ccur - avail : 0; skip > 0 && off < len; skip--) {   /* целыми буквами */
        off++;
        while (off < len && ((unsigned char)buf[off] & 0xC0) == 0x80) off++;
    }
    int show = 0;
    for (int k = 0; off + show < len && k < avail; k++) {
        show++;
        while (off + show < len && ((unsigned char)buf[off + show] & 0xC0) == 0x80) show++;
    }
    int n = snprintf(out, sizeof(out), "\r%s%.*s\x1b[K\r", prompt, show, buf + off);
    int col = plen + ccur - u8len(buf, off);
    if (col > 0) n += snprintf(out + n, sizeof(out) - n, "\x1b[%dC", col);
    write(STDOUT, out, n);
}

/* --- Tab: дополнение --- */
static char *cand[64];
static char  cand_pool[2048];
static bool  cand_dir[64];

static int collect(const char *dir, const char *prefix, bool only_bin) {
    int n = 0, used = 0, plen = strlen(prefix);
    static const char *builtins[] = { "cd", "pwd", "exit", "help", "history" };
    if (only_bin)
        for (int i = 0; i < 5; i++)
            if (!strncmp(builtins[i], prefix, plen)) { cand[n] = (char *)builtins[i]; cand_dir[n++] = false; }
    struct dirent_info e;
    for (int i = 0; n < 64 && dirread(dir, i, &e) == 1; i++) {
        if (strncmp(e.name, prefix, plen)) continue;
        int l = strlen(e.name) + 1;
        if (used + l > (int)sizeof(cand_pool)) break;
        cand[n] = strcpy(cand_pool + used, e.name);
        cand_dir[n++] = e.type == 2;
        used += l;
    }
    /* /ext/bin дополняет /bin, но не перекрывает одноимённую родную команду */
    if (only_bin) {
        for (int i = 0; n < 64 && dirread("/ext/bin", i, &e) == 1; i++) {
            if (strncmp(e.name, prefix, plen)) continue;
            bool seen = false;
            for (int k = 0; k < n; k++) if (!strcmp(cand[k], e.name)) { seen = true; break; }
            if (seen) continue;
            int l = strlen(e.name) + 1;
            if (used + l > (int)sizeof(cand_pool)) break;
            cand[n] = strcpy(cand_pool + used, e.name);
            cand_dir[n++] = false;
            used += l;
        }
    }
    return n;
}

static void insert(char *buf, int *len, int *cur, int max, const char *s, int n) {
    if (*len + n >= max) n = max - 1 - *len;
    if (n <= 0) return;
    for (int i = *len - *cur - 1; i >= 0; i--) buf[*cur + n + i] = buf[*cur + i];   /* сдвиг вправо — с конца */
    memcpy(buf + *cur, s, n);
    *len += n;
    *cur += n;
}

/* -> true, если нужно перерисовать приглашение заново (был выведен список) */
static bool complete(const char *prompt, char *buf, int *len, int *cur, int max, bool second_tab) {
    int ws = *cur;
    while (ws > 0 && buf[ws - 1] != ' ') ws--;
    int pv = ws - 1;                                           /* первое слово (или после |) = команда */
    while (pv >= 0 && buf[pv] == ' ') pv--;
    bool first = pv < 0 || buf[pv] == '|';

    char word[MAX_LINE];
    int wlen = *cur - ws;
    memcpy(word, buf + ws, wlen);
    word[wlen] = 0;

    char dir[128] = ".";
    const char *prefix = word;
    bool cmd = first && !strchr(word, '/');
    char *sl = strrchr(word, '/');
    if (sl) {
        int dl = sl - word + 1;
        memcpy(dir, word, dl);
        dir[dl] = 0;
        prefix = sl + 1;
    }
    int n = collect(cmd ? "/bin" : dir, prefix, cmd);
    if (n == 0) return false;

    int plen = strlen(prefix), common = strlen(cand[0]);
    for (int i = 1; i < n; i++) {
        int k = 0;
        while (k < common && cand[i][k] == cand[0][k]) k++;
        common = k;
    }
    if (common > plen) insert(buf, len, cur, max, cand[0] + plen, common - plen);
    if (n == 1) { insert(buf, len, cur, max, cand_dir[0] ? "/" : " ", 1); return false; }
    if (common > plen || !second_tab) return false;

    write(STDOUT, "\n", 1);                                    /* второй Tab — показать варианты */
    for (int i = 0; i < n; i++) printf("%s%s%s  ", cand_dir[i] ? ANSI_BLUE : "", cand[i], ANSI_RESET);
    write(STDOUT, "\n", 1);
    (void)prompt;
    return true;
}

/* -> длина строки, -1 = конец ввода (Ctrl+D на пустой строке) */
static int edit_line(const char *prompt, char *buf, int max) {
    int len = 0, cur = 0, hpos = hist_n;
    char draft[MAX_LINE];
    bool last_tab = false;
    buf[0] = 0;
    ttymode(TTY_RAW);
    render(prompt, buf, len, cur);
    for (;;) {
        int k = readkey();
        if (k == KEY_RESIZE) { render(prompt, buf, len, cur); continue; }
        bool was_tab = last_tab;
        last_tab = false;
        if (k == '\r' || k == '\n') break;
        if (k >= 32 && k < 127) { char ch = k; insert(buf, &len, &cur, max, &ch, 1); }
        else if (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS)) {   /* русская буква и т.п. */
            char u[4];
            insert(buf, &len, &cur, max, u, utf8_put(u, k));
        }
        else if (k == '\b' || k == 127) {
            if (cur > 0) {
                int st = cur - 1;
                while (st > 0 && ((unsigned char)buf[st] & 0xC0) == 0x80) st--;
                for (int i = 0; i < len - cur; i++) buf[st + i] = buf[cur + i];
                len -= cur - st; cur = st;
            }
        }
        else if (k == KEY_DEL || (k == CTRL('d') && len > 0)) {
            if (cur < len) {
                int e = cur + 1;
                while (e < len && ((unsigned char)buf[e] & 0xC0) == 0x80) e++;
                for (int i = 0; i < len - e; i++) buf[cur + i] = buf[e + i];
                len -= e - cur;
            }
        }
        else if (k == CTRL('d')) { ttymode(0); write(STDOUT, "\n", 1); return -1; }
        else if (k == KEY_LEFT)  { if (cur > 0) { cur--; while (cur > 0 && ((unsigned char)buf[cur] & 0xC0) == 0x80) cur--; } }
        else if (k == KEY_RIGHT) { if (cur < len) { cur++; while (cur < len && ((unsigned char)buf[cur] & 0xC0) == 0x80) cur++; } }
        else if (k == KEY_HOME || k == CTRL('a')) cur = 0;
        else if (k == KEY_END  || k == CTRL('e')) cur = len;
        else if (k == CTRL('u')) { for (int i = 0; i < len - cur; i++) buf[i] = buf[cur + i]; len -= cur; cur = 0; }
        else if (k == CTRL('k')) len = cur;
        else if (k == CTRL('w')) {
            int st = cur;
            while (st > 0 && buf[st - 1] == ' ') st--;
            while (st > 0 && buf[st - 1] != ' ') st--;
            for (int i = 0; i < len - cur; i++) buf[st + i] = buf[cur + i];
            len -= cur - st; cur = st;
        }
        else if (k == CTRL('l')) write(STDOUT, ANSI_CLEAR, strlen(ANSI_CLEAR));
        else if (k == CTRL('c')) { write(STDOUT, "^C\n", 3); len = cur = 0; }
        else if (k == KEY_UP || k == KEY_DOWN) {
            int oldest = hist_n > HISTORY ? hist_n - HISTORY : 0;
            if (hpos == hist_n) { buf[len] = 0; strcpy(draft, buf); }
            if (k == KEY_UP && hpos > oldest) hpos--;
            else if (k == KEY_DOWN && hpos < hist_n) hpos++;
            else continue;
            strcpy(buf, hpos == hist_n ? draft : history[hpos % HISTORY]);
            len = cur = strlen(buf);
        }
        else if (k == '\t') {
            buf[len] = 0;
            complete(prompt, buf, &len, &cur, max, was_tab);
            last_tab = true;
        }
        buf[len] = 0;
        render(prompt, buf, len, cur);
    }
    buf[len] = 0;
    ttymode(0);                       /* программы получат обычный терминал */
    write(STDOUT, "\n", 1);
    add_history(buf);
    return len;
}

/* ------------------------------------------------------------ встроенные */


/* Passwords bypass readline/history and are never echoed. */
static char username[24];
static int credential(const char *prompt,char *out,int max,bool secret) {
    printf("%s",prompt);int old=ttymode(TTY_RAW|TTY_NOSIG),n=0,overflow=0;
    for(;;) {
        char c;int r=read(STDIN,&c,1);
        if(r<=0 || c==3 || c==4) {ttymode(old);printf("\n");memset(out,0,max);return -1;}
        if(c=='\n'||c=='\r') break;
        if(c==8||c==127) {if(overflow) overflow--;else if(n) {n--;if(!secret) printf("\b \b");}continue;}
        if((unsigned char)c<32 || c==127) continue;
        if(n<max-1 && !overflow) {out[n++]=c;if(!secret) write(STDOUT,&c,1);} else overflow++;
    }
    ttymode(old);printf("\n");out[n]=0;
    if(overflow) {memset(out,0,max);printf("Input too long.\n");return -1;}return n;
}
static void clear_password(char *p,int n) {volatile char *v=p;while(n--) *v++=0;}
static int new_password(const char *name,int op) {
    char a[128]={0},b[128]={0};int r=-1;
    if(credential("New password: ",a,sizeof(a),true)>0 &&
       credential("Confirm password: ",b,sizeof(b),true)>0) {
        if(strcmp(a,b)) printf("Passwords do not match.\n");
        else {r=account(op,name,a);if(r<0) printf("Account: %s\n",strerror(-r));}
    } else printf("Password must not be empty.\n");
    clear_password(a,sizeof(a));clear_password(b,sizeof(b));return r;
}
static int login_prompt(void) {
    /* SSH and terminal children inherit a kernel-authenticated identity.
     * Never trust argv/environment to bypass login; logout clears this identity. */
    if(account(5,username,NULL)==0) return 0;
    int r=account(0,0,0);
    if(r<0) {printf("Cannot load accounts: %s\n",strerror(-r));return -1;}
    while(r==1) {
        printf("First boot: set the root password.\n");
        if(new_password("root",3)==0) break;
    }
    for(;;) {
        char pw[128]={0};
        if(credential("Login [root]: ",username,sizeof(username),false)<0) return -1;
        if(!username[0]) strcpy(username,"root");
        if(credential("Password: ",pw,sizeof(pw),true)<0) return -1;
        r=account(1,username,pw);clear_password(pw,sizeof(pw));
        if(!r) {hist_n=0;memset(history,0,sizeof(history));return 0;}
        printf("Login incorrect.\n");sleep_ms(1000);
    }
}

/* ---------- синтаксис POSIX, которого требуют рецепты Git ---------- */

extern char **environ;
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
char *getenv(const char *name);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { dprintf(STDERR, "sh: out of memory\n"); exit(1); }
    return p;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s ? s : "");
    char *p = xmalloc(n + 1);
    memcpy(p, s ? s : "", n + 1);
    return p;
}
static char *xstrndup(const char *s, int n) {
    char *p = xmalloc((size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

struct buf { char *s; int n, cap; };
static void binit(struct buf *b) { b->s = xmalloc(32); b->s[0] = 0; b->n = 0; b->cap = 32; }
static void bgrow(struct buf *b, int k) {
    if (b->n + k + 1 <= b->cap) return;
    while (b->n + k + 1 > b->cap) b->cap *= 2;
    b->s = realloc(b->s, (size_t)b->cap);
    if (!b->s) { dprintf(STDERR, "sh: out of memory\n"); exit(1); }
}
static void badd(struct buf *b, const char *p, int n) {
    if (n <= 0) return;
    bgrow(b, n); memcpy(b->s + b->n, p, (size_t)n); b->n += n; b->s[b->n] = 0;
}
static void bputc(struct buf *b, char c) { badd(b, &c, 1); }
static void bputs(struct buf *b, const char *s) { badd(b, s, (int)strlen(s)); }
static char *btake(struct buf *b) { char *s = b->s; b->s = NULL; return s; }

static int decode_wait(int st) { return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st); }

/* ---------- переменные ---------- */

struct var { char *name, *val; int exp; };
static struct var *vars;
static int nvars, vcap;

static struct var *var_find(const char *name) {
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) return &vars[i];
    return NULL;
}
static void var_set(const char *name, const char *val, int exp) {
    struct var *v = var_find(name);
    if (!v) {
        if (nvars == vcap) { vcap = vcap ? vcap * 2 : 16; vars = realloc(vars, (size_t)vcap * sizeof *vars); }
        if (!vars) { dprintf(STDERR, "sh: out of memory\n"); exit(1); }
        v = &vars[nvars++];
        v->name = xstrdup(name); v->val = NULL; v->exp = 0;
    }
    free(v->val);
    v->val = xstrdup(val ? val : "");
    if (exp) v->exp = 1;
    if (v->exp) setenv(name, v->val, 1);
}
static void var_unset(const char *name) {
    struct var *v = var_find(name);
    if (!v) { unsetenv(name); return; }
    free(v->name); free(v->val);
    *v = vars[--nvars];
    unsetenv(name);
}
static const char *var_get(const char *name) {
    struct var *v = var_find(name);
    return v ? v->val : NULL;
}

static char **pos;
static int npos;
static int in_func, flow, depth;
static int saw_subst, subst_status;

static void pos_free(void) {
    for (int i = 0; i < npos; i++) free(pos[i]);
    free(pos); pos = NULL; npos = 0;
}
static void pos_init(char **v, int n) {
    pos_free();
    pos = xmalloc((size_t)(n + 1) * sizeof *pos);
    for (int i = 0; i < n; i++) pos[i] = xstrdup(v[i] ? v[i] : "");
    npos = n;
}

static void import_env(void) {
    if (!environ) return;
    for (char **e = environ; *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char *name = xstrndup(*e, (int)(eq - *e));
        var_set(name, eq + 1, 1);
        free(name);
    }
}

/* ---------- дерево ---------- */

enum { N_LIST, N_AND, N_OR, N_PIPE, N_SIMPLE, N_IF, N_WHILE, N_FOR, N_CASE, N_BRACE, N_SUB, N_FUNC };
enum { RD_IN, RD_OUT, RD_APP, RD_DUP, RD_HERE };

struct redir { int fd, how, hq; char *target, *doc; };
struct arm { char **pat; int np; struct node *body; };
struct node {
    int kind, bg;
    struct node **k; int nk, kcap;
    char **w; int nw, wcap;
    struct redir *rd; int nrd, rdcap;
    struct arm *arms; int narms;
    char *name;
    struct node *els;
};

struct fn { char *name; struct node *body; };
static struct fn *fns;
static int nfns;

static struct node *newn(int kind) {
    struct node *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = kind;
    return n;
}
static void addk(struct node *n, struct node *c) {
    if (!c) return;
    if (n->nk == n->kcap) { n->kcap = n->kcap ? n->kcap * 2 : 4; n->k = realloc(n->k, (size_t)n->kcap * sizeof *n->k); }
    n->k[n->nk++] = c;
}
static void addw(struct node *n, const char *s) {
    if (n->nw == n->wcap) { n->wcap = n->wcap ? n->wcap * 2 : 4; n->w = realloc(n->w, (size_t)n->wcap * sizeof *n->w); }
    n->w[n->nw++] = xstrdup(s);
}
static void addr(struct node *n, struct redir r) {
    if (n->nrd == n->rdcap) { n->rdcap = n->rdcap ? n->rdcap * 2 : 4; n->rd = realloc(n->rd, (size_t)n->rdcap * sizeof *n->rd); }
    n->rd[n->nrd++] = r;
}
static void free_node(struct node *n) {
    if (!n) return;
    for (int i = 0; i < n->nk; i++) free_node(n->k[i]);
    free(n->k);
    for (int i = 0; i < n->nw; i++) free(n->w[i]);
    free(n->w);
    for (int i = 0; i < n->nrd; i++) { free(n->rd[i].target); free(n->rd[i].doc); }
    free(n->rd);
    for (int i = 0; i < n->narms; i++) {
        for (int j = 0; j < n->arms[i].np; j++) free(n->arms[i].pat[j]);
        free(n->arms[i].pat);
        free_node(n->arms[i].body);
    }
    free(n->arms);
    free_node(n->els);
    free(n->name);
    free(n);
}

/* ---------- шаблоны ---------- */

static int wild(const char *p, const char *s) {
    for (;;) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (wild(p, s)) return 1;
            return wild(p, s);
        }
        if (*p == '[') {
            int neg = 0, ok = 0;
            p++;
            if (*p == '!' || *p == '^') { neg = 1; p++; }
            if (*p == ']') { if (*s == ']') ok = 1; p++; }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if (*s && *s >= *p && *s <= p[2]) ok = 1;
                    p += 3;
                } else {
                    if (*s && *s == *p) ok = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if (!*s || ok == neg) return 0;
            s++;
            continue;
        }
        if (!*p) return !*s;
        if (*p == '?') { if (!*s) return 0; p++; s++; continue; }
        if (*p != *s) return 0;
        p++; s++;
    }
}

/* ---------- лексер ---------- */

enum {
    TK_EOF, TK_WORD, TK_NL, TK_SEMI, TK_AMP, TK_PIPE, TK_AND, TK_OR,
    TK_LP, TK_RP, TK_LB, TK_RB, TK_DSEMI, TK_REDIR
};
struct tok { int kind, fd, how, hq; char *s, *doc; };
static struct tok *toks;
static int ntoks, tcap, ti;
static int perr, hungry;
static const char *synmsg;

static void syn(const char *m) { if (!synmsg) synmsg = m; perr = 1; }
static void add_tok(struct tok t) {
    if (ntoks == tcap) { tcap = tcap ? tcap * 2 : 32; toks = realloc(toks, (size_t)tcap * sizeof *toks); }
    toks[ntoks++] = t;
}
static int peek(void) { return ti < ntoks ? toks[ti].kind : TK_EOF; }
static int is_word(const char *s) {
    return peek() == TK_WORD && toks[ti].s && !strcmp(toks[ti].s, s);
}

static const char *scan_balanced(const char *p, struct buf *b, char open, char close) {
    int d = 0, q = 0;
    while (*p) {
        char c = *p;
        bputc(b, c);
        if (q) {
            if (c == '\\' && q == '"' && p[1]) { p++; bputc(b, *p); }
            else if (c == q) q = 0;
        } else if (c == '\'' || c == '"') q = c;
        else if (c == open) d++;
        else if (c == close) { if (--d == 0) return p + 1; }
        p++;
    }
    hungry = 1;
    return p;
}

static const char *read_word(const char *p, struct buf *b, int *openq) {
    int q = 0;
    *openq = 0;
    while (*p) {
        char c = *p;
        if (!q && (c == ' ' || c == '\t' || c == '\n' || strchr("|&;<>(){}", c))) break;
        if (!q && c == '\\' && p[1] == '\n') { p += 2; continue; }
        if (c == '\\' && p[1] && (q != '\'')) { bputc(b, '\\'); bputc(b, p[1]); p += 2; continue; }
        if (!q && (c == '\'' || c == '"')) { q = c; bputc(b, c); p++; continue; }
        if (q && c == q) { q = 0; bputc(b, c); p++; continue; }
        if (c == '$' && p[1] == '{') {
            bputc(b, '$'); p++;
            p = scan_balanced(p, b, '{', '}');
            continue;
        }
        if (c == '$' && p[1] == '(') {
            bputc(b, '$'); p++;
            p = scan_balanced(p, b, '(', ')');
            continue;
        }
        if (c == '`') {
            bputc(b, c); p++;
            while (*p && *p != '`') {
                if (*p == '\\' && p[1]) { bputc(b, *p++); }
                bputc(b, *p++);
            }
            if (*p == '`') bputc(b, *p++);
            else hungry = 1;
            continue;
        }
        bputc(b, c);
        p++;
    }
    if (q) { hungry = 1; *openq = 1; }
    return p;
}

static char *here_body(const char **pp, const char *delim) {
    struct buf b; binit(&b);
    const char *p = *pp;
    int dl = (int)strlen(delim);
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len == dl && !memcmp(p, delim, (size_t)dl)) {
            *pp = nl ? nl + 1 : p + len;
            return btake(&b);
        }
        badd(&b, p, len);
        bputc(&b, '\n');
        if (!nl) { *pp = p + len; hungry = 1; return btake(&b); }
        p = nl + 1;
    }
    hungry = 1;
    *pp = p;
    return btake(&b);
}

static char *plain_delim(const char *w, int *hq) {
    struct buf b; binit(&b);
    *hq = 0;
    for (const char *p = w; *p; p++) {
        if (*p == '\'' || *p == '"') { *hq = 1; continue; }
        if (*p == '\\' && p[1]) { p++; bputc(&b, *p); continue; }
        bputc(&b, *p);
    }
    return btake(&b);
}

static void lex(const char *src) {
    for (int i = 0; i < ntoks; i++) { free(toks[i].s); free(toks[i].doc); }
    ntoks = 0; ti = 0; perr = 0; hungry = 0; synmsg = NULL;
    const char *p = src;
    int pend[32], npend = 0;
    while (*p) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
        if (*p == '#' ) { while (*p && *p != '\n') p++; continue; }
        if (*p == '\n') {
            struct tok t; memset(&t, 0, sizeof t); t.kind = TK_NL; add_tok(t);
            p++;
            for (int i = 0; i < npend; i++) {
                int hq = 0;
                char *d = plain_delim(toks[pend[i]].s, &hq);
                toks[pend[i]].hq = hq;
                toks[pend[i]].doc = here_body(&p, d);
                free(d);
            }
            npend = 0;
            continue;
        }
        struct tok t; memset(&t, 0, sizeof t); t.fd = -1;
        if (*p == '&' && p[1] == '&') { t.kind = TK_AND; p += 2; add_tok(t); continue; }
        if (*p == '|' && p[1] == '|') { t.kind = TK_OR; p += 2; add_tok(t); continue; }
        if (*p == ';' && p[1] == ';') { t.kind = TK_DSEMI; p += 2; add_tok(t); continue; }
        if (*p == '&') { t.kind = TK_AMP; p++; add_tok(t); continue; }
        if (*p == '|') { t.kind = TK_PIPE; p++; add_tok(t); continue; }
        if (*p == ';') { t.kind = TK_SEMI; p++; add_tok(t); continue; }
        if (*p == '(') { t.kind = TK_LP; p++; add_tok(t); continue; }
        if (*p == ')') { t.kind = TK_RP; p++; add_tok(t); continue; }
        if (*p == '{') { t.kind = TK_LB; p++; add_tok(t); continue; }
        if (*p == '}') { t.kind = TK_RB; p++; add_tok(t); continue; }
        int fd = -1;
        const char *dig = p;
        if (isdigit((unsigned char)*p) && (strchr(p, '>') || strchr(p, '<'))) {
            int v = 0; const char *q = p;
            while (isdigit((unsigned char)*q)) { v = v * 10 + *q - '0'; q++; }
            if (*q == '>' || *q == '<') { fd = v; p = q; }
            else dig = p;
            (void)dig;
        }
        if (*p == '<' || *p == '>') {
            t.kind = TK_REDIR;
            if (*p == '<' && p[1] == '<') {
                t.how = RD_HERE; p += 2;
                if (*p == '-') p++;
                t.fd = fd >= 0 ? fd : 0;
                while (*p == ' ' || *p == '\t') p++;
                struct buf b; binit(&b); int oq = 0;
                p = read_word(p, &b, &oq);
                t.s = btake(&b);
                add_tok(t);
                if (npend < 32) pend[npend++] = ntoks - 1;
                else syn("too many here-documents");
                continue;
            }
            if (*p == '>' && p[1] == '>') { t.how = RD_APP; p += 2; t.fd = fd >= 0 ? fd : 1; }
            else if (*p == '>' && p[1] == '&') { t.how = RD_DUP; p += 2; t.fd = fd >= 0 ? fd : 1; }
            else if (*p == '<' && p[1] == '&') { t.how = RD_DUP; p += 2; t.fd = fd >= 0 ? fd : 0; }
            else if (*p == '>') { t.how = RD_OUT; p++; t.fd = fd >= 0 ? fd : 1; }
            else { t.how = RD_IN; p++; t.fd = fd >= 0 ? fd : 0; }
            add_tok(t);
            continue;
        }
        struct buf b; binit(&b); int oq = 0;
        p = read_word(p, &b, &oq);
        t.kind = TK_WORD; t.s = btake(&b);
        if (!t.s[0] && oq) { free(t.s); continue; }
        add_tok(t);
    }
    if (npend) {
        hungry = 1;
        for (int i = 0; i < npend; i++) {
            int hq = 0; char *d = plain_delim(toks[pend[i]].s, &hq);
            toks[pend[i]].hq = hq;
            toks[pend[i]].doc = here_body(&p, d);
            free(d);
        }
    }
}

/* ---------- разбор ---------- */

static struct node *parse_list(void);
static struct node *parse_andor(void);
static struct node *parse_pipeline(void);
static struct node *parse_command(void);

static int list_end(void) {
    int k = peek();
    if (k == TK_EOF || k == TK_RP || k == TK_RB || k == TK_DSEMI) return 1;
    if (k == TK_WORD) {
        const char *s = toks[ti].s;
        if (!strcmp(s, "then") || !strcmp(s, "fi") || !strcmp(s, "do") || !strcmp(s, "done")
            || !strcmp(s, "else") || !strcmp(s, "elif") || !strcmp(s, "esac")) return 1;
    }
    return 0;
}
static int cmd_end(void) {
    int k = peek();
    if (k == TK_EOF || k == TK_SEMI || k == TK_AMP || k == TK_NL || k == TK_PIPE || k == TK_AND
        || k == TK_OR || k == TK_RP || k == TK_RB || k == TK_DSEMI) return 1;
    if (k == TK_WORD) {
        const char *s = toks[ti].s;
        if (!strcmp(s, "then") || !strcmp(s, "fi") || !strcmp(s, "do") || !strcmp(s, "done")
            || !strcmp(s, "else") || !strcmp(s, "elif") || !strcmp(s, "esac")) return 1;
    }
    return 0;
}
static void skip_nl(void) { while (peek() == TK_NL) ti++; }
static int want(int k, const char *what) {
    if (peek() == k) { ti++; return 1; }
    if (peek() == TK_EOF) hungry = 1;
    syn(what);
    return 0;
}
static int want_word(const char *s) {
    skip_nl();
    if (is_word(s)) { ti++; return 1; }
    if (peek() == TK_EOF) hungry = 1;
    syn(s);
    return 0;
}
static void take_redir(struct node *n) {
    struct tok *t = &toks[ti++];
    struct redir r; memset(&r, 0, sizeof r);
    r.fd = t->fd; r.how = t->how; r.hq = t->hq;
    if (t->doc) r.doc = xstrdup(t->doc);
    if (t->how != RD_HERE) {
        if (peek() != TK_WORD) { syn("redirect"); return; }
        r.target = xstrdup(toks[ti].s);
        ti++;
    }
    addr(n, r);
}

static struct node *parse_simple(void) {
    if (cmd_end() || peek() == TK_LB || peek() == TK_LP) return NULL;
    struct node *n = newn(N_SIMPLE);
    while (!cmd_end()) {
        if (peek() == TK_REDIR) { take_redir(n); continue; }
        if (peek() == TK_WORD) { addw(n, toks[ti].s); ti++; continue; }
        break;
    }
    if (!n->nw && !n->nrd) { free_node(n); return NULL; }
    return n;
}
static struct node *parse_brace(void) {
    ti++;
    struct node *n = newn(N_BRACE);
    addk(n, parse_list());
    want(TK_RB, "}");
    return n;
}
static struct node *parse_sub(void) {
    ti++;
    struct node *n = newn(N_SUB);
    addk(n, parse_list());
    want(TK_RP, ")");
    return n;
}
static struct node *parse_if(void) {
    ti++;
    struct node *n = newn(N_IF);
    struct node *tail = n;
    n->kcap = 2; n->k = xmalloc(2 * sizeof *n->k); n->nk = 0;
    addk(n, parse_list());
    if (!want_word("then")) return n;
    addk(n, parse_list());
    while (is_word("elif")) {
        ti++;
        struct node *e = newn(N_IF);
        e->kcap = 2; e->k = xmalloc(2 * sizeof *e->k);
        addk(e, parse_list());
        want_word("then");
        addk(e, parse_list());
        tail->els = e;
        tail = e;
    }
    if (is_word("else")) { ti++; tail->els = parse_list(); }
    want_word("fi");
    return n;
}
static struct node *parse_while(void) {
    ti++;
    struct node *n = newn(N_WHILE);
    addk(n, parse_list());
    want_word("do");
    addk(n, parse_list());
    want_word("done");
    return n;
}
static struct node *parse_for(void) {
    ti++;
    struct node *n = newn(N_FOR);
    if (peek() != TK_WORD) { syn("for"); return n; }
    n->name = xstrdup(toks[ti].s); ti++;
    skip_nl();
    if (is_word("in")) {
        ti++;
        while (peek() == TK_WORD) { addw(n, toks[ti].s); ti++; }
        if (peek() == TK_SEMI || peek() == TK_NL) ti++;
    } else n->narms = -1; /* нет in: перебор "$@" */
    skip_nl();
    want_word("do");
    addk(n, parse_list());
    want_word("done");
    return n;
}
static struct node *parse_case(void) {
    ti++;
    struct node *n = newn(N_CASE);
    if (peek() != TK_WORD) { syn("case"); return n; }
    addw(n, toks[ti].s); ti++;
    skip_nl();
    want_word("in");
    skip_nl();
    while (!is_word("esac") && peek() != TK_EOF) {
        if (peek() == TK_LP) ti++;
        struct arm a; memset(&a, 0, sizeof a);
        while (peek() == TK_WORD || peek() == TK_PIPE) {
            if (peek() == TK_PIPE) { ti++; continue; }
            if (a.np % 4 == 0) a.pat = realloc(a.pat, (size_t)(a.np + 4) * sizeof *a.pat);
            a.pat[a.np++] = xstrdup(toks[ti].s);
            ti++;
        }
        if (!want(TK_RP, ")")) { free(a.pat); break; }
        a.body = parse_list();
        if (peek() == TK_DSEMI) ti++;
        skip_nl();
        if (n->narms % 4 == 0) n->arms = realloc(n->arms, (size_t)(n->narms + 4) * sizeof *n->arms);
        n->arms[n->narms++] = a;
    }
    want_word("esac");
    return n;
}
static struct node *parse_command(void) {
    struct node *n = NULL;
    skip_nl();
    if (is_word("if")) n = parse_if();
    else if (is_word("while")) n = parse_while();
    else if (is_word("for")) n = parse_for();
    else if (is_word("case")) n = parse_case();
    else if (peek() == TK_LB) n = parse_brace();
    else if (peek() == TK_WORD && ti + 2 < ntoks && toks[ti + 1].kind == TK_LP && toks[ti + 2].kind == TK_RP) {
        n = newn(N_FUNC);
        n->name = xstrdup(toks[ti].s);
        ti += 3;
        skip_nl();
        addk(n, parse_command());
    } else if (peek() == TK_LP) n = parse_sub();
    else n = parse_simple();
    if (!n) return NULL;
    while (peek() == TK_REDIR) take_redir(n);
    return n;
}
static struct node *parse_pipeline(void) {
    struct node *n = parse_command();
    if (!n || peek() != TK_PIPE) return n;
    struct node *p = newn(N_PIPE);
    addk(p, n);
    while (peek() == TK_PIPE) {
        ti++;
        skip_nl();
        addk(p, parse_command());
    }
    return p;
}
static struct node *parse_andor(void) {
    struct node *n = parse_pipeline();
    if (!n) return NULL;
    while (peek() == TK_AND || peek() == TK_OR) {
        int or = peek() == TK_OR; ti++;
        skip_nl();
        struct node *r = parse_pipeline();
        struct node *a = newn(or ? N_OR : N_AND);
        addk(a, n); addk(a, r ? r : newn(N_LIST));
        n = a;
    }
    return n;
}
static struct node *parse_list(void) {
    struct node *n = newn(N_LIST);
    skip_nl();
    while (!list_end()) {
        struct node *c = parse_andor();
        if (!c) break;
        int sep = 0;
        if (peek() == TK_AMP) { c->bg = 1; ti++; sep = 1; }
        else if (peek() == TK_SEMI) { ti++; sep = 1; }
        addk(n, c);
        if (c->bg) sep = 1;
        while (peek() == TK_NL) { ti++; sep = 1; }
        if (list_end()) break;
        if (!sep && peek() != TK_EOF) break;
    }
    return n;
}

/* ---------- раскрытие ---------- */

struct fields { char **v; int n, cap; int *glob; };
static void fadd(struct fields *f, char *s, int g) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->v = realloc(f->v, (size_t)f->cap * sizeof *f->v);
        f->glob = realloc(f->glob, (size_t)f->cap * sizeof *f->glob);
    }
    f->v[f->n] = s; f->glob[f->n] = g; f->n++;
}
static void ffree(struct fields *f) {
    for (int i = 0; i < f->n; i++) free(f->v[i]);
    free(f->v); free(f->glob);
}
struct ebuf { char *s; unsigned char *q; int n, cap, glob, any; };
static void einit(struct ebuf *e) { memset(e, 0, sizeof *e); e->s = xmalloc(16); e->s[0] = 0; e->q = xmalloc(16); e->cap = 16; }
static void efree(struct ebuf *e) { free(e->s); free(e->q); }
static void eputc(struct ebuf *e, char c, int quoted) {
    if (e->n + 2 > e->cap) {
        e->cap *= 2; e->s = realloc(e->s, (size_t)e->cap); e->q = realloc(e->q, (size_t)e->cap);
    }
    e->s[e->n] = c; e->q[e->n] = (unsigned char)quoted; e->n++; e->s[e->n] = 0;
    e->any = 1;
    if (!quoted && (c == '*' || c == '?' || c == '[')) e->glob = 1;
}
static void eputs(struct ebuf *e, const char *s, int quoted) { while (*s) eputc(e, *s++, quoted); }

static int is_ifs(char c) {
    const char *ifs = var_get("IFS");
    if (!ifs) return c == ' ' || c == '\t' || c == '\n';
    return strchr(ifs, c) != NULL;
}
static const char *special_param(int c, char *tmp, int tmpsz) {
    if (c == '?') { snprintf(tmp, (size_t)tmpsz, "%d", last_status); return tmp; }
    if (c == '$') { snprintf(tmp, (size_t)tmpsz, "%d", getpid()); return tmp; }
    if (c == '#') { snprintf(tmp, (size_t)tmpsz, "%d", npos > 0 ? npos - 1 : 0); return tmp; }
    if (c == '0') return npos > 0 ? pos[0] : "sh";
    if (c >= '1' && c <= '9') {
        int i = c - '0';
        return i < npos ? pos[i] : "";
    }
    return NULL;
}

static char *cmdsub(const char *script, int len);
static int eval_arith(const char **pp);
static const char *expand_span(const char *p, const char *end, struct ebuf *e, int quoted, struct fields *out);

static char *affix(const char *val, const char *pat, int suffix, int longest) {
    int n = (int)strlen(val);
    if (!suffix) {
        int from = longest ? n : 0, to = longest ? -1 : n + 1, step = longest ? -1 : 1;
        for (int i = from; i != to; i += step) {
            char *pre = xstrndup(val, i);
            int ok = wild(pat, pre);
            free(pre);
            if (ok) return xstrdup(val + i);
        }
    } else {
        int from = longest ? 0 : n, to = longest ? n + 1 : -1, step = longest ? 1 : -1;
        for (int i = from; i != to; i += step) {
            if (wild(pat, val + i)) return xstrndup(val, i);
        }
    }
    return xstrdup(val);
}

static const char *expand_dollar(const char *p, const char *end, struct ebuf *e, int quoted, struct fields *out) {
    /* p указывает на '$' */
    if (p + 1 >= end) { eputc(e, '$', quoted); return p + 1; }
    p++;
    if (*p == '(' && p + 1 < end && p[1] == '(') {
        p += 2;
        const char *s = p; int d = 1;
        while (p < end && *p && d) {
            if (*p == '(') d++;
            else if (*p == ')') d--;
            if (d) p++;
        }
        char *expr = xstrndup(s, (int)(p - s));
        const char *ep = expr;
        int v = eval_arith(&ep);
        char tmp[32]; snprintf(tmp, sizeof tmp, "%d", v);
        eputs(e, tmp, quoted);
        free(expr);
        if (p < end && *p == ')') p++;
        if (p < end && *p == ')') p++;
        return p;
    }
    if (*p == '(') {
        p++;
        const char *s = p; int d = 1, q = 0;
        while (p < end && *p && d) {
            if (q) { if (*p == q) q = 0; else if (*p == '\\' && p[1]) p++; }
            else if (*p == '\'' || *p == '"') q = *p;
            else if (*p == '(') d++;
            else if (*p == ')') { if (--d == 0) break; }
            p++;
        }
        char *out = cmdsub(s, (int)(p - s));
        eputs(e, out ? out : "", quoted);
        free(out);
        saw_subst = 1;
        if (p < end && *p == ')') p++;
        return p;
    }
    if (*p != '{') {
        char tmp[32];
        if (*p == '@' || *p == '*') {
            int join = quoted && *p == '*';
            if (quoted && *p == '@' && out) {
                if (e->any || e->n) {
                    char *s = xstrndup(e->s, e->n);
                    fadd(out, s, e->glob);
                    e->n = 0; e->s[0] = 0; e->glob = 0; e->any = 0;
                }
                for (int i = 1; i < npos; i++) fadd(out, xstrdup(pos[i]), 0);
                p++;
                return p;
            }
            struct buf b; binit(&b);
            for (int i = 1; i < npos; i++) { if (i > 1) bputc(&b, ' '); bputs(&b, pos[i]); }
            eputs(e, b.s, quoted || join);
            free(b.s);
            p++;
            return p;
        }
        const char *sp = special_param((unsigned char)*p, tmp, (int)sizeof tmp);
        if (sp) { eputs(e, sp, quoted); return p + 1; }
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
            char *name = xstrndup(s, (int)(p - s));
            const char *v = var_get(name);
            eputs(e, v ? v : "", quoted);
            free(name);
            return p;
        }
        eputc(e, '$', quoted);
        return p;
    }
    p++;
    if (*p == '#') {
        const char *s = p + 1;
        const char *q = s;
        while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
        if (q < end && *q == '}' && q > s) {
            char *name = xstrndup(s, (int)(q - s));
            const char *v = var_get(name);
            char tmp[32]; snprintf(tmp, sizeof tmp, "%d", v ? (int)strlen(v) : 0);
            eputs(e, tmp, quoted);
            free(name);
            return q + 1;
        }
    }
    const char *ns = p;
    if (*p == '@' || *p == '*' || *p == '?' || *p == '#' || *p == '$' || *p == '!' || isdigit((unsigned char)*p)) p++;
    else while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
    char *name = xstrndup(ns, (int)(p - ns));
    int op = 0, longest = 0; /* 1 :-, 2 #, 3 % */
    if (p + 1 < end && *p == ':' && p[1] == '-') { op = 1; p += 2; }
    else if (*p == '#') { op = 2; p++; if (*p == '#') { longest = 1; p++; } }
    else if (*p == '%') { op = 3; p++; if (*p == '%') { longest = 1; p++; } }
    const char *ws = p; int d = 0;
    while (p < end && *p && !(d == 0 && *p == '}')) {
        if (*p == '{') d++;
        else if (*p == '}') d--;
        p++;
    }
    char *word = xstrndup(ws, (int)(p - ws));
    if (p < end && *p == '}') p++;
    const char *val = NULL; char tmp[32];
    if (!strcmp(name, "@") || !strcmp(name, "*")) val = NULL;
    else if (name[0] && !name[1] && special_param((unsigned char)name[0], tmp, (int)sizeof tmp)) val = special_param((unsigned char)name[0], tmp, (int)sizeof tmp);
    else if (isdigit((unsigned char)name[0]) && !name[1]) val = special_param((unsigned char)name[0], tmp, (int)sizeof tmp);
    else val = var_get(name);
    int empty = !val || !val[0];
    if (op == 1 && empty) {
        struct ebuf w; einit(&w);
        expand_span(word, word + strlen(word), &w, 1, NULL);
        eputs(e, w.s, quoted);
        efree(&w);
    } else if (op == 1) {
        eputs(e, val, quoted);
    } else if ((op == 2 || op == 3) && val) {
        struct ebuf w; einit(&w);
        expand_span(word, word + strlen(word), &w, 1, NULL);
        char *cut = affix(val, w.s, op == 3, longest);
        eputs(e, cut, quoted);
        free(cut); efree(&w);
    } else eputs(e, val ? val : "", quoted);
    free(name); free(word);
    return p;
}

/* Арифметика $((...)): числа, скобки, + - * / % и имена переменных. */
static int arith_factor(const char **pp);
static int arith_term(const char **pp);
static int arith_expr(const char **pp);
static void askip(const char **pp) { while (**pp == ' ' || **pp == '\t') (*pp)++; }
static int arith_factor(const char **pp) {
    askip(pp);
    if (**pp == '+') { (*pp)++; return arith_factor(pp); }
    if (**pp == '-') { (*pp)++; return -arith_factor(pp); }
    if (**pp == '(') { (*pp)++; int v = arith_expr(pp); askip(pp); if (**pp == ')') (*pp)++; return v; }
    if (**pp == '$') (*pp)++;
    if (isdigit((unsigned char)**pp)) {
        int v = 0; while (isdigit((unsigned char)**pp)) { v = v * 10 + **pp - '0'; (*pp)++; }
        return v;
    }
    if (isalpha((unsigned char)**pp) || **pp == '_') {
        char name[64]; int n = 0;
        while ((isalnum((unsigned char)**pp) || **pp == '_') && n < 63) name[n++] = *(*pp)++;
        name[n] = 0;
        const char *v = var_get(name);
        return v ? atoi(v) : 0;
    }
    return 0;
}
static int arith_term(const char **pp) {
    int v = arith_factor(pp);
    for (;;) {
        askip(pp);
        if (**pp == '*') { (*pp)++; v *= arith_factor(pp); }
        else if (**pp == '/') { (*pp)++; int d = arith_factor(pp); v = d ? v / d : 0; }
        else if (**pp == '%') { (*pp)++; int d = arith_factor(pp); v = d ? v % d : 0; }
        else break;
    }
    return v;
}
static int arith_expr(const char **pp) {
    int v = arith_term(pp);
    for (;;) {
        askip(pp);
        if (**pp == '+') { (*pp)++; v += arith_term(pp); }
        else if (**pp == '-') { (*pp)++; v -= arith_term(pp); }
        else break;
    }
    return v;
}
static int eval_arith(const char **pp) { return arith_expr(pp); }

static const char *expand_span(const char *p, const char *end, struct ebuf *e, int quoted, struct fields *out) {
    while (p < end && *p) {
        if (!quoted && *p == '\'') {
            e->any = 1; p++;
            while (p < end && *p && *p != '\'') eputc(e, *p++, 1);
            if (p < end && *p == '\'') p++;
            continue;
        }
        if (!quoted && *p == '"') {
            e->any = 1; p++;
            p = expand_span(p, end, e, 1, out);
            if (p < end && *p == '"') p++;
            continue;
        }
        if (quoted && *p == '"') return p;
        if (*p == '\\') {
            p++;
            if (p >= end || !*p) break;
            if (!quoted || *p == '$' || *p == '`' || *p == '"' || *p == '\\' || *p == '\n') {
                if (*p != '\n') eputc(e, *p, 1);
                p++;
            } else { eputc(e, '\\', 1); eputc(e, *p, 1); p++; }
            continue;
        }
        if (*p == '$') { p = expand_dollar(p, end, e, quoted, out); continue; }
        if (*p == '`') {
            p++;
            const char *s = p;
            struct buf raw; binit(&raw);
            while (p < end && *p && *p != '`') {
                /* В обратных кавычках «\» снимается только перед $ ` \ и переводом строки. */
                if (*p == '\\' && p + 1 < end) {
                    char nch = p[1];
                    if (nch == '$' || nch == '`' || nch == '\\' || nch == '\n') {
                        p++;
                        if (nch != '\n') bputc(&raw, nch);
                        p++;
                        continue;
                    }
                }
                bputc(&raw, *p++);
            }
            char *got = cmdsub(raw.s, raw.n);
            eputs(e, got ? got : "", quoted);
            free(got); free(raw.s);
            saw_subst = 1;
            if (p < end && *p == '`') p++;
            (void)s;
            continue;
        }
        eputc(e, *p, quoted);
        p++;
    }
    return p;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void glob_add(struct fields *f, char *word, int doglob) {
    if (!doglob || (!strchr(word, '*') && !strchr(word, '?') && !strchr(word, '['))) {
        fadd(f, word, 0);
        return;
    }
    const char *slash = strrchr(word, '/');
    char dir[256];
    const char *pat = word;
    int dlen = 0;
    if (slash) {
        dlen = (int)(slash - word + 1);
        if (dlen >= (int)sizeof dir) { fadd(f, word, 0); return; }
        memcpy(dir, word, (size_t)dlen);
        dir[dlen] = 0;
        pat = slash + 1;
    } else strcpy(dir, ".");
    if (strchr(dir, '*') || strchr(dir, '?')) { fadd(f, word, 0); return; }
    char **hit = NULL; int nh = 0, hc = 0;
    struct dirent_info e;
    for (int i = 0; dirread(dir, i, &e) == 1; i++) {
        if (e.name[0] == '.' && pat[0] != '.') continue;
        if (!wild(pat, e.name)) continue;
        if (nh == hc) { hc = hc ? hc * 2 : 8; hit = realloc(hit, (size_t)hc * sizeof *hit); }
        struct buf b; binit(&b);
        if (slash) badd(&b, word, dlen);
        bputs(&b, e.name);
        hit[nh++] = btake(&b);
    }
    if (!nh) { fadd(f, word, 0); free(hit); return; }
    qsort(hit, (size_t)nh, sizeof *hit, cmpstr);
    for (int i = 0; i < nh; i++) fadd(f, hit[i], 0);
    free(hit);
    free(word);
}

static int is_assign(const char *w) {
    if (!w || (!isalpha((unsigned char)*w) && *w != '_')) return 0;
    const char *p = w;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    return *p == '=';
}

static void expand_one(const char *raw, struct fields *out, int nosplit) {
    struct ebuf e; einit(&e);
    const char *end = raw + strlen(raw);
    expand_span(raw, end, &e, 0, nosplit ? NULL : out);
    if (nosplit) {
        fadd(out, xstrdup(e.s), 0);
        efree(&e);
        return;
    }
    int i = 0;
    while (i < e.n) {
        if (!e.q[i] && is_ifs(e.s[i])) { i++; continue; }
        int st = i, g = 0;
        while (i < e.n && !(!e.q[i] && is_ifs(e.s[i]))) { if (e.q[i] == 0 && (e.s[i] == '*' || e.s[i] == '?' || e.s[i] == '[')) g = 1; i++; }
        char *s = xstrndup(e.s + st, i - st);
        glob_add(out, s, g);
    }
    if (!e.n && e.any) fadd(out, xstrdup(""), 0);
    efree(&e);
}

static char *expand_str(const char *raw) {
    struct fields f; memset(&f, 0, sizeof f);
    expand_one(raw ? raw : "", &f, 1);
    char *s = f.n ? f.v[0] : xstrdup("");
    if (f.n) f.v[0] = NULL;
    ffree(&f);
    return s;
}

static void eval(struct node *n);
static int eval_source(const char *text, int allow_more);

static char *cmdsub(const char *script, int len) {
    char *text = xstrndup(script ? script : "", len);
    int fds[2];
    if (pipe(fds) < 0) { free(text); subst_status = 1; return xstrdup(""); }
    int pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); free(text); subst_status = 1; return xstrdup(""); }
    if (pid == 0) {
        dup2(fds[1], STDOUT);
        close(fds[0]); close(fds[1]);
        eval_source(text, 0);
        exit(last_status);
    }
    close(fds[1]);
    struct buf b; binit(&b);
    char tmp[1024]; int n;
    while ((n = read(fds[0], tmp, sizeof tmp)) > 0) badd(&b, tmp, n);
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    subst_status = decode_wait(st);
    while (b.n && b.s[b.n - 1] == '\n') b.s[--b.n] = 0;
    free(text);
    return btake(&b);
}

struct fdsave { int fd, spare, ok; };
static struct fdsave saves[40];
static int nsaves, here_seq;

static int apply_redir(struct redir *r) {
    if (r->how == RD_HERE) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/sh-%d-%d", getpid(), here_seq++);
        int w = open(path, O_WRITE, 0666);
        if (w < 0) { dprintf(STDERR, "sh: here-document: %s\n", strerror(errno)); return -1; }
        char *body = NULL;
        const char *doc = r->doc ? r->doc : "";
        if (!r->hq) { body = expand_str(doc); doc = body; }
        if (doc[0]) write(w, doc, strlen(doc));
        close(w);
        free(body);
        int rd = open(path, O_READ);
        unlink(path);
        if (rd < 0) return -1;
        if (rd != r->fd) { dup2(rd, r->fd); close(rd); }
        return 0;
    }
    char *target = r->target ? expand_str(r->target) : xstrdup("");
    int rc = 0;
    if (r->how == RD_DUP) {
        if (target[0] == '-' && !target[1]) close(r->fd);
        else if (dup2(atoi(target), r->fd) < 0) { dprintf(STDERR, "sh: dup: %s\n", target); rc = -1; }
    } else {
        int flags = r->how == RD_IN ? O_READ
            : r->how == RD_APP ? (O_WRONLY | O_CREAT | O_APPEND)
            : O_WRITE;
        int fd = open(target, flags, 0666);
        if (fd < 0) { dprintf(STDERR, "sh: %s: %s\n", target, strerror(errno)); rc = -1; }
        else {
            if (fd != r->fd) { dup2(fd, r->fd); close(fd); }
        }
    }
    free(target);
    return rc;
}
static int push_redirs(struct redir *r, int n) {
    int mark = nsaves;
    for (int i = 0; i < n; i++) {
        /* spare через F_DUPFD: раньше 50+nsaves при MAX_FDS=16/32 всегда
         * промахивался, pop закрывал stdout без восстановления. */
        int spare = fcntl(r[i].fd, F_DUPFD, 3);
        int ok = spare >= 0;
        if (nsaves < 40) { saves[nsaves].fd = r[i].fd; saves[nsaves].spare = spare; saves[nsaves].ok = ok; nsaves++; }
        if (apply_redir(&r[i]) < 0) {
            while (nsaves > mark) {
                nsaves--;
                if (saves[nsaves].ok) { dup2(saves[nsaves].spare, saves[nsaves].fd); close(saves[nsaves].spare); }
                else close(saves[nsaves].fd);
            }
            return -1;
        }
    }
    return mark;
}
static void pop_redirs(int mark) {
    while (nsaves > mark) {
        nsaves--;
        if (saves[nsaves].ok) { dup2(saves[nsaves].spare, saves[nsaves].fd); close(saves[nsaves].spare); }
        else close(saves[nsaves].fd);
    }
}

static int test_unary(const char *op, const char *path) {
    if (!strcmp(op, "-z")) return path[0] == 0;
    if (!strcmp(op, "-n")) return path[0] != 0;
    struct stat st;
    int ok = stat(path, &st) == 0;
    if (!strcmp(op, "-e") || !strcmp(op, "-r") || !strcmp(op, "-w") || !strcmp(op, "-x")) return ok;
    if (!strcmp(op, "-f")) return ok && S_ISREG(st.st_mode);
    if (!strcmp(op, "-d")) return ok && S_ISDIR(st.st_mode);
    if (!strcmp(op, "-s")) return ok && st.st_size > 0;
    if (!strcmp(op, "-L")) { struct stat ls; return lstat(path, &ls) == 0 && S_ISLNK(ls.st_mode); }
    return -1;
}
static int test_bin(const char *a, const char *op, const char *b) {
    if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
    if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
    if (!strcmp(op, "-ef")) {
        struct stat sa, sb;
        if (stat(a, &sa) || stat(b, &sb)) return 0;
        return sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev;
    }
    if (op[0] == '-' && ( !strcmp(op, "-eq") || !strcmp(op, "-ne") || !strcmp(op, "-gt")
            || !strcmp(op, "-lt") || !strcmp(op, "-ge") || !strcmp(op, "-le"))) {
        int x = atoi(a), y = atoi(b);
        if (!strcmp(op, "-eq")) return x == y;
        if (!strcmp(op, "-ne")) return x != y;
        if (!strcmp(op, "-gt")) return x > y;
        if (!strcmp(op, "-lt")) return x < y;
        if (!strcmp(op, "-ge")) return x >= y;
        return x <= y;
    }
    return -1;
}
static int test_expr(char **av, int n) {
    if (n <= 0) return 1;
    if (!strcmp(av[0], "!")) { int r = test_expr(av + 1, n - 1); return r == 2 ? 2 : !r; }
    if (n == 1) return av[0][0] ? 0 : 1;
    if (n == 2 && av[0][0] == '-') { int r = test_unary(av[0], av[1]); return r < 0 ? 2 : !r; }
    if (n == 3) { int r = test_bin(av[0], av[1], av[2]); return r < 0 ? 2 : !r; }
    return 2;
}

static void launch_external(char **argv) {
    if (strchr(argv[0], '/')) {
        int r = exec(argv[0], argv);
        dprintf(STDERR, "sh: %s: %s\n", argv[0], strerror(-r));
        exit(127);
    }
    const char *path = var_get("PATH");
    if (!path) path = "/bin:/ext/bin";
    char *copy = xstrdup(path);
    int last = -ENOENT;
    for (char *p = copy; *p || p == copy; ) {
        char *c = strchr(p, ':');
        if (c) *c = 0;
        char buf[512];
        snprintf(buf, sizeof buf, "%s/%s", p[0] ? p : ".", argv[0]);
        int r = exec(buf, argv);
        last = r;
        if (r != -ENOENT) break;
        if (!c) break;
        p = c + 1;
        if (!*p && !c[1]) break;
    }
    free(copy);
    struct dirent_info st;
    if (last == -ENOENT && dirstat(argv[0], &st) == 0 && st.type == 1)
        dprintf(STDERR, "sh: %s: command not found (the file is here - run it as ./%s)\n", argv[0], argv[0]);
    else if (last == -ENOENT) dprintf(STDERR, "sh: %s: command not found\n", argv[0]);
    else dprintf(STDERR, "sh: %s: %s\n", argv[0], strerror(-last));
    exit(last == -ENOENT ? 127 : 126);
}

static struct fn *fn_find(const char *name) {
    for (int i = 0; i < nfns; i++) if (!strcmp(fns[i].name, name)) return &fns[i];
    return NULL;
}

static int builtin(char **av, int ac);
static void help(void);

static int wait_fg(int pid) {
    setpgid(pid, pid);
    setfg(pid);
    int st = 0;
    if (waitpid(pid, &st, 0) > 0) last_status = decode_wait(st);
    else last_status = 1;
    setfg(0);
    return last_status;
}

static void run_external(char **argv, struct redir *rd, int nrd, char **asg, int nasg) {
    int pid = fork();
    if (pid < 0) { dprintf(STDERR, "sh: fork failed\n"); last_status = 1; return; }
    if (pid == 0) {
        setpgid(0, 0);
        for (int i = 0; i < nasg; i++) {
            char *eq = strchr(asg[i], '=');
            if (!eq) continue;
            *eq = 0;
            char *val = expand_str(eq + 1);
            setenv(asg[i], val, 1);
            free(val);
            *eq = '=';
        }
        if (nrd && push_redirs(rd, nrd) < 0) exit(1);
        launch_external(argv);
    }
    wait_fg(pid);
}

static void call_func(struct fn *f, char **av, int ac) {
    char **save = pos; int sn = npos;
    pos = xmalloc((size_t)(ac > 0 ? ac : 1) * sizeof *pos);
    pos[0] = xstrdup(sn > 0 ? save[0] : "sh");
    for (int i = 1; i < ac; i++) pos[i] = xstrdup(av[i]);
    npos = ac > 0 ? ac : 1;
    in_func++;
    if (f->body) eval(f->body);
    in_func--;
    if (flow == 3) flow = 0;
    for (int i = 0; i < npos; i++) free(pos[i]);
    free(pos);
    pos = save; npos = sn;
}

static int do_printf(char **av, int ac) {
    if (ac < 2) return 2;
    const char *fmt = av[1];
    int ai = 2, conv = 0;
    for (const char *q = fmt; *q; q++) if (*q == '%' && q[1] && q[1] != '%') conv = 1;
    do {
        int used = 0;
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') {
                p++;
                char c = *p;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c == 'r') c = '\r';
                else if (c == '0') {
                    int v = 0;
                    while (p[1] >= '0' && p[1] <= '7') { p++; v = v * 8 + (*p - '0'); }
                    c = (char)v;
                }
                if (c) write(STDOUT, &c, 1);
            } else if (*p == '%' && p[1] == '%') { p++; write(STDOUT, "%", 1); }
            else if (*p == '%' && (p[1] == 's' || p[1] == 'd' || p[1] == 'i')) {
                const char *arg = ai < ac ? av[ai++] : "";
                used = 1;
                if (p[1] == 's') write(STDOUT, arg, strlen(arg));
                else { char tmp[32]; snprintf(tmp, sizeof tmp, "%d", atoi(arg)); write(STDOUT, tmp, strlen(tmp)); }
                p++;
            } else write(STDOUT, p, 1);
        }
        if (!conv || !used) break;
    } while (ai < ac);
    return 0;
}

static int builtin(char **av, int ac) {
    const char *n = av[0];
    if (!strcmp(n, ":") || !strcmp(n, "true")) return 0;
    if (!strcmp(n, "false")) return 1;
    if (!strcmp(n, "echo")) {
        int i = 1, nl = 1;
        if (i < ac && !strcmp(av[i], "-n")) { nl = 0; i++; }
        int first = 1;
        for (; i < ac; i++) {
            if (!first) write(STDOUT, " ", 1);
            first = 0;
            write(STDOUT, av[i], strlen(av[i]));
        }
        if (nl) write(STDOUT, "\n", 1);
        return 0;
    }
    if (!strcmp(n, "printf")) return do_printf(av, ac);
    if (!strcmp(n, "test") || !strcmp(n, "[")) {
        int from = 1, to = ac;
        if (!strcmp(n, "[")) {
            if (ac < 2 || strcmp(av[ac - 1], "]")) { dprintf(STDERR, "sh: [: missing ]\n"); return 2; }
            to = ac - 1;
        }
        return test_expr(av + from, to - from);
    }
    if (!strcmp(n, "exit")) exit(ac > 1 ? atoi(av[1]) : last_status);
    if (!strcmp(n, "return")) {
        if (!in_func) return 1;
        if (ac > 1) last_status = atoi(av[1]);
        flow = 3;
        return last_status;
    }
    if (!strcmp(n, "break")) { flow = 1; return 0; }
    if (!strcmp(n, "continue")) { flow = 2; return 0; }
    if (!strcmp(n, "shift")) {
        int k = ac > 1 ? atoi(av[1]) : 1;
        if (k < 0 || k >= npos) return 1;
        for (int i = 1; i <= k; i++) free(pos[i]);
        for (int i = 1; i + k < npos; i++) pos[i] = pos[i + k];
        npos -= k;
        return 0;
    }
    if (!strcmp(n, "export")) {
        if (ac == 1) {
            for (int i = 0; i < nvars; i++) if (vars[i].exp) printf("export %s=%s\n", vars[i].name, vars[i].val);
            return 0;
        }
        for (int i = 1; i < ac; i++) {
            char *eq = strchr(av[i], '=');
            if (eq) { *eq = 0; var_set(av[i], eq + 1, 1); *eq = '='; }
            else { const char *v = var_get(av[i]); var_set(av[i], v ? v : "", 1); }
        }
        return 0;
    }
    if (!strcmp(n, "unset")) {
        for (int i = 1; i < ac; i++) var_unset(av[i]);
        return 0;
    }
    if (!strcmp(n, "read")) {
        if (ac < 2) return 1;
        struct buf b; binit(&b);
        char c = 0;
        int got = 0;
        for (;;) {
            int r = read(STDIN, &c, 1);
            if (r <= 0) break;
            got = 1;
            if (c == '\n') break;
            bputc(&b, c);
        }
        if (!got) { free(b.s); return 1; }
        char *p = b.s;
        int vi = 1;
        while (vi < ac - 1) {
            while (*p && is_ifs(*p)) p++;
            char *s = p;
            while (*p && !is_ifs(*p)) p++;
            char sv = *p; *p = 0;
            var_set(av[vi], s, var_find(av[vi]) && var_find(av[vi])->exp);
            if (sv) p++;
            vi++;
        }
        while (*p && is_ifs(*p)) p++;
        var_set(av[ac - 1], p, var_find(av[ac - 1]) && var_find(av[ac - 1])->exp);
        free(b.s);
        return 0;
    }
    if (!strcmp(n, "cd")) {
        char home[40] = "/", name[24] = {0};
        if (account(5, name, 0) == 0) {
            if (!strcmp(name, "root")) strcpy(home, "/root");
            else snprintf(home, sizeof home, "/home/%s", name);
        }
        const char *dir = ac > 1 ? av[1] : home;
        int r = chdir(dir);
        if (r < 0) dprintf(STDERR, "cd: %s: %s\n", dir, strerror(-r));
        return r < 0;
    }
    if (!strcmp(n, "pwd")) {
        char buf[256];
        if (!getcwd(buf, sizeof buf)) return 1;
        printf("%s\n", buf);
        return 0;
    }
    if (!strcmp(n, "umask")) {
        if (ac == 1) { unsigned old = umask(0); umask(old); printf("%04o\n", old); return 0; }
        unsigned mask = 0; char *p = av[1];
        if (!*p) return 1;
        for (; *p; p++) { if (*p < '0' || *p > '7') return 1; mask = mask * 8 + (unsigned)(*p - '0'); }
        if (mask > 0777) return 1;
        umask(mask);
        return 0;
    }
    if (!strcmp(n, "whoami")) {
        char name[24] = {0};
        if (account(5, name, 0) < 0) return 1;
        printf("%s\n", name);
        return 0;
    }
    if (!strcmp(n, "useradd") || !strcmp(n, "passwd")) {
        int add = !strcmp(n, "useradd");
        char name[24] = {0};
        if (ac > 2 || (add && ac != 2)) return 1;
        if (!interactive && !term_mode) { printf("Requires interactive shell.\n"); return 1; }
        if (ac == 2) { if (strlen(av[1]) >= sizeof name) return 1; strcpy(name, av[1]); }
        else account(5, name, 0);
        if (add && account(4, 0, 0) != 0) { printf("useradd: root required.\n"); return 1; }
        return new_password(name, add ? 2 : 3) < 0;
    }
    if (!strcmp(n, "logout")) {
        if (!interactive && !term_mode) return 1;
        account(6, 0, 0);
        if (login_prompt() < 0) exit(0);
        return 0;
    }
    if (!strcmp(n, "help")) { help(); return 0; }
    if (!strcmp(n, "history")) {
        int from = hist_n > HISTORY ? hist_n - HISTORY : 0;
        for (int i = from; i < hist_n; i++) printf("%4d  %s\n", i + 1, history[i % HISTORY]);
        return 0;
    }
    return -1;
}

static void apply_assigns(char **asg, int n, int persist) {
    for (int i = 0; i < n; i++) {
        char *eq = strchr(asg[i], '=');
        if (!eq) continue;
        *eq = 0;
        char *val = expand_str(eq + 1);
        if (persist) var_set(asg[i], val, var_find(asg[i]) && var_find(asg[i])->exp);
        else setenv(asg[i], val, 1);
        free(val);
        *eq = '=';
    }
}

static void eval_simple(struct node *n) {
    char *asg[64]; int nasg = 0, assigning = 1;
    struct fields args; memset(&args, 0, sizeof args);
    saw_subst = 0;
    for (int i = 0; i < n->nw; i++) {
        if (assigning && is_assign(n->w[i])) {
            if (nasg < 64) asg[nasg++] = n->w[i];
            continue;
        }
        assigning = 0;
        int before = args.n;
        expand_one(n->w[i], &args, 0);
        (void)before;
    }
    if (args.n) {
        if (args.n == args.cap) {
            args.cap++;
            args.v = realloc(args.v, (size_t)args.cap * sizeof *args.v);
            args.glob = realloc(args.glob, (size_t)args.cap * sizeof *args.glob);
        }
        args.v[args.n] = NULL;
    }
    if (!args.n) {
        int mark = push_redirs(n->rd, n->nrd);
        if (n->nrd && mark < 0) { last_status = 1; ffree(&args); return; }
        apply_assigns(asg, nasg, 1);
        if (n->nrd) pop_redirs(mark);
        last_status = saw_subst ? subst_status : 0;
        ffree(&args);
        return;
    }
    struct fn *fn = fn_find(args.v[0]);
    const char *nm = args.v[0];
    int is_bi = !strcmp(nm, ":") || !strcmp(nm, "true") || !strcmp(nm, "false") || !strcmp(nm, "echo")
        || !strcmp(nm, "printf") || !strcmp(nm, "test") || !strcmp(nm, "[") || !strcmp(nm, "exit")
        || !strcmp(nm, "return") || !strcmp(nm, "break") || !strcmp(nm, "continue") || !strcmp(nm, "shift")
        || !strcmp(nm, "export") || !strcmp(nm, "unset") || !strcmp(nm, "read") || !strcmp(nm, "cd")
        || !strcmp(nm, "pwd") || !strcmp(nm, "umask") || !strcmp(nm, "whoami") || !strcmp(nm, "useradd")
        || !strcmp(nm, "passwd") || !strcmp(nm, "logout") || !strcmp(nm, "help") || !strcmp(nm, "history");
    if (fn || is_bi) {
        char *oldv[64]; int olde[64];
        for (int i = 0; i < nasg && i < 64; i++) {
            char *eq = strchr(asg[i], '=');
            *eq = 0;
            struct var *v = var_find(asg[i]);
            oldv[i] = v ? xstrdup(v->val) : NULL;
            olde[i] = v ? v->exp : 0;
            char *val = expand_str(eq + 1);
            var_set(asg[i], val, 1);
            free(val);
            *eq = '=';
        }
        int mark = 0, pushed = n->nrd > 0;
        if (pushed) {
            mark = push_redirs(n->rd, n->nrd);
            if (mark < 0) { last_status = 1; ffree(&args); return; }
        }
        if (fn) call_func(fn, args.v, args.n);
        else last_status = builtin(args.v, args.n);
        if (pushed) pop_redirs(mark);
        for (int i = nasg - 1; i >= 0 && i < 64; i--) {
            char *eq = strchr(asg[i], '=');
            *eq = 0;
            if (oldv[i]) var_set(asg[i], oldv[i], olde[i]);
            else var_unset(asg[i]);
            if (olde[i] && oldv[i]) setenv(asg[i], oldv[i], 1);
            free(oldv[i]);
            *eq = '=';
        }
        ffree(&args);
        return;
    }
    run_external(args.v, n->rd, n->nrd, asg, nasg);
    ffree(&args);
}

static void background(struct node *n) {
    int pid = fork();
    if (pid < 0) { last_status = 1; return; }
    if (pid == 0) { setpgid(0, 0); eval(n); exit(last_status); }
    setpgid(pid, pid);
    if (interactive) printf("[%d] started in background\n", pid);
}

static void eval(struct node *n) {
    if (!n || flow) return;
    if (++depth > 80) { dprintf(STDERR, "sh: too much nesting\n"); last_status = 1; depth--; return; }
    int compound = n->kind == N_IF || n->kind == N_WHILE || n->kind == N_FOR || n->kind == N_CASE
        || n->kind == N_BRACE || n->kind == N_SUB;
    int mark = 0;
    if (compound && n->nrd) {
        mark = push_redirs(n->rd, n->nrd);
        if (mark < 0) { last_status = 1; depth--; return; }
    }
    switch (n->kind) {
    case N_LIST:
        for (int i = 0; i < n->nk && !flow; i++) {
            if (n->k[i] && n->k[i]->bg) background(n->k[i]);
            else eval(n->k[i]);
        }
        break;
    case N_AND:
        eval(n->k[0]);
        if (!flow && last_status == 0) eval(n->k[1]);
        break;
    case N_OR:
        eval(n->k[0]);
        if (!flow && last_status != 0) eval(n->k[1]);
        break;
    case N_PIPE: {
        if (n->nk <= 1) { eval(n->nk ? n->k[0] : NULL); break; }
        int prev = -1, *pids = xmalloc((size_t)n->nk * sizeof *pids), started = 0;
        for (int i = 0; i < n->nk; i++) {
            int fds[2] = {-1, -1};
            if (i + 1 < n->nk && pipe(fds) < 0) { last_status = 1; break; }
            int pid = fork();
            if (pid < 0) { last_status = 1; break; }
            if (pid == 0) {
                if (prev >= 0) { dup2(prev, STDIN); close(prev); }
                if (fds[1] >= 0) { dup2(fds[1], STDOUT); close(fds[1]); close(fds[0]); }
                eval(n->k[i]);
                exit(last_status);
            }
            if (prev >= 0) close(prev);
            if (fds[1] >= 0) close(fds[1]);
            prev = fds[0];
            pids[started++] = pid;
        }
        if (prev >= 0) close(prev);
        for (int i = 0; i < started; i++) {
            int st = 0;
            if (waitpid(pids[i], &st, 0) > 0 && i == started - 1) last_status = decode_wait(st);
        }
        free(pids);
        break;
    }
    case N_SIMPLE:
        eval_simple(n);
        break;
    case N_IF:
        eval(n->nk ? n->k[0] : NULL);
        if (!flow) {
            if (last_status == 0) eval(n->nk > 1 ? n->k[1] : NULL);
            else if (n->els) eval(n->els);
            else last_status = 0;   /* POSIX: if без else и с ложным условием завершается нулём */
        }
        break;
    case N_WHILE: {
        int ran = 0, body_status = 0;
        for (;;) {
            eval(n->nk ? n->k[0] : NULL);
            if (flow || last_status) break;
            eval(n->nk > 1 ? n->k[1] : NULL);
            ran = 1; body_status = last_status;
            if (flow == 1) { flow = 0; break; }
            if (flow == 2) { flow = 0; continue; }
            if (flow) break;
        }
        if (!flow) last_status = ran ? body_status : 0;
        break;
    }
    case N_FOR: {
        struct fields words; memset(&words, 0, sizeof words);
        if (n->narms < 0) {
            for (int i = 1; i < npos; i++) fadd(&words, xstrdup(pos[i]), 0);
        } else for (int i = 0; i < n->nw; i++) expand_one(n->w[i], &words, 0);
        for (int i = 0; i < words.n && !flow; i++) {
            var_set(n->name, words.v[i], var_find(n->name) && var_find(n->name)->exp);
            eval(n->nk ? n->k[0] : NULL);
            if (flow == 1) { flow = 0; break; }
            if (flow == 2) { flow = 0; continue; }
        }
        ffree(&words);
        break;
    }
    case N_CASE: {
        char *word = n->nw ? expand_str(n->w[0]) : xstrdup("");
        for (int i = 0; i < n->narms; i++) {
            int hit = 0;
            for (int j = 0; j < n->arms[i].np; j++) {
                char *pat = expand_str(n->arms[i].pat[j]);
                if (wild(pat, word)) hit = 1;
                free(pat);
                if (hit) break;
            }
            if (hit) { eval(n->arms[i].body); break; }
        }
        free(word);
        break;
    }
    case N_BRACE:
        eval(n->nk ? n->k[0] : NULL);
        break;
    case N_SUB: {
        int pid = fork();
        if (pid < 0) last_status = 1;
        else if (pid == 0) { eval(n->nk ? n->k[0] : NULL); exit(last_status); }
        else {
            int st = 0;
            if (waitpid(pid, &st, 0) > 0) last_status = decode_wait(st);
        }
        break;
    }
    case N_FUNC: {
        struct fn *f = fn_find(n->name);
        if (!f) {
            if (nfns % 8 == 0) fns = realloc(fns, (size_t)(nfns + 8) * sizeof *fns);
            f = &fns[nfns++];
            f->name = xstrdup(n->name);
        } else free_node(f->body);
        f->body = n->nk ? n->k[0] : NULL;
        if (n->nk) n->k[0] = NULL;
        last_status = 0;
        break;
    }
    default:
        break;
    }
    if (compound && n->nrd) pop_redirs(mark);
    depth--;
}

static int eval_source(const char *text, int allow_more) {
    lex(text ? text : "");
    struct node *n = parse_list();
    int inc = hungry && allow_more;
    int bad = perr && !inc;
    if (inc) { free_node(n); return -2; }
    if (bad) {
        dprintf(STDERR, "sh: %s near '%s'\n", synmsg ? synmsg : "syntax error",
            (ti < ntoks && toks[ti].s) ? toks[ti].s : "end of command");
        last_status = 2;
        free_node(n);
        return -1;
    }
    flow = 0;
    eval(n);
    free_node(n);
    return 0;
}

static void reap_background(void) {
    int st, pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
        printf("[%d] done, exit code %d\n", pid, decode_wait(st));
}

static void help(void) {
    printf("MyOS sh. Built-ins: cd, pwd, exit, export, unset, test, [, echo, printf, shift, read, help\n");
    printf("Syntax: if/while/for/case, functions, $VAR ${VAR:-x} ${VAR#pat} $(( )), $(cmd), pipes, redirects\n");
    printf("Keys: arrows, Home/End, Up/Down = history, Tab = complete, Ctrl+L/U/K/W, Ctrl+D = exit\n");
    printf("Programs in /bin:");
    struct dirent_info e;
    for (int i = 0; dirread("/bin", i, &e) == 1; i++) printf(" %s", e.name);
    printf("\nPrograms in /ext/bin:");
    int ext = 0;
    for (int i = 0; dirread("/ext/bin", i, &e) == 1; i++) { printf(" %s", e.name); ext++; }
    if (!ext) printf(" (none)");
    printf("\nSame name: /bin wins. Run the other copy as /ext/bin/name.\n");
}

static char *read_fd(int fd) {
    struct buf b; binit(&b);
    char tmp[4096]; int n;
    while ((n = read(fd, tmp, sizeof tmp)) > 0) badd(&b, tmp, n);
    return btake(&b);
}

int main(int argc, char **argv) {
    int once = 0;
    char *text = NULL;
    char *arg0 = "sh";
    char **rest = NULL;
    int nrest = 0;
    term_mode = false;
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        if (argc < 3) { dprintf(STDERR, "sh: -c: command required\n"); return 2; }
        text = argv[2];
        once = 1;
        interactive = false;
        arg0 = argc > 3 ? argv[3] : "sh";
        rest = argv + 3;
        nrest = argc - 3;
    } else if (argc > 1 && !strcmp(argv[1], "-t")) { term_mode = true; argc = 1; }
    if (!once && argc > 1) {
        int fd = open(argv[1], O_READ);
        if (fd < 0) { dprintf(STDERR, "sh: %s: %s\n", argv[1], strerror(errno)); return 1; }
        text = read_fd(fd);
        close(fd);
        once = 2;
        interactive = false;
        arg0 = argv[1];
        rest = argv + 1;
        nrest = argc - 1;
    }
    import_env();
    if (!once) {
        char *v0[] = { "sh" };
        pos_init(v0, 1);
    } else if (nrest <= 0) {
        char *v0[] = { arg0 };
        pos_init(v0, 1);
    } else pos_init(rest, nrest);

    setfg(0);
    if (interactive && login_prompt() < 0) return 1;
    if (term_mode) { interactive = false; printf("MyOS sh (pid %d). Type 'help'.\n", getpid()); }
    else if (interactive && ttyctl(STDIN, TTYCTL_GETSIZE, 0) >= 0)
        printf("MyOS sh (pid %d). Type 'help'.\n", getpid());
    else if (interactive)
        printf("MyOS sh (pid %d). Type 'help'. 'desktop' starts the graphical desktop.\n", getpid());

    if (once) {
        eval_source(text, 0);
        if (once == 2) free(text);
        return last_status;
    }

    struct buf acc; binit(&acc);
    char line[MAX_LINE], cwd[256];
    for (;;) {
        if (interactive && ttyctl(STDIN, TTYCTL_GETSIZE, 0) < 0) {
            struct unit_info desktop;
            while (unitctl(UNIT_STATUS, "desktop.service", &desktop, 0) == 0 &&
                   (desktop.state == UNIT_ACTIVE || desktop.state == UNIT_RESTARTING)) sleep_ms(100);
        }
        reap_background();
        if (interactive) {
            char prompt[160];
            getcwd(cwd, sizeof cwd);
            if (!acc.n)
                snprintf(prompt, sizeof prompt, ANSI_GREEN ANSI_BOLD "%s" ANSI_RESET ":" ANSI_BLUE ANSI_BOLD "%s" ANSI_RESET "$ ", username, cwd);
            else snprintf(prompt, sizeof prompt, "> ");
            if (edit_line(prompt, line, sizeof line) < 0) break;
        } else {
            if (term_mode) {
                getcwd(cwd, sizeof cwd);
                printf(ANSI_GREEN ANSI_BOLD "%s" ANSI_RESET ":" ANSI_BLUE ANSI_BOLD "%s" ANSI_RESET "$ ", username, cwd);
            }
            if (readln(STDIN, line, sizeof line) < 0) break;
        }
        if (acc.n) bputc(&acc, '\n');
        bputs(&acc, line);
        int rc = eval_source(acc.s, 1);
        if (rc == -2) continue;
        free(acc.s); binit(&acc);
    }
    if (interactive) printf("exit\n");
    return last_status;
}
