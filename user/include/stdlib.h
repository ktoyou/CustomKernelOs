#ifndef STDLIB_H
#define STDLIB_H
#include "stddef.h"
#include "stdint.h"
void abort(void) __attribute__((noreturn));
int atexit(void (*fn)(void));
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
extern char *__progname;
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);
int system(const char *command);
#define RAND_MAX 32767
int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);
int abs(int x);
long labs(long x);
long long llabs(long long x);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
intmax_t strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int putenv(char *string);
int unsetenv(const char *name);
int clearenv(void);
char *mktemp(char *template);
int mkstemp(char *template);
char *mkdtemp(char *template);
char *realpath(const char *path, char *resolved);
long long strtonum(const char *nptr, long long minval, long long maxval, const char **errstr);
#endif
