/* Открытые файлы: консоль, файлы на диске и каналы (pipe). */
#include "file.h"
#include "net.h"
#include "heap.h"
#include "fs.h"
#include "task.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"
#include "syscall.h"
#include "tty.h"
static int console_readable(void);
#include "ssh-local.inc"

static struct file *disk_files;

struct file console_file = { .type = FILE_CONSOLE, .refs = 1, .mode = 2 };   /* не освобождается */

struct file *file_dup(struct file *f) {
    if (f && f != &console_file) {
        u32 fl = irq_save();
        f->refs++;
        irq_restore(fl);
    }
    return f;
}

/* Можно звать и из обработчика прерывания (Ctrl+C убивает процесс —
 * его файлы закрываются), поэтому всё под irq_save. */
void file_close(struct file *f) {
    if (!f || f == &console_file) return;
    u32 fl = irq_save();
    struct sock *to_close = NULL;
    if (--f->refs == 0) {
        if(f->type==FILE_DISK){struct file **p=&disk_files;while(*p && *p!=f)p=&(*p)->disk_next;if(*p)*p=f->disk_next;}
        if (f->type == FILE_SOCKET) to_close = f->sock;
        if (f->type == FILE_LOCAL) local_close(f);
        if (f->type == FILE_PIPE) {
            struct pipe *p = f->pipe;
            if (f->mode == 0) p->readers--; else p->writers--;
            task_wakeup(p);                 /* разбудить ждущих: конец канала исчез */
            if (!p->readers && !p->writers) kfree(p);
        }
        if (f->type == FILE_PTY_MASTER || f->type == FILE_PTY_SLAVE) pty_close(f);
        kfree(f);
    }
    irq_restore(fl);
    if (to_close) sock_close(to_close);         /* берёт мьютекс — уже с разрешёнными прерываниями */
}

struct file *file_open_socket(struct sock *s) {
    struct file *f = kcalloc(1, sizeof(*f));
    if (!f) return NULL;
    f->type = FILE_SOCKET;
    f->refs = 1;
    f->mode = 2;
    f->sock = s;
    return f;
}

struct file *file_open_disk(u32 ino, u8 mode, u32 off) {
    struct file *f = kcalloc(1, sizeof(*f));
    if (!f) return NULL;
    f->type = FILE_DISK;
    f->refs = 1;
    f->mode = mode;
    f->ino  = ino;
    f->off  = off;
    u32 flags=irq_save();f->disk_next=disk_files;disk_files=f;irq_restore(flags);
    return f;
}

bool file_disk_busy(u32 ino) {
    u32 flags=irq_save();bool busy=false;for(struct file *f=disk_files;f;f=f->disk_next)if(f->ino==ino){busy=true;break;}
    irq_restore(flags);return busy;
}
bool file_disk_volume_busy(u32 slot) {
    u32 flags=irq_save();bool busy=false;for(struct file *f=disk_files;f;f=f->disk_next)if((f->ino>>24)==slot){busy=true;break;}
    irq_restore(flags);return busy;
}

int pipe_create(struct file **rd, struct file **wr) {
    struct pipe *p = kcalloc(1, sizeof(*p));
    struct file *r = kcalloc(1, sizeof(*r)), *w = kcalloc(1, sizeof(*w));
    if (!p || !r || !w) { kfree(p); kfree(r); kfree(w); return -1; }
    p->readers = p->writers = 1;
    r->type = w->type = FILE_PIPE;
    r->refs = w->refs = 1;
    r->pipe = w->pipe = p;
    r->mode = 0;
    w->mode = 1;
    *rd = r;
    *wr = w;
    return 0;
}

/* ---------------------------------------------------------------- консоль */

