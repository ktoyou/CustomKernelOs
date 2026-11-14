/* jslib2.c — встроенные объекты JS (часть 2): Array, String, RegExp (обёртка над
 * jsre.c), JSON, Date, Map/Set, Promise, итераторы. */
#include "jsint.h"

#define NATIVE(name) static int name(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self)
#define ARG(i) ((i) < argc ? argv[i] : js_undef())
#define UNUSED (void)thisv; (void)argc; (void)argv; (void)self
#define TRY(x) do { if ((x) < 0) return -1; } while (0)

extern int js_is_construct;
jsobj *js_make_ctor(const char *name, jsnative fn, int nargs, jsobj *proto);
jsobj *js_own_keys(jsobj *o, int only_enum);
void js_inspect(jsbuf *b, jsval v, int top);

static jsstr *idx_str(int i) {
    char b[16];
    int n = snprintf(b, sizeof(b), "%d", i);
    return js_intern(b, n);
}

static int to_int(jsval v, double dflt, double *out) {
    if (v.tag == JS_UNDEF) { *out = dflt; return 0; }
    double d;
    TRY(js_to_number(v, &d));
    if (js_isnan(d)) d = 0;
    *out = d < 0 ? js_ceil(d) : js_floor(d);
    return 0;
}

/* относительный индекс (как у slice): отрицательные — с конца */
static int rel_index(jsval v, int len, int dflt, int *out) {
    double d;
    TRY(to_int(v, dflt, &d));
    if (d < 0) d += len;
    if (d < 0) d = 0;
    if (d > len) d = len;
    *out = (int)d;
    return 0;
}

/* ------------------------------------------------------------------ итераторы */

enum { IT_AVALUES, IT_AKEYS, IT_AENTRIES, IT_MENTRIES, IT_MKEYS, IT_MVALUES, IT_SVALUES, IT_SENTRIES, IT_STRING };

static jsval make_iter(jsval target, int kind) {
    jsobj *it = js_new_object_proto(js_iter_proto);
    it->cls = C_ITER;
    it->iv[0] = target;
    it->iv[1] = js_num(0);
    it->ptr = (void *)(long)kind;
    return js_objv(it);
}

static jsval iter_result(jsval v, int done) {
    jsobj *r = js_new_object();
    js_define_key(r, S_value, v, P_ENUM | P_WRITE | P_CONF);
    js_define_key(r, S_done, js_bool(done), P_ENUM | P_WRITE | P_CONF);
    return js_objv(r);
}

static jsval pair(jsval a, jsval b) {
    jsobj *p = js_new_array(0);
    js_array_push(p, a);
    js_array_push(p, b);
    return js_objv(p);
}

NATIVE(Iter_next) {
    UNUSED;
    if (!IS_OBJ(thisv) || thisv.u.o->cls != C_ITER) return js_throw("TypeError", "next called on non-iterator");
    jsobj *it = thisv.u.o;
    int kind = (int)(long)it->ptr, i = (int)it->iv[1].u.n;
    jsval t = it->iv[0];
    if (t.tag == JS_UNDEF) { *ret = iter_result(js_undef(), 1); return 0; }
    if (kind == IT_STRING) {
        jsstr *s = t.u.s;
        if (i >= s->len) { it->iv[0] = js_undef(); *ret = iter_result(js_undef(), 1); return 0; }
        int c = (ju8)s->s[i], l = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + l > s->len) l = s->len - i;
        it->iv[1] = js_num(i + l);
        *ret = iter_result(js_strv(js_strn(s->s + i, l)), 0);
        return 0;
    }
    if (kind <= IT_AENTRIES) {
        int len;
        TRY(js_array_len(t, &len));
        if (i >= len) { it->iv[0] = js_undef(); *ret = iter_result(js_undef(), 1); return 0; }
        it->iv[1] = js_num(i + 1);
        jsval v = js_undef();
        if (kind != IT_AKEYS) {
            jsobj *o = t.u.o;
            if ((o->cls == C_ARRAY || o->cls == C_ARGS) && i < o->alen) v = o->arr[i].tag == JS_EMPTY ? js_undef() : o->arr[i];
            else TRY(js_get(t, idx_str(i), &v));
        }
        *ret = iter_result(kind == IT_AVALUES ? v : kind == IT_AKEYS ? js_num(i) : pair(js_num(i), v), 0);
        return 0;
    }
    jsobj *m = t.u.o;
    int step = (kind == IT_SVALUES || kind == IT_SENTRIES) ? 1 : 2;
    while (i < m->alen && m->arr[i].tag == JS_EMPTY) i += step;
    if (i >= m->alen) { it->iv[0] = js_undef(); *ret = iter_result(js_undef(), 1); return 0; }
    it->iv[1] = js_num(i + step);
    jsval k = m->arr[i], v = step == 2 ? m->arr[i + 1] : k;
    switch (kind) {
    case IT_MENTRIES: case IT_SENTRIES: *ret = iter_result(pair(k, v), 0); break;
    case IT_MKEYS: *ret = iter_result(k, 0); break;
    default: *ret = iter_result(v, 0); break;
    }
    return 0;
}

NATIVE(Iter_self) { UNUSED; *ret = thisv; return 0; }

/* ------------------------------------------------------------------ Array */

static int is_array_like_fast(jsval v) { return v.tag == JS_OBJ && (v.u.o->cls == C_ARRAY || v.u.o->cls == C_ARGS) && !v.u.o->hc; }

static int elem(jsval o, int i, jsval *v, int *present) {
    if (is_array_like_fast(o)) {
        jsobj *a = o.u.o;
        if (i < a->alen && a->arr[i].tag != JS_EMPTY) { *v = a->arr[i]; if (present) *present = 1; return 0; }
        if (present) *present = 0;
        *v = js_undef();
        if (!a->proto || a->proto == js_array_proto) return 0;
    }
    jsstr *k = idx_str(i);
    if (present) *present = o.tag == JS_OBJ ? js_has_prop(o.u.o, k) : 1;
    return js_get(o, k, v);
}

static int this_obj(jsval thisv, jsval *o, int *len) {
    jsobj *ob;
    TRY(js_to_object(thisv, &ob));
    *o = js_objv(ob);
    return js_array_len(*o, len);
}

static jsobj *this_array(jsval thisv) {
    return (IS_OBJ(thisv) && thisv.u.o->cls == C_ARRAY) ? thisv.u.o : NULL;
}

NATIVE(Array_ctor) {
    UNUSED;
    jsobj *a;
    if (argc == 1 && argv[0].tag == JS_NUM) {
        double d = argv[0].u.n;
        if (d < 0 || d != js_floor(d) || d > 50000000) return js_throw("RangeError", "Invalid array length");
        a = js_new_array(0);
        for (int i = 0; i < (int)d; i++) { jsval h; h.tag = JS_EMPTY; h.u.n = 0; js_array_push(a, h); }
    } else {
        a = js_new_array(0);
        for (int i = 0; i < argc; i++) js_array_push(a, argv[i]);
    }
    if (js_is_construct && IS_OBJ(thisv) && thisv.u.o->proto != js_array_proto) a->proto = thisv.u.o->proto;
    *ret = js_objv(a);
    return 0;
}

int js_is_array(jsval v);
NATIVE(Array_isArray) { UNUSED; *ret = js_bool(js_is_array(ARG(0))); return 0; }
NATIVE(Array_of) { UNUSED; jsobj *a = js_new_array(0); for (int i = 0; i < argc; i++) js_array_push(a, argv[i]); *ret = js_objv(a); return 0; }

NATIVE(Array_from) {
    UNUSED;
    jsval src = ARG(0), fn = ARG(1);
    jsobj *a = js_new_array(0);
    if (IS_NULLISH(src)) return js_throw("TypeError", "Array.from requires an array-like object");
    jsval itf = js_undef();
    if (src.tag == JS_OBJ) TRY(js_get(src, S_iterator, &itf));
    if (src.tag == JS_STR || js_is_callable(itf)) {
        struct js_iter it;
        TRY(js_iter_open(src, &it));
        for (int i = 0;; i++) {
            jsval v;
            int done;
            TRY(js_iter_step(&it, &v, &done));
            if (done) break;
            if (js_is_callable(fn)) { jsval args[2] = { v, js_num(i) }; TRY(js_call(fn, ARG(2), 2, args, &v)); }
            js_array_push(a, v);
        }
    } else {
        int len;
        TRY(js_array_len(src, &len));
        for (int i = 0; i < len; i++) {
            jsval v;
            TRY(elem(src, i, &v, NULL));
            if (js_is_callable(fn)) { jsval args[2] = { v, js_num(i) }; TRY(js_call(fn, ARG(2), 2, args, &v)); }
            js_array_push(a, v);
        }
    }
    *ret = js_objv(a);
    return 0;
}

NATIVE(ArrayProto_push) {
    UNUSED;
    jsobj *a = this_array(thisv);
    if (a) {
        for (int i = 0; i < argc; i++) js_array_push(a, argv[i]);
        *ret = js_num(a->alen);
        return 0;
    }
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    for (int i = 0; i < argc; i++) TRY(js_set(o, idx_str(len + i), argv[i]));
    TRY(js_set(o, S_length, js_num(len + argc)));
    *ret = js_num(len + argc);
    return 0;
}

NATIVE(ArrayProto_pop) {
    UNUSED;
    jsobj *a = this_array(thisv);
    if (a) {
        if (!a->alen) { *ret = js_undef(); return 0; }
        *ret = a->arr[--a->alen];
        if (ret->tag == JS_EMPTY) *ret = js_undef();
        return 0;
    }
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    if (!len) { TRY(js_set(o, S_length, js_num(0))); *ret = js_undef(); return 0; }
    TRY(elem(o, len - 1, ret, NULL));
    if (IS_OBJ(o)) js_delete(o.u.o, idx_str(len - 1));
    return js_set(o, S_length, js_num(len - 1));
}

NATIVE(ArrayProto_shift) {
    UNUSED;
    jsobj *a = this_array(thisv);
    if (!a) { *ret = js_undef(); return 0; }
    if (!a->alen) { *ret = js_undef(); return 0; }
    *ret = a->arr[0];
    if (ret->tag == JS_EMPTY) *ret = js_undef();
    memmove(a->arr, a->arr + 1, sizeof(jsval) * (a->alen - 1));
    a->alen--;
    return 0;
}

NATIVE(ArrayProto_unshift) {
    UNUSED;
    jsobj *a = this_array(thisv);
    if (!a) { *ret = js_undef(); return 0; }
    for (int i = 0; i < argc; i++) js_array_push(a, js_undef());
    memmove(a->arr + argc, a->arr, sizeof(jsval) * (a->alen - argc));
    for (int i = 0; i < argc; i++) a->arr[i] = argv[i];
    *ret = js_num(a->alen);
    return 0;
}

NATIVE(ArrayProto_slice) {
    UNUSED;
    jsval o;
    int len, s, e;
    TRY(this_obj(thisv, &o, &len));
    TRY(rel_index(ARG(0), len, 0, &s));
    TRY(rel_index(ARG(1), len, len, &e));
    jsobj *r = js_new_array(0);
    for (int i = s; i < e; i++) {
        jsval v;
        int present;
        TRY(elem(o, i, &v, &present));
        if (!present) v.tag = JS_EMPTY;
        js_array_push(r, v);
    }
    *ret = js_objv(r);
    return 0;
}

NATIVE(ArrayProto_splice) {
    UNUSED;
    jsobj *a = this_array(thisv);
    if (!a) return js_throw("TypeError", "splice on non-array");
    int len = a->alen, s, dc;
    TRY(rel_index(ARG(0), len, 0, &s));
    if (argc < 1) dc = 0;
    else if (argc < 2) dc = len - s;
    else { double d; TRY(to_int(argv[1], 0, &d)); dc = d < 0 ? 0 : d > len - s ? len - s : (int)d; }
    jsobj *removed = js_new_array(0);
    for (int i = 0; i < dc; i++) js_array_push(removed, a->arr[s + i]);
    int ins = argc > 2 ? argc - 2 : 0;
    int nl = len - dc + ins;
    if (ins > dc) for (int i = 0; i < ins - dc; i++) js_array_push(a, js_undef());
    memmove(a->arr + s + ins, a->arr + s + dc, sizeof(jsval) * (len - s - dc));
    for (int i = 0; i < ins; i++) a->arr[s + i] = argv[2 + i];
    a->alen = nl;
    *ret = js_objv(removed);
    return 0;
}

NATIVE(ArrayProto_concat) {
    UNUSED;
    jsobj *r = js_new_array(0);
    for (int k = -1; k < argc; k++) {
        jsval v = k < 0 ? thisv : argv[k];
        if (v.tag == JS_OBJ && v.u.o->cls == C_ARRAY) {
            jsobj *a = v.u.o;
            for (int i = 0; i < a->alen; i++) js_array_push(r, a->arr[i]);
        } else js_array_push(r, v);
    }
    *ret = js_objv(r);
    return 0;
}

