/* VFS: opaque handles encode mount slot + local inode. /dev is virtual.
 * The recursive lock stays held across backend selection and blocking I/O. */
#include "fs.h"
#include "myfs.h"
#include "block.h"
#include "task.h"
#include "string.h"
#ifdef HOST_TEST
void *malloc(unsigned long);void free(void*);
#define lookup_alloc(n) malloc(n)
#define lookup_free(p) free(p)
#define file_disk_busy(i) false
#define file_disk_volume_busy(i) false
#else
#include "heap.h"
#include "file.h"
#define lookup_alloc(n) kmalloc(n)
#define lookup_free(p) kfree(p)
#endif
#define DEVROOT 0xf0000001u
#define DEVBASE 0xf0000010u
#define DEVNULL 0xf0000100u
#define DEVZERO 0xf0000101u
#define DEVRANDOM 0xf0000102u
#define DEVURANDOM 0xf0000103u
#define SLOT(i) ((i)>>24)
#define LOCAL(i) ((i)&0xffffff)
#define HANDLE(s,i) (((u32)(s)<<24)|(i))
#define FS_MAX_SYMLINKS 40
static struct { bool used; int device; u32 covered, parent; } mounts[4];
static struct mutex lock;
void fs_namespace_lock(void) { mutex_lock(&lock); }
void fs_namespace_unlock(void) { mutex_unlock(&lock); }
static bool ready;
static bool isdev(u32 i) { return i>=DEVROOT; }
static bool devchr(u32 i) { return i==DEVNULL || i==DEVZERO || i==DEVRANDOM || i==DEVURANDOM; }
/* Поток для OpenSSL: /dev/urandom и /dev/random. Состояние крутит xoshiro128**
 * и каждый вызов подмешивает rdtsc — отдельного аппаратного ГСЧ нет. */
