/* Мини-libc для программ MyOS. Всё общение с миром — через int 0x80. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "time.h"
#include "sys/time.h"
#include "sys/stat.h"
#include "dirent.h"
#include "libgen.h"
#include "wchar.h"
#include "util.h"
#include "math.h"
#include "locale.h"
#include "signal.h"
#include "fcntl.h"
#include "poll.h"
#include "pwd.h"
#include "sys/socket.h"
#include "netinet/in.h"
#include "netinet/tcp.h"
#include "arpa/inet.h"
#include "netdb.h"
#include "sys/select.h"
#include "sys/ioctl.h"
#include "utime.h"
#include "syslog.h"
#include "sys/resource.h"
#include "sys/statvfs.h"
#include "termios.h"
#include "grp.h"
#include "fnmatch.h"
#include "glob.h"
#include "spawn.h"
#include "sys/utsname.h"

/* Filesystem administration wrappers are defined below syscall3. */

/* ================================================================ точка входа */

int main(int argc, char **argv);

int __libc_start(int argc, char **argv, char **envp) __attribute__((noreturn));

#if !defined(__MYOS_CC__) && !defined(__LIBC_SO__)
/* Ядро «вызывает» _start(argc, argv): кладёт их на стек как аргументы функции.
 * (Программам, собранным cc внутри MyOS, такую же точку входа делает сам cc.
 *  В libc.so точки входа нет: её даёт crt0 самой программы.) */
__attribute__((noreturn, used, section(".text.start")))
void _start(int argc, char **argv, char **envp) {
    __libc_start(argc, argv, envp);
}
#endif

/* ================================================================ системные вызовы */

#if defined(__LINUX_TEST__)
/* i386 stat64 avoids EOVERFLOW for DrvFS inode/device numbers. */
static int linux_metadata(int op,int arg,struct stat *st) {
    unsigned char raw[96];int ret=__syscall(op==1?195:op==2?196:197,arg,(int)raw,0);
    if(ret<0)return ret;memset(st,0,sizeof(*st));
    st->st_mode=*(unsigned*)(raw+16);st->st_size=*(unsigned*)(raw+44);
    st->st_uid=*(unsigned*)(raw+24);st->st_gid=*(unsigned*)(raw+28);
    st->st_ino=*(unsigned*)(raw+88);st->st_dev=*(unsigned*)raw;
    st->st_blocks=*(unsigned*)(raw+56);st->st_nlink=*(unsigned*)(raw+20);
    st->st_atime=*(unsigned*)(raw+64);st->st_mtime=*(unsigned*)(raw+72);st->st_ctime=*(unsigned*)(raw+80);
    st->st_atim.tv_sec=st->st_atime;st->st_mtim.tv_sec=st->st_mtime;st->st_ctim.tv_sec=st->st_ctime;
    return 0;
}
/* Только для проверки cc на Linux: те же вызовы, но в номерах Linux */
static int linux_brk;

/* Поиндексное чтение каталога через getdents64. Точки пропускаются, как в ядре. */
static int linux_dir_at(const char *path, int index, struct dirent_info *o) {
    unsigned char buf[1024];
    int fd = __syscall(5, (int)path, 0, 0);
    int seen = 0;
    if (fd < 0) return fd;
    for (;;) {
        int n = __syscall(220, fd, (int)buf, (int)sizeof(buf));
        int off = 0;
        if (n < 0) { __syscall(6, fd); return n; }
        if (n == 0) { __syscall(6, fd); return 0; }
        while (off + 19 < n) {
            unsigned short reclen;
            unsigned char typ;
            char *name;
            int i;
            memcpy(&reclen, buf + off + 16, 2);
            if (reclen < 19 || off + reclen > n) { __syscall(6, fd); return -EIO; }
            typ = buf[off + 18];
            name = (char *)(buf + off + 19);
            off += reclen;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            if (seen++ != index) continue;
            memset(o, 0, sizeof *o);
            for (i = 0; i < 255 && name[i]; i++) o->name[i] = name[i];
            if (typ == 4) o->type = 2;
            else if (typ == 2) o->type = 4;
            else if (typ == 6) o->type = 3;
            else if (typ == 10) o->type = 5;
            else o->type = 1;
            {
                char full[512];
                struct stat st;
                unsigned mode;
                if (snprintf(full, sizeof full, "%s/%s", path, o->name) < (int)sizeof full &&
                    linux_metadata(1,(int)full,&st)==0) {
                    mode=st.st_mode;
                    o->size=st.st_size;
                    if ((mode & 0170000) == 0040000) o->type = 2;
                    else if ((mode & 0170000) == 0020000) o->type = 4;
                    else if ((mode & 0170000) == 0060000) o->type = 3;
                    else if ((mode & 0170000) == 0120000) o->type = 5;
                    else o->type = 1;
                }
            }
            __syscall(6, fd);
            return 1;
        }
    }
}

static int syscall3(int n, int a, int b, int c) {
    switch (n) {
    case SYS_EXIT:  return __syscall(1, a);
    case SYS_WRITE: return __syscall(4, a, b, c);
    case SYS_READ:  return __syscall(3, a, b, c);
    case SYS_OPEN:  return __syscall(5, a, b, c);
    case SYS_CLOSE: return __syscall(6, a);
    case SYS_LSEEK: return __syscall(19, a, b, c);
    case SYS_FSTAT: {
        struct stat st;int ret=linux_metadata(3,a,&st);if(ret<0)return ret;
        ((unsigned*)b)[0]=st.st_mode;((unsigned*)b)[1]=st.st_size;return 0;
    }
    case SYS_UNLINK: {
        int r = __syscall(10, a);
        if (r == -EISDIR) r = __syscall(40, a);   /* пустой каталог, как unlink в MyOS */
        return r;
    }
    case SYS_MKDIR: return __syscall(39, a, 0755);
    case SYS_CHDIR: return __syscall(12, a);
    case SYS_GETCWD: return __syscall(183, a, b);
    case SYS_FORK:  return __syscall(2);
    case SYS_EXEC: return __syscall(11, a, b, c ? c : (int)environ);
    case SYS_PIPE: return __syscall(42, a);
    case SYS_DUP2: return __syscall(63, a, b);
    case SYS_WAITPID: {
        int st = 0, r = __syscall(7, a, (int)&st, c);
        if (r < 0) return r;
        if (b) *(int *)b = st;
        return r;
    }
    case SYS_RENAME: return __syscall(38, a, b);
    case SYS_SBRK: {
        if (!linux_brk) linux_brk = __syscall(45, 0);
        int old = linux_brk;
        if (__syscall(45, old + a) != old + a) return -1;
        linux_brk = old + a;
        return old;
    }
    case SYS_TERMSIZE: return (80 << 16) | 25;
    case SYS_STAT:
    case SYS_LSTAT: {
        struct stat st;int ret=linux_metadata(n==SYS_STAT?1:2,a,&st);if(ret<0)return ret;
        struct dirent_info *out=(void*)b;memset(out,0,sizeof(*out));out->size=st.st_size;
        out->type=S_ISDIR(st.st_mode)?2:S_ISCHR(st.st_mode)?4:S_ISBLK(st.st_mode)?3:S_ISLNK(st.st_mode)?5:1;
        return 0;
    }
    case SYS_SYMLINK: return __syscall(83, a, b);
    case SYS_READLINK: return __syscall(85, a, b, c);
    case SYS_READDIR: return linux_dir_at((const char *)a, b, (struct dirent_info *)c);
    case SYS_REBOOT: return -38;                /* тесты на Linux не выключают машину */
    case SYS_MMAP: return __syscall(90, a);     /* старый mmap: в ebx указатель на шесть полей */
    case SYS_MUNMAP: return __syscall(91, a, b);
    case SYS_MPROTECT: return __syscall(125, a, b, c);
    default: return -38;
    }
}
static int syscall5(int n, int a, int b, int c, int d, int e) { (void)d; (void)e; return syscall3(n, a, b, c); }
#elif defined(__MYOS_CC__)
static int syscall3(int n, int a, int b, int c) { return __syscall(n, a, b, c); }
static int syscall5(int n, int a, int b, int c, int d, int e) { return __syscall(n, a, b, c, d, e); }
#else
static inline int syscall3(int n, int a, int b, int c) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

static inline int syscall5(int n, int a, int b, int c, int d, int e) {
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
    return ret;
}
#endif

static void (*exit_handlers[64])(void);
static int exit_handler_n;

int atexit(void (*fn)(void)) {
    if (!fn || exit_handler_n >= 64) return -1;
    exit_handlers[exit_handler_n++] = fn;
    return 0;
}

void exit(int code) {
    while (exit_handler_n > 0) exit_handlers[--exit_handler_n]();
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;) ;
}
void _exit(int code) {
#if defined(__LINUX_TEST__)
    __syscall(1, code);
#else
    syscall3(SYS_EXIT, code, 0, 0);
#endif
    for (;;) ;
}

#undef errno
int errno;
static struct thread_tcb main_tcb;

int thread_ctl(int op, int a, int b, int c) {
    return syscall5(SYS_THREAD_CTL, op, a, b, c, 0);
}
int thread_set_tcb(struct thread_tcb *tcb) {
    return thread_ctl(TC_SET_TCB, (int)tcb, 0, 0);
}
struct thread_tcb *thread_get_tcb(void) {
    unsigned r = (unsigned)thread_ctl(TC_GET_TCB, 0, 0, 0);
    if (r < 0x40000000u || r >= 0xfffff001u) return 0;
    return (struct thread_tcb *)r;
}
int *__errno_location(void) {
    struct thread_tcb *tcb = thread_get_tcb();
    if (!tcb) return &errno;
    return &tcb->errno;
}
#define errno (*__errno_location())

/* POSIX: -1 и errno. Остальные вызовы по-прежнему возвращают -код. */
static int ioerr(int r) {
    if (r < 0) { errno = -r; return -1; }
    return r;
}

/* Public metadata/permissions ABI, with a real Linux adapter for cc -linux tests. */
static int fs_call(int op,int arg,int ptr) {
#if defined(__LINUX_TEST__)
    if(op==1 || op==2 || op==3) {
        return linux_metadata(op,arg,(struct stat*)ptr);
    }
    if(op==4 || op==5)return __syscall(op==4?15:94,arg,ptr,0);
    if(op==6 || op==7){unsigned *ids=(void*)ptr;return __syscall(op==6?212:207,arg,ids[0],ids[1]);}
    if(op==8)return __syscall(33,arg,ptr,0);
    if(op==9)return __syscall(60,arg,0,0);
    if(op==10){unsigned *a=(void*)ptr;return __syscall(5,a[0],a[1],a[2]);}
    if(op==11)return __syscall(39,arg,ptr,0);
    return -EINVAL;
#else
    return syscall3(SYS_FSCTL,op,arg,ptr);
#endif
}

int   write(int fd, const void *buf, size_t len) { return ioerr(syscall3(SYS_WRITE, fd, (int)buf, len)); }
int   read(int fd, void *buf, size_t len)        { return ioerr(syscall3(SYS_READ, fd, (int)buf, len)); }
int open(const char *path,int flags,...) {
    unsigned mode=0666;
    if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=va_arg(ap,unsigned);va_end(ap);}
    unsigned a[3]={(unsigned)path,flags,mode};return ioerr(fs_call(10,0,(int)a));
}
int myos_open_default(const char *path,int flags){return open(path,flags,0666);}
static void adv_clear_fd(int fd);
int   close(int fd) {
    int r = ioerr(syscall3(SYS_CLOSE, fd, 0, 0));
    if (r == 0) adv_clear_fd(fd);
    return r;
}
int   lseek(int fd, long offset, int whence)     { return ioerr(syscall3(SYS_LSEEK, fd, (int)offset, whence)); }
ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    if (offset < 0) { errno = EINVAL; return -1; }
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(180, fd, (int)buf, (int)count, (int)offset, 0));
#else
    return ioerr(syscall5(SYS_PREAD, fd, (int)buf, (int)count, (int)offset, 0));
#endif
}
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if (offset < 0) { errno = EINVAL; return -1; }
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(181, fd, (int)buf, (int)count, (int)offset, 0));
#else
    return ioerr(syscall5(SYS_PWRITE, fd, (int)buf, (int)count, (int)offset, 0));
#endif
}
int fsync(int fd) {
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(118, fd));
#else
    return ioerr(syscall3(SYS_FSYNC, fd, 0, 0));
#endif
}
int fdatasync(int fd) { return fsync(fd); }
int ftruncate(int fd, off_t length) {
    struct stat st;
    if (length < 0) { errno = EINVAL; return -1; }
    if (fstat(fd, &st) < 0) return -1;
    if ((off_t)st.st_size == length) return 0;
    if ((off_t)st.st_size < length) {
        char z = 0;
        if (pwrite(fd, &z, 1, length - 1) != 1) return -1;
        return 0;
    }
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(93, fd, (int)length));
#else
    return ioerr(fs_call(14, fd, (int)length));
#endif
}
int getpagesize(void) { return 4096; }
char *getpass(const char *prompt) {
    static char buf[128];
    int i = 0, old = 0, have = 0;
    if (prompt) write(2, prompt, strlen(prompt));
    if (isatty(0)) { old = ttymode(TTY_RAW | TTY_NOSIG); have = 1; }
    while (i + 1 < (int)sizeof buf) {
        char c;
        if (read(0, &c, 1) != 1) break;
        if (c == '\r' || c == '\n' || c == 4) break;
        if (c == 8 || c == 127) { if (i) i--; continue; }
        buf[i++] = c;
    }
    buf[i] = 0;
    if (have) ttymode(old);
    write(2, "\n", 1);
    return buf;
}
void *sbrk(int inc)                              { return (void *)syscall3(SYS_SBRK, inc, 0, 0); }

struct mmap_args { unsigned addr, len, prot, flags, fd, off; };

void *mmap(void *addr, unsigned len, int prot, int flags, int fd, unsigned off) {
    struct mmap_args a;
    a.addr = (unsigned)addr; a.len = len; a.prot = prot; a.flags = flags; a.fd = fd; a.off = off;
    unsigned r = syscall3(SYS_MMAP, (int)&a, 0, 0);
    if (r >= 0xfffff001u) { errno = -(int)r; return (void *)-1; }
    return (void *)r;
}
int munmap(void *addr, unsigned len)             { return syscall3(SYS_MUNMAP, (int)addr, len, 0); }
int mprotect(void *addr, unsigned len, int prot) { return syscall3(SYS_MPROTECT, (int)addr, len, prot); }
void  sleep_ms(unsigned ms)                      { syscall3(SYS_SLEEP, ms, 0, 0); }
int   getpid(void)                               { return syscall3(SYS_GETPID, 0, 0, 0); }
unsigned ticks(void)                             { return syscall3(SYS_TICKS, 0, 0, 0); }
void  yield(void)                                { syscall3(SYS_YIELD, 0, 0, 0); }
int   spawn(const char *path, char **argv, int stdin_fd) { return syscall3(SYS_SPAWN, (int)path, (int)argv, stdin_fd); }
static void thread_entry(void (*fn)(void *), void *arg) {
    struct thread_tcb tcb;
    memset(&tcb, 0, sizeof tcb);
    thread_set_tcb(&tcb);
    fn(arg);
    thread_exit(0);
}
int thread_create(void (*fn)(void *), void *arg, void *stack, unsigned size) {
    return syscall5(SYS_THREAD_CREATE,(int)thread_entry,(int)fn,(int)arg,(int)stack,size);
}
int thread_join(int tid,int *code) { return syscall3(SYS_THREAD_JOIN,tid,(int)code,0); }
void thread_exit(int code) { syscall3(SYS_THREAD_EXIT,code,0,0); for (;;) {} }
int gettid(void) { return syscall3(SYS_GETTID,0,0,0); }
int thread_mutex_lock(thread_mutex_t *m) { return syscall3(SYS_THREAD_MUTEX,(int)m,0,0); }
int thread_mutex_unlock(thread_mutex_t *m) { return syscall3(SYS_THREAD_MUTEX,(int)m,1,0); }
int thread_mutex_trylock(thread_mutex_t *m) { return syscall3(SYS_THREAD_MUTEX,(int)m,2,0); }
int   fork(void)                                 { return ioerr(syscall3(SYS_FORK, 0, 0, 0)); }
int   execve(const char *path, char *const argv[], char *const envp[]) {
    return syscall3(SYS_EXEC, (int)path, (int)argv, (int)(envp ? envp : environ));
}
int   exec(const char *path, char **argv)        { return execve(path, (char *const *)argv, environ); }
int   execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }
int execvp(const char *file, char *const argv[]) {
    const char *path, *colon;
    int last = ENOENT;
    if (!file || !argv) { errno = EINVAL; return -1; }
    if (strchr(file, '/')) {
        int r = execve(file, argv, environ);
        if (r < 0) { errno = -r; return -1; }
        return r;
    }
    path = getenv("PATH");
    if (!path || !path[0]) path = "/bin:/ext/bin";
    while (*path) {
        char buf[1024];
        int n, r;
        colon = strchr(path, ':');
        n = colon ? (int)(colon - path) : (int)strlen(path);
        if (n + 2 + (int)strlen(file) < (int)sizeof buf) {
            if (n) memcpy(buf, path, (size_t)n);
            buf[n] = n ? '/' : 0;
            if (!n) { memcpy(buf, file, strlen(file) + 1); }
            else memcpy(buf + n + 1, file, strlen(file) + 1);
            r = execve(buf, argv, environ);
            if (r >= 0) return r;
            last = -r;
            if (last != ENOENT && last != EACCES && last != ENOTDIR) { errno = last; return -1; }
        }
        if (!colon) break;
        path = colon + 1;
    }
    errno = last;
    return -1;
}
static int exec_collect(char **argv, int cap, const char *arg0, va_list ap) {
    int n = 0;
    argv[n++] = (char *)arg0;
    for (;;) {
        char *a = va_arg(ap, char *);
        if (n < cap - 1) argv[n++] = a;
        if (!a) break;
    }
    argv[n < cap ? n : cap - 1] = NULL;
    return n;
}
int execl(const char *path, const char *arg0, ...) {
    char *argv[64];
    va_list ap;
    int r;
    va_start(ap, arg0);
    exec_collect(argv, 64, arg0, ap);
    va_end(ap);
    r = execve(path, argv, environ);
    if (r < 0) { errno = -r; return -1; }
    return r;
}
int execlp(const char *file, const char *arg0, ...) {
    char *argv[64];
    va_list ap;
    va_start(ap, arg0);
    exec_collect(argv, 64, arg0, ap);
    va_end(ap);
    return execvp(file, argv);
}
int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
    int child;
    (void)file_actions;
    (void)attrp;
    if (!pid || !file || !argv) { errno = EINVAL; return EINVAL; }
    child = fork();
    if (child < 0) return errno;
    if (child == 0) {
        if (envp) environ = (char **)envp;
        execvp(file, argv);
        _exit(127);
    }
    *pid = child;
    return 0;
}
int   waitpid(int pid, int *status, int options) { return syscall3(SYS_WAITPID, pid, (int)status, options); }
int   getppid(void)                              { return syscall3(SYS_GETPPID, 0, 0, 0); }
int   kill(int pid, ...)                         { return syscall3(SYS_KILL, pid, 0, 0); }
int   setpgid(int pid, int pgid)                 { return syscall3(SYS_SETPGID, pid, pgid, 0); }
pid_t getpgid(pid_t pid) {
#if defined(__LINUX_TEST__)
    int r = __syscall(132, pid);
#else
    int r = syscall3(SYS_GETPGID, pid, 0, 0);
#endif
    return r < 0 ? ioerr(r) : r;
}
pid_t getpgrp(void) { return getpgid(0); }
pid_t setsid(void) {
#if defined(__LINUX_TEST__)
    int r = __syscall(66);
    return r < 0 ? ioerr(r) : r;
#else
    if (setpgid(0, 0) < 0) return -1;
    return getpid();
#endif
}
int   setfg(int pgid)                            { return syscall3(SYS_SETFG, pgid, 0, 0); }
int   pipe(int fds[2])                           { return syscall3(SYS_PIPE, (int)fds, 0, 0); }
int   dup2(int oldfd, int newfd)                 { return syscall3(SYS_DUP2, oldfd, newfd, 0); }
int   dup(int fd)                                { return fcntl(fd, F_DUPFD, 0); }
int   chdir(const char *path)                    { return syscall3(SYS_CHDIR, (int)path, 0, 0); }
/* POSIX: успех — указатель на путь, не 0. getcwd(NULL, 0) выделяет строку:
 * bash так узнаёт текущий каталог и считает нулевой результат ошибкой. */