static int console_write(const char *buf, u32 len, bool err) {
    for (u32 done=0;done<len;) {
        u32 count=len-done;
        if (count>256) count=256;
        u32 f=irq_save();
        if (err && !done) vga_set_color(LIGHT_RED, BLACK);
        vga_write(buf+done,count);
        if (err && done+count==len) vga_set_color(LIGHT_GREY, BLACK);
        irq_restore(f);
        done+=count;
        /* Syscall entry has IF=0. Merely restoring flags cannot let the USB
         * worker or timer run; yield at a completed console batch instead. */
        if (done<len) task_yield();
    }
    return len;
}

/* ---- режимы терминала ----
 * Обычный («канонический») режим: ядро само собирает строку с эхом и Backspace.
 * Сырой режим (TTY_RAW): каждая клавиша сразу отдаётся программе, без эха;
 * стрелки и прочие особые клавиши — escape-последовательностями, как в xterm.
 * Режим принадлежит процессу, который его включил: когда тот умрёт, ядро
 * вернёт терминал в нормальное состояние (иначе упавшая игра «сломала» бы консоль). */
static u32 tty_mode, tty_owner;

u32 console_get_mode(void) {return tty_mode;}

u32 console_set_mode(u32 mode) {
    u32 old = tty_mode;
    tty_mode = mode;
    tty_owner = mode ? task_current()->id : 0;
    return old;
}

bool console_isig(void) { return !(tty_mode & TTY_NOSIG); }

void console_release(u32 pid) {
    if (tty_owner && tty_owner == pid) {
        tty_mode = 0;
        tty_owner = 0;
        vga_reset_terminal();
    }
}

static u8  pend[8];
static u32 pend_n, pend_pos;
static int console_readable(void){return pend_pos<pend_n || kb_has_key();}

static void translate(int c) {
    const char *seq = NULL;
    /* Shift navigation and clipboard shortcuts; private CSI finals are
     * decoded by readkey without changing the Unicode key representation. */
    if (c >= 0x90 && c <= 0x9B) {
        pend_pos = 0; pend_n = 3;
        pend[0] = 27; pend[1] = '['; pend[2] = 'a' + c - 0x90;
        return;
    }
    if (c >= KEY_UNI) {                                      /* буква не латиницей — в UTF-8 */
        u32 u = c - KEY_UNI;
        pend_pos = 0;
        if (u < 0x800) { pend[0] = 0xC0 | u >> 6; pend[1] = 0x80 | (u & 0x3F); pend_n = 2; }
        else { pend[0] = 0xE0 | u >> 12; pend[1] = 0x80 | (u >> 6 & 0x3F); pend[2] = 0x80 | (u & 0x3F); pend_n = 3; }
        return;
    }
    switch (c) {
    case KEY_UP:    seq = "\x1b[A";  break;
    case KEY_DOWN:  seq = "\x1b[B";  break;
    case KEY_RIGHT: seq = "\x1b[C";  break;
    case KEY_LEFT:  seq = "\x1b[D";  break;
    case KEY_HOME:  seq = "\x1b[H";  break;
    case KEY_END:   seq = "\x1b[F";  break;
    case KEY_PGUP:  seq = "\x1b[5~"; break;
    case KEY_PGDN:  seq = "\x1b[6~"; break;
    case KEY_DEL:   seq = "\x1b[3~"; break;
    case KEY_INS:   seq = "\x1b[2~"; break;
    }
    pend_pos = 0;
    if (seq) { pend_n = strlen(seq); memcpy(pend, seq, pend_n); }
    else { pend[0] = (c == '\n') ? '\r' : c; pend_n = 1; }   /* Enter в сыром режиме = \r */
}

static int console_read_raw(u8 *buf, u32 len) {
    u32 n = 0;
    while (n < len) {
        if (pend_pos < pend_n) { buf[n++] = pend[pend_pos++]; continue; }
        int c = kb_poll();
        if (c < 0) {
            if (n || (tty_mode & TTY_NONBLOCK)) break;   /* что было — отдаём сразу */
            c = kb_getchar();                            /* ждём хотя бы одну клавишу */
            if (task_current()->kill_pending) return -EINTR;
        }
        translate(c);
    }
    return n;
}

