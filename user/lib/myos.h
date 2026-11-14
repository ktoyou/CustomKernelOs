/* myos.h — всё, что нужно программе для MyOS: системные вызовы и мини-libc.
 * Никаких системных заголовков: программы собираются с -nostdinc. */
#ifndef MYOS_H
#define MYOS_H
#include "sysinfo.h"
#include "hwinfo.h"
int sysinfo(struct system_info *out, unsigned size);
/* Returns online CPU count, or negative errno; capacity is in records. */
int cpuinfo(struct cpu_info *out, unsigned capacity);
#define KLOG_CAPACITY 16384u
#define KLOG_OLDEST 1u
#define KLOG_END 2u
struct klog_cursor { unsigned lo,hi,dropped,flags; };
/* Returns bytes or -1/errno; updates this reader's cursor, never clears logs.
 * OLDEST starts at retained history; END skips history. Flags reset on read.
 * dropped reports overwritten bytes since the previous call. */
int klog_read(void *buffer,unsigned capacity,struct klog_cursor *cursor);
#define RB_POWEROFF 0
#define RB_REBOOT   1
int reboot(int how);            /* при успехе не возвращается */


typedef unsigned int   size_t;
typedef int            ssize_t;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef _Bool          bool;
#define true  1
#define false 0
#define NULL  ((void *)0)

#ifdef __MYOS_CC__
/* cc (компилятор внутри MyOS): аргументы лежат в стеке подряд, по 4 байта (double — 8) */
typedef char *va_list;
#define va_start(v, l) ((v) = (char *)&(l) + ((sizeof(l) + 3) & ~3))
#define va_end(v)      ((void)0)
#define va_arg(v, t)   (*(t *)(((v) += (sizeof(t) + 3) & ~3) - ((sizeof(t) + 3) & ~3)))
#define va_copy(d, s)  ((d) = (s))
#else
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)
#define va_copy(d, s)  __builtin_va_copy(d, s)
#endif

/* ---- системные вызовы (номера совпадают с kernel/syscall.h) ---- */
enum {
    SYS_EXIT, SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE,
    SYS_SBRK, SYS_SLEEP, SYS_GETPID, SYS_TICKS, SYS_YIELD,
    SYS_FORK, SYS_EXEC, SYS_WAITPID, SYS_PIPE, SYS_DUP2,
    SYS_CHDIR, SYS_GETCWD, SYS_READDIR, SYS_KILL, SYS_SETPGID,
    SYS_SETFG, SYS_MKDIR, SYS_UNLINK, SYS_PS, SYS_GETPPID,
    SYS_TTYMODE, SYS_STAT, SYS_GFX_INFO, SYS_GFX_ACQUIRE, SYS_GFX_BLIT,
    SYS_MOUSE, SYS_GFX_FONT, SYS_TERMSIZE, SYS_SOCKET, SYS_CONNECT,
    SYS_BIND, SYS_LISTEN, SYS_ACCEPT, SYS_SENDTO, SYS_RECVFROM,
    SYS_NETINFO, SYS_PING, SYS_NETCONFIG, SYS_SHM_ALLOC, SYS_SHM_MAP,
    SYS_SHM_UNMAP, SYS_MSG_SEND, SYS_MSG_RECV, SYS_SERVICE, SYS_TIME,
    SYS_GFX_MODE, SYS_GFX_INFO2, SYS_GFX_ACCEL, SYS_GFX_CURSOR,
    SYS_PTY_OPEN, SYS_TTYCTL, SYS_KBLAYOUT, SYS_SOCKCTL,
    SYS_MOUNT, SYS_MOUNTINFO, SYS_BLOCKINFO, SYS_MKFS, SYS_PARTCTL, SYS_ACCOUNT,
    SYS_CLIPBOARD, SYS_LSEEK, SYS_FSTAT, SYS_RENAME, SYS_UNITCTL,
    SYS_MMAP, SYS_MUNMAP, SYS_MPROTECT,
    SYS_SYMLINK, SYS_READLINK, SYS_LSTAT,
    SYS_SYSINFO, SYS_SPAWN, SYS_CPUINFO,
    SYS_THREAD_CREATE, SYS_THREAD_JOIN, SYS_THREAD_EXIT, SYS_GETTID, SYS_THREAD_MUTEX, SYS_KLOG_READ,
    SYS_SSHCTL, SYS_FSCTL, SYS_IPCCTL,
    SYS_REBOOT,
    SYS_THREAD_CTL, SYS_HWINFO,
    SYS_PREAD, SYS_PWRITE, SYS_FSYNC, SYS_GETPGID
};
/* Совпадают с kernel/syscall.h. Первое поле thread_tcb — errno этого потока. */
#define TC_SET_TCB   0
#define TC_GET_TCB   1
#define TC_SET_STACK 2
#define TC_DETACH    3
#define TC_COND_WAIT 4
#define TC_COND_WAKE 5
#define THREAD_KEYS 128
struct thread_key { void *val; unsigned gen; };
struct thread_tcb {
    int errno;
    struct thread_key key[THREAD_KEYS];
};
/* user/ldso.c не включает этот заголовок и держит номера явно. */
_Static_assert(SYS_HWINFO + 1 == SYS_PREAD, "SYS_PREAD follows SYS_HWINFO in kernel/syscall.h");
_Static_assert(SYS_SSHCTL == 84, "Keep the published SYS_SSHCTL ABI stable");
_Static_assert(SYS_FSCTL == 85, "Keep the published SYS_FSCTL ABI stable");
_Static_assert(SYS_IPCCTL == 86, "Keep the published SYS_IPCCTL ABI stable");
_Static_assert(SYS_MMAP == 69, "SYS_MMAP must stay 69: user/ldso.c hardcodes it");
_Static_assert(SYS_MUNMAP == 70, "SYS_MUNMAP must stay 70: user/ldso.c hardcodes it");

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