static u32 rng_s[4];
static bool rng_ready;
static u32 rotl32(u32 x, int k) { return (x<<k) | (x>>(32-k)); }
static u32 rng_next(void) {
    u32 lo, hi, r, t;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if(!rng_ready) {
        rng_s[0]=lo^0x9e3779b9u; rng_s[1]=hi^0x6a09e667u;
        rng_s[2]=lo*0x85ebca6bu+1u; rng_s[3]=hi*0xc2b2ae35u+0x27d4eb2fu;
        if(!(rng_s[0]|rng_s[1]|rng_s[2]|rng_s[3])) rng_s[0]=1;
        rng_ready=true;
    }
    r=rotl32(rng_s[1]*5, 7)*9;
    t=rng_s[1]<<9;
    rng_s[2]^=rng_s[0]; rng_s[3]^=rng_s[1]; rng_s[1]^=rng_s[2]; rng_s[0]^=rng_s[3];
    rng_s[2]^=t; rng_s[3]=rotl32(rng_s[3], 11);
    rng_s[0]^=lo; rng_s[1]^=hi;
    return r^lo;
}
static void fill_random(void *buf, u32 n) {
    u8 *p=buf;
    while(n) {
        u32 v=rng_next(), k=n<4?n:4;
        memcpy(p, &v, k); p+=k; n-=k;
    }
}
static int select_inode(u32 i) {
    u32 s=SLOT(i); if(s>=4 || !mounts[s].used) return FS_ENODEV;
    myfs_select(s,mounts[s].device); return 0;
}
struct fs_orphan {u32 ino;struct fs_orphan *next;};
static struct fs_orphan *orphans;
static void reap_orphans(void) {
    struct fs_orphan **link=&orphans;
    while(*link){struct fs_orphan *o=*link;
        if(!file_disk_busy(o->ino) && !select_inode(o->ino) && !myfs_reap_orphan(LOCAL(o->ino))){*link=o->next;lookup_free(o);}
        else link=&o->next;
    }
}
static u32 cross(u32 i) {
    for(int s=1;s<4;s++) if(mounts[s].used && mounts[s].covered==i) return HANDLE(s,FS_ROOT);
    return i;
}
static int access_impl(u32 ino,u32 mask,bool real_ids);
static void credentials(u32 *uid,u32 *gid,u32 *mask,bool real_ids);
static int stat_impl(u32 i,struct fs_stat *st) {
    if(!isdev(i)) { int r=select_inode(i); return r ? r : myfs_stat(LOCAL(i),st); }
    memset(st,0,sizeof(*st));
    st->links=1;
    st->mode=i==DEVROOT?0755:devchr(i)?0666:0600;
    if(i==DEVROOT) { st->type=FS_TYPE_DIR; return 0; }
    if(devchr(i)) { st->type=FS_TYPE_CHAR; return 0; }
    const struct block_device *d=block_get((int)(i-DEVBASE)); if(!d) return FS_ENOENT;
    st->type=FS_TYPE_BLOCK; st->blocks=d->sectors;
    st->size=d->sectors>0x7fffff ? 0xffffffffu : d->sectors*512; return 0;
}
static int readdir_impl(u32 dir,u32 *pos,struct fs_dirent *out) {
    if(dir==DEVROOT) {
        u32 n=(*pos)++;
        if(n<(u32)block_count()) { int id=block_at(n); out->ino=DEVBASE+id; strcpy(out->name,block_get(id)->name); return 1; }
        if(n==(u32)block_count()) { out->ino=DEVNULL; strcpy(out->name,"null"); return 1; }
        if(n==(u32)block_count()+1) { out->ino=DEVZERO; strcpy(out->name,"zero"); return 1; }
        if(n==(u32)block_count()+2) { out->ino=DEVRANDOM; strcpy(out->name,"random"); return 1; }
        if(n==(u32)block_count()+3) { out->ino=DEVURANDOM; strcpy(out->name,"urandom"); return 1; }
        return 0;
    }
    if(isdev(dir)) return FS_ENOTDIR;
    int r=select_inode(dir); if(r) return r;
    if(dir==FS_ROOT && *pos==0) { *pos=1; out->ino=DEVROOT; strcpy(out->name,"dev"); return 1; }
    u32 p=dir==FS_ROOT ? *pos-1 : *pos;
    while((r=myfs_readdir(LOCAL(dir),&p,out))==1) {
        if(dir==FS_ROOT && !strcmp(out->name,"dev")) continue;
        out->ino=cross(HANDLE(SLOT(dir),out->ino)); break;
    }
    *pos=dir==FS_ROOT ? p+1 : p; return r;
}
static int lookup_uncached(u32 dir,const char *name,u32 *out) {
    struct fs_stat st; int r=stat_impl(dir,&st); if(r) return r;
    if(st.type!=FS_TYPE_DIR) return FS_ENOTDIR;
    if(!strcmp(name,".")) { *out=dir; return 0; }
    if(!strcmp(name,"..")) {
        if(dir==DEVROOT) { *out=FS_ROOT; return 0; }
        if(LOCAL(dir)==FS_ROOT && SLOT(dir)>0) { *out=mounts[SLOT(dir)].parent; return 0; }
        r=select_inode(dir); if(r) return r;
        r=myfs_resolve(LOCAL(dir),"..",out); if(!r) *out=HANDLE(SLOT(dir),*out); return r;
    }
    u32 pos=0; struct fs_dirent de;
    while((r=readdir_impl(dir,&pos,&de))==1) if(!strcmp(name,de.name)) { *out=de.ino; return 0; }
    return r<0 ? r : FS_ENOENT;
}
/* Positive inode identities only: creates cannot invalidate existing names.
 * Removal, rename, format and mount changes flush the cache under the VFS lock.
 * Keep virtual devices and dot entries on their dynamic lookup paths. */
