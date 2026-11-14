#ifndef FS_H
#define FS_H
#include "types.h"

/* MyFS — простая файловая система в духе классической Unix FS.
 *
 * Раскладка диска (блок = 512 байт = 1 сектор):
 *   [0]            суперблок
 *   [1..B]         битовая карта занятых блоков (1 бит на блок)
 *   [B+1..B+I]     таблица inode (по 8 штук на блок)
 *   [...]          блоки данных
 *
 * inode 0 не используется (0 = «нет inode»), корневой каталог — inode 1.
 * Каталог версии 1 — записи по 32 байта, имя до 27 символов.
 * Каталог версии 2 — запись переменной длины, кратная 4 байтам:
 * inode, длина записи, длина имени и само имя до 255 символов.
 * format() и mkfs пишут версию 3 с отдельной таблицей прав; данные v1/v2 читаются
 * без перемещения inode и блоков. V3 сохраняет исходную раскладку в суперблоке.
 * Карта блоков версии 1 — 12 прямых, один одинарный и один двойной косвенный
 * (файл до ~8 МиБ). У версии 2 последний прямой слот занят тройным косвенным
 * блоком, поэтому файл может вырасти до свободного места на томе.
 * Символическая ссылка — отдельный тип inode: в теле лежит текст цели. */

#define FS_ROOT       1
#define FS_NAME_MAX   255
#define FS_PATH_MAX   1024
/* 8 GiB at 512-byte sectors. The in-memory bitmap for this size is 2 MiB. */
#define FS_MAX_BLOCKS (16u * 1024u * 1024u)

enum { FS_TYPE_FREE = 0, FS_TYPE_FILE = 1, FS_TYPE_DIR = 2, FS_TYPE_BLOCK = 3, FS_TYPE_CHAR = 4, FS_TYPE_LINK = 5 };

enum {
    FS_OK        =  0,
    FS_ENOENT    = -1,
    FS_EEXIST    = -2,
    FS_ENOTDIR   = -3,
    FS_EISDIR    = -4,
    FS_ENOSPC    = -5,
    FS_ENOTEMPTY = -6,
    FS_EINVAL    = -7,
    FS_EIO       = -8,
    FS_ENODEV    = -9,
    FS_EFBIG     = -10,
    FS_ENAMETOOLONG = -11,
    FS_EROFS = -12,
    FS_EBUSY = -13,
    FS_EXDEV = -14,
    FS_ELOOP = -15,
    FS_EACCES = -16,
    FS_EPERM = -17,
};

/* VFS: up to four MyFS volumes; /dev is a virtual filesystem. */
int fs_mount(u32 cwd, const char *device, const char *target);
int fs_mount_info(u32 index, char *device, char *target);
int fs_mkfs(const char *device);
int fs_partition(const char *device,u32 op,u32 slot,u32 start,u32 sectors);
/* Keep raw-device lookup and descriptor publication atomic with admin writes. */
void fs_namespace_lock(void);
void fs_namespace_unlock(void);

struct fs_stat {
    u32 type;
    u32 size;
    u32 blocks;
    u32 mode, uid, gid, links;
    u32 atime, mtime, ctime;
};

struct fs_dirent {
    u32  ino;
    char name[FS_NAME_MAX + 1];
};

struct fs_info {
    u32 total_blocks, free_blocks;
    u32 total_inodes, free_inodes;
};

int  fs_init(bool *formatted);   /* root: raw MyFS or first valid MyFS MBR partition */
int  fs_format(void);
#ifdef HOST_TEST
int  fs_format_legacy(void);     /* том версии 1: записи по 32 байта */
#endif
bool fs_ready(void);

int  fs_resolve(u32 cwd, const char *path, u32 *ino);
int  fs_resolve_nofollow(u32 cwd, const char *path, u32 *ino);
int  fs_symlink(u32 cwd, const char *target, const char *linkpath);
int  fs_readlink(u32 cwd, const char *path, char *buf, u32 len); /* -> байт или ошибка */
int  fs_create(u32 cwd, const char *path, int type, u32 *ino);
int  fs_create_mode(u32 cwd, const char *path, int type, u32 mode, u32 *ino);
int  fs_access(u32 ino, u32 mask, bool real_ids);
int fs_access_path(u32 cwd,const char *path,u32 mask);
int  fs_chmod(u32 ino, u32 mode);
int  fs_chown(u32 ino, u32 uid, u32 gid);
int  fs_utime(u32 ino, u32 when);
int  fs_set_size(u32 ino, u32 size);
u32 fs_umask(u32 mask);
/* Kernel-only, scoped account database/home maintenance; never inherited. */
void fs_account_enter(void);
void fs_account_leave(void);
int  fs_remove(u32 cwd, const char *path);
int  fs_rename(u32 cwd, const char *src, const char *dst);

int  fs_read(u32 ino, u32 off, void *buf, u32 n);         /* -> прочитано байт */
int  fs_write(u32 ino, u32 off, const void *buf, u32 n);  /* -> записано байт */
int  fs_truncate(u32 ino);
int  fs_stat(u32 ino, struct fs_stat *st);

int  fs_readdir(u32 dir, u32 *pos, struct fs_dirent *out); /* 1 = есть запись, 0 = конец */
int  fs_getpath(u32 ino, char *buf, u32 len);
void fs_info(struct fs_info *info);

const char *fs_strerror(int err);

#endif
