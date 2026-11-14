#ifndef MYFS_H
#define MYFS_H
#include "fs.h"
void myfs_select(int slot, int device);
int  myfs_init(bool *formatted, bool allow_format); /* private VFS backend */
int  myfs_format(void);
#ifdef HOST_TEST
int  myfs_format_legacy(void);
#endif
bool myfs_ready(void);

int  myfs_resolve(u32 cwd, const char *path, u32 *ino);
int  myfs_create(u32 cwd, const char *path, int type, u32 *ino);
int  myfs_remove(u32 cwd, const char *path);
int myfs_remove_keep(u32 cwd,const char *path,bool keep);
int myfs_reap_orphan(u32 ino);
int  myfs_rename(u32 cwd, const char *src, const char *dst);

int  myfs_read(u32 ino, u32 off, void *buf, u32 n);         /* -> прочитано байт */
int  myfs_write(u32 ino, u32 off, const void *buf, u32 n);  /* -> записано байт */
int  myfs_truncate(u32 ino);
int  myfs_set_size(u32 ino, u32 size);
int  myfs_stat(u32 ino, struct fs_stat *st);
int myfs_setattr(u32 ino,u32 mode,u32 uid,u32 gid);
int myfs_touch(u32 ino);
int myfs_set_time(u32 ino, u32 when);

int  myfs_readdir(u32 dir, u32 *pos, struct fs_dirent *out); /* 1 = есть запись, 0 = конец */
int  myfs_getpath(u32 ino, char *buf, u32 len);
void myfs_info(struct fs_info *info);

const char *myfs_strerror(int err);


#endif