char *getcwd(char *buf, size_t len) {
    char tmp[1024];
    int heap = 0;
    if (!buf) {
        if (len == 0 || len > sizeof tmp) len = sizeof tmp;
        buf = tmp;
        heap = 1;
    } else if (len == 0) {
        errno = EINVAL;
        return NULL;
    }
    int r = syscall3(SYS_GETCWD, (int)buf, len, 0);
    if (r < 0) { errno = -r; return NULL; }
    if (!heap) return buf;
    buf = strdup(tmp);
    if (!buf) errno = ENOMEM;
    return buf;
}
int   dirread(const char *p, int i, struct dirent_info *o) { return syscall3(SYS_READDIR, (int)p, i, (int)o); }
int mkdir(const char *path,...) {
    va_list ap;va_start(ap,path);unsigned mode=va_arg(ap,unsigned);va_end(ap);
    return ioerr(fs_call(11,(int)path,mode));
}
int myos_mkdir_default(const char *path){return fs_call(11,(int)path,0777);}
int   unlink(const char *path)                   { return syscall3(SYS_UNLINK, (int)path, 0, 0); }
int   ps(struct proc_info *buf, int max)         { return syscall3(SYS_PS, (int)buf, max, 0); }
int   dirstat(const char *p, struct dirent_info *o) { return syscall3(SYS_STAT, (int)p, (int)o, 0); }

/* размер не запоминаем: окно терминала могут растянуть в любой момент */
int term_cols(void) { return syscall3(SYS_TERMSIZE, 0, 0, 0) >> 16; }
int term_rows(void) { return syscall3(SYS_TERMSIZE, 0, 0, 0) & 0xFFFF; }

int pty_open(int fds[2]) { return syscall3(SYS_PTY_OPEN, (int)fds, 0, 0); }
int ttyctl(int fd, int op, int arg) { return syscall3(SYS_TTYCTL, fd, op, arg); }
int kblayout(int l) { return syscall3(SYS_KBLAYOUT, l, 0, 0); }
int sockctl(int fd, int op, unsigned arg) { return syscall3(SYS_SOCKCTL, fd, op, (int)arg); }

int utf8_put(char *o, int cp) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 0x3F); return 2; }
    o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3;
}

static int cur_tty;
int ttymode(int mode) { cur_tty = mode; return syscall3(SYS_TTYMODE, mode, 0, 0); }

void gotoxy(int x, int y) { printf("\x1b[%d;%dH", y + 1, x + 1); }

/* Разобрать escape-последовательность: ESC [ A ... ESC [ 5 ~ */
int clipboard_set(const char *s, unsigned n) { return syscall3(SYS_CLIPBOARD, 1, (int)s, n); }
int clipboard_get(char *s, unsigned n) { return syscall3(SYS_CLIPBOARD, 0, (int)s, n); }

static int key_termsize;
int readkey(void) {
    int size = syscall3(SYS_TERMSIZE, 0, 0, 0);
    if (key_termsize && size != key_termsize) { key_termsize = size; return KEY_RESIZE; }
    key_termsize = size;
    unsigned char c;
    /* Only readkey opts into resize interruption. Raw read callers (notably
     * login/password input) must never see resize as EOF or an input error. */
    syscall3(SYS_TTYMODE, cur_tty | TTY_RESIZE_EVENT, 0, 0);
    int count = read(STDIN, &c, 1);
    syscall3(SYS_TTYMODE, cur_tty, 0, 0);
    if (count <= 0) {
        size = syscall3(SYS_TERMSIZE, 0, 0, 0);
        if (size != key_termsize) { key_termsize = size; return KEY_RESIZE; }
        return -1;
    }
    if (c >= 0xC0) {                                /* UTF-8: остальные байты буквы уже в буфере */
        int n = c >= 0xE0 ? 2 : 1, cp = c & (c >= 0xE0 ? 0x0F : 0x1F);
        for (int i = 0; i < n; i++) {
            unsigned char d;
            if (read(STDIN, &d, 1) <= 0) break;
            cp = cp << 6 | (d & 0x3F);
        }
        return cp;
    }
    if (c != 27) return c;
    /* одиночный Esc от начала последовательности отличаем так: остальные байты
     * последовательности ядро кладёт сразу, а после голого Esc — ничего нет */
    syscall3(SYS_TTYMODE, cur_tty | TTY_NONBLOCK, 0, 0);
    unsigned char a = 0, b = 0, d = 0;
    int n = read(STDIN, &a, 1);
    if (n > 0 && a == '[') read(STDIN, &b, 1);
    if (b >= '0' && b <= '9') read(STDIN, &d, 1);
    syscall3(SYS_TTYMODE, cur_tty, 0, 0);
    if (n <= 0 || a != '[') return 27;
    if (b >= 'a' && b <= 'l') {
        static const int shifted[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
            KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_INS };
        if (b == 'k') return KEY_COPY;
        if (b == 'l') return KEY_PASTE;
        return shifted[b - 'a'] | KEY_SHIFT;
    }
    switch (b) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '2': return KEY_INS;
    case '3': return KEY_DEL;
    case '5': return KEY_PGUP;
    case '6': return KEY_PGDN;
    }
    return 27;
}

/* ================================================================ строки */

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? NULL : (char *)s; }
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    if (p == q || n == 0) return d;
    if (p < q || p >= q + n) {                      /* не перекрываются «назад» — можно вперёд */
        while (n--) *p++ = *q++;
    } else {
        p += n; q += n;
        while (n--) *--p = *--q;
    }
    return d;
}

/* memcpy/memset — по 4 байта за раз (окна и графика копируют много) */
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n && ((unsigned)p & 3)) { *p++ = (uint8_t)c; n--; }
    uint32_t v = (uint8_t)c * 0x01010101u;
#ifndef __MYOS_CC__
    size_t words = n / 4;
    __asm__ volatile ("rep stosl" : "+D"(p), "+c"(words) : "a"(v) : "memory");
#else
    uint32_t *w = (uint32_t *)p;
    for (size_t i = n / 4; i; i--) *w++ = v;
    p = (uint8_t *)w;
#endif
    for (n &= 3; n; n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    if ((((unsigned)p | (unsigned)q) & 3) == 0) {
#ifndef __MYOS_CC__
        size_t words = n / 4;
        __asm__ volatile ("rep movsl" : "+D"(p), "+S"(q), "+c"(words) : : "memory");
#else
        uint32_t *w = (uint32_t *)p;
        const uint32_t *r = (const uint32_t *)q;
        for (size_t i = n / 4; i; i--) *w++ = *r++;
        p = (uint8_t *)w; q = (const uint8_t *)r;
#endif
        n &= 3;
    }
    while (n--) *p++ = *q++;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
char *strstr(const char *h, const char *n) {
    size_t ln = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, ln)) return (char *)h;
    return ln ? NULL : (char *)h;
}
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return (char *)(c ? r : s);
}
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}
char *strncat(char *d, const char *s, size_t n) {
    size_t i = strlen(d), k = 0;
    while (k < n && s[k]) d[i++] = s[k++];
    d[i] = 0;
    return d;
}
static int in_set(const char *set, int c) {
    while (*set) if (*set++ == c) return 1;
    return 0;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && in_set(accept, (unsigned char)s[n])) n++;
    return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !in_set(reject, (unsigned char)s[n])) n++;
    return n;
}
char *strpbrk(const char *s, const char *accept) {
    while (*s) { if (in_set(accept, (unsigned char)*s)) return (char *)s; s++; }
    return NULL;
}
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
char *strtok(char *s, const char *delim) {
    static char *save;
    char *start;
    if (s) save = s;
    if (!save) return NULL;
    while (*save && in_set(delim, (unsigned char)*save)) save++;
    if (!*save) { save = NULL; return NULL; }
    start = save;
    while (*save && !in_set(delim, (unsigned char)*save)) save++;
    if (*save) *save++ = 0;
    else save = NULL;
    return start;
}
const char *strerror(int e) {
    switch (e) {
    case EBUSY: return "mountpoint or device busy";
    case EXDEV: return "cross-device rename";
    case EROFS: return "read-only filesystem/device";
    case EPERM:     return "operation not permitted";
    case ENOENT:    return "no such file or directory";
    case ESRCH:     return "no such process";
    case EINTR:     return "interrupted";
    case EIO:       return "I/O error";
    case E2BIG:     return "argument list too long";
    case ENOEXEC:   return "not an executable";
    case EBADF:     return "bad file descriptor";
    case ECHILD:    return "no child processes";
    case EAGAIN:    return "too many processes";
    case ENOMEM:    return "out of memory";
    case EACCES:    return "permission denied";
    case EFAULT:    return "bad address";
    case EEXIST:    return "already exists";
    case ENOTDIR:   return "not a directory";
    case EISDIR:    return "is a directory";
    case EINVAL:    return "invalid argument";
    case ERANGE:    return "result out of range";
    case ESPIPE:    return "illegal seek";
    case EMFILE:    return "too many open files";
    case EFBIG:     return "file or filesystem too large";
    case ENOSPC:    return "no space left on disk";
    case EPIPE:     return "broken pipe";
    case ENAMETOOLONG: return "file name too long";
    case ENOTEMPTY: return "directory not empty";
    case ELOOP:     return "too many levels of symbolic links";
    case EMSGSIZE:  return "message too long";
    case EADDRINUSE:   return "address already in use";
    case ENETDOWN:     return "network is down";
    case ECONNRESET:   return "connection reset by peer";
    case EISCONN:      return "already connected";
    case ENOTCONN:     return "not connected";
    case ETIMEDOUT:    return "timed out";
    case ECONNREFUSED: return "connection refused";
    case EHOSTUNREACH: return "host unreachable";
    default:        return "unknown error";
    }
}
long atol(const char *s) { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }
int atoi(const char *s) {
    int sign = 1, v = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    return v * sign;
}

/* ================================================================ форматирование */

struct out { char *buf; size_t cap, len; };

static void emit(struct out *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void emit_num(struct out *o, unsigned v, unsigned base, bool upper, bool neg,
                     int width, char pad, bool left, bool plus, bool blank, int prec, bool alt) {
    char tmp[16], pfx[2];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char sign = neg ? '-' : plus ? '+' : blank ? ' ' : 0;
    unsigned orig = v;
    int n = 0, zeros, pn = 0, total, i;
    if (!(prec == 0 && v == 0)) {
        do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    }
    if (prec >= 0 && pad == '0') pad = ' ';
    zeros = prec > n ? prec - n : 0;
    if (alt && base == 8) {
        if (zeros == 0 && (n == 0 || tmp[n - 1] != '0')) zeros = 1;
    } else if (alt && base == 16 && orig != 0) {
        pfx[pn++] = '0';
        pfx[pn++] = upper ? 'X' : 'x';
    }
    total = n + zeros + pn + (sign ? 1 : 0);
    if (!left && pad == ' ') for (; total < width; width--) emit(o, ' ');
    if (sign) emit(o, sign);
    for (i = 0; i < pn; i++) emit(o, pfx[i]);
    if (!left && pad == '0') for (; total < width; width--) emit(o, '0');
    while (zeros--) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) for (; total < width; width--) emit(o, ' ');
}

static void emit_u64(struct out *o, unsigned long long v, unsigned base, bool upper, bool neg,
                     int width, char pad, bool left, bool plus, bool blank, int prec, bool alt) {
    char tmp[32], pfx[2];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char sign = neg ? '-' : plus ? '+' : blank ? ' ' : 0;
    unsigned long long orig = v;
    int n = 0, zeros, pn = 0, total, i;
    if (!(prec == 0 && v == 0)) {
        do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    }
    if (prec >= 0 && pad == '0') pad = ' ';
    zeros = prec > n ? prec - n : 0;
    if (alt && base == 8) {
        if (zeros == 0 && (n == 0 || tmp[n - 1] != '0')) zeros = 1;
    } else if (alt && base == 16 && orig != 0) {
        pfx[pn++] = '0';
        pfx[pn++] = upper ? 'X' : 'x';
    }
    total = n + zeros + pn + (sign ? 1 : 0);
    if (!left && pad == ' ') for (; total < width; width--) emit(o, ' ');
    if (sign) emit(o, sign);
    for (i = 0; i < pn; i++) emit(o, pfx[i]);
    if (!left && pad == '0') for (; total < width; width--) emit(o, '0');
    while (zeros--) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) for (; total < width; width--) emit(o, ' ');
}

/* 10^e для |e| <= 308. Нужен печати и разбору вещественных, без libm. */
static double dpow10(int e) {
    static const double p[] = { 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256 };
    int neg = e < 0, i;
    double r = 1;
    if (neg) e = -e;
    if (e > 308) return neg ? 0 : 1e300 * 1e300;
    for (i = 0; e && i < 9; i++, e >>= 1)
        if (e & 1) r *= p[i];
    return neg ? 1.0 / r : r;
}

static int fkind(double v, int *sign) {
    unsigned long long u;
    memcpy(&u, &v, 8);
    *sign = (int)(u >> 63);
    if (((u >> 52) & 0x7ff) == 0x7ff) return (u << 12) ? 2 : 1; /* nan, inf */
    if ((u << 1) == 0) return 0;
    return 3;
}

/* Старшая десятичная степень: 10^e <= v < 10^(e+1), v > 0. */
static int dec_exp(double v) {
    int lo = -324, hi = 308, ans = -324;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (dpow10(mid) <= v) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return ans;
}

/* prec значащих цифр числа v > 0. Возвращает степень первой цифры. */
static int sig_digits(double v, int prec, unsigned long long *digs) {
    int e = dec_exp(v), guard = 0;
    while (guard++ < 6) {
        double s = v / dpow10(e - (prec - 1));
        double hi = dpow10(prec);
        double lo = prec > 1 ? dpow10(prec - 1) : 1;
        unsigned long long n;
        if (!(s < hi)) { e++; continue; }
        if (s < lo) { e--; continue; }
        n = (unsigned long long)(s + 0.5);
        if ((double)n >= hi) { e++; continue; }
        if (prec > 1 && (double)n < lo) { e--; continue; }
        *digs = n;
        return e;
    }
    *digs = 0;
    return e;
}

