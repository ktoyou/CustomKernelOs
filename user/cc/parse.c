/* Парсер: токены -> дерево программы (функции, переменные, выражения) с типами.
 * Обычный «рекурсивный спуск»: каждой части грамматики Си — своя функция.
 *
 *   program    = (typedef | function | global-var)*
 *   stmt       = return | if | switch | case | for | while | do | goto | { ... } | expr ;
 *   expr       = assign ("," expr)?
 *   assign     = conditional (= += -= ... assign)?
 *   ...        = || && | ^ & == < << + * cast unary postfix primary */
#include "cc.h"

/* ------------------------------------------------------------ области видимости */

typedef struct VarScope {
    struct VarScope *next;
    char *name;
    Obj  *var;
    Type *type_def;
    Type *enum_ty;
    int   enum_val;
} VarScope;

typedef struct TagScope {
    struct TagScope *next;
    char *name;
    Type *ty;
} TagScope;

typedef struct Scope {
    struct Scope *next;
    VarScope *vars;
    TagScope *tags;
} Scope;

typedef struct {
    bool is_typedef, is_static, is_extern, is_inline;
    int  align;
} VarAttr;

/* Инициализатор: дерево по форме типа ({ {1, 2}, "abc" }) */
typedef struct Initializer {
    struct Initializer *next;
    Type *ty;
    Token *tok;
    bool  is_flexible;
    Node *expr;
    struct Initializer **children;
} Initializer;

typedef struct InitDesg {
    struct InitDesg *next;
    int  idx;
    Member *member;
    Obj *var;
} InitDesg;

static Obj   *locals, *globals;
static Scope  scope0;
static Scope *scope = &scope0;
static Obj   *current_fn;
static Node  *gotos, *labels;
static int    brk_label, cont_label;
static Node  *current_switch;
static int    label_count = 1;
static int    file_id;                  /* номер файла: static-имена делаем уникальными между файлами */

static char *static_name(char *name) { return format("%s.%d", name, file_id); }

int new_label(void) { return label_count++; }

static bool is_typename(Token *tok);
static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Type *typename_(Token **rest, Token *tok);
static Type *enum_specifier(Token **rest, Token *tok);
static Type *struct_union_decl(Token **rest, Token *tok, TypeKind kind);
static Type *type_suffix(Token **rest, Token *tok, Type *ty);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static void  initializer2(Token **rest, Token *tok, Initializer *init);
static Initializer *initializer(Token **rest, Token *tok, Type *ty, Type **new_ty);
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var);
static void  gvar_initializer(Token **rest, Token *tok, Obj *var);
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Node *expr_stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static int   eval(Node *node);
static int   eval2(Node *node, char ***label);
static double eval_double(Node *node);
static Node *assign(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static Node *conditional(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *bitor_(Token **rest, Token *tok);
static Node *bitxor_(Token **rest, Token *tok);
static Node *bitand_(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *new_add(Node *lhs, Node *rhs, Token *tok);
static Node *new_sub(Node *lhs, Node *rhs, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Member *get_struct_member(Type *ty, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *funcall(Token **rest, Token *tok, Node *node);
static Node *primary(Token **rest, Token *tok);
static Token *parse_typedef(Token *tok, Type *basety);
static Token *function(Token *tok, Type *basety, VarAttr *attr);
static Token *global_variable(Token *tok, Type *basety, VarAttr *attr);

static void enter_scope(void) { Scope *sc = xcalloc(sizeof(Scope)); sc->next = scope; scope = sc; }
static void leave_scope(void) { scope = scope->next; }

static char *get_ident(Token *tok) {
    if (tok->kind != TK_IDENT) error_tok(tok, "expected an identifier");
    return strndup2(tok->loc, tok->len);
}

static VarScope *find_var(Token *tok) {
    for (Scope *sc = scope; sc; sc = sc->next)
        for (VarScope *v = sc->vars; v; v = v->next)
            if ((int)strlen(v->name) == tok->len && !strncmp(v->name, tok->loc, tok->len)) return v;
    return NULL;
}

static Type *find_tag(Token *tok) {
    for (Scope *sc = scope; sc; sc = sc->next)
        for (TagScope *t = sc->tags; t; t = t->next)
            if ((int)strlen(t->name) == tok->len && !strncmp(t->name, tok->loc, tok->len)) return t->ty;
    return NULL;
}

static Node *new_node(NodeKind kind, Token *tok) {
    Node *node = xcalloc(sizeof(Node));
    node->kind = kind;
    node->tok = tok;
    return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
    Node *node = new_node(kind, tok);
    node->lhs = lhs;
    node->rhs = rhs;
    return node;
}

static Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
    Node *node = new_node(kind, tok);
    node->lhs = expr;
    return node;
}

static Node *new_num(int val, Token *tok) {
    Node *node = new_node(ND_NUM, tok);
    node->val = val;
    node->ival = (unsigned long long)(long long)val;
    return node;
}

static Node *new_var_node(Obj *var, Token *tok) {
    Node *node = new_node(ND_VAR, tok);
    node->var = var;
    return node;
}

Node *new_cast(Node *expr, Type *ty) {
    add_type(expr);
    Node *node = xcalloc(sizeof(Node));
    node->kind = ND_CAST;
    node->tok = expr->tok;
    node->lhs = expr;
    node->ty = copy_type(ty);
    return node;
}

static VarScope *push_scope(char *name) {
    VarScope *sc = xcalloc(sizeof(VarScope));
    sc->name = name;
    sc->next = scope->vars;
    scope->vars = sc;
    return sc;
}

static Initializer *new_initializer(Type *ty, bool is_flexible) {
    Initializer *init = xcalloc(sizeof(Initializer));
    init->ty = ty;
    if (ty->kind == TY_ARRAY) {
        if (is_flexible && ty->size < 0) { init->is_flexible = true; return init; }
        init->children = xcalloc(sizeof(Initializer *) * (ty->array_len > 0 ? ty->array_len : 1));
        for (int i = 0; i < ty->array_len; i++) init->children[i] = new_initializer(ty->base, false);
        return init;
    }
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        int len = 0;
        for (Member *m = ty->members; m; m = m->next) len++;
        init->children = xcalloc(sizeof(Initializer *) * (len ? len : 1));
        for (Member *m = ty->members; m; m = m->next) {
            if (is_flexible && ty->is_flexible && !m->next) {
                Initializer *child = xcalloc(sizeof(Initializer));
                child->ty = m->ty;
                child->is_flexible = true;
                init->children[m->idx] = child;
            } else {
                init->children[m->idx] = new_initializer(m->ty, false);
            }
        }
        return init;
    }
    return init;
}

static Obj *new_var(char *name, Type *ty) {
    Obj *var = xcalloc(sizeof(Obj));
    var->name = name;
    var->ty = ty;
    var->align = ty->align;
    push_scope(name)->var = var;
    return var;
}

static Obj *new_lvar(char *name, Type *ty) {
    Obj *var = new_var(name, ty);
    var->is_local = true;
    var->next = locals;
    locals = var;
    return var;
}

static Obj *new_gvar(char *name, Type *ty) {
    Obj *var = new_var(name, ty);
    var->next = globals;
    var->is_static = true;
    var->is_definition = true;
    globals = var;
    return var;
}

static char *new_unique_name(void) {
    static int id = 0;
    return format(".L..%d", id++);
}

static Obj *new_anon_gvar(Type *ty) { return new_gvar(new_unique_name(), ty); }

/* Одинаковые литералы — один объект в rodata: текст программы занимает меньше ОЗУ. */
static Obj *new_string_literal(char *p, int len, Type *ty) {
    for (Obj *v = globals; v; v = v->next) {
        if (!v->is_rodata || !v->init_data || v->ty->size != len) continue;
        if (!memcmp(v->init_data, p, (size_t)len)) return v;
    }
    Obj *var = new_anon_gvar(ty);
    var->init_data = xcalloc(len);
    memcpy(var->init_data, p, len);
    var->is_rodata = true;
    return var;
}

/* помнить, что текущая функция упоминает name (для выбрасывания неиспользуемых функций) */
static void add_ref(char *name) {
    if (!current_fn) return;
    Obj *fn = current_fn;
    for (int i = 0; i < fn->n_refs; i++) if (!strcmp(fn->refs[i], name)) return;
    if (fn->n_refs == fn->cap_refs) {
        fn->cap_refs = fn->cap_refs ? fn->cap_refs * 2 : 8;
        char **r = xcalloc(sizeof(char *) * fn->cap_refs);
        if (fn->refs) memcpy(r, fn->refs, sizeof(char *) * fn->n_refs);
        fn->refs = r;
    }
    fn->refs[fn->n_refs++] = name;
}

/* ------------------------------------------------------------ __attribute__((...)) */

static Token *skip_attributes(Token *tok) {
    while (equal(tok, "__attribute__")) {
        tok = skip(tok->next, "(");
        int depth = 1;
        while (depth) {
            if (tok->kind == TK_EOF) error_tok(tok, "unterminated __attribute__");
            if (equal(tok, "(")) depth++;
            if (equal(tok, ")")) depth--;
            tok = tok->next;
        }
    }
    return tok;
}

/* ------------------------------------------------------------ спецификаторы типа */

static Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
    enum {
        VOID = 1 << 0, BOOL = 1 << 2, CHAR = 1 << 4, SHORT = 1 << 6, INT = 1 << 8,
        LONG = 1 << 10, FLOAT = 1 << 12, DOUBLE = 1 << 14, OTHER = 1 << 16,
        SIGNED = 1 << 17, UNSIGNED = 1 << 18,
    };
    Type *ty = ty_int;
    int counter = 0;
    bool seen = false, vol = false;

    while (is_typename(tok)) {
        if (equal(tok, "typedef") || equal(tok, "static") || equal(tok, "extern") ||
            equal(tok, "inline") || equal(tok, "__inline") || equal(tok, "_Noreturn")) {
            if (!attr) error_tok(tok, "storage class specifier is not allowed in this context");
            if (equal(tok, "typedef")) attr->is_typedef = true;
            else if (equal(tok, "static")) attr->is_static = true;
            else if (equal(tok, "extern")) attr->is_extern = true;
            else if (!equal(tok, "_Noreturn")) attr->is_inline = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "const") || equal(tok, "auto") || equal(tok, "register") ||
            equal(tok, "restrict") || equal(tok, "__restrict")) {
            tok = tok->next;
            continue;
        }
        if (equal(tok, "volatile")) { vol = true; tok = tok->next; continue; }
        if (equal(tok, "__attribute__")) { tok = skip_attributes(tok); continue; }

        VarScope *vs = find_var(tok);
        Type *ty2 = vs ? vs->type_def : NULL;
        if (equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum") || ty2) {
            if (counter) break;
            if (equal(tok, "struct")) ty = struct_union_decl(&tok, tok->next, TY_STRUCT);
            else if (equal(tok, "union")) ty = struct_union_decl(&tok, tok->next, TY_UNION);
            else if (equal(tok, "enum")) ty = enum_specifier(&tok, tok->next);
            else {
                /* У функции typedef — свой узел: declarator пишет в него имя. */
                ty = ty2->kind == TY_FUNC ? copy_type(ty2) : ty2;
                tok = tok->next;
            }
            counter += OTHER;
            seen = true;
            continue;
        }

        if (equal(tok, "void")) counter += VOID;
        else if (equal(tok, "_Bool")) counter += BOOL;
        else if (equal(tok, "char")) counter += CHAR;
        else if (equal(tok, "short")) counter += SHORT;
        else if (equal(tok, "int")) counter += INT;
        else if (equal(tok, "long")) counter += LONG;
        else if (equal(tok, "float")) counter += FLOAT;
        else if (equal(tok, "double")) counter += DOUBLE;
        else if (equal(tok, "signed")) counter |= SIGNED;
        else if (equal(tok, "unsigned")) counter |= UNSIGNED;
        else error_tok(tok, "internal error");
        seen = true;

        switch (counter) {
        case VOID: ty = ty_void; break;
        case BOOL: ty = ty_bool; break;
        case CHAR: case SIGNED + CHAR: ty = ty_char; break;
        case UNSIGNED + CHAR: ty = ty_uchar; break;
        case SHORT: case SHORT + INT: case SIGNED + SHORT: case SIGNED + SHORT + INT: ty = ty_short; break;
        case UNSIGNED + SHORT: case UNSIGNED + SHORT + INT: ty = ty_ushort; break;
        case INT: case SIGNED: case SIGNED + INT: case LONG: case LONG + INT: case SIGNED + LONG:
        case SIGNED + LONG + INT: ty = ty_int; break;
        case UNSIGNED: case UNSIGNED + INT: case UNSIGNED + LONG: case UNSIGNED + LONG + INT: ty = ty_uint; break;
        case FLOAT: ty = ty_float; break;
        case DOUBLE: case LONG + DOUBLE: ty = ty_double; break;
        case LONG + LONG: case LONG + LONG + INT: case SIGNED + LONG + LONG: case SIGNED + LONG + LONG + INT:
            ty = ty_llong; break;
        case UNSIGNED + LONG + LONG: case UNSIGNED + LONG + LONG + INT:
            ty = ty_ullong; break;
        default: error_tok(tok, "invalid type");
        }
        tok = tok->next;
    }
    if (vol) {
        ty = copy_type(ty);
        ty->is_volatile = true;
    }
    (void)seen;
    *rest = tok;
    return ty;
}

/* параметры функции: (int a, char *b, ...) */
static Type *func_params(Token **rest, Token *tok, Type *ty) {
    if (equal(tok, "void") && equal(tok->next, ")")) {
        *rest = tok->next->next;
        ty = func_type(ty);
        ty->is_prototyped = true;
        return ty;
    }
    Type head = {0}, *cur = &head;
    bool is_variadic = false;
    bool any = false;
    while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");
        if (equal(tok, "...")) { is_variadic = true; tok = tok->next; skip(tok, ")"); break; }
        any = true;
        Type *ty2 = declspec(&tok, tok, NULL);
        ty2 = declarator(&tok, tok, ty2);
        Token *name = ty2->name;
        if (ty2->kind == TY_ARRAY) { ty2 = pointer_to(ty2->base); ty2->name = name; }   /* int a[] -> int *a */
        else if (ty2->kind == TY_FUNC) { ty2 = pointer_to(ty2); ty2->name = name; }
        cur = cur->next = copy_type(ty2);
    }
    ty = func_type(ty);
    ty->params = head.next;
    ty->is_variadic = is_variadic;
    ty->is_prototyped = any || is_variadic;
    *rest = tok->next;
    return ty;
}

