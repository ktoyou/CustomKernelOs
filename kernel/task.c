/* Многозадачность: потоки ядра, пользовательские процессы, планировщик.
 *
 * КАК ПЕРЕКЛЮЧАЕТСЯ КОНТЕКСТ
 * Любое прерывание попадает в isr_common (isr.asm), который кладёт все регистры
 * задачи на стек — получается struct regs — и вызывает isr_handler(regs*).
 * isr_handler возвращает указатель на struct regs, из которого надо «вернуться».
 * Если вернуть regs другой задачи, isr_common сделает mov esp, eax, снимет её
 * регистры и выполнит iret — процессор продолжит работу уже в другой задаче.
 *
 * ПРОЦЕССЫ (ring 3)
 * У процесса свой каталог страниц и свой стек ядра. При переключении на него
 * мы загружаем его CR3 и пишем вершину его стека ядра в TSS (esp0): когда в
 * программе случится прерывание, процессор перейдёт именно на этот стек. */
#include "task.h"
#include "block.h"
#include "tty.h"
#include "ipc.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "vga.h"
#include "gdt.h"
#include "paging.h"
#include "pmm.h"
#include "file.h"
#include "fb.h"
#include "syscall.h"

#define CANARY       0xDEADC0DE
#define CANARY_WORDS 4
#define STACK_RED    2048         /* последние 2 КБ стека — запретная зона */

static struct task *tasks[MAX_TASKS];
#define current (cpu_states[smp_cpu()].running)
#define idle_task (cpu_states[smp_cpu()].idle)

/* VFS holds its lock, so no new raw opens can race this snapshot. */
bool task_raw_device_busy(int disk) {
    u32 flags=irq_save(); bool busy=false;
    for(int i=0;i<MAX_TASKS;i++) if(tasks[i])
        for(int j=0;j<MAX_FDS;j++) {
            struct file *f=tasks[i]->fds[j];
            if(f && f->type==FILE_DISK && f->ino>=0xf0000010u && f->ino<0xf0000010u+10 && (int)((f->ino-0xf0000010u)/5)==disk) busy=true;
        }
    irq_restore(flags); return busy;
}
static struct task  task0;          /* kmain / оболочка: живёт на стеке загрузчика */
static u32 next_id;
#define slice (cpu_states[smp_cpu()].slice)
#define need_resched (cpu_states[smp_cpu()].resched)
static u32 next_cpu;
static struct task ap_idle[SMP_MAX_CPUS];

/* ---------------------------------------------------------------- служебное */

struct task *task_current(void) { return current; }
struct task *task_process(struct task *t) { return t && t->leader ? t->leader : t; }
bool task_has_threads(struct task *t) {
    t = task_process(t);
    for (int i=0; i<MAX_TASKS; i++) if (tasks[i] && tasks[i]->leader == t) return true;
    return false;
}
static bool process_alive(struct task *p) {
    for (int i=0; i<MAX_TASKS; i++)
        if (tasks[i] && task_process(tasks[i]) == p && tasks[i]->state != TASK_ZOMBIE) return true;
    return false;
}
bool tasking_ready(void) { return current != NULL; }

const char *task_state_name(enum task_state s) {
    switch (s) {
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_SLEEPING: return "sleeping";
    case TASK_BLOCKED:  return "blocked";
    case TASK_ZOMBIE:   return "zombie";
    }
    return "?";
}

static int slot_of(struct task *t) {
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i] == t) return i;
    return -1;
}

struct task *task_by_id(u32 id) {
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i] && tasks[i]->id == id) return tasks[i];
    return NULL;
}

static bool canary_ok(struct task *t) {
    if (!t->stack) return true;
    u32 *c = (u32 *)t->stack;
    for (int i = 0; i < CANARY_WORDS; i++) if (c[i] != CANARY) return false;
    return true;
}

static void release_mutex(struct task *t) {
    if (t->holding && t->holding->owner == t) {
        t->holding->owner = NULL;
        t->holding->depth = 0;
        task_wakeup(t->holding);
    }
    t->holding = NULL;
}

static void close_all_files(struct task *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        file_close(t->fds[i]);
        t->fds[i] = NULL;
    }
}

