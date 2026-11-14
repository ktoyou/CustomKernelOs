#ifndef SIGNAL_H
#define SIGNAL_H
/* Ядро не доставляет сигналы: Ctrl+C убивает группу переднего плана.
 * signal() только запоминает обработчик, чтобы интерпретатор Lua собрался. */
typedef int sig_atomic_t;
#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS  7
#define SIGFPE  8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGWINCH 28
#define NSIG 64
extern char *sys_siglist[NSIG];
#define SIG_ERR ((void (*)(int))-1)
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
typedef int sigset_t;
void (*signal(int sig, void (*handler)(int)))(int);
int raise(int sig);
#define SA_NOCLDSTOP 1
#define SA_RESTART 0x10000000
struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_sigaction)(int, void *, void *);
};
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

#define SIG_BLOCK   2
#define SIG_UNBLOCK 1
#define SIG_SETMASK 3
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigblock(int mask);
int sigsetmask(int mask);
int siginterrupt(int sig, int flag);
#endif
