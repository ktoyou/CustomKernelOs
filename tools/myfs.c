/* myfs — работа с диском MyFS (disk.img) прямо с компьютера, без запуска MyOS.
 *
 *   myfs disk.img ls [path]          список файлов
 *   myfs disk.img mkdir <path>       создать каталог (вместе с родителями)
 *   myfs disk.img put <file> <path>  скопировать файл компьютера на диск
 *   myfs disk.img cat <path>         вывести (текстовый) файл с диска
 *   myfs disk.img rm <path>          удалить файл; если его нет — не ошибка
 *   myfs disk.img grow <MB>          скопировать ФС в новый образ этого размера (1..8192)
 *
 * Используется ТОТ ЖЕ kernel/fs.c, что и в ядре: вместо драйвера ATA ему
 * подсовываются функции, читающие и пишущие сектора файла-образа. */
#include "fs.h"
#include "myfs.h"
#include "block.h"
#include "ata.h"
#include "task.h"
#include "string.h"

int  host_open_image(const char *path);
int  host_create_image(const char *path, unsigned sectors);
int  host_replace_image(const char *path, const char *neu);
unsigned host_sectors(void);
int  host_read_sector(unsigned lba, void *buf);
int  host_write_sector(unsigned lba, const void *buf);
void host_close_image(void);
void host_remove_file(const char *path);
void *host_load_file(const char *path, unsigned *len);
int  host_save_file(const char *path, const void *data, unsigned len);
void *host_alloc(unsigned n);
void host_free(void *p);
void host_print(const char *s);
void host_error(const char *s);
int  snprintf(char *buf, unsigned long n, const char *fmt, ...);

/* ---- «драйвер диска» для fs.c ---- */
bool ata_init(void)                   { return true; }
u32  ata_sectors(void)                { return host_sectors(); }
const char *ata_model(void)           { return "disk image"; }
int  ata_read(u32 lba, void *buf)     { return host_read_sector(lba, buf); }
int  ata_write(u32 lba, const void *buf) { return host_write_sector(lba, buf); }
void mutex_lock(struct mutex *m)      { (void)m; }
void mutex_unlock(struct mutex *m)    { (void)m; }

static char out[512];
#define say(...) (snprintf(out, sizeof(out), __VA_ARGS__), host_print(out))
#define err(...) (snprintf(out, sizeof(out), __VA_ARGS__), host_error(out))

static int mkdir_p(const char *path) {
    char buf[FS_PATH_MAX];
    u32 n = strlen(path), ino;
    if (n >= sizeof(buf)) return FS_ENAMETOOLONG;
    memcpy(buf, path, n + 1);
    for (u32 i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        char c = buf[i];
        buf[i] = 0;
        if (fs_resolve(FS_ROOT, buf, &ino) == FS_ENOENT) {
            int r = fs_create(FS_ROOT, buf, FS_TYPE_DIR, NULL);
            if (r) return r;
        }
        buf[i] = c;
    }
    return FS_OK;
}

static int cmd_ls(const char *path) {
    u32 dir, pos = 0;
    struct fs_dirent de;
    struct fs_stat st;
    int r = fs_resolve(FS_ROOT, path, &dir);
    if (r) return r;
    while (fs_readdir(dir, &pos, &de) == 1) {
        if (fs_stat(de.ino, &st)) continue;
        if (st.type == FS_TYPE_DIR) say("   <DIR>  %s/\n", de.name);
        else if (st.type == FS_TYPE_LINK) say("   <LNK>  %s\n", de.name);
        else                        say("%8u  %s\n", st.size, de.name);
    }
    return FS_OK;
}

static int cmd_put(const char *src, const char *dst) {
    unsigned len;
    unsigned char *data = host_load_file(src, &len);
    if (!data) { err("myfs: cannot read %s\n", src); return FS_EIO; }

    /* создать родительские каталоги */
    char parent[FS_PATH_MAX];
    u32 n = strlen(dst);
    if (n >= sizeof(parent)) return FS_ENAMETOOLONG;
    memcpy(parent, dst, n + 1);
    while (n && parent[n - 1] != '/') n--;
    if (n > 1) { parent[n - 1] = 0; int r = mkdir_p(parent); if (r) return r; }

    u32 ino;
    int r = fs_resolve(FS_ROOT, dst, &ino);
    if (r == FS_ENOENT) r = fs_create(FS_ROOT, dst, FS_TYPE_FILE, &ino);
    if (r) return r;
    if ((r = fs_truncate(ino))) return r;
    r = fs_write(ino, 0, data, len);
    if (r < 0) return r;
    if(r!=(int)len)return FS_EIO;
    r=fs_chmod(ino,len>=4 && !memcmp(data,"\177ELF",4)?0755:0644);
    if(r)return r;
    say("  %-24s -> %s (%u bytes)\n", src, dst, len);
    return FS_OK;
}