static int join(jsval o, int len, jsstr *sep, jsval *ret, int depth) {
    if (depth > 20) { *ret = js_cstr(""); return 0; }
    jsbuf b = { 0 };
    for (int i = 0; i < len; i++) {
        if (i) jb_add(&b, sep->s, sep->len);
        jsval v;
        if (elem(o, i, &v, NULL) < 0) { jb_free(&b); return -1; }
        if (IS_NULLISH(v) || v.tag == JS_EMPTY) continue;
        jsstr *s;
        if (v.tag == JS_OBJ && v.u.o->cls == C_ARRAY) {
            jsval inner;
            if (join(v, v.u.o->alen, js_intern(",", 1), &inner, depth + 1) < 0) { jb_free(&b); return -1; }
            s = inner.u.s;
        } else if (js_to_string(v, &s) < 0) { jb_free(&b); return -1; }
        jb_add(&b, s->s, s->len);
    }
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(ArrayProto_join) {
    UNUSED;
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    jsstr *sep = js_intern(",", 1);
    if (argc > 0 && argv[0].tag != JS_UNDEF) TRY(js_to_string(argv[0], &sep));
    return join(o, len, sep, ret, 0);
}

NATIVE(ArrayProto_toString) {
    UNUSED;
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    return join(o, len, js_intern(",", 1), ret, 0);
}

NATIVE(ArrayProto_reverse) {
    UNUSED;
    jsobj *a = this_array(thisv);
    if (a) for (int i = 0, j = a->alen - 1; i < j; i++, j--) { jsval t = a->arr[i]; a->arr[i] = a->arr[j]; a->arr[j] = t; }
    *ret = thisv;
    return 0;
}

static int same_value_zero(jsval a, jsval b) {
    if (a.tag == JS_NUM && b.tag == JS_NUM && js_isnan(a.u.n) && js_isnan(b.u.n)) return 1;
    return js_strict_eq(a, b);
}

static int index_of(jsval thisv, int argc, jsval *argv, int last, int includes, jsval *ret) {
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    jsval x = ARG(0);
    int start;
    if (last) {
        double d;
        TRY(to_int(ARG(1), len - 1, &d));
        if (d < 0) d += len;
        start = d >= len ? len - 1 : (int)d;
        for (int i = start; i >= 0; i--) {
            jsval v;
            int pr;
            TRY(elem(o, i, &v, &pr));
            if (pr && js_strict_eq(v, x)) { *ret = js_num(i); return 0; }
        }
        *ret = js_num(-1);
        return 0;
    }
    TRY(rel_index(ARG(1), len, 0, &start));
    for (int i = start; i < len; i++) {
        jsval v;
        int pr;
        TRY(elem(o, i, &v, &pr));
        if (includes ? same_value_zero(v, x) : (pr && js_strict_eq(v, x))) { *ret = includes ? js_bool(1) : js_num(i); return 0; }
    }
    *ret = includes ? js_bool(0) : js_num(-1);
    return 0;
}

NATIVE(ArrayProto_indexOf) { UNUSED; return index_of(thisv, argc, argv, 0, 0, ret); }
NATIVE(ArrayProto_lastIndexOf) { UNUSED; return index_of(thisv, argc, argv, 1, 0, ret); }
NATIVE(ArrayProto_includes) { UNUSED; return index_of(thisv, argc, argv, 0, 1, ret); }

/* общий обход с функцией: forEach, map, filter, some, every, find, findIndex... */
enum { A_FOREACH, A_MAP, A_FILTER, A_SOME, A_EVERY, A_FIND, A_FINDINDEX, A_FINDLAST, A_FINDLASTINDEX, A_FLATMAP };

static int flat_into(jsobj *r, jsval v, int depth);

static int iterate_cb(jsval thisv, int argc, jsval *argv, jsval *ret, int mode) {
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    jsval fn = ARG(0), that = ARG(1);
    if (!js_is_callable(fn)) return js_throw("TypeError", "callback is not a function");
    jsobj *r = (mode == A_MAP) ? js_new_array(len) : (mode == A_FILTER || mode == A_FLATMAP) ? js_new_array(0) : NULL;
    int rev = mode == A_FINDLAST || mode == A_FINDLASTINDEX;
    for (int k = 0; k < len; k++) {
        int i = rev ? len - 1 - k : k;
        jsval v, res;
        int present;
        TRY(elem(o, i, &v, &present));
        if (!present && mode != A_FIND && mode != A_FINDINDEX && mode != A_FINDLAST && mode != A_FINDLASTINDEX) {
            if (mode == A_MAP) r->arr[i].tag = JS_EMPTY;
            continue;
        }
        jsval args[3] = { v, js_num(i), o };
        TRY(js_call(fn, that, 3, args, &res));
        switch (mode) {
        case A_MAP: r->arr[i] = res; break;
        case A_FILTER: if (js_truthy(res)) js_array_push(r, v); break;
        case A_SOME: if (js_truthy(res)) { *ret = js_bool(1); return 0; } break;
        case A_EVERY: if (!js_truthy(res)) { *ret = js_bool(0); return 0; } break;
        case A_FIND: case A_FINDLAST: if (js_truthy(res)) { *ret = v; return 0; } break;
        case A_FINDINDEX: case A_FINDLASTINDEX: if (js_truthy(res)) { *ret = js_num(i); return 0; } break;
        case A_FLATMAP: TRY(flat_into(r, res, 1)); break;
        }
    }
    switch (mode) {
    case A_MAP: case A_FILTER: case A_FLATMAP: *ret = js_objv(r); break;
    case A_SOME: *ret = js_bool(0); break;
    case A_EVERY: *ret = js_bool(1); break;
    case A_FINDINDEX: case A_FINDLASTINDEX: *ret = js_num(-1); break;
    default: *ret = js_undef();
    }
    return 0;
}

NATIVE(ArrayProto_forEach) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_FOREACH); }
NATIVE(ArrayProto_map) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_MAP); }
NATIVE(ArrayProto_filter) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_FILTER); }
NATIVE(ArrayProto_some) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_SOME); }
NATIVE(ArrayProto_every) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_EVERY); }
NATIVE(ArrayProto_find) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_FIND); }
NATIVE(ArrayProto_findIndex) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_FINDINDEX); }
NATIVE(ArrayProto_findLast) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_FINDLAST); }
NATIVE(ArrayProto_findLastIndex) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_FINDLASTINDEX); }
NATIVE(ArrayProto_flatMap) { UNUSED; return iterate_cb(thisv, argc, argv, ret, A_FLATMAP); }

static int reduce(jsval thisv, int argc, jsval *argv, jsval *ret, int right) {
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    jsval fn = ARG(0);
    if (!js_is_callable(fn)) return js_throw("TypeError", "reduce callback is not a function");
    int k = 0;
    jsval acc;
    if (argc >= 2) acc = argv[1];
    else {
        int found = 0;
        for (; k < len; k++) {
            int i = right ? len - 1 - k : k, pr;
            TRY(elem(o, i, &acc, &pr));
            if (pr) { found = 1; k++; break; }
        }
        if (!found) return js_throw("TypeError", "Reduce of empty array with no initial value");
    }
    for (; k < len; k++) {
        int i = right ? len - 1 - k : k, pr;
        jsval v;
        TRY(elem(o, i, &v, &pr));
        if (!pr) continue;
        jsval args[4] = { acc, v, js_num(i), o };
        TRY(js_call(fn, js_undef(), 4, args, &acc));
    }
    *ret = acc;
    return 0;
}
NATIVE(ArrayProto_reduce) { UNUSED; return reduce(thisv, argc, argv, ret, 0); }
NATIVE(ArrayProto_reduceRight) { UNUSED; return reduce(thisv, argc, argv, ret, 1); }

static int flat_into(jsobj *r, jsval v, int depth) {
    if (depth > 0 && v.tag == JS_OBJ && v.u.o->cls == C_ARRAY) {
        jsobj *a = v.u.o;
        for (int i = 0; i < a->alen; i++) if (a->arr[i].tag != JS_EMPTY) TRY(flat_into(r, a->arr[i], depth - 1));
        return 0;
    }
    js_array_push(r, v);
    return 0;
}

NATIVE(ArrayProto_flat) {
    UNUSED;
    double d;
    TRY(to_int(ARG(0), 1, &d));
    jsobj *r = js_new_array(0);
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    for (int i = 0; i < len; i++) {
        jsval v;
        int pr;
        TRY(elem(o, i, &v, &pr));
        if (pr) TRY(flat_into(r, v, d > 100 ? 100 : (int)d));
    }
    *ret = js_objv(r);
    return 0;
}

NATIVE(ArrayProto_fill) {
    UNUSED;
    jsobj *a = this_array(thisv);
    if (a) {
        int s, e;
        TRY(rel_index(ARG(1), a->alen, 0, &s));
        TRY(rel_index(ARG(2), a->alen, a->alen, &e));
        for (int i = s; i < e; i++) a->arr[i] = ARG(0);
    }
    *ret = thisv;
    return 0;
}

NATIVE(ArrayProto_at) {
    UNUSED;
    jsval o;
    int len;
    TRY(this_obj(thisv, &o, &len));
    double d;
    TRY(to_int(ARG(0), 0, &d));
    if (d < 0) d += len;
    if (d < 0 || d >= len) { *ret = js_undef(); return 0; }
    return elem(o, (int)d, ret, NULL);
}

/* сортировка слиянием (устойчивая); compare может бросить исключение */
static jsval sort_fn;
static int sort_err;

static int sort_cmp(jsval a, jsval b) {
    if (sort_err) return 0;
    if (a.tag == JS_UNDEF) return b.tag == JS_UNDEF ? 0 : 1;
    if (b.tag == JS_UNDEF) return -1;
    if (sort_fn.tag != JS_UNDEF) {
        jsval args[2] = { a, b }, r;
        if (js_call(sort_fn, js_undef(), 2, args, &r) < 0) { sort_err = 1; return 0; }
        double d = js_num_nothrow(r);
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    jsstr *x = js_to_str_nothrow(a), *y = js_to_str_nothrow(b);
    int l = x->len < y->len ? x->len : y->len, c = memcmp(x->s, y->s, l);
    return c ? c : x->len - y->len;
}

static void merge_sort(jsval *v, jsval *tmp, int n) {
    if (n < 2 || sort_err) return;
    int h = n / 2;
    merge_sort(v, tmp, h);
    merge_sort(v + h, tmp, n - h);
    int i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = sort_cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < h) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, sizeof(jsval) * n);
}

NATIVE(ArrayProto_sort) {
    UNUSED;
    jsobj *a = this_array(thisv);
    *ret = thisv;
    if (!a) return 0;
    if (argc > 0 && argv[0].tag != JS_UNDEF && !js_is_callable(argv[0])) return js_throw("TypeError", "The comparison function must be either a function or undefined");
    int n = 0;
    for (int i = 0; i < a->alen; i++) if (a->arr[i].tag != JS_EMPTY) a->arr[n++] = a->arr[i];
    int holes = a->alen - n;
    jsval *tmp = malloc(sizeof(jsval) * (n ? n : 1));
    jsval save = sort_fn;
    sort_fn = ARG(0);
    sort_err = 0;
    merge_sort(a->arr, tmp, n);
    sort_fn = save;
    free(tmp);
    for (int i = 0; i < holes; i++) a->arr[n + i].tag = JS_EMPTY;
    if (sort_err) { sort_err = 0; return -1; }
    return 0;
}

NATIVE(ArrayProto_keys) { UNUSED; *ret = make_iter(thisv, IT_AKEYS); return 0; }
NATIVE(ArrayProto_values) { UNUSED; *ret = make_iter(thisv, IT_AVALUES); return 0; }
NATIVE(ArrayProto_entries) { UNUSED; *ret = make_iter(thisv, IT_AENTRIES); return 0; }

/* ------------------------------------------------------------------ String */

static int this_str(jsval thisv, jsstr **s) {
    if (thisv.tag == JS_STR) { *s = thisv.u.s; return 0; }
    if (IS_NULLISH(thisv)) return js_throw("TypeError", "String.prototype method called on null or undefined");
    return js_to_string(thisv, s);
}

static int str_arg(int argc, jsval *argv, int i, jsstr **s) {
    if (i >= argc) { *s = js_intern("undefined", 9); return 0; }
    return js_to_string(argv[i], s);
}

NATIVE(String_ctor) {
    UNUSED;
    jsstr *s = js_intern("", 0);
    if (argc > 0) {
        if (argv[0].tag == JS_STR && argv[0].u.s->symbol && !js_is_construct) s = js_to_str_nothrow(argv[0]);
        else TRY(js_to_string(argv[0], &s));
    }
    if (js_is_construct && IS_OBJ(thisv)) { thisv.u.o->cls = C_STRING; thisv.u.o->iv[0] = js_strv(s); *ret = thisv; return 0; }
    *ret = js_strv(s);
    return 0;
}

static void put_cp(jsbuf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = cp; jb_add(b, t, 1); }
    else if (cp < 0x800) { t[0] = 0xC0 | (cp >> 6); t[1] = 0x80 | (cp & 63); jb_add(b, t, 2); }
    else if (cp < 0x10000) { t[0] = 0xE0 | (cp >> 12); t[1] = 0x80 | ((cp >> 6) & 63); t[2] = 0x80 | (cp & 63); jb_add(b, t, 3); }
    else { t[0] = 0xF0 | (cp >> 18); t[1] = 0x80 | ((cp >> 12) & 63); t[2] = 0x80 | ((cp >> 6) & 63); t[3] = 0x80 | (cp & 63); jb_add(b, t, 4); }
}