static Type *array_dimensions(Token **rest, Token *tok, Type *ty) {
    while (equal(tok, "static") || equal(tok, "restrict") || equal(tok, "const")) tok = tok->next;
    if (equal(tok, "]")) {
        ty = type_suffix(rest, tok->next, ty);
        return array_of(ty, -1);
    }
    int sz = const_expr(&tok, tok);
    tok = skip(tok, "]");
    ty = type_suffix(rest, tok, ty);
    return array_of(ty, sz);
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
    if (equal(tok, "(")) return func_params(rest, tok->next, ty);
    if (equal(tok, "[")) return array_dimensions(rest, tok->next, ty);
    *rest = tok;
    return ty;
}

static Type *pointers(Token **rest, Token *tok, Type *ty) {
    while (consume(&tok, tok, "*")) {
        ty = pointer_to(ty);
        while (equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") || equal(tok, "__restrict"))
            tok = tok->next;
    }
    *rest = tok;
    return ty;
}

/* declarator = "*"* ("(" ident ")" | "(" declarator ")" | ident) type-suffix */
static Type *declarator(Token **rest, Token *tok, Type *ty) {
    ty = pointers(&tok, tok, ty);
    tok = skip_attributes(tok);
    if (equal(tok, "(")) {
        Token *start = tok;
        Type dummy = {0};
        declarator(&tok, start->next, &dummy);
        tok = skip(tok, ")");
        ty = type_suffix(rest, tok, ty);
        return declarator(&tok, start->next, ty);
    }
    Token *name = NULL;
    Token *name_pos = tok;
    if (tok->kind == TK_IDENT) { name = tok; tok = tok->next; }
    ty = type_suffix(rest, tok, ty);
    *rest = skip_attributes(*rest);
    ty->name = name;
    (void)name_pos;
    return ty;
}

static Type *abstract_declarator(Token **rest, Token *tok, Type *ty) {
    ty = pointers(&tok, tok, ty);
    if (equal(tok, "(") && !is_typename(tok->next) && !equal(tok->next, ")")) {
        Token *start = tok;
        Type dummy = {0};
        abstract_declarator(&tok, start->next, &dummy);
        tok = skip(tok, ")");
        ty = type_suffix(rest, tok, ty);
        return abstract_declarator(&tok, start->next, ty);
    }
    return type_suffix(rest, tok, ty);
}

static Type *typename_(Token **rest, Token *tok) {
    Type *ty = declspec(&tok, tok, NULL);
    return abstract_declarator(rest, tok, ty);
}

static bool is_end(Token *tok) {
    return equal(tok, "}") || (equal(tok, ",") && equal(tok->next, "}"));
}

static bool consume_end(Token **rest, Token *tok) {
    if (equal(tok, "}")) { *rest = tok->next; return true; }
    if (equal(tok, ",") && equal(tok->next, "}")) { *rest = tok->next->next; return true; }
    return false;
}

static Type *enum_specifier(Token **rest, Token *tok) {
    Type *ty = enum_type();
    Token *tag = NULL;
    if (tok->kind == TK_IDENT) { tag = tok; tok = tok->next; }
    if (tag && !equal(tok, "{")) {
        Type *t = find_tag(tag);
        if (!t) error_tok(tag, "unknown enum type");
        if (t->kind != TY_ENUM) error_tok(tag, "not an enum tag");
        *rest = tok;
        return t;
    }
    tok = skip(tok, "{");
    int i = 0, val = 0;
    while (!consume_end(rest, tok)) {
        if (i++ > 0) tok = skip(tok, ",");
        char *name = get_ident(tok);
        tok = tok->next;
        if (equal(tok, "=")) val = const_expr(&tok, tok->next);
        VarScope *sc = push_scope(name);
        sc->enum_ty = ty;
        sc->enum_val = val++;
    }
    if (tag) {
        TagScope *sc = xcalloc(sizeof(TagScope));
        sc->name = strndup2(tag->loc, tag->len);
        sc->ty = ty;
        sc->next = scope->tags;
        scope->tags = sc;
    }
    return ty;
}

static void push_tag_scope(Token *tok, Type *ty) {
    TagScope *sc = xcalloc(sizeof(TagScope));
    sc->name = strndup2(tok->loc, tok->len);
    sc->ty = ty;
    sc->next = scope->tags;
    scope->tags = sc;
}

static void struct_members(Token **rest, Token *tok, Type *ty) {
    Member head = {0}, *cur = &head;
    int idx = 0;
    while (!equal(tok, "}")) {
        VarAttr attr = {0};
        Type *basety = declspec(&tok, tok, &attr);
        bool first = true;
        if ((basety->kind == TY_STRUCT || basety->kind == TY_UNION) && consume(&tok, tok, ";")) {
            Member *mem = xcalloc(sizeof(Member));          /* безымянная вложенная структура */
            mem->ty = basety;
            mem->idx = idx++;
            cur = cur->next = mem;
            continue;
        }
        while (!consume(&tok, tok, ";")) {
            if (!first) tok = skip(tok, ",");
            first = false;
            Member *mem = xcalloc(sizeof(Member));
            mem->ty = declarator(&tok, tok, basety);
            mem->name = mem->ty->name;
            mem->idx = idx++;
            if (consume(&tok, tok, ":")) {
                mem->is_bitfield = true;
                mem->bit_width = const_expr(&tok, tok);
            }
            cur = cur->next = mem;
        }
    }
    if (cur != &head && cur->ty->kind == TY_ARRAY && cur->ty->array_len < 0) {
        cur->ty = array_of(cur->ty->base, 0);
        ty->is_flexible = true;
    }
    *rest = tok->next;
    ty->members = head.next;
}

/* Соседние битовые поля одного типа пакуются в ячейку этого типа.
 * Нулевая ширина закрывает ячейку и выравнивает следующее поле. */
static void layout_record(Type *ty, bool is_union, Token *tok) {
    int offset = 0, max_end = 0, bit_end = -1, unit = 0;
    for (Member *mem = ty->members; mem; mem = mem->next) {
        if (mem->ty->size < 0) error_tok(mem->name ? mem->name : tok, "incomplete member type");
        if (!mem->is_bitfield) {
            bit_end = -1;
            if (is_union) {
                mem->offset = 0;
                if (max_end < mem->ty->size) max_end = mem->ty->size;
            } else {
                offset = align_to(offset, mem->ty->align);
                mem->offset = offset;
                offset += mem->ty->size;
                if (max_end < offset) max_end = offset;
            }
            if (ty->align < mem->ty->align) ty->align = mem->ty->align;
            continue;
        }
        int usz = mem->ty->size, ual = mem->ty->align, width = mem->bit_width;
        if (!is_integer(mem->ty) || usz < 1 || usz > 4)
            error_tok(mem->name ? mem->name : tok, "invalid bit field type");
        if (width < 0 || width > usz * 8)
            error_tok(mem->name ? mem->name : tok, "invalid bit field width");
        if (width == 0) {
            int base = bit_end < 0 ? offset : (bit_end + 7) / 8;
            base = align_to(base, ual);
            bit_end = -1;
            mem->offset = is_union ? 0 : base;
            if (!is_union) offset = base;
            if (max_end < mem->offset) max_end = mem->offset;
            if (ty->align < ual) ty->align = ual;
            continue;
        }
        int boundary = usz * 8, start;
        if (bit_end < 0 || unit != usz || (bit_end % boundary) + width > boundary) {
            int base = bit_end < 0 ? offset : (bit_end + 7) / 8;
            base = align_to(base, ual);
            start = base * 8;
            unit = usz;
        } else start = bit_end;
        mem->offset = (start / 8) & ~(usz - 1);
        mem->bit_off = start - mem->offset * 8;
        bit_end = start + width;
        if (!is_union) offset = mem->offset + usz;
        if (max_end < mem->offset + usz) max_end = mem->offset + usz;
        if (ty->align < ual) ty->align = ual;
    }
    if (ty->align < 1) ty->align = 1;
    ty->size = align_to(max_end, ty->align);
}

static Type *struct_union_decl(Token **rest, Token *tok, TypeKind kind) {
    Token *tag = NULL;
    tok = skip_attributes(tok);
    if (tok->kind == TK_IDENT) { tag = tok; tok = tok->next; }
    if (tag && !equal(tok, "{")) {
        *rest = tok;
        Type *ty = find_tag(tag);
        if (ty) return ty;
        ty = struct_type();
        ty->kind = kind;
        ty->size = -1;
        push_tag_scope(tag, ty);
        return ty;
    }
    tok = skip(tok, "{");
    Type *ty = struct_type();
    ty->kind = kind;
    struct_members(&tok, tok, ty);
    *rest = skip_attributes(tok);

    layout_record(ty, kind == TY_UNION, tok);
    ty->is_complete = true;

    if (tag) {
        /* уже объявленную наперёд (struct x;) — дополняем на месте */
        for (TagScope *sc = scope->tags; sc; sc = sc->next) {
            if ((int)strlen(sc->name) == tag->len && !strncmp(tag->loc, sc->name, tag->len)) {
                *sc->ty = *ty;
                return sc->ty;
            }
        }
        push_tag_scope(tag, ty);
    }
    return ty;
}

/* ------------------------------------------------------------ объявления */

static Node *declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
    Node head = {0}, *cur = &head;
    int i = 0;
    while (!equal(tok, ";")) {
        if (i++ > 0) tok = equal(tok, ",") ? tok->next : skip(tok, ";");
        Type *ty = declarator(&tok, tok, basety);
        if (ty->kind == TY_VOID) error_tok(tok, "variable declared void");
        if (!ty->name) error_tok(tok, "variable name omitted");

        if (attr && attr->is_static) {                      /* static внутри функции = глобальная */
            Obj *var = new_anon_gvar(ty);
            push_scope(get_ident(ty->name))->var = var;
            if (equal(tok, "=")) gvar_initializer(&tok, tok->next, var);
            continue;
        }
        if (attr && attr->is_extern) {
            Obj *var = new_gvar(get_ident(ty->name), ty);
            var->is_definition = false;
            var->is_static = false;
            continue;
        }
        if (ty->kind == TY_FUNC) {                           /* объявление функции внутри функции */
            Obj *fn = new_gvar(get_ident(ty->name), ty);
            fn->is_function = true;
            fn->is_definition = false;
            fn->is_static = false;
            continue;
        }

        Obj *var = new_lvar(get_ident(ty->name), ty);
        if (equal(tok, "=")) {
            Node *expr = lvar_initializer(&tok, tok->next, var);
            cur = cur->next = new_unary(ND_EXPR_STMT, expr, tok);
        }
        if (var->ty->size < 0) error_tok(ty->name, "variable has incomplete type");
        if (var->ty->kind == TY_VOID) error_tok(ty->name, "variable declared void");
    }
    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    *rest = tok->next;
    return node;
}

