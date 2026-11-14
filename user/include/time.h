#ifndef TIME_H
#define TIME_H
#include "sys/types.h"

typedef long clock_t;
#define CLOCKS_PER_SEC 1000

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
int clock_gettime(int clock_id, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
time_t time(time_t *tloc);
double difftime(time_t a, time_t b);
clock_t clock(void);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
char *ctime_r(const time_t *t, char *buf);
time_t mktime(struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
#endif