/* «Канонический» режим терминала: строка с эхом и Backspace, до Enter.
 * Ctrl+D в начале строки — конец ввода (0). Ctrl+C — бросить строку. */
static int console_read(char *buf, u32 len) {
    if (tty_mode & TTY_RAW) return console_read_raw((u8 *)buf, len);
    u32 n = 0;
    while (n < len) {
        int c = kb_getchar();
        if (task_current()->kill_pending) return -EINTR;
        if (c == KEY_CTRL_D) { if (n == 0) return 0; break; }
        if (c == KEY_CTRL_C) {
            vga_puts("^C\n");
            buf[0] = '\n';
            return 1;
        }
        if (c == '\b') {
            if (n) {
                while (n > 1 && ((u8)buf[n - 1] & 0xC0) == 0x80) n--;   /* буква UTF-8 целиком */
                n--;
                vga_putc('\b');
            }
            continue;
        }
        if (c >= KEY_UNI) {                           /* в строку — UTF-8; шрифт консоли кириллицы не знает: эхо «?» */
            u32 u = c - KEY_UNI;
            if (n + 3 > len) continue;
            if (u < 0x800) { buf[n++] = 0xC0 | u >> 6; buf[n++] = 0x80 | (u & 0x3F); }
            else { buf[n++] = 0xE0 | u >> 12; buf[n++] = 0x80 | (u >> 6 & 0x3F); buf[n++] = 0x80 | (u & 0x3F); }
            vga_putc('?');
            continue;
        }
        if (c >= 0x80) continue;                      /* стрелки и т.п. в обычном режиме не нужны */
        if (c == '\n') { buf[n++] = '\n'; vga_putc('\n'); break; }
        if ((u8)c < 32 || (u8)c >= 127) continue;
        buf[n++] = c;
        vga_putc(c);
    }
    return n;
}

/* ---------------------------------------------------------------- канал
 *
 * Кольцевой буфер. Читатель спит, пока в канале пусто (и есть писатели);
 * писатель спит, пока полно (и есть читатели). Все проверки — с запрещёнными
 * прерываниями, иначе пробуждение может прийти между проверкой и сном. */

static int pipe_read(struct pipe *p, u8 *buf, u32 len) {
    u32 f = irq_save();
    while (p->count == 0) {
        if (p->writers == 0) { irq_restore(f); return 0; }        /* конец файла */
        task_wait(p);
        if (task_current()->kill_pending) { irq_restore(f); return -EINTR; }
    }
    u32 n = len < p->count ? len : p->count;
    for (u32 i = 0; i < n; i++) {
        buf[i] = p->buf[p->rpos];
        p->rpos = (p->rpos + 1) % PIPE_SIZE;
    }
    p->count -= n;
    task_wakeup(p);                                               /* место освободилось */
    irq_restore(f);
    return n;
}

static int pipe_write(struct pipe *p, const u8 *buf, u32 len) {
    u32 f = irq_save();
    u32 done = 0;
    while (done < len) {
        if (p->readers == 0) { irq_restore(f); return done ? (int)done : -EPIPE; }
        if (p->count == PIPE_SIZE) {
            task_wait(p);
            if (task_current()->kill_pending) { irq_restore(f); return -EINTR; }
            continue;
        }
        while (done < len && p->count < PIPE_SIZE) {
            p->buf[(p->rpos + p->count) % PIPE_SIZE] = buf[done++];
            p->count++;
        }
        task_wakeup(p);                                           /* есть что читать */
    }
    irq_restore(f);
    return done;
}

/* ---------------------------------------------------------------- общий ввод-вывод */

