#ifndef PTHREAD_H
#define PTHREAD_H
/* Обёртка над потоками MyOS. Не NPTL: стек выделяет pthread_create,
 * errno и ключи лежат в thread_tcb, мьютекс — уже в ядре. */
#include "myos.h"
#include "signal.h"

typedef unsigned pthread_t;
typedef int pthread_once_t;
typedef int pthread_key_t;
typedef void *(*pthread_start_t)(void *);

typedef struct { unsigned owner; unsigned depth; int type; } pthread_mutex_t;
typedef int pthread_mutexattr_t;
typedef struct { unsigned seq; } pthread_cond_t;
typedef struct { int detachstate; unsigned stacksize; } pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0 }
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_STACK_MIN 4096u
#define PTHREAD_KEYS_MAX THREAD_KEYS

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_setstacksize(pthread_attr_t *attr, unsigned size);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, pthread_start_t start, void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval) __attribute__((noreturn));
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_once(pthread_once_t *once, void (*fn)(void));

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_setcancelstate(int state, int *oldstate);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const void *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int pthread_key_create(pthread_key_t *key, void (*dtor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

#endif