#define LOOKUP_CACHE_SIZE 256
struct lookup_entry { u32 dir,ino,age; char name[FS_NAME_MAX+1]; };
static struct lookup_entry *lookup_cache;
static u32 lookup_age;
static void lookup_cache_clear(void) {
    for(unsigned i=0;lookup_cache && i<LOOKUP_CACHE_SIZE;i++) lookup_cache[i].age=0;
    lookup_age=0;
}
static int lookup(u32 dir,const char *name,u32 *out) {
    if(isdev(dir) || !strcmp(name,".") || !strcmp(name,"..")) return lookup_uncached(dir,name,out);
    if(!lookup_cache){lookup_cache=lookup_alloc(LOOKUP_CACHE_SIZE*sizeof(*lookup_cache));if(!lookup_cache)return lookup_uncached(dir,name,out);lookup_cache_clear();}
    if(!++lookup_age) {lookup_cache_clear();lookup_age=1;}
    unsigned slot=0;u32 oldest=0xffffffffu;
    for(unsigned i=0;i<LOOKUP_CACHE_SIZE;i++) {
        if(lookup_cache[i].age && lookup_cache[i].dir==dir && !strcmp(lookup_cache[i].name,name)) {
            lookup_cache[i].age=lookup_age;*out=lookup_cache[i].ino;return 0;
        }
        if(lookup_cache[i].age<oldest) {slot=i;oldest=lookup_cache[i].age;}
    }
    int ret=lookup_uncached(dir,name,out);
    if(!ret && !isdev(*out)) {
        lookup_cache[slot].dir=dir;lookup_cache[slot].ino=*out;lookup_cache[slot].age=lookup_age;
        strcpy(lookup_cache[slot].name,name);
    }
    return ret;
}
/* Текст цели в буфер с завершающим нулём. Размер ссылки меньше cap. */
static int link_text(u32 ino,char *buf,u32 cap) {
    struct fs_stat st; int r=stat_impl(ino,&st); if(r) return r;
    if(st.type!=FS_TYPE_LINK || isdev(ino)) return FS_EINVAL;
    if(!st.size) return FS_EINVAL;
    if(st.size>=cap) return FS_ENAMETOOLONG;
    r=select_inode(ino); if(r) return r;
    int n=myfs_read(LOCAL(ino),0,buf,st.size);
    if(n<0) return n;
    if((u32)n!=st.size) return FS_EIO;
    buf[st.size]=0; return 0;
}
/* follow_last: открытие, stat, chdir и exec раскрывают последнюю ссылку.
 * unlink, rename, readlink и lstat — нет. Промежуточные компоненты всегда
 * раскрываются. Хвост «/» тоже требует раскрытия: дальше нужен каталог.
 * Обход итеративный: два буфера пути, не рекурсия по стеку ядра. */
#include "fs-permissions.inc"

