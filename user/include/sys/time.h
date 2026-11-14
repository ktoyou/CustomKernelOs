#ifndef SYS_TIME_H
#define SYS_TIME_H
#include <time.h>
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2
int gettimeofday(struct timeval *tv, void *tz);
int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value);
int getitimer(int which, struct itimerval *curr_value);
#endif