/* mmap-стек отсоединённого потока. Чужой стек (куча, не mmap) не трогаем. */
static void release_user_stack(struct task *t) {
    u32 addr = t->user_stack_base, len = t->user_stack_size, end, va, n = 0;
    struct task *owner;
    if (!addr || !len || (addr & (PAGE_SIZE - 1)) || addr < MMAP_BASE) return;
    end = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (end < len || addr + end < addr || addr + end > MMAP_END) return;
    t->user_stack_base = 0;
    t->user_stack_size = 0;
    t->guard_va = 0;
    for (va = addr; va < addr + end; va += PAGE_SIZE) {
        u32 frame = vmm_unmap_in(t->page_dir, va);
        if (!frame) continue;
        pmm_free(frame);
        n++;
    }
    owner = task_process(t);
    if (owner && owner->user_pages >= n) owner->user_pages -= n;
}

/* Превратить задачу в зомби: стек и память освободит reap().
 * Файлы закрываем сразу — чтобы, например, читатель канала увидел конец. */
static void make_zombie(struct task *t, int code) {
    if (t->state == TASK_ZOMBIE) return;
    release_mutex(t);
    for (int i=0; i<MAX_FDS; i++) {
        struct file *f=t->syscall_fds[i]; t->syscall_fds[i]=NULL; file_close(f);
    }
    struct task *p = task_process(t);
    /* A thread exiting must not close the process descriptor table. */
    if (!t->leader) {
        for (int i=0; i<MAX_TASKS; i++) if (tasks[i] && tasks[i]->leader == t) {
            tasks[i]->keep_zombie = false;
            task_kill(tasks[i]->id);
        }
    }
    console_release(t->id);         /* если программа перевела терминал в сырой режим */
    tty_release(t->id);
    fb_release(t->id);              /* если программа захватила экран — вернуть консоль */
    t->exit_code = code;
    t->state = TASK_ZOMBIE;
    if (t->detached) {
        release_user_stack(t);
        t->keep_zombie = false;
    }
    if (!process_alive(p)) close_all_files(p);
    /* A dead joiner cannot retain another thread forever. */
    for (int i=0; i<MAX_TASKS; i++) if (tasks[i] && tasks[i]->joiner == t->id) tasks[i]->joiner=0;

    /* дети умершего становятся сиротами: их код выхода никто не заберёт */
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *c = tasks[i];
        if (c && !c->leader && c->parent == t->id && c->user) {
            c->parent = 0;
            c->keep_zombie = false;     /* ждать его больше некому — пусть reap() уберёт */
        }
    }
    task_wakeup(t);                 /* того, кто ждёт именно эту задачу (task_waitpid) */
    struct task *parent = t->parent ? task_by_id(t->parent) : NULL;
    if (parent) task_wakeup(parent);          /* родителя, ждущего любого ребёнка (waitpid -1) */
}

bool task_same_identity(struct task *a,struct task *b) {
    a=task_process(a);b=task_process(b);
    return a && b && (a==b || (a->authenticated && b->authenticated && a->uid==b->uid));
}
bool task_can_control(struct task *caller,struct task *target) {
    caller=task_process(caller);target=task_process(target);
    return caller && target && (!caller->user || (caller->authenticated && !caller->uid) || task_same_identity(caller,target));
}
bool task_stack_busy(u32 dir,u32 start,u32 end) {
    for(int i=0;i<MAX_TASKS;i++){struct task *t=tasks[i];
        if(t && t->leader && t->page_dir==dir && t->guard_va && start<t->user_stack_base+t->user_stack_size && t->user_stack_base<end)return true;
    }return false;
}
static void free_task(struct task *t) {
    if(t->leader && t->guard_va){u32 frame;
        if(vmm_lookup(t->page_dir,t->guard_va,&frame,NULL))vmm_map_in(t->page_dir,t->guard_va,frame,t->guard_flags);
        t->guard_va=0;
    }
    if (!t->leader) close_all_files(t);
    if (!t->leader && t->user && t->page_dir) { shm_release_all(t, t->page_dir); vmm_destroy_dir(t->page_dir); }
    msg_free(t);
    kfree(t->env_buf);
    kfree(t->stack);
    kfree(t);
}

/* Освободить завершившиеся задачи. Свой стек (и свой каталог страниц)
 * освобождать нельзя — поэтому это делает кто-то другой. */
