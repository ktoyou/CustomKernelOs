#ifndef SYS_WAIT_H
#define SYS_WAIT_H
#include "sys/types.h"

#ifndef WNOHANG
#define WNOHANG 1
#endif

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)    (((s) >> 8) & 0xff)

pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);
#endif