static int cmd_rm(const char *path) {
    int r = fs_remove(FS_ROOT, path);
    if (r == FS_ENOENT) return FS_OK;
    return r;
}

/* скопировать байты в уже открытый образ (образ должен быть отформатирован) */
static int put_bytes(const char *dst, const unsigned char *data, unsigned len) {
    char parent[FS_PATH_MAX];
    u32 n = strlen(dst);
    if (n >= sizeof(parent)) return FS_ENAMETOOLONG;
    memcpy(parent, dst, n + 1);
    while (n && parent[n - 1] != '/') n--;
    if (n > 1) { parent[n - 1] = 0; int r = mkdir_p(parent); if (r) return r; }

    u32 ino;
    int r = fs_resolve(FS_ROOT, dst, &ino);
    if (r == FS_ENOENT) r = fs_create(FS_ROOT, dst, FS_TYPE_FILE, &ino);
    if (r) return r;
    if ((r = fs_truncate(ino))) return r;
    u32 off = 0;
    while (off < len) {
        int w = fs_write(ino, off, data + off, len - off);
        if (w < 0) return w;
        if (w == 0) return FS_EIO;
        off += w;
    }
    return FS_OK;
}

/* grow: ФС не умеет расти на месте. Читаем дерево в память, форматируем новый
 * образ нужного размера и копируем файлы. Прежний образ остаётся как .bak. */
struct snap {
    char *path;
    bool dir;
    bool link;
    unsigned char *data;
    u32 len;
    u32 mode,uid,gid;
};
static struct snap *snaps;
static int n_snaps, cap_snaps;

static int snap_add(const char *path, bool dir, bool link, unsigned char *data, u32 len) {
    if (n_snaps == cap_snaps) {
        cap_snaps = cap_snaps ? cap_snaps * 2 : 64;
        struct snap *p = host_alloc(sizeof(struct snap) * cap_snaps);
        if (snaps) memcpy(p, snaps, sizeof(struct snap) * n_snaps);
        host_free(snaps);
        snaps = p;
    }
    u32 n = strlen(path);
    snaps[n_snaps].path = host_alloc(n + 1);
    memcpy(snaps[n_snaps].path, path, n + 1);
    snaps[n_snaps].dir = dir;
    snaps[n_snaps].link = link;
    snaps[n_snaps].data = data;
    snaps[n_snaps].len = len;
    n_snaps++;
    return FS_OK;
}

static int snap_dir(u32 ino, const char *path) {
    u32 pos = 0;
    struct fs_dirent de;
    int r;
    while ((r = myfs_readdir(ino, &pos, &de)) == 1) {
        if (!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        char child[FS_PATH_MAX];
        u32 pl = strlen(path), nl = strlen(de.name);
        if (pl == 1) {                                                          /* "/" */
            if (1 + nl >= sizeof(child)) return FS_ENAMETOOLONG;
            child[0] = '/';
            memcpy(child + 1, de.name, nl + 1);
        } else {
            if (pl + 1 + nl >= sizeof(child)) return FS_ENAMETOOLONG;
            memcpy(child, path, pl);
            child[pl] = '/';
            memcpy(child + pl + 1, de.name, nl + 1);
        }
        struct fs_stat st;
        if ((r = myfs_stat(de.ino, &st))) return r;
        if (st.type == FS_TYPE_DIR) {
            if ((r = snap_add(child, true, false, NULL, 0)))return r;
            snaps[n_snaps-1].mode=st.mode;snaps[n_snaps-1].uid=st.uid;snaps[n_snaps-1].gid=st.gid;
            if((r=snap_dir(de.ino,child)))return r;
        } else if (st.type == FS_TYPE_LINK || st.type == FS_TYPE_FILE) {
            unsigned char *data = NULL;
            if (st.size) {
                data = host_alloc(st.size);
                u32 off = 0;
                while (off < st.size) {
                    int n = myfs_read(de.ino, off, data + off, st.size - off);
                    if (n < 0) return n;
                    if (n == 0) return FS_EIO;
                    off += n;
                }
            }
            if ((r = snap_add(child, false, st.type == FS_TYPE_LINK, data, st.size))) return r;
            snaps[n_snaps-1].mode=st.mode;snaps[n_snaps-1].uid=st.uid;snaps[n_snaps-1].gid=st.gid;
        } else {
            err("myfs: cannot copy %s\n", child);
            return FS_EINVAL;
        }
    }
    return r < 0 ? r : FS_OK;
}

static unsigned parse_mb(const char *s) {
    unsigned v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 8192) return 0;
        v = v * 10 + (unsigned)(*s - '0');
    }
    return v >= 1 && v <= 8192 ? v : 0;
}