/* ------------------------------------------------------------ инициализаторы */

static Token *skip_excess_element(Token *tok) {
    if (equal(tok, "{")) {
        tok = skip_excess_element(tok->next);
        return skip(tok, "}");
    }
    assign(&tok, tok);
    return tok;
}

static void string_initializer(Token **rest, Token *tok, Initializer *init) {
    if (init->is_flexible) *init = *new_initializer(array_of(init->ty->base, tok->ty->array_len), false);
    int len = init->ty->array_len < tok->ty->array_len ? init->ty->array_len : tok->ty->array_len;
    for (int i = 0; i < len; i++) init->children[i]->expr = new_num((signed char)tok->str[i], tok);
    *rest = tok->next;
}

static int count_array_init_elements(Token *tok, Type *ty) {
    Initializer *dummy = new_initializer(ty->base, true);
    int i = 0, max = 0;
    while (!consume_end(&tok, tok)) {
        if (i > 0) tok = skip(tok, ",");
        if (equal(tok, "[")) {                               /* [5] = x */
            i = const_expr(&tok, tok->next);
            tok = skip(skip(tok, "]"), "=");
        }
        initializer2(&tok, tok, dummy);
        i++;
        if (max < i) max = i;
    }
    return max;
}

static void array_initializer1(Token **rest, Token *tok, Initializer *init) {
    tok = skip(tok, "{");
    if (init->is_flexible) {
        int len = count_array_init_elements(tok, init->ty);
        *init = *new_initializer(array_of(init->ty->base, len), false);
    }
    for (int i = 0; !consume_end(rest, tok); i++) {
        if (i > 0) tok = skip(tok, ",");
        if (equal(tok, "[")) {
            i = const_expr(&tok, tok->next);
            tok = skip(skip(tok, "]"), "=");
        }
        if (i < init->ty->array_len) initializer2(&tok, tok, init->children[i]);
        else tok = skip_excess_element(tok);
    }
}

static void array_initializer2(Token **rest, Token *tok, Initializer *init) {
    if (init->is_flexible) {
        int len = count_array_init_elements(tok, init->ty);
        *init = *new_initializer(array_of(init->ty->base, len), false);
    }
    for (int i = 0; i < init->ty->array_len && !is_end(tok); i++) {
        if (i > 0) tok = skip(tok, ",");
        initializer2(&tok, tok, init->children[i]);
    }
    *rest = tok;
}

/* volatile struct X копируется, пока X ещё не дописана. Поля остаются у origin. */
static Type *completed(Type *ty) {
    if (ty && !ty->members && ty->origin && ty->origin->members) return ty->origin;
    return ty;
}

static Member *find_member_by_name(Type *ty, Token *tok) {
    ty = completed(ty);
    for (Member *m = ty->members; m; m = m->next)
        if (m->name && m->name->len == tok->len && !strncmp(m->name->loc, tok->loc, tok->len)) return m;
    error_tok(tok, "no such member");
}