static int ull_to_digits(unsigned long long v, char *dst) {
    char tmp[24];
    int n = 0, i;
    if (!v) { dst[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    return n;
}

static void emit_text(struct out *o, const char *s, int width, bool left) {
    int n = (int)strlen(s), i;
    if (!left) for (i = n; i < width; i++) emit(o, ' ');
    for (i = 0; i < n; i++) emit(o, s[i]);
    if (left) for (i = n; i < width; i++) emit(o, ' ');
}

#define BIG_N 96
typedef struct { unsigned v[BIG_N]; int n; } Big;

static void big_set(Big *b, unsigned long long x) {
    int i;
    for (i = 0; i < BIG_N; i++) b->v[i] = 0;
    b->v[0] = (unsigned)x;
    b->v[1] = (unsigned)(x >> 32);
    b->n = b->v[1] ? 2 : (b->v[0] ? 1 : 1);
}

static void big_mul(Big *b, unsigned m) {
    unsigned c = 0;
    int i;
    for (i = 0; i < b->n; i++) {
        unsigned x = b->v[i];
        unsigned p0 = (x & 0xffff) * m;
        unsigned p1 = (x >> 16) * m;
        unsigned lo = (p0 & 0xffff) + (c & 0xffff);
        unsigned mid = (p0 >> 16) + (p1 & 0xffff) + (c >> 16) + (lo >> 16);
        unsigned hi = (p1 >> 16) + (mid >> 16);
        b->v[i] = (lo & 0xffff) | ((mid & 0xffff) << 16);
        c = hi;
    }
    if (c && b->n < BIG_N) b->v[b->n++] = c;
}

static void big_shl(Big *b, int sh) {
    int i;
    while (sh-- > 0) {
        unsigned c = 0;
        for (i = 0; i < b->n; i++) {
            unsigned n = (b->v[i] << 1) | c;
            c = b->v[i] >> 31;
            b->v[i] = n;
        }
        if (c && b->n < BIG_N) b->v[b->n++] = c;
    }
}

static int big_bit(const Big *b, int k) {
    int w, bit;
    if (k < 0) return 0;
    w = k >> 5;
    bit = k & 31;
    if (w >= b->n) return 0;
    return (int)((b->v[w] >> bit) & 1);
}

static void big_add1(Big *b) {
    unsigned c = 1;
    int i;
    for (i = 0; c && i < BIG_N; i++) {
        unsigned long long t = (unsigned long long)b->v[i] + c;
        if (i >= b->n) b->n = i + 1;
        b->v[i] = (unsigned)t;
        c = (unsigned)(t >> 32);
    }
}

static void big_shr_round(Big *b, int sh) {
    int words, bits, i, bit, sticky = 0;
    unsigned c;
    if (sh <= 0) return;
    for (i = 0; i < sh - 1; i++) if (big_bit(b, i)) { sticky = 1; break; }
    bit = big_bit(b, sh - 1);
    words = sh >> 5;
    bits = sh & 31;
    if (words >= b->n) {
        for (i = 0; i < BIG_N; i++) b->v[i] = 0;
        b->n = 1;
        if (bit && sticky) b->v[0] = 1;
        return;
    }
    if (words) {
        for (i = 0; i < b->n - words; i++) b->v[i] = b->v[i + words];
        for (; i < b->n; i++) b->v[i] = 0;
        b->n -= words;
    }
    if (bits) {
        c = 0;
        for (i = b->n - 1; i >= 0; i--) {
            unsigned n = (b->v[i] >> bits) | c;
            c = b->v[i] << (32 - bits);
            b->v[i] = n;
        }
        while (b->n > 1 && !b->v[b->n - 1]) b->n--;
    }
    if (bit && (sticky || (b->v[0] & 1))) big_add1(b);
}

static unsigned big_div10(Big *b) {
    unsigned rem = 0;
    int i;
    for (i = b->n - 1; i >= 0; i--) {
        unsigned long long cur = ((unsigned long long)rem << 32) | b->v[i];
        b->v[i] = (unsigned)(cur / 10);
        rem = (unsigned)(cur % 10);
    }
    while (b->n > 1 && !b->v[b->n - 1]) b->n--;
    return rem;
}

/* %.Nf для больших чисел: точное значение * 10^N, чтобы tonumber вернул тот же double. */
static void emit_fixed_big(struct out *o, double v, int prec, int sch) {
    unsigned long long u, mant;
    int be, e, i, nd, pos;
    char digs[640];
    Big b;
    memcpy(&u, &v, 8);
    be = (int)((u >> 52) & 0x7ff);
    mant = u & ((1ULL << 52) - 1);
    if (be == 0) e = -1022 - 52;
    else { mant |= 1ULL << 52; e = be - 1023 - 52; }
    big_set(&b, mant);
    for (i = 0; i < prec; i++) big_mul(&b, 5);
    if (e + prec >= 0) big_shl(&b, e + prec);
    else big_shr_round(&b, -(e + prec));
    nd = 0;
    do { digs[nd++] = (char)('0' + big_div10(&b)); } while ((b.n > 1 || b.v[0]) && nd < (int)sizeof digs);
    if (sch) emit(o, (char)sch);
    if (nd <= prec) {
        emit(o, '0');
        if (prec > 0) {
            emit(o, '.');
            for (i = nd; i < prec; i++) emit(o, '0');
            for (i = nd - 1; i >= 0; i--) emit(o, digs[i]);
        }
    } else {
        for (pos = nd - 1; pos >= prec; pos--) emit(o, digs[pos]);
        if (prec > 0) {
            emit(o, '.');
            for (; pos >= 0; pos--) emit(o, digs[pos]);
        }
    }
}
static void emit_float(struct out *o, double v, int spec, int prec, int width, char pad,
                       bool left, bool alt, bool plus, bool blank) {
    int sign = 0, kind = fkind(v, &sign), exp, nd, i;
    unsigned long long digs = 0;
    char num[40];
    char sch = sign ? '-' : plus ? '+' : blank ? ' ' : 0;
    int upper = spec == 'E' || spec == 'G';
    if (spec == 'E') spec = 'e';
    if (spec == 'G') spec = 'g';
    if (kind == 2 || kind == 1) {
        char tmp[8];
        int n = 0;
        if (sch) tmp[n++] = sch;
        tmp[n++] = kind == 2 ? 'n' : 'i';
        tmp[n++] = kind == 2 ? 'a' : 'n';
        tmp[n++] = kind == 2 ? 'n' : 'f';
        tmp[n] = 0;
        emit_text(o, tmp, width, left);
        return;
    }
    if (spec == 'g') {
        if (prec < 0) prec = 6;
        if (prec == 0) prec = 1;
        if (prec > 17) prec = 17;
    } else {
        if (prec < 0) prec = 6;
        if (prec > 500) prec = 500;
    }
    if (spec == 'f' && kind == 3 && prec > 15) {
        emit_fixed_big(o, sign ? -v : v, prec, sch);
        return;
    }
    if (kind == 0) {
        exp = 0;
        digs = 0;
        nd = 1;
        num[0] = '0';
    } else {
        int use = spec == 'e' ? prec + 1 : spec == 'g' ? prec : 17;
        double av = sign ? -v : v;
        if (use < 1) use = 1;
        if (use > 17) use = 17;
        if (spec == 'f' && prec >= 0 && av < 1e15 && (prec == 0 || av * dpow10(prec) < 1e15)) {
            double scale = prec ? dpow10(prec) : 1;
            unsigned long long n = (unsigned long long)(av * scale + 0.5);
            nd = ull_to_digits(n, num);
            exp = nd - (prec > 0 ? prec : 0) - 1;
            if (prec > 0 && nd <= prec) exp = -1 - (prec - nd);
        } else {
            exp = sig_digits(av, use, &digs);
            nd = ull_to_digits(digs, num);
            while (nd < use) {
                memmove(num + 1, num, (size_t)nd);
                num[0] = '0';
                nd++;
            }
        }
    }
    {
        int sci = spec == 'e' || (spec == 'g' && (exp < -4 || exp >= prec));
        int frac = sci ? (spec == 'g' ? prec - 1 : prec) : prec;
        int before = exp + 1;
        int total, padn;
        if (sci) {
            int body = 1 + ((frac > 0 || alt) ? 1 + frac : 0);
            if (spec == 'g' && !alt) {
                int last = 1 + frac;
                while (last > 1 && (last - 1 >= nd || num[last - 1] == '0')) {
                    if (last - 1 < nd && num[last - 1] != '0') break;
                    if (last - 1 >= nd || num[last - 1] == '0') last--;
                    else break;
                }
                /* last is the count of significant characters before the exponent, without the dot if it would be last */
                frac = last - 1;
                if (frac < 0) frac = 0;
                body = 1 + (frac > 0 ? 1 + frac : 0);
            }
            total = (sch ? 1 : 0) + body + 2 + (exp < 0 || exp >= 100 ? 3 : 2);
            if (exp <= -100 || exp >= 100) total = (sch ? 1 : 0) + body + 2 + 3;
            else if (exp <= -10 || exp >= 10) total = (sch ? 1 : 0) + body + 2 + 2;
            else total = (sch ? 1 : 0) + body + 2 + 2; /* exponent is at least two digits */
        } else {
            int digits = before > 0 ? before : 1;
            total = (sch ? 1 : 0) + digits + ((frac > 0 || alt) ? 1 + frac : 0);
        }
        padn = width > total ? width - total : 0;
        if (!left && pad == ' ') for (; padn; padn--) emit(o, ' ');
        if (sch) emit(o, sch);
        if (!left && pad == '0') for (; padn; padn--) emit(o, '0');
        if (sci) {
            int en = exp, ei = 0, show;
            char eb[8];
            emit(o, nd > 0 ? num[0] : '0');
            show = frac;
            if (spec == 'g' && !alt) {
                while (show > 0 && (show >= nd || num[show] == '0')) show--;
            }
            if (show > 0 || alt) {
                emit(o, '.');
                for (i = 0; i < show; i++) emit(o, i + 1 < nd ? num[i + 1] : '0');
                if (alt) for (; i < frac; i++) emit(o, '0');
            }
            emit(o, upper ? 'E' : 'e');
            emit(o, en < 0 ? '-' : '+');
            if (en < 0) en = -en;
            if (!en) eb[ei++] = '0';
            while (en) { eb[ei++] = (char)('0' + en % 10); en /= 10; }
            if (ei < 2) eb[ei++] = '0';
            while (ei) emit(o, eb[--ei]);
        } else if (before <= 0) {
            emit(o, '0');
            if (frac > 0 || alt) {
                int z = -before, k;
                emit(o, '.');
                if (spec == 'g' && !alt) {
                    int last = frac;
                    while (last > 0) {
                        int idx = last - 1 + before;
                        char c = (idx >= 0 && idx < nd) ? num[idx] : '0';
                        if (c != '0') break;
                        last--;
                    }
                    frac = last;
                }
                for (k = 0; k < z && k < frac; k++) emit(o, '0');
                for (; k < frac; k++) {
                    int idx = k + before;
                    emit(o, (idx >= 0 && idx < nd) ? num[idx] : '0');
                }
            }
        } else {
            int cut = before + frac;
            if (spec == 'g' && !alt) {
                while (cut > before && (cut - 1 >= nd || num[cut - 1] == '0')) {
                    if (cut - 1 < nd && num[cut - 1] != '0') break;
                    cut--;
                }
                if (cut == before) frac = 0;
                else frac = cut - before;
            }
            for (i = 0; i < before; i++) emit(o, i < nd ? num[i] : '0');
            if (frac > 0 || alt) {
                emit(o, '.');
                for (i = 0; i < frac; i++) emit(o, before + i < nd ? num[before + i] : '0');
            }
        }
        if (left) for (; padn; padn--) emit(o, ' ');
    }
}

/* %d %i %u %x %X %c %s %p %lld %f %e %g %%; флаги '-'+' '#' и '0', ширина, точность */
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    /* «o += snprintf(buf + o, max - o, ...)» после переполнения даёт отрицательный размер,
     * который превращается в огромный size_t: считаем его нулём, а не «писать сколько угодно» */
    if ((int)cap < 0) cap = 0;
    struct out o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        bool left = false, alt = false, plus = false, blank = false;
        char pad = ' ';
        int width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') blank = true;
            else if (*fmt == '#') alt = true;
            else if (*fmt == '0') pad = '0';
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = true; width = -width; } }
        while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { fmt++; lng = lng < 1 ? 1 : lng; }
        switch (*fmt) {
        case 'd': case 'i': {
            if (lng >= 2) {
                long long sv = va_arg(ap, long long);
                unsigned long long u = (unsigned long long)sv;
                if (sv < 0) u = -u;
                emit_u64(&o, u, 10, false, sv < 0, width, pad, left, plus, blank, prec, alt);
            } else {
                int v = va_arg(ap, int);
                emit_num(&o, v < 0 ? -(unsigned)v : (unsigned)v, 10, false, v < 0, width, pad, left, plus, blank, prec, alt);
            }
            break;
        }
        case 'u':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 10, false, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 10, false, false, width, pad, left, false, false, prec, alt);
            break;
        case 'o':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 8, false, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 8, false, false, width, pad, left, false, false, prec, alt);
            break;
        case 'x':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 16, false, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 16, false, false, width, pad, left, false, false, prec, alt);
            break;
        case 'X':
            if (lng >= 2) emit_u64(&o, va_arg(ap, unsigned long long), 16, true, false, width, pad, left, false, false, prec, alt);
            else emit_num(&o, va_arg(ap, unsigned), 16, true, false, width, pad, left, false, false, prec, alt);
            break;
        case 'p': {
            unsigned v = va_arg(ap, unsigned);
            char tmp[16];
            int n = 0, k;
            tmp[n++] = '0';
            tmp[n++] = 'x';
            for (k = 28; k >= 0; k -= 4) {
                int d = (v >> k) & 0xf;
                tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d);
            }
            if (width < n) width = n;
            if (!left) while (width > n) { emit(&o, ' '); width--; }
            for (k = 0; k < n; k++) emit(&o, tmp[k]);
            if (left) while (width > n) { emit(&o, ' '); width--; }
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G':
            emit_float(&o, va_arg(ap, double), *fmt, prec, width, pad, left, alt, plus, blank);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int k;
            if (!left) for (k = 1; k < width; k++) emit(&o, ' ');
            emit(&o, ch);
            if (left) for (k = 1; k < width; k++) emit(&o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) for (int k = len; k < width; k++) emit(&o, ' ');
            for (int k = 0; k < len; k++) emit(&o, s[k]);
            if (left) for (int k = len; k < width; k++) emit(&o, ' ');
            break;
        }
        case '%': emit(&o, '%'); break;
        case 0: fmt--; break;
        default: emit(&o, '%'); emit(&o, *fmt);
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static int vdprintf(int fd, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(fd, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    return n;
}

void __libc_init(void);
int printf(const char *fmt, ...) {
    va_list ap;
    __libc_init();
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) { write(STDOUT, s, strlen(s)); write(STDOUT, "\n", 1); return 0; }
int putchar(int c) { char ch = c; write(STDOUT, &ch, 1); return c; }

/* Построчное чтение с буфером на каждый дескриптор: read() по байту был бы
 * системным вызовом на каждый символ. */
#define RL_FDS 16
static struct { char buf[256]; int pos, len; } rl[RL_FDS];

int readln(int fd, char *out, int max) {
    if (fd < 0 || fd >= RL_FDS) return -1;
    int n = 0;
    for (;;) {
        if (rl[fd].pos == rl[fd].len) {
            rl[fd].pos = 0;
            rl[fd].len = read(fd, rl[fd].buf, sizeof(rl[fd].buf));
            if (rl[fd].len <= 0) { rl[fd].len = 0; out[n] = 0; return n ? n : -1; }
        }
        char c = rl[fd].buf[rl[fd].pos++];
        if (c == '\n') { out[n] = 0; return n; }
        if (n < max - 1) out[n++] = c;
    }
}

int getln(char *buf, int max) {
    int n = read(STDIN, buf, max - 1);
    if (n <= 0) { buf[0] = 0; return -1; }
    if (buf[n - 1] == '\n') n--;
    buf[n] = 0;
    return n;
}

/* ================================================================ malloc
 * Размеры округляем до «классов» (16, 32, ... 256, затем 512, 1024, ... 64 КБ;
 * крупнее — шагами в четверть: 80, 96, 112, 128 КБ, 160, 192 ... — иначе буфер
 * экрана 1920x1200 в 9 МБ занимал бы 16 МБ).
 * У каждого класса свой список освободившихся блоков: malloc берёт первый из
 * списка, free кладёт блок обратно — оба за постоянное время, сколько бы блоков
 * ни было. Новую память берём у ядра через sbrk() кусками по 64 КБ. */

struct hdr {
    size_t size;            /* размер данных (= размер класса) */
    size_t magic;
    struct hdr *next;       /* в списке свободных */
    size_t pad;             /* чтобы данные были выровнены на 16 */
};

#define HEAP_MAGIC 0x4D594F53u                      /* "MYOS" */
#define N_CLASSES 96
static struct hdr *free_list[N_CLASSES];
static char *wild, *wild_end;                       /* ещё не нарезанный кусок от sbrk */

static int size_class(size_t n, size_t *rounded) {
    if (n <= 256) { n = (n + 15) & ~15u; *rounded = n; return n / 16 - 1; }    /* 0..15 */
    size_t c = 512;
    int k = 16;
    if (n <= 65536) {
        while (c < n) { c <<= 1; k++; }
        *rounded = c;
        return k;                                   /* 16..23 */
    }
    size_t base = 65536;
    k = 24;
    for (;;) {
        for (int q = 5; q <= 8; q++, k++) {
            c = base / 4 * q;
            if (c >= n) { *rounded = c; return k; }
        }
        base <<= 1;
    }
}

static void *malloc_unlocked(size_t n) {
    if (n == 0) n = 1;
    if (n > 0x40000000u) return NULL;
    size_t sz;
    int k = size_class(n, &sz);
    struct hdr *h = free_list[k];
    if (h) {
        free_list[k] = h->next;
        h->next = NULL;
        return h + 1;
    }
    size_t need = sizeof(struct hdr) + sz;
    if ((size_t)(wild_end - wild) < need) {
        size_t chunk = need > 65536 ? need : 65536;
        char *p = sbrk(chunk);
        if (p == (void *)-1 && chunk > need) { chunk = need; p = sbrk(need); }   /* хоть сколько-нибудь */
        if (p == (void *)-1) {
            /* память у ядра кончилась: разрезать свободный блок побольше — он станет новым
             * «неразрезанным куском» (остаток старого куска пропадёт, он маленький) */
            int j = k + 1;
            while (j < N_CLASSES && !free_list[j]) j++;
            if (j == N_CLASSES) return NULL;
            struct hdr *big = free_list[j];
            free_list[j] = big->next;
            wild = (char *)big;
            wild_end = (char *)(big + 1) + big->size;
        } else {
            if (p != wild_end) wild = p;            /* новый кусок не впритык к старому */
            wild_end = p + chunk;
        }
    }
    h = (struct hdr *)wild;
    wild += need;
    h->size = sz;
    h->magic = HEAP_MAGIC;
    h->next = NULL;
    return h + 1;
}

static void free_unlocked(void *p) {
    if (!p) return;
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != HEAP_MAGIC) return;             /* не наш указатель — не трогаем */
    size_t sz;
    int k = size_class(h->size, &sz);
    h->next = free_list[k];
    free_list[k] = h;
}

static thread_mutex_t heap_lock;
void *malloc(size_t n) {
    thread_mutex_lock(&heap_lock);
    void *p=malloc_unlocked(n);
    thread_mutex_unlock(&heap_lock);
    return p;
}
void free(void *p) {
    if (!p) return;
    thread_mutex_lock(&heap_lock);
    free_unlocked(p);
    thread_mutex_unlock(&heap_lock);
}
void *calloc(size_t n, size_t size) {
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *reallocarray(void *p, size_t n, size_t size) {
    if (n && size > (size_t)-1 / n) { errno = ENOMEM; return NULL; }
    return realloc(p, n * size);
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    thread_mutex_lock(&heap_lock);
    struct hdr *h = (struct hdr *)p - 1;
    if (h->magic != HEAP_MAGIC) {
        thread_mutex_unlock(&heap_lock);
        errno = EINVAL;
        return NULL;
    }
    if (h->size >= n) {
        thread_mutex_unlock(&heap_lock);
        return p;
    }
    size_t old = h->size;
    void *q = malloc_unlocked(n);
    if (q) {
        memcpy(q, p, old);
        free_unlocked(p);
    }
    thread_mutex_unlock(&heap_lock);
    return q;
}

/* ================================================================ случайные числа */

static unsigned rnd_state = 1;
void srand(unsigned seed) { rnd_state = seed ? seed : 1; }
unsigned rand(void) { rnd_state = rnd_state * 1103515245 + 12345; return (rnd_state >> 16) & 0x7FFF; }

/* ================================================================ сеть */

int mount(const char *device,const char *target) { return syscall3(SYS_MOUNT,(int)device,(int)target,0); }
int mkfs(const char *device) { return syscall3(SYS_MKFS,(int)device,0,0); }
int partctl(const char *device,unsigned op,unsigned slot,unsigned start,unsigned sectors) {
    unsigned args[3]; args[0]=slot; args[1]=start; args[2]=sectors;
    return syscall3(SYS_PARTCTL,(int)device,op,(int)args);
}
int mountinfo(unsigned index,char device[128],char target[128]) { return syscall3(SYS_MOUNTINFO,index,(int)device,(int)target); }
int blockinfo(unsigned index,struct block_info *info) { return syscall3(SYS_BLOCKINFO,index,(int)info,0); }
int socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) { errno = EPROTONOSUPPORT; return -1; }
#if defined(__LINUX_TEST__)
    { unsigned long a[3]; a[0] = domain; a[1] = type; a[2] = protocol; return ioerr(__syscall(102, 1, (int)a, 0)); }
#else
    return ioerr(syscall3(SYS_SOCKET, type, 0, 0));
#endif
}
int socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    if (!sv) { errno = EFAULT; return -1; }
    if (domain != AF_UNIX) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM) { errno = EPROTONOSUPPORT; return -1; }
#if defined(__LINUX_TEST__)
    { unsigned long a[4]; a[0] = domain; a[1] = type; a[2] = protocol; a[3] = (unsigned long)sv;
      return ioerr(__syscall(102, 8, (int)a, 0)); }
#else
    return ioerr(pipe(sv));
#endif
}

static void sockaddr_from_kernel(struct sockaddr *out, socklen_t *len, unsigned ip, unsigned port) {
    struct sockaddr_in in;
    socklen_t n;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(ip);
    in.sin_port = htons((unsigned short)port);
    n = sizeof(in);
    if (out && len) {
        if (*len < n) n = *len;
        memcpy(out, &in, n);
        *len = sizeof(in);
    }
}

static int sockaddr_to_kernel(const struct sockaddr *addr, socklen_t len, unsigned *ip, unsigned *port) {
    const struct sockaddr_in *in;
    if (!addr || len < sizeof(struct sockaddr_in) || addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    in = (const struct sockaddr_in *)addr;
    *ip = ntohl(in->sin_addr.s_addr);
    *port = ntohs(in->sin_port);
    return 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    unsigned ip, port;
    if (sockaddr_to_kernel(addr, len, &ip, &port) < 0) return -1;
#if defined(__LINUX_TEST__)
    { unsigned long a[3]; a[0] = fd; a[1] = (unsigned long)addr; a[2] = len; return ioerr(__syscall(102, 3, (int)a, 0)); }
#else
    return ioerr(syscall3(SYS_CONNECT, fd, (int)ip, (int)port));
#endif
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    unsigned ip, port;
    if (sockaddr_to_kernel(addr, len, &ip, &port) < 0) return -1;
#if defined(__LINUX_TEST__)
    { unsigned long a[3]; a[0] = fd; a[1] = (unsigned long)addr; a[2] = len; return ioerr(__syscall(102, 2, (int)a, 0)); }
#else
    if (ip != 0) { errno = EADDRNOTAVAIL; return -1; }
    return ioerr(syscall3(SYS_BIND, fd, (int)port, 0));
#endif
}

int listen(int fd, int backlog) {
#if defined(__LINUX_TEST__)
    { unsigned long a[2]; a[0] = fd; a[1] = backlog; return ioerr(__syscall(102, 4, (int)a, 0)); }
#else
    return ioerr(syscall3(SYS_LISTEN, fd, backlog, 0));
#endif
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
#if defined(__LINUX_TEST__)
    unsigned long a[3];
    a[0] = fd; a[1] = (unsigned long)addr; a[2] = (unsigned long)len;
    return ioerr(__syscall(102, 5, (int)a, 0));
#else
    struct { unsigned ip, port; } raw;
    int r = ioerr(syscall3(SYS_ACCEPT, fd, (int)&raw, 0));
    if (r >= 0) sockaddr_from_kernel(addr, len, raw.ip, raw.port);
    return r;
#endif
}

static int socknames(int fd, struct sockaddr *addr, socklen_t *len, int peer) {
#if defined(__LINUX_TEST__)
    unsigned long a[3];
    a[0] = fd; a[1] = (unsigned long)addr; a[2] = (unsigned long)len;
    return ioerr(__syscall(102, peer ? 7 : 6, (int)a, 0));
#else
    unsigned v[4];
    int r = ioerr(syscall3(SYS_SSHCTL, 5, fd, (int)v));
    if (r < 0) return -1;
    sockaddr_from_kernel(addr, len, peer ? v[2] : v[0], peer ? v[3] : v[1]);
    return 0;
#endif
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) { return socknames(fd, addr, len, 0); }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) { return socknames(fd, addr, len, 1); }

int shutdown(int fd, int how) {
#if defined(__LINUX_TEST__)
    unsigned long a[2];
    a[0] = fd; a[1] = how;
    return ioerr(__syscall(102, 13, (int)a, 0));
#else
    (void)how;
    return ioerr(syscall3(SYS_SSHCTL, 9, fd, 0));
#endif
}

int setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
#if defined(__LINUX_TEST__)
    unsigned long a[5];
    /* SO_NOSIGPIPE есть в наших заголовках для гостя, у Linux его нет. */
    if (level == SOL_SOCKET && opt == SO_NOSIGPIPE) return 0;
    a[0] = fd; a[1] = level; a[2] = opt; a[3] = (unsigned long)val; a[4] = len;
    return ioerr(__syscall(102, 14, (int)a, 0));
#else
    (void)fd; (void)val; (void)len;
    if ((level == SOL_SOCKET && (opt == SO_REUSEADDR || opt == SO_KEEPALIVE || opt == SO_LINGER ||
                                 opt == SO_RCVTIMEO || opt == SO_SNDTIMEO || opt == SO_NOSIGPIPE)) ||
        (level == IPPROTO_TCP && opt == TCP_NODELAY))
        return 0;
    errno = ENOPROTOOPT;
    return -1;
#endif
}

int getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
#if defined(__LINUX_TEST__)
    unsigned long a[5];
    a[0] = fd; a[1] = level; a[2] = opt; a[3] = (unsigned long)val; a[4] = (unsigned long)len;
    return ioerr(__syscall(102, 15, (int)a, 0));
#else
    if (!len || *len < sizeof(int)) { errno = EINVAL; return -1; }
    if (level == SOL_SOCKET && opt == SO_ERROR) { *(int *)val = 0; *len = sizeof(int); return 0; }
    if (level == SOL_SOCKET && opt == SO_RCVBUF) { *(int *)val = 32768; *len = sizeof(int); return 0; }
    if (level == SOL_SOCKET && opt == SO_SNDBUF) { *(int *)val = 16384; *len = sizeof(int); return 0; }
    if (level == SOL_SOCKET && opt == SO_TYPE) { *(int *)val = SOCK_STREAM; *len = sizeof(int); return 0; }
    (void)fd;
    errno = ENOPROTOOPT;
    return -1;