int file_read(struct file *f, void *buf, u32 len) {
    if (!len) return 0;
    if ((f->status_flags & 04000) && !(file_ready(f,1)&(1|16))) return -EAGAIN;
    switch (f->type) {
    case FILE_CONSOLE: return console_read(buf, len);
    case FILE_LOCAL: return pipe_read(&f->local->p[f->mode], buf, len);
    case FILE_PIPE:    return f->mode == 0 ? pipe_read(f->pipe, buf, len) : -EBADF;
    case FILE_SOCKET:  return sock_read(f->sock, buf, len);
    case FILE_PTY_MASTER: case FILE_PTY_SLAVE: return pty_read(f, buf, len);
    case FILE_DISK: {
        if (!(f->mode & FMODE_READ)) return -EBADF;
        int r = fs_read(f->ino, f->off, buf, len);
        if (r > 0) f->off += r;
        return r < 0 ? -EIO : r;
    }
    }
    return -EBADF;
}

int file_write(struct file *f, const void *buf, u32 len, bool is_stderr) {
    if (!len) return 0;
    if (f->status_flags & 04000) {
        if (!(file_ready(f,4)&(4|8|16))) return -EAGAIN;
        u32 room=len;
        if(f->type==FILE_LOCAL) room=PIPE_SIZE-f->local->p[1-f->mode].count;
        if(f->type==FILE_PIPE) room=PIPE_SIZE-f->pipe->count;
        if(f->type==FILE_SOCKET) room=SOCK_TXBUF-f->sock->tx_len;
        if(f->type==FILE_PTY_MASTER) room=TTY_BUF-f->tty->in.count;
        if(f->type==FILE_PTY_SLAVE) room=TTY_BUF-f->tty->out.count;
        if(!room) return -EAGAIN;
        if(len>room) len=room;
    }
    switch (f->type) {
    case FILE_CONSOLE: return console_write(buf, len, is_stderr);
    case FILE_LOCAL: return pipe_write(&f->local->p[1-f->mode], buf, len);
    case FILE_PIPE:    return f->mode ? pipe_write(f->pipe, buf, len) : -EBADF;
    case FILE_SOCKET:  return sock_write(f->sock, buf, len);
    case FILE_PTY_MASTER: case FILE_PTY_SLAVE: return pty_write(f, buf, len);
    case FILE_DISK: {
        if (!(f->mode & FMODE_WRITE)) return -EBADF;
        if (f->mode & FMODE_APPEND) {
            struct fs_stat st;
            if (fs_stat(f->ino, &st)) return -EIO;
            f->off = st.size;
        }
        int r = fs_write(f->ino, f->off, buf, len);
        if (r > 0) f->off += r;
        return r == FS_ENOSPC ? -ENOSPC : r == FS_EFBIG ? -EFBIG : r < 0 ? -EIO : r;
    }
    }
    return -EBADF;
}

int file_pread(struct file *f, void *buf, u32 len, u32 off) {
    if (!len) return 0;
    if (f->type != FILE_DISK) return -ESPIPE;
    if (!(f->mode & FMODE_READ)) return -EBADF;
    int r = fs_read(f->ino, off, buf, len);
    return r < 0 ? -EIO : r;
}

int file_pwrite(struct file *f, const void *buf, u32 len, u32 off) {
    if (!len) return 0;
    if (f->type != FILE_DISK) return -ESPIPE;
    if (!(f->mode & FMODE_WRITE)) return -EBADF;
    int r = fs_write(f->ino, off, buf, len);
    return r == FS_ENOSPC ? -ENOSPC : r == FS_EFBIG ? -EFBIG : r < 0 ? -EIO : r;
}

int file_lseek(struct file *f, int off, int whence) {
    if (f->type != FILE_DISK) return -ESPIPE;
    struct fs_stat st;
    if (fs_stat(f->ino, &st)) return -EIO;
    int base = whence == 0 ? 0 : whence == 1 ? (int)f->off : whence == 2 ? (int)st.size : -1;
    if (base < 0) return -EINVAL;
    int next = base + off;
    if (next < 0) return -EINVAL;
    f->off = (u32)next;
    return next;
}
