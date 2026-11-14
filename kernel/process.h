#ifndef PROCESS_H
#define PROCESS_H
#include "types.h"
#include "idt.h"

#include "paging.h" /* shared process stack and guard boundaries */
#define PROC_MAX_ARGS    1024           /* ar libgit.a передаёт сразу все объекты */
#define PROC_ARG_MAX     (64 * 1024)    /* суммарный текст argv и envp */

enum {
    PROC_ENOENT  = -1,
    PROC_ENOEXEC = -2,
    PROC_ENOMEM  = -3,
    PROC_EAGAIN  = -4,
    PROC_EISDIR  = -5,
    PROC_EBUSY   = -6,
    PROC_EACCES  = -7,
    PROC_ELOOP   = -8,
};

/* Загрузить ELF-файл и запустить его как процесс. -> pid или PROC_E* */
int  proc_spawn(const char *path, u32 cwd, int argc, char **argv, bool foreground);
const char *proc_strerror(int err);

struct file;
int proc_spawn_child(const char *path, int argc, char **argv, char **env, struct file *input);
int proc_spawn_service(const char *path, int argc, char **argv, struct file *log, struct file *input, bool root);

int  proc_fork(struct regs *r);         /* -> id потомка или PROC_E* */
int  proc_exec(struct regs *r, const char *path, int argc, char **argv, char **env);

/* группа процессов «на переднем плане» — её убивает Ctrl+C */
void proc_set_foreground(u32 pgid);
u32  proc_foreground(void);
bool proc_ctrl_c(void);                 /* из обработчика клавиатуры; true = кого-то убили */

#endif