void task_reap(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        u32 f = irq_save();
        struct task *t = tasks[i];
        /* каталог процесса нельзя освобождать, пока он загружен в CR3
         * (у потоков ядра общий каталог ядра — его не освобождаем вовсе) */
        if (t && t->state == TASK_ZOMBIE && !smp_task_active(t) && !t->keep_zombie && (t->leader || !task_has_threads(t)) &&
            !(t->user && !t->leader && t->page_dir == vmm_current_dir())) {
            tasks[i] = NULL;
            irq_restore(f);
            free_task(t);
            continue;
        }
        irq_restore(f);
    }
}

/* ---------------------------------------------------------------- FPU
 * Сопроцессор (x87) — общий для всех. Если программа A считала с double, а
 * планировщик переключился на B, которая тоже считает, регистры FPU надо
 * сохранить и потом вернуть. fnsave кладёт всё состояние (108 байт) в память,
 * frstor загружает обратно. Ядро само FPU не использует (-mno-80387). */
static u8 fpu_clean[108];

void fpu_init(void) {
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);                  /* EM=0: FPU есть, команды не эмулировать */
    cr0 &= ~(1u << 3);                  /* TS=0 */
    cr0 |= (1u << 1) | (1u << 5);       /* MP=1, NE=1: ошибки FPU — обычным исключением */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("fninit; fnsave %0" : "=m"(fpu_clean));
}

static inline void fpu_save(struct task *t)    { __asm__ volatile ("fnsave %0" : "=m"(t->fpu)); }
static inline void fpu_restore(struct task *t) { __asm__ volatile ("frstor %0" : : "m"(t->fpu)); }

void fpu_reset_current(void) {
    memcpy(current->fpu, fpu_clean, sizeof(fpu_clean));
    fpu_restore(current);
}

void fpu_copy_current(struct task *to) {
    fpu_save(current);                  /* fnsave сбрасывает FPU — сразу вернём */
    fpu_restore(current);
    memcpy(to->fpu, current->fpu, sizeof(current->fpu));
}

/* ---------------------------------------------------------------- планировщик */

static struct task *pick_next(void) {
    int cur = slot_of(current);
    for (int i = 1; i <= MAX_TASKS; i++) {
        struct task *t = tasks[(cur + i) % MAX_TASKS];
        if (t && !t->cpu_idle && t->cpu == smp_cpu() && t != current && t->state == TASK_READY) return t;
    }
    if (current != idle_task && current->state == TASK_READY) return current;
    return idle_task;
}

struct regs *schedule(struct regs *r) {
    if (!current) return r;

    current->esp = (u32)r;
    /* переполнение стека ядра: esp в «красной зоне» или затёрта канарейка */
    if (current->state != TASK_ZOMBIE && current->stack &&
        ((u32)r < (u32)current->stack + STACK_RED || !canary_ok(current))) {
        kprintf("\n[task %u '%s' killed: kernel stack overflow]\n", current->id, current->name);
        make_zombie(current, 134);
    }
    if (current->state == TASK_RUNNING) current->state = TASK_READY;

    struct task *next = pick_next();
    if (next->state == TASK_READY) next->state = TASK_RUNNING;
    if (next != current) {
        if (current->user && current->state != TASK_ZOMBIE) fpu_save(current);
        if (next->user) fpu_restore(next);
    }
    current = next;
    slice = TIME_SLICE;
    need_resched = false;

    vmm_switch(next->page_dir);                     /* адресное пространство задачи */
    if (next->stack) tss_set_kernel_stack((u32)next->stack + TASK_STACK);
    return (struct regs *)next->esp;
}

struct regs *sched_tick(struct regs *r) {
    if (!current) return r;
    if ((r->cs & 3) && current->kill_pending && !current->in_syscall)
        make_zombie(current, current->pending_code);
    current->cpu_ticks++;
    cpu_states[smp_cpu()].total_ticks++;
    if (current->cpu_idle) cpu_states[smp_cpu()].idle_ticks++;

    u32 now = timer_ticks();
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = tasks[i];
        if (t && t->state == TASK_SLEEPING && (i32)(now - t->wake_tick) >= 0) {
            t->state = TASK_READY;
            smp_wake(t->cpu);
        }
        if (t && t->state == TASK_BLOCKED && t->wait_deadline &&
            (i32)(now - t->wait_deadline) >= 0) {       /* ждал с тайм-аутом — время вышло */
            t->state = TASK_READY;
            smp_wake(t->cpu);
        }
    }
    if (smp_cpu() == 0 && now % TIMER_HZ == 0) {                      /* CPU% за последнюю секунду */
        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *t = tasks[i];
            if (!t) continue;
            t->cpu_recent = t->cpu_ticks - t->cpu_snapshot;
            t->cpu_snapshot = t->cpu_ticks;
        }
    }

    if (current->state == TASK_ZOMBIE) return schedule(r);
    if (current == idle_task && need_resched) return schedule(r);
    if (slice) slice--;
    if (slice == 0) return schedule(r);             /* квант исчерпан — вытесняем */
    return r;
}

