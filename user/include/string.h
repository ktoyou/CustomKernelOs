#ifndef STRING_H
#define STRING_H
#include "stddef.h"
void *memchr(const void *s, int c, size_t n);
char *strncat(char *dst, const char *src, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
int strcoll(const char *a, const char *b);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
size_t strlcpy(char *dst, const char *src, size_t n);
size_t strlcat(char *dst, const char *src, size_t n);
void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);
char *strcasestr(const char *haystack, const char *needle);
char *strtok(char *s, const char *delim);
void bcopy(const void *src, void *dst, size_t n);
char *strsignal(int sig);
#endif
