/* time команда [аргументы] — запустить программу и сказать, сколько она работала */
#include "myos.h"
#include "sys/wait.h"

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: time command [args...]\n"); return 2; }
    char path[128];
    unsigned start = ticks();
    int pid = fork();
    if (pid == 0) {
        int r;
        if (strchr(argv[1], '/')) {
            snprintf(path, sizeof(path), "%s", argv[1]);
            r = exec(path, argv + 1);
        } else {
            snprintf(path, sizeof(path), "/bin/%s", argv[1]);
            r = exec(path, argv + 1);
            if (r == -ENOENT) {
                snprintf(path, sizeof(path), "/ext/bin/%s", argv[1]);
                r = exec(path, argv + 1);
            }
        }
        dprintf(STDERR, "time: %s: %s\n", argv[1], strerror(-r));
        exit(127);
    }
    if (pid < 0) { dprintf(STDERR, "time: fork: %s\n", strerror(-pid)); return 1; }
    int status = 0;
    waitpid(pid, &status, 0);
    unsigned ms = ticks() - start;
    dprintf(STDERR, "real %u.%03u s\n", ms / 1000, ms % 1000);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
}
