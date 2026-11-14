#ifndef FILE_H
#define FILE_H
#include "types.h"

/* Открытый файл — объект ядра. Дескриптор (0, 1, 2, ...) в процессе — это
 * просто индекс в его таблице указателей на такие объекты. После fork() или
 * dup2() несколько дескрипторов (даже в разных процессах) указывают на ОДИН
 * объект: у них общее смещение, а закрыт он будет, когда уйдёт последняя ссылка. */

enum file_type { FILE_CONSOLE, FILE_DISK, FILE_PIPE, FILE_SOCKET, FILE_PTY_MASTER, FILE_PTY_SLAVE, FILE_LOCAL };

struct sock;
struct tty;
struct local_pair;

#define PIPE_SIZE 4096

struct pipe {
    u8  buf[PIPE_SIZE];
    u32 rpos, count;
    u32 readers, writers;           /* сколько открытых концов осталось */
};

struct file {
    enum file_type type;
    u32  refs;
    u32 status_flags;
    struct local_pair *local;
    u8   mode;                      /* диск: биты FMODE_*; канал: 0 чтение, иначе запись */
    struct file *disk_next;
    u32  ino, off;                  /* FILE_DISK */
    struct pipe *pipe;              /* FILE_PIPE */
    struct sock *sock;              /* FILE_SOCKET */
    struct tty *tty;                /* FILE_PTY_* */
};

int local_pair_create(struct file **a, struct file **b);
int local_sendfd(struct file *f, struct file *passed);
struct file *local_recvfd(struct file *f, int *err);
int file_ready(struct file *f, int events);

extern struct file console_file;    /* клавиатура + экран: stdin/stdout/stderr */

#define TTY_RAW      1                 /* клавиши сразу, без эха и без сборки строки */
#define TTY_NONBLOCK 2                 /* read не ждёт: нет клавиши — вернёт 0 */
#define TTY_RESIZE_EVENT 8             /* readkey-only: resize interrupts the pending read. */
#define TTY_NOSIG    4                 /* Ctrl+C — обычная клавиша (код 3), а не убийство */

u32 console_get_mode(void);
u32  console_set_mode(u32 mode);       /* -> прежний режим */
bool console_isig(void);
void console_release(u32 pid);         /* процесс умер — вернуть терминал в норму */

struct file *file_dup(struct file *f);
void  file_close(struct file *f);
struct file *file_open_disk(u32 ino, u8 mode, u32 off);
bool file_disk_busy(u32 ino);
bool file_disk_volume_busy(u32 slot);
struct file *file_open_socket(struct sock *s);
int   pipe_create(struct file **rd, struct file **wr);

/* buf — указатель в памяти текущего процесса (уже проверенный) */
int   file_read(struct file *f, void *buf, u32 len);
int   file_write(struct file *f, const void *buf, u32 len, bool is_stderr);
int   file_pread(struct file *f, void *buf, u32 len, u32 off);
int   file_pwrite(struct file *f, const void *buf, u32 len, u32 off);
int   file_lseek(struct file *f, int off, int whence);   /* -> новое смещение или -errno */

#define FMODE_READ   1
#define FMODE_WRITE  2
#define FMODE_APPEND 4

#endif
