#ifndef WCHAR_H
#define WCHAR_H
#include "stddef.h"
int mbtowc(wchar_t *wc, const char *s, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
#ifndef MB_CUR_MAX
#define MB_CUR_MAX 1
#endif
#endif