/* ошибки: системные вызовы возвращают -код (как в Linux); open/read/write/close/lseek/fstat
 * дополнительно приводят его к -1 и пишут сюда, как в POSIX. */
int *__errno_location(void);
#define errno (*__errno_location())

/* ошибки: системные вызовы возвращают -код (как в Linux) */
#define EPERM 1
#define EBUSY 16
#define EXDEV 18
#define EROFS 30
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EEXIST 17
#define ENODEV 19
#define ESPIPE 29
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOTTY 25
#define EDEADLK 35
#define EDOM 33
#define ERANGE 34
#define EMFILE 24
#define EFBIG 27
#define ENOSPC 28
#define EPIPE 32
#define ENAMETOOLONG 36
#define ENOLCK 37
#define ENOTEMPTY 39
#define ELOOP 40
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define EADDRINUSE 98
#define ENETDOWN 100
#define ECONNRESET 104
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define ENOSYS 38
#define ENXIO 6
#define ENFILE 23
#define EMLINK 31
#define EOVERFLOW 75
#define EILSEQ 84
#define ENOTSOCK 88
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRNOTAVAIL 99
#define ENETRESET 102
#define ECONNABORTED 103
#define ESHUTDOWN 108
#define EINPROGRESS 115
#define EWOULDBLOCK EAGAIN
#define ENOTSUP EOPNOTSUPP
#define EWOULDBLOCK EAGAIN

#define WNOHANG 1

struct dirent_info {
    char name[256];                 /* имя до 255 символов и нуль */
    unsigned type;                  /* 1 файл, 2 каталог, 5 символическая ссылка */
    unsigned size;
};

struct proc_info {
    unsigned pid, ppid, pgid;
    char     name[16];
    unsigned state;                 /* 0 ready 1 running 2 sleeping 3 blocked 4 zombie */
    unsigned ring;
    unsigned cpu_percent, cpu_ms, mem_kb;
};

#define STDIN  0
#define STDOUT 1
#define STDERR 2

/* Флаги open — как у System V / Linux i386. O_READ и O_WRITE сохранены:
 * раньше это были режимы 0 и 1 с тем же смыслом (чтение и «создать и обнулить»). */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_EXCL    0200
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_READ    O_RDONLY
#define O_WRITE   (O_WRONLY | O_CREAT | O_TRUNC)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

