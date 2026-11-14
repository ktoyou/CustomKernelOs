/* strtol, qsort, getenv и соседние функции из issue #2. */
#include <myos.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#include <glob.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    char *end;
    const char *err = NULL;
    int nums[6] = { 4, 1, 4, -2, 9, 0 };
    int i;
    char *copy;
    time_t now;
    int fd;

    CHECK(strtol("  -42", &end, 10) == -42 && *end == 0);
    CHECK(strtol("0x10", NULL, 0) == 16);
    CHECK(strtol("075", NULL, 0) == 61);
    CHECK(strtol("99abc", &end, 10) == 99 && *end == 'a');
    errno = 0;
    CHECK(strtol("999999999999", NULL, 10) == LONG_MAX && errno == ERANGE);
    CHECK(strtoll("10000000000", NULL, 10) == 10000000000LL);
    CHECK(strtoul("-1", NULL, 10) == 4294967295UL);
    CHECK(strtoull("ff", NULL, 16) == 255);

    err = NULL;
    CHECK(strtonum("2", 1, 10, &err) == 2 && err == NULL);
    CHECK(strtonum("0", 1, 10, &err) == 0 && err && err[0] == 't');
    CHECK(strtonum("12x", 0, 20, &err) == 0 && err && err[0] == 'i');

    copy = strdup("myos");
    CHECK(copy && strcmp(copy, "myos") == 0);
    free(copy);

    qsort(nums, 6, sizeof(int), cmp_int);
    for (i = 1; i < 6; i++) CHECK(nums[i - 1] <= nums[i]);
    CHECK(nums[0] == -2 && nums[5] == 9);

    CHECK(getenv("HOME") == NULL);
    CHECK(getenv("POSIXLY_CORRECT") == NULL);
#ifdef __LINUX_TEST__
    CHECK(getuid() == (uid_t)__syscall(199) && geteuid() == (uid_t)__syscall(201) && getgid() == (gid_t)__syscall(200));
#else
    CHECK(geteuid() == (uid_t)account(4,NULL,NULL));
#endif
    CHECK(setvbuf(stdout, NULL, _IONBF, 0) == 0);
    CHECK(sleep(0) == 0);

    now = time(NULL);
    CHECK(now > 1700000000 && now < 2100000000);

    fd = open("build/posix-notty.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0 && isatty(fd) == 0);
    if (fd >= 0) close(fd);
    unlink("build/posix-notty.txt");
    CHECK(isatty(-1) == 0);

    CHECK(strtof("1.5", &end) == 1.5f && end && *end == 0);
    CHECK(strtold("2.5", &end) == 2.5L && end && *end == 0);
    {
        char stamp[32];
        now = 1700000000;
        CHECK(ctime_r(&now, stamp) == stamp && stamp[24] == '\n');
    }
    {
        struct rusage ru;
        CHECK(getrusage(RUSAGE_SELF, &ru) == 0);
    }
    errno = 0;
    CHECK(mkfifo("/tmp/myos-ext-mkfifo", 0600) == -1 && errno == ENOSYS);
    {
        glob_t g;
        memset(&g, 0, sizeof g);
        CHECK(glob("tests/cc/posix.c", 0, NULL, &g) == 0 && g.gl_pathc == 1);
        globfree(&g);
        CHECK(glob("tests/cc/no-such-posix", 0, NULL, &g) == GLOB_NOMATCH && g.gl_pathc == 0);
        CHECK(glob("tests/cc/posix.*", 0, NULL, &g) == 0 && g.gl_pathc >= 1);
        globfree(&g);
    }
#ifdef __LINUX_TEST__
    {
        pid_t pid;
        int st = 1;
        char *av[] = { "true", NULL };
        CHECK(posix_spawnp(&pid, "true", NULL, NULL, av, environ) == 0);
        CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
#endif

    return fails != 0;
}
