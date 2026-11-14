#ifndef GRP_H
#define GRP_H
#include "stddef.h"
#include "sys/types.h"
struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};
struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);
int setgroups(size_t size, const gid_t *list);
int initgroups(const char *user, gid_t group);
#endif