NATIVE(String_fromCharCode) {
    UNUSED;
    jsbuf b = { 0 };
    for (int i = 0; i < argc; i++) {
        int c;
        if (js_to_int32(argv[i], &c) < 0) { jb_free(&b); return -1; }
        unsigned cp = c & 0xFFFF;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < argc) {
            int lo;
            if (js_to_int32(argv[i + 1], &lo) < 0) { jb_free(&b); return -1; }
            lo &= 0xFFFF;
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        put_cp(&b, cp);
    }
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(String_fromCodePoint) {
    UNUSED;
    jsbuf b = { 0 };
    for (int i = 0; i < argc; i++) { int c; if (js_to_int32(argv[i], &c) < 0) { jb_free(&b); return -1; } put_cp(&b, c); }
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(String_raw) {
    UNUSED;
    jsval strs = ARG(0), raw;
    TRY(js_get_c(strs, "raw", &raw));
    int n;
    TRY(js_array_len(raw, &n));
    jsbuf b = { 0 };
    for (int i = 0; i < n; i++) {
        jsval v;
        jsstr *s;
        TRY(js_get(raw, idx_str(i), &v));
        TRY(js_to_string(v, &s));
        jb_add(&b, s->s, s->len);
        if (i + 1 < n && i + 1 < argc) { TRY(js_to_string(argv[i + 1], &s)); jb_add(&b, s->s, s->len); }
    }
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(StringProto_toString) {
    UNUSED;
    if (thisv.tag == JS_STR) { *ret = thisv; return 0; }
    if (IS_OBJ(thisv) && thisv.u.o->cls == C_STRING) { *ret = thisv.u.o->iv[0]; return 0; }
    return js_throw("TypeError", "String.prototype.toString requires that 'this' be a String");
}

NATIVE(StringProto_charAt) {
    UNUSED;
    jsstr *s;
    double d;
    TRY(this_str(thisv, &s));
    TRY(to_int(ARG(0), 0, &d));
    *ret = (d >= 0 && d < s->len) ? js_strv(js_intern(s->s + (int)d, 1)) : js_strv(js_intern("", 0));
    return 0;
}

/* код символа: для байтов UTF-8 возвращаем код всего символа, если i — его начало */
static unsigned cp_at(jsstr *s, int i, int *len) {
    ju8 c = s->s[i];
    *len = 1;
    if (c < 0x80 || c < 0xC0) return c;
    int l = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (i + l > s->len) return c;
    unsigned cp = c & (l == 2 ? 0x1F : l == 3 ? 0x0F : 0x07);
    for (int k = 1; k < l; k++) {
        if (((ju8)s->s[i + k] & 0xC0) != 0x80) return c;
        cp = (cp << 6) | (s->s[i + k] & 0x3F);
    }
    *len = l;
    return cp;
}

NATIVE(StringProto_charCodeAt) {
    UNUSED;
    jsstr *s;
    double d;
    TRY(this_str(thisv, &s));
    TRY(to_int(ARG(0), 0, &d));
    if (d < 0 || d >= s->len) { *ret = js_num(JS_NAN); return 0; }
    int l;
    unsigned cp = cp_at(s, (int)d, &l);
    *ret = js_num(cp > 0xFFFF ? 0xD800 + ((cp - 0x10000) >> 10) : cp);
    return 0;
}

NATIVE(StringProto_codePointAt) {
    UNUSED;
    jsstr *s;
    double d;
    TRY(this_str(thisv, &s));
    TRY(to_int(ARG(0), 0, &d));
    if (d < 0 || d >= s->len) { *ret = js_undef(); return 0; }
    int l;
    *ret = js_num(cp_at(s, (int)d, &l));
    return 0;
}

static int find_str(jsstr *s, jsstr *t, int from) {
    if (from < 0) from = 0;
    if (t->len == 0) return from <= s->len ? from : s->len;
    for (int i = from; i + t->len <= s->len; i++)
        if (s->s[i] == t->s[0] && !memcmp(s->s + i, t->s, t->len)) return i;
    return -1;
}

NATIVE(StringProto_indexOf) {
    UNUSED;
    jsstr *s, *t;
    double d;
    TRY(this_str(thisv, &s));
    TRY(str_arg(argc, argv, 0, &t));
    TRY(to_int(ARG(1), 0, &d));
    *ret = js_num(find_str(s, t, d > s->len ? s->len : (int)d));
    return 0;
}

NATIVE(StringProto_lastIndexOf) {
    UNUSED;
    jsstr *s, *t;
    TRY(this_str(thisv, &s));
    TRY(str_arg(argc, argv, 0, &t));
    double d;
    TRY(to_int(ARG(1), s->len, &d));
    int from = d > s->len - t->len ? s->len - t->len : (int)d;
    for (int i = from; i >= 0; i--) if (!memcmp(s->s + i, t->s, t->len)) { *ret = js_num(i); return 0; }
    *ret = js_num(-1);
    return 0;
}

NATIVE(StringProto_includes) {
    UNUSED;
    jsstr *s, *t;
    double d;
    TRY(this_str(thisv, &s));
    if (IS_OBJ(ARG(0)) && argv[0].u.o->cls == C_REGEXP) return js_throw("TypeError", "First argument must not be a regular expression");
    TRY(str_arg(argc, argv, 0, &t));
    TRY(to_int(ARG(1), 0, &d));
    *ret = js_bool(find_str(s, t, (int)d) >= 0);
    return 0;
}

NATIVE(StringProto_startsWith) {
    UNUSED;
    jsstr *s, *t;
    double d;
    TRY(this_str(thisv, &s));
    TRY(str_arg(argc, argv, 0, &t));
    TRY(to_int(ARG(1), 0, &d));
    int p = d < 0 ? 0 : d > s->len ? s->len : (int)d;
    *ret = js_bool(p + t->len <= s->len && !memcmp(s->s + p, t->s, t->len));
    return 0;
}

NATIVE(StringProto_endsWith) {
    UNUSED;
    jsstr *s, *t;
    double d;
    TRY(this_str(thisv, &s));
    TRY(str_arg(argc, argv, 0, &t));
    TRY(to_int(ARG(1), s->len, &d));
    int e = d < 0 ? 0 : d > s->len ? s->len : (int)d;
    *ret = js_bool(e >= t->len && !memcmp(s->s + e - t->len, t->s, t->len));
    return 0;
}

NATIVE(StringProto_slice) {
    UNUSED;
    jsstr *s;
    int a, b;
    TRY(this_str(thisv, &s));
    TRY(rel_index(ARG(0), s->len, 0, &a));
    TRY(rel_index(ARG(1), s->len, s->len, &b));
    *ret = js_strv(b > a ? js_strn(s->s + a, b - a) : js_intern("", 0));
    return 0;
}

NATIVE(StringProto_substring) {
    UNUSED;
    jsstr *s;
    double a, b;
    TRY(this_str(thisv, &s));
    TRY(to_int(ARG(0), 0, &a));
    TRY(to_int(ARG(1), s->len, &b));
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > s->len) a = s->len;
    if (b > s->len) b = s->len;
    if (a > b) { double t = a; a = b; b = t; }
    *ret = js_strv(js_strn(s->s + (int)a, (int)(b - a)));
    return 0;
}

NATIVE(StringProto_substr) {
    UNUSED;
    jsstr *s;
    int a;
    double l;
    TRY(this_str(thisv, &s));
    TRY(rel_index(ARG(0), s->len, 0, &a));
    TRY(to_int(ARG(1), s->len - a, &l));
    if (l > s->len - a) l = s->len - a;
    if (l < 0) l = 0;
    *ret = js_strv(js_strn(s->s + a, (int)l));
    return 0;
}

/* регистр: ASCII и кириллица в UTF-8 */
static jsstr *change_case(jsstr *s, int upper) {
    jsstr *r = js_strn(s->s, s->len);
    ju8 *p = (ju8 *)r->s;
    for (int i = 0; i < r->len; i++) {
        if (p[i] < 0x80) { p[i] = upper ? toupper(p[i]) : tolower(p[i]); continue; }
        if (i + 1 < r->len && (p[i] == 0xD0 || p[i] == 0xD1)) {
            unsigned cp = ((p[i] & 0x1F) << 6) | (p[i + 1] & 0x3F), n = cp;
            if (upper) { if (cp >= 0x430 && cp <= 0x44F) n = cp - 0x20; else if (cp >= 0x450 && cp <= 0x45F) n = cp - 0x50; }
            else { if (cp >= 0x410 && cp <= 0x42F) n = cp + 0x20; else if (cp >= 0x400 && cp <= 0x40F) n = cp + 0x50; }
            p[i] = 0xC0 | (n >> 6);
            p[i + 1] = 0x80 | (n & 63);
            i++;
        }
    }
    r->hash = 0;
    for (int i = 0; i < r->len; i++) { r->hash ^= p[i]; r->hash *= 16777619u; }
    jsstr *fixed = js_strn(r->s, r->len);                   /* пересчитать хеш правильно */
    return fixed;
}

NATIVE(StringProto_toUpperCase) { UNUSED; jsstr *s; TRY(this_str(thisv, &s)); *ret = js_strv(change_case(s, 1)); return 0; }
NATIVE(StringProto_toLowerCase) { UNUSED; jsstr *s; TRY(this_str(thisv, &s)); *ret = js_strv(change_case(s, 0)); return 0; }

static int is_ws(ju8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
static int trim(jsval thisv, jsval *ret, int l, int r) {
    jsstr *s;
    TRY(this_str(thisv, &s));
    int a = 0, b = s->len;
    if (l) while (a < b && (is_ws(s->s[a]) || ((ju8)s->s[a] == 0xC2 && a + 1 < b && (ju8)s->s[a + 1] == 0xA0 && ++a))) a++;
    if (r) while (b > a && is_ws(s->s[b - 1])) b--;
    *ret = js_strv(js_strn(s->s + a, b - a));
    return 0;
}
NATIVE(StringProto_trim) { UNUSED; return trim(thisv, ret, 1, 1); }
NATIVE(StringProto_trimStart) { UNUSED; return trim(thisv, ret, 1, 0); }
NATIVE(StringProto_trimEnd) { UNUSED; return trim(thisv, ret, 0, 1); }

NATIVE(StringProto_repeat) {
    UNUSED;
    jsstr *s;
    double n;
    TRY(this_str(thisv, &s));
    TRY(to_int(ARG(0), 0, &n));
    if (n < 0 || js_isinf(n)) return js_throw("RangeError", "Invalid count value");
    if (n * s->len > 50000000) return js_throw("RangeError", "Invalid string length");
    jsbuf b = { 0 };
    for (int i = 0; i < (int)n; i++) jb_add(&b, s->s, s->len);
    *ret = js_strv(jb_str(&b));
    return 0;
}

static int pad(jsval thisv, int argc, jsval *argv, jsval *ret, int start) {
    jsstr *s, *f = js_intern(" ", 1);
    double n;
    TRY(this_str(thisv, &s));
    TRY(to_int(ARG(0), 0, &n));
    if (argc > 1 && argv[1].tag != JS_UNDEF) TRY(js_to_string(argv[1], &f));
    if (n <= s->len || !f->len) { *ret = js_strv(s); return 0; }
    if (n > 10000000) return js_throw("RangeError", "Invalid string length");
    int need = (int)n - s->len;
    jsbuf b = { 0 };
    if (!start) jb_add(&b, s->s, s->len);
    for (int i = 0; i < need; i++) jb_addc(&b, f->s[i % f->len]);
    if (start) jb_add(&b, s->s, s->len);
    *ret = js_strv(jb_str(&b));
    return 0;
}
NATIVE(StringProto_padStart) { UNUSED; return pad(thisv, argc, argv, ret, 1); }
NATIVE(StringProto_padEnd) { UNUSED; return pad(thisv, argc, argv, ret, 0); }

NATIVE(StringProto_concat) {
    UNUSED;
    jsstr *s;
    TRY(this_str(thisv, &s));
    for (int i = 0; i < argc; i++) { jsstr *t; TRY(js_to_string(argv[i], &t)); s = js_concat(s, t); }
    *ret = js_strv(s);
    return 0;
}

NATIVE(StringProto_at) {
    UNUSED;
    jsstr *s;
    double d;
    TRY(this_str(thisv, &s));
    TRY(to_int(ARG(0), 0, &d));
    if (d < 0) d += s->len;
    *ret = (d >= 0 && d < s->len) ? js_strv(js_intern(s->s + (int)d, 1)) : js_undef();
    return 0;
}

NATIVE(StringProto_localeCompare) {
    UNUSED;
    jsstr *s, *t;
    TRY(this_str(thisv, &s));
    TRY(str_arg(argc, argv, 0, &t));
    int l = s->len < t->len ? s->len : t->len, c = memcmp(s->s, t->s, l);
    if (!c) c = s->len - t->len;
    *ret = js_num(c < 0 ? -1 : c > 0 ? 1 : 0);
    return 0;
}

NATIVE(StringProto_iterator) { UNUSED; jsstr *s; TRY(this_str(thisv, &s)); *ret = make_iter(js_strv(s), IT_STRING); return 0; }

/* --- регулярные выражения в строковых методах */

static int is_regexp(jsval v) { return IS_OBJ(v) && v.u.o->cls == C_REGEXP; }
static int re_flag(jsobj *re, char f) { return strchr(re->iv[0].u.s->s, f) != NULL; }

static int get_last_index(jsobj *re, int *li) {
    jsval v;
    TRY(js_get(js_objv(re), S_lastIndex, &v));
    double d;
    TRY(to_int(v, 0, &d));
    *li = d < 0 ? 0 : d > 1e9 ? 1000000000 : (int)d;
    return 0;
}

#define MAXCAP 40

/* RegExp.prototype.exec: -> массив совпадения или null */
static int re_exec(jsobj *re, jsstr *s, jsval *ret) {
    int global = re_flag(re, 'g') || re_flag(re, 'y');
    int li = 0;
    if (global) TRY(get_last_index(re, &li));
    if (li > s->len) { TRY(js_set(js_objv(re), S_lastIndex, js_num(0))); *ret = js_null(); return 0; }
    int caps[MAXCAP * 2];
    int n = js_regexp_exec(re, s, li, caps, MAXCAP);
    if (n < 0) return -1;
    if (!n) {
        if (global) TRY(js_set(js_objv(re), S_lastIndex, js_num(0)));
        *ret = js_null();
        return 0;
    }
    if (global) TRY(js_set(js_objv(re), S_lastIndex, js_num(caps[1] == caps[0] && 0 ? caps[1] + 1 : caps[1])));
    jsobj *a = js_new_array(0);
    for (int i = 0; i < n; i++) {
        if (caps[2 * i] < 0) js_array_push(a, js_undef());
        else js_array_push(a, js_strv(js_strn(s->s + caps[2 * i], caps[2 * i + 1] - caps[2 * i])));
    }
    js_define_key(a, S_index, js_num(caps[0]), P_ENUM | P_WRITE | P_CONF);
    js_define_key(a, S_input, js_strv(s), P_ENUM | P_WRITE | P_CONF);
    js_define(a, "groups", js_undef(), P_ENUM | P_WRITE | P_CONF);
    *ret = js_objv(a);
    return 0;
}

NATIVE(RegExp_ctor) {
    UNUSED;
    jsval p = ARG(0), f = ARG(1);
    jsstr *src, *flags = js_intern("", 0);
    if (is_regexp(p)) {
        if (f.tag == JS_UNDEF) { if (!js_is_construct) { *ret = p; return 0; } flags = p.u.o->iv[0].u.s; }
        jsval sv;
        TRY(js_get_c(p, "source", &sv));
        src = sv.u.s;
    } else if (p.tag == JS_UNDEF) src = js_intern("(?:)", 4);
    else TRY(js_to_string(p, &src));
    if (f.tag != JS_UNDEF) TRY(js_to_string(f, &flags));
    jsobj *re;
    TRY(js_regexp_create(src, flags, &re));
    *ret = js_objv(re);
    return 0;
}

NATIVE(RegExpProto_exec) {
    UNUSED;
    if (!is_regexp(thisv)) return js_throw("TypeError", "RegExp.prototype.exec called on incompatible receiver");
    jsstr *s;
    TRY(str_arg(argc, argv, 0, &s));
    return re_exec(thisv.u.o, s, ret);
}

NATIVE(RegExpProto_test) {
    UNUSED;
    if (!is_regexp(thisv)) return js_throw("TypeError", "RegExp.prototype.test called on incompatible receiver");
    jsstr *s;
    TRY(str_arg(argc, argv, 0, &s));
    jsval r;
    TRY(re_exec(thisv.u.o, s, &r));
    *ret = js_bool(r.tag != JS_NULL);
    return 0;
}

NATIVE(RegExpProto_toString) {
    UNUSED;
    jsval src, fl;
    TRY(js_get_c(thisv, "source", &src));
    TRY(js_get_c(thisv, "flags", &fl));
    jsstr *a = js_to_str_nothrow(src), *b = js_to_str_nothrow(fl);
    jsbuf o = { 0 };
    jb_addc(&o, '/');
    jb_add(&o, a->s, a->len);
    jb_addc(&o, '/');
    jb_add(&o, b->s, b->len);
    *ret = js_strv(jb_str(&o));
    return 0;
}

NATIVE(StringProto_match) {
    UNUSED;
    jsstr *s;
    TRY(this_str(thisv, &s));
    jsobj *re;
    if (is_regexp(ARG(0))) re = argv[0].u.o;
    else {
        jsstr *p = ARG(0).tag == JS_UNDEF ? js_intern("(?:)", 4) : NULL;
        if (!p) TRY(js_to_string(argv[0], &p));
        TRY(js_regexp_create(p, js_intern("", 0), &re));
    }
    if (!re_flag(re, 'g')) return re_exec(re, s, ret);
    TRY(js_set(js_objv(re), S_lastIndex, js_num(0)));
    jsobj *a = js_new_array(0);
    int pos = 0;
    while (pos <= s->len) {
        int caps[MAXCAP * 2];
        int n = js_regexp_exec(re, s, pos, caps, MAXCAP);
        if (n < 0) return -1;
        if (!n) break;
        js_array_push(a, js_strv(js_strn(s->s + caps[0], caps[1] - caps[0])));
        pos = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
    }
    *ret = a->alen ? js_objv(a) : js_null();
    return 0;
}

NATIVE(StringProto_matchAll) {
    UNUSED;
    jsstr *s;
    TRY(this_str(thisv, &s));
    jsobj *re;
    if (is_regexp(ARG(0))) re = argv[0].u.o;
    else { jsstr *p; TRY(str_arg(argc, argv, 0, &p)); TRY(js_regexp_create(p, js_intern("g", 1), &re)); }
    jsobj *all = js_new_array(0);
    int pos = 0;
    jsobj *copy;
    TRY(js_regexp_create(js_to_str_nothrow(re->props[0].v), js_intern("", 0), &copy));
    (void)copy;
    while (pos <= s->len) {
        int caps[MAXCAP * 2];
        int n = js_regexp_exec(re, s, pos, caps, MAXCAP);
        if (n < 0) return -1;
        if (!n) break;
        jsobj *m = js_new_array(0);
        for (int i = 0; i < n; i++) js_array_push(m, caps[2 * i] < 0 ? js_undef() : js_strv(js_strn(s->s + caps[2 * i], caps[2 * i + 1] - caps[2 * i])));
        js_define_key(m, S_index, js_num(caps[0]), P_ENUM | P_WRITE | P_CONF);
        js_define_key(m, S_input, js_strv(s), P_ENUM | P_WRITE | P_CONF);
        js_array_push(all, js_objv(m));
        pos = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
    }
    *ret = make_iter(js_objv(all), IT_AVALUES);
    return 0;
}

NATIVE(StringProto_search) {
    UNUSED;
    jsstr *s;
    TRY(this_str(thisv, &s));
    jsobj *re;
    if (is_regexp(ARG(0))) re = argv[0].u.o;
    else { jsstr *p; TRY(str_arg(argc, argv, 0, &p)); TRY(js_regexp_create(p, js_intern("", 0), &re)); }
    int caps[MAXCAP * 2];
    int n = js_regexp_exec(re, s, 0, caps, MAXCAP);
    if (n < 0) return -1;
    *ret = js_num(n ? caps[0] : -1);
    return 0;
}

/* $1, $&, $` ... в строке замены */
static void expand_repl(jsbuf *b, jsstr *repl, jsstr *s, int *caps, int n) {
    for (int i = 0; i < repl->len; i++) {
        char c = repl->s[i];
        if (c != '$' || i + 1 >= repl->len) { jb_addc(b, c); continue; }
        char d = repl->s[i + 1];
        if (d == '$') { jb_addc(b, '$'); i++; }
        else if (d == '&') { jb_add(b, s->s + caps[0], caps[1] - caps[0]); i++; }
        else if (d == '`') { jb_add(b, s->s, caps[0]); i++; }
        else if (d == '\'') { jb_add(b, s->s + caps[1], s->len - caps[1]); i++; }
        else if (isdigit((ju8)d)) {
            int g = d - '0', used = 1;
            if (i + 2 < repl->len && isdigit((ju8)repl->s[i + 2]) && g * 10 + (repl->s[i + 2] - '0') < n) { g = g * 10 + (repl->s[i + 2] - '0'); used = 2; }
            if (g > 0 && g < n) {
                if (caps[2 * g] >= 0) jb_add(b, s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]);
                i += used;
            } else jb_addc(b, c);
        } else jb_addc(b, c);
    }
}

static int do_replace(jsval thisv, int argc, jsval *argv, jsval *ret, int all) {
    jsstr *s;
    TRY(this_str(thisv, &s));
    jsval pat = ARG(0), rep = ARG(1);
    int fn = js_is_callable(rep);
    jsstr *rs = NULL;
    if (!fn) TRY(js_to_string(rep, &rs));
    jsbuf b = { 0 };
    int pos = 0;
    if (is_regexp(pat)) {
        jsobj *re = pat.u.o;
        int global = re_flag(re, 'g');
        int start = 0;
        if (!global && re_flag(re, 'y')) { if (get_last_index(re, &start) < 0) return -1; }
        while (start <= s->len) {
            int caps[MAXCAP * 2];
            int n = js_regexp_exec(re, s, start, caps, MAXCAP);
            if (n < 0) { jb_free(&b); return -1; }
            if (!n) break;
            jb_add(&b, s->s + pos, caps[0] - pos);
            if (fn) {
                jsval args[MAXCAP + 2], r;
                for (int i = 0; i < n; i++) args[i] = caps[2 * i] < 0 ? js_undef() : js_strv(js_strn(s->s + caps[2 * i], caps[2 * i + 1] - caps[2 * i]));
                args[n] = js_num(caps[0]);
                args[n + 1] = js_strv(s);
                jsstr *t;
                if (js_call(rep, js_undef(), n + 2, args, &r) < 0 || js_to_string(r, &t) < 0) { jb_free(&b); return -1; }
                jb_add(&b, t->s, t->len);
            } else expand_repl(&b, rs, s, caps, n);
            pos = caps[1];
            if (!global) break;
            start = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
            if (caps[1] == caps[0] && caps[0] < s->len) { jb_addc(&b, s->s[caps[0]]); pos = caps[0] + 1; }
        }
        if (global) js_set(pat, S_lastIndex, js_num(0));
    } else {
        jsstr *p;
        TRY(js_to_string(pat, &p));
        int from = 0;
        for (;;) {
            int i = find_str(s, p, from);
            if (i < 0) break;
            jb_add(&b, s->s + pos, i - pos);
            int caps[2] = { i, i + p->len };
            if (fn) {
                jsval args[3] = { js_strv(p), js_num(i), js_strv(s) }, r;
                jsstr *t;
                if (js_call(rep, js_undef(), 3, args, &r) < 0 || js_to_string(r, &t) < 0) { jb_free(&b); return -1; }
                jb_add(&b, t->s, t->len);
            } else expand_repl(&b, rs, s, caps, 1);
            pos = i + p->len;
            if (!all) break;
            from = p->len ? pos : pos + 1;
            if (!p->len) { if (i < s->len) jb_addc(&b, s->s[i]); pos = i + 1; if (pos > s->len) break; }
        }
    }
    if (pos < s->len) jb_add(&b, s->s + pos, s->len - pos);
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(StringProto_replace) { UNUSED; return do_replace(thisv, argc, argv, ret, 0); }
NATIVE(StringProto_replaceAll) { UNUSED; return do_replace(thisv, argc, argv, ret, 1); }

NATIVE(StringProto_split) {
    UNUSED;
    jsstr *s;
    TRY(this_str(thisv, &s));
    jsobj *a = js_new_array(0);
    double lim = 4294967295.0;
    if (argc > 1 && argv[1].tag != JS_UNDEF) TRY(js_to_number(argv[1], &lim));
    jsval sep = ARG(0);
    if (lim == 0) { *ret = js_objv(a); return 0; }
    if (sep.tag == JS_UNDEF) { js_array_push(a, js_strv(s)); *ret = js_objv(a); return 0; }
    if (is_regexp(sep)) {
        jsobj *re = sep.u.o;
        if (!s->len) {
            int caps[MAXCAP * 2];
            if (!js_regexp_exec(re, s, 0, caps, MAXCAP)) js_array_push(a, js_strv(s));
            *ret = js_objv(a);
            return 0;
        }
        int p = 0, q = 0;
        while (q < s->len) {
            int caps[MAXCAP * 2];
            int n = js_regexp_exec(re, s, q, caps, MAXCAP);
            if (n < 0) return -1;
            if (!n) break;
            if (caps[1] == caps[0] && caps[0] >= s->len) break;
            if (caps[1] == p || (caps[0] == caps[1] && caps[0] == p)) { q = caps[0] + 1; if (caps[1] > caps[0]) q = caps[1]; if (caps[0] == caps[1]) continue; }
            if (caps[0] == caps[1] && caps[0] <= p) { q = p + 1; continue; }
            js_array_push(a, js_strv(js_strn(s->s + p, caps[0] - p)));
            if (a->alen >= lim) { *ret = js_objv(a); return 0; }
            for (int i = 1; i < n; i++) js_array_push(a, caps[2 * i] < 0 ? js_undef() : js_strv(js_strn(s->s + caps[2 * i], caps[2 * i + 1] - caps[2 * i])));
            p = caps[1];
            q = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
        }
        js_array_push(a, js_strv(js_strn(s->s + p, s->len - p)));
        *ret = js_objv(a);
        return 0;
    }
    jsstr *t;
    TRY(js_to_string(sep, &t));
    if (!t->len) {                                          /* по символам UTF-8 */
        for (int i = 0; i < s->len && a->alen < lim;) {
            int l;
            cp_at(s, i, &l);
            js_array_push(a, js_strv(js_strn(s->s + i, l)));
            i += l;
        }
        *ret = js_objv(a);
        return 0;
    }
    int p = 0;
    for (;;) {
        int i = find_str(s, t, p);
        if (i < 0) break;
        js_array_push(a, js_strv(js_strn(s->s + p, i - p)));
        if (a->alen >= lim) { *ret = js_objv(a); return 0; }
        p = i + t->len;
    }
    js_array_push(a, js_strv(js_strn(s->s + p, s->len - p)));
    *ret = js_objv(a);
    return 0;
}

NATIVE(StringProto_normalize) { UNUSED; jsstr *s; TRY(this_str(thisv, &s)); *ret = js_strv(s); return 0; }

/* <a>, <b> и т. п. — старые методы String.prototype */
NATIVE(StringProto_link) {
    UNUSED;
    jsstr *s, *u;
    TRY(this_str(thisv, &s));
    TRY(str_arg(argc, argv, 0, &u));
    jsbuf b = { 0 };
    jb_adds(&b, "<a href=\"");
    jb_add(&b, u->s, u->len);
    jb_adds(&b, "\">");
    jb_add(&b, s->s, s->len);
    jb_adds(&b, "</a>");
    *ret = js_strv(jb_str(&b));
    return 0;
}

/* ------------------------------------------------------------------ JSON */

struct jstr_ctx { jsbuf b; jsval replacer; jsobj *allow; jsstr *gap; jsobj *stack[128]; int depth; };

static void json_quote(jsbuf *b, jsstr *s) {
    jb_addc(b, '"');
    for (int i = 0; i < s->len; i++) {
        ju8 c = s->s[i];
        switch (c) {
        case '"': jb_adds(b, "\\\""); break;
        case '\\': jb_adds(b, "\\\\"); break;
        case '\n': jb_adds(b, "\\n"); break;
        case '\r': jb_adds(b, "\\r"); break;
        case '\t': jb_adds(b, "\\t"); break;
        case '\b': jb_adds(b, "\\b"); break;
        case '\f': jb_adds(b, "\\f"); break;
        default:
            if (c < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", c); jb_adds(b, t); }
            else jb_addc(b, c);
        }
    }
    jb_addc(b, '"');
}

static void json_indent(struct jstr_ctx *c) {
    if (!c->gap || !c->gap->len) return;
    jb_addc(&c->b, '\n');
    for (int i = 0; i < c->depth; i++) jb_add(&c->b, c->gap->s, c->gap->len);
}

/* -> 1 записано, 0 пропустить (undefined/функция), -1 исключение */
static int json_value(struct jstr_ctx *c, jsval holder, jsstr *key, jsval v) {
    if (IS_OBJ(v)) {
        jsval f;
        TRY(js_get(v, S_toJSON, &f));
        if (js_is_callable(f)) { jsval k = js_strv(key); TRY(js_call(f, v, 1, &k, &v)); }
    }
    if (js_is_callable(c->replacer)) {
        jsval args[2] = { js_strv(key), v };
        TRY(js_call(c->replacer, holder, 2, args, &v));
    }
    if (IS_OBJ(v)) {
        jsobj *o = v.u.o;
        if (o->cls == C_NUMBER) v = o->iv[0];
        else if (o->cls == C_STRING) v = o->iv[0];
        else if (o->cls == C_BOOLEAN) v = o->iv[0];
    }
    switch (v.tag) {
    case JS_NULL: jb_adds(&c->b, "null"); return 1;
    case JS_BOOL: jb_adds(&c->b, v.u.b ? "true" : "false"); return 1;
    case JS_NUM: {
        if (js_isnan(v.u.n) || js_isinf(v.u.n)) { jb_adds(&c->b, "null"); return 1; }
        char t[40];
        js_num_to_str(v.u.n, t);
        jb_adds(&c->b, t);
        return 1;
    }
    case JS_STR: if (v.u.s->symbol) return 0; json_quote(&c->b, v.u.s); return 1;
    case JS_OBJ: break;
    default: return 0;
    }
    if (js_is_callable(v)) return 0;
    jsobj *o = v.u.o;
    for (int i = 0; i < c->depth; i++) if (c->stack[i] == o) return js_throw("TypeError", "Converting circular structure to JSON");
    if (c->depth >= 127) return js_throw("RangeError", "JSON nesting too deep");
    c->stack[c->depth++] = o;
    if (js_is_array(v)) {
        jb_addc(&c->b, '[');
        int len;
        TRY(js_array_len(v, &len));
        for (int i = 0; i < len; i++) {
            if (i) jb_addc(&c->b, ',');
            json_indent(c);
            jsval e;
            if (o->cls == C_ARRAY) e = i < o->alen && o->arr[i].tag != JS_EMPTY ? o->arr[i] : js_undef();
            else TRY(js_get(v, idx_str(i), &e));
            int r = json_value(c, v, idx_str(i), e);
            if (r < 0) return -1;
            if (!r) jb_adds(&c->b, "null");
        }
        c->depth--;
        if (len) json_indent(c);
        jb_addc(&c->b, ']');
        return 1;
    }
    jb_addc(&c->b, '{');
    jsobj *keys = c->allow ? c->allow : js_own_keys(o, 1);
    int any = 0;
    for (int i = 0; i < keys->alen; i++) {
        jsstr *k = keys->arr[i].u.s;
        jsval e;
        TRY(js_get(v, k, &e));
        int mark = c->b.n;
        if (any) jb_addc(&c->b, ',');
        json_indent(c);
        json_quote(&c->b, k);
        jb_addc(&c->b, ':');
        if (c->gap && c->gap->len) jb_addc(&c->b, ' ');
        int r = json_value(c, v, k, e);
        if (r < 0) return -1;
        if (!r) { c->b.n = mark; continue; }
        any = 1;
    }
    c->depth--;
    if (any) json_indent(c);
    jb_addc(&c->b, '}');
    return 1;
}

int js_json_stringify(jsval v, jsstr **out) {
    struct jstr_ctx c;
    memset(&c, 0, sizeof(c));
    int r = json_value(&c, js_undef(), js_intern("", 0), v);
    if (r < 0) { jb_free(&c.b); return -1; }
    *out = r ? jb_str(&c.b) : NULL;
    if (!r) jb_free(&c.b);
    return 0;
}

NATIVE(JSON_stringify) {
    UNUSED;
    struct jstr_ctx c;
    memset(&c, 0, sizeof(c));
    jsval rep = ARG(1), sp = ARG(2);
    if (js_is_callable(rep)) c.replacer = rep;
    else if (IS_OBJ(rep) && rep.u.o->cls == C_ARRAY) {
        c.allow = js_new_array(0);
        for (int i = 0; i < rep.u.o->alen; i++) { jsstr *k; TRY(js_to_string(rep.u.o->arr[i], &k)); js_array_push(c.allow, js_strv(js_intern_str(k))); }
    }
    if (sp.tag == JS_NUM) {
        int n = sp.u.n > 10 ? 10 : sp.u.n < 0 ? 0 : (int)sp.u.n;
        c.gap = js_strn("          ", n);
    } else if (sp.tag == JS_STR) c.gap = js_strn(sp.u.s->s, sp.u.s->len > 10 ? 10 : sp.u.s->len);
    jsobj *wrap = js_new_object();
    js_define(wrap, "", ARG(0), P_ENUM | P_WRITE | P_CONF);
    int r = json_value(&c, js_objv(wrap), js_intern("", 0), ARG(0));
    if (r < 0) { jb_free(&c.b); return -1; }
    if (!r) { jb_free(&c.b); *ret = js_undef(); return 0; }
    *ret = js_strv(jb_str(&c.b));
    return 0;
}

struct jparse { const char *s; int n, i; };

static void jp_ws(struct jparse *p) { while (p->i < p->n && is_ws(p->s[p->i])) p->i++; }

static int jp_err(struct jparse *p) {
    return js_throw("SyntaxError", "Unexpected token in JSON at position %d", p->i);
}

static int jp_value(struct jparse *p, jsval *out, int depth);

static int jp_string(struct jparse *p, jsstr **out) {
    p->i++;
    jsbuf b = { 0 };
    while (p->i < p->n && p->s[p->i] != '"') {
        char c = p->s[p->i++];
        if (c != '\\') { jb_addc(&b, c); continue; }
        if (p->i >= p->n) break;
        c = p->s[p->i++];
        switch (c) {
        case 'n': jb_addc(&b, '\n'); break;
        case 't': jb_addc(&b, '\t'); break;
        case 'r': jb_addc(&b, '\r'); break;
        case 'b': jb_addc(&b, '\b'); break;
        case 'f': jb_addc(&b, '\f'); break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4 && p->i < p->n; k++) {
                char h = tolower((ju8)p->s[p->i++]);
                cp = cp * 16 + (isdigit((ju8)h) ? h - '0' : h - 'a' + 10);
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 5 < p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                unsigned lo = 0;
                for (int k = 0; k < 4; k++) { char h = tolower((ju8)p->s[p->i + 2 + k]); lo = lo * 16 + (isdigit((ju8)h) ? h - '0' : h - 'a' + 10); }
                if (lo >= 0xDC00 && lo <= 0xDFFF) { p->i += 6; cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
            }
            put_cp(&b, cp);
            break;
        }
        default: jb_addc(&b, c);
        }
    }
    if (p->i >= p->n) { jb_free(&b); return jp_err(p); }
    p->i++;
    *out = jb_str(&b);
    return 0;
}

static int jp_value(struct jparse *p, jsval *out, int depth) {
    if (depth > 500) return js_throw("RangeError", "JSON nesting too deep");
    jp_ws(p);
    if (p->i >= p->n) return jp_err(p);
    char c = p->s[p->i];
    if (c == '{') {
        p->i++;
        jsobj *o = js_new_object();
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; *out = js_objv(o); return 0; }
        for (;;) {
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') return jp_err(p);
            jsstr *k;
            TRY(jp_string(p, &k));
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') return jp_err(p);
            p->i++;
            jsval v;
            TRY(jp_value(p, &v, depth + 1));
            js_define_key(o, k, v, P_ENUM | P_WRITE | P_CONF);
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
            return jp_err(p);
        }
        *out = js_objv(o);
        return 0;
    }
    if (c == '[') {
        p->i++;
        jsobj *a = js_new_array(0);
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; *out = js_objv(a); return 0; }
        for (;;) {
            jsval v;
            TRY(jp_value(p, &v, depth + 1));
            js_array_push(a, v);
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
            return jp_err(p);
        }
        *out = js_objv(a);
        return 0;
    }
    if (c == '"') { jsstr *s; TRY(jp_string(p, &s)); *out = js_strv(s); return 0; }
    if (c == '-' || isdigit((ju8)c)) {
        int used;
        double d = js_parse_float(p->s + p->i, p->n - p->i, &used);
        if (!used) return jp_err(p);
        p->i += used;
        *out = js_num(d);
        return 0;
    }
    if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "true", 4)) { p->i += 4; *out = js_bool(1); return 0; }
    if (p->n - p->i >= 5 && !strncmp(p->s + p->i, "false", 5)) { p->i += 5; *out = js_bool(0); return 0; }
    if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "null", 4)) { p->i += 4; *out = js_null(); return 0; }
    return jp_err(p);
}