void  exit(int code) __attribute__((noreturn));
int   write(int fd, const void *buf, size_t len);
int   read(int fd, void *buf, size_t len);
int   open(const char *path, int flags, ...);
#ifndef MYOS_HOST_LIBC
int myos_open_default(const char *path,int flags);
#define MYOS_OPEN_PICK(_1,_2,_3,NAME,...) NAME
#define open(...) MYOS_OPEN_PICK(__VA_ARGS__,open,myos_open_default)(__VA_ARGS__)
#endif
int   close(int fd);
int   lseek(int fd, long offset, int whence);
struct block_info { char name[12]; unsigned start,sectors,partition; };
int account(int op, const char *name, const char *password);
/* Enumerate public account names (24-byte output), zero-based, -ENOENT at end. */
int account_list(char *name, unsigned index);
int mount(const char *device,const char *target);
int mountinfo(unsigned index,char device[128],char target[128]);
int blockinfo(unsigned index,struct block_info *info);
int mkfs(const char *device);
int partctl(const char *device,unsigned op,unsigned slot,unsigned start,unsigned sectors);
void *sbrk(int increment);
void *mmap(void *addr, unsigned len, int prot, int flags, int fd, unsigned off);
int   munmap(void *addr, unsigned len);
int   mprotect(void *addr, unsigned len, int prot);
void  sleep_ms(unsigned ms);
int   getpid(void);
unsigned ticks(void);           /* миллисекунды с момента загрузки */
void  yield(void);

/* Нативные потоки: стек принадлежит вызвавшему до успешного join. Это не POSIX.
 * errno у каждого потока свой, как только libc записала thread_tcb.
 * Мьютекс обнуляют. lock/unlock/trylock возвращают 0 или отрицательный errno. */
typedef struct { unsigned owner; } thread_mutex_t;
int thread_create(void (*fn)(void *), void *arg, void *stack, unsigned stack_size);
int thread_join(int tid, int *code);
void thread_exit(int code) __attribute__((noreturn));
int gettid(void);
int thread_mutex_lock(thread_mutex_t *m);
int thread_mutex_unlock(thread_mutex_t *m);
int thread_mutex_trylock(thread_mutex_t *m);
int thread_ctl(int op, int a, int b, int c);
int thread_set_tcb(struct thread_tcb *tcb);
struct thread_tcb *thread_get_tcb(void);

/* процессы в стиле Unix */
int   spawn(const char *path, char **argv, int stdin_fd); /* -1 inherits stdin */
int   fork(void);                         /* 0 в потомке, pid потомка в родителе */
int   exec(const char *path, char **argv);/* при успехе не возвращается */
int   waitpid(int pid, int *status, int options);   /* pid -1 = любой ребёнок */
int   getppid(void);
int   kill(int pid, ...);          /* второй аргумент — номер сигнала, его можно не передавать */
int   setpgid(int pid, int pgid);
int   setfg(int pgid);                    /* кого убьёт Ctrl+C (0 — никого) */
int   pipe(int fds[2]);                   /* fds[0] — читать, fds[1] — писать */
int   dup2(int oldfd, int newfd);
int   chdir(const char *path);
char *getcwd(char *buf, size_t len);   /* буфер или NULL; NULL, 0 — выделить строку */
int   dirread(const char *path, int index, struct dirent_info *out);  /* 1 есть, 0 конец */
int   mkdir(const char *path, ...);
#ifndef MYOS_HOST_LIBC
int myos_mkdir_default(const char *path);
#define MYOS_MKDIR_PICK(_1,_2,NAME,...) NAME
#define mkdir(...) MYOS_MKDIR_PICK(__VA_ARGS__,mkdir,myos_mkdir_default)(__VA_ARGS__)
#endif
int   unlink(const char *path);
int   ps(struct proc_info *buf, int max);
int   dirstat(const char *path, struct dirent_info *out);   /* 0 или -errno */
int   symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);

/* ---- терминал ---- */
int  term_cols(void);           /* размер терминала: 128x48 в графике, 80x25 в тексте */
int  term_rows(void);
#define SCREEN_W term_cols()
#define SCREEN_H term_rows()
#define TTY_RAW      1          /* клавиши сразу, без эха */
#define TTY_NONBLOCK 2          /* read не ждёт клавишу (вернёт 0) */
#define TTY_RESIZE_EVENT 8 /* Internal readkey opt-in; ordinary read stays uninterrupted. */
#define TTY_NOSIG    4          /* Ctrl+C приходит как код 3, а не убивает */
int  ttymode(int mode);         /* -> прежний режим */

