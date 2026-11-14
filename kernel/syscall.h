#ifndef SYSCALL_H
#define SYSCALL_H
#include "idt.h"

/* Номера системных вызовов. ДОЛЖНЫ совпадать с user/lib/myos.h.
 * Вызов: int 0x80, eax = номер, ebx/ecx/edx = аргументы, результат в eax.
 * Ошибка — отрицательное значение: -errno (как в Linux). */
enum {
    SYS_EXIT    = 0,    /* exit(code)                                   */
    SYS_WRITE   = 1,    /* write(fd, buf, len)          -> записано     */
    SYS_READ    = 2,    /* read(fd, buf, len)           -> прочитано    */
    SYS_OPEN    = 3,    /* open(path, mode)             -> fd           */
    SYS_CLOSE   = 4,    /* close(fd)                                    */
    SYS_SBRK    = 5,    /* sbrk(increment)              -> старый brk   */
    SYS_SLEEP   = 6,    /* sleep(ms)                                    */
    SYS_GETPID  = 7,    /* getpid()                                     */
    SYS_TICKS   = 8,    /* ticks()                      -> мс с загрузки */
    SYS_YIELD   = 9,    /* yield()                                      */
    SYS_FORK    = 10,   /* fork()                       -> 0 / pid       */
    SYS_EXEC    = 11,   /* exec(path, argv)             -> не вернётся   */
    SYS_WAITPID = 12,   /* waitpid(pid, &status, opts)  -> pid           */
    SYS_PIPE    = 13,   /* pipe(int fds[2])                              */
    SYS_DUP2    = 14,   /* dup2(old, new)               -> new           */
    SYS_CHDIR   = 15,   /* chdir(path)                                   */
    SYS_GETCWD  = 16,   /* getcwd(buf, len)                              */
    SYS_READDIR = 17,   /* readdir(path, index, &ent)   -> 1 / 0 (конец) */
    SYS_KILL    = 18,   /* kill(pid)                                     */
    SYS_SETPGID = 19,   /* setpgid(pid, pgid)                            */
    SYS_SETFG   = 20,   /* setfg(pgid): кого убивает Ctrl+C              */
    SYS_MKDIR   = 21,   /* mkdir(path)                                   */
    SYS_UNLINK  = 22,   /* unlink(path): файл или пустой каталог         */
    SYS_PS      = 23,   /* ps(struct proc_info *buf, max) -> сколько     */
    SYS_GETPPID = 24,   /* getppid()                                     */
    SYS_TTYMODE = 25,   /* ttymode(mode): TTY_RAW|NONBLOCK|NOSIG -> старый */
    SYS_STAT    = 26,   /* stat(path, &dirent_info): тип и размер        */
    SYS_GFX_INFO    = 27,   /* gfx_info(&gfx_info)                       */
    SYS_GFX_ACQUIRE = 28,   /* gfx_acquire(on): забрать экран у консоли  */
    SYS_GFX_BLIT    = 29,   /* gfx_blit(pixels, &rect): кадр на экран    */
    SYS_MOUSE       = 30,   /* mouse(&mouse_state)                       */
    SYS_GFX_FONT    = 31,   /* gfx_font(buf[4096]): шрифт 8x16           */
    SYS_TERMSIZE    = 32,   /* termsize() -> (столбцы << 16) | строки    */
    SYS_SOCKET      = 33,   /* socket(1 TCP | 2 UDP) -> fd               */
    SYS_CONNECT     = 34,   /* connect(fd, ip, port)                     */
    SYS_BIND        = 35,   /* bind(fd, port)                            */
    SYS_LISTEN      = 36,   /* listen(fd, backlog)                       */
    SYS_ACCEPT      = 37,   /* accept(fd, &sockaddr) -> новый fd         */
    SYS_SENDTO      = 38,   /* sendto(fd, buf, len, ip, port)            */
    SYS_RECVFROM    = 39,   /* recvfrom(fd, buf, len, &sockaddr, мс)     */
    SYS_NETINFO     = 40,   /* netinfo(&net_info)                        */
    SYS_PING        = 41,   /* ping(ip, seq, мс) -> мс или -errno        */
    SYS_NETCONFIG   = 42,   /* netconfig(что, значение): 1 = DNS         */
    SYS_SHM_ALLOC   = 43,   /* shm_alloc(байт) -> id общей памяти        */
    SYS_SHM_MAP     = 44,   /* shm_map(id) -> адрес в своём процессе     */
    SYS_SHM_UNMAP   = 45,   /* shm_unmap(адрес)                          */
    SYS_MSG_SEND    = 46,   /* msg_send(pid, &msg[64])                   */
    SYS_MSG_RECV    = 47,   /* msg_recv(&msg[64], мс: 0 не ждать, -1 вечно) */
    SYS_SERVICE     = 48,   /* service(1 зарегистрировать | 2 найти, имя) */
    SYS_TIME        = 49,   /* time(&rtc_time): дата и время из CMOS     */
    SYS_GFX_MODE    = 50,   /* gfx_mode(w,h,check_only): set or query mode */
    SYS_GFX_INFO2   = 51,   /* gfx_info2(&gfx_info2): карта, режим, умения */
    SYS_GFX_ACCEL   = 52,   /* gfx_accel(GA_COPY|GA_FILL, &args): на GPU */
    SYS_GFX_CURSOR  = 53,   /* gfx_cursor(вкл, &gfx_cursor|0): аппаратный курсор */
    SYS_PTY_OPEN    = 54,   /* pty_open(int fds[2]): [0] ведущий, [1] ведомый */
    SYS_TTYCTL      = 55,   /* ttyctl(fd, op, arg): размер pty и т.п. (tty.h) */
    SYS_KBLAYOUT    = 56,   /* kblayout(-1 узнать | 0 US | 1 RU) -> текущая */
    SYS_SOCKCTL     = 57,   /* sockctl(fd, op, arg): тайм-ауты сокета (net.h) */
    SYS_MOUNT,
    SYS_MOUNTINFO,
    SYS_BLOCKINFO,
    SYS_MKFS,
    SYS_PARTCTL,
    SYS_ACCOUNT,
    SYS_CLIPBOARD,      /* clipboard(0 прочитать | 1 записать, buf, len) */
    SYS_LSEEK,          /* lseek(fd, offset, whence) -> новое смещение */
    SYS_FSTAT,          /* fstat(fd, &{mode, size}) */
    SYS_RENAME,         /* rename(old, new) */
    SYS_UNITCTL,        /* unitctl(op, name|index, &info): службы */
    SYS_MMAP,           /* mmap(&{addr,len,prot,flags,fd,off}) -> адрес */
    SYS_MUNMAP,         /* munmap(addr, len) */
    SYS_MPROTECT,       /* mprotect(addr, len, prot) */
    SYS_SYMLINK,        /* symlink(target, linkpath) */
    SYS_READLINK,       /* readlink(path, buf, len) -> байт, без нуля */
    SYS_LSTAT,          /* lstat(path, &dirent_info): сама ссылка, не цель */
    SYS_SYSINFO,        /* sysinfo(&system_info, size) */
    SYS_SPAWN,          /* spawn(path, argv, stdin_fd) */
    SYS_CPUINFO,        /* cpuinfo(&cpu_info, capacity) */
    SYS_THREAD_CREATE, SYS_THREAD_JOIN, SYS_THREAD_EXIT, SYS_GETTID, SYS_THREAD_MUTEX,
    SYS_KLOG_READ,      /* klog_read(buf, capacity, &cursor), nondestructive */
    SYS_SSHCTL,         /* POSIX adapter operations, see ssh-syscall.inc */
    SYS_FSCTL,          /* file metadata, access, umask and mode-aware creation */
    SYS_IPCCTL,         /* explicit SHM grants, discard, size and mailbox grants */
    SYS_REBOOT,         /* reboot(RB_POWEROFF | RB_REBOOT): не возвращается */
    SYS_THREAD_CTL,     /* errno/ключи, detach, cond_wait/cond_wake; номера как в myos.h */
    SYS_HWINFO,         /* read-only PCI, USB root ports and SATA inventory */
    SYS_PREAD,          /* pread(fd, buf, len, off): смещение дескриптора не меняется */
    SYS_PWRITE,         /* pwrite(fd, buf, len, off) */
    SYS_FSYNC,          /* fsync(fd): сбросить кэш блоков на диск */
    SYS_GETPGID,        /* getpgid(pid): 0 — группа текущего процесса */
    SYS_COUNT
};
/* user/lib/myos.h держит те же номера TC_*. */
#define TC_SET_TCB   0
#define TC_GET_TCB   1
#define TC_SET_STACK 2
#define TC_DETACH    3
#define TC_COND_WAIT 4
#define TC_COND_WAKE 5
/* user/ldso.c не включает этот заголовок и держит номера явно. */
_Static_assert(SYS_SSHCTL == 84, "Keep the published SYS_SSHCTL ABI stable");
_Static_assert(SYS_FSCTL == 85, "Keep the published SYS_FSCTL ABI stable");
_Static_assert(SYS_IPCCTL == 86, "Keep the published SYS_IPCCTL ABI stable");
_Static_assert(SYS_HWINFO + 1 == SYS_PREAD, "SYS_PREAD follows SYS_HWINFO in user/lib/myos.h");
_Static_assert(SYS_MMAP == 69, "SYS_MMAP must stay 69: user/ldso.c hardcodes it");
_Static_assert(SYS_MUNMAP == 70, "SYS_MUNMAP must stay 70: user/ldso.c hardcodes it");