int js_json_parse(const char *s, int n, jsval *out) {
    struct jparse p = { s, n, 0 };
    TRY(jp_value(&p, out, 0));
    jp_ws(&p);
    if (p.i < p.n) return jp_err(&p);
    return 0;
}

static int revive(jsval holder, jsstr *key, jsval fn, jsval *out) {
    jsval v;
    TRY(js_get(holder, key, &v));
    if (IS_OBJ(v)) {
        jsobj *keys = js_own_keys(v.u.o, 1);
        for (int i = 0; i < keys->alen; i++) {
            jsval nv;
            TRY(revive(v, keys->arr[i].u.s, fn, &nv));
            if (nv.tag == JS_UNDEF) js_delete(v.u.o, keys->arr[i].u.s);
            else TRY(js_set(v, keys->arr[i].u.s, nv));
        }
    }
    jsval args[2] = { js_strv(key), v };
    return js_call(fn, holder, 2, args, out);
}

NATIVE(JSON_parse) {
    UNUSED;
    jsstr *s;
    TRY(str_arg(argc, argv, 0, &s));
    TRY(js_json_parse(s->s, s->len, ret));
    if (js_is_callable(ARG(1))) {
        jsobj *root = js_new_object();
        js_define(root, "", *ret, P_ENUM | P_WRITE | P_CONF);
        return revive(js_objv(root), js_intern("", 0), argv[1], ret);
    }
    return 0;
}