/* псевдотерминал: fds[0] — ведущая сторона (у окна терминала), fds[1] — ведомая
 * (stdin/stdout/stderr оболочки). Размер задаёт окно: ttyctl(fd, TTYCTL_SETSIZE, cols << 16 | rows). */
int  pty_open(int fds[2]);
#define TTYCTL_SETSIZE 1
#define TTYCTL_GETSIZE 2
#define TTYCTL_GETFG   3
#define TTYCTL_GETOFLAGS 5
#define TTYCTL_SETOFLAGS 6
#define TTYCTL_GETMODE 4
int  ttyctl(int fd, int op, int arg);
int  kblayout(int l);           /* раскладка: -1 узнать, 0 US, 1 русская -> текущая */
int  utf8_put(char *out, int cp); /* кодовая точка -> UTF-8 (1..3 байта) -> сколько */

/* readkey(): одна клавиша в сыром режиме. Обычные — свой код (Enter = '\r',
 * Ctrl+буква = 1..26, Esc = 27), особые — коды ниже. -1 = клавиш нет (NONBLOCK).
 * Буквы не латиницей (русская раскладка) — кодовой точкой Unicode: 'й' = 0x439. */
enum { KEY_UP = 1000, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END,
       KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_INS };
int  readkey(void);
#define KEY_RESIZE 0x30000 /* Terminal geometry changed; redraw using term_cols/rows. */
#define KEY_SHIFT 0x10000
#define KEY_COPY  0x20000
#define KEY_PASTE 0x20001
/* UTF-8 clipboard, at most 65536 bytes. get returns length, -EFBIG if too small.
 * No trailing NUL is stored. get(NULL, 0) queries length. */
#define CLIPBOARD_MAX 65536
int clipboard_set(const char *text, unsigned length);
int clipboard_get(char *text, unsigned capacity);

#define CTRL(c) ((c) & 0x1F)

/* escape-последовательности ANSI для консоли */
#define ANSI_CLEAR    "\x1b[2J\x1b[H"
#define ANSI_RESET    "\x1b[0m"
#define ANSI_HIDE     "\x1b[?25l"
#define ANSI_SHOW     "\x1b[?25h"
#define ANSI_BOLD     "\x1b[1m"
#define ANSI_REVERSE  "\x1b[7m"
#define ANSI_RED      "\x1b[31m"
#define ANSI_GREEN    "\x1b[32m"
#define ANSI_YELLOW   "\x1b[33m"
#define ANSI_BLUE     "\x1b[34m"
#define ANSI_MAGENTA  "\x1b[35m"
#define ANSI_CYAN     "\x1b[36m"
#define ANSI_WHITE    "\x1b[37m"
void gotoxy(int x, int y);      /* x, y с нуля */

/* ---- строки и память ---- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);   /* можно с перекрытием */
int    memcmp(const void *a, const void *b, size_t n);
int    atoi(const char *s);
int    isdigit(int c);
int    isspace(int c);
int    isalpha(int c);
int    toupper(int c);
int    tolower(int c);
char  *strstr(const char *haystack, const char *needle);
int    isalnum(int c);
char  *strrchr(const char *s, int c);
char  *strncpy(char *dst, const char *src, size_t n);
const char *strerror(int err);           /* err — положительный код (errno) */

/* ---- ввод-вывод ---- */
int  printf(const char *fmt, ...);
int  dprintf(int fd, const char *fmt, ...);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  puts(const char *s);
int  putchar(int c);
int  getln(char *buf, int max);     /* строка с клавиатуры без '\n'; -1 = конец ввода */
int  readln(int fd, char *buf, int max);  /* строка из любого fd (файл, канал); -1 = конец */

/* ---- память ---- */
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void *reallocarray(void *p, size_t n, size_t size);
void  free(void *p);

