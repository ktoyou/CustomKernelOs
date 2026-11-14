/* libpthread: pthread_create сам берёт стек через mmap и отдаёт его thread_create.
 * Join возвращает этот стек. Detach оставляет его ядру: поток снимет отображение при выходе.
 * Коды возврата — положительный errno, как в POSIX. */
#include "pthread.h"

#define REC_MAX 128
#define DEFAULT_STACK 65536u

struct boot { pthread_start_t fn; void *arg; };
struct rec { unsigned tid; void *stack; unsigned len; int live; };

static thread_mutex_t rec_mu;
static struct rec recs[REC_MAX];
static thread_mutex_t once_mu;
static thread_mutex_t key_mu;
static int key_used[THREAD_KEYS];
static unsigned key_gen[THREAD_KEYS];
static void (*key_dtor[THREAD_KEYS])(void *);

static int perr(int r) { return r < 0 ? -r : r; }

static int rec_add(unsigned tid, void *stack, unsigned len) {
    int i, e;
    e = thread_mutex_lock(&rec_mu);
    if (e) return perr(e);
    for (i = 0; i < REC_MAX; i++) if (!recs[i].live) {
        recs[i].live = 1;
        recs[i].tid = tid;
        recs[i].stack = stack;
        recs[i].len = len;
        thread_mutex_unlock(&rec_mu);
        return 0;
    }
    thread_mutex_unlock(&rec_mu);
    return EAGAIN;
}

static int rec_take(unsigned tid, void **stack, unsigned *len) {
    int i, e;
    e = thread_mutex_lock(&rec_mu);
    if (e) return -1;
    for (i = 0; i < REC_MAX; i++) if (recs[i].live && recs[i].tid == tid) {
        *stack = recs[i].stack;
        *len = recs[i].len;
        recs[i].live = 0;
        thread_mutex_unlock(&rec_mu);
        return 0;
    }
    thread_mutex_unlock(&rec_mu);
    return -1;
}

static void pthread_start(void *p) {
    struct boot *b = p;
    pthread_start_t fn = b->fn;
    void *arg = b->arg;
    free(b);
    pthread_exit(fn(arg));
}

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    attr->stacksize = DEFAULT_STACK;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *attr) { return attr ? 0 : EINVAL; }
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state) {
    if (!attr || (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED))
        return EINVAL;
    attr->detachstate = state;
    return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *attr, unsigned size) {
    if (!attr || size < PTHREAD_STACK_MIN || size > 1024u * 1024u) return EINVAL;
    attr->stacksize = size;
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, pthread_start_t start, void *arg) {
    unsigned size;
    int detach, tid, e;
    void *stack;
    struct boot *b;
    if (!thread || !start) return EINVAL;
    size = attr && attr->stacksize ? attr->stacksize : DEFAULT_STACK;
    detach = attr ? attr->detachstate : PTHREAD_CREATE_JOINABLE;
    if (size < PTHREAD_STACK_MIN || size > 1024u * 1024u) return EINVAL;
    stack = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == (void *)-1) return errno ? errno : ENOMEM;
    b = malloc(sizeof *b);
    if (!b) { munmap(stack, size); return ENOMEM; }
    b->fn = start;
    b->arg = arg;
    tid = thread_create(pthread_start, b, stack, size);
    if (tid < 0) { free(b); munmap(stack, size); return -tid; }
    e = rec_add((unsigned)tid, stack, size);
    if (e) { thread_join(tid, 0); munmap(stack, size); return e; }
    if (detach == PTHREAD_CREATE_DETACHED) {
        int de = thread_ctl(TC_DETACH, tid, 0, 0);
        void *st;
        unsigned ln;
        if (de < 0) {
            thread_join(tid, 0);
            rec_take((unsigned)tid, &st, &ln);
            munmap(stack, size);
            return -de;
        }
        rec_take((unsigned)tid, &st, &ln);
    }
    *thread = (unsigned)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    int code = 0, r;
    void *stack;
    unsigned len;
    r = thread_join((int)thread, &code);
    if (r < 0) return -r;
    if (retval) *retval = (void *)code;
    if (rec_take(thread, &stack, &len) == 0) munmap(stack, len);
    return 0;
}

void pthread_exit(void *retval) {
    struct thread_tcb *tcb = thread_get_tcb();
    int pass, i;
    for (pass = 0; pass < 4; pass++) {
        int any = 0;
        if (!tcb) break;
        for (i = 0; i < THREAD_KEYS; i++) {
            void *v;
            void (*d)(void *);
            thread_mutex_lock(&key_mu);
            d = 0;
            v = 0;
            if (key_used[i] && key_dtor[i] && tcb->key[i].gen == key_gen[i]) {
                v = tcb->key[i].val;
                tcb->key[i].val = 0;
                d = key_dtor[i];
            }
            thread_mutex_unlock(&key_mu);
            if (d && v) { d(v); any = 1; }
        }
        if (!any) break;
    }
    thread_exit((int)retval);
}