struct regs *sched_maybe(struct regs *r) {
    if (!current) return r;
    if (current->state == TASK_ZOMBIE) return schedule(r);   /* например, убит по Ctrl+C */
    if (need_resched && current == idle_task) return schedule(r);
    return r;
}

/* ---------------------------------------------------------------- создание */

static void task_entry(void) {
    current->entry(current->arg);
    task_exit();
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) {
        block_flush_idle();
        task_reap();
        smp_idle();
    }
}

void tasking_init(void) {
    memset(&task0, 0, sizeof(task0));
    task0.id = next_id++;
    memcpy(task0.name, "shell", 6);
    task0.state = TASK_RUNNING;
    task0.page_dir = vmm_kernel_dir();
    tasks[0] = &task0;
    current = &task0;
    slice = TIME_SLICE;

    task_create("idle", idle_loop, NULL);   /* займёт слот 1 */
    idle_task = tasks[1];
    idle_task->cpu_idle = true;
}

void tasking_init_ap(void) {
    u32 cpu = smp_cpu();
    struct task *t = &ap_idle[cpu];
    memset(t, 0, sizeof(*t));
    memcpy(t->name, "idle", 5);
    t->cpu = cpu; t->cpu_idle = true; t->state = TASK_RUNNING;
    t->page_dir = vmm_kernel_dir();
    for (int i=0; i<MAX_TASKS; i++) if (!tasks[i]) {
        t->id = next_id++; tasks[i] = t; break;
    }
    current = idle_task = t;
    slice = TIME_SLICE;
}

static struct task *alloc_task(const char *name) {
    task_reap();
    struct task *t = kcalloc(1, sizeof(*t));
    u8 *stack = kcalloc(1, TASK_STACK);
    if (!t || !stack) { kfree(t); kfree(stack); return NULL; }
    for (int i = 0; i < CANARY_WORDS; i++) ((u32 *)stack)[i] = CANARY;
    u32 n = strlen(name);
    if (n >= TASK_NAME) n = TASK_NAME - 1;
    memcpy(t->name, name, n);
    t->stack = stack;
    t->state = TASK_READY;
    t->created = timer_ticks();
    t->page_dir = vmm_kernel_dir();
    return t;
}

/* кадр «прерванной» задачи на вершине её стека ядра */
static struct regs *initial_frame(struct task *t) {
    struct regs *r = (struct regs *)(t->stack + TASK_STACK - sizeof(struct regs));
    memset(r, 0, sizeof(*r));
    r->eflags = 0x202;                      /* IF=1: прерывания включены */
    t->esp = (u32)r;
    return r;
}

/* поставить задачу в таблицу -> её id; при неудаче задача освобождается и -> 0 */
u32 task_start(struct task *t) {
    u32 f = irq_save();
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) if (!tasks[i]) { slot = i; break; }
    if (slot >= 0) {
        t->id = next_id++;
        t->cpu = t->user ? next_cpu++ % smp_count() : 0;
        tasks[slot] = t;
        smp_wake(t->cpu);
    }
    if (slot >= 0 && !t->pgid) t->pgid = t->id;   /* новая группа (fork задаёт свою) */
    u32 id = slot >= 0 ? t->id : 0;
    irq_restore(f);
    if (!id) free_task(t);
    return id;
}

int task_create(const char *name, void (*entry)(void *), void *arg) {
    struct task *t = alloc_task(name);
    if (!t) return -1;
    t->entry = entry;
    t->arg = arg;
    struct regs *r = initial_frame(t);
    r->fs = r->es = r->ds = KERNEL_DS;
    r->gs = 0x30; /* CPU-local kernel segment */
    r->cs = KERNEL_CS;
    r->eip = (u32)task_entry;
    u32 id = task_start(t);
    return id ? (int)id : -2;
}

/* Процесс: первый же iret из этого кадра уйдёт в кольцо 3 (cs = 0x1B),
 * процессор заодно загрузит пользовательские ss:esp из кадра. */