#endif
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    if (flags & ~MSG_NOSIGNAL) { errno = EOPNOTSUPP; return -1; }
    return write(fd, buf, len);
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    if (flags) { errno = EOPNOTSUPP; return -1; }
    return read(fd, buf, len);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
    unsigned ip, port;
    if (flags & ~MSG_NOSIGNAL) { errno = EOPNOTSUPP; return -1; }
    if (!to) return send(fd, buf, len, 0);
    if (sockaddr_to_kernel(to, tolen, &ip, &port) < 0) return -1;
#if defined(__LINUX_TEST__)
    { unsigned long a[6]; a[0] = fd; a[1] = (unsigned long)buf; a[2] = len; a[3] = flags & ~MSG_NOSIGNAL;
      a[4] = (unsigned long)to; a[5] = tolen; return ioerr(__syscall(102, 11, (int)a, 0)); }
#else
    return ioerr(syscall5(SYS_SENDTO, fd, (int)buf, (int)len, (int)ip, (int)port));
#endif
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen) {
    if (flags) { errno = EOPNOTSUPP; return -1; }
#if defined(__LINUX_TEST__)
    { unsigned long a[6]; a[0] = fd; a[1] = (unsigned long)buf; a[2] = len; a[3] = 0;
      a[4] = (unsigned long)from; a[5] = (unsigned long)fromlen; return ioerr(__syscall(102, 12, (int)a, 0)); }
#else
    struct { unsigned ip, port; } raw;
    int r = ioerr(syscall5(SYS_RECVFROM, fd, (int)buf, (int)len, (int)&raw, 0));
    if (r >= 0 && from && fromlen) sockaddr_from_kernel(from, fromlen, raw.ip, raw.port);
    return r;
#endif
}

unsigned short htons(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }
unsigned short ntohs(unsigned short v) { return htons(v); }
unsigned htonl(unsigned v) { return (v << 24) | ((v << 8) & 0xff0000) | ((v >> 8) & 0xff00) | (v >> 24); }
unsigned ntohl(unsigned v) { return htonl(v); }

int inet_pton(int af, const char *s, void *dst) {
    unsigned ip;
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (!ip_parse(s, &ip)) return 0;
    ((struct in_addr *)dst)->s_addr = htonl(ip);
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t n) {
    char tmp[16];
    if (af != AF_INET) { errno = EAFNOSUPPORT; return 0; }
    ip_str(ntohl(((const struct in_addr *)src)->s_addr), tmp);
    if (strlen(tmp) >= n) { errno = ENOSPC; return 0; }
    strcpy(dst, tmp);
    return dst;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    unsigned ip, port;
    char *end;
    unsigned long v;
    struct addrinfo *ai;
    struct sockaddr_in *in;
    *res = 0;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) return EAI_FAMILY;
    port = 0;
    if (service && *service) {
        v = strtoul(service, &end, 10);
        if (*end || v > 65535) return EAI_SERVICE;
        port = (unsigned)v;
    }
    if (!node) ip = (hints && (hints->ai_flags & AI_PASSIVE)) ? 0 : 0x7f000001u;
    else if (resolve(node, &ip) < 0) return EAI_NONAME;
    ai = calloc(1, sizeof(*ai));
    in = calloc(1, sizeof(*in));
    if (!ai || !in) { free(ai); free(in); return EAI_MEMORY; }
    in->sin_family = AF_INET;
    in->sin_port = htons((unsigned short)port);
    in->sin_addr.s_addr = htonl(ip);
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = IPPROTO_TCP;
    ai->ai_addr = (struct sockaddr *)in;
    ai->ai_addrlen = sizeof(*in);
    if (node && hints && (hints->ai_flags & AI_CANONNAME)) ai->ai_canonname = strdup(node);
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai) {
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_addr);
        free(ai->ai_canonname);
        free(ai);
        ai = next;
    }
}

const char *gai_strerror(int err) { (void)err; return "address lookup failed"; }

int h_errno;

char *inet_ntoa(struct in_addr in) {
    static char buf[16];
    ip_str(ntohl(in.s_addr), buf);
    return buf;
}

int getnameinfo(const struct sockaddr *sa, socklen_t len, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    const struct sockaddr_in *in;
    char tmp[8];
    (void)flags;
    if (!sa || sa->sa_family != AF_INET || len < sizeof(*in)) return EAI_FAMILY;
    in = (const struct sockaddr_in *)sa;
    if (host && hostlen && !inet_ntop(AF_INET, &in->sin_addr, host, hostlen)) return EAI_FAIL;
    if (serv && servlen) {
        snprintf(tmp, sizeof(tmp), "%u", ntohs(in->sin_port));
        if (strlen(tmp) >= servlen) return EAI_FAIL;
        strcpy(serv, tmp);
    }
    return 0;
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static char *aliases[1];
    static unsigned char addr[4];
    static char *list[2];
    unsigned ip;
    if (resolve(name, &ip) < 0) { h_errno = 1; return 0; }
    addr[0] = (char)(ip >> 24);
    addr[1] = (char)(ip >> 16);
    addr[2] = (char)(ip >> 8);
    addr[3] = (char)ip;
    list[0] = (char *)addr;
    list[1] = 0;
    he.h_name = (char *)name;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = list;
    return &he;
}

struct servent *getservbyname(const char *name, const char *proto) {
    static struct servent se;
    static char nbuf[8];
    int port;
    (void)proto;
    if (!name) return 0;
    if (!strcmp(name, "http")) port = 80;
    else if (!strcmp(name, "https")) port = 443;
    else return 0;
    snprintf(nbuf, sizeof(nbuf), "%s", name);
    se.s_name = nbuf;
    se.s_aliases = 0;
    se.s_port = htons((unsigned short)port);
    se.s_proto = "tcp";
    return &se;
}

int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
    struct pollfd p[FD_SETSIZE];
    int i, n, count, timeout;
    if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }
    for (i = 0; i < nfds; i++) {
        p[i].fd = i;
        p[i].events = 0;
        p[i].revents = 0;
        if (r && FD_ISSET(i, r)) p[i].events |= POLLIN;
        if (w && FD_ISSET(i, w)) p[i].events |= POLLOUT;
        if (!p[i].events) p[i].fd = -1;
    }
    timeout = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    n = poll(p, (unsigned)nfds, timeout);
    if (n <= 0) return n;
    if (r) FD_ZERO(r);
    if (w) FD_ZERO(w);
    if (e) FD_ZERO(e);
    count = 0;
    for (i = 0; i < nfds; i++) {
        if (r && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(i, r); count++; }
        if (w && (p[i].revents & POLLOUT)) { FD_SET(i, w); count++; }
    }
    return count;
}
int sysinfo(struct system_info *out, unsigned size) { return syscall3(SYS_SYSINFO, (int)out, size, 0); }
int cpuinfo(struct cpu_info *out, unsigned capacity) { return syscall3(SYS_CPUINFO, (int)out, capacity, 0); }
int hwinfo(unsigned kind, unsigned index, void *out, unsigned size) {
    return syscall5(SYS_HWINFO,kind,index,(int)out,size,0);
}
int klog_read(void *buffer,unsigned capacity,struct klog_cursor *cursor) {
    return ioerr(syscall3(SYS_KLOG_READ,(int)buffer,capacity,(int)cursor));
}
int reboot(int how) { return ioerr(syscall3(SYS_REBOOT, how, 0, 0)); }
int netinfo(struct net_info *out)          { return syscall3(SYS_NETINFO, (int)out, 0, 0); }
int ping(unsigned ip, int seq, unsigned t) { return syscall3(SYS_PING, (int)ip, seq, (int)t); }
int set_dns(unsigned ip)                   { return syscall3(SYS_NETCONFIG, 1, (int)ip, 0); }

int ip_parse(const char *s, unsigned *ip) {
    unsigned v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit(*s)) return 0;
        unsigned n = 0;
        while (isdigit(*s)) { n = n * 10 + (*s++ - '0'); if (n > 255) return 0; }
        v = (v << 8) | n;
        if (part < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    *ip = v;
    return 1;
}

char *ip_str(unsigned ip, char *buf) {
    snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

/* DNS: один UDP-запрос «дай A-запись» к серверу из netinfo, ответ ищем
 * среди записей. Имена в ответе бывают сжаты (ссылка 0xC0xx) — их пропускаем. */
static int dns_skip_name(const unsigned char *p, int pos, int len) {
    while (pos < len) {
        unsigned char c = p[pos];
        if (c == 0) return pos + 1;
        if ((c & 0xC0) == 0xC0) return pos + 2;
        pos += c + 1;
    }
    return -1;
}

/* кэш DNS: браузер спрашивает одно и то же имя на каждую картинку и скрипт */
#define DNS_CACHE 32
static struct { char name[64]; unsigned ip, until; int err; } dns_cache[DNS_CACHE];
static int dns_next;

static int dns_resolve(const char *name, unsigned *ip);

int resolve(const char *name, unsigned *ip) {
    if (ip_parse(name, ip)) return 0;
    if (!strcmp(name, "localhost")) { *ip = IP(127, 0, 0, 1); return 0; }
    char low[64];
    int n = 0;
    for (; name[n] && n < 63; n++) low[n] = tolower(name[n]);
    low[n] = 0;
    unsigned now = ticks();
    if (!name[n])
        for (int i = 0; i < DNS_CACHE; i++)
            if (dns_cache[i].name[0] && !strcmp(dns_cache[i].name, low) && (int)(dns_cache[i].until - now) > 0) {
                *ip = dns_cache[i].ip;
                return dns_cache[i].err;
            }
    int r = dns_resolve(name, ip);
    if (!name[n] && (r == 0 || r == -ENOENT)) {           /* ответ (или «нет такого имени») запомним */
        int k = dns_next++ % DNS_CACHE;
        strcpy(dns_cache[k].name, low);
        dns_cache[k].ip = r ? 0 : *ip;
        dns_cache[k].err = r;
        dns_cache[k].until = now + (r ? 30000 : 300000);  /* 30 с для ошибки, 5 минут для адреса */
    }
    return r;
}

static int dns_resolve(const char *name, unsigned *ip) {
    struct net_info ni;
    if (netinfo(&ni) < 0 || !ni.up) return -ENETDOWN;
    if (!ni.dns) return -EHOSTUNREACH;

    unsigned char q[300];
    int n = strlen(name);
    if (n == 0 || n > 250) return -EINVAL;
    unsigned short id = (unsigned short)(ticks() * 7919 + getpid());
    memset(q, 0, 12);
    q[0] = id >> 8; q[1] = id; q[2] = 0x01;          /* рекурсия, пожалуйста */
    q[5] = 1;                                         /* один вопрос */
    int pos = 12;
    const char *s = name;
    while (*s) {                                      /* www.example.com -> 3www7example3com0 */
        const char *dot = strchr(s, '.');
        int l = dot ? dot - s : (int)strlen(s);
        if (l == 0 || l > 63) return -EINVAL;
        q[pos++] = l;
        memcpy(q + pos, s, l); pos += l;
        s += l;
        if (*s == '.') s++;
    }
    q[pos++] = 0;
    q[pos++] = 0; q[pos++] = 1;                       /* тип A */
    q[pos++] = 0; q[pos++] = 1;                       /* класс IN */

    int fd = syscall3(SYS_SOCKET, SOCK_UDP, 0, 0);
    if (fd < 0) return fd;
    int err = -ETIMEDOUT;
    for (int attempt = 0; attempt < 2 && err == -ETIMEDOUT; attempt++) {
        int r = syscall5(SYS_SENDTO, fd, (int)q, pos, (int)ni.dns, 53);
        if (r < 0 && r != -EHOSTUNREACH) { err = r; break; }   /* ARP ещё не знает сервер — повторим */
        unsigned char a[512];
        struct { unsigned ip, port; } from;
        for (;;) {
            r = syscall5(SYS_RECVFROM, fd, (int)a, (int)sizeof(a), (int)&from, 1500);
            if (r < 0) { if (r != -ETIMEDOUT) err = r; break; }
            if (r < 12 || a[0] != q[0] || a[1] != q[1]) continue;   /* чужой ответ */
            if ((a[3] & 0x0F) == 3) { err = -ENOENT; break; }        /* NXDOMAIN: имени нет */
            if ((a[3] & 0x0F) != 0) { err = -EHOSTUNREACH; break; }  /* сервер не смог ответить */
            int qd = (a[4] << 8) | a[5], an = (a[6] << 8) | a[7], p = 12;
            for (int i = 0; i < qd && p > 0; i++) { p = dns_skip_name(a, p, r); if (p > 0) p += 4; }
            err = -ENOENT;
            for (int i = 0; i < an && p > 0 && p < r; i++) {
                p = dns_skip_name(a, p, r);
                if (p < 0 || p + 10 > r) break;
                int type = (a[p] << 8) | a[p + 1], rdlen = (a[p + 8] << 8) | a[p + 9];
                p += 10;
                if (type == 1 && rdlen == 4 && p + 4 <= r) {
                    *ip = ((unsigned)a[p] << 24) | (a[p + 1] << 16) | (a[p + 2] << 8) | a[p + 3];
                    err = 0;
                    break;
                }
                p += rdlen;                               /* CNAME и прочее — дальше */
            }
            break;
        }
    }
    close(fd);
    return err;
}

int tcp_connect(const char *host, int port) { return tcp_connect_timeout(host, port, 0); }

int tcp_connect_timeout(const char *host, int port, unsigned ms) {
    unsigned ip;
    struct sockaddr_in in;
    int fd, r;
    r = resolve(host, &ip);
    if (r < 0) return r;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    if (ms) sockctl(fd, SOCK_CONNTIMEO, ms);
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons((unsigned short)port);
    in.sin_addr.s_addr = htonl(ip);
    for (int attempt = 0; ; attempt++) {
        r = connect(fd, (struct sockaddr *)&in, sizeof(in));
        if (r < 0) r = -errno;
        if (r != -EHOSTUNREACH || attempt == 2) break;   /* ARP ещё не ответил — чуть подождём */
        sleep_ms(200);
    }
    if (r < 0) { close(fd); return r; }
    return fd;
}

/* ================================================================ общение процессов */

int msg_send(int pid, struct msg *m)            { return syscall3(SYS_MSG_SEND, pid, (int)m, 0); }
int msg_recv(struct msg *m, int timeout_ms)     { return syscall3(SYS_MSG_RECV, (int)m, timeout_ms, 0); }
int shm_alloc(unsigned size)                    { return syscall3(SYS_SHM_ALLOC, (int)size, 0, 0); }
void *shm_map(int id) {
    int r = syscall3(SYS_SHM_MAP, id, 0, 0);
    if(r<0 && r>-4096){errno=-r;return NULL;}return (void*)r;  /* адреса общей памяти «отрицательные» как int */
}
int shm_grant(int id,int pid,unsigned rights){unsigned grant[2]={pid,rights};return syscall3(SYS_IPCCTL,1,id,(int)grant);}
int shm_discard(int id){return syscall3(SYS_IPCCTL,2,id,0);}
int shm_size(int id){return syscall3(SYS_IPCCTL,4,id,0);}
int msg_allow(int pid,int allow){return syscall3(SYS_IPCCTL,3,pid,allow);}
int shm_unmap(void *addr)                       { return syscall3(SYS_SHM_UNMAP, (int)addr, 0, 0); }
int unitctl(unsigned op, const char *name, struct unit_info *out, unsigned index) {
    return syscall3(SYS_UNITCTL, op, op == UNIT_LIST ? (int)index : (int)name, (int)out);
}

int service_register(const char *name)          { return syscall3(SYS_SERVICE, 1, (int)name, 0); }
int service_find(const char *name)              { return syscall3(SYS_SERVICE, 2, (int)name, 0); }
int datetime_get(struct datetime *t)            { return syscall3(SYS_TIME, (int)t, 0, 0); }
int get_time(struct datetime *t)                { return datetime_get(t); } /* устар. имя */



int account(int op,const char *name,const char *password) { return syscall3(SYS_ACCOUNT,op,(int)name,(int)password); }
int account_list(char *name,unsigned index) { return syscall3(SYS_ACCOUNT,7,(int)name,index); }

/* ================================================================ 64-битная арифметика
 * Имена — как у libgcc: gcc -m32 без libgcc зовёт их из программ и из самого cc.
 * Внутри только 32-битные операции, иначе помощник вызвал бы сам себя. */

typedef union {
    unsigned long long u;
    struct { unsigned lo, hi; } p;
} di_parts;

static void mul32_full(unsigned a, unsigned b, unsigned *lo, unsigned *hi) {
    unsigned a0 = a & 0xFFFF, a1 = a >> 16, b0 = b & 0xFFFF, b1 = b >> 16;
    unsigned p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    unsigned mid = (p0 >> 16) + (p1 & 0xFFFF) + (p2 & 0xFFFF);
    *lo = (p0 & 0xFFFF) | (mid << 16);
    *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

unsigned long long __muldi3(unsigned long long a, unsigned long long b) {
    di_parts A, B, R;
    unsigned hi, extra, ign;
    A.u = a; B.u = b;
    mul32_full(A.p.lo, B.p.lo, &R.p.lo, &hi);
    mul32_full(A.p.lo, B.p.hi, &extra, &ign); hi += extra;
    mul32_full(A.p.hi, B.p.lo, &extra, &ign); hi += extra;
    R.p.hi = hi;
    return R.u;
}

long long __ashldi3(long long a, int n) {
    di_parts x; x.u = (unsigned long long)a;
    if (n <= 0) return a;
    n &= 63;
    if (n >= 32) { x.p.hi = x.p.lo << (n - 32); x.p.lo = 0; }
    else { x.p.hi = (x.p.hi << n) | (x.p.lo >> (32 - n)); x.p.lo <<= n; }
    return (long long)x.u;
}

unsigned long long __lshrdi3(unsigned long long a, int n) {
    di_parts x; x.u = a;
    if (n <= 0) return a;
    n &= 63;
    if (n >= 32) { x.p.lo = x.p.hi >> (n - 32); x.p.hi = 0; }
    else { x.p.lo = (x.p.lo >> n) | (x.p.hi << (32 - n)); x.p.hi >>= n; }
    return x.u;
}

long long __ashrdi3(long long a, int n) {
    di_parts x; x.u = (unsigned long long)a;
    if (n <= 0) return a;
    n &= 63;
    if (n >= 32) { x.p.lo = (unsigned)((int)x.p.hi >> (n - 32)); x.p.hi = (unsigned)((int)x.p.hi >> 31); }
    else {
        x.p.lo = (x.p.lo >> n) | (x.p.hi << (32 - n));
        x.p.hi = (unsigned)((int)x.p.hi >> n);
    }
    return (long long)x.u;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d) {
    di_parts N, D, Q, R;
    if (!d) return 0;
    N.u = n; D.u = d; Q.u = 0; R.u = 0;
    for (int i = 63; i >= 0; i--) {
        unsigned bit = i >= 32 ? (N.p.hi >> (i - 32)) & 1 : (N.p.lo >> i) & 1;
        R.p.hi = (R.p.hi << 1) | (R.p.lo >> 31);
        R.p.lo = (R.p.lo << 1) | bit;
        if (R.p.hi > D.p.hi || (R.p.hi == D.p.hi && R.p.lo >= D.p.lo)) {
            unsigned borrow = R.p.lo < D.p.lo;
            R.p.lo -= D.p.lo;
            R.p.hi = R.p.hi - D.p.hi - borrow;
            if (i >= 32) Q.p.hi |= 1u << (i - 32);
            else Q.p.lo |= 1u << i;
        }
    }
    return Q.u;
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b) {
    unsigned long long q = __udivdi3(a, b);
    return a - q * b;
}

/* gcc -m32 складывает «частное и остаток» в один вызов */
unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d, unsigned long long *rp) {
    unsigned long long q = __udivdi3(n, d);
    if (rp) *rp = n - q * d;
    return q;
}

static unsigned long long di_neg(unsigned long long u) { return ~u + 1; }

long long __divdi3(long long a, long long b) {
    di_parts A, B, Q;
    int neg = 0;
    A.u = (unsigned long long)a; B.u = (unsigned long long)b;
    if (A.p.hi & 0x80000000u) { neg ^= 1; A.u = di_neg(A.u); }
    if (B.p.hi & 0x80000000u) { neg ^= 1; B.u = di_neg(B.u); }
    Q.u = __udivdi3(A.u, B.u);
    if (neg) Q.u = di_neg(Q.u);
    return (long long)Q.u;
}

long long __moddi3(long long a, long long b) {
    di_parts A, B, R;
    int neg = 0;
    A.u = (unsigned long long)a; B.u = (unsigned long long)b;
    if (A.p.hi & 0x80000000u) { neg = 1; A.u = di_neg(A.u); }
    if (B.p.hi & 0x80000000u) B.u = di_neg(B.u);
    R.u = __umoddi3(A.u, B.u);
    if (neg) R.u = di_neg(R.u);
    return (long long)R.u;
}

/* Новые gcc зовут эту функцию даже для одного деления signed long long. */
long long __divmoddi4(long long a, long long b, long long *rp) {
    di_parts A, B, Q, R;
    int qneg = 0, rneg = 0;
    A.u = (unsigned long long)a; B.u = (unsigned long long)b;
    if (A.p.hi & 0x80000000u) { qneg ^= 1; rneg = 1; A.u = di_neg(A.u); }
    if (B.p.hi & 0x80000000u) { qneg ^= 1; B.u = di_neg(B.u); }
    Q.u = __udivdi3(A.u, B.u);
    R.u = A.u - Q.u * B.u;
    if (qneg) Q.u = di_neg(Q.u);
    if (rneg) R.u = di_neg(R.u);
    if (rp) *rp = (long long)R.u;
    return (long long)Q.u;
}

/* ================================================================ stdio */

enum { FF_READ = 1, FF_WRITE = 2, FF_EOF = 4, FF_ERR = 8, FF_UNGET = 16 };

static FILE stdfiles[8];
static FILE *stdpool;
static int stdused;
FILE *stdin, *stdout, *stderr;
char *__progname;
char *optarg;
int optind = 1, opterr = 1, optopt;

void __libc_init(void) {
    static int once;
    if (once) return;
    once = 1;
    stdfiles[0].fd = 0; stdfiles[0].flags = FF_READ;
    stdfiles[1].fd = 1; stdfiles[1].flags = FF_WRITE;
    stdfiles[2].fd = 2; stdfiles[2].flags = FF_WRITE;
    stdin = &stdfiles[0];
    stdout = &stdfiles[1];
    stderr = &stdfiles[2];
    stdpool = stdfiles + 3;
    stdused = 0;
}

int __libc_start(int argc, char **argv, char **envp) {
    thread_set_tcb(&main_tcb);
    __libc_init();
    if (envp) {
        int n = 0;
        while (envp[n]) n++;
        char **copy = malloc((n + 1) * sizeof(char *));
        if (copy) {
            for (int i = 0; i < n; i++) copy[i] = strdup(envp[i] ? envp[i] : "");
            copy[n] = NULL;
            environ = copy;
        }
    }
    if (argc > 0 && argv && argv[0]) {
        char *s = argv[0], *b = s;
        while (*s) { if (*s == '/') b = s + 1; s++; }
        __progname = b;
    }
    /* main бывает и main(void), и main(argc, argv, env). Лишние аргументы просто лежат на стеке. */
    int (*entry)(int, char **, char **) = (int (*)(int, char **, char **))main;
    exit(entry(argc, argv, environ));
}

static FILE *file_new(int fd, int flags) {
    __libc_init();
    FILE *f;
    if (stdused < 5) f = &stdpool[stdused++];
    else {
        f = malloc(sizeof *f);
        if (!f) return NULL;
    }
    memset(f, 0, sizeof *f);
    f->fd = fd;
    f->flags = flags;
    return f;
}

static int mode_flags(const char *mode, int *acc) {
    int wr = 0, rd = 0, plus = 0;
    if (!mode || !mode[0]) return -1;
    if (mode[0] == 'r') rd = 1;
    else if (mode[0] == 'w' || mode[0] == 'a') wr = 1;
    else return -1;
    for (const char *p = mode + 1; *p; p++) if (*p == '+') plus = 1;
    if (plus) rd = wr = 1;
    *acc = (rd && wr) ? FF_READ | FF_WRITE : wr ? FF_WRITE : FF_READ;
    if (mode[0] == 'w') return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    if (mode[0] == 'a') return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    return plus ? O_RDWR : O_RDONLY;
}

FILE *fopen(const char *path, const char *mode) {
    int acc, flags = mode_flags(mode, &acc);
    if (flags < 0) return NULL;
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    return file_new(fd, acc);
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    int acc, flags, fd;
    if (!f) return NULL;
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    f->flags = 0;
    if (!path) { errno = EINVAL; return NULL; }
    flags = mode_flags(mode, &acc);
    if (flags < 0) { errno = EINVAL; return NULL; }
    fd = open(path, flags);
    if (fd < 0) return NULL;
    f->fd = fd;
    f->flags = acc;
    return f;
}

FILE *tmpfile(void) {
    char name[L_tmpnam];
    FILE *f;
    if (!tmpnam(name)) return NULL;
    f = fopen(name, "w+");
    return f;
}

char *tmpnam(char *s) {
    static char buf[L_tmpnam];
    static unsigned n;
    char *p = s ? s : buf;
    snprintf(p, L_tmpnam, "/tmp/myos-%u-%u", (unsigned)getpid(), ++n);
    return p;
}

FILE *fdopen(int fd, const char *mode) {
    int acc, flags = mode_flags(mode, &acc);
    if (flags < 0 || fd < 0) return NULL;
    return file_new(fd, acc);
}

static void memstream_sync(FILE *f) {
    if (!f->ms_buf || !f->mem) return;
    if (f->mem_len < 0) f->mem_len = 0;
    if (f->mem_len >= f->mem_cap) {
        int cap = f->mem_len + 1;
        char *grown = realloc(f->mem, (size_t)cap);
        if (!grown) return;
        f->mem = grown;
        f->mem_cap = cap;
    }
    f->mem[f->mem_len] = 0;
    *f->ms_buf = f->mem;
    if (f->ms_size) *f->ms_size = (size_t)f->mem_len;
}

int fclose(FILE *f) {
    int r = 0, pooled;
    if (!f) return EOF;
    if (f->ms_buf) {
        memstream_sync(f);
        f->mem = NULL;
        f->ms_buf = NULL;
    }
    if (f->mem) { free(f->mem); f->mem = NULL; }
    if (f->fd >= 0) r = close(f->fd);
    f->fd = -1;
    pooled = f >= stdfiles && f < stdfiles + (int)(sizeof stdfiles / sizeof stdfiles[0]);
    if (!pooled) free(f);
    return r < 0 ? EOF : 0;
}

FILE *fmemopen(void *buf, size_t size, const char *mode) {
    FILE *f;
    char *m;
    (void)mode;
    if (size > 1024 * 1024) return NULL;
    f = file_new(-1, FF_READ);
    if (!f) return NULL;
    m = malloc(size ? size : 1);
    if (!m) return NULL;
    if (size && buf) memcpy(m, buf, size);
    f->mem = m;
    f->mem_pos = 0;
    f->mem_len = (int)size;
    f->mem_cap = (int)size;
    return f;
}

FILE *open_memstream(char **ptr, size_t *sizeloc) {
    FILE *f;
    char *m;
    if (!ptr || !sizeloc) { errno = EINVAL; return NULL; }
    f = file_new(-1, FF_WRITE);
    if (!f) return NULL;
    m = malloc(64);
    if (!m) { fclose(f); return NULL; }
    m[0] = 0;
    f->mem = m;
    f->mem_pos = 0;
    f->mem_len = 0;
    f->mem_cap = 64;
    f->ms_buf = ptr;
    f->ms_size = sizeloc;
    *ptr = m;
    *sizeloc = 0;
    return f;
}

int fflush(FILE *f) {
    if (f && f->ms_buf) memstream_sync(f);
    return 0;
}
int fileno(FILE *f) { return f ? f->fd : -1; }
int feof(FILE *f) { return f && (f->flags & FF_EOF); }
int ferror(FILE *f) { return f && (f->flags & FF_ERR); }
void clearerr(FILE *f) { if (f) f->flags &= ~(FF_EOF | FF_ERR); }
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }

int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    int r = lseek(f->fd, off, whence);
    if (r < 0) { f->flags |= FF_ERR; return -1; }
    f->flags &= ~(FF_EOF | FF_UNGET);
    return 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    int r = lseek(f->fd, 0, SEEK_CUR);
    return r < 0 ? -1 : r;
}
int fseeko(FILE *f, off_t off, int whence) { return fseek(f, (long)off, whence); }
off_t ftello(FILE *f) { return ftell(f); }

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    __libc_init();
    if (!f || !size || !nmemb) return 0;
    unsigned char *d = ptr;
    int want = (int)(size * nmemb), got = 0;
    if (f->flags & FF_UNGET) {
        d[got++] = (unsigned char)(f->flags >> 16);
        f->flags &= ~FF_UNGET;
    }
    while (got < want) {
        int r;
        if (f->mem) {
            if (f->mem_pos >= f->mem_len) { f->flags |= FF_EOF; break; }
            d[got++] = (unsigned char)f->mem[f->mem_pos++];
            continue;
        }
        r = read(f->fd, d + got, (size_t)(want - got));
        if (r < 0) { f->flags |= FF_ERR; break; }
        if (r == 0) { f->flags |= FF_EOF; break; }
        got += r;
    }
    return (size_t)got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    __libc_init();
    if (!f || !size || !nmemb) return 0;
    int want = (int)(size * nmemb), off = 0;
    const char *s = ptr;
    while (off < want) {
        int r, chunk;
        if (f->mem) {
            chunk = want - off;
            if (f->ms_buf) {
                while (f->mem_pos > f->mem_cap - chunk - 1) {
                    int cap = f->mem_cap > 0 ? f->mem_cap * 2 : 64;
                    char *grown;
                    if (cap < f->mem_pos + chunk + 1) cap = f->mem_pos + chunk + 1;
                    if (cap < f->mem_cap) { f->flags |= FF_ERR; chunk = 0; break; }
                    grown = realloc(f->mem, (size_t)cap);
                    if (!grown) { f->flags |= FF_ERR; chunk = 0; break; }
                    f->mem = grown;
                    f->mem_cap = cap;
                }
            } else if (f->mem_pos >= f->mem_cap) {
                f->flags |= FF_ERR;
                break;
            } else if (chunk > f->mem_cap - f->mem_pos) {
                chunk = f->mem_cap - f->mem_pos;
            }
            if (chunk <= 0) break;
            memcpy(f->mem + f->mem_pos, s + off, (size_t)chunk);
            f->mem_pos += chunk;
            if (f->mem_pos > f->mem_len) f->mem_len = f->mem_pos;
            if (f->ms_buf) memstream_sync(f);
            off += chunk;
            continue;
        }
        r = write(f->fd, s + off, (size_t)(want - off));
        if (r <= 0) { f->flags |= FF_ERR; break; }
        off += r;
    }
    return (size_t)off / size;
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF || (f->flags & FF_UNGET)) return EOF;
    f->flags = (f->flags & 0xFFFF) | FF_UNGET | ((c & 0xFF) << 16);
    f->flags &= ~FF_EOF;
    return c & 0xFF;
}

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) return EOF;
    return c;
}

int fputc(int c, FILE *f) {
    unsigned char b = (unsigned char)c;
    return fwrite(&b, 1, 1, f) == 1 ? b : EOF;
}

char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    if (!s || n <= 0) return NULL;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = 0;
    return s;
}

int fputs(const char *s, FILE *f) {
    int n = (int)strlen(s);
    return fwrite(s, 1, (size_t)n, f) == (size_t)n ? 0 : EOF;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char stack[256];
    va_list ap2;
    __libc_init();
    va_copy(ap2, ap);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    if (n < 0) { va_end(ap2); return -1; }
    if (n < (int)sizeof stack) {
        va_end(ap2);
        return fwrite(stack, 1, (size_t)n, f) == (size_t)n ? n : -1;
    }
    char *b = malloc((size_t)n + 1);
    if (!b) { va_end(ap2); return -1; }
    vsnprintf(b, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    int rc = fwrite(b, 1, (size_t)n, f) == (size_t)n ? n : -1;
    free(b);
    return rc;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) { __libc_init(); return vfprintf(stdout, fmt, ap); }

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *s) {
    __libc_init();
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

ssize_t getline(char **line, size_t *n, FILE *stream) {
    size_t i = 0;
    if (!line || !n) return -1;
    if (!*line || *n < 64) {
        size_t cap = 64;
        char *p = realloc(*line, cap);
        if (!p) return -1;
        *line = p; *n = cap;
    }
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) return i ? (ssize_t)i : -1;
        if (i + 2 > *n) {
            size_t cap = *n * 2;
            char *p = realloc(*line, cap);
            if (!p) return -1;
            *line = p; *n = cap;
        }
        (*line)[i++] = (char)c;
        if (c == '\n') break;
    }
    (*line)[i] = 0;
    return (ssize_t)i;
}

mode_t umask(mode_t mask){return fs_call(9,mask,0);}
int access(const char *path,int mode){return ioerr(fs_call(8,(int)path,mode));}
int chmod(const char *path,mode_t mode){return ioerr(fs_call(4,(int)path,mode));}
int mkfifo(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}
int fchmod(int fd,mode_t mode){return ioerr(fs_call(5,fd,mode));}
int chown(const char *path,uid_t uid,gid_t gid){unsigned ids[2]={uid,gid};return ioerr(fs_call(6,(int)path,(int)ids));}
int fchown(int fd,uid_t uid,gid_t gid){unsigned ids[2]={uid,gid};return ioerr(fs_call(7,fd,(int)ids));}
static void stat_times(struct stat *st) {
    if (!st) return;
    st->st_atim.tv_sec = st->st_atime; st->st_atim.tv_nsec = 0;
    st->st_mtim.tv_sec = st->st_mtime; st->st_mtim.tv_nsec = 0;
    st->st_ctim.tv_sec = st->st_ctime; st->st_ctim.tv_nsec = 0;
}
int fstat(int fd,struct stat *st){int r=ioerr(fs_call(3,fd,(int)st));if(r==0)stat_times(st);return r;}

char *ttyname(int fd) { (void)fd; return (char *)"/dev/tty"; }

/* Ядро не хранит флаги дескриптора. F_DUPFD ищет свободный номер и делает dup2.
 * F_GETFD/F_SETFD пока ничего не запоминают: close-on-exec нет. */
struct adv_lock { int used; unsigned dev, ino; int fd, tid, excl; };
static struct adv_lock adv_locks[32];

static void adv_clear_fd(int fd) {
    for (int i = 0; i < 32; i++)
        if (adv_locks[i].used && adv_locks[i].fd == fd) adv_locks[i].used = 0;
}

static int adv_probe(int fd, int excl) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    for (int i = 0; i < 32; i++) {
        struct adv_lock *l = &adv_locks[i];
        if (!l->used || l->fd == fd || l->dev != st.st_dev || l->ino != st.st_ino) continue;
        if (excl || l->excl) return 1;
    }
    return 0;
}

static int adv_set(int fd, int excl, int block) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    int self = gettid();
    for (;;) {
        int holder = 0, slot = -1, mine = -1;
        for (int i = 0; i < 32; i++) {
            struct adv_lock *l = &adv_locks[i];
            if (!l->used) { if (slot < 0) slot = i; continue; }
            if (l->dev != st.st_dev || l->ino != st.st_ino) continue;
            if (l->fd == fd) { mine = i; continue; }
            if (excl || l->excl) holder = l->tid ? l->tid : -1;
        }
        if (holder) {
            if (holder == self) { errno = EDEADLK; return -1; }
            if (!block) { errno = EWOULDBLOCK; return -1; }
            sleep_ms(10);
            continue;
        }
        if (mine < 0 && slot < 0) { errno = ENOLCK; return -1; }
        struct adv_lock *l = &adv_locks[mine >= 0 ? mine : slot];
        l->used = 1; l->dev = st.st_dev; l->ino = st.st_ino;
        l->fd = fd; l->tid = self; l->excl = excl;
        return 0;
    }
}

int flock(int fd, int op) {
    int block = (op & LOCK_NB) == 0;
    op &= ~LOCK_NB;
    if (op == LOCK_UN) { adv_clear_fd(fd); return 0; }
    if (op != LOCK_SH && op != LOCK_EX) { errno = EINVAL; return -1; }
    return adv_set(fd, op == LOCK_EX, block);
}

int fcntl(int fd, int cmd, ...) {
    struct stat st;
    va_list ap;
    int arg = 0;
    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) {
        struct flock *lk;
        va_start(ap, cmd);
        lk = va_arg(ap, struct flock *);
        va_end(ap);
        if (!lk) { errno = EFAULT; return -1; }
        if (cmd == F_GETLK) {
            int hit = adv_probe(fd, lk->l_type == F_WRLCK);
            if (hit < 0) return -1;
            if (!hit) lk->l_type = F_UNLCK;
            return 0;
        }
        if (lk->l_type == F_UNLCK) { adv_clear_fd(fd); return 0; }
        if (lk->l_type != F_RDLCK && lk->l_type != F_WRLCK) { errno = EINVAL; return -1; }
        return adv_set(fd, lk->l_type == F_WRLCK, cmd == F_SETLKW);
    }
    if (cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD) {
        va_start(ap, cmd);
        arg = va_arg(ap, int);
        va_end(ap);
    }
    if (cmd == F_GETFD || cmd == F_GETFL || cmd == F_SETFD || cmd == F_SETFL) {
#if defined(__LINUX_TEST__)
        return ioerr(__syscall(55, fd, cmd, arg));
#else
        int v[2];
        v[0] = cmd;
        v[1] = arg;
        return ioerr(syscall3(SYS_SSHCTL, 7, fd, (int)v));
#endif
    }
    if (cmd == F_DUPFD) {
        va_list ap;
        int i, min;
        va_start(ap, cmd);
        min = va_arg(ap, int);
        va_end(ap);
        if (min < 0) { errno = EINVAL; return -1; }
        if (fstat(fd, &st) < 0) return -1;
        for (i = min; i < 16; i++) {
            if (fstat(i, &st) == 0) continue;
            if (errno != EBADF) return -1;
            return ioerr(syscall3(SYS_DUP2, fd, i, 0));
        }
        errno = EMFILE;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

int stat(const char *path,struct stat *st){int r=ioerr(fs_call(1,(int)path,(int)st));if(r==0)stat_times(st);return r;}
int lstat(const char *path,struct stat *st){int r=ioerr(fs_call(2,(int)path,(int)st));if(r==0)stat_times(st);return r;}
int rmdir(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return ioerr(unlink(path));
}

int link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    errno = EPERM; /* жёстких ссылок в MyFS нет: вызывающий копирует файл */
    return -1;
}
int symlink(const char *target, const char *linkpath) {
    return ioerr(syscall3(SYS_SYMLINK, (int)target, (int)linkpath, 0));
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    int r = syscall3(SYS_READLINK, (int)path, (int)buf, (int)bufsiz);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int rename(const char *old, const char *new) {
    return ioerr(syscall3(SYS_RENAME, (int)old, (int)new, 0));
}

int remove(const char *path) {
    return ioerr(syscall3(SYS_UNLINK, (int)path, 0, 0));
}

struct DIR {
    char path[1024];
    int index;
    struct dirent ent;
};

DIR *opendir(const char *name) {
    struct dirent_info info;
    DIR *d;
    size_t n;
    int r = syscall3(SYS_STAT, (int)name, (int)&info, 0);
    if (r < 0) { errno = -r; return NULL; }
    if (info.type != 2) { errno = ENOTDIR; return NULL; }
    n = strlen(name);
    if (n == 0 || n >= 1024) { errno = n ? ENAMETOOLONG : ENOENT; return NULL; }
    d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    memcpy(d->path, name, n + 1);
    d->index = 0;
    return d;
}

struct dirent *readdir(DIR *dir) {
    struct dirent_info info;
    int r, i;
    if (!dir) { errno = EBADF; return NULL; }
    r = syscall3(SYS_READDIR, (int)dir->path, dir->index, (int)&info);
    if (r == 0) return NULL;
    if (r < 0) { errno = -r; return NULL; }
    dir->index++;
    dir->ent.d_ino = 0;
    if (info.type == 2) dir->ent.d_type = DT_DIR;
    else if (info.type == 4) dir->ent.d_type = DT_CHR;
    else if (info.type == 3) dir->ent.d_type = DT_BLK;
    else if (info.type == 5) dir->ent.d_type = DT_LNK;
    else dir->ent.d_type = DT_REG;
    for (i = 0; i < 255 && info.name[i]; i++) dir->ent.d_name[i] = info.name[i];
    dir->ent.d_name[i] = 0;
    return &dir->ent;
}

int closedir(DIR *dir) {
    if (!dir) { errno = EBADF; return -1; }
    free(dir);
    return 0;
}

static int glob_add(glob_t *p, const char *path) {
    size_t n = p->gl_pathc;
    char **v = realloc(p->gl_pathv, (n + 2) * sizeof *v);
    char *copy;
    if (!v) return -1;
    p->gl_pathv = v;
    copy = strdup(path);
    if (!copy) return -1;
    v[n] = copy;
    v[n + 1] = NULL;
    p->gl_pathc = n + 1;
    return 0;
}

static int glob_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void globfree(glob_t *pglob) {
    size_t i;
    if (!pglob || !pglob->gl_pathv) return;
    for (i = 0; i < pglob->gl_pathc; i++) free(pglob->gl_pathv[i]);
    free(pglob->gl_pathv);
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob) {
    const char *slash, *base;
    char dirbuf[512], path[512];
    const char *dir;
    DIR *dp;
    struct dirent *ent;
    int matched = 0;
    (void)flags;
    (void)errfunc;
    if (!pglob) return GLOB_NOSPACE;
    pglob->gl_pathc = 0;
    pglob->gl_pathv = NULL;
    pglob->gl_offs = 0;
    if (!pattern || !pattern[0]) return GLOB_NOMATCH;
    if (!strpbrk(pattern, "*?[")) {
        struct stat st;
        if (stat(pattern, &st) != 0) return GLOB_NOMATCH;
        return glob_add(pglob, pattern) < 0 ? GLOB_NOSPACE : 0;
    }
    slash = strrchr(pattern, '/');
    if (!slash) {
        dir = ".";
        base = pattern;
    } else if (slash == pattern) {
        dir = "/";
        base = slash + 1;
    } else {
        int n = (int)(slash - pattern);
        if (n >= (int)sizeof dirbuf) return GLOB_NOSPACE;
        memcpy(dirbuf, pattern, (size_t)n);
        dirbuf[n] = 0;
        dir = dirbuf;
        base = slash + 1;
    }
    if (strpbrk(dir, "*?[")) return GLOB_NOMATCH;
    dp = opendir(dir);
    if (!dp) return GLOB_NOMATCH;
    while ((ent = readdir(dp))) {
        int nd, nn;
        if (ent->d_name[0] == '.' && base[0] != '.') continue;
        if (fnmatch(base, ent->d_name, 0) != 0) continue;
        nd = (int)strlen(dir);
        nn = (int)strlen(ent->d_name);
        if (nd == 1 && dir[0] == '/') {
            if (nn + 2 >= (int)sizeof path) continue;
            path[0] = '/';
            memcpy(path + 1, ent->d_name, (size_t)nn + 1);
        } else {
            if (nd + nn + 2 >= (int)sizeof path) continue;
            memcpy(path, dir, (size_t)nd);
            path[nd] = '/';
            memcpy(path + nd + 1, ent->d_name, (size_t)nn + 1);
        }
        if (glob_add(pglob, path) < 0) {
            closedir(dp);
            globfree(pglob);
            return GLOB_NOSPACE;
        }
        matched = 1;
    }
    closedir(dp);
    if (!matched) return GLOB_NOMATCH;
    qsort(pglob->gl_pathv, pglob->gl_pathc, sizeof(char *), glob_cmp);
    return 0;
}

static char *strip_slashes(char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    return path;
}

char *basename(char *path) {
    char *s;
    if (!path || !*path) return ".";
    strip_slashes(path);
    if (!path[0] || (path[0] == '/' && path[1] == 0)) return path[0] ? path : "/";
    s = path + strlen(path);
    while (s > path && s[-1] != '/') s--;
    return s;
}

char *dirname(char *path) {
    char *s;
    if (!path || !*path) return ".";
    strip_slashes(path);
    if (!*path) return "/";
    s = path + strlen(path);
    while (s > path && s[-1] != '/') s--;
    if (s == path) return ".";
    if (s == path + 1 && path[0] == '/') return "/";
    s[-1] = 0;
    return path;
}

void abort(void) { exit(134); }

int getopt(int argc, char *const argv[], const char *optstring) {
    static int sp = 1;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == 0) return -1;
    if (argv[optind][1] == '-' && argv[optind][2] == 0) { optind++; return -1; }
    char c = argv[optind][sp];
    const char *p = strchr(optstring, c);
    optopt = c;
    if (!p || c == ':') {
        if (opterr && optstring[0] != ':') fprintf(stderr, "%s: illegal option -- %c\n", __progname ? __progname : "", c);
        if (!argv[optind][++sp]) { optind++; sp = 1; }
        return '?';
    }
    if (p[1] == ':') {
        if (argv[optind][sp + 1]) { optarg = &argv[optind][sp + 1]; optind++; sp = 1; }
        else if (optind + 1 < argc) { optarg = argv[++optind]; optind++; sp = 1; }
        else {
            if (opterr && optstring[0] != ':') fprintf(stderr, "%s: option requires an argument -- %c\n", __progname ? __progname : "", c);
            optind++; sp = 1;
            return optstring[0] == ':' ? ':' : '?';
        }
    } else if (!argv[optind][++sp]) { optind++; sp = 1; }
    return (unsigned char)c;
}