static void struct_initializer1(Token **rest, Token *tok, Initializer *init) {
    tok = skip(tok, "{");
    Member *mem = init->ty->members;
    bool first = true;
    while (!consume_end(rest, tok)) {
        if (!first) tok = skip(tok, ",");
        first = false;
        if (equal(tok, ".")) {                               /* .x = 1, .x[0] = 1, .a.b = 1 */
            Initializer *child;
            mem = find_member_by_name(init->ty, tok->next);
            tok = tok->next->next;
            child = init->children[mem->idx];
            while (equal(tok, ".") || equal(tok, "[")) {
                if (equal(tok, ".")) {
                    Member *sub = find_member_by_name(child->ty, tok->next);
                    tok = tok->next->next;
                    child = child->children[sub->idx];
                } else {
                    int i = const_expr(&tok, tok->next);
                    tok = skip(tok, "]");
                    if (i < 0 || i >= child->ty->array_len) error_tok(tok, "array designator out of range");
                    child = child->children[i];
                }
            }
            if (equal(tok, "=")) tok = tok->next;
            initializer2(&tok, tok, child);
            continue;
        }
        if (mem) { initializer2(&tok, tok, init->children[mem->idx]); mem = mem->next; }
        else tok = skip_excess_element(tok);
    }
}

static void struct_initializer2(Token **rest, Token *tok, Initializer *init) {
    bool first = true;
    for (Member *mem = init->ty->members; mem && !is_end(tok); mem = mem->next) {
        if (!first) tok = skip(tok, ",");
        first = false;
        initializer2(&tok, tok, init->children[mem->idx]);
    }
    *rest = tok;
}

static void union_initializer(Token **rest, Token *tok, Initializer *init) {
    if (equal(tok, "{")) {
        Member *mem = init->ty->members;
        tok = tok->next;
        if (equal(tok, ".")) { mem = find_member_by_name(init->ty, tok->next); tok = skip(tok->next->next, "="); }
        initializer2(&tok, tok, init->children[mem->idx]);
        consume(&tok, tok, ",");
        *rest = skip(tok, "}");
    } else {
        initializer2(rest, tok, init->children[0]);
    }
}

static void initializer2(Token **rest, Token *tok, Initializer *init) {
    if (init->ty->kind == TY_ARRAY && tok->kind == TK_STR) { string_initializer(rest, tok, init); return; }
    if (init->ty->kind == TY_ARRAY && equal(tok, "[")) {
        int i = const_expr(&tok, tok->next);
        tok = skip(skip(tok, "]"), "=");
        if (i >= 0 && i < init->ty->array_len) initializer2(&tok, tok, init->children[i]);
        *rest = tok;
        return;
    }
    if (init->ty->kind == TY_ARRAY) {
        if (equal(tok, "{")) array_initializer1(rest, tok, init);
        else array_initializer2(rest, tok, init);
        return;
    }
    if (init->ty->kind == TY_STRUCT) {
        if (equal(tok, "{")) { struct_initializer1(rest, tok, init); return; }
        Node *e = assign(rest, tok);                          /* struct x = другая_структура; */
        add_type(e);
        if (e->ty->kind == TY_STRUCT) { init->expr = e; return; }
        struct_initializer2(rest, tok, init);
        return;
    }
    if (init->ty->kind == TY_UNION) { union_initializer(rest, tok, init); return; }
    if (equal(tok, "{")) {                                    /* int x = {3}; */
        initializer2(&tok, tok->next, init);
        *rest = skip(tok, "}");
        return;
    }
    init->expr = assign(rest, tok);
}

static Type *copy_struct_type(Type *ty) {
    ty = copy_type(ty);
    Member head = {0}, *cur = &head;
    for (Member *mem = ty->members; mem; mem = mem->next) {
        Member *m = xcalloc(sizeof(Member));
        *m = *mem;
        cur = cur->next = m;
    }
    ty->members = head.next;
    return ty;
}

static Initializer *initializer(Token **rest, Token *tok, Type *ty, Type **new_ty) {
    Initializer *init = new_initializer(ty, true);
    initializer2(rest, tok, init);
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible) {
        ty = copy_struct_type(ty);
        Member *mem = ty->members;
        while (mem->next) mem = mem->next;
        mem->ty = init->children[mem->idx]->ty;
        ty->size += mem->ty->size;
        *new_ty = ty;
        return init;
    }
    *new_ty = init->ty;
    return init;
}

static Node *init_desg_expr(InitDesg *desg, Token *tok) {
    if (desg->var) return new_var_node(desg->var, tok);
    if (desg->member) {
        Node *node = new_unary(ND_MEMBER, init_desg_expr(desg->next, tok), tok);
        node->member = desg->member;
        return node;
    }
    Node *lhs = init_desg_expr(desg->next, tok);
    Node *rhs = new_num(desg->idx, tok);
    return new_unary(ND_DEREF, new_add(lhs, rhs, tok), tok);
}

static Node *create_lvar_init(Initializer *init, Type *ty, InitDesg *desg, Token *tok) {
    if (ty->kind == TY_ARRAY) {
        Node *node = new_node(ND_NULL_EXPR, tok);
        for (int i = 0; i < ty->array_len; i++) {
            InitDesg desg2 = { desg, i };
            Node *rhs = create_lvar_init(init->children[i], ty->base, &desg2, tok);
            node = new_binary(ND_COMMA, node, rhs, tok);
        }
        return node;
    }
    if (ty->kind == TY_STRUCT && !init->expr) {
        Node *node = new_node(ND_NULL_EXPR, tok);
        for (Member *mem = ty->members; mem; mem = mem->next) {
            InitDesg desg2 = { desg, 0, mem };
            Node *rhs = create_lvar_init(init->children[mem->idx], mem->ty, &desg2, tok);
            node = new_binary(ND_COMMA, node, rhs, tok);
        }
        return node;
    }
    if (ty->kind == TY_UNION) {
        InitDesg desg2 = { desg, 0, ty->members };
        return create_lvar_init(init->children[0], ty->members->ty, &desg2, tok);
    }
    if (!init->expr) return new_node(ND_NULL_EXPR, tok);
    Node *lhs = init_desg_expr(desg, tok);
    return new_binary(ND_ASSIGN, lhs, init->expr, tok);
}

/* локальная переменная с инициализатором: сначала обнулить, потом присвоить заданное */
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var) {
    Initializer *init = initializer(rest, tok, var->ty, &var->ty);
    InitDesg desg = { NULL, 0, NULL, var };
    Node *lhs = new_node(ND_MEMZERO, tok);
    lhs->var = var;
    Node *rhs = create_lvar_init(init, var->ty, &desg, tok);
    return new_binary(ND_COMMA, lhs, rhs, tok);
}

static void write_buf(char *buf, unsigned long long val, int sz) {
    for (int i = 0; i < sz && i < 8; i++) buf[i] = (char)((val >> (8 * i)) & 0xFF);
}

static void poke_bitfield(char *buf, Member *mem, int base, Initializer *init) {
    unsigned word, mask, val;
    int i, n;
    if (!mem->bit_width || !init || !init->expr) return;
    n = mem->ty->size;
    mask = mem->bit_width >= 32 ? 0xffffffffu : (1u << mem->bit_width) - 1;
    val = (unsigned)eval(init->expr) & mask;
    word = 0;
    for (i = 0; i < n; i++) word |= (unsigned char)buf[base + i] << (8 * i);
    word = (word & ~(mask << mem->bit_off)) | (val << mem->bit_off);
    for (i = 0; i < n; i++) buf[base + i] = (char)((word >> (8 * i)) & 0xff);
}

static Reloc *write_gvar_data(Reloc *cur, Initializer *init, Type *ty, char *buf, int offset) {
    if (ty->kind == TY_ARRAY) {
        int sz = ty->base->size;
        for (int i = 0; i < ty->array_len; i++)
            cur = write_gvar_data(cur, init->children[i], ty->base, buf, offset + sz * i);
        return cur;
    }
    if (ty->kind == TY_STRUCT) {
        if (init->expr) error_tok(init->expr->tok, "initializer is not a constant");
        for (Member *mem = ty->members; mem; mem = mem->next) {
            if (mem->is_bitfield) {
                poke_bitfield(buf, mem, offset + mem->offset, init->children[mem->idx]);
                continue;
            }
            cur = write_gvar_data(cur, init->children[mem->idx], mem->ty, buf, offset + mem->offset);
        }
        return cur;
    }
    if (ty->kind == TY_UNION) {
        if (ty->members && ty->members->is_bitfield) {
            poke_bitfield(buf, ty->members, offset + ty->members->offset, init->children[0]);
            return cur;
        }
        return write_gvar_data(cur, init->children[0], ty->members->ty, buf, offset);
    }
    if (!init->expr) return cur;

    if (ty->kind == TY_FLOAT) {
        float f = (float)eval_double(init->expr);
        memcpy(buf + offset, &f, 4);
        return cur;
    }
    if (ty->kind == TY_DOUBLE) {
        double d = eval_double(init->expr);
        memcpy(buf + offset, &d, 8);
        return cur;
    }
    if (init->expr->kind == ND_NUM) { write_buf(buf + offset, init->expr->ival, ty->size); return cur; }
    char **label = NULL;
    int val = eval2(init->expr, &label);
    if (!label) {
        unsigned long long wide = (unsigned long long)(long long)val;
        write_buf(buf + offset, wide, ty->size);
        return cur;
    }
    Reloc *rel = xcalloc(sizeof(Reloc));
    rel->offset = offset;
    rel->label = *label;
    rel->addend = val;
    cur->next = rel;
    return cur->next;
}