static int cmd_grow(const char *image, const char *mb_text) {
    unsigned mb = parse_mb(mb_text);
    if (!mb) { err("myfs: grow size is megabytes, from 1 to 8192\n"); return FS_EINVAL; }
    if (block_partitioned()) { err("myfs: grow works on a whole MyFS disk, not a partition table\n"); return FS_EINVAL; }

    unsigned old_kb = host_sectors() / 2;
    myfs_select(0, 0);
    struct fs_stat root_attributes;int r=fs_stat(FS_ROOT,&root_attributes);
    if(!r)r = snap_dir(FS_ROOT, "/");
    if (r) return r;
    host_close_image();

    char *neu = host_alloc(strlen(image) + 5);
    memcpy(neu, image, strlen(image));
    memcpy(neu + strlen(image), ".new", 5);
    if (host_create_image(neu, mb * 2048)) { err("myfs: cannot create %s\n", neu); return FS_EIO; }
    bool formatted = false;
    r = fs_init(&formatted);
    if (!r && !formatted) r = FS_EINVAL;            /* новый образ обязан быть пустым */
    for (int i = 0; !r && i < n_snaps; i++) {
        if (snaps[i].dir) r = mkdir_p(snaps[i].path);
        else if (snaps[i].link) {
            char target[FS_PATH_MAX];
            if (!snaps[i].len || snaps[i].len >= sizeof(target)) r = FS_EINVAL;
            else {
                memcpy(target, snaps[i].data, snaps[i].len);
                target[snaps[i].len] = 0;
                r = fs_symlink(FS_ROOT, target, snaps[i].path);
            }
        } else r = put_bytes(snaps[i].path, snaps[i].data, snaps[i].len);
    }
    myfs_select(0, 0);
    if(!r)r=myfs_setattr(FS_ROOT,root_attributes.mode,root_attributes.uid,root_attributes.gid);
    for(int i=0;!r && i<n_snaps;i++) {
        u32 ino;r=fs_resolve_nofollow(FS_ROOT,snaps[i].path,&ino);
        if(!r)r=myfs_setattr(ino,snaps[i].mode,snaps[i].uid,snaps[i].gid);
    }
    for (int i = 0; !r && i < n_snaps; i++) {
        u32 ino;
        struct fs_stat st;
        r = myfs_resolve(FS_ROOT, snaps[i].path, &ino);
        if (!r) r = myfs_stat(ino, &st);
        if (r) break;
        if (snaps[i].dir) { if (st.type != FS_TYPE_DIR) r = FS_EIO; continue; }
        if (snaps[i].link) {
            if (st.type != FS_TYPE_LINK || st.size != snaps[i].len) { r = FS_EIO; break; }
        } else if (st.type != FS_TYPE_FILE || st.size != snaps[i].len) { r = FS_EIO; break; }
        if (!snaps[i].len) continue;
        unsigned char *got = host_alloc(snaps[i].len);
        u32 off = 0;
        while (!r && off < snaps[i].len) {
            int n = myfs_read(ino, off, got + off, snaps[i].len - off);
            if (n < 0) r = n;
            else if (n == 0) r = FS_EIO;
            else off += n;
        }
        if (!r && memcmp(got, snaps[i].data, snaps[i].len)) r = FS_EIO;
        host_free(got);
    }
    if (r) {
        host_close_image();
        host_remove_file(neu);
        err("myfs: grow failed, %s is unchanged\n", image);
        return r;
    }
    struct fs_info after;
    fs_info(&after);
    host_close_image();
    if (host_replace_image(image, neu)) { err("myfs: cannot replace %s (new image is %s)\n", image, neu); return FS_EIO; }
    say("myfs: %s %u MB -> %u MB, %d entries, %u KB free, %u/%u inodes used\n",
        image, old_kb / 1024, mb, n_snaps, after.free_blocks / 2,
        after.total_inodes - after.free_inodes, after.total_inodes);
    say("myfs: previous image kept as %s.bak\n", image);
    return FS_OK;
}