static void vmsg(int status, const char *fmt, va_list ap, int with_errno) {
    __libc_init();
    fprintf(stderr, "%s: ", __progname ? __progname : "");
    if (fmt && *fmt) {
        vfprintf(stderr, fmt, ap);
        if (with_errno) fprintf(stderr, ": ");
    }
    if (with_errno) fprintf(stderr, "%s", strerror(errno));
    fprintf(stderr, "\n");
    if (status) exit(status);
}

void warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(0, fmt, ap, 1); va_end(ap); }
void warnx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(0, fmt, ap, 0); va_end(ap); }
void err(int status, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(status ? status : 1, fmt, ap, 1); va_end(ap); exit(1); }
void errx(int status, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vmsg(status ? status : 1, fmt, ap, 0); va_end(ap); exit(1); }

int pledge(const char *promises, const char *execpromises) { (void)promises; (void)execpromises; return 0; }
char *setlocale(int category, const char *locale) {
    (void)category;
    if (locale && locale[0] && strcmp(locale, "C") && strcmp(locale, "POSIX")) return NULL;
    return (char *)"C";
}

/* j_strsignal в уже собранном bash индексирует эту таблицу: без HAVE_STRSIGNAL
 * его strsignal — это макрос sys_siglist[sig]. Ноль значит «Signal N». */
char *sys_siglist[NSIG] = {
    "Unknown signal",
    "Hangup",
    "Interrupt",
    "Quit",
    "Illegal instruction",
    "Trace/breakpoint trap",
    "Aborted",
    "Bus error",
    "Floating point exception",
    "Killed",
    "User signal 1",
    "Segmentation fault",
    "User signal 2",
    "Broken pipe",
    "Alarm clock",
    "Terminated",
    0,
    "Child exited",
    "Continued",
    "Stopped (signal)",
    "Stopped",
    "Stopped (tty input)",
    "Stopped (tty output)",
    0, 0, 0, 0, 0,
    "Window changed",
};

char *strsignal(int sig) {
    if (sig < 0 || sig >= NSIG) return NULL;
    return sys_siglist[sig];
}

/* Уже собранный sig.c зовёт это, когда HAVE_STRSIGNAL ещё не было.
 * Имена сигналов уже лежат в sys_siglist, затирать их нечем. */
void initialize_siglist(void) {}

struct lconv *localeconv(void) {
    static struct lconv lc;
    static int ready;
    if (!ready) {
        memset(&lc, 0, sizeof lc);
        lc.decimal_point = ".";
        lc.thousands_sep = "";
        lc.grouping = "";
        lc.int_curr_symbol = "";
        lc.currency_symbol = "";
        lc.mon_decimal_point = "";
        lc.mon_thousands_sep = "";
        lc.mon_grouping = "";
        lc.positive_sign = "";
        lc.negative_sign = "";
        lc.int_frac_digits = lc.frac_digits = 127;
        lc.p_cs_precedes = lc.p_sep_by_space = lc.n_cs_precedes = lc.n_sep_by_space = 127;
        lc.p_sign_posn = lc.n_sign_posn = 127;
        ready = 1;
    }
    return &lc;
}
int wcscmp(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return (a != NULL) - (b != NULL);
    while (*a && *a == *b) { a++; b++; }
    return *a < *b ? -1 : *a > *b;
}
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    if (!a || !b) return (a != NULL) - (b != NULL);
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return *a < *b ? -1 : *a > *b;
}
int mbtowc(wchar_t *wc, const char *s, size_t n) {
    if (!s) return 0;
    if (!n || !*s) return 0;
    if (wc) *wc = (unsigned char)*s;
    return 1;
}
int iswspace(wchar_t wc) { return isspace((unsigned char)wc); }
int fmt_scaled(long long number, char *result) {
    snprintf(result, FMT_SCALED_STRSIZE, "%lld", number);
    return 0;
}

/* ================================================================ числа, сортировка, POSIX-мелочь */

static int conv_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* lim — наибольшее по модулю значение. Для знакового отрицательного это 2^63. */
static unsigned long long parse_int(const char *nptr, char **end, int base, int *neg, int *ovf) {
    const char *s = nptr;
    while (isspace((unsigned char)*s)) s++;
    *neg = 0;
    if (*s == '+' || *s == '-') { *neg = *s == '-'; s++; }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && conv_digit((unsigned char)s[2]) >= 0 && conv_digit((unsigned char)s[2]) < 16) {
            base = 16; s += 2;
        } else if (s[0] == '0') base = 8;
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && conv_digit((unsigned char)s[2]) >= 0 && conv_digit((unsigned char)s[2]) < 16)
        s += 2;
    *ovf = 0;
    if (base < 2 || base > 36) { errno = EINVAL; if (end) *end = (char *)nptr; return 0; }
    unsigned long long acc = 0;
    int any = 0;
    for (;;) {
        int d = conv_digit((unsigned char)*s);
        if (d < 0 || d >= base) break;
        any = 1;
        if (acc > (~0ULL - (unsigned)d) / (unsigned)base) *ovf = 1;
        else acc = acc * (unsigned)base + (unsigned)d;
        s++;
    }
    if (end) *end = (char *)(any ? s : nptr);
    return any ? acc : 0;
}

long long strtoll(const char *nptr, char **end, int base) {
    int neg, ovf;
    unsigned long long lim, acc = parse_int(nptr, end, base, &neg, &ovf);
    if (errno == EINVAL && end && *end == nptr && acc == 0) return 0;
    lim = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    if (ovf || acc > lim) {
        errno = ERANGE;
        return neg ? (-9223372036854775807LL - 1) : 9223372036854775807LL;
    }
    return neg ? -(long long)acc : (long long)acc;
}

static void big_add_small(Big *b, unsigned a) {
    unsigned c = a;
    int i;
    for (i = 0; i < b->n; i++) {
        unsigned t = b->v[i] + c;
        c = t < b->v[i];
        b->v[i] = t;
        if (!c) return;
    }
    if (c && b->n < BIG_N) b->v[b->n++] = c;
}

/* Знак, inf и nan для strtod. Публичные sin/pow живут в libm, libc его не тянет. */
static double libc_bits(unsigned long long u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}
static unsigned long long libc_ubits(double x) {
    unsigned long long u;
    memcpy(&u, &x, 8);
    return u;
}
static double libc_inf(void) { return libc_bits(0x7ff0000000000000ULL); }
static double libc_nan(void) { return libc_bits(0x7ff8000000000000ULL); }
static double libc_copysign(double x, double y) {
    unsigned long long u = (libc_ubits(x) & ~(1ULL << 63)) | (libc_ubits(y) & (1ULL << 63));
    return libc_bits(u);
}

/* Ближайший double к целому * 2^extra. extra < 0 — это сдвиг вправо всего числа. */
static double big_to_double(Big *b, int sign, int extra) {
    int bits, i, be, roundb, sticky;
    unsigned long long mant, u;
    double d;
    bits = b->n * 32;
    while (bits > 0 && !big_bit(b, bits - 1)) bits--;
    if (bits <= 0) return libc_copysign(0.0, sign < 0 ? -1.0 : 1.0);
    roundb = 0;
    sticky = 0;
    if (bits > 53) {
        roundb = big_bit(b, bits - 54);
        for (i = 0; i < bits - 54; i++) if (big_bit(b, i)) { sticky = 1; break; }
        mant = 0;
        for (i = 0; i < 53; i++) mant = (mant << 1) | (unsigned)big_bit(b, bits - 1 - i);
        if (roundb && (sticky || (mant & 1))) {
            mant++;
            if (mant == (1ULL << 53)) { mant >>= 1; bits++; }
        }
    } else {
        mant = 0;
        for (i = 0; i < bits; i++) mant = (mant << 1) | (unsigned)big_bit(b, bits - 1 - i);
    }
    be = bits - 1 + extra;
    if (be > 1023) { errno = ERANGE; return libc_copysign(libc_inf(), sign < 0 ? -1.0 : 1.0); }
    if (be < -1022) { errno = ERANGE; return libc_copysign(0.0, sign < 0 ? -1.0 : 1.0); }
    u = ((unsigned long long)(be + 1023) << 52) | (mant & ((1ULL << 52) - 1));
    if (sign < 0) u |= 1ULL << 63;
    memcpy(&d, &u, 8);
    return d;
}

static double strtod_precise(const char *nptr, char **end) {
    const char *s = nptr;
    char digs[400];
    int sign = 1, nd = 0, exp = 0, sticky = 0, esign, ev, i, k;
    Big b;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    while (*s == '0') s++;
    while (isdigit((unsigned char)*s)) {
        if (nd < (int)sizeof digs) digs[nd++] = (char)(*s - '0');
        else { exp++; if (*s != '0') sticky = 1; }
        s++;
    }
    if (*s == '.') {
        s++;
        if (!nd) while (*s == '0') { exp--; s++; }
        while (isdigit((unsigned char)*s)) {
            if (nd < (int)sizeof digs) { digs[nd++] = (char)(*s - '0'); exp--; }
            else if (*s != '0') sticky = 1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        const char *e0 = s++;
        esign = 1;
        ev = 0;
        if (*s == '+' || *s == '-') { if (*s == '-') esign = -1; s++; }
        if (!isdigit((unsigned char)*s)) s = e0;
        else {
            while (isdigit((unsigned char)*s)) {
                if (ev < 100000) ev = ev * 10 + (*s - '0');
                s++;
            }
            exp += esign * ev;
        }
    }
    if (end) *end = (char *)s;
    while (nd > 0 && digs[nd - 1] == 0) { nd--; exp++; }
    if (!nd) return libc_copysign(0.0, sign < 0 ? -1.0 : 1.0);
    big_set(&b, 0);
    for (i = 0; i < nd; i++) {
        big_mul(&b, 10);
        big_add_small(&b, (unsigned)digs[i]);
    }
    if (sticky) big_add_small(&b, 1);
    if (exp >= 0) {
        for (k = 0; k < exp; k++) big_mul(&b, 5);
        return big_to_double(&b, sign, exp);
    }
    k = -exp;
    for (i = 0; i < k; i++) big_mul(&b, 5);
    return big_to_double(&b, sign, -k);
}

double strtod(const char *nptr, char **end) {
    const char *s = nptr;
    int sign = 1, exp = 0, nd = 0, saw = 0, esign, ev, trunc = 0;
    unsigned long long acc = 0;
    double x;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') && (s[2] == 'f' || s[2] == 'F')) {
        s += 3;
        if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') && (s[2] == 'i' || s[2] == 'I') &&
            (s[3] == 't' || s[3] == 'T') && (s[4] == 'y' || s[4] == 'Y')) s += 5;
        if (end) *end = (char *)s;
        return libc_copysign(libc_inf(), sign < 0 ? -1.0 : 1.0);
    }
    if ((s[0] == 'n' || s[0] == 'N') && (s[1] == 'a' || s[1] == 'A') && (s[2] == 'n' || s[2] == 'N')) {
        s += 3;
        if (end) *end = (char *)s;
        return libc_nan();
    }
    while (isdigit((unsigned char)*s)) {
        saw = 1;
        if (nd < 15) { acc = acc * 10 + (unsigned)(*s - '0'); nd++; }
        else { exp++; trunc = 1; }
        s++;
    }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            saw = 1;
            if (nd < 15) { acc = acc * 10 + (unsigned)(*s - '0'); nd++; exp--; }
            else trunc = 1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        const char *e0 = s++;
        esign = 1;
        ev = 0;
        if (*s == '+' || *s == '-') { if (*s == '-') esign = -1; s++; }
        if (!isdigit((unsigned char)*s)) s = e0;
        else {
            while (isdigit((unsigned char)*s)) {
                if (ev < 100000) ev = ev * 10 + (*s - '0');
                s++;
            }
            exp += esign * ev;
        }
    }
    if (!saw) { if (end) *end = (char *)nptr; return 0; }
    if (trunc) return strtod_precise(nptr, end);
    if (end) *end = (char *)s;
    if (exp > 350) { errno = ERANGE; return libc_copysign(libc_inf(), sign < 0 ? -1.0 : 1.0); }
    if (exp < -350) { errno = ERANGE; return libc_copysign(0.0, sign < 0 ? -1.0 : 1.0); }
    x = (double)acc;
    if (exp > 0) x *= dpow10(exp);
    else if (exp < 0) x /= dpow10(-exp);
    return sign < 0 ? -x : x;
}

float strtof(const char *nptr, char **end) { return (float)strtod(nptr, end); }
long double strtold(const char *nptr, char **end) { return (long double)strtod(nptr, end); }

long strtol(const char *nptr, char **end, int base) {
    int saved = errno;
    errno = 0;
    long long v = strtoll(nptr, end, base);
    if (errno == EINVAL) return 0;
    if (errno == ERANGE || v > 2147483647LL || v < -2147483647LL - 1) {
        errno = ERANGE;
        return v < 0 ? (long)(-2147483647L - 1) : 2147483647L;
    }
    errno = saved;
    return (long)v;
}

unsigned long long strtoull(const char *nptr, char **end, int base) {
    int neg, ovf;
    unsigned long long acc = parse_int(nptr, end, base, &neg, &ovf);
    if (ovf) { errno = ERANGE; return ~0ULL; }
    if (neg) acc = -acc;
    return acc;
}

intmax_t strtoimax(const char *nptr, char **end, int base) {
    return (intmax_t)strtoll(nptr, end, base);
}

uintmax_t strtoumax(const char *nptr, char **end, int base) {
    return (uintmax_t)strtoull(nptr, end, base);
}

unsigned long strtoul(const char *nptr, char **end, int base) {
    int neg, ovf, saved = errno;
    unsigned long long acc;
    errno = 0;
    acc = parse_int(nptr, end, base, &neg, &ovf);
    if (errno == EINVAL) return 0;
    if (ovf || acc > 4294967295ULL) { errno = ERANGE; return 4294967295UL; }
    errno = saved;
    if (neg) return (unsigned long)(0UL - (unsigned long)acc);
    return (unsigned long)acc;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t i = 0;
    char *p;
    while (i < n && s[i]) i++;
    p = malloc(i + 1);
    if (!p) return NULL;
    memcpy(p, s, i);
    p[i] = 0;
    return p;
}

static int fold_ascii(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch; }