/* ------------------------------------------------------------------ Date */

static double days_from_civil(double y, double m, double d) {   /* m: 1..12 */
    y -= m <= 2;
    double era = js_floor(y / 400);
    double yoe = y - era * 400;
    double mp = m > 2 ? m - 3 : m + 9;
    double doy = js_floor((153 * mp + 2) / 5) + d - 1;
    double doe = yoe * 365 + js_floor(yoe / 4) - js_floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(double z, int *y, int *m, int *d) {
    z += 719468;
    double era = js_floor(z / 146097);
    double doe = z - era * 146097;
    double yoe = js_floor((doe - js_floor(doe / 1460) + js_floor(doe / 36524) - js_floor(doe / 146096)) / 365);
    double yy = yoe + era * 400;
    double doy = doe - (365 * yoe + js_floor(yoe / 4) - js_floor(yoe / 100));
    double mp = js_floor((5 * doy + 2) / 153);
    *d = (int)(doy - js_floor((153 * mp + 2) / 5) + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static double make_time(double y, double mon, double d, double h, double mi, double s, double ms) {
    double ym = y + js_floor(mon / 12);
    double mn = js_fmod(mon, 12);
    if (mn < 0) mn += 12;
    return (days_from_civil(ym, mn + 1, 1) + d - 1) * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}

struct dparts { int y, m, d, h, mi, s, ms, wd; };
static void split_time(double t, struct dparts *p) {
    double days = js_floor(t / 86400000.0);
    double rem = t - days * 86400000.0;
    civil_from_days(days, &p->y, &p->m, &p->d);
    p->m--;
    p->h = (int)(rem / 3600000.0);
    p->mi = (int)(js_fmod(rem, 3600000.0) / 60000.0);
    p->s = (int)(js_fmod(rem, 60000.0) / 1000.0);
    p->ms = (int)js_fmod(rem, 1000.0);
    double w = js_fmod(days + 4, 7);
    if (w < 0) w += 7;
    p->wd = (int)w;
}

static double parse_date(jsstr *s) {
    const char *p = s->s;
    int y = 0, mo = 1, d = 1, h = 0, mi = 0, se = 0, ms = 0, n = 0, neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    while (isdigit((ju8)*p)) { y = y * 10 + (*p++ - '0'); n++; }
    if (n >= 4 && (*p == '-' || !*p)) {                       /* ISO 8601 */
        if (neg) y = -y;
        if (*p == '-') { p++; mo = 0; while (isdigit((ju8)*p)) mo = mo * 10 + (*p++ - '0'); }
        if (*p == '-') { p++; d = 0; while (isdigit((ju8)*p)) d = d * 10 + (*p++ - '0'); }
        if (*p == 'T' || *p == ' ') {
            p++;
            while (isdigit((ju8)*p)) h = h * 10 + (*p++ - '0');
            if (*p == ':') { p++; while (isdigit((ju8)*p)) mi = mi * 10 + (*p++ - '0'); }
            if (*p == ':') { p++; while (isdigit((ju8)*p)) se = se * 10 + (*p++ - '0'); }
            if (*p == '.') { p++; int k = 0; while (isdigit((ju8)*p)) { if (k < 3) ms = ms * 10 + (*p - '0'); k++; p++; } while (k < 3) { ms *= 10; k++; } }
        }
        double t = make_time(y, mo - 1, d, h, mi, se, ms);
        if (*p == '+' || *p == '-') {
            int sg = *p == '-' ? -1 : 1, oh = 0, om = 0;
            p++;
            while (isdigit((ju8)*p) && oh < 100) oh = oh * 10 + (*p++ - '0');
            if (*p == ':') p++;
            while (isdigit((ju8)*p)) om = om * 10 + (*p++ - '0');
            if (oh >= 100) { om = oh % 100; oh /= 100; }
            t -= sg * (oh * 3600000.0 + om * 60000.0);
        }
        return t;
    }
    /* «Mon, 05 Nov 2026 10:00:00 GMT» / «November 5, 2026» */
    static const char *mons = "janfebmaraprmayjunjulaugsepoctnovdec";
    int day = -1, month = -1, year = -1;
    p = s->s;
    while (*p) {
        if (isalpha((ju8)*p)) {
            char w[4] = { tolower((ju8)p[0]), p[1] ? tolower((ju8)p[1]) : 0, p[1] && p[2] ? tolower((ju8)p[2]) : 0, 0 };
            for (int i = 0; i < 12; i++) if (!strncmp(mons + i * 3, w, 3)) month = i;
            while (isalpha((ju8)*p)) p++;
        } else if (isdigit((ju8)*p)) {
            int v = 0, k = 0;
            while (isdigit((ju8)*p)) { v = v * 10 + (*p++ - '0'); k++; }
            if (*p == ':') {
                h = v; p++;
                mi = 0; while (isdigit((ju8)*p)) mi = mi * 10 + (*p++ - '0');
                if (*p == ':') { p++; se = 0; while (isdigit((ju8)*p)) se = se * 10 + (*p++ - '0'); }
            } else if (k == 4 || v > 31) year = v;
            else if (day < 0) day = v;
            else year = v;
        } else p++;
    }
    if (day < 0 || month < 0 || year < 0) return JS_NAN;
    return make_time(year, month, day, h, mi, se, 0);
}

static int this_date(jsval t, double *v) {
    if (IS_OBJ(t) && t.u.o->cls == C_DATE) { *v = t.u.o->iv[0].u.n; return 0; }
    return js_throw("TypeError", "this is not a Date object.");
}

static const char *wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mnames[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static jsval date_string(double t, int kind) {
    if (js_isnan(t)) return js_cstr("Invalid Date");
    struct dparts p;
    split_time(t, &p);
    char b[80];
    switch (kind) {
    case 0: snprintf(b, sizeof(b), "%s %s %02d %04d %02d:%02d:%02d GMT+0000", wdays[p.wd], mnames[p.m], p.d, p.y, p.h, p.mi, p.s); break;
    case 1: snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.y, p.m + 1, p.d, p.h, p.mi, p.s, p.ms); break;
    case 2: snprintf(b, sizeof(b), "%s %s %02d %04d", wdays[p.wd], mnames[p.m], p.d, p.y); break;
    case 3: snprintf(b, sizeof(b), "%02d:%02d:%02d GMT+0000", p.h, p.mi, p.s); break;
    case 4: snprintf(b, sizeof(b), "%s, %02d %s %04d %02d:%02d:%02d GMT", wdays[p.wd], p.d, mnames[p.m], p.y, p.h, p.mi, p.s); break;
    case 5: snprintf(b, sizeof(b), "%d/%d/%d, %02d:%02d:%02d", p.m + 1, p.d, p.y, p.h, p.mi, p.s); break;
    case 6: snprintf(b, sizeof(b), "%d/%d/%d", p.m + 1, p.d, p.y); break;
    default: snprintf(b, sizeof(b), "%02d:%02d:%02d", p.h, p.mi, p.s); break;
    }
    return js_cstr(b);
}

static double time_clip(double t) { return (js_isnan(t) || t > 8.64e15 || t < -8.64e15) ? JS_NAN : (t < 0 ? js_ceil(t) : js_floor(t)); }

NATIVE(Date_ctor) {
    UNUSED;
    if (!js_is_construct) { *ret = date_string(js_floor(js_now_ms()), 0); return 0; }
    double t;
    if (argc == 0) t = js_floor(js_now_ms());
    else if (argc == 1) {
        jsval v = argv[0];
        if (IS_OBJ(v) && v.u.o->cls == C_DATE) t = v.u.o->iv[0].u.n;
        else {
            TRY(js_to_primitive(v, 0, &v));
            if (v.tag == JS_STR) t = parse_date(v.u.s);
            else TRY(js_to_number(v, &t));
        }
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < 7 && i < argc; i++) TRY(js_to_number(argv[i], &f[i]));
        if (f[0] >= 0 && f[0] <= 99) f[0] += 1900;
        t = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    }
    jsobj *o = IS_OBJ(thisv) ? thisv.u.o : js_new_object_proto(js_date_proto);
    o->cls = C_DATE;
    o->iv[0] = js_num(time_clip(t));
    *ret = js_objv(o);
    return 0;
}

NATIVE(Date_now) { UNUSED; *ret = js_num(js_floor(js_now_ms())); return 0; }
NATIVE(Date_parse) { UNUSED; jsstr *s; TRY(str_arg(argc, argv, 0, &s)); *ret = js_num(parse_date(s)); return 0; }
NATIVE(Date_UTC) {
    UNUSED;
    double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < 7 && i < argc; i++) TRY(js_to_number(argv[i], &f[i]));
    if (f[0] >= 0 && f[0] <= 99) f[0] += 1900;
    *ret = js_num(time_clip(make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6])));
    return 0;
}

