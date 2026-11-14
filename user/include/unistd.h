#ifndef UNISTD_H
#define UNISTD_H
#include "sys/types.h"
#include "myos.h"
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char *path, int mode);
int rmdir(const char *path);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int fsync(int fd);
int fdatasync(int fd);
int ftruncate(int fd, off_t length);
int getpagesize(void);
int gethostname(char *name, size_t len);
char *getpass(const char *prompt);
pid_t getpgid(pid_t pid);
pid_t setsid(void);
void _exit(int status) __attribute__((noreturn));
pid_t getpgrp(void);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
int chown(const char *path,uid_t uid,gid_t gid);
int fchown(int fd,uid_t uid,gid_t gid);
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);
int pledge(const char *promises, const char *execpromises);
int isatty(int fd);
int pipe(int fds[2]);
int dup(int fd);
char *ttyname(int fd);
int symlink(const char *target, const char *linkpath);
int link(const char *oldpath, const char *newpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg0, ...);
int execlp(const char *file, const char *arg0, ...);
extern char **environ;
unsigned sleep(unsigned seconds);
long syscall(long n, ...);
unsigned alarm(unsigned seconds);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
#endif