size_t strlcpy(char *dst, const char *src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t c = len >= n ? n - 1 : len;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return len;
}
size_t strlcat(char *dst, const char *src, size_t n) {
    size_t d = 0;
    while (d < n && dst[d]) d++;
    if (d == n) return d + strlen(src);
    return d + strlcpy(dst + d, src, n - d);
}
void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {
    const unsigned char *h = haystack, *nd = needle;
    size_t i;
    if (!needlelen) return (void *)haystack;
    if (needlelen > haystacklen || !haystack || !needle) return NULL;
    for (i = 0; i + needlelen <= haystacklen; i++)
        if (!memcmp(h + i, nd, needlelen)) return (void *)(h + i);
    return NULL;
}
int strcasecmp(const char *a, const char *b) {
    for (;;) {
        int ca = fold_ascii((unsigned char)*a), cb = fold_ascii((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
        a++; b++;
    }
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        int ca = fold_ascii((unsigned char)*a), cb = fold_ascii((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
        a++; b++;
    }
    return 0;
}

char *strcasestr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h;
    return NULL;
}

long long strtonum(const char *nptr, long long minval, long long maxval, const char **errstr) {
    char *ep;
    int saved = errno;
    errno = 0;
    long long v = strtoll(nptr, &ep, 10);
    if (errstr) *errstr = NULL;
    if (nptr == ep || *ep) {
        errno = EINVAL;
        if (errstr) *errstr = "invalid";
        return 0;
    }
    if (errno == ERANGE || v < minval || v > maxval) {
        errno = ERANGE;
        if (errstr) *errstr = v < minval ? "too small" : "too large";
        return 0;
    }
    errno = saved;
    return v;
}

static void memswap(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void sift_down(char *b, size_t root, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    while (root * 2 + 1 < count) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && cmp(b + child * size, b + (child + 1) * size) < 0) child++;
        if (cmp(b + root * size, b + child * size) >= 0) return;
        memswap(b + root * size, b + child * size, size);
        root = child;
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *b = base;
    size_t start, end;
    if (n < 2 || !size || !cmp) return;
    for (start = n / 2; start > 0; start--) sift_down(b, start - 1, n, size, cmp);
    for (end = n; end > 1; end--) {
        memswap(b, b + (end - 1) * size, size);
        sift_down(b, 0, end - 1, size, cmp);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    const char *b = base;
    size_t lo = 0, hi = n;
    if (!base || !size || !cmp) return 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = b + mid * size;
        int c = cmp(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

/* gcc вызывает это вместо машинной инструкции popcnt, которой на i386 нет. */
unsigned __popcountdi2(unsigned long long x) {
    unsigned n = 0;
    while (x) { n += (unsigned)(x & 1u); x >>= 1; }
    return n;
}

/* gcc -m32 для __builtin_ctzll зовёт это имя: bsf умеет только 32 бита.
 * Считаем по половинам, иначе -Oz узнает цикл и вызовет __ctzdi2 из него же. */
unsigned __ctzdi2(unsigned long long x) {
    di_parts v;
    unsigned n, w;
    v.u = x;
    if (!v.p.lo) {
        if (!v.p.hi) return 64;
        w = v.p.hi;
        n = 32;
    } else {
        w = v.p.lo;
        n = 0;
    }
    while ((w & 1u) == 0) { n++; w >>= 1; }
    return n;
}

static char *empty_environ[] = { NULL };
char **environ = empty_environ;

char *getenv(const char *name) {
    size_t n;
    char **e;
    if (!name || !environ) return NULL;
    n = strlen(name);
    for (e = environ; *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

static int env_count(void) {
    int n = 0;
    if (environ) while (environ[n]) n++;
    return n;
}

static int env_grow(int n) {
    char **v = malloc((n + 2) * sizeof(char *));
    if (!v) return -1;
    for (int i = 0; i < n; i++) v[i] = environ[i];
    v[n] = NULL;
    v[n + 1] = NULL;
    if (environ != empty_environ) free(environ);
    environ = v;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t nlen, vlen;
    int n, i;
    char *s;
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!value) value = "";
    nlen = strlen(name);
    vlen = strlen(value);
    n = env_count();
    for (i = 0; i < n; i++) {
        if (!strncmp(environ[i], name, nlen) && environ[i][nlen] == '=') {
            if (!overwrite) return 0;
            s = malloc(nlen + 1 + vlen + 1);
            if (!s) return -1;
            memcpy(s, name, nlen);
            s[nlen] = '=';
            memcpy(s + nlen + 1, value, vlen + 1);
            free(environ[i]);
            environ[i] = s;
            return 0;
        }
    }
    s = malloc(nlen + 1 + vlen + 1);
    if (!s || env_grow(n)) { free(s); return -1; }
    memcpy(s, name, nlen);
    s[nlen] = '=';
    memcpy(s + nlen + 1, value, vlen + 1);
    environ[n] = s;
    environ[n + 1] = NULL;
    return 0;
}

int putenv(char *string) {
    char *eq;
    int n, i, nlen;
    if (!string || !(eq = strchr(string, '=')) || eq == string) { errno = EINVAL; return -1; }
    nlen = (int)(eq - string);
    n = env_count();
    for (i = 0; i < n; i++) {
        if (!strncmp(environ[i], string, nlen) && environ[i][nlen] == '=') {
            free(environ[i]);
            environ[i] = string;
            return 0;
        }
    }
    if (env_grow(n)) return -1;
    environ[n] = string;
    environ[n + 1] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    int n, i, j;
    size_t nlen;
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    nlen = strlen(name);
    n = env_count();
    for (i = 0; i < n; i++) {
        if (!strncmp(environ[i], name, nlen) && environ[i][nlen] == '=') {
            free(environ[i]);
            for (j = i; j < n; j++) environ[j] = environ[j + 1];
            return 0;
        }
    }
    return 0;
}

int clearenv(void) {
    int n = env_count(), i;
    for (i = 0; i < n; i++) free(environ[i]);
    if (environ != empty_environ) free(environ);
    environ = empty_environ;
    return 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)f; (void)buf; (void)mode; (void)size;
    return 0;
}
void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

static int scan_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int vsscanf(const char *s, const char *fmt, va_list ap) {
    const char *start = s;
    int assigned = 0;
    while (*fmt) {
        int suppress, width, lcount;
        char conv, buf[128];
        const char *num;
        int i;
        if (scan_isspace((unsigned char)*fmt)) {
            while (scan_isspace((unsigned char)*fmt)) fmt++;
            while (scan_isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++;
        suppress = 0; width = 0; lcount = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') { lcount = 1; fmt++; if (*fmt == 'l') { lcount = 2; fmt++; } }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        conv = *fmt ? *fmt++ : 0;
        if (conv == '%') { if (*s != '%') break; s++; continue; }
        if (conv == 'n') {
            if (!suppress) { *va_arg(ap, int *) = (int)(s - start); assigned++; }
            continue;
        }
        if (conv != 'c') while (scan_isspace((unsigned char)*s)) s++;
        if (!*s && conv != 'n') break;
        if (conv == 'c' || conv == 's') {
            char *out = suppress ? 0 : va_arg(ap, char *);
            int max = width ? width : (conv == 'c' ? 1 : 1000000);
            i = 0;
            while (i < max && *s && (conv == 'c' || !scan_isspace((unsigned char)*s))) {
                if (out) out[i] = *s;
                s++; i++;
            }
            if (i == 0) break;
            if (out && conv == 's') out[i] = 0;
            if (!suppress) assigned++;
            continue;
        }
        if (conv != 'd' && conv != 'i' && conv != 'u' && conv != 'o' && conv != 'x' && conv != 'X')
            break;
        num = s;
        i = 0;
        if ((*s == '+' || *s == '-') && i < 126) buf[i++] = *s++;
        while (*s && i < 127 && (!width || (int)(s - num) < width)) {
            int hex = conv == 'x' || conv == 'X' || conv == 'i';
            if (*s >= '0' && *s <= '9') buf[i++] = *s++;
            else if (hex && ((*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) buf[i++] = *s++;
            else if (hex && i > 0 && (buf[i - 1] == '0') && (*s == 'x' || *s == 'X')) buf[i++] = *s++;
            else break;
        }
        if (i == 0 || (i == 1 && (buf[0] == '+' || buf[0] == '-'))) break;
        buf[i] = 0;
        if (!suppress) {
            int base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : conv == 'i' ? 0 : 10;
            if (conv == 'd' || conv == 'i') {
                if (lcount >= 2) *va_arg(ap, long long *) = strtoll(buf, 0, base);
                else if (lcount == 1) *va_arg(ap, long *) = strtol(buf, 0, base);
                else *va_arg(ap, int *) = (int)strtol(buf, 0, base);
            } else {
                if (lcount >= 2) *va_arg(ap, unsigned long long *) = strtoull(buf, 0, base);
                else if (lcount == 1) *va_arg(ap, unsigned long *) = strtoul(buf, 0, base);
                else *va_arg(ap, unsigned *) = (unsigned)strtoul(buf, 0, base);
            }
            assigned++;
        }
    }
    (void)start;
    return assigned;
}
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}
int fscanf(FILE *f, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    int n;
    if (!fgets(buf, (int)sizeof buf, f)) return EOF;
    va_start(ap, fmt);
    n = vsscanf(buf, fmt, ap);
    va_end(ap);
    return n;
}
int scanf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    int n;
    if (!fgets(buf, (int)sizeof buf, stdin)) return EOF;
    va_start(ap, fmt);
    n = vsscanf(buf, fmt, ap);
    va_end(ap);
    return n;
}
int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    (void)which; (void)new_value;
    if (old_value) memset(old_value, 0, sizeof *old_value);
    errno = ENOSYS;
    return -1;
}
int getitimer(int which, struct itimerval *curr_value) {
    (void)which;
    if (curr_value) memset(curr_value, 0, sizeof *curr_value);
    errno = ENOSYS;
    return -1;
}

int isatty(int fd) {
#if defined(__LINUX_TEST__)
    struct { unsigned short row, col, xpixel, ypixel; } ws;
    return __syscall(54, fd, 0x5413, (int)&ws) == 0;
#else
    int mode[2]={0,0};int ret=syscall3(SYS_SSHCTL,11,fd,(int)mode);
    if(ret<0){errno=-ret;return 0;}return 1;
#endif
}
pid_t tcgetpgrp(int fd) {
    int g;
    if (!isatty(fd)) { errno = ENOTTY; return -1; }
#if defined(__LINUX_TEST__)
    g = __syscall(147, fd);
    return g < 0 ? ioerr(g) : g;
#else
    g = ttyctl(fd, TTYCTL_GETFG, 0);
    return g < 0 ? ioerr(g) : g;
#endif
}
int tcsetpgrp(int fd, pid_t pgrp) {
    if (!isatty(fd)) { errno = ENOTTY; return -1; }
    (void)pgrp;
    return 0;
}

unsigned sleep(unsigned seconds) {
#if defined(__LINUX_TEST__)
    struct { long sec, nsec; } req, rem;
    req.sec = (long)seconds; req.nsec = 0; rem.sec = 0; rem.nsec = 0;
    if (__syscall(162, (int)&req, (int)&rem) < 0) return seconds;
    return (unsigned)rem.sec;
#else
    while (seconds) {
        unsigned chunk = seconds > 1000u ? 1000u : seconds;
        sleep_ms(chunk * 1000u);
        seconds -= chunk;
    }
    return 0;
#endif
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
#if defined(__LINUX_TEST__)
    struct { long sec, nsec; } r, m;
    if (!req) return -1;
    r.sec = (long)req->tv_sec; r.nsec = req->tv_nsec; m.sec = 0; m.nsec = 0;
    if (__syscall(162, (int)&r, (int)&m) < 0) return -1;
    if (rem) { rem->tv_sec = m.sec; rem->tv_nsec = m.nsec; }
    return 0;
#else
    unsigned ms;
    (void)rem;
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0) return -1;
    ms = (unsigned)req->tv_sec * 1000u + (unsigned)(req->tv_nsec / 1000000L);
    sleep_ms(ms);
    return 0;
#endif
}

long syscall(long n, ...) {
    long a, b, c, d, e;
    va_list ap;
    va_start(ap, n);
    a = va_arg(ap, long); b = va_arg(ap, long); c = va_arg(ap, long);
    d = va_arg(ap, long); e = va_arg(ap, long);
    va_end(ap);
#if defined(__LINUX_TEST__)
    return __syscall((int)n, (int)a, (int)b, (int)c, (int)d, (int)e);
#else
    (void)n; (void)a; (void)b; (void)c; (void)d; (void)e;
    return -38;
#endif
}

int poll(struct pollfd *fds, unsigned nfds, int timeout) {
#if defined(__LINUX_TEST__)
    return __syscall(168, (int)fds, (int)nfds, timeout);
#else
    unsigned start = ticks();
    for (;;) {
        int r = syscall3(SYS_SSHCTL, 1, (int)nfds, (int)fds);
        if (r < 0) { errno = -r; return -1; }
        if (r || timeout == 0) return r;
        if (timeout > 0 && ticks() - start >= (unsigned)timeout) return 0;
        sleep_ms(10);
    }
#endif
}

static unsigned identity_get(int group,int effective) {
#if defined(__LINUX_TEST__)
    return __syscall(group?(effective?202:200):(effective?201:199),0,0,0);
#else
    int ids[3];if(syscall3(SYS_SSHCTL,group?12:8,0,(int)ids)<0)return (unsigned)-1;return ids[effective];
#endif
}
uid_t getuid(void){return identity_get(0,0);}
uid_t geteuid(void){return identity_get(0,1);}
gid_t getgid(void){return identity_get(1,0);}
gid_t getegid(void){return identity_get(1,1);}
unsigned alarm(unsigned seconds){(void)seconds;return 0;}
int setuid(uid_t uid) {
    if(uid==(uid_t)-1){errno=EINVAL;return -1;}
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(213,uid,0,0));
#else
    int ids[3]={uid,uid,uid};return ioerr(syscall3(SYS_SSHCTL,8,1,(int)ids));
#endif
}
int setgid(gid_t gid) {
    if(gid==(gid_t)-1){errno=EINVAL;return -1;}
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(214,gid,0,0));
#else
    int ids[3]={gid,gid,gid};return ioerr(syscall3(SYS_SSHCTL,12,1,(int)ids));
#endif
}
struct passwd *getpwnam(const char *name) {
    static struct passwd p;static char user[24],home[40];int id=account(8,name,NULL);
    if(id<0){errno=ENOENT;return NULL;}strcpy(user,name);
    snprintf(home,sizeof(home),id?"/home/%s":"/root",name);
    p.pw_name=user;p.pw_passwd="*";p.pw_uid=p.pw_gid=id;p.pw_gecos=user;p.pw_dir=home;p.pw_shell="/bin/sh";return &p;
}
struct passwd *getpwuid(uid_t uid) {
    char name[24];for(unsigned i=0;account_list(name,i)==0;i++)if(account(8,name,NULL)==(int)uid)return getpwnam(name);
    errno=ENOENT;return NULL;
}
void endpwent(void){}

static int leap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

time_t time(time_t *tloc) {
    time_t t;
#if defined(__LINUX_TEST__)
    t = (time_t)__syscall(13, 0);
#else
    static const unsigned md[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    struct datetime d;
    unsigned y;
    long long days;
    if (datetime_get(&d) < 0 || d.year < 1970 || d.month < 1 || d.month > 12) return (time_t)-1;
    days = 0;
    for (y = 1970; y < d.year; y++) days += leap(y) ? 366 : 365;
    days += md[d.month - 1];
    if (d.month > 2 && leap(d.year)) days++;
    days += (long)d.day - 1;
    days = days * 86400LL + (long long)d.hour * 3600 + (long long)d.min * 60 + d.sec;
    if (days < 0 || days > 2147483647LL) return (time_t)-1;
    t = (time_t)days;
#endif
    if (tloc && t != (time_t)-1) *tloc = t;
    return t;
}

int gettimeofday(struct timeval *tv, void *tz) {
    time_t t;
    (void)tz;
    t = time(NULL);
    if (t == (time_t)-1) return -1;
    if (tv) { tv->tv_sec = t; tv->tv_usec = 0; }
    return 0;
}

int clock_gettime(int clock_id, struct timespec *ts) {
    struct timeval tv;
    (void)clock_id;
    if (!ts || gettimeofday(&tv, 0) < 0) return -1;
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000L;
    return 0;
}

int ioctl(int fd, unsigned long request, ...) {
    if (request == TIOCGWINSZ) {
        struct winsize *ws;
        va_list ap;
        va_start(ap, request);
        ws = va_arg(ap, struct winsize *);
        va_end(ap);
        if (!ws) { errno = EFAULT; return -1; }
        memset(ws, 0, sizeof *ws);
        ws->ws_col = (unsigned short)term_cols();
        ws->ws_row = (unsigned short)term_rows();
        (void)fd;
        return 0;
    }
    (void)fd;
    errno = EINVAL;
    return -1;
}

char *mktemp(char *template) {
    char *p;
    unsigned n;
    if (!template) { errno = EINVAL; return NULL; }
    p = template + strlen(template);
    n = (unsigned)getpid();
    while (p > template && p[-1] == 'X') {
        *--p = "0123456789abcdef"[n & 15];
        n >>= 4;
        if (!n) n = (unsigned)getpid() + 1;
    }
    return template;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

clock_t clock(void) {
#if defined(__LINUX_TEST__)
    struct { long sec, nsec; } ts;
    if (__syscall(265, 1, (int)&ts) < 0) return (clock_t)-1;
    return (clock_t)(ts.sec * 1000 + ts.nsec / 1000000);
#else
    return (clock_t)ticks();
#endif
}

static long long days_from_civil(int y, unsigned m, unsigned d) {
    long long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long long z, int *y, unsigned *m, unsigned *d) {
    long long era;
    unsigned doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + (int)era * 400 + (*m <= 2);
}

static int yday_of(int y, unsigned m, unsigned d) {
    static const unsigned md[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int n = (int)md[m - 1] + (int)d - 1;
    if (m > 2 && leap((unsigned)y)) n++;
    return n;
}

struct tm *gmtime_r(const time_t *tp, struct tm *out) {
    long long secs, days;
    int rem, y;
    unsigned m, d;
    if (!tp || !out) return NULL;
    secs = *tp;
    days = secs / 86400;
    rem = (int)(secs % 86400);
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = rem / 3600;
    rem %= 3600;
    out->tm_min = rem / 60;
    out->tm_sec = rem % 60;
    civil_from_days(days, &y, &m, &d);
    out->tm_year = y - 1900;
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_yday = yday_of(y, m, d);
    out->tm_wday = (int)((days % 7 + 7 + 4) % 7);
    out->tm_isdst = 0;
    return out;
}

struct tm *localtime_r(const time_t *tp, struct tm *out) { return gmtime_r(tp, out); }

char *ctime_r(const time_t *tp, char *buf) {
    struct tm tm;
    if (!tp || !buf) { errno = EINVAL; return NULL; }
    if (!localtime_r(tp, &tm)) return NULL;
    if (strftime(buf, 26, "%a %b %e %H:%M:%S %Y\n", &tm) == 0) return NULL;
    return buf;
}

struct tm *gmtime(const time_t *tp) {
    static struct tm buf;
    return gmtime_r(tp, &buf);
}

struct tm *localtime(const time_t *tp) { return gmtime(tp); }

static int norm_field(int *field, int *carry, int base) {
    int v = *field;
    if (v >= base || v < 0) {
        int q = v / base;
        int r = v % base;
        if (r < 0) { r += base; q--; }
        *field = r;
        *carry += q;
    }
    return 0;
}

time_t mktime(struct tm *tm) {
    int sec, min, hour, day, mon, year, mdays;
    long long days, secs;
    static const int mlen[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (!tm) { errno = EINVAL; return (time_t)-1; }
    sec = tm->tm_sec;
    min = tm->tm_min;
    hour = tm->tm_hour;
    day = tm->tm_mday;
    mon = tm->tm_mon;
    year = tm->tm_year + 1900;
    norm_field(&sec, &min, 60);
    norm_field(&min, &hour, 60);
    norm_field(&hour, &day, 24);
    mon += 1;
    while (mon > 12) { mon -= 12; year++; }
    while (mon < 1) { mon += 12; year--; }
    for (;;) {
        mdays = mlen[mon - 1] + (mon == 2 && leap((unsigned)year));
        if (day > mdays) { day -= mdays; mon++; if (mon > 12) { mon = 1; year++; } }
        else if (day < 1) {
            mon--;
            if (mon < 1) { mon = 12; year--; }
            day += mlen[mon - 1] + (mon == 2 && leap((unsigned)year));
        } else break;
        if (year < 1 || year > 9999) { errno = ERANGE; return (time_t)-1; }
    }
    days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    secs = days * 86400LL + hour * 3600LL + min * 60LL + sec;
    if (secs < -2147483647LL - 1 || secs > 2147483647LL) { errno = ERANGE; return (time_t)-1; }
    tm->tm_sec = sec;
    tm->tm_min = min;
    tm->tm_hour = hour;
    tm->tm_mday = day;
    tm->tm_mon = mon - 1;
    tm->tm_year = year - 1900;
    tm->tm_yday = yday_of(year, (unsigned)mon, (unsigned)day);
    tm->tm_wday = (int)((days % 7 + 7 + 4) % 7);
    tm->tm_isdst = 0;
    return (time_t)secs;
}

static int fmt_put(char *s, int n, int max, const char *t) {
    while (*t) {
        if (n + 1 >= max) return -1;
        s[n++] = *t++;
    }
    return n;
}

static int fmt_num(char *s, int n, int max, int width, int v) {
    char tmp[16];
    int i = 0, neg = v < 0;
    if (neg) v = -v;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) { if (n + 1 >= max) return -1; s[n++] = '-'; }
    while (width > i) { if (n + 1 >= max) return -1; s[n++] = '0'; width--; }
    while (i) { if (n + 1 >= max) return -1; s[n++] = tmp[--i]; }
    return n;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    static const char *wshort[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *wlong[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mshort[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *mlong[] = { "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December" };
    int n = 0, w, mon, y;
    char piece[64];
    if (!s || !fmt || !tm || max == 0) return 0;
    w = tm->tm_wday;
    mon = tm->tm_mon;
    y = tm->tm_year + 1900;
    if (w < 0 || w > 6) w = 0;
    if (mon < 0 || mon > 11) mon = 0;
    for (; *fmt; fmt++) {
        const char *txt = NULL;
        if (*fmt != '%') {
            if (n + 1 >= (int)max) return 0;
            s[n++] = *fmt;
            continue;
        }
        fmt++;
        if (*fmt == 'E' || *fmt == 'O') fmt++;
        if (!*fmt) break;
        switch (*fmt) {
        case 'a': txt = wshort[w]; break;
        case 'A': txt = wlong[w]; break;
        case 'b': case 'h': txt = mshort[mon]; break;
        case 'B': txt = mlong[mon]; break;
        case 'c':
            snprintf(piece, sizeof piece, "%s %s %2d %02d:%02d:%02d %04d",
                     wshort[w], mshort[mon], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, y);
            txt = piece;
            break;
        case 'C': n = fmt_num(s, n, (int)max, 2, y / 100); if (n < 0) return 0; continue;
        case 'd': n = fmt_num(s, n, (int)max, 2, tm->tm_mday); if (n < 0) return 0; continue;
        case 'D':
            snprintf(piece, sizeof piece, "%02d/%02d/%02d", mon + 1, tm->tm_mday, y % 100);
            txt = piece;
            break;
        case 'e':
            snprintf(piece, sizeof piece, "%2d", tm->tm_mday);
            txt = piece;
            break;
        case 'F':
            snprintf(piece, sizeof piece, "%04d-%02d-%02d", y, mon + 1, tm->tm_mday);
            txt = piece;
            break;
        case 'H': n = fmt_num(s, n, (int)max, 2, tm->tm_hour); if (n < 0) return 0; continue;
        case 'I': {
            int h = tm->tm_hour % 12;
            if (!h) h = 12;
            n = fmt_num(s, n, (int)max, 2, h);
            if (n < 0) return 0;
            continue;
        }
        case 'j': n = fmt_num(s, n, (int)max, 3, tm->tm_yday + 1); if (n < 0) return 0; continue;
        case 'm': n = fmt_num(s, n, (int)max, 2, mon + 1); if (n < 0) return 0; continue;
        case 'M': n = fmt_num(s, n, (int)max, 2, tm->tm_min); if (n < 0) return 0; continue;
        case 'n': txt = "\n"; break;
        case 'p': txt = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'R':
            snprintf(piece, sizeof piece, "%02d:%02d", tm->tm_hour, tm->tm_min);
            txt = piece;
            break;
        case 'S': n = fmt_num(s, n, (int)max, 2, tm->tm_sec); if (n < 0) return 0; continue;
        case 't': txt = "\t"; break;
        case 'T': case 'X':
            snprintf(piece, sizeof piece, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            txt = piece;
            break;
        case 'u': n = fmt_num(s, n, (int)max, 1, w == 0 ? 7 : w); if (n < 0) return 0; continue;
        case 'U': n = fmt_num(s, n, (int)max, 2, (tm->tm_yday + 7 - w) / 7); if (n < 0) return 0; continue;
        case 'w': n = fmt_num(s, n, (int)max, 1, w); if (n < 0) return 0; continue;
        case 'W': n = fmt_num(s, n, (int)max, 2, (tm->tm_yday + 7 - (w == 0 ? 6 : w - 1)) / 7); if (n < 0) return 0; continue;
        case 'x':
            snprintf(piece, sizeof piece, "%02d/%02d/%02d", mon + 1, tm->tm_mday, y % 100);
            txt = piece;
            break;
        case 'y': n = fmt_num(s, n, (int)max, 2, y % 100); if (n < 0) return 0; continue;
        case 'Y': n = fmt_num(s, n, (int)max, 4, y); if (n < 0) return 0; continue;
        case 'z': txt = "+0000"; break;
        case 'Z': txt = "UTC"; break;
        case '%': txt = "%"; break;
        default: txt = ""; break;
        }
        if (txt) {
            n = fmt_put(s, n, (int)max, txt);
            if (n < 0) return 0;
        }
    }
    if (n >= (int)max) return 0;
    s[n] = 0;
    return (size_t)n;
}

static void (*sig_handlers[32])(int);

static sigset_t proc_sigmask;
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (oldset) *oldset = proc_sigmask;
    if (!set) return 0;
    if (how == SIG_BLOCK) proc_sigmask |= *set;
    else if (how == SIG_UNBLOCK) proc_sigmask &= ~*set;
    else if (how == SIG_SETMASK) proc_sigmask = *set;
    else { errno = EINVAL; return -1; }
    return 0;
}
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    int r = sigprocmask(how, set, oldset);
    return r < 0 ? errno : 0;
}
int sigblock(int mask) { (void)mask; return 0; }
int sigsetmask(int mask) { (void)mask; return 0; }
int siginterrupt(int sig, int flag) { (void)sig; (void)flag; return 0; }

void (*signal(int sig, void (*handler)(int)))(int) {
    void (*old)(int);
    if (sig < 0 || sig >= 32) { errno = EINVAL; return SIG_ERR; }
    old = sig_handlers[sig];
    sig_handlers[sig] = handler;
    return old;
}

int sigemptyset(sigset_t *set) { if (!set) { errno = EINVAL; return -1; } *set = 0; return 0; }
int sigfillset(sigset_t *set) { if (!set) { errno = EINVAL; return -1; } *set = ~0; return 0; }
int sigaddset(sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= 32) { errno = EINVAL; return -1; }
    *set |= 1 << (sig - 1); return 0;
}
int sigdelset(sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= 32) { errno = EINVAL; return -1; }
    *set &= ~(1 << (sig - 1)); return 0;
}
int sigismember(const sigset_t *set, int sig) {
    if (!set || sig <= 0 || sig >= 32) { errno = EINVAL; return -1; }
    return (*set & (1 << (sig - 1))) != 0;
}
int raise(int sig) {
    void (*handler)(int);
    if (sig <= 0 || sig >= 32) { errno = EINVAL; return -1; }
    handler = sig_handlers[sig];
    if (handler == SIG_IGN) return 0;
    if (handler && handler != SIG_DFL) { handler(sig); return 0; }
    exit(128 + sig);
}
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig <= 0 || sig >= 32 || sig == SIGKILL || sig == SIGSTOP) { errno = EINVAL; return -1; }
    if (old) { memset(old, 0, sizeof *old); old->sa_handler = sig_handlers[sig]; }
    if (act) sig_handlers[sig] = act->sa_handler;
    return 0;
}

int uname(struct utsname *name) {
    if (!name) { errno = EFAULT; return -1; }
    memset(name, 0, sizeof *name);
    memcpy(name->sysname, "MyOS", 5);
    memcpy(name->nodename, "myos", 5);
    memcpy(name->release, "0.1", 4);
    memcpy(name->version, "MyOS 0.1", 9);
    memcpy(name->machine, "i386", 5);
    return 0;
}
int gethostname(char *name, size_t len) {
    struct utsname u;
    size_t n;
    if (!name || !len) { errno = EINVAL; return -1; }
    if (uname(&u) < 0) return -1;
    n = strlen(u.nodename);
    if (n >= len) { errno = ENAMETOOLONG; return -1; }
    memcpy(name, u.nodename, n + 1);
    return 0;
}

int utime(const char *path, const struct utimbuf *times) {
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(30, (int)path, (int)times));
#else
    unsigned when = times ? (unsigned)times->modtime : 0;
    return ioerr(fs_call(12, (int)path, (int)when));
#endif
}

int getrlimit(int resource, struct rlimit *rlim) {
    if (!rlim) { errno = EFAULT; return -1; }
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(76, resource, (int)rlim));
#else
    if (resource != RLIMIT_NOFILE) { errno = EINVAL; return -1; }
    rlim->rlim_cur = rlim->rlim_max = 16;
    return 0;
#endif
}
int getrusage(int who, struct rusage *usage) {
    if (!usage) { errno = EFAULT; return -1; }
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(77, who, (int)usage));
#else
    (void)who;
    memset(usage, 0, sizeof *usage);
    return 0;
#endif
}
int setrlimit(int resource, const struct rlimit *rlim) {
    if (!rlim) { errno = EFAULT; return -1; }
#if defined(__LINUX_TEST__)
    return ioerr(__syscall(75, resource, (int)rlim));
#else
    if (resource != RLIMIT_NOFILE) { errno = EINVAL; return -1; }
    if (rlim->rlim_cur > 16 || rlim->rlim_max > 16) { errno = EPERM; return -1; }
    return 0;
#endif
}

static int vfs_fill(struct statvfs *buf) {
    unsigned info[4];
    int r = fs_call(13, 0, (int)info);
    if (r < 0) { errno = -r; return -1; }
    memset(buf, 0, sizeof *buf);
    buf->f_bsize = buf->f_frsize = 512;
    buf->f_blocks = info[0];
    buf->f_bfree = buf->f_bavail = info[1];
    buf->f_files = info[2];
    buf->f_ffree = buf->f_favail = info[3];
    buf->f_namemax = 255;
    return 0;
}
int statvfs(const char *path, struct statvfs *buf) {
    struct stat st;
    if (!path || !buf) { errno = EFAULT; return -1; }
    if (stat(path, &st) < 0) return -1;
#if defined(__LINUX_TEST__)
    long raw[16];
    int r = __syscall(99, (int)path, (int)raw);
    if (r < 0) { errno = -r; return -1; }
    memset(buf, 0, sizeof *buf);
    buf->f_bsize = (unsigned long)raw[1];
    buf->f_blocks = (unsigned long)raw[2];
    buf->f_bfree = (unsigned long)raw[3];
    buf->f_bavail = (unsigned long)raw[4];
    buf->f_files = (unsigned long)raw[5];
    buf->f_ffree = (unsigned long)raw[6];
    buf->f_namemax = (unsigned long)raw[9];
    buf->f_frsize = raw[10] ? (unsigned long)raw[10] : buf->f_bsize;
    return 0;
#else
    (void)st;
    return vfs_fill(buf);
#endif
}
int fstatvfs(int fd, struct statvfs *buf) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
#if defined(__LINUX_TEST__)
    long raw[16];
    int r = __syscall(100, fd, (int)raw);
    if (r < 0) { errno = -r; return -1; }
    memset(buf, 0, sizeof *buf);
    buf->f_bsize = (unsigned long)raw[1];
    buf->f_blocks = (unsigned long)raw[2];
    buf->f_bfree = (unsigned long)raw[3];
    buf->f_bavail = (unsigned long)raw[4];
    buf->f_namemax = 255;
    return 0;
#else
    (void)st;
    return vfs_fill(buf);
#endif
}

