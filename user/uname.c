/* uname: имя системы. Git смотрит на -s и не должен принять MyOS за Linux. */
#include "myos.h"
#include "stdio.h"
#include "string.h"
#include "sys/utsname.h"

static void put(int *first, const char *s) {
    if (!*first) printf(" ");
    *first = 0;
    printf("%s", s);
}

int main(int argc, char **argv) {
    struct utsname u;
    int s = 0, n = 0, r = 0, v = 0, m = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') {
            fprintf(stderr, "uname: лишний аргумент %s\n", a);
            return 1;
        }
        if (a[1] == '-') {
            if (!strcmp(a, "--kernel-name") || !strcmp(a, "--sysname")) s = 1;
            else if (!strcmp(a, "--nodename")) n = 1;
            else if (!strcmp(a, "--kernel-release") || !strcmp(a, "--release")) r = 1;
            else if (!strcmp(a, "--kernel-version")) v = 1;
            else if (!strcmp(a, "--machine")) m = 1;
            else if (!strcmp(a, "--all")) s = n = r = v = m = 1;
            else { fprintf(stderr, "uname: неизвестный параметр %s\n", a); return 1; }
        } else {
            for (int j = 1; a[j]; j++) {
                if (a[j] == 's') s = 1;
                else if (a[j] == 'n') n = 1;
                else if (a[j] == 'r') r = 1;
                else if (a[j] == 'v') v = 1;
                else if (a[j] == 'm') m = 1;
                else if (a[j] == 'a') s = n = r = v = m = 1;
                else { fprintf(stderr, "uname: неизвестный параметр -%c\n", a[j]); return 1; }
            }
        }
        any = 1;
    }
    if (!any) s = 1;
    if (uname(&u) < 0) return 1;
    int first = 1;
    if (s) put(&first, u.sysname);
    if (n) put(&first, u.nodename);
    if (r) put(&first, u.release);
    if (v) put(&first, u.version);
    if (m) put(&first, u.machine);
    printf("\n");
    return 0;
}