static void gvar_initializer(Token **rest, Token *tok, Obj *var) {
    Initializer *init = initializer(rest, tok, var->ty, &var->ty);
    Reloc head = {0};
    char *buf = xcalloc(var->ty->size > 0 ? var->ty->size : 1);
    write_gvar_data(&head, init, var->ty, buf, 0);
    var->init_data = buf;
    var->rel = head.next;
}

/* ------------------------------------------------------------ операторы */

static bool is_typename(Token *tok) {
    static const char *kw[] = {
        "void", "_Bool", "char", "short", "int", "long", "struct", "union", "typedef", "enum",
        "static", "extern", "signed", "unsigned", "const", "volatile", "auto", "register",
        "restrict", "__restrict", "float", "double", "inline", "__inline", "_Noreturn", "__attribute__",
    };
    for (int i = 0; i < (int)(sizeof(kw) / sizeof(*kw)); i++)
        if (equal(tok, kw[i])) return true;
    if (tok->kind != TK_IDENT) return false;
    VarScope *vs = find_var(tok);
    return vs && vs->type_def;
}

static Node *stmt(Token **rest, Token *tok) {
    if (equal(tok, "return")) {
        Node *node = new_node(ND_RETURN, tok);
        if (consume(rest, tok->next, ";")) return node;
        Node *exp = expr(&tok, tok->next);
        *rest = skip(tok, ";");
        add_type(exp);
        Type *ty = current_fn->ty->return_ty;
        if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) exp = new_cast(exp, ty);
        node->lhs = exp;
        return node;
    }
    if (equal(tok, "if")) {
        Node *node = new_node(ND_IF, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        if (equal(tok, "else")) node->els = stmt(&tok, tok->next);
        *rest = tok;
        return node;
    }
    if (equal(tok, "switch")) {
        Node *node = new_node(ND_SWITCH, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        Node *sw = current_switch;
        current_switch = node;
        int brk = brk_label;
        brk_label = node->brk_label = new_label();
        node->then = stmt(rest, tok);
        current_switch = sw;
        brk_label = brk;
        return node;
    }
    if (equal(tok, "case")) {
        if (!current_switch) error_tok(tok, "stray case");
        Node *node = new_node(ND_CASE, tok);
        int begin = const_expr(&tok, tok->next), end = begin;
        if (equal(tok, "...")) end = const_expr(&tok, tok->next);
        tok = skip(tok, ":");
        node->case_label = new_label();
        node->lhs = stmt(rest, tok);
        node->case_begin = begin;
        node->case_end = end;
        node->case_next = current_switch->case_next;
        current_switch->case_next = node;
        return node;
    }
    if (equal(tok, "default")) {
        if (!current_switch) error_tok(tok, "stray default");
        Node *node = new_node(ND_CASE, tok);
        tok = skip(tok->next, ":");
        node->case_label = new_label();
        node->lhs = stmt(rest, tok);
        current_switch->default_case = node;
        return node;
    }
    if (equal(tok, "for")) {
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");
        enter_scope();
        int brk = brk_label, cont = cont_label;
        brk_label = node->brk_label = new_label();
        cont_label = node->cont_label = new_label();
        if (is_typename(tok)) {
            Type *basety = declspec(&tok, tok, NULL);
            node->init = declaration(&tok, tok, basety, NULL);
        } else {
            node->init = expr_stmt(&tok, tok);
        }
        if (!equal(tok, ";")) node->cond = expr(&tok, tok);
        tok = skip(tok, ";");
        if (!equal(tok, ")")) node->inc = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(rest, tok);
        leave_scope();
        brk_label = brk; cont_label = cont;
        return node;
    }
    if (equal(tok, "while")) {
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        int brk = brk_label, cont = cont_label;
        brk_label = node->brk_label = new_label();
        cont_label = node->cont_label = new_label();
        node->then = stmt(rest, tok);
        brk_label = brk; cont_label = cont;
        return node;
    }
    if (equal(tok, "do")) {
        Node *node = new_node(ND_DO, tok);
        int brk = brk_label, cont = cont_label;
        brk_label = node->brk_label = new_label();
        cont_label = node->cont_label = new_label();
        node->then = stmt(&tok, tok->next);
        brk_label = brk; cont_label = cont;
        tok = skip(tok, "while");
        tok = skip(tok, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        *rest = skip(tok, ";");
        return node;
    }
    if (equal(tok, "goto")) {
        Node *node = new_node(ND_GOTO, tok);
        node->label = get_ident(tok->next);
        node->goto_next = gotos;
        gotos = node;
        *rest = skip(tok->next->next, ";");
        return node;
    }
    if (equal(tok, "break")) {
        if (!brk_label) error_tok(tok, "stray break");
        Node *node = new_node(ND_GOTO, tok);
        node->val = brk_label;
        *rest = skip(tok->next, ";");
        return node;
    }
    if (equal(tok, "continue")) {
        if (!cont_label) error_tok(tok, "stray continue");
        Node *node = new_node(ND_GOTO, tok);
        node->val = cont_label;
        *rest = skip(tok->next, ";");
        return node;
    }
    if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
        Node *node = new_node(ND_LABEL, tok);
        node->label = strndup2(tok->loc, tok->len);
        node->val = new_label();
        node->lhs = stmt(rest, tok->next->next);
        node->goto_next = labels;
        labels = node;
        return node;
    }
    if (equal(tok, "{")) return compound_stmt(rest, tok->next);
    return expr_stmt(rest, tok);
}

static Node *compound_stmt(Token **rest, Token *tok) {
    Node *node = new_node(ND_BLOCK, tok);
    Node head = {0}, *cur = &head;
    enter_scope();
    while (!equal(tok, "}")) {
        if (tok->kind == TK_EOF) error_tok(tok, "expected '}'");
        if (equal(tok, "_Static_assert")) {
            tok = skip(tok->next, "(");
            int v = const_expr(&tok, tok);
            if (consume(&tok, tok, ",")) tok = tok->next;
            if (!v) error_tok(tok, "static assertion failed");
            tok = skip(skip(tok, ")"), ";");
            continue;
        }
        if (is_typename(tok) && !equal(tok->next, ":")) {
            VarAttr attr = {0};
            Type *basety = declspec(&tok, tok, &attr);
            if (attr.is_typedef) { tok = parse_typedef(tok, basety); continue; }
            cur = cur->next = declaration(&tok, tok, basety, &attr);
        } else {
            cur = cur->next = stmt(&tok, tok);
        }
        add_type(cur);
    }
    leave_scope();
    node->body = head.next;
    *rest = tok->next;
    return node;
}

static Node *expr_stmt(Token **rest, Token *tok) {
    if (equal(tok, ";")) { *rest = tok->next; return new_node(ND_BLOCK, tok); }
    Node *node = new_node(ND_EXPR_STMT, tok);
    node->lhs = expr(&tok, tok);
    *rest = skip(tok, ";");
    return node;
}

/* ------------------------------------------------------------ выражения */

static Node *expr(Token **rest, Token *tok) {
    Node *node = assign(&tok, tok);
    if (equal(tok, ",")) return new_binary(ND_COMMA, node, expr(rest, tok->next), tok);
    *rest = tok;
    return node;
}

/* вычислить константное выражение (размер массива, case, #if) */
static int eval(Node *node) { return eval2(node, NULL); }

static int eval_rval(Node *node, char ***label);

static int eval2(Node *node, char ***label) {
    add_type(node);
    if (is_flonum(node->ty)) return (int)eval_double(node);
    switch (node->kind) {
    case ND_ADD: return eval2(node->lhs, label) + eval(node->rhs);
    case ND_SUB: return eval2(node->lhs, label) - eval(node->rhs);
    case ND_MUL: return eval(node->lhs) * eval(node->rhs);
    case ND_DIV: {
        int r = eval(node->rhs);
        if (!r) error_tok(node->tok, "division by zero");
        if (node->ty->is_unsigned) return (unsigned)eval(node->lhs) / (unsigned)r;
        return eval(node->lhs) / r;
    }
    case ND_NEG: return -eval(node->lhs);
    case ND_MOD: {
        int r = eval(node->rhs);
        if (!r) error_tok(node->tok, "division by zero");
        if (node->ty->is_unsigned) return (unsigned)eval(node->lhs) % (unsigned)r;
        return eval(node->lhs) % r;
    }
    case ND_BITAND: return eval(node->lhs) & eval(node->rhs);
    case ND_BITOR: return eval(node->lhs) | eval(node->rhs);
    case ND_BITXOR: return eval(node->lhs) ^ eval(node->rhs);
    case ND_SHL: return (unsigned)eval(node->lhs) << eval(node->rhs);
    case ND_SHR:
        if (node->ty->is_unsigned) return (unsigned)eval(node->lhs) >> eval(node->rhs);
        return eval(node->lhs) >> eval(node->rhs);
    case ND_EQ: return eval(node->lhs) == eval(node->rhs);
    case ND_NE: return eval(node->lhs) != eval(node->rhs);
    case ND_LT:
        if (node->lhs->ty->is_unsigned) return (unsigned)eval(node->lhs) < (unsigned)eval(node->rhs);
        return eval(node->lhs) < eval(node->rhs);
    case ND_LE:
        if (node->lhs->ty->is_unsigned) return (unsigned)eval(node->lhs) <= (unsigned)eval(node->rhs);
        return eval(node->lhs) <= eval(node->rhs);
    case ND_COND: return eval(node->cond) ? eval2(node->then, label) : eval2(node->els, label);
    case ND_COMMA: return eval2(node->rhs, label);
    case ND_NOT: return !eval(node->lhs);
    case ND_BITNOT: return ~eval(node->lhs);
    case ND_LOGAND: return eval(node->lhs) && eval(node->rhs);
    case ND_LOGOR: return eval(node->lhs) || eval(node->rhs);
    case ND_CAST: {
        if (is_flonum(node->lhs->ty)) {
            double d = eval_double(node->lhs);
            if (node->ty->kind == TY_BOOL) return d != 0;
            if (node->ty->is_unsigned) return (int)(unsigned)d;
            return (int)d;
        }
        int val = eval2(node->lhs, label);
        if (is_integer(node->ty)) {
            switch (node->ty->size) {
            case 1:
                if (node->ty->kind == TY_BOOL) return val != 0;
                return node->ty->is_unsigned ? (unsigned char)val : (signed char)val;
            case 2: return node->ty->is_unsigned ? (unsigned short)val : (short)val;
            }
        }
        return val;
    }
    case ND_ADDR: return eval_rval(node->lhs, label);
    case ND_MEMBER:
        if (!label) error_tok(node->tok, "not a compile-time constant");
        if (node->ty->kind != TY_ARRAY) error_tok(node->tok, "invalid initializer");
        return eval_rval(node->lhs, label) + node->member->offset;
    case ND_VAR:
        if (!label) error_tok(node->tok, "not a compile-time constant");
        if (node->var->ty->kind != TY_ARRAY && node->var->ty->kind != TY_FUNC)
            error_tok(node->tok, "invalid initializer");
        *label = &node->var->name;
        return 0;
    case ND_NUM: return node->val;
    default: break;
    }
    error_tok(node->tok, "not a compile-time constant");
}

static int eval_rval(Node *node, char ***label) {
    switch (node->kind) {
    case ND_VAR:
        if (node->var->is_local) error_tok(node->tok, "not a compile-time constant");
        *label = &node->var->name;
        return 0;
    case ND_DEREF: return eval2(node->lhs, label);
    case ND_MEMBER:
        if (node->member->is_bitfield) error_tok(node->tok, "cannot take address of bit-field");
        return eval_rval(node->lhs, label) + node->member->offset;
    default: break;
    }
    error_tok(node->tok, "invalid initializer");
}

static bool is_const_expr(Node *node) {
    add_type(node);
    switch (node->kind) {
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_BITAND: case ND_BITOR: case ND_BITXOR:
    case ND_SHL: case ND_SHR: case ND_EQ: case ND_NE: case ND_LT: case ND_LE: case ND_LOGAND: case ND_LOGOR:
    case ND_MOD:
        return is_const_expr(node->lhs) && is_const_expr(node->rhs);
    case ND_COND:
        if (!is_const_expr(node->cond)) return false;
        return is_const_expr(eval(node->cond) ? node->then : node->els);
    case ND_COMMA: return is_const_expr(node->rhs);
    case ND_NEG: case ND_NOT: case ND_BITNOT: case ND_CAST: return is_const_expr(node->lhs);
    case ND_NUM: return true;
    default: return false;
    }
}

int const_expr(Token **rest, Token *tok) {
    Node *node = conditional(rest, tok);
    return eval(node);
}

static double eval_double(Node *node) {
    add_type(node);
    if (is_integer(node->ty)) {
        if (node->ty->is_unsigned) return (unsigned)eval(node);
        return eval(node);
    }
    switch (node->kind) {
    case ND_ADD: return eval_double(node->lhs) + eval_double(node->rhs);
    case ND_SUB: return eval_double(node->lhs) - eval_double(node->rhs);
    case ND_MUL: return eval_double(node->lhs) * eval_double(node->rhs);
    case ND_DIV: return eval_double(node->lhs) / eval_double(node->rhs);
    case ND_NEG: return -eval_double(node->lhs);
    case ND_COND: return eval_double(node->cond) ? eval_double(node->then) : eval_double(node->els);
    case ND_COMMA: return eval_double(node->rhs);
    case ND_CAST:
        if (is_flonum(node->lhs->ty)) return eval_double(node->lhs);
        if (node->lhs->ty->is_unsigned) return (unsigned)eval(node->lhs);
        return eval(node->lhs);
    case ND_NUM: return node->fval;
    default: break;
    }
    error_tok(node->tok, "not a compile-time constant");
}

/* a op= b  ->  tmp = &a, *tmp = *tmp op b */
/* += у битового поля нельзя свести к адресу: читаем значение, потом пишем его обратно. */
static Node *bitfield_update(Node *binary) {
    Node *mem = binary->lhs, *acc, *acc2, *save, *read, *write;
    Token *tok = binary->tok;
    Obj *base = new_lvar("", pointer_to(mem->lhs->ty));
    Obj *val = new_lvar("", mem->ty);
    save = new_binary(ND_ASSIGN, new_var_node(base, tok), new_unary(ND_ADDR, mem->lhs, tok), tok);
    acc = new_unary(ND_MEMBER, new_unary(ND_DEREF, new_var_node(base, tok), tok), tok);
    acc->member = mem->member;
    acc2 = new_unary(ND_MEMBER, new_unary(ND_DEREF, new_var_node(base, tok), tok), tok);
    acc2->member = mem->member;
    read = new_binary(ND_ASSIGN, new_var_node(val, tok), acc, tok);
    write = new_binary(ND_ASSIGN, acc2, new_binary(binary->kind, new_var_node(val, tok), binary->rhs, tok), tok);
    return new_binary(ND_COMMA, save, new_binary(ND_COMMA, read, write, tok), tok);
}

static Node *to_assign(Node *binary) {
    add_type(binary->lhs);
    add_type(binary->rhs);
    if (binary->lhs->kind == ND_MEMBER && binary->lhs->member->is_bitfield)
        return bitfield_update(binary);
    Token *tok = binary->tok;
    Obj *var = new_lvar("", pointer_to(binary->lhs->ty));
    Node *expr1 = new_binary(ND_ASSIGN, new_var_node(var, tok), new_unary(ND_ADDR, binary->lhs, tok), tok);
    Node *expr2 = new_binary(ND_ASSIGN,
                             new_unary(ND_DEREF, new_var_node(var, tok), tok),
                             new_binary(binary->kind, new_unary(ND_DEREF, new_var_node(var, tok), tok), binary->rhs, tok),
                             tok);
    return new_binary(ND_COMMA, expr1, expr2, tok);
}

static Node *assign(Token **rest, Token *tok) {
    Node *node = conditional(&tok, tok);
    if (equal(tok, "=")) return new_binary(ND_ASSIGN, node, assign(rest, tok->next), tok);
    if (equal(tok, "+=")) return to_assign(new_add(node, assign(rest, tok->next), tok));
    if (equal(tok, "-=")) return to_assign(new_sub(node, assign(rest, tok->next), tok));
    if (equal(tok, "*=")) return to_assign(new_binary(ND_MUL, node, assign(rest, tok->next), tok));
    if (equal(tok, "/=")) return to_assign(new_binary(ND_DIV, node, assign(rest, tok->next), tok));
    if (equal(tok, "%=")) return to_assign(new_binary(ND_MOD, node, assign(rest, tok->next), tok));
    if (equal(tok, "&=")) return to_assign(new_binary(ND_BITAND, node, assign(rest, tok->next), tok));
    if (equal(tok, "|=")) return to_assign(new_binary(ND_BITOR, node, assign(rest, tok->next), tok));
    if (equal(tok, "^=")) return to_assign(new_binary(ND_BITXOR, node, assign(rest, tok->next), tok));
    if (equal(tok, "<<=")) return to_assign(new_binary(ND_SHL, node, assign(rest, tok->next), tok));
    if (equal(tok, ">>=")) return to_assign(new_binary(ND_SHR, node, assign(rest, tok->next), tok));
    *rest = tok;
    return node;
}

static Node *conditional(Token **rest, Token *tok) {
    Node *cond = logor(&tok, tok);
    if (!equal(tok, "?")) { *rest = tok; return cond; }
    Node *node = new_node(ND_COND, tok);
    node->cond = cond;
    node->then = expr(&tok, tok->next);
    tok = skip(tok, ":");
    node->els = conditional(rest, tok);
    return node;
}

static Node *logor(Token **rest, Token *tok) {
    Node *node = logand(&tok, tok);
    while (equal(tok, "||")) { Token *start = tok; node = new_binary(ND_LOGOR, node, logand(&tok, tok->next), start); }
    *rest = tok;
    return node;
}

static Node *logand(Token **rest, Token *tok) {
    Node *node = bitor_(&tok, tok);
    while (equal(tok, "&&")) { Token *start = tok; node = new_binary(ND_LOGAND, node, bitor_(&tok, tok->next), start); }
    *rest = tok;
    return node;
}

static Node *bitor_(Token **rest, Token *tok) {
    Node *node = bitxor_(&tok, tok);
    while (equal(tok, "|")) { Token *start = tok; node = new_binary(ND_BITOR, node, bitxor_(&tok, tok->next), start); }
    *rest = tok;
    return node;
}

static Node *bitxor_(Token **rest, Token *tok) {
    Node *node = bitand_(&tok, tok);
    while (equal(tok, "^")) { Token *start = tok; node = new_binary(ND_BITXOR, node, bitand_(&tok, tok->next), start); }
    *rest = tok;
    return node;
}

static Node *bitand_(Token **rest, Token *tok) {
    Node *node = equality(&tok, tok);
    while (equal(tok, "&")) { Token *start = tok; node = new_binary(ND_BITAND, node, equality(&tok, tok->next), start); }
    *rest = tok;
    return node;
}

static Node *equality(Token **rest, Token *tok) {
    Node *node = relational(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equal(tok, "==")) { node = new_binary(ND_EQ, node, relational(&tok, tok->next), start); continue; }
        if (equal(tok, "!=")) { node = new_binary(ND_NE, node, relational(&tok, tok->next), start); continue; }
        *rest = tok;
        return node;
    }
}

