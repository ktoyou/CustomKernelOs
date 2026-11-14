#ifndef FCNTL_H
#define FCNTL_H
#include "myos.h"

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define FD_CLOEXEC 1
#define O_NONBLOCK 04000
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif
struct flock {
    short l_type;
    short l_whence;
    long l_start;
    long l_len;
    int l_pid;
};

int fcntl(int fd, int cmd, ...);
int flock(int fd, int op);
#endif