static u32 process_root(void) {
#ifdef HOST_TEST
    return FS_ROOT;
#else
    struct task *t=task_process(task_current());
    return t && !task_current()->fs_account_scope && t->fs_root ? t->fs_root : FS_ROOT;
#endif
}
static int resolve_ids(u32 cwd,const char *path,u32 *out,bool follow_last,bool real_ids) {
    if(!ready) return FS_ENODEV;
    if(!path || !*path) return FS_ENOENT;
    char curpath[FS_PATH_MAX], scratch[FS_PATH_MAX];
    u32 len=strlen(path);
    if(len>=FS_PATH_MAX) return FS_ENAMETOOLONG;
    memcpy(curpath,path,len+1);
    u32 root=process_root();
    u32 dir=curpath[0]=='/' ? root : cwd;
    char *p=curpath;
    int nlinks=0;
    for(;;) {
        while(*p=='/') p++;
        if(!*p) { *out=dir; return 0; }
        char name[FS_NAME_MAX+1]; u32 n=0;
        while(p[n] && p[n]!='/') { if(n==FS_NAME_MAX) return FS_ENAMETOOLONG; name[n]=p[n]; n++; }
        name[n]=0; p+=n;
        bool slash=*p=='/';
        u32 next; int r=access_impl(dir,1,real_ids);if(r)return r;
        if(dir==root && !strcmp(name,"..")) {next=root;r=0;}
        else r=lookup(dir,name,&next);
        if(r) return r;
        struct fs_stat st; r=stat_impl(next,&st); if(r) return r;
        bool more=false;
        if(slash) { more=true; while(*p=='/') p++; }
        bool last=!*p;
        if(st.type==FS_TYPE_LINK && (more || follow_last)) {
            if(++nlinks>FS_MAX_SYMLINKS) return FS_ELOOP;
            r=link_text(next,scratch,sizeof(scratch)); if(r) return r;
            u32 tl=strlen(scratch), rl=strlen(p);
            bool add= (rl || (more && last)) && (tl==0 || scratch[tl-1]!='/');
            if(tl+(add?1u:0u)+rl>=FS_PATH_MAX) return FS_ENAMETOOLONG;
            if(add) scratch[tl++]='/';
            memcpy(scratch+tl,p,rl+1);
            memcpy(curpath,scratch,tl+rl+1);
            p=curpath;
            if(curpath[0]=='/') dir=root;
            continue;
        }
        if(more && st.type!=FS_TYPE_DIR) return FS_ENOTDIR;
        dir=next;
        if(last) { *out=dir; return 0; }
    }
}
static int resolve_impl(u32 cwd,const char *path,u32 *out,bool follow_last) {
    return resolve_ids(cwd,path,out,follow_last,false);
}
int fs_access_path(u32 cwd,const char *path,u32 mask) {
    mutex_lock(&lock);u32 ino;int ret=resolve_ids(cwd,path,&ino,true,true);
    if(!ret)ret=access_impl(ino,mask,true);
    mutex_unlock(&lock);return ret;
}
static int parent_impl(u32 cwd,const char *path,u32 *parent,char *name) {
    char buf[FS_PATH_MAX]; u32 n=strlen(path); if(!n || n>=sizeof(buf)) return FS_EINVAL;
    memcpy(buf,path,n+1); while(n>1 && buf[n-1]=='/') buf[--n]=0;
    int slash=-1; for(u32 j=0;j<n;j++) if(buf[j]=='/') slash=j;
    const char *base=buf+slash+1;
    if(!*base || !strcmp(base,".") || !strcmp(base,"..")) return FS_EINVAL;
    if(strlen(base)>FS_NAME_MAX) return FS_ENAMETOOLONG;
    strcpy(name,base); int r=0;
    if(slash<0) *parent=cwd; else if(slash==0) *parent=process_root();
    else { buf[slash]=0; r=resolve_impl(cwd,buf,parent,true); }
    if(r) return r;
    struct fs_stat st; r=stat_impl(*parent,&st); return r ? r : st.type==FS_TYPE_DIR ? 0 : FS_ENOTDIR;
}
static bool protected(u32 ino) {
    if(isdev(ino) || LOCAL(ino)==FS_ROOT) return true;
    /* Prevent moving ancestors of active mounts. */
    for(int s=1;s<4;s++) if(mounts[s].used) {
        u32 p=mounts[s].covered;
        for(int depth=0;depth<64;depth++) { if(p==ino) return true; if(p==FS_ROOT || lookup(p,"..",&p)) break; }
    }
    return false;
}
static int getpath_impl(u32 ino,char *buf,u32 len) {
    /* Номера inode, а не сами имена: 64 имени по 256 байт не помещаются в стек ядра. */
    u32 chain[64]; int depth=0;
    u32 root=process_root();
    while(ino!=root) {
        if(depth==64) return FS_ENAMETOOLONG;
        chain[depth++]=ino;
        u32 parent; int r=lookup(ino,"..",&parent); if(r) return r;
        if(parent==ino) return FS_EIO;
        ino=parent;
    }
    if(len<2) return FS_ENAMETOOLONG;
    u32 p=0; buf[p++]='/';
    for(int i=depth-1;i>=0;i--) {
        u32 want=chain[i], parent=i==depth-1 ? root : chain[i+1];
        u32 pos=0; struct fs_dirent de; bool found=false;
        while(readdir_impl(parent,&pos,&de)==1) if(de.ino==want) { found=true; break; }
        if(!found) return FS_ENOENT;
        u32 n=strlen(de.name);
        if(p+n+(i?1u:0u)+1u>len) return FS_ENAMETOOLONG;
        memcpy(buf+p,de.name,n); p+=n; if(i) buf[p++]='/';
    }
    buf[p]=0; return 0;
}
int fs_init(bool *formatted) {
    mutex_lock(&lock);while(orphans){struct fs_orphan *o=orphans;orphans=o->next;lookup_free(o);}
    ready=false; memset(mounts,0,sizeof(mounts)); *formatted=false;
    int r=block_init();
    if(!r) {
        r=FS_ENODEV;
        for(int d=block_partitioned()?1:0;d<5;d++) {
            if(!block_get(d)) continue;
            myfs_select(0,d); r=myfs_init(formatted,!block_partitioned());
            if(!r) { mounts[0].used=true; mounts[0].device=d; ready=true; break; }
        }
    } else r=FS_EIO;
    lookup_cache_clear(); mutex_unlock(&lock); return r;
}
bool fs_ready(void) { return ready; }
int fs_format(void) {
    mutex_lock(&lock);reap_orphans(); int r=ready ? 0 : FS_ENODEV;
    if(file_disk_volume_busy(0))r=FS_EBUSY;
    for(int s=1;s<4;s++) if(mounts[s].used) r=FS_EBUSY;
    if(!r) { myfs_select(0,mounts[0].device); r=myfs_format(); }
    lookup_cache_clear(); mutex_unlock(&lock); return r;
}
#ifdef HOST_TEST
int fs_format_legacy(void) {
    mutex_lock(&lock);reap_orphans(); int r=ready ? 0 : FS_ENODEV;
    if(file_disk_volume_busy(0))r=FS_EBUSY;
    for(int s=1;s<4;s++) if(mounts[s].used) r=FS_EBUSY;
    if(!r) { myfs_select(0,mounts[0].device); r=myfs_format_legacy(); }
    lookup_cache_clear(); mutex_unlock(&lock); return r;
}
#endif
int fs_mount(u32 cwd,const char *device,const char *target) {
    mutex_lock(&lock); int d=block_find(device),r=0,slot=0; u32 ino=0,parent=0;
    if(d<0) r=FS_ENODEV;
    if(!r) r=resolve_impl(cwd,target,&ino,true);
    struct fs_stat st;
    if(!r) { r=stat_impl(ino,&st); if(!r && st.type!=FS_TYPE_DIR) r=FS_ENOTDIR; }
    if(!r && protected(ino)) r=FS_EBUSY;
    for(int s=0;!r && s<4;s++) if(mounts[s].used) {
        const struct block_device *a=block_get(d),*b=block_get(mounts[s].device);
        if(d/5==mounts[s].device/5 && a->start < b->start+b->sectors && b->start < a->start+a->sectors) r=FS_EBUSY;
    }
    if(!r) r=lookup(ino,"..",&parent);
    for(int s=1;s<4;s++) if(!mounts[s].used) { slot=s; break; }
    if(!r && !slot) r=FS_ENOSPC;
    if(!r) { bool formatted; myfs_select(slot,d); r=myfs_init(&formatted,false); }
    if(!r) { mounts[slot].used=true; mounts[slot].device=d; mounts[slot].covered=ino; mounts[slot].parent=parent; }
    lookup_cache_clear(); mutex_unlock(&lock); return r;
}
int fs_mount_info(u32 index,char *device,char *target) {
    mutex_lock(&lock); int r=0;
    if(index<4 && mounts[index].used) {
        strcpy(device,"/dev/"); strcpy(device+5,block_get(mounts[index].device)->name);
        r=getpath_impl(HANDLE(index,FS_ROOT),target,128); r=r ? r : 1;
    }
    mutex_unlock(&lock); return r;
}
/* Administrative writes are restricted to unmounted disks/ranges. No generic
 * raw-write interface is exported to userspace. Slot 4 is a private formatter. */