/* коды ошибок (как в Linux) */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EBUSY        16
#define EFAULT       14
#define EEXIST       17
#define ESPIPE       29
#define EROFS        30
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define EPIPE        32
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ELOOP        40
#define EADDRINUSE   98
#define ENETDOWN     100
#define ECONNRESET   104
#define EISCONN      106
#define EMSGSIZE     90
#define ENOTCONN     107
#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113

#define WNOHANG 1

/* для readdir */
struct dirent_info {
    char name[256];                 /* имя до 255 символов и нуль */
    unsigned type;                  /* 1 файл, 2 каталог */
    unsigned size;
};

/* для ps */
struct proc_info {
    unsigned pid, ppid, pgid;
    char     name[16];
    unsigned state;                 /* 0 ready 1 running 2 sleeping 3 blocked 4 zombie */
    unsigned ring;
    unsigned cpu_percent, cpu_ms, mem_kb;
};

struct gfx_info { unsigned width, height, bpp, pitch; };
struct gfx_rect { int x, y, w, h; };
struct gfx_info2 {
    unsigned width, height, bpp, pitch;
    unsigned vram_kb, max_w, max_h, caps, gen;
    int hw_cursor;                  /* аппаратный курсор сейчас включён */
    char driver[24];
};
#define GA_COPY 1                   /* args: sx, sy, dx, dy, w, h */
#define GA_SURFACE_UPDATE 8
#define GA_SURFACE_SCALED 9
#define GA_READBACK 7
#define GA_SURFACE_UPLOAD 5
#define GA_SURFACE_BLIT 6
#define GA_FRAME_BEGIN 3
#define GA_FRAME_END 4
#define GA_FILL 2                   /* args: x, y, w, h, цвет */
struct gfx_cursor { int w, h, hot_x, hot_y; unsigned px[32 * 32]; };

void syscall_dispatch(struct regs *r);

#endif