static Node *relational(Token **rest, Token *tok) {
    Node *node = shift(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equal(tok, "<")) { node = new_binary(ND_LT, node, shift(&tok, tok->next), start); continue; }
        if (equal(tok, "<=")) { node = new_binary(ND_LE, node, shift(&tok, tok->next), start); continue; }
        if (equal(tok, ">")) { node = new_binary(ND_LT, shift(&tok, tok->next), node, start); continue; }
        if (equal(tok, ">=")) { node = new_binary(ND_LE, shift(&tok, tok->next), node, start); continue; }
        *rest = tok;
        return node;
    }
}

static Node *shift(Token **rest, Token *tok) {
    Node *node = add(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equal(tok, "<<")) { node = new_binary(ND_SHL, node, add(&tok, tok->next), start); continue; }
        if (equal(tok, ">>")) { node = new_binary(ND_SHR, node, add(&tok, tok->next), start); continue; }
        *rest = tok;
        return node;
    }
}

/* + с учётом указателей: p + 1 сдвигает на sizeof(*p) */
static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
    add_type(lhs);
    add_type(rhs);
    if (is_numeric(lhs->ty) && is_numeric(rhs->ty)) return new_binary(ND_ADD, lhs, rhs, tok);
    if (lhs->ty->base && rhs->ty->base) error_tok(tok, "invalid operands");
    if (!lhs->ty->base && rhs->ty->base) { Node *t = lhs; lhs = rhs; rhs = t; }
    if (lhs->ty->kind == TY_FUNC) error_tok(tok, "invalid operands");
    if (!is_integer(rhs->ty)) error_tok(tok, "invalid operands");
    int sz = lhs->ty->base->size;
    if (lhs->ty->base->kind == TY_VOID) sz = 1;
    rhs = new_binary(ND_MUL, rhs, new_num(sz, tok), tok);
    return new_binary(ND_ADD, lhs, rhs, tok);
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
    add_type(lhs);
    add_type(rhs);
    if (is_numeric(lhs->ty) && is_numeric(rhs->ty)) return new_binary(ND_SUB, lhs, rhs, tok);
    int sz = lhs->ty->base ? (lhs->ty->base->kind == TY_VOID ? 1 : lhs->ty->base->size) : 1;
    if (lhs->ty->base && is_integer(rhs->ty)) {
        rhs = new_binary(ND_MUL, rhs, new_num(sz, tok), tok);
        add_type(rhs);
        Node *node = new_binary(ND_SUB, lhs, rhs, tok);
        node->ty = lhs->ty;
        return node;
    }
    if (lhs->ty->base && rhs->ty->base) {                     /* p - q: сколько элементов между */
        Node *node = new_binary(ND_SUB, lhs, rhs, tok);
        node->ty = ty_int;
        return new_binary(ND_DIV, node, new_num(sz, tok), tok);
    }
    error_tok(tok, "invalid operands");
}