static bool device_busy(int d,bool whole_disk) {
#ifndef HOST_TEST
    if(task_raw_device_busy(d/5)) return true;
#endif
    const struct block_device *a=block_get(d);
    for(int s=0;s<4;s++) if(mounts[s].used && mounts[s].device/5==d/5) {
        const struct block_device *b=block_get(mounts[s].device);
        if(whole_disk || (a->start<b->start+b->sectors && b->start<a->start+a->sectors)) return true;
    }
    return false;
}
int fs_mkfs(const char *device) {
    mutex_lock(&lock); int d=block_find(device),r=0;
    if(d<0) r=FS_ENODEV;
    else if(device_busy(d,false)) r=FS_EBUSY;
    /* Whole disks with a partition table must be explicitly reinitialized first. */
    else if(!block_get(d)->partition && block_disk_partitioned(d/5)) r=FS_EINVAL;
    else { myfs_select(4,d); r=myfs_format(); }
    mutex_unlock(&lock); return r;
}
static u32 mbr_u32(const u8 *p) { return p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static void mbr_put(u8 *p,u32 v) { for(int i=0;i<4;i++) p[i]=v>>(8*i); }
int fs_partition(const char *device,u32 op,u32 slot,u32 start,u32 sectors) {
    mutex_lock(&lock); int d=block_find(device),r=0;
    if(d<0) r=FS_ENODEV;
    else if(block_get(d)->partition || op>2) r=FS_EINVAL;
    else if(device_busy(d,true)) r=FS_EBUSY;
    u8 buf[512];
    if(!r && block_read(d,0,buf)) r=FS_EIO;
    if(!r && op==0) { /* Explicit init: preserve boot code, clear primary entries. */
        if(buf[510]==0x55 && buf[511]==0xaa)
            for(int i=0;i<4;i++) if(buf[446+i*16+4]==0xee) r=FS_EINVAL;
        if(!r) { memset(buf+446,0,64); buf[510]=0x55; buf[511]=0xaa; }
    } else if(!r) {
        if(slot<1 || slot>4 || buf[510]!=0x55 || buf[511]!=0xaa) r=FS_EINVAL;
        for(int i=0;!r && i<4;i++) {
            const u8 *e=buf+446+i*16;
            if(e[4] && e[4]!=0x83) r=FS_EINVAL;
        }
        if(!r) {
            u8 *p=buf+446+(slot-1)*16;
            if(op==2) {
                if(!p[4]) r=FS_ENOENT; else memset(p,0,16);
            } else {
                u32 total=block_get(d)->sectors;
                if(p[4]) r=FS_EEXIST;
                else if(start<2048 || !sectors || sectors>FS_MAX_BLOCKS || start>=total || sectors>total-start) r=FS_EINVAL;
                for(int i=0;!r && i<4;i++) {
                    const u8 *e=buf+446+i*16; if(!e[4]) continue;
                    u32 a=mbr_u32(e+8),n=mbr_u32(e+12);
                    if(!a || !n || a>=total || n>total-a || (start<a+n && a<start+sectors)) r=FS_EINVAL;
                }
                if(!r) { memset(p,0,16); p[1]=p[5]=0xfe; p[2]=p[3]=p[6]=p[7]=0xff; p[4]=0x83; mbr_put(p+8,start); mbr_put(p+12,sectors); }
            }
        }
    }
    if(!r && block_write(d,0,buf)) r=FS_EIO;
    if(!r && block_rescan(d/5)) r=FS_EIO;
    mutex_unlock(&lock); return r;
}
int fs_resolve(u32 cwd,const char *path,u32 *ino) { mutex_lock(&lock); int r=resolve_impl(cwd,path,ino,true); mutex_unlock(&lock); return r; }
int fs_resolve_nofollow(u32 cwd,const char *path,u32 *ino) { mutex_lock(&lock); int r=resolve_impl(cwd,path,ino,false); mutex_unlock(&lock); return r; }
int fs_symlink(u32 cwd,const char *target,const char *linkpath) {
    if(!target || !*target || !linkpath) return FS_EINVAL;
    u32 n=strlen(target); if(n>=FS_PATH_MAX) return FS_ENAMETOOLONG;
    mutex_lock(&lock);reap_orphans(); u32 parent=0,ino=0; char name[FS_NAME_MAX+1];
    int r=parent_impl(cwd,linkpath,&parent,name);
    if(!r)r=access_impl(parent,3,false);
    if(!r && (isdev(parent) || (parent==FS_ROOT && !strcmp(name,"dev")))) r=FS_EROFS;
    if(!r) {
        r=select_inode(parent);
        if(!r) r=myfs_create(LOCAL(parent),name,FS_TYPE_LINK,&ino);
        if(!r) {
            u32 uid,gid,mask;credentials(&uid,&gid,&mask,false);
            r=myfs_setattr(ino,0777,uid,gid);
            int w=r?r:myfs_write(ino,0,target,n);
            if(w<0) r=w; else if((u32)w!=n) r=FS_EIO;
            if(r) myfs_remove(LOCAL(parent),name);
        }
    }
    mutex_unlock(&lock); return r;
}
int fs_readlink(u32 cwd,const char *path,char *buf,u32 len) {
    mutex_lock(&lock); u32 ino; struct fs_stat st;
    int r=resolve_impl(cwd,path,&ino,false);
    if(!r) r=stat_impl(ino,&st);
    if(!r && st.type!=FS_TYPE_LINK) r=FS_EINVAL;
    if(!r) {
        if(len>st.size) len=st.size;
        if(!len) r=0;
        else {
            r=select_inode(ino);
            if(!r) { int n=myfs_read(LOCAL(ino),0,buf,len); r=n<0 ? n : n; }
        }
    }
    mutex_unlock(&lock); return r;
}
int fs_stat(u32 ino,struct fs_stat *st) { mutex_lock(&lock); int r=stat_impl(ino,st); mutex_unlock(&lock); return r; }
int fs_readdir(u32 dir,u32 *pos,struct fs_dirent *out) { mutex_lock(&lock); int r=access_impl(dir,4,false);if(!r)r=readdir_impl(dir,pos,out); mutex_unlock(&lock); return r; }
int fs_getpath(u32 ino,char *buf,u32 len) { mutex_lock(&lock); int r=getpath_impl(ino,buf,len); mutex_unlock(&lock); return r; }
int fs_create_mode(u32 cwd,const char *path,int type,u32 mode,u32 *out) {
    mutex_lock(&lock);reap_orphans();u32 parent=0,ino;char name[FS_NAME_MAX+1];
    int r=parent_impl(cwd,path,&parent,name);
    if(!r)r=access_impl(parent,3,false);
    if(!r && (isdev(parent) || (parent==FS_ROOT && !strcmp(name,"dev"))))r=FS_EROFS;
    if(!r && type!=FS_TYPE_DIR && type!=FS_TYPE_FILE)r=FS_EINVAL;
    if(!r) {
        r=select_inode(parent);if(!r)r=myfs_create(LOCAL(parent),name,type,&ino);
        if(!r) {
            u32 uid,gid,mask;credentials(&uid,&gid,&mask,false);
            r=myfs_setattr(ino,mode&0777&~mask,uid,gid);
            if(r)myfs_remove(LOCAL(parent),name);
            else if(out)*out=HANDLE(SLOT(parent),ino);
        }
    }
    mutex_unlock(&lock);return r;
}
int fs_create(u32 cwd,const char *path,int type,u32 *out) {
    return fs_create_mode(cwd,path,type,type==FS_TYPE_DIR?0777:0666,out);
}
int fs_remove(u32 cwd,const char *path) {
    mutex_lock(&lock); u32 ino,parent; char name[FS_NAME_MAX+1]; int r=resolve_impl(cwd,path,&ino,false);
    if(!r && protected(ino)) r=FS_EBUSY;
    if(!r) r=parent_impl(cwd,path,&parent,name);
    if(!r)r=access_impl(parent,3,false);
    if(!r)r=sticky_check(parent,ino);
    if(!r) {
        bool keep=file_disk_busy(ino);struct fs_orphan *o=keep?lookup_alloc(sizeof(*o)):NULL;
        if(keep && !o)r=FS_ENOSPC;
        else {r=select_inode(parent);if(!r)r=myfs_remove_keep(LOCAL(parent),name,keep);}
        if(!r && keep){o->ino=ino;o->next=orphans;orphans=o;}else if(o)lookup_free(o);
    }
    lookup_cache_clear(); mutex_unlock(&lock); return r;
}
int fs_rename(u32 cwd,const char *src,const char *dst) {
    mutex_lock(&lock); u32 ino,sp,dp,dest; char sn[FS_NAME_MAX+1],dn[FS_NAME_MAX+1]; int r=resolve_impl(cwd,src,&ino,false);
    if(!r && protected(ino)) r=FS_EBUSY;
    if(!r) r=parent_impl(cwd,src,&sp,sn);
    if(!r) {
        struct fs_stat st;
        if(!resolve_impl(cwd,dst,&dest,false) && !stat_impl(dest,&st) && st.type==FS_TYPE_DIR) { dp=dest; strcpy(dn,sn); }
        else r=parent_impl(cwd,dst,&dp,dn);
    }
    if(!r && (isdev(dp) || (dp==FS_ROOT && !strcmp(dn,"dev")))) r=FS_EROFS;
    if(!r && SLOT(sp)!=SLOT(dp)) r=FS_EXDEV;
    if(!r)r=access_impl(sp,3,false);
    if(!r)r=access_impl(dp,3,false);
    if(!r)r=sticky_check(sp,ino);
    if(!r) {
        char destpath[FS_PATH_MAX]; r=select_inode(dp);
        if(!r) r=myfs_getpath(LOCAL(dp),destpath,sizeof(destpath));
        if(!r) { u32 n=strlen(destpath),l=strlen(dn);
            if(n+l+2>sizeof(destpath)) r=FS_ENAMETOOLONG;
            else { if(n>1) destpath[n++]='/'; strcpy(destpath+n,dn); r=myfs_rename(LOCAL(sp),sn,destpath); }
        }
    }
    lookup_cache_clear(); mutex_unlock(&lock); return r;
}
int fs_set_size(u32 ino, u32 size) {
    mutex_lock(&lock);
    int r = select_inode(ino);
    if (!r) r = myfs_set_size(LOCAL(ino), size);
    mutex_unlock(&lock);
    return r;
}
int fs_truncate(u32 ino) {
    mutex_lock(&lock); int r;
    if(devchr(ino)) r=0; else if(isdev(ino)) r=FS_EROFS;
    else { r=select_inode(ino); if(!r) r=myfs_truncate(LOCAL(ino)); }
    mutex_unlock(&lock); return r;
}
int fs_read(u32 ino,u32 off,void *buf,u32 n) {
    mutex_lock(&lock); int r=0;
    if(ino==DEVNULL) r=0; else if(ino==DEVZERO) { memset(buf,0,n); r=n; }
    else if(ino==DEVRANDOM || ino==DEVURANDOM) { fill_random(buf,n); r=(int)n; }
    else if(ino==DEVROOT) r=FS_EISDIR;
    else if(isdev(ino)) {
        if(n>0xffffffffu-off) n=0xffffffffu-off;
        int d=(int)(ino-DEVBASE); const struct block_device *dev=block_get(d);
        if(!dev) r=FS_ENODEV;
        else {
            u32 done=0; u8 sector[512];
            while(done<n && off/512<dev->sectors) {
                if(block_read(d,off/512,sector)) { r=FS_EIO; break; }
                u32 part=512-off%512; if(part>n-done) part=n-done;
                memcpy((u8*)buf+done,sector+off%512,part); done+=part;
                if(off>0xffffffffu-part) break;
                off+=part;
            }
            if(done) r=done;
        }
    } else { r=select_inode(ino); if(!r) r=myfs_read(LOCAL(ino),off,buf,n); }
    mutex_unlock(&lock); return r;
}
int fs_write(u32 ino,u32 off,const void *buf,u32 n) {
    mutex_lock(&lock); int r;
    if(devchr(ino)) r=n; else if(isdev(ino)) r=FS_EROFS;
    else { r=select_inode(ino); if(!r) r=myfs_write(LOCAL(ino),off,buf,n); }
    mutex_unlock(&lock); return r;
}
void fs_info(struct fs_info *info) { mutex_lock(&lock);reap_orphans(); memset(info,0,sizeof(*info)); if(ready) { myfs_select(0,mounts[0].device); myfs_info(info); } mutex_unlock(&lock); }
const char *fs_strerror(int e) {
    if(e==FS_EROFS) return "read-only filesystem/device";
    if(e==FS_EBUSY) return "mountpoint or device busy";
    if(e==FS_EXDEV) return "cross-device rename";
    return myfs_strerror(e);
}


