#ifndef STDIO_H
#define STDIO_H
#include "stdarg.h"
#include "sys/types.h"

#define EOF (-1)
#define BUFSIZ 512
#define L_tmpnam 64
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define getc(f) fgetc(f)
#define putc(c, f) fputc((c), (f))

typedef struct FILE {
    int fd;
    int flags;
    char *mem;          /* буфер fmemopen или open_memstream, иначе NULL */
    int mem_pos, mem_len, mem_cap;
    char **ms_buf;      /* open_memstream: куда отдать буфер */
    size_t *ms_size;    /* длина без завершающего нуля */
} FILE;

extern FILE *stdin, *stdout, *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
FILE *tmpfile(void);
char *tmpnam(char *s);
FILE *fdopen(int fd, const char *mode);
FILE *fmemopen(void *buf, size_t size, const char *mode);
FILE *open_memstream(char **ptr, size_t *sizeloc);
int fclose(FILE *f);
int fflush(FILE *f);
int fileno(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
void perror(const char *s);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
int fseeko(FILE *f, off_t off, int whence);
off_t ftello(FILE *f);
void rewind(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fgetc(FILE *f);
int fputc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputs(const char *s, FILE *f);
int ungetc(int c, FILE *f);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int fprintf(FILE *f, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
int fscanf(FILE *f, const char *fmt, ...);
int scanf(const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
ssize_t getline(char **line, size_t *n, FILE *stream);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
int rename(const char *old, const char *new);
int remove(const char *path);
#endif