static Node *add(Token **rest, Token *tok) {
    Node *node = mul(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equal(tok, "+")) { node = new_add(node, mul(&tok, tok->next), start); continue; }
        if (equal(tok, "-")) { node = new_sub(node, mul(&tok, tok->next), start); continue; }
        *rest = tok;
        return node;
    }
}

static Node *mul(Token **rest, Token *tok) {
    Node *node = cast(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equal(tok, "*")) { node = new_binary(ND_MUL, node, cast(&tok, tok->next), start); continue; }
        if (equal(tok, "/")) { node = new_binary(ND_DIV, node, cast(&tok, tok->next), start); continue; }
        if (equal(tok, "%")) { node = new_binary(ND_MOD, node, cast(&tok, tok->next), start); continue; }
        *rest = tok;
        return node;
    }
}

static Node *cast(Token **rest, Token *tok) {
    if (equal(tok, "(") && is_typename(tok->next)) {
        Token *start = tok;
        Type *ty = typename_(&tok, tok->next);
        tok = skip(tok, ")");
        if (equal(tok, "{")) {
            /* (тип){...} — безымянный объект с инициализатором.
             * На уровне файла это статический объект: его адрес — константа. */
            if (!current_fn) {
                Obj *var = new_anon_gvar(ty);
                gvar_initializer(&tok, tok, var);
                *rest = tok;
                return new_var_node(var, start);
            }
            Obj *var = new_lvar(new_unique_name(), ty);
            Node *init = lvar_initializer(&tok, tok, var);
            *rest = tok;
            return new_binary(ND_COMMA, init, new_var_node(var, start), start);
        }
        Node *node = new_cast(cast(rest, tok), ty);
        node->tok = start;
        return node;
    }
    return unary(rest, tok);
}

static Node *unary(Token **rest, Token *tok) {
    if (equal(tok, "+")) return cast(rest, tok->next);
    if (equal(tok, "-")) return new_unary(ND_NEG, cast(rest, tok->next), tok);
    if (equal(tok, "&")) {
        Node *lhs = cast(rest, tok->next);
        add_type(lhs);
        return new_unary(ND_ADDR, lhs, tok);
    }
    if (equal(tok, "*")) {
        Node *node = cast(rest, tok->next);
        add_type(node);
        if (node->ty->kind == TY_FUNC) return node;            /* *func == func */
        return new_unary(ND_DEREF, node, tok);
    }
    if (equal(tok, "!")) return new_unary(ND_NOT, cast(rest, tok->next), tok);
    if (equal(tok, "~")) return new_unary(ND_BITNOT, cast(rest, tok->next), tok);
    if (equal(tok, "++")) return to_assign(new_add(unary(rest, tok->next), new_num(1, tok), tok));
    if (equal(tok, "--")) return to_assign(new_sub(unary(rest, tok->next), new_num(1, tok), tok));
    return postfix(rest, tok);
}

static Member *get_struct_member(Type *ty, Token *tok) {
    ty = completed(ty);
    for (Member *mem = ty->members; mem; mem = mem->next) {
        if ((mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION) && !mem->name) {
            if (get_struct_member(mem->ty, tok)) return mem;          /* безымянная вложенная */
            continue;
        }
        if (mem->name && mem->name->len == tok->len && !strncmp(mem->name->loc, tok->loc, tok->len)) return mem;
    }
    return NULL;
}

static Node *struct_ref(Node *node, Token *tok) {
    add_type(node);
    if (node->ty->kind != TY_STRUCT && node->ty->kind != TY_UNION) error_tok(node->tok, "not a struct nor a union");
    Type *ty = completed(node->ty);
    for (;;) {
        Member *mem = get_struct_member(ty, tok);
        if (!mem) error_tok(tok, "no such member");
        node = new_unary(ND_MEMBER, node, tok);
        node->member = mem;
        if (mem->name) break;
        ty = mem->ty;
    }
    return node;
}

/* x++  ->  (tmp = &x, *tmp += 1) - 1 */
static Node *new_inc_dec(Node *node, Token *tok, int addend) {
    add_type(node);
    return new_cast(new_add(to_assign(new_add(node, new_num(addend, tok), tok)), new_num(-addend, tok), tok), node->ty);
}

static Node *postfix(Token **rest, Token *tok) {
    Node *node = primary(&tok, tok);
    for (;;) {
        if (equal(tok, "(")) { node = funcall(&tok, tok->next, node); continue; }
        if (equal(tok, "[")) {
            Token *start = tok;
            Node *idx = expr(&tok, tok->next);
            tok = skip(tok, "]");
            node = new_unary(ND_DEREF, new_add(node, idx, start), start);
            continue;
        }
        if (equal(tok, ".")) { node = struct_ref(node, tok->next); tok = tok->next->next; continue; }
        if (equal(tok, "->")) {
            node = new_unary(ND_DEREF, node, tok);
            node = struct_ref(node, tok->next);
            tok = tok->next->next;
            continue;
        }
        if (equal(tok, "++")) { node = new_inc_dec(node, tok, 1); tok = tok->next; continue; }
        if (equal(tok, "--")) { node = new_inc_dec(node, tok, -1); tok = tok->next; continue; }
        *rest = tok;
        return node;
    }
}

static Node *funcall(Token **rest, Token *tok, Node *fn) {
    add_type(fn);
    if (fn->ty->kind != TY_FUNC && (fn->ty->kind != TY_PTR || fn->ty->base->kind != TY_FUNC))
        error_tok(fn->tok, "not a function");
    Type *ty = fn->ty->kind == TY_FUNC ? fn->ty : fn->ty->base;
    Type *param_ty = ty->params;
    Node head = {0}, *cur = &head;
    while (!equal(tok, ")")) {
        if (cur != &head) tok = skip(tok, ",");
        Node *arg = assign(&tok, tok);
        add_type(arg);
        if (!param_ty && !ty->is_variadic && ty->is_prototyped) error_tok(tok, "too many arguments");
        if (param_ty) {
            if (param_ty->kind != TY_STRUCT && param_ty->kind != TY_UNION) arg = new_cast(arg, param_ty);
            param_ty = param_ty->next;
        } else if (arg->ty->kind == TY_FLOAT) {
            arg = new_cast(arg, ty_double);                   /* «...»: float передаётся как double */
        } else if (is_integer(arg->ty) && arg->ty->size < 4) {
            arg = new_cast(arg, ty_int);
        }
        cur = cur->next = arg;
    }
    if (param_ty) error_tok(tok, "too few arguments");
    *rest = skip(tok, ")");
    Node *node = new_unary(ND_FUNCALL, fn, tok);
    node->func_ty = ty;
    node->ty = ty->return_ty;
    node->args = head.next;
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) node->ret_buffer = new_lvar("", node->ty);
    return node;
}