int pthread_detach(pthread_t thread) {
    int r = thread_ctl(TC_DETACH, (int)thread, 0, 0);
    void *stack;
    unsigned len;
    if (r < 0) return -r;
    rec_take(thread, &stack, &len);
    return 0;
}

pthread_t pthread_self(void) { return (pthread_t)gettid(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    int e;
    if (!once || !fn) return EINVAL;
    e = thread_mutex_lock(&once_mu);
    if (e) return perr(e);
    if (*once != 2) { *once = 1; fn(); *once = 2; }
    thread_mutex_unlock(&once_mu);
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    *attr = PTHREAD_MUTEX_DEFAULT;
    return 0;
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    return 0;
}
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE && type != PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    *attr = type;
    return 0;
}
int pthread_setcancelstate(int state, int *oldstate) {
    static int cur = PTHREAD_CANCEL_ENABLE;
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) return EINVAL;
    if (oldstate) *oldstate = cur;
    cur = state;
    return 0;
}
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    if (!m) return EINVAL;
    m->owner = 0;
    m->depth = 0;
    m->type = attr ? *attr : PTHREAD_MUTEX_DEFAULT;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (m->owner) return EBUSY;
    return 0;
}
static int mutex_held(pthread_mutex_t *m) {
    return m->type == PTHREAD_MUTEX_RECURSIVE && m->depth && m->owner == (unsigned)gettid();
}
int pthread_mutex_lock(pthread_mutex_t *m) {
    int r;
    if (!m) return EINVAL;
    if (mutex_held(m)) { m->depth++; return 0; }
    r = perr(thread_mutex_lock((thread_mutex_t *)m));
    if (!r) m->depth = 1;
    return r;
}
int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) return EINVAL;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->depth > 1 && m->owner == (unsigned)gettid()) {
        m->depth--;
        return 0;
    }
    m->depth = 0;
    return perr(thread_mutex_unlock((thread_mutex_t *)m));
}
int pthread_mutex_trylock(pthread_mutex_t *m) {
    int r;
    if (!m) return EINVAL;
    if (mutex_held(m)) { m->depth++; return 0; }
    r = perr(thread_mutex_trylock((thread_mutex_t *)m));
    if (!r) m->depth = 1;
    return r;
}

int pthread_cond_init(pthread_cond_t *c, const void *attr) {
    (void)attr;
    if (!c) return EINVAL;
    c->seq = 0;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) { return c ? 0 : EINVAL; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    if (!c || !m) return EINVAL;
    return perr(thread_ctl(TC_COND_WAIT, (int)c, (int)m, 0));
}
int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) return EINVAL;
    return perr(thread_ctl(TC_COND_WAKE, (int)c, 0, 0));
}
int pthread_cond_broadcast(pthread_cond_t *c) {
    if (!c) return EINVAL;
    return perr(thread_ctl(TC_COND_WAKE, (int)c, 1, 0));
}

int pthread_key_create(pthread_key_t *key, void (*dtor)(void *)) {
    int i, e;
    if (!key) return EINVAL;
    e = thread_mutex_lock(&key_mu);
    if (e) return perr(e);
    for (i = 0; i < THREAD_KEYS; i++) if (!key_used[i]) {
        key_gen[i]++;
        if (!key_gen[i]) key_gen[i] = 1;
        key_used[i] = 1;
        key_dtor[i] = dtor;
        *key = i;
        thread_mutex_unlock(&key_mu);
        return 0;
    }
    thread_mutex_unlock(&key_mu);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    int e;
    if (key < 0 || key >= THREAD_KEYS) return EINVAL;
    e = thread_mutex_lock(&key_mu);
    if (e) return perr(e);
    if (!key_used[key]) { thread_mutex_unlock(&key_mu); return EINVAL; }
    key_used[key] = 0;
    key_dtor[key] = 0;
    key_gen[key]++;
    if (!key_gen[key]) key_gen[key] = 1;
    thread_mutex_unlock(&key_mu);
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    struct thread_tcb *tcb;
    int e;
    if (key < 0 || key >= THREAD_KEYS) return EINVAL;
    tcb = thread_get_tcb();
    if (!tcb) return EAGAIN;
    e = thread_mutex_lock(&key_mu);
    if (e) return perr(e);
    if (!key_used[key]) { thread_mutex_unlock(&key_mu); return EINVAL; }
    tcb->key[key].gen = key_gen[key];
    tcb->key[key].val = (void *)value;
    thread_mutex_unlock(&key_mu);
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    struct thread_tcb *tcb;
    void *v;
    if (key < 0 || key >= THREAD_KEYS) return 0;
    tcb = thread_get_tcb();
    if (!tcb) return 0;
    thread_mutex_lock(&key_mu);
    v = 0;
    if (key_used[key] && tcb->key[key].gen == key_gen[key]) v = tcb->key[key].val;
    thread_mutex_unlock(&key_mu);
    return v;
}