struct task *task_alloc_user(const char *name, u32 page_dir, u32 entry, u32 user_esp) {
    struct task *t = alloc_task(name);
    if (!t) return NULL;
    t->user = true;
    t->page_dir = page_dir;
    struct regs *r = initial_frame(t);
    r->gs = r->fs = r->es = r->ds = USER_DS;
    r->cs = USER_CS;
    r->ss = USER_DS;
    r->eip = entry;
    r->useresp = user_esp;
    for (int i = 0; i < 3; i++) t->fds[i] = &console_file;   /* stdin, stdout, stderr */
    memcpy(t->fpu, fpu_clean, sizeof(fpu_clean));
    return t;
}

/* ---------------------------------------------------------------- завершение */

int task_thread_create(u32 entry,u32 fn,u32 arg,u32 sp,u32 base,u32 size) {
    struct task *p=task_process(current);
    if (p->state == TASK_ZOMBIE || p->kill_pending) return -EINTR;
    if(task_stack_busy(p->page_dir,base,base+size))return -EBUSY;
    u32 guard=(base+PAGE_SIZE-1)&~(PAGE_SIZE-1),frame,page_flags;
    if(guard+2*PAGE_SIZE>base+size || !vmm_lookup(p->page_dir,guard,&frame,&page_flags))return -EINVAL;
    struct task *t=task_alloc_user(p->name,p->page_dir,entry,sp);
    if (!t) return -ENOMEM;
    /* The process owns these defaults; this thread owns no file references. */
    memset(t->fds,0,sizeof(t->fds));
    t->leader=p; t->parent=p->id; t->pgid=p->pgid; t->keep_zombie=true;
    t->user_stack_base=base;t->user_stack_size=size;t->guard_va=guard;t->guard_flags=page_flags;
    /* Keep the backing frame pinned, but make the full page inaccessible in ring 3. */
    if(vmm_map_in(p->page_dir,guard,frame,page_flags&~PG_USER)){t->guard_va=0;free_task(t);return -ENOMEM;}
    u32 flags=irq_save();
    if (p->state==TASK_ZOMBIE || p->kill_pending || current->kill_pending) {
        free_task(t); irq_restore(flags); return -EINTR;
    }
    u32 *s=(u32 *)sp; s[0]=0; s[1]=fn; s[2]=arg;
    u32 id=task_start(t);
    irq_restore(flags);
    return id ? (int)id : -EAGAIN;
}
int task_thread_join(u32 tid, int *code) {
    u32 f=irq_save();
    for (;;) {
        struct task *t=task_by_id(tid);
        if (!t || !t->leader || t->leader != task_process(current)) { irq_restore(f); return -ESRCH; }
        if (t->detached) { irq_restore(f); return -EINVAL; }
        if (t==current || (t->joiner && t->joiner != current->id)) { irq_restore(f); return -EBUSY; }
        t->joiner=current->id;
        /* The outgoing CPU must have stopped using the thread's user stack. */
        if (t->state==TASK_ZOMBIE && !smp_task_active(t)) {
            if (code) *code=t->exit_code;
            t->keep_zombie=false;
            tasks[slot_of(t)]=NULL;
            free_task(t);
            irq_restore(f); return 0;
        }
        if (current->kill_pending) { t->joiner=0; irq_restore(f); return -EINTR; }
        task_wait_timeout(t,1);
    }
}
int task_thread_detach(u32 tid) {
    u32 f = irq_save();
    struct task *thr = task_by_id(tid);
    if (!thr || !thr->leader || thr->leader != task_process(current)) { irq_restore(f); return -ESRCH; }
    if (thr->detached || thr->joiner) { irq_restore(f); return -EINVAL; }
    thr->detached = true;
    if (thr->state == TASK_ZOMBIE) {
        release_user_stack(thr);
        thr->keep_zombie = false;
    }
    irq_restore(f);
    return 0;
}

int task_thread_set_stack(u32 tid, u32 base, u32 len) {
    u32 f = irq_save();
    struct task *thr = task_by_id(tid);
    if (!thr || !thr->leader || thr->leader != task_process(current)) { irq_restore(f); return -ESRCH; }
    if (!base || (base & (PAGE_SIZE - 1)) || base < MMAP_BASE || !len ||
        base + len < base || base + len > MMAP_END) { irq_restore(f); return -EINVAL; }
    thr->user_stack_base = base;
    thr->user_stack_size = len;
    irq_restore(f);
    return 0;
}