enum { DG_TIME, DG_YEAR, DG_MONTH, DG_DATE, DG_DAY, DG_HOURS, DG_MIN, DG_SEC, DG_MS, DG_TZ };
static int date_get(jsval thisv, int what, jsval *ret) {
    double t = 0;
    TRY(this_date(thisv, &t));
    if (what == DG_TIME) { *ret = js_num(t); return 0; }
    if (what == DG_TZ) { *ret = js_num(0); return 0; }
    if (js_isnan(t)) { *ret = js_num(JS_NAN); return 0; }
    struct dparts p;
    split_time(t, &p);
    int v[] = { 0, p.y, p.m, p.d, p.wd, p.h, p.mi, p.s, p.ms };
    *ret = js_num(v[what]);
    return 0;
}
#define DGET(name, w) NATIVE(DateProto_##name) { UNUSED; return date_get(thisv, w, ret); }
DGET(getTime, DG_TIME) DGET(getFullYear, DG_YEAR) DGET(getMonth, DG_MONTH) DGET(getDate, DG_DATE)
DGET(getDay, DG_DAY) DGET(getHours, DG_HOURS) DGET(getMinutes, DG_MIN) DGET(getSeconds, DG_SEC)
DGET(getMilliseconds, DG_MS) DGET(getTimezoneOffset, DG_TZ)
NATIVE(DateProto_getYear) { UNUSED; TRY(date_get(thisv, DG_YEAR, ret)); ret->u.n -= 1900; return 0; }

static int date_set(jsval thisv, int argc, jsval *argv, int what, jsval *ret) {
    double t = 0;
    TRY(this_date(thisv, &t));
    double a[4];
    for (int i = 0; i < 4; i++) a[i] = JS_NAN;
    for (int i = 0; i < argc && i < 4; i++) TRY(js_to_number(argv[i], &a[i]));
    if (what == DG_TIME) t = a[0];
    else {
        if (js_isnan(t)) t = 0;
        struct dparts p;
        split_time(t, &p);
        double f[7] = { p.y, p.m, p.d, p.h, p.mi, p.s, p.ms };
        int base = what == DG_YEAR ? 0 : what == DG_MONTH ? 1 : what == DG_DATE ? 2 : what == DG_HOURS ? 3 : what == DG_MIN ? 4 : what == DG_SEC ? 5 : 6;
        for (int i = 0; i < argc && base + i < 7 && i < 4; i++) f[base + i] = a[i];
        t = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    }
    t = time_clip(t);
    thisv.u.o->iv[0] = js_num(t);
    *ret = js_num(t);
    return 0;
}
#define DSET(name, w) NATIVE(DateProto_##name) { UNUSED; return date_set(thisv, argc, argv, w, ret); }
DSET(setTime, DG_TIME) DSET(setFullYear, DG_YEAR) DSET(setMonth, DG_MONTH) DSET(setDate, DG_DATE)
DSET(setHours, DG_HOURS) DSET(setMinutes, DG_MIN) DSET(setSeconds, DG_SEC) DSET(setMilliseconds, DG_MS)

#define DSTR(name, k) NATIVE(DateProto_##name) { UNUSED; double t = 0; TRY(this_date(thisv, &t)); *ret = date_string(t, k); return 0; }
DSTR(toString, 0) DSTR(toDateString, 2) DSTR(toTimeString, 3) DSTR(toUTCString, 4) DSTR(toLocaleString, 5)
DSTR(toLocaleDateString, 6) DSTR(toLocaleTimeString, 7)
NATIVE(DateProto_toISOString) {
    UNUSED;
    double t = 0;
    TRY(this_date(thisv, &t));
    if (js_isnan(t)) return js_throw("RangeError", "Invalid time value");
    *ret = date_string(t, 1);
    return 0;
}
NATIVE(DateProto_toJSON) {
    UNUSED;
    double t = 0;
    TRY(this_date(thisv, &t));
    *ret = js_isnan(t) ? js_null() : date_string(t, 1);
    return 0;
}

/* ------------------------------------------------------------------ Map / Set */

static int map_find(jsobj *m, jsval k, int step) {
    for (int i = 0; i < m->alen; i += step) {
        jsval x = m->arr[i];
        if (x.tag == JS_EMPTY) continue;
        if (same_value_zero(x, k)) return i;
    }
    return -1;
}

static int this_coll(jsval thisv, int cls, jsobj **o) {
    if (IS_OBJ(thisv) && thisv.u.o->cls == cls) { *o = thisv.u.o; return 0; }
    return js_throw("TypeError", "Method called on incompatible receiver");
}

static void coll_compact(jsobj *m, int step) {           /* убрать удалённые, если их много */
    int live = (int)m->iv[0].u.n * step;
    if (m->alen < 64 || live * 2 > m->alen) return;
    int w = 0;
    for (int i = 0; i < m->alen; i += step) {
        if (m->arr[i].tag == JS_EMPTY) continue;
        for (int k = 0; k < step; k++) m->arr[w + k] = m->arr[i + k];
        w += step;
    }
    m->alen = w;
}

static int coll_init(jsval thisv, int cls, jsobj *proto, int argc, jsval *argv, jsval *ret) {
    if (!js_is_construct) return js_throw("TypeError", "Constructor requires 'new'");
    jsobj *m = IS_OBJ(thisv) ? thisv.u.o : js_new_object_proto(proto);
    m->cls = cls;
    m->iv[0] = js_num(0);
    js_is_construct = 0;
    *ret = js_objv(m);
    if (argc > 0 && !IS_NULLISH(argv[0])) {
        struct js_iter it;
        TRY(js_iter_open(argv[0], &it));
        for (;;) {
            jsval v;
            int done;
            TRY(js_iter_step(&it, &v, &done));
            if (done) break;
            if (cls == C_MAP) {
                jsval k, x;
                TRY(js_get(v, idx_str(0), &k));
                TRY(js_get(v, idx_str(1), &x));
                int i = map_find(m, k, 2);
                if (i >= 0) m->arr[i + 1] = x;
                else { js_array_push(m, k); js_array_push(m, x); m->iv[0].u.n++; }
            } else if (map_find(m, v, 1) < 0) { js_array_push(m, v); m->iv[0].u.n++; }
        }
    }
    return 0;
}

NATIVE(Map_ctor) { UNUSED; return coll_init(thisv, C_MAP, js_map_proto, argc, argv, ret); }
NATIVE(Set_ctor) { UNUSED; return coll_init(thisv, C_SET, js_set_proto, argc, argv, ret); }

NATIVE(MapProto_get) { UNUSED; jsobj *m = NULL; TRY(this_coll(thisv, C_MAP, &m)); int i = map_find(m, ARG(0), 2); *ret = i >= 0 ? m->arr[i + 1] : js_undef(); return 0; }
NATIVE(MapProto_has) { UNUSED; jsobj *m = NULL; TRY(this_coll(thisv, C_MAP, &m)); *ret = js_bool(map_find(m, ARG(0), 2) >= 0); return 0; }
NATIVE(MapProto_set) {
    UNUSED;
    jsobj *m = NULL;
    TRY(this_coll(thisv, C_MAP, &m));
    jsval k = ARG(0);
    if (k.tag == JS_NUM && k.u.n == 0) k = js_num(0);
    int i = map_find(m, k, 2);
    if (i >= 0) m->arr[i + 1] = ARG(1);
    else { js_array_push(m, k); js_array_push(m, ARG(1)); m->iv[0].u.n++; }
    *ret = thisv;
    return 0;
}
NATIVE(MapProto_delete) {
    UNUSED;
    jsobj *m = NULL;
    TRY(this_coll(thisv, C_MAP, &m));
    int i = map_find(m, ARG(0), 2);
    if (i >= 0) { m->arr[i].tag = JS_EMPTY; m->arr[i + 1] = js_undef(); m->iv[0].u.n--; coll_compact(m, 2); }
    *ret = js_bool(i >= 0);
    return 0;
}
NATIVE(MapProto_clear) { UNUSED; jsobj *m; TRY(js_to_object(thisv, &m)); m->alen = 0; m->iv[0] = js_num(0); *ret = js_undef(); return 0; }
NATIVE(MapProto_size) { UNUSED; jsobj *m; TRY(js_to_object(thisv, &m)); *ret = m->iv[0]; return 0; }
NATIVE(MapProto_forEach) {
    UNUSED;
    jsobj *m;
    TRY(js_to_object(thisv, &m));
    int step = m->cls == C_MAP ? 2 : 1;
    for (int i = 0; i < m->alen; i += step) {
        if (m->arr[i].tag == JS_EMPTY) continue;
        jsval args[3] = { step == 2 ? m->arr[i + 1] : m->arr[i], m->arr[i], thisv }, r;
        TRY(js_call(ARG(0), ARG(1), 3, args, &r));
    }
    *ret = js_undef();
    return 0;
}
NATIVE(MapProto_entries) { UNUSED; *ret = make_iter(thisv, IT_MENTRIES); return 0; }
NATIVE(MapProto_keys) { UNUSED; *ret = make_iter(thisv, IT_MKEYS); return 0; }
NATIVE(MapProto_values) { UNUSED; *ret = make_iter(thisv, IT_MVALUES); return 0; }

NATIVE(SetProto_add) {
    UNUSED;
    jsobj *m = NULL;
    TRY(this_coll(thisv, C_SET, &m));
    if (map_find(m, ARG(0), 1) < 0) { js_array_push(m, ARG(0)); m->iv[0].u.n++; }
    *ret = thisv;
    return 0;
}
NATIVE(SetProto_has) { UNUSED; jsobj *m = NULL; TRY(this_coll(thisv, C_SET, &m)); *ret = js_bool(map_find(m, ARG(0), 1) >= 0); return 0; }
NATIVE(SetProto_delete) {
    UNUSED;
    jsobj *m = NULL;
    TRY(this_coll(thisv, C_SET, &m));
    int i = map_find(m, ARG(0), 1);
    if (i >= 0) { m->arr[i].tag = JS_EMPTY; m->iv[0].u.n--; coll_compact(m, 1); }
    *ret = js_bool(i >= 0);
    return 0;
}
NATIVE(SetProto_values) { UNUSED; *ret = make_iter(thisv, IT_SVALUES); return 0; }
NATIVE(SetProto_entries) { UNUSED; *ret = make_iter(thisv, IT_SENTRIES); return 0; }

/* ------------------------------------------------------------------ Promise */

/* iv[0] — состояние (0 ждёт, 1 выполнен, 2 отклонён), iv[1] — значение,
 * arr — ожидающие реакции тройками [onFulfilled, onRejected, производный promise] */

static jsobj *reaction_job;           /* встроенная функция, исполняющая реакцию */
static jsobj *thenable_job;

jsobj *js_new_promise(void) {
    jsobj *p = js_new_object_proto(js_promise_proto);
    p->cls = C_PROMISE;
    p->iv[0] = js_num(0);
    return p;
}

int js_promise_state(jsobj *p, jsval *v) { *v = p->iv[1]; return (int)p->iv[0].u.n; }

static void queue_reaction(jsval handler, jsval derived, jsval value, int rejected) {
    jsobj *rec = js_new_array(0);
    js_array_push(rec, handler);
    js_array_push(rec, derived);
    js_array_push(rec, value);
    js_array_push(rec, js_num(rejected));
    js_enqueue_job(js_objv(reaction_job), js_objv(rec));
}

static void settle(jsobj *p, int state, jsval v) {
    if (p->iv[0].u.n != 0) return;
    p->iv[0] = js_num(state);
    p->iv[1] = v;
    for (int i = 0; i + 2 < p->alen; i += 3) queue_reaction(p->arr[i + (state == 2)], p->arr[i + 2], v, state == 2);
    p->alen = 0;
}

void js_promise_reject_fn(jsobj *p, jsval v) { settle(p, 2, v); }

void js_promise_resolve_fn(jsobj *p, jsval v) {
    if (p->iv[0].u.n != 0) return;
    if (IS_OBJ(v) && v.u.o == p) { settle(p, 2, js_objv(js_make_error("TypeError", "Chaining cycle detected for promise"))); return; }
    if (IS_OBJ(v)) {
        if (v.u.o->cls == C_PROMISE) {                       /* подписаться на другой promise */
            jsobj *q = v.u.o;
            if (q->iv[0].u.n == 0) {
                js_array_push(q, js_undef());
                js_array_push(q, js_undef());
                js_array_push(q, js_objv(p));
            } else queue_reaction(js_undef(), js_objv(p), q->iv[1], q->iv[0].u.n == 2);
            return;
        }
        jsval then;
        if (js_get(v, S_then, &then) < 0) { jsval e = js_exc; js_exc = js_undef(); settle(p, 2, e); return; }
        if (js_is_callable(then)) {
            jsobj *rec = js_new_array(0);
            js_array_push(rec, js_objv(p));
            js_array_push(rec, v);
            js_array_push(rec, then);
            js_enqueue_job(js_objv(thenable_job), js_objv(rec));
            return;
        }
    }
    settle(p, 1, v);
}

