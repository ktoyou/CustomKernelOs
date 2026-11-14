/* stdio, POSIX open и %lld на хосте (cc -linux). */
#include <myos.h>
#include <stdio.h>
#include <fcntl.h>

int main(void) {
    int fails = 0;
    const char *path = "build/myos-stdio-test.txt";
    FILE *f = fopen(path, "w");
    if (!f) { printf("fopen write failed\n"); return 1; }

    long long sum = 0;
    for (int i = 0; i < 1000; i++) sum += i;
    if (fprintf(f, "%lld\n", sum) < 0) fails++;
    if (fclose(f) != 0) fails++;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("open failed\n"); return 1; }
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) { printf("read failed\n"); return 1; }
    buf[n] = 0;
    close(fd);
    if (strcmp(buf, "499500\n") != 0) { printf("got [%s]\n", buf); fails++; }

    f = fopen(path, "r");
    if (!f) { printf("fopen read failed\n"); return 1; }
    if (!fgets(buf, (int)sizeof(buf), f)) fails++;
    if (strcmp(buf, "499500\n") != 0) { printf("fgets [%s]\n", buf); fails++; }
    if (fseek(f, 0, SEEK_SET) != 0) fails++;
    if (fgetc(f) != '4') fails++;
    if (fclose(f) != 0) fails++;
    unlink(path);
    {
        char *mem = NULL;
        size_t len = 0;
        FILE *ms = open_memstream(&mem, &len);
        if (!ms || fprintf(ms, "ab%d", 7) < 0 || fflush(ms) != 0 || len != 3 || strcmp(mem, "ab7") != 0) {
            printf("memstream early [%s] %u\n", mem ? mem : "", (unsigned)len);
            fails++;
        }
        if (ms && (fputc('!', ms) == EOF || fclose(ms) != 0 || !mem || strcmp(mem, "ab7!") != 0)) {
            printf("memstream [%s]\n", mem ? mem : "");
            fails++;
        }
        free(mem);
    }
    if (!fails) printf("stdio: ok\n");
    return fails != 0;
}
