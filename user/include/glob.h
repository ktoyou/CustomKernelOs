#ifndef GLOB_H
#define GLOB_H
#include "stddef.h"

#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *pglob);
void globfree(glob_t *pglob);
#endif
