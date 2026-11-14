#ifndef SPAWN_H
#define SPAWN_H
#include "sys/types.h"

typedef struct { int unused; } posix_spawn_file_actions_t;
typedef struct { int unused; } posix_spawnattr_t;

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]);
#endif