NATIVE(Promise_reactionJob) {
    UNUSED;
    jsobj *rec = ARG(0).u.o;
    jsval handler = rec->arr[0], derived = rec->arr[1], value = rec->arr[2];
    int rejected = rec->arr[3].u.n != 0;
    jsobj *d = IS_OBJ(derived) ? derived.u.o : NULL;
    if (!js_is_callable(handler)) {
        if (d) { if (rejected) settle(d, 2, value); else js_promise_resolve_fn(d, value); }
        *ret = js_undef();
        return 0;
    }
    jsval r;
    if (js_call(handler, js_undef(), 1, &value, &r) < 0) {
        jsval e = js_exc;
        js_exc = js_undef();
        if (d) settle(d, 2, e);
    } else if (d) js_promise_resolve_fn(d, r);
    *ret = js_undef();
    return 0;
}

/* функции resolve/reject, которые получает executor: self->iv[0] — promise, iv[1] — 1 для reject */
NATIVE(Promise_resolver) {
    UNUSED;
    jsobj *p = self->iv[0].u.o;
    jsobj *flag = self->env ? (jsobj *)self->env : NULL;
    (void)flag;
    if (self->iv[1].tag == JS_OBJ && self->iv[1].u.o->iv[0].u.b) { *ret = js_undef(); return 0; }   /* уже вызвали */
    if (self->iv[1].tag == JS_OBJ) self->iv[1].u.o->iv[0] = js_bool(1);
    if (self->is_arrow) settle(p, 2, ARG(0));
    else js_promise_resolve_fn(p, ARG(0));
    *ret = js_undef();
    return 0;
}

static void make_resolvers(jsobj *p, jsval *res, jsval *rej) {
    jsobj *once = js_new_object();
    once->iv[0] = js_bool(0);
    jsobj *a = js_new_native(Promise_resolver, "resolve", 1), *b = js_new_native(Promise_resolver, "reject", 1);
    a->iv[0] = b->iv[0] = js_objv(p);
    a->iv[1] = b->iv[1] = js_objv(once);
    b->is_arrow = 1;
    *res = js_objv(a);
    *rej = js_objv(b);
}

NATIVE(Promise_thenableJob) {
    UNUSED;
    jsobj *rec = ARG(0).u.o;
    jsval res, rej, r;
    make_resolvers(rec->arr[0].u.o, &res, &rej);
    jsval args[2] = { res, rej };
    if (js_call(rec->arr[2], rec->arr[1], 2, args, &r) < 0) {
        jsval e = js_exc;
        js_exc = js_undef();
        js_call(rej, js_undef(), 1, &e, &r);
    }
    *ret = js_undef();
    return 0;
}

NATIVE(Promise_ctor) {
    UNUSED;
    if (!js_is_construct) return js_throw("TypeError", "Promise constructor cannot be invoked without 'new'");
    js_is_construct = 0;
    if (!js_is_callable(ARG(0))) return js_throw("TypeError", "Promise resolver is not a function");
    jsobj *p = IS_OBJ(thisv) ? thisv.u.o : js_new_promise();
    p->cls = C_PROMISE;
    p->iv[0] = js_num(0);
    jsval res, rej, r;
    make_resolvers(p, &res, &rej);
    jsval args[2] = { res, rej };
    if (js_call(argv[0], js_undef(), 2, args, &r) < 0) {
        jsval e = js_exc;
        js_exc = js_undef();
        js_call(rej, js_undef(), 1, &e, &r);
    }
    *ret = js_objv(p);
    return 0;
}

static jsobj *promise_then(jsobj *p, jsval onf, jsval onr) {
    jsobj *d = js_new_promise();
    if (p->iv[0].u.n == 0) {
        js_array_push(p, onf);
        js_array_push(p, onr);
        js_array_push(p, js_objv(d));
    } else queue_reaction(p->iv[0].u.n == 1 ? onf : onr, js_objv(d), p->iv[1], p->iv[0].u.n == 2);
    return d;
}

NATIVE(PromiseProto_then) {
    UNUSED;
    if (!IS_OBJ(thisv) || thisv.u.o->cls != C_PROMISE) return js_throw("TypeError", "Promise.prototype.then called on incompatible receiver");
    *ret = js_objv(promise_then(thisv.u.o, ARG(0), ARG(1)));
    return 0;
}

NATIVE(PromiseProto_catch) {
    UNUSED;
    jsval f;
    TRY(js_get(thisv, S_then, &f));
    jsval args[2] = { js_undef(), ARG(0) };
    return js_call(f, thisv, 2, args, ret);
}

/* finally: self->iv[0] — функция, is_arrow — это ветка «отклонён» */
NATIVE(Promise_finallyStep) {
    UNUSED;
    jsval r;
    if (js_is_callable(self->iv[0])) TRY(js_call(self->iv[0], js_undef(), 0, NULL, &r));
    if (self->is_arrow) return js_throw_val(ARG(0));
    *ret = ARG(0);
    return 0;
}

NATIVE(PromiseProto_finally) {
    UNUSED;
    if (!IS_OBJ(thisv) || thisv.u.o->cls != C_PROMISE) return js_throw("TypeError", "not a promise");
    jsobj *a = js_new_native(Promise_finallyStep, "", 1), *b = js_new_native(Promise_finallyStep, "", 1);
    a->iv[0] = b->iv[0] = ARG(0);
    b->is_arrow = 1;
    *ret = js_objv(promise_then(thisv.u.o, js_objv(a), js_objv(b)));
    return 0;
}

static jsobj *to_promise(jsval v) {
    if (IS_OBJ(v) && v.u.o->cls == C_PROMISE) return v.u.o;
    jsobj *p = js_new_promise();
    js_promise_resolve_fn(p, v);
    return p;
}

NATIVE(Promise_resolve) { UNUSED; *ret = js_objv(to_promise(ARG(0))); return 0; }
NATIVE(Promise_reject) { UNUSED; jsobj *p = js_new_promise(); settle(p, 2, ARG(0)); *ret = js_objv(p); return 0; }

/* Promise.all / allSettled / any: self->iv[0] — запись [результаты, осталось, итоговый promise], iv[1] — индекс */
enum { PA_ALL, PA_SETTLED_OK, PA_SETTLED_ERR, PA_ANY };

NATIVE(Promise_allStep) {
    UNUSED;
    jsobj *rec = self->iv[0].u.o;
    jsobj *vals = rec->arr[0].u.o, *res = rec->arr[2].u.o;
    int i = (int)self->iv[1].u.n, kind = (int)(long)self->ptr == 0 ? 0 : 0;
    kind = (int)rec->arr[3].u.n;
    if (self->is_arrow) kind = kind == PA_ALL ? -1 : kind == PA_SETTLED_OK ? PA_SETTLED_ERR : kind;
    jsval v = ARG(0);
    if (kind == -1) { settle(res, 2, v); *ret = js_undef(); return 0; }
    if (kind == PA_ANY && !self->is_arrow) { js_promise_resolve_fn(res, v); *ret = js_undef(); return 0; }
    if (kind == PA_SETTLED_OK || kind == PA_SETTLED_ERR) {
        jsobj *o = js_new_object();
        js_define(o, "status", js_cstr(kind == PA_SETTLED_OK ? "fulfilled" : "rejected"), P_ENUM | P_WRITE | P_CONF);
        js_define(o, kind == PA_SETTLED_OK ? "value" : "reason", v, P_ENUM | P_WRITE | P_CONF);
        v = js_objv(o);
    }
    vals->arr[i] = v;
    rec->arr[1].u.n--;
    if (rec->arr[1].u.n == 0) {
        if (kind == PA_ANY) settle(res, 2, js_objv(js_make_error("AggregateError", "All promises were rejected")));
        else js_promise_resolve_fn(res, js_objv(vals));
    }
    *ret = js_undef();
    return 0;
}

static int promise_combine(int argc, jsval *argv, jsval *ret, int kind) {
    jsobj *res = js_new_promise(), *vals = js_new_array(0);
    jsobj *rec = js_new_array(0);
    js_array_push(rec, js_objv(vals));
    js_array_push(rec, js_num(1));
    js_array_push(rec, js_objv(res));
    js_array_push(rec, js_num(kind));
    struct js_iter it;
    TRY(js_iter_open(ARG(0), &it));
    for (int i = 0;; i++) {
        jsval v;
        int done;
        TRY(js_iter_step(&it, &v, &done));
        if (done) break;
        js_array_push(vals, js_undef());
        rec->arr[1].u.n++;
        jsobj *ok = js_new_native(Promise_allStep, "", 1), *bad = js_new_native(Promise_allStep, "", 1);
        ok->iv[0] = bad->iv[0] = js_objv(rec);
        ok->iv[1] = bad->iv[1] = js_num(i);
        bad->is_arrow = 1;
        jsobj *p = to_promise(v);
        if (kind == PA_ANY) promise_then(p, js_objv(ok), js_objv(bad));
        else promise_then(p, js_objv(ok), js_objv(bad));
    }
    if (--rec->arr[1].u.n == 0) {
        if (kind == PA_ANY) settle(res, 2, js_objv(js_make_error("AggregateError", "All promises were rejected")));
        else js_promise_resolve_fn(res, js_objv(vals));
    }
    *ret = js_objv(res);
    return 0;
}

NATIVE(Promise_all) { UNUSED; return promise_combine(argc, argv, ret, PA_ALL); }
NATIVE(Promise_allSettled) { UNUSED; return promise_combine(argc, argv, ret, PA_SETTLED_OK); }
NATIVE(Promise_any) { UNUSED; return promise_combine(argc, argv, ret, PA_ANY); }

NATIVE(Promise_race) {
    UNUSED;
    jsobj *res = js_new_promise();
    jsval rs, rj;
    make_resolvers(res, &rs, &rj);
    struct js_iter it;
    TRY(js_iter_open(ARG(0), &it));
    for (;;) {
        jsval v;
        int done;
        TRY(js_iter_step(&it, &v, &done));
        if (done) break;
        promise_then(to_promise(v), rs, rj);
    }
    *ret = js_objv(res);
    return 0;
}

NATIVE(g_import) {                                         /* import() — модули не поддерживаются */
    UNUSED;
    jsobj *p = js_new_promise();
    js_promise_reject_fn(p, js_objv(js_make_error("TypeError", "dynamic import() is not supported")));
    *ret = js_objv(p);
    return 0;
}

NATIVE(g_queueMicrotask) { UNUSED; js_enqueue_job(ARG(0), js_undef()); *ret = js_undef(); return 0; }

NATIVE(g_structuredClone) {
    UNUSED;
    jsstr *s;
    TRY(js_json_stringify(ARG(0), &s));
    if (!s) { *ret = js_undef(); return 0; }
    return js_json_parse(s->s, s->len, ret);
}

/* ------------------------------------------------------------------ время */

double js_now_ms(void) {
#ifdef JS_HOST
    extern double js_host_now(void);
    return js_host_now();
#else
    static double base;
    static unsigned base_ticks;
    if (!base) {
        struct datetime t;
        datetime_get(&t);
        base = days_from_civil(t.year, t.month, t.day) * 86400000.0 + t.hour * 3600000.0 + t.min * 60000.0 + t.sec * 1000.0;
        base_ticks = ticks();
    }
    return base + (double)(ticks() - base_ticks);
#endif
}

/* ------------------------------------------------------------------ типизированные массивы
 * Упрощённо, на самом JS: это обычные массивы чисел (без общей памяти у
 * ArrayBuffer и без обрезания значений). Хватает, чтобы библиотеки загружались. */
static const char typed_src[] =
    "(function(g){\n"
    "function AB(n){ if(!(this instanceof AB)) throw new TypeError(\"Constructor ArrayBuffer requires 'new'\");"
    " this._b = new Array(n > 0 ? n | 0 : 0).fill(0); this.byteLength = this._b.length; }\n"
    "AB.prototype.slice = function(s, e){ var r = new AB(0); r._b = this._b.slice(s, e); r.byteLength = r._b.length; return r; };\n"
    "AB.isView = function(v){ return !!(v && v.buffer instanceof AB); };\n"
    "g.ArrayBuffer = AB; g.SharedArrayBuffer = AB;\n"
    "var names = ['Int8Array',1,'Uint8Array',1,'Uint8ClampedArray',1,'Int16Array',2,'Uint16Array',2,'Int32Array',4,"
    "'Uint32Array',4,'Float32Array',4,'Float64Array',8,'BigInt64Array',8,'BigUint64Array',8];\n"
    "var TA = function TypedArray(){};\n"
    "TA.prototype = Object.create(Array.prototype);\n"
    "TA.prototype.subarray = function(s, e){ return new this.constructor(Array.prototype.slice.call(this, s, e)); };\n"
    "TA.prototype.set = function(src, off){ off = off | 0; for (var i = 0; i < src.length; i++) this[off + i] = src[i]; };\n"
    "for (var i = 0; i < names.length; i += 2) (function(name, size){\n"
    "  function C(a, off, len){\n"
    "    if (!(this instanceof C)) throw new TypeError(\"Constructor \" + name + \" requires 'new'\");\n"
    "    var arr;\n"
    "    if (a === undefined || a === null || typeof a === 'number') arr = new Array(a > 0 ? a | 0 : 0).fill(0);\n"
    "    else if (a instanceof AB) { arr = size === 1 ? a._b.slice(off | 0, len === undefined ? a.byteLength : (off | 0) + len) : new Array(((len !== undefined ? len * size : a.byteLength - (off | 0)) / size) | 0).fill(0); }\n"
    "    else arr = Array.from(a, Number);\n"
    "    Object.setPrototypeOf(arr, C.prototype);\n"
    "    Object.defineProperty(arr, 'buffer', { value: a instanceof AB ? a : new AB(arr.length * size), writable: true });\n"
    "    Object.defineProperty(arr, 'byteLength', { value: arr.length * size, writable: true });\n"
    "    Object.defineProperty(arr, 'byteOffset', { value: off | 0, writable: true });\n"
    "    return arr;\n"
    "  }\n"
    "  C.prototype = Object.create(TA.prototype); C.prototype.constructor = C; C.BYTES_PER_ELEMENT = size; C.prototype.BYTES_PER_ELEMENT = size;\n"
    "  C.from = function(a, f){ return new C(Array.from(a, f)); }; C.of = function(){ return new C(Array.prototype.slice.call(arguments)); };\n"
    "  Object.defineProperty(C, 'name', { value: name });\n"
    "  g[name] = C;\n"
    "})(names[i], names[i + 1]);\n"
    "g.DataView = function DataView(buf){ this.buffer = buf; this.byteLength = buf ? buf.byteLength : 0; this.byteOffset = 0; };\n"
    "['Int8','Uint8','Int16','Uint16','Int32','Uint32','Float32','Float64'].forEach(function(t){\n"
    "  g.DataView.prototype['get' + t] = function(o){ return this.buffer._b[o] || 0; };\n"
    "  g.DataView.prototype['set' + t] = function(o, v){ this.buffer._b[o] = v; };\n"
    "});\n"
    "g.BigInt = function BigInt(v){ return Math.trunc(Number(v)); };\n"
    "g.FinalizationRegistry = function(){ this.register = function(){}; this.unregister = function(){}; };\n"
    "})(globalThis);\n";