/* ---- сеть ----
 * Сокет — обычный дескриптор: read/write/close работают, его можно отдать
 * программе через dup2. POSIX-вызовы socket/connect/bind живут в sys/socket.h.
 * IP-адрес — число: 10.0.2.2 = IP(10,0,2,2). */
#define SOCK_TCP 1
#define SOCK_UDP 2
#define IP(a, b, c, d) (((unsigned)(a) << 24) | ((unsigned)(b) << 16) | ((unsigned)(c) << 8) | (unsigned)(d))
struct net_info {
    unsigned char mac[6], up, dhcp;
    unsigned ip, mask, gw, dns, lease;
    unsigned rx_packets, tx_packets, rx_bytes, tx_bytes;
};
int  netinfo(struct net_info *out);
int  ping(unsigned ip, int seq, unsigned timeout_ms);  /* -> время ответа в мс или -errno */
int  set_dns(unsigned ip);
int  ip_parse(const char *s, unsigned *ip);             /* "10.0.2.2" -> 1, иначе 0 */
char *ip_str(unsigned ip, char *buf);                   /* buf минимум 16 байт */
int  resolve(const char *name, unsigned *ip);           /* имя или IP -> 0 / -errno (DNS) */
int  tcp_connect(const char *host, int port);           /* resolve + socket + connect -> fd */
int  tcp_connect_timeout(const char *host, int port, unsigned ms);   /* то же, ждать ответа не дольше ms */
/* настройки сокета: sockctl(fd, SOCK_RCVTIMEO, мс) — read() вернёт -ETIMEDOUT, если данных
 * нет дольше мс (0 — ждать вечно); SOCK_CONNTIMEO — то же для connect(); SOCK_NREAD — сколько
 * байт можно прочитать прямо сейчас, не ожидая */
#define SOCK_RCVTIMEO  1
#define SOCK_CONNTIMEO 2
#define SOCK_NREAD     3
int  sockctl(int fd, int op, unsigned arg);

/* ---- общение процессов ----
 * Сообщение — 64 байта: кому-то в почтовый ящик (msg_send), из своего — msg_recv.
 * Общая память: shm_alloc создаёт кусок, shm_map(id) делает его видимым в процессе
 * (в каждом процессе — по своему адресу). Другому PID нужен явный shm_grant.
 * Смена UID/exec инвалидирует старые grants; уже полученное отображение сохраняется. */
struct msg {
    int  from;                      /* pid отправителя — заполняет ядро */
    int  type;
    int  a, b, c, d;
    char data[40];
};
int   msg_send(int pid, struct msg *m);             /* 0 или -errno (ESRCH — нет такого) */
int   msg_recv(struct msg *m, int timeout_ms);      /* 0 — не ждать, -1 — ждать вечно */
int   shm_alloc(unsigned size);                     /* -> id */
void *shm_map(int id);                              /* -> адрес или NULL */
int   shm_unmap(void *addr);
#define SHM_READ 1
#define SHM_WRITE 2
#define THREAD_STACK_MIN_BYTES 12288u
int shm_grant(int id,int pid,unsigned rights); /* 0 revokes future attachments */
int shm_discard(int id); /* release an allocation that has never been mapped */
int shm_size(int id);
int msg_allow(int pid,int allow); /* receiver authorizes a foreign sender */
int   service_register(const char *name);           /* «я — служба name» */
int   service_find(const char *name);               /* -> pid службы или 0 */

struct datetime { unsigned year, month, day, hour, min, sec; };
int   datetime_get(struct datetime *t);             /* часы компьютера (не get_time: так зовёт Git) */

enum { UNIT_LIST, UNIT_STATUS, UNIT_START, UNIT_STOP, UNIT_RESTART,
       UNIT_ENABLE, UNIT_DISABLE, UNIT_RELOAD };
enum { UNIT_INACTIVE, UNIT_ACTIVE, UNIT_FAILED, UNIT_RESTARTING };
struct unit_info {
    char name[28];
    unsigned state, enabled, pid;
    int exit_code;
    unsigned restarts;
};
int unitctl(unsigned op, const char *name, struct unit_info *out, unsigned index);

/* ---- разное ---- */
void     srand(unsigned seed);
unsigned rand(void);

#endif
