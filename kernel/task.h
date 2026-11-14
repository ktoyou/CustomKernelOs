#ifndef TASK_H
#define TASK_H
#include "types.h"
#include "idt.h"
#include "smp.h"

/* Задачи (потоки ядра и пользовательские процессы) и вытесняющий планировщик. */

#define MAX_TASKS   256
#define TASK_STACK  (16 * 1024)     /* стек ядра у каждой задачи */
#define TASK_NAME   16
#define TIME_SLICE  2               /* тиков таймера (по 10 мс) на задачу */
#define MAX_FDS     32  /* u32 cloexec; git/OpenSSL открывают много fd */

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED, TASK_ZOMBIE };

struct mutex;

struct file;

struct task {
    struct task *leader;           /* NULL for process leader; owns shared resources */
    u32 joiner;
    u32   id;
    u32   cpu;                     /* pinned CPU, assigned at task_start */
    bool  cpu_idle;
    char  name[TASK_NAME];
    enum task_state state;
    u32   esp;                      /* сохранённый указатель на struct regs */
    u8   *stack;                    /* стек ядра; NULL у задачи 0 (стек загрузчика) */
    void (*entry)(void *);
    void *arg;
    u32   wake_tick;                /* для SLEEPING */
    void *wait_chan;                /* для BLOCKED: чего ждём */
    u32   wait_deadline;            /* для BLOCKED с тайм-аутом: тик, когда сдаться (0 — ждать вечно) */
    struct mutex *holding;          /* какой мьютекс держим (чтобы отпустить при kill) */
    u32   cpu_ticks;
    u32   cpu_snapshot, cpu_recent; /* для CPU% за последнюю секунду */
    u32   created;

    /* --- только у пользовательских процессов --- */
    bool  user;
    u32   page_dir;                 /* физ. адрес каталога страниц (у потоков ядра — ядра) */
    u32   brk_start, brk;           /* куча программы (sbrk) */
    u32   user_pages;               /* сколько страниц RAM занимает программа */
    u32   cwd;                      /* текущий каталог (inode) */
    char *env_buf;                  /* упакованные строки KEY=VAL, принадлежат процессу */
    u32   env_len;
    struct file *syscall_fds[MAX_FDS]; /* transient refs while a syscall blocks */
    struct file *fds[MAX_FDS];      /* открытые файлы (общие объекты, см. file.h) */
    u32 uid;
    u32 real_uid, saved_uid, cloexec, fs_root;
    u32 gid, real_gid, saved_gid;
    u32 creation_mask, fs_account_scope, ipc_epoch;
    u32 user_stack_base,user_stack_size,guard_va,guard_flags;
    bool authenticated;
    u32   parent;                   /* id родителя (0 — ядро/сирота) */
    u32   pgid;                     /* группа процессов: Ctrl+C убивает всю группу */
    int   exit_code;
    bool  keep_zombie;              /* кто-то ждёт код выхода — не убирать раньше времени */
    bool  detached;                 /* pthread_detach: слот и mmap-стек освобождаются без join */
    u32   tcb;                      /* указатель программы: первое поле — её errno */
    bool  in_syscall;               /* сейчас выполняет системный вызов */
    bool  kill_pending;             /* убить при выходе из системного вызова */
    int   pending_code;             /* exit status, including zero; kill sets 137 */
    u8    fpu[108];                 /* регистры FPU (fnsave), только у процессов */
    struct shm_mapping { int id; u32 addr; } shm[16];   /* общая память (ipc.c) */
    struct mbox *mbox;              /* почтовый ящик для сообщений (ipc.c) */
};

struct mutex {                      /* рекурсивный мьютекс */
    struct task *owner;
    u32 depth;
};

void  fpu_init(void);               /* включить FPU, запомнить чистое состояние */
void  fpu_reset_current(void);      /* exec: новой программе — чистый FPU */
void  fpu_copy_current(struct task *to);   /* fork: потомку — копию */
void  tasking_init_ap(void);
void  tasking_init(void);           /* текущий поток (kmain) становится задачей 0 */
bool  tasking_ready(void);
struct task *task_current(void);
struct task *task_process(struct task *t);
bool task_has_threads(struct task *t);
int task_thread_create(u32 entry,u32 fn,u32 arg,u32 sp,u32 base,u32 size);
bool task_stack_busy(u32 dir,u32 start,u32 end);
bool task_same_identity(struct task *a,struct task *b);
bool task_can_control(struct task *caller,struct task *target);
int task_thread_join(u32 tid, int *code);
int task_thread_detach(u32 tid);
int task_thread_set_stack(u32 tid, u32 base, u32 len);
void task_set_pgid(struct task *p, u32 pgid);
void task_exit_group(int code) __attribute__((noreturn));
struct task *task_by_id(u32 id);
int   task_create(const char *name, void (*entry)(void *), void *arg);  /* -> id или <0 */
struct task *task_alloc_user(const char *name, u32 page_dir, u32 entry, u32 user_esp);
u32   task_start(struct task *t);   /* поставить в очередь -> id (0 = не вышло, t освобождена) */
void  task_exit_code(int code) __attribute__((noreturn));
void  task_exit(void) __attribute__((noreturn));
int   task_kill(u32 id);            /* можно звать и из обработчика прерывания */
int   task_kill_group(u32 pgid);    /* убить всех с этой группой -> сколько */
int   task_wait_child(int pid, int *code, bool nohang);  /* waitpid для процессов */
int   task_waitpid(u32 id, int *code);
void  task_reap(void);             /* освободить память завершившихся задач */
void  task_yield(void);
void  task_sleep_ticks(u32 ticks);
void  task_wait(void *chan);        /* заснуть до task_wakeup(chan); вызывать с cli */
bool  task_wait_timeout(void *chan, u32 ticks);   /* -> false, если вышло время */
void  task_wakeup(void *chan);
void  task_wake_one(void *chan);

void  mutex_lock(struct mutex *m);
void  mutex_unlock(struct mutex *m);
bool  mutex_trylock(struct mutex *m);   /* false: владелец другой, не засыпать */
bool task_raw_device_busy(int disk);

/* для idt.c */
struct regs *sched_tick(struct regs *r);
struct regs *schedule(struct regs *r);
struct regs *sched_maybe(struct regs *r);     /* после IRQ/syscall: надо ли переключиться */
bool  task_fault(struct regs *r, const char *what);
#define irq_nesting (cpu_states[smp_cpu()].nesting) /* >0 inside this CPU's IRQ */

/* для ps */
struct task_info {
    u32 id;
    char name[TASK_NAME];
    enum task_state state;
    bool user;
    u32 parent;
    u32 cpu_ticks, age_ticks;
    u32 cpu_percent;
    u32 mem_kb;
};
int   task_list(struct task_info *out, int max);
const char *task_state_name(enum task_state s);

#endif