static Node *primary(Token **rest, Token *tok) {
    Token *start = tok;
    if (equal(tok, "(") && equal(tok->next, "{")) {           /* ({ ... }) — расширение GNU */
        Node *node = new_node(ND_STMT_EXPR, tok);
        node->body = compound_stmt(&tok, tok->next->next)->body;
        *rest = skip(tok, ")");
        return node;
    }
    if (equal(tok, "(")) {
        Node *node = expr(&tok, tok->next);
        *rest = skip(tok, ")");
        return node;
    }
    if (equal(tok, "sizeof") && equal(tok->next, "(") && is_typename(tok->next->next)) {
        Type *ty = typename_(&tok, tok->next->next);
        *rest = skip(tok, ")");
        if (ty->size < 0) error_tok(start, "sizeof of incomplete type");
        return new_cast(new_num(ty->size, start), ty_uint);
    }
    if (equal(tok, "sizeof")) {
        Node *node = unary(rest, tok->next);
        add_type(node);
        if (node->ty->size < 0) error_tok(start, "sizeof of incomplete type");
        return new_cast(new_num(node->ty->size, tok), ty_uint);
    }
    if (equal(tok, "_Alignof") && equal(tok->next, "(") && is_typename(tok->next->next)) {
        Type *ty = typename_(&tok, tok->next->next);
        *rest = skip(tok, ")");
        return new_cast(new_num(ty->align, tok), ty_uint);
    }
    if (equal(tok, "__syscall")) {                            /* __syscall(n, a, b, c, d, e) -> int 0x80 */
        Node *node = new_node(ND_SYSCALL, tok);
        tok = skip(tok->next, "(");
        Node head = {0}, *cur = &head;
        int n = 0;
        while (!equal(tok, ")")) {
            if (n++) tok = skip(tok, ",");
            cur = cur->next = new_cast(assign(&tok, tok), ty_int);
        }
        if (n < 1 || n > 6) error_tok(start, "__syscall takes 1 to 6 arguments");
        node->args = head.next;
        node->val = n;
        *rest = skip(tok, ")");
        return node;
    }
    if (tok->kind == TK_IDENT && (equal(tok, "__func__") || equal(tok, "__FUNCTION__"))) {
        if (!current_fn) error_tok(tok, "__func__ outside of a function");
        char *name = get_ident(current_fn->tok);
        int len = (int)strlen(name) + 1;
        Obj *var = new_string_literal(name, len, array_of(ty_char, len));
        add_ref(var->name);
        *rest = tok->next;
        return new_var_node(var, tok);
    }
    if (tok->kind == TK_IDENT) {
        VarScope *sc = find_var(tok);
        *rest = tok->next;
        if (sc) {
            if (sc->var) {
                if (!sc->var->is_local) add_ref(sc->var->name);
                return new_var_node(sc->var, tok);
            }
            if (sc->enum_ty) return new_num(sc->enum_val, tok);
        }
        if (equal(tok->next, "(")) error_tok(tok, "implicit declaration of a function (include the header?)");
        error_tok(tok, "undefined variable");
    }
    if (tok->kind == TK_STR) {
        Obj *var = new_string_literal(tok->str, tok->str_len, tok->ty);
        add_ref(var->name);
        *rest = tok->next;
        return new_var_node(var, tok);
    }
    if (tok->kind == TK_NUM) {
        Node *node;
        if (is_flonum(tok->ty)) { node = new_node(ND_NUM, tok); node->fval = tok->fval; }
        else { node = new_num(tok->val, tok); node->ival = tok->ival; }
        node->ty = tok->ty;
        *rest = tok->next;
        return node;
    }
    error_tok(tok, "expected an expression");
}

/* ------------------------------------------------------------ верхний уровень */

static Token *parse_typedef(Token *tok, Type *basety) {
    bool first = true;
    while (!consume(&tok, tok, ";")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        Type *ty = declarator(&tok, tok, basety);
        if (!ty->name) error_tok(tok, "typedef name omitted");
        push_scope(get_ident(ty->name))->type_def = ty;
    }
    return tok;
}

static void create_param_lvars(Type *param) {
    if (param) {
        create_param_lvars(param->next);
        if (!param->name) error_tok(param->name ? param->name : current_fn->tok, "parameter name omitted");
        new_lvar(get_ident(param->name), param);
    }
}

/* метки goto -> уникальные номера */
static void resolve_goto_labels(void) {
    for (Node *x = gotos; x; x = x->goto_next) {
        for (Node *y = labels; y; y = y->goto_next)
            if (!strcmp(x->label, y->label)) { x->val = y->val; break; }
        if (!x->val) error_tok(x->tok->next, "use of undeclared label");
    }
    gotos = labels = NULL;
}

static Obj *find_func(char *name) {
    Scope *sc = scope;
    while (sc->next) sc = sc->next;
    for (VarScope *sc2 = sc->vars; sc2; sc2 = sc2->next)
        if (!strcmp(sc2->name, name) && sc2->var && sc2->var->is_function) return sc2->var;
    return NULL;
}

static Token *function(Token *tok, Type *basety, VarAttr *attr) {
    Type *ty;
    Obj *fn = NULL;
    for (;;) {
        ty = declarator(&tok, tok, basety);
        if (!ty->name) error_tok(tok, "function name omitted");
        char *name = get_ident(ty->name);

        fn = find_func(name);
        if (fn) {
            if (!fn->is_function) error_tok(tok, "redeclared as a different kind of symbol");
            if (fn->is_definition && equal(tok, "{")) error_tok(tok, "redefinition of %s", name);
            if (!fn->ty->is_prototyped || equal(tok, "{")) fn->ty = ty;
        } else {
            fn = new_gvar(name, ty);
            fn->is_function = true;
            fn->is_definition = false;
        }
        /* new_gvar ставит is_static=true; для обычных функций сбрасываем.
         * static на определении перекрывает прежнее extern из заголовка
         * (иначе get_time из myos.h делал static get_time в date.c глобальным). */
        if (attr->is_static || (attr->is_inline && !attr->is_extern)) {
            fn->is_static = true;
            if (!strchr(fn->name, '.')) fn->name = static_name(name);
        } else {
            fn->is_static = false;
        }
        fn->tok = ty->name;
        fn->is_definition = fn->is_definition || equal(tok, "{");

        if (consume(&tok, tok, ";")) return tok;
        if (!consume(&tok, tok, ",")) break;
    }

    current_fn = fn;
    locals = NULL;
    enter_scope();
    create_param_lvars(ty->params);
    fn->params = locals;
    if (ty->is_variadic) fn->va_area = NULL;
    tok = skip(tok, "{");
    fn->body = compound_stmt(&tok, tok);
    fn->locals = locals;
    leave_scope();
    resolve_goto_labels();
    current_fn = NULL;
    return tok;
}

static Token *global_variable(Token *tok, Type *basety, VarAttr *attr) {
    bool first = true;
    while (!consume(&tok, tok, ";")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        Type *ty = declarator(&tok, tok, basety);
        if (!ty->name) error_tok(tok, "variable name omitted");
        char *name = get_ident(ty->name);

        /* повторное объявление (extern int x; ... int x = 5;) — та же переменная */
        Obj *var = NULL;
        VarScope *vs = find_var(ty->name);
        if (vs && vs->var && !vs->var->is_local && !vs->var->is_function) var = vs->var;
        if (var) {
            if (ty->size > 0 || var->ty->size < 0) var->ty = ty;
            if (!attr->is_extern) var->is_definition = true;
        } else {
            var = new_gvar(name, ty);
            var->is_definition = !attr->is_extern;
            var->is_static = attr->is_static;
            if (var->is_static) var->name = static_name(name);
        }
        if (equal(tok, "=")) gvar_initializer(&tok, tok->next, var);
        else if (!attr->is_extern) var->is_tentative = true;
    }
    return tok;
}

static bool is_function(Token *tok, Type *basety) {
    if (equal(tok, ";")) return false;
    Type *ty = declarator(&tok, tok, basety);
    return ty->kind == TY_FUNC;
}

Obj *parse(Token *tok) {
    static int next_file_id;
    globals = NULL;
    scope = xcalloc(sizeof(Scope));
    file_id = ++next_file_id;
    while (tok->kind != TK_EOF) {
        if (equal(tok, "_Static_assert")) {
            tok = skip(tok->next, "(");
            int v = const_expr(&tok, tok);
            if (consume(&tok, tok, ",")) tok = tok->next;
            if (!v) error_tok(tok, "static assertion failed");
            tok = skip(skip(tok, ")"), ";");
            continue;
        }
        if (equal(tok, ";")) { tok = tok->next; continue; }
        VarAttr attr = {0};
        Type *basety = declspec(&tok, tok, &attr);
        if (attr.is_typedef) { tok = parse_typedef(tok, basety); continue; }
        if (is_function(tok, basety)) { tok = function(tok, basety, &attr); continue; }
        tok = global_variable(tok, basety, &attr);
    }
    (void)is_const_expr;
    return globals;
}