int tcgetattr(int fd, struct termios *t) {
    if (!t) { errno = EFAULT; return -1; }
    if (!isatty(fd)) { errno = ENOTTY; return -1; }
    memset(t, 0, sizeof *t);
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | IEXTEN;
#if !defined(__LINUX_TEST__)
    int mode = ttyctl(fd, TTYCTL_GETMODE, 0);
    if (mode >= 0) {
        if (mode & TTY_RAW) t->c_lflag &= ~(unsigned)(ICANON | ECHO | ECHOE);
        if (mode & TTY_NOSIG) t->c_lflag &= ~(unsigned)ISIG;
    }
#endif
    t->c_cc[VINTR] = 3;
    t->c_cc[VERASE] = 127;
    t->c_cc[VMIN] = 1;
    return 0;
}
int tcsetattr(int fd, int optional_actions, const struct termios *t) {
    (void)optional_actions;
    if (!t) { errno = EFAULT; return -1; }
    if (!isatty(fd)) { errno = ENOTTY; return -1; }
#if !defined(__LINUX_TEST__)
    int mode = ttyctl(fd, TTYCTL_GETMODE, 0);
    if (mode < 0) mode = 0;
    mode &= ~(TTY_RAW | TTY_NOSIG);
    if (!(t->c_lflag & ICANON)) mode |= TTY_RAW;
    if (!(t->c_lflag & ISIG)) mode |= TTY_NOSIG;
    ttymode(mode);
#else
    (void)t;
#endif
    return 0;
}
int tcflush(int fd, int queue) {
    (void)queue;
    if (!isatty(fd)) { errno = ENOTTY; return -1; }
    errno = ENOSYS;
    return -1;
}
int cfsetospeed(struct termios *t, speed_t speed) { if (!t) { errno = EFAULT; return -1; } t->c_ospeed = speed; return 0; }
int cfsetispeed(struct termios *t, speed_t speed) { if (!t) { errno = EFAULT; return -1; } t->c_ispeed = speed; return 0; }
speed_t cfgetospeed(const struct termios *t) { return t ? t->c_ospeed : 0; }
speed_t cfgetispeed(const struct termios *t) { return t ? t->c_ispeed : 0; }

static char log_ident[64];
void openlog(const char *ident, int option, int facility) {
    (void)option; (void)facility;
    log_ident[0] = 0;
    if (ident) snprintf(log_ident, sizeof log_ident, "%s", ident);
}
void syslog(int priority, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    (void)priority;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_ident[0]) dprintf(2, "%s: %s\n", log_ident, buf);
    else dprintf(2, "%s\n", buf);
}
void closelog(void) { log_ident[0] = 0; }

struct group *getgrgid(gid_t gid) { (void)gid; errno = ENOENT; return NULL; }
struct group *getgrnam(const char *name) { (void)name; errno = ENOENT; return NULL; }
struct group *getgrent(void) { errno = ENOENT; return NULL; }
void setgrent(void) {}
void endgrent(void) {}
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups) {
    (void)user;
    if (!ngroups) { errno = EINVAL; return -1; }
    if (!groups || *ngroups < 1) { *ngroups = 1; errno = EINVAL; return -1; }
    groups[0] = group;
    *ngroups = 1;
    return 1;
}
int setgroups(size_t size, const gid_t *list) {
    (void)size;
    (void)list;
    return 0;
}
int initgroups(const char *user, gid_t group) {
    gid_t groups[32];
    int n = 32;
    if (getgrouplist(user, group, groups, &n) < 0) return -1;
    return setgroups((size_t)n, groups);
}

static int fill_temp(char *x, unsigned *state) {
    unsigned v = *state = *state * 1664525u + 1013904223u;
    for (int i = 0; i < 6; i++) {
        x[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
        v /= 36;
    }
    return 0;
}
static char *temp_xs(char *template) {
    size_t n;
    char *x;
    if (!template) { errno = EINVAL; return NULL; }
    n = strlen(template);
    if (n < 6) { errno = EINVAL; return NULL; }
    x = template + n - 6;
    for (int i = 0; i < 6; i++) if (x[i] != 'X') { errno = EINVAL; return NULL; }
    return x;
}
char *mkdtemp(char *template) {
    char *x = temp_xs(template);
    unsigned state;
    if (!x) return NULL;
    state = (unsigned)getpid() * 2246822519u + 1u;
    for (int attempt = 0; attempt < 256; attempt++) {
        fill_temp(x, &state);
        if ((mkdir)(template, 0700) == 0) return template;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST;
    return NULL;
}
int mkstemp(char *template) {
    size_t n;
    unsigned state;
    char *x;
    if (!template) { errno = EINVAL; return -1; }
    n = strlen(template);
    if (n < 6) { errno = EINVAL; return -1; }
    x = template + n - 6;
    for (int i = 0; i < 6; i++) if (x[i] != 'X') { errno = EINVAL; return -1; }
    state = (unsigned)getpid() * 2654435761u + 1u;
    for (int attempt = 0; attempt < 256; attempt++) {
        unsigned v = state = state * 1664525u + 1013904223u + (unsigned)attempt;
        for (int i = 0; i < 6; i++) {
            x[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
            v /= 36;
        }
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

char *realpath(const char *path, char *resolved) {
    char cur[1024], link[1024], out[1024];
    struct stat st;
    int hops = 0;
    if (!path || !path[0]) { errno = EINVAL; return NULL; }
    if (path[0] == '/') {
        if (strlen(path) >= sizeof cur) { errno = ENAMETOOLONG; return NULL; }
        memcpy(cur, path, strlen(path) + 1);
    } else {
        if (!getcwd(cur, sizeof cur)) return NULL;
        if (strlen(cur) + strlen(path) + 2 > sizeof cur) { errno = ENAMETOOLONG; return NULL; }
        if (cur[0] && cur[strlen(cur) - 1] != '/') strcat(cur, "/");
        strcat(cur, path);
    }
    for (;;) {
        int i = 0, oi = 0, saw = 0;
        out[0] = 0;
        if (cur[0] == '/') { out[oi++] = '/'; out[oi] = 0; i = 1; }
        while (cur[i]) {
            int start, cl;
            while (cur[i] == '/') i++;
            if (!cur[i]) break;
            start = i;
            while (cur[i] && cur[i] != '/') i++;
            cl = i - start;
            if (cl == 1 && cur[start] == '.') continue;
            if (cl == 2 && cur[start] == '.' && cur[start + 1] == '.') {
                if (oi > 1) {
                    while (oi > 1 && out[oi - 1] != '/') oi--;
                    if (oi > 1) oi--;
                    if (oi < 1) oi = 1;
                    out[oi] = 0;
                }
                continue;
            }
            if (oi > 1) out[oi++] = '/';
            if (oi + cl >= (int)sizeof out) { errno = ENAMETOOLONG; return NULL; }
            memcpy(out + oi, cur + start, (size_t)cl);
            oi += cl;
            out[oi] = 0;
            if (lstat(out, &st) < 0) return NULL;
            if (!S_ISLNK(st.st_mode)) continue;
            if (++hops > 40) { errno = ELOOP; return NULL; }
            {
                int n = (int)readlink(out, link, sizeof link - 1);
                char rest[1024];
                if (n < 0) return NULL;
                link[n] = 0;
                if (strlen(cur + i) >= sizeof rest) { errno = ENAMETOOLONG; return NULL; }
                memcpy(rest, cur + i, strlen(cur + i) + 1);
                if (link[0] == '/') {
                    if (strlen(link) + strlen(rest) >= sizeof cur) { errno = ENAMETOOLONG; return NULL; }
                    memcpy(cur, link, strlen(link) + 1);
                    strcat(cur, rest);
                } else {
                    char dir[1024];
                    char *sl;
                    memcpy(dir, out, strlen(out) + 1);
                    sl = strrchr(dir, '/');
                    if (sl && sl != dir) *sl = 0;
                    else if (sl) sl[1] = 0;
                    if (snprintf(cur, sizeof cur, "%s%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", link, rest) >= (int)sizeof cur) {
                        errno = ENAMETOOLONG; return NULL;
                    }
                }
            }
            saw = 1;
            break;
        }
        if (saw) continue;
        if (!out[0]) { errno = ENOENT; return NULL; }
        if (lstat(out, &st) < 0) return NULL;
        {
            char *dst = resolved ? resolved : malloc(strlen(out) + 1);
            if (!dst) return NULL;
            memcpy(dst, out, strlen(out) + 1);
            return dst;
        }
    }
}

static int fnm_class(const char **pat, char c) {
    const char *p = *pat;
    int neg = 0, ok = 0;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    if (*p == ']') { if (c == ']') ok = 1; p++; }
    while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            if (c >= p[0] && c <= p[2]) ok = 1;
            p += 3;
        } else {
            if (c == *p) ok = 1;
            p++;
        }
    }
    if (*p != ']') return -1;
    *pat = p + 1;
    return ok != neg;
}
static int fnm_period(const char *str, const char *base, int flags) {
    if (!(flags & FNM_PERIOD) || *str != '.') return 0;
    if (str == base) return 1;
    return (flags & FNM_PATHNAME) && str > base && str[-1] == '/';
}
static int fnm_rec(const char *pat, const char *str, const char *base, int flags) {
    while (*pat) {
        if (!(flags & FNM_NOESCAPE) && *pat == '\\' && pat[1]) {
            if (*str != pat[1]) return FNM_NOMATCH;
            pat += 2; str++;
            continue;
        }
        if (*pat == '*') {
            if (fnm_period(str, base, flags)) return FNM_NOMATCH;
            while (*pat == '*') pat++;
            if (!*pat) {
                if ((flags & FNM_PATHNAME) && strchr(str, '/')) return FNM_NOMATCH;
                return 0;
            }
            for (;;) {
                if (fnm_rec(pat, str, base, flags & ~FNM_PERIOD) == 0) return 0;
                if (!*str || ((flags & FNM_PATHNAME) && *str == '/')) break;
                str++;
            }
            return FNM_NOMATCH;
        }
        if (*pat == '?') {
            if (fnm_period(str, base, flags) || !*str || ((flags & FNM_PATHNAME) && *str == '/'))
                return FNM_NOMATCH;
            pat++; str++;
            continue;
        }
        if (*pat == '[') {
            int m;
            if (fnm_period(str, base, flags) || !*str || ((flags & FNM_PATHNAME) && *str == '/'))
                return FNM_NOMATCH;
            pat++;
            m = fnm_class(&pat, *str);
            if (m <= 0) return FNM_NOMATCH;
            str++;
            continue;
        }
        if (*pat != *str) return FNM_NOMATCH;
        pat++;
        if (!*str) return *pat ? FNM_NOMATCH : 0;
        str++;
    }
    return *str ? FNM_NOMATCH : 0;
}
int fnmatch(const char *pattern, const char *string, int flags) {
    if (!pattern || !string) { errno = EINVAL; return FNM_NOMATCH; }
    return fnm_rec(pattern, string, string, flags);
}

int system(const char *cmd) {
    int pid, st = 0;
    char *av[4];
    if (!cmd) return 1;
    if (strlen(cmd) >= 64 * 1024) { errno = EINVAL; return -1; }
    pid = fork();
    if (pid < 0) { errno = -pid; return -1; }
    if (pid == 0) {
        av[0] = "sh";
        av[1] = "-c";
        av[2] = (char *)cmd;
        av[3] = NULL;
        exec("/bin/sh", av);
        exit(127);
    }
    if (waitpid(pid, &st, 0) < 0) return -1;
    errno = 0;
    return st;
}

#include "regex.c"