void task_set_pgid(struct task *p, u32 pgid) {
    for (int i=0; i<MAX_TASKS; i++) if (tasks[i] && task_process(tasks[i])==p) tasks[i]->pgid=pgid;
}
void task_exit_group(int code) {
    __asm__ volatile("cli" ::: "memory");
    struct task *p=task_process(current);
    for (int i=0; i<MAX_TASKS; i++) if (tasks[i] && task_process(tasks[i])==p && tasks[i]!=current) {
        if (tasks[i]->leader) tasks[i]->keep_zombie=false;
        task_kill(tasks[i]->id);
    }
    for (int i=0; i<MAX_TASKS; i++) if (tasks[i] && task_process(tasks[i])==p) tasks[i]->pending_code=code;
    task_exit_code(code);
}
void task_yield(void) {
    __asm__ volatile ("int $0x81");
}

void task_exit_code(int code) {
    __asm__ volatile ("cli");
    make_zombie(current, code);
    task_yield();
    for (;;) ;                               /* сюда управление не вернётся */
}

void task_exit(void) { task_exit_code(0); }

int task_kill(u32 id) {
    u32 f = irq_save();
    struct task *t = task_by_id(id);
    if (!t || t->state == TASK_ZOMBIE) { irq_restore(f); return -1; }
    if (t == &task0 || t->cpu_idle) { irq_restore(f); return -2; }
    if (t->user && !t->leader) {
        for (int i=0; i<MAX_TASKS; i++) if (tasks[i] && tasks[i]->leader==t) {
            tasks[i]->keep_zombie=false;
            task_kill(tasks[i]->id);
        }
    }
    if (t->user) {
        /* Always clean up a user process on its own CPU. Even a READY process
         * could start on another CPU if closing its files blocks the killer.
         * Deliver at the next ring-3 timer interrupt or syscall return. */
        if (!t->kill_pending) t->pending_code=137;
        t->kill_pending = true;
        if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED) t->state = TASK_READY;
        smp_wake(t->cpu);
    } else {
        make_zombie(t, 137);                 /* как SIGKILL в Unix: 128 + 9 */
    }
    need_resched = true;
    irq_restore(f);
    if (t == current && !irq_nesting && !t->user) task_yield();
    return 0;
}

int task_kill_group(u32 pgid) {
    if (!pgid) return 0;
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = tasks[i];
        if (t && t->pgid == pgid && t->state != TASK_ZOMBIE && task_kill(t->id) == 0) n++;
    }
    return n;
}

/* waitpid() для процессов: ждать своего ребёнка pid (или любого, если pid = -1).
 * -> id завершившегося, 0 (nohang и все ещё живы), -1 (детей нет), -2 (прервано) */
int task_wait_child(int pid, int *code, bool nohang) {
    u32 f = irq_save();
    for (;;) {
        bool have = false;
        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *c = tasks[i];
            if (!c || c->leader || c->parent != task_process(current)->id || (pid > 0 && c->id != (u32)pid)) continue;
            have = true;
            if (c->state == TASK_ZOMBIE) {
                u32 id = c->id;
                if (code) *code = c->exit_code;
                c->keep_zombie = false;
                c->parent = 0;
                irq_restore(f);
                return id;
            }
        }
        if (!have) { irq_restore(f); return -1; }
        if (nohang) { irq_restore(f); return 0; }
        task_wait(task_process(current));                  /* make_zombie ребёнка разбудит нас */
        if (current->kill_pending) { irq_restore(f); return -2; }
    }
}

/* дождаться завершения задачи и забрать код выхода */
int task_waitpid(u32 id, int *code) {
    u32 f = irq_save();
    for (;;) {
        struct task *t = task_by_id(id);
        if (!t) { irq_restore(f); return -1; }
        if (t->state == TASK_ZOMBIE) {
            if (code) *code = t->exit_code;
            t->keep_zombie = false;          /* теперь reap() может её убрать */
            irq_restore(f);
            return 0;
        }
        task_wait(t);
    }
}

/* ---------------------------------------------------------------- сон и ожидание */