/* ------------------------------------------------------------------ сборка */

void js_lib2_init(void) {
    jsobj *op = js_object_proto, *g = js_global;

    js_iter_proto = js_new_object_proto(op);
    js_defn(js_iter_proto, "next", Iter_next, 0);
    js_define_key(js_iter_proto, S_iterator, js_objv(js_new_native(Iter_self, "[Symbol.iterator]", 0)), P_WRITE | P_CONF);

    /* Array */
    js_array_proto = js_new_object_proto(op);
    js_array_proto->cls = C_ARRAY;
    jsobj *A = js_make_ctor("Array", Array_ctor, 1, js_array_proto);
    js_defn(A, "isArray", Array_isArray, 1);
    js_defn(A, "of", Array_of, 0);
    js_defn(A, "from", Array_from, 1);
    jsobj *ap = js_array_proto;
    struct { const char *n; jsnative f; int a; } am[] = {
        { "push", ArrayProto_push, 1 }, { "pop", ArrayProto_pop, 0 }, { "shift", ArrayProto_shift, 0 },
        { "unshift", ArrayProto_unshift, 1 }, { "slice", ArrayProto_slice, 2 }, { "splice", ArrayProto_splice, 2 },
        { "concat", ArrayProto_concat, 1 }, { "join", ArrayProto_join, 1 }, { "toString", ArrayProto_toString, 0 },
        { "toLocaleString", ArrayProto_toString, 0 }, { "reverse", ArrayProto_reverse, 0 },
        { "indexOf", ArrayProto_indexOf, 1 }, { "lastIndexOf", ArrayProto_lastIndexOf, 1 },
        { "includes", ArrayProto_includes, 1 }, { "forEach", ArrayProto_forEach, 1 }, { "map", ArrayProto_map, 1 },
        { "filter", ArrayProto_filter, 1 }, { "some", ArrayProto_some, 1 }, { "every", ArrayProto_every, 1 },
        { "find", ArrayProto_find, 1 }, { "findIndex", ArrayProto_findIndex, 1 }, { "findLast", ArrayProto_findLast, 1 },
        { "findLastIndex", ArrayProto_findLastIndex, 1 }, { "flatMap", ArrayProto_flatMap, 1 },
        { "reduce", ArrayProto_reduce, 1 }, { "reduceRight", ArrayProto_reduceRight, 1 }, { "flat", ArrayProto_flat, 0 },
        { "fill", ArrayProto_fill, 1 }, { "at", ArrayProto_at, 1 }, { "sort", ArrayProto_sort, 1 },
        { "keys", ArrayProto_keys, 0 }, { "values", ArrayProto_values, 0 }, { "entries", ArrayProto_entries, 0 },
    };
    for (unsigned i = 0; i < sizeof(am) / sizeof(am[0]); i++) js_defn(ap, am[i].n, am[i].f, am[i].a);
    js_define_key(ap, S_iterator, js_own_prop(ap, js_perm("values"))->v, P_WRITE | P_CONF);

    /* String */
    js_string_proto = js_new_object_proto(op);
    js_string_proto->cls = C_STRING;
    js_string_proto->iv[0] = js_strv(js_intern("", 0));
    jsobj *S = js_make_ctor("String", String_ctor, 1, js_string_proto);
    js_defn(S, "fromCharCode", String_fromCharCode, 1);
    js_defn(S, "fromCodePoint", String_fromCodePoint, 1);
    js_defn(S, "raw", String_raw, 1);
    struct { const char *n; jsnative f; int a; } sm[] = {
        { "toString", StringProto_toString, 0 }, { "valueOf", StringProto_toString, 0 }, { "charAt", StringProto_charAt, 1 },
        { "charCodeAt", StringProto_charCodeAt, 1 }, { "codePointAt", StringProto_codePointAt, 1 },
        { "indexOf", StringProto_indexOf, 1 }, { "lastIndexOf", StringProto_lastIndexOf, 1 },
        { "includes", StringProto_includes, 1 }, { "startsWith", StringProto_startsWith, 1 },
        { "endsWith", StringProto_endsWith, 1 }, { "slice", StringProto_slice, 2 }, { "substring", StringProto_substring, 2 },
        { "substr", StringProto_substr, 2 }, { "toUpperCase", StringProto_toUpperCase, 0 },
        { "toLowerCase", StringProto_toLowerCase, 0 }, { "toLocaleUpperCase", StringProto_toUpperCase, 0 },
        { "toLocaleLowerCase", StringProto_toLowerCase, 0 }, { "trim", StringProto_trim, 0 },
        { "trimStart", StringProto_trimStart, 0 }, { "trimEnd", StringProto_trimEnd, 0 },
        { "trimLeft", StringProto_trimStart, 0 }, { "trimRight", StringProto_trimEnd, 0 },
        { "repeat", StringProto_repeat, 1 }, { "padStart", StringProto_padStart, 2 }, { "padEnd", StringProto_padEnd, 2 },
        { "concat", StringProto_concat, 1 }, { "at", StringProto_at, 1 }, { "localeCompare", StringProto_localeCompare, 1 },
        { "match", StringProto_match, 1 }, { "matchAll", StringProto_matchAll, 1 }, { "search", StringProto_search, 1 },
        { "replace", StringProto_replace, 2 }, { "replaceAll", StringProto_replaceAll, 2 }, { "split", StringProto_split, 2 },
        { "normalize", StringProto_normalize, 0 }, { "link", StringProto_link, 1 },
    };
    for (unsigned i = 0; i < sizeof(sm) / sizeof(sm[0]); i++) js_defn(js_string_proto, sm[i].n, sm[i].f, sm[i].a);
    js_define_key(js_string_proto, S_iterator, js_objv(js_new_native(StringProto_iterator, "[Symbol.iterator]", 0)), P_WRITE | P_CONF);

    /* RegExp */
    js_regexp_proto = js_new_object_proto(op);
    jsobj *R = js_make_ctor("RegExp", RegExp_ctor, 2, js_regexp_proto);
    (void)R;
    js_defn(js_regexp_proto, "exec", RegExpProto_exec, 1);
    js_defn(js_regexp_proto, "test", RegExpProto_test, 1);
    js_defn(js_regexp_proto, "toString", RegExpProto_toString, 0);

    /* JSON */
    jsobj *J = js_new_object();
    js_define(g, "JSON", js_objv(J), P_WRITE | P_CONF);
    js_defn(J, "stringify", JSON_stringify, 3);
    js_defn(J, "parse", JSON_parse, 2);

    /* Date */
    js_date_proto = js_new_object_proto(op);
    jsobj *D = js_make_ctor("Date", Date_ctor, 7, js_date_proto);
    js_defn(D, "now", Date_now, 0);
    js_defn(D, "parse", Date_parse, 1);
    js_defn(D, "UTC", Date_UTC, 7);
    struct { const char *n; jsnative f; int a; } dm[] = {
        { "getTime", DateProto_getTime, 0 }, { "valueOf", DateProto_getTime, 0 }, { "getFullYear", DateProto_getFullYear, 0 },
        { "getMonth", DateProto_getMonth, 0 }, { "getDate", DateProto_getDate, 0 }, { "getDay", DateProto_getDay, 0 },
        { "getHours", DateProto_getHours, 0 }, { "getMinutes", DateProto_getMinutes, 0 }, { "getSeconds", DateProto_getSeconds, 0 },
        { "getMilliseconds", DateProto_getMilliseconds, 0 }, { "getTimezoneOffset", DateProto_getTimezoneOffset, 0 },
        { "getUTCFullYear", DateProto_getFullYear, 0 }, { "getUTCMonth", DateProto_getMonth, 0 }, { "getUTCDate", DateProto_getDate, 0 },
        { "getUTCDay", DateProto_getDay, 0 }, { "getUTCHours", DateProto_getHours, 0 }, { "getUTCMinutes", DateProto_getMinutes, 0 },
        { "getUTCSeconds", DateProto_getSeconds, 0 }, { "getUTCMilliseconds", DateProto_getMilliseconds, 0 }, { "getYear", DateProto_getYear, 0 },
        { "setTime", DateProto_setTime, 1 }, { "setFullYear", DateProto_setFullYear, 3 }, { "setMonth", DateProto_setMonth, 2 },
        { "setDate", DateProto_setDate, 1 }, { "setHours", DateProto_setHours, 4 }, { "setMinutes", DateProto_setMinutes, 3 },
        { "setSeconds", DateProto_setSeconds, 2 }, { "setMilliseconds", DateProto_setMilliseconds, 1 },
        { "setUTCFullYear", DateProto_setFullYear, 3 }, { "setUTCMonth", DateProto_setMonth, 2 }, { "setUTCDate", DateProto_setDate, 1 },
        { "setUTCHours", DateProto_setHours, 4 }, { "setUTCMinutes", DateProto_setMinutes, 3 }, { "setUTCSeconds", DateProto_setSeconds, 2 },
        { "setUTCMilliseconds", DateProto_setMilliseconds, 1 },
        { "toString", DateProto_toString, 0 }, { "toDateString", DateProto_toDateString, 0 }, { "toTimeString", DateProto_toTimeString, 0 },
        { "toUTCString", DateProto_toUTCString, 0 }, { "toGMTString", DateProto_toUTCString, 0 }, { "toISOString", DateProto_toISOString, 0 },
        { "toJSON", DateProto_toJSON, 1 }, { "toLocaleString", DateProto_toLocaleString, 0 },
        { "toLocaleDateString", DateProto_toLocaleDateString, 0 }, { "toLocaleTimeString", DateProto_toLocaleTimeString, 0 },
    };
    for (unsigned i = 0; i < sizeof(dm) / sizeof(dm[0]); i++) js_defn(js_date_proto, dm[i].n, dm[i].f, dm[i].a);

    /* Map, Set (WeakMap/WeakSet — то же самое, но без перебора) */
    js_map_proto = js_new_object_proto(op);
    js_make_ctor("Map", Map_ctor, 0, js_map_proto);
    js_defn(js_map_proto, "get", MapProto_get, 1);
    js_defn(js_map_proto, "set", MapProto_set, 2);
    js_defn(js_map_proto, "has", MapProto_has, 1);
    js_defn(js_map_proto, "delete", MapProto_delete, 1);
    js_defn(js_map_proto, "clear", MapProto_clear, 0);
    js_defn(js_map_proto, "forEach", MapProto_forEach, 1);
    js_defn(js_map_proto, "entries", MapProto_entries, 0);
    js_defn(js_map_proto, "keys", MapProto_keys, 0);
    js_defn(js_map_proto, "values", MapProto_values, 0);
    js_def_accessor(js_map_proto, "size", MapProto_size, NULL);
    js_define_key(js_map_proto, S_iterator, js_own_prop(js_map_proto, js_perm("entries"))->v, P_WRITE | P_CONF);
    jsobj *wmp = js_new_object_proto(js_map_proto);
    js_make_ctor("WeakMap", Map_ctor, 0, wmp);

    js_set_proto = js_new_object_proto(op);
    js_make_ctor("Set", Set_ctor, 0, js_set_proto);
    js_defn(js_set_proto, "add", SetProto_add, 1);
    js_defn(js_set_proto, "has", SetProto_has, 1);
    js_defn(js_set_proto, "delete", SetProto_delete, 1);
    js_defn(js_set_proto, "clear", MapProto_clear, 0);
    js_defn(js_set_proto, "forEach", MapProto_forEach, 1);
    js_defn(js_set_proto, "values", SetProto_values, 0);
    js_defn(js_set_proto, "keys", SetProto_values, 0);
    js_defn(js_set_proto, "entries", SetProto_entries, 0);
    js_def_accessor(js_set_proto, "size", MapProto_size, NULL);
    js_define_key(js_set_proto, S_iterator, js_own_prop(js_set_proto, js_perm("values"))->v, P_WRITE | P_CONF);
    jsobj *wsp = js_new_object_proto(js_set_proto);
    js_make_ctor("WeakSet", Set_ctor, 0, wsp);
    /* WeakMap(obj) у Map_ctor: proto берётся из new — уже правильно */

    /* Promise */
    js_promise_proto = js_new_object_proto(op);
    jsobj *P = js_make_ctor("Promise", Promise_ctor, 1, js_promise_proto);
    js_defn(js_promise_proto, "then", PromiseProto_then, 2);
    js_defn(js_promise_proto, "catch", PromiseProto_catch, 1);
    js_defn(js_promise_proto, "finally", PromiseProto_finally, 1);
    js_defn(P, "resolve", Promise_resolve, 1);
    js_defn(P, "reject", Promise_reject, 1);
    js_defn(P, "all", Promise_all, 1);
    js_defn(P, "allSettled", Promise_allSettled, 1);
    js_defn(P, "any", Promise_any, 1);
    js_defn(P, "race", Promise_race, 1);
    reaction_job = js_new_native(Promise_reactionJob, "", 1);
    thenable_job = js_new_native(Promise_thenableJob, "", 1);
    js_define(P, "%job", js_objv(reaction_job), 0);
    js_define(P, "%tjob", js_objv(thenable_job), 0);
    js_defn(g, "queueMicrotask", g_queueMicrotask, 1);
    js_define(g, "__import", js_objv(js_new_native(g_import, "import", 1)), 0);
    js_defn(g, "structuredClone", g_structuredClone, 1);

    extern void coro_init_protos(void);
    coro_init_protos();
    jsval r;
    if (js_eval(typed_src, sizeof(typed_src) - 1, "typed arrays", &r) < 0) {
        char e[200];
        js_print(js_error_text(js_exc, e, sizeof(e)), strlen(e));
        js_exc = js_undef();
    }
}