static int cmd_cat(const char *path) {
    u32 ino, off = 0;
    int r = fs_resolve(FS_ROOT, path, &ino);
    if (r) return r;
    for (;;) {
        int n = fs_read(ino, off, out, sizeof(out) - 1);
        if (n <= 0) break;
        out[n] = 0;
        host_print(out);
        off += n;
    }
    return FS_OK;
}

static int cmd_get(const char *src, const char *dst) {
    u32 ino;
    struct fs_stat st;
    unsigned char *buf;
    int r = fs_resolve(FS_ROOT, src, &ino);
    if (r) return r;
    if ((r = fs_stat(ino, &st))) return r;
    buf = host_alloc(st.size ? st.size : 1);
    r = fs_read(ino, 0, buf, st.size);
    if (r < 0) { host_free(buf); return r; }
    if ((unsigned)r != st.size) { host_free(buf); return FS_EIO; }
    if (host_save_file(dst, buf, st.size)) { host_free(buf); return FS_EIO; }
    host_free(buf);
    say("  %s -> %s (%u bytes)\n", src, dst, st.size);
    return FS_OK;
}


static int number(const char *s,u32 base,u32 *out) {
    if(!s || !*s)return FS_EINVAL;u32 n=0;
    for(;*s;s++){u32 v=*s-'0';if(v>=base || n>(0xffffffffu-v)/base)return FS_EINVAL;n=n*base+v;}
    *out=n;return 0;
}
static int metadata_command(const char *cmd,int argc,char **argv) {
    u32 ino,mode,uid,gid;int ret;
    if(!strcmp(cmd,"stat")) {
        if(argc!=4)return FS_EINVAL;
        ret=fs_resolve_nofollow(FS_ROOT,argv[3],&ino);struct fs_stat st;
        if(!ret)ret=fs_stat(ino,&st);
        if(!ret)say("mode=%04o uid=%u gid=%u size=%u inode=%u type=%u\n",st.mode,st.uid,st.gid,st.size,ino,st.type);
        return ret;
    }
    if(!strcmp(cmd,"chmod")) {
        if(argc!=5 || number(argv[3],8,&mode))return FS_EINVAL;
        ret=fs_resolve(FS_ROOT,argv[4],&ino);return ret?ret:fs_chmod(ino,mode);
    }
    if(argc!=6 || number(argv[3],10,&uid) || number(argv[4],10,&gid))return FS_EINVAL;
    ret=fs_resolve_nofollow(FS_ROOT,argv[5],&ino);return ret?ret:fs_chown(ino,uid,gid);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        err("usage: myfs <disk.img> ls [path] | mkdir <path> | put <file> <path> | cat <path> | rm <path> | grow <MB>\n");
        return 2;
    }
    if (host_open_image(argv[1])) { err("myfs: cannot open %s\n", argv[1]); return 1; }
    bool formatted;
    int r = fs_init(&formatted);
    if (r) { err("myfs: %s\n", fs_strerror(r)); return 1; }
    if (formatted) say("myfs: %s had no file system - formatted\n", argv[1]);

    const char *cmd = argv[2];
    if (!strcmp(cmd, "ls"))                       r = cmd_ls(argc > 3 ? argv[3] : "/");
    else if (!strcmp(cmd, "mkdir") && argc > 3)   r = mkdir_p(argv[3]);
    else if (!strcmp(cmd, "put") && argc > 4)     r = cmd_put(argv[3], argv[4]);
    else if (!strcmp(cmd, "cat") && argc > 3)     r = cmd_cat(argv[3]);
    else if (!strcmp(cmd, "get") && argc > 4)     r = cmd_get(argv[3], argv[4]);
    else if (!strcmp(cmd, "rm") && argc > 3)      r = cmd_rm(argv[3]);
    else if(!strcmp(cmd,"stat") || !strcmp(cmd,"chmod") || !strcmp(cmd,"chown"))r=metadata_command(cmd,argc,argv);
    else if (!strcmp(cmd, "grow") && argc > 3)    r = cmd_grow(argv[1], argv[3]);
    else { err("myfs: bad command\n"); r = FS_EINVAL; }

    host_close_image();
    if (r) { err("myfs: %s\n", fs_strerror(r)); return 1; }
    return 0;
}