void task_sleep_ticks(u32 ticks) {
    if (!current) {
        u32 end = timer_ticks() + ticks;
        while ((i32)(timer_ticks() - end) < 0) __asm__ volatile ("hlt");
        return;
    }
    if (!ticks) { task_yield(); return; }
    u32 f = irq_save();
    if (current->user && current->kill_pending) { irq_restore(f); return; }
    current->wake_tick = timer_ticks() + ticks;
    current->state = TASK_SLEEPING;
    task_yield();
    irq_restore(f);
}

/* вызывается с запрещёнными прерываниями; после пробуждения они тоже запрещены */
void task_wait(void *chan) {
    current->wait_chan = chan;
    current->wait_deadline = 0;
    current->state = TASK_BLOCKED;
    task_yield();
    current->wait_chan = NULL;
}

/* то же, но не дольше ticks тиков; вызывать с cli */
bool task_wait_timeout(void *chan, u32 ticks) {
    u32 deadline = timer_ticks() + (ticks ? ticks : 1);
    current->wait_chan = chan;
    current->wait_deadline = deadline;
    current->state = TASK_BLOCKED;
    task_yield();
    current->wait_chan = NULL;
    current->wait_deadline = 0;
    return (i32)(timer_ticks() - deadline) < 0;
}

void task_wakeup(void *chan) {
    u32 f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = tasks[i];
        if (t && t->state == TASK_BLOCKED && t->wait_chan == chan) {
            t->state = TASK_READY;
            smp_wake(t->cpu);
        }
    }
    irq_restore(f);
}

void task_wake_one(void *chan) {
    u32 f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = tasks[i];
        if (t && t->state == TASK_BLOCKED && t->wait_chan == chan) {
            t->state = TASK_READY;
            smp_wake(t->cpu);
            break;
        }
    }
    irq_restore(f);
}

/* ---------------------------------------------------------------- мьютекс */

void mutex_lock(struct mutex *m) {
    if (!current) return;
    u32 f = irq_save();
    while (m->owner && m->owner != current) task_wait(m);
    m->owner = current;
    m->depth++;
    current->holding = m;
    irq_restore(f);
}

bool mutex_trylock(struct mutex *m) {
    if (!current) return true;
    u32 f = irq_save();
    if (m->owner && m->owner != current) { irq_restore(f); return false; }
    m->owner = current;
    m->depth++;
    current->holding = m;
    irq_restore(f);
    return true;
}

void mutex_unlock(struct mutex *m) {
    if (!current) return;
    u32 f = irq_save();
    if (m->owner == current && --m->depth == 0) {
        m->owner = NULL;
        current->holding = NULL;
        task_wakeup(m);
    }
    irq_restore(f);
}

/* ---------------------------------------------------------------- падения */

/* Исключение процессора. Если упал процесс или поток — убиваем только его. */
bool task_fault(struct regs *r, const char *what) {
    if (!current || current == &task0 || current == idle_task) return false;
    vga_set_color(LIGHT_RED, BLACK);
    if (current->user) {
        kprintf("\n%s: Segmentation fault (%s at EIP=%p", current->name, what, r->eip);
        if (r->int_no == 14) {
            u32 cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf(", address %p", cr2);
        }
        kprintf(")\n");
    } else {
        kprintf("\n[task %u '%s' killed: %s at EIP=%p]\n", current->id, current->name, what, r->eip);
    }
    vga_set_color(LIGHT_GREY, BLACK);
    if (current->leader) {
        task_kill(current->leader->id);
        current->leader->pending_code=139;
    }
    make_zombie(current, 139);               /* как SIGSEGV: 128 + 11 */
    return true;
}

/* ---------------------------------------------------------------- ps */

int task_list(struct task_info *out, int max) {
    task_reap();
    int n = 0;
    u32 now = timer_ticks();
    u32 f = irq_save();
    for (int i = 0; i < MAX_TASKS && n < max; i++) {
        struct task *t = tasks[i];
        if (!t) continue;
        struct task_info *o = &out[n++];
        o->id = t->id;
        memcpy(o->name, t->name, TASK_NAME);
        o->state = t->state;
        o->user = t->user;
        o->parent = t->parent;
        o->cpu_ticks = t->cpu_ticks;
        o->cpu_percent = t->cpu_recent * 100 / TIMER_HZ;
        o->age_ticks = now - t->created;
        o->mem_kb = (t->stack ? TASK_STACK / 1024 : 0) + t->user_pages * 4;
    }
    irq_restore(f);
    return n;
}
