/* MyFS: реализация. Описание формата — в fs.h. */
#include "myfs.h"
#include "block.h"
#include "string.h"
#include "task.h"
#ifndef HOST_TEST
#include "ipc.h"
#endif
#ifdef HOST_TEST
void *malloc(unsigned long); void free(void *);
#define bitmap_alloc(n) malloc(n)
#define bitmap_free(p) free(p)
#else
#include "heap.h"
#define bitmap_alloc(n) kmalloc(n)
#define bitmap_free(p) kfree(p)
#endif

#define BS            512                       /* размер блока */
#define MAGIC         0x5346594D                /* "MYFS" в little-endian */
#define VERSION       3                       /* format() пишет её; монтирование принимает и 1 */
#define VERSION_MIN   1
#define NDIRECT       12                        /* версия 1; у версии 2 последний слот — тройной косвенный */
#define NDIRECT_V2    11
#define NINDIRECT     (BS / 4)                  /* 128 ссылок в косвенном блоке */
#define NDINDIRECT    (NINDIRECT * NINDIRECT)   /* 16384 блока через двойной косвенный */
#define NTINDIRECT    (NDINDIRECT * NINDIRECT)  /* 128^3 блока через тройной косвенный */
#define MAX_FILE_BLKS (NDIRECT + NINDIRECT + NDINDIRECT) /* предел версии 1 */
#define INODES_PER_BLK (BS / sizeof(struct inode))
#define BITS_PER_BLK  (BS * 8)
#define MAX_BLOCKS    FS_MAX_BLOCKS             /* 8 GiB; one inode per 64 KiB up to MAX_INODES */
#define MIN_INODES    256u
#define MAX_INODES    131072u                   /* an 8 GiB volume at the ratio above */

struct superblock {
    u32 magic;
    u32 version;
    u32 total_blocks;
    u32 bitmap_start, bitmap_blocks;
    u32 inode_start, inode_blocks, inode_count;
    u32 data_start;
    u32 layout_version, permissions_root, times_root; /* version 3; inode/data layout stays v1/v2 */
    u8  pad[BS - 12 * 4];
} __attribute__((packed));

struct inode {
    u16 type;
    u16 links;
    u32 size;
    u32 direct[NDIRECT];
    u32 indirect;
    u32 dindirect;      /* двойной косвенный. В версии 2 direct[11] — тройной косвенный */
} __attribute__((packed));                      /* ровно 64 байта */

struct dirent_v1 {
    u32  ino;
    char name[28];
} __attribute__((packed));                      /* версия 1: 32 байта, имя до 27 символов */

struct dirent_v2 {
    u32 ino;
    u16 reclen;                                 /* вся запись, кратна 4 */
    u8  namelen;
    u8  pad;
} __attribute__((packed));

#define NAME_V1 27

_Static_assert(sizeof(struct inode) == 64, "inode must be 64 bytes");
_Static_assert(sizeof(struct dirent_v1) == 32, "dirent v1 must be 32 bytes");
_Static_assert(sizeof(struct dirent_v2) == 8, "dirent v2 header must be 8 bytes");
_Static_assert(sizeof(struct superblock) == BS, "superblock must be one block");

#define BITMAP_DIRTY_BYTES ((MAX_BLOCKS / BITS_PER_BLK + 7) / 8)
struct fs_state {
    struct superblock super;
    u8 *bits;
    u32 bitmap_bytes, hint, ino_hint;
    bool active;
    u32 nblocks, ninodes;
    int device;
    u8 bitmap_dirty[BITMAP_DIRTY_BYTES];
};
static struct fs_state states[5];
static int selected;
void myfs_select(int slot, int device) { selected = slot; states[slot].device = device; }
#define sb (states[selected].super)
#define bitmap (states[selected].bits)
static int prepare_bitmap(u32 bytes) {
    if (states[selected].bitmap_bytes >= bytes) return 0;
    u8 *p=bitmap_alloc(bytes); if(!p) return FS_ENOSPC;
    bitmap_free(bitmap); bitmap=p; states[selected].bitmap_bytes=bytes; return 0;
}
#define mounted (states[selected].active)
#define available_blocks (states[selected].nblocks)
#define available_inodes (states[selected].ninodes)

/* ---------------------------------------------------------------- блоки */

static int bread(u32 b, void *buf)        { return block_read(states[selected].device, b, buf)  ? FS_EIO : FS_OK; }
static int bwrite(u32 b, const void *buf) {
    return block_write_through(states[selected].device, b, buf) ? FS_EIO : FS_OK;
}
static int bwrite_data(u32 b, const void *buf) {
    return block_write(states[selected].device, b, buf) ? FS_EIO : FS_OK;
}

static bool bit_get(u32 b) { return bitmap[b / 8] & (1 << (b % 8)); }
static void bit_set(u32 b, bool v) {
    if (v) bitmap[b / 8] |=  (1 << (b % 8));
    else   bitmap[b / 8] &= ~(1 << (b % 8));
}

/* Бит в памяти меняется сразу. Сектор битмапа уходит на диск один раз,
 * в конце операции, а не на каждое выделение. */
static void bitmap_mark(u32 b) {
    u32 sec = b / BITS_PER_BLK;
    if (sec >= sb.bitmap_blocks || sec / 8 >= BITMAP_DIRTY_BYTES) return;
    states[selected].bitmap_dirty[sec / 8] |= (u8)(1u << (sec % 8));
}

static int bitmap_flush(void) {
    if (!mounted || !bitmap) return 0;
    for (u32 sec = 0; sec < sb.bitmap_blocks; sec++) {
        u8 bit = (u8)(1u << (sec % 8));
        u8 *cell = &states[selected].bitmap_dirty[sec / 8];
        if (!(*cell & bit)) continue;
        if (bwrite(sb.bitmap_start + sec, bitmap + sec * BS)) return FS_EIO;
        *cell = (u8)(*cell & (u8)~bit);
    }
    return 0;
}

/* Содержимое нового блока не определено, пока вызывающий его не запишет. */
static u32 balloc(void) {
    u32 start=states[selected].hint;
    if(start<sb.data_start || start>=sb.total_blocks) start=sb.data_start;
    for (u32 n=0; n<sb.total_blocks-sb.data_start; n++) {
        u32 b=start+n; if(b>=sb.total_blocks) b=sb.data_start+(b-sb.total_blocks);
        if (!bit_get(b)) {
            bit_set(b, true);
            bitmap_mark(b);
            states[selected].hint=b+1;
            available_blocks--;
            return b;
        }
    }
    return 0;  /* места нет */
}

static void ind_invalidate_block(u32 b);

static void bfree(u32 b) {
    if (b < sb.data_start || b >= sb.total_blocks || !bit_get(b)) return;
    bit_set(b, false);
    if(b<states[selected].hint) states[selected].hint=b;
    bitmap_mark(b);
    ind_invalidate_block(b);
    available_blocks++;
}

static int write_zero_block(u32 b) {
    static u8 zero[BS];
    return bwrite(b, zero);
}

/* ---------------------------------------------------------------- inode */

static struct {
    int slot;
    u32 block;
    struct inode in[INODES_PER_BLK];
    bool valid;
} inoblk;

static int inoblk_load(u32 b) {
    if (inoblk.valid && inoblk.slot == selected && inoblk.block == b) return 0;
    if (bread(b, inoblk.in)) return FS_EIO;
    inoblk.slot = selected;
    inoblk.block = b;
    inoblk.valid = true;
    return 0;
}

static int iread(u32 ino, struct inode *out) {
    if (ino == 0 || ino >= sb.inode_count) return FS_EINVAL;
    u32 b = sb.inode_start + ino / INODES_PER_BLK;
    if (inoblk_load(b)) return FS_EIO;
    *out = inoblk.in[ino % INODES_PER_BLK];
    return FS_OK;
}

/* Последние прочитанные inode и блок файла. Обход каталога читает заголовок
 * записи и имя двумя вызовами подряд: оба попадают в один блок и один inode. */
static struct {
    int slot;
    u32 ino;
    struct inode in;
    bool valid;
} inocache;
static struct {
    int slot;
    u32 ino, index;
    u8 data[BS];
    bool valid;
} blkcache;

static struct ind_cache {
    int slot;
    u32 block;
    u32 data[NINDIRECT];
    bool valid, dirty;
} ind_level, ind_double, ind_triple;

static int ind_flush_one(struct ind_cache *c) {
    if (!c->valid || !c->dirty) return 0;
    if (c->slot != selected) return 0;
    if (bwrite(c->block, c->data)) return FS_EIO;
    c->dirty = false;
    return 0;
}

static int ind_flush_all(void) {
    if (ind_flush_one(&ind_level) || ind_flush_one(&ind_double) || ind_flush_one(&ind_triple))
        return FS_EIO;
    return 0;
}

static void ind_invalidate_block(u32 b) {
    struct ind_cache *cs[3] = { &ind_level, &ind_double, &ind_triple };
    for (int i = 0; i < 3; i++) {
        if (cs[i]->valid && cs[i]->slot == selected && cs[i]->block == b) {
            cs[i]->valid = false;
            cs[i]->dirty = false;
        }
    }
}

static int ind_load(struct ind_cache *c, u32 block, bool zero_new) {
    if (c->valid && c->slot == selected && c->block == block) return 0;
    if (ind_flush_one(c)) return FS_EIO;
    if (zero_new) memset(c->data, 0, sizeof c->data);
    else if (bread(block, c->data)) return FS_EIO;
    c->slot = selected;
    c->block = block;
    c->valid = true;
    c->dirty = zero_new;
    return 0;
}

static int read_ind(u32 block, u32 *out) {
    struct ind_cache *cs[3] = { &ind_level, &ind_double, &ind_triple };
    for (int i = 0; i < 3; i++) {
        if (cs[i]->valid && cs[i]->slot == selected && cs[i]->block == block) {
            memcpy(out, cs[i]->data, sizeof cs[i]->data);
            return 0;
        }
    }
    return bread(block, out);
}

static int sync_meta(void) {
    int b = bitmap_flush();
    int i = ind_flush_all();
    if (b) return b;
    return i;
}

static void permissions_cache_clear(void);
static void cache_drop_all(void) {
    if (mounted && bitmap) sync_meta();
    inocache.valid = false;
    blkcache.valid = false;
    inoblk.valid = false;
    ind_level.valid = ind_double.valid = ind_triple.valid = false;
    ind_level.dirty = ind_double.dirty = ind_triple.dirty = false;
    memset(states[selected].bitmap_dirty, 0, sizeof states[selected].bitmap_dirty);
    permissions_cache_clear();
}

static void cache_drop_ino(u32 ino) {
    if (inocache.valid && inocache.slot == selected && inocache.ino == ino)
        inocache.valid = false;
    if (blkcache.valid && blkcache.slot == selected && blkcache.ino == ino)
        blkcache.valid = false;
    permissions_cache_clear();
}

static int iwrite(u32 ino, const struct inode *in) {
    if (ino == 0 || ino >= sb.inode_count) return FS_EINVAL;
    u32 b = sb.inode_start + ino / INODES_PER_BLK;
    cache_drop_ino(ino);
    if (inoblk_load(b)) return FS_EIO;
    inoblk.in[ino % INODES_PER_BLK] = *in;
    return bwrite(b, inoblk.in);
}

#include "myfs-permissions.inc"

static void ino_note_free(u32 ino) {
    if (ino >= FS_ROOT && ino < sb.inode_count &&
        (states[selected].ino_hint < FS_ROOT || ino < states[selected].ino_hint))
        states[selected].ino_hint = ino;
}

static u32 ialloc(int type) {
    struct inode in;
    u32 start = states[selected].ino_hint;
    u32 span = sb.inode_count > FS_ROOT ? sb.inode_count - FS_ROOT : 0;
    if (start < FS_ROOT || start >= sb.inode_count) start = FS_ROOT;
    for (u32 n = 0; n < span; n++) {
        u32 i = start + n;
        if (i >= sb.inode_count) i = FS_ROOT + (i - sb.inode_count);
        if (iread(i, &in)) return 0;
        if (in.type == FS_TYPE_FREE) {
            memset(&in, 0, sizeof(in));
            in.type = type;
            in.links = 1;
            if (iwrite(i, &in)) return 0;
            if (permissions_store(i,type==FS_TYPE_DIR?0755:0644,0,0)) {
                memset(&in,0,sizeof(in));iwrite(i,&in);ino_note_free(i);return 0;
            }
            available_inodes--;
            states[selected].ino_hint = (i + 1 < sb.inode_count) ? i + 1 : FS_ROOT;
            return i;
        }
    }
    return 0;
}

static int legacy(void);

static u32 n_direct(void) { return legacy() ? (u32)NDIRECT : (u32)NDIRECT_V2; }

/* Сколько блоков файла адресует карта этой версии. */
static u32 max_file_blks(void) {
    u32 n = n_direct() + NINDIRECT + NDINDIRECT;
    if (!legacy()) n += NTINDIRECT;
    return n;
}

/* Один уровень косвенности. Новый блок собирается в памяти: с диска его
 * читать нельзя, balloc больше не зануляет сектор. */
static u32 map_level(u32 *root, u32 index, bool alloc, bool *created) {
    if (created) *created = false;
    if (index >= NINDIRECT) return 0;
    bool fresh = false;
    u32 born = 0;
    if (!*root) {
        if (!alloc) return 0;
        born = balloc();
        if (!born) return 0;
        fresh = true;
    }
    if (ind_load(&ind_level, fresh ? born : *root, fresh)) {
        if (fresh) bfree(born);
        return 0;
    }
    if (fresh) *root = born;
    if (!ind_level.data[index] && alloc) {
        u32 b = balloc();
        if (!b) return 0;
        ind_level.data[index] = b;
        ind_level.dirty = true;
        if (created) *created = true;
    }
    return ind_level.data[index];
}

static u32 map_double(u32 *root, u32 idx, bool alloc, bool *created) {
    if (created) *created = false;
    if (idx >= NDINDIRECT) return 0;
    bool fresh = false;
    u32 born = 0;
    if (!*root) {
        if (!alloc) return 0;
        born = balloc();
        if (!born) return 0;
        fresh = true;
    }
    if (ind_load(&ind_double, fresh ? born : *root, fresh)) {
        if (fresh) bfree(born);
        return 0;
    }
    if (fresh) *root = born;
    u32 slot = idx / NINDIRECT;
    if (!ind_double.data[slot]) {
        if (!alloc) return 0;
        u32 b = balloc();
        if (!b) return 0;
        if (ind_load(&ind_level, b, true)) { bfree(b); return 0; }
        ind_double.data[slot] = b;
        ind_double.dirty = true;
    }
    u32 mid = ind_double.data[slot];
    return map_level(&mid, idx % NINDIRECT, alloc, created);
}

static u32 map_triple(u32 *root, u32 idx, bool alloc, bool *created) {
    if (created) *created = false;
    if (idx >= NTINDIRECT) return 0;
    bool fresh = false;
    u32 born = 0;
    if (!*root) {
        if (!alloc) return 0;
        born = balloc();
        if (!born) return 0;
        fresh = true;
    }
    if (ind_load(&ind_triple, fresh ? born : *root, fresh)) {
        if (fresh) bfree(born);
        return 0;
    }
    if (fresh) *root = born;
    u32 slot = idx / NDINDIRECT;
    if (!ind_triple.data[slot]) {
        if (!alloc) return 0;
        u32 b = balloc();
        if (!b) return 0;
        if (ind_load(&ind_double, b, true)) { bfree(b); return 0; }
        ind_triple.data[slot] = b;
        ind_triple.dirty = true;
    }
    u32 child = ind_triple.data[slot];
    return map_double(&child, idx % NDINDIRECT, alloc, created);
}

/* created — блок только что выделен и на диске ещё мусор. */
static u32 bmap(struct inode *in, u32 idx, bool alloc, bool *created) {
    if (created) *created = false;
    u32 nd = n_direct();
    if (idx < nd) {
        if (!in->direct[idx] && alloc) {
            u32 b = balloc();
            if (!b) return 0;
            in->direct[idx] = b;
            if (created) *created = true;
        }
        return in->direct[idx];
    }
    idx -= nd;
    if (idx < NINDIRECT) {
        u32 slot = in->indirect;
        u32 b = map_level(&slot, idx, alloc, created);
        in->indirect = slot;
        return b;
    }
    idx -= NINDIRECT;
    if (idx < NDINDIRECT) {
        u32 slot = in->dindirect;
        u32 b = map_double(&slot, idx, alloc, created);
        in->dindirect = slot;
        return b;
    }
    if (legacy()) return 0;
    idx -= NDINDIRECT;
    {
        u32 slot = in->direct[NDIRECT_V2];
        u32 b = map_triple(&slot, idx, alloc, created);
        in->direct[NDIRECT_V2] = slot;
        return b;
    }
}

static void drop_new_block(struct inode *in, u32 idx, u32 block) {
    u32 nd = n_direct();
    if (idx < nd) {
        if (in->direct[idx] == block) in->direct[idx] = 0;
    } else {
        struct ind_cache *cs[3] = { &ind_level, &ind_double, &ind_triple };
        for (int k = 0; k < 3; k++) {
            struct ind_cache *c = cs[k];
            if (!c->valid || c->slot != selected) continue;
            for (u32 i = 0; i < NINDIRECT; i++) if (c->data[i] == block) {
                c->data[i] = 0;
                c->dirty = true;
            }
        }
    }
    bfree(block);
}

static void free_indirect_block(u32 block) {
    static u32 ind[NINDIRECT];
    if (!block) return;
    if (!read_ind(block, ind))
        for (u32 i = 0; i < NINDIRECT; i++) bfree(ind[i]);
    bfree(block);
}

static void free_double_block(u32 block) {
    static u32 top[NINDIRECT];
    u32 kids[NINDIRECT];
    if (!block) return;
    if (!read_ind(block, top)) {
        for (u32 j = 0; j < NINDIRECT; j++) kids[j] = top[j];
        for (u32 j = 0; j < NINDIRECT; j++) free_indirect_block(kids[j]);
    }
    bfree(block);
}

static void free_triple_block(u32 block) {
    static u32 top[NINDIRECT];
    u32 kids[NINDIRECT];
    if (!block) return;
    if (!read_ind(block, top)) {
        for (u32 j = 0; j < NINDIRECT; j++) kids[j] = top[j];
        for (u32 j = 0; j < NINDIRECT; j++) free_double_block(kids[j]);
    }
    bfree(block);
}

static void free_all_blocks(struct inode *in) {
    u32 nd = n_direct();
    for (u32 i = 0; i < nd; i++) { bfree(in->direct[i]); in->direct[i] = 0; }
    if (in->indirect) { free_indirect_block(in->indirect); in->indirect = 0; }
    if (in->dindirect) { free_double_block(in->dindirect); in->dindirect = 0; }
    if (nd == NDIRECT_V2 && in->direct[NDIRECT_V2]) {
        free_triple_block(in->direct[NDIRECT_V2]);
        in->direct[NDIRECT_V2] = 0;
    }
    in->size = 0;
}

/* ---------------------------------------------------------------- чтение/запись */

static int myfs_read_impl(u32 ino, u32 off, void *buf, u32 n) {
    struct inode in;
    int r;
    if (inocache.valid && inocache.slot == selected && inocache.ino == ino) {
        in = inocache.in;
    } else {
        r = iread(ino, &in);
        if (r) return r;
        inocache.slot = selected;
        inocache.ino = ino;
        inocache.in = in;
        inocache.valid = true;
    }
    if (off >= in.size) return 0;
    if (n > in.size - off) n = in.size - off;

    u8 *dst = buf;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done, bo = pos % BS, index = pos / BS;
        u32 chunk = BS - bo;
        if (chunk > n - done) chunk = n - done;
        u32 b = bmap(&in, index, false, NULL);
        if (b) {
            if (!(blkcache.valid && blkcache.slot == selected && blkcache.ino == ino && blkcache.index == index)) {
                if (bread(b, blkcache.data)) return FS_EIO;
                blkcache.slot = selected;
                blkcache.ino = ino;
                blkcache.index = index;
                blkcache.valid = true;
            }
            memcpy(dst + done, blkcache.data + bo, chunk);
        } else memset(dst + done, 0, chunk);    /* «дырка» в файле */
        done += chunk;
    }
    return done;
}

static int myfs_write_impl(u32 ino, u32 off, const void *buf, u32 n) {
    static u8 blk[BS];
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    u32 limit = max_file_blks() * BS;
    if (off > limit || n > limit - off) return FS_EFBIG;

    const u8 *src = buf;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done, bo = pos % BS;
        u32 chunk = BS - bo;
        if (chunk > n - done) chunk = n - done;
        bool created = false;
        u32 b = bmap(&in, pos / BS, true, &created);
        if (!b) { r = FS_ENOSPC; break; }
        if (chunk < BS) {
            if (created) memset(blk, 0, BS);
            else if (bread(b, blk)) { r = FS_EIO; break; }
        }
        memcpy(blk + bo, src + done, chunk);
        if (bwrite_data(b, blk)) {
            if (created) drop_new_block(&in, pos / BS, b);
            r = FS_EIO;
            break;
        }
        done += chunk;
    }
    if (off + done > in.size) in.size = off + done;
    myfs_touch(ino);
    int meta = sync_meta();
    int w = iwrite(ino, &in);
    if (r) return r;
    if (meta) return meta;
    return w ? w : (int)done;
}

static int myfs_truncate_impl(u32 ino) {
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (in.type != FS_TYPE_FILE) return FS_EISDIR;
    free_all_blocks(&in);
    myfs_touch(ino);
    int w = iwrite(ino, &in);
    int meta = sync_meta();
    if (w) return w;
    return meta;
}

static int myfs_stat_impl(u32 ino, struct fs_stat *st) {
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (in.type == FS_TYPE_FREE) return FS_ENOENT;
    st->type = in.type;
    st->links=in.links;
    st->size = in.size;
    r=permissions_load(ino,&in,&st->mode,&st->uid,&st->gid);if(r)return r;
    st->mtime=st->atime=st->ctime=times_load(ino);
    u32 data = (in.size + BS - 1) / BS;
    u32 nd = n_direct();
    st->blocks = data + (in.indirect ? 1 : 0);
    if (data > nd + NINDIRECT) {
        u32 past = data - nd - NINDIRECT;
        u32 in_double = past > NDINDIRECT ? NDINDIRECT : past;
        st->blocks += 1 + (in_double + NINDIRECT - 1) / NINDIRECT;
        if (!legacy() && past > NDINDIRECT) {
            u32 extra = past - NDINDIRECT;
            st->blocks += 1;
            st->blocks += (extra + NDINDIRECT - 1) / NDINDIRECT;
            st->blocks += (extra + NINDIRECT - 1) / NINDIRECT;
        }
    }
    return FS_OK;
}

/* ---------------------------------------------------------------- каталоги */

static int dir_check(u32 dir) {
    struct inode in;
    int r = iread(dir, &in);
    if (r) return r;
    if (in.type == FS_TYPE_FREE) return FS_ENOENT;
    return in.type == FS_TYPE_DIR ? FS_OK : FS_ENOTDIR;
}

static int legacy(void) { return sb.version==1 || (sb.version==3 && sb.layout_version==1); }
static u32 name_max(void) { return legacy() ? NAME_V1 : (u32)FS_NAME_MAX; }

/* Длина записи версии 2: заголовок, имя и нуль, округление вверх до 4. */
static u16 span_v2(unsigned namelen) {
    unsigned n = 8 + namelen + 1;
    return (u16)((n + 3u) & ~3u);
}

static int read_v1(u32 dir, u32 off, struct dirent_v1 *de) {
    int n = myfs_read(dir, off, de, sizeof(*de));
    if (n == 0) return 0;
    if (n != (int)sizeof(*de)) return FS_EIO;
    de->name[NAME_V1] = 0;
    return 1;
}

static int read_v2(u32 dir, u32 off, struct dirent_v2 *h, char *name) {
    int n = myfs_read(dir, off, h, sizeof(*h));
    if (n == 0) return 0;
    if (n != (int)sizeof(*h)) return FS_EIO;
    if (h->reclen < 12 || (h->reclen & 3) || (unsigned)h->namelen + 9u > h->reclen)
        return FS_EIO;
    if (name) {
        n = myfs_read(dir, off + 8, name, h->namelen + 1u);
        if (n != (int)h->namelen + 1) return FS_EIO;
        name[h->namelen] = 0;
    }
    return 1;
}

static int write_v2(u32 dir, u32 off, u32 ino, const char *name, u16 reclen) {
    unsigned namelen = strlen(name);
    u8 rec[8 + FS_NAME_MAX + 1];
    if (reclen < span_v2(namelen) || reclen > sizeof(rec)) return FS_EIO;
    memset(rec, 0, reclen);
    rec[0] = (u8)ino; rec[1] = (u8)(ino >> 8); rec[2] = (u8)(ino >> 16); rec[3] = (u8)(ino >> 24);
    rec[4] = (u8)reclen; rec[5] = (u8)(reclen >> 8);
    rec[6] = (u8)namelen;
    memcpy(rec + 8, name, namelen);
    int w = myfs_write(dir, off, rec, reclen);
    if (w < 0) return w;
    return w == (int)reclen ? FS_OK : FS_EIO;
}

/* Найти запись name. *ino = 0, если имени нет. Отрицательный код — ошибка чтения,
 * её нельзя путать с «не найдено»: иначе создание добавит вторую запись. */
static int dir_lookup(u32 dir, const char *name, u32 *ino, u32 *offset) {
    u32 off = 0;
    *ino = 0;
    if (legacy()) {
        struct dirent_v1 de;
        for (;;) {
            int r = read_v1(dir, off, &de);
            if (r < 0) return r;
            if (r == 0) return FS_OK;
            if (de.ino && !strcmp(de.name, name)) {
                if (offset) *offset = off;
                *ino = de.ino;
                return FS_OK;
            }
            off += sizeof(de);
        }
    }
    for (;;) {
        struct dirent_v2 h;
        char got[FS_NAME_MAX + 1];
        u32 start = off;
        int r = read_v2(dir, off, &h, got);
        if (r < 0) return r;
        if (r == 0) return FS_OK;
        if (h.ino && h.namelen == strlen(name) && !memcmp(got, name, h.namelen)) {
            if (offset) *offset = start;
            *ino = h.ino;
            return FS_OK;
        }
        off += h.reclen;
    }
}

static int dir_add(u32 dir, const char *name, u32 ino) {
    unsigned namelen = strlen(name);
    if (namelen > name_max()) return FS_ENAMETOOLONG;
    if (legacy()) {
        struct dirent_v1 de;
        u32 off = 0;
        for (;;) {
            int n = myfs_read(dir, off, &de, sizeof(de));
            if (n == 0) break;
            if (n != (int)sizeof(de)) return FS_EIO;
            if (!de.ino) break;
            off += sizeof(de);
        }
        memset(&de, 0, sizeof(de));
        de.ino = ino;
        memcpy(de.name, name, namelen + 1);
        int w = myfs_write(dir, off, &de, sizeof(de));
        return w < 0 ? w : FS_OK;
    }
    u16 need = span_v2(namelen);
    u32 off = 0;
    for (;;) {
        struct dirent_v2 h;
        u32 start = off;
        int r = read_v2(dir, off, &h, NULL);
        if (r < 0) return r;
        if (r == 0) return write_v2(dir, start, ino, name, need);
        if (!h.ino && h.reclen >= need) return write_v2(dir, start, ino, name, h.reclen);
        off += h.reclen;
    }
}

static int dir_set(u32 dir, u32 off, u32 ino, const char *name) {
    if (legacy()) {
        struct dirent_v1 de;
        memset(&de, 0, sizeof(de));
        de.ino = ino;
        if (name) memcpy(de.name, name, strlen(name) + 1);
        int w = myfs_write(dir, off, &de, sizeof(de));
        return w < 0 ? w : FS_OK;
    }
    if (!name) {
        u32 zero = 0;
        int w = myfs_write(dir, off, &zero, 4);   /* inode обнулён, длина записи сохранена */
        return w < 0 ? w : FS_OK;
    }
    struct dirent_v2 h;
    int r = read_v2(dir, off, &h, NULL);
    if (r <= 0) return r < 0 ? r : FS_EIO;
    if (h.reclen < span_v2(strlen(name))) return FS_ENOSPC;
    return write_v2(dir, off, ino, name, h.reclen);
}

/* 1 — пуст, 0 — есть записи, отрицательное — ошибка чтения. */
static int dir_empty(u32 dir) {
    u32 off = 0;
    if (legacy()) {
        struct dirent_v1 de;
        for (;;) {
            int r = read_v1(dir, off, &de);
            if (r < 0) return r;
            if (r == 0) return 1;
            if (de.ino && strcmp(de.name, ".") && strcmp(de.name, "..")) return 0;
            off += sizeof(de);
        }
    }
    for (;;) {
        struct dirent_v2 h;
        char name[FS_NAME_MAX + 1];
        int r = read_v2(dir, off, &h, name);
        if (r < 0) return r;
        if (r == 0) return 1;
        if (h.ino && strcmp(name, ".") && strcmp(name, "..")) return 0;
        off += h.reclen;
    }
}

static int myfs_readdir_impl(u32 dir, u32 *pos, struct fs_dirent *out) {
    u32 off = *pos;
    int r = dir_check(dir);
    if (r) return r;
    if (legacy()) {
        struct dirent_v1 de;
        for (;;) {
            r = read_v1(dir, off, &de);
            if (r <= 0) return r < 0 ? r : 0;
            off += sizeof(de);
            if (!de.ino || !strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
            *pos = off;
            out->ino = de.ino;
            memcpy(out->name, de.name, NAME_V1 + 1);
            return 1;
        }
    }
    for (;;) {
        struct dirent_v2 h;
        char name[FS_NAME_MAX + 1];
        r = read_v2(dir, off, &h, name);
        if (r <= 0) return r < 0 ? r : 0;
        off += h.reclen;
        if (!h.ino || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        *pos = off;
        out->ino = h.ino;
        memcpy(out->name, name, h.namelen + 1u);
        return 1;
    }
}

/* ---------------------------------------------------------------- пути */

/* Разбор пути: "/a/b", "a/b", "../x", "." — относительно cwd */
static int myfs_resolve_impl(u32 cwd, const char *path, u32 *ino) {
    if (!mounted) return FS_ENODEV;
    u32 cur = (path[0] == '/') ? FS_ROOT : cwd;
    char name[FS_NAME_MAX + 1];

    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        u32 len = 0;
        while (path[len] && path[len] != '/') len++;
        if (len > name_max()) return FS_ENAMETOOLONG;
        memcpy(name, path, len);
        name[len] = 0;
        path += len;

        int r = dir_check(cur);
        if (r) return r;
        u32 next;
        r = dir_lookup(cur, name, &next, NULL);
        if (r) return r;
        if (!next) return FS_ENOENT;
        cur = next;
    }
    *ino = cur;
    return FS_OK;
}

/* Разделить путь на родительский каталог и последнее имя */
static int resolve_parent(u32 cwd, const char *path, u32 *parent, char *name) {
    char buf[FS_PATH_MAX];
    u32 len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return FS_EINVAL;
    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = 0;   /* хвостовые '/' */

    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (buf[i] == '/') { slash = i; break; }

    const char *base = buf + slash + 1;
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) return FS_EINVAL;
    if (strlen(base) > name_max()) return FS_ENAMETOOLONG;
    memcpy(name, base, strlen(base) + 1);

    int r;
    if (slash < 0)       { *parent = cwd; r = FS_OK; }
    else if (slash == 0) { *parent = FS_ROOT; r = FS_OK; }
    else                 { buf[slash] = 0; r = myfs_resolve(cwd, buf, parent); }
    if (r) return r;
    return dir_check(*parent);
}

static int myfs_create_impl(u32 cwd, const char *path, int type, u32 *out) {
    if (!mounted) return FS_ENODEV;
    u32 parent;
    char name[FS_NAME_MAX + 1];
    int r = resolve_parent(cwd, path, &parent, name);
    if (r) return r;
    u32 existing;
    r = dir_lookup(parent, name, &existing, NULL);
    if (r) return r;
    if (existing) return FS_EEXIST;

    u32 ino = ialloc(type);
    if (!ino) return FS_ENOSPC;
    if (type == FS_TYPE_DIR) {
        if ((r = dir_add(ino, ".", ino)) || (r = dir_add(ino, "..", parent))) return r;
    }
    if ((r = dir_add(parent, name, ino))) return r;
    if (out) *out = ino;
    return FS_OK;
}

static int myfs_remove_impl(u32 cwd, const char *path,bool keep) {
    if (!mounted) return FS_ENODEV;
    u32 parent, off;
    char name[FS_NAME_MAX + 1];
    int r = resolve_parent(cwd, path, &parent, name);
    if (r) return r;
    u32 ino;
    r = dir_lookup(parent, name, &ino, &off);
    if (r) return r;
    if (!ino) return FS_ENOENT;

    struct inode in;
    if ((r = iread(ino, &in))) return r;
    if (in.type == FS_TYPE_DIR) {
        int empty = dir_empty(ino);
        if (empty < 0) return empty;
        if (!empty) return FS_ENOTEMPTY;
    }

    /* Publish unlink before reclamation. Open descriptors retain the inode. */
    if((r=dir_set(parent,off,0,NULL)))return r;
    in.links=0;
    if(!keep){free_all_blocks(&in);in.type=FS_TYPE_FREE;ino_note_free(ino);}
    if((r=iwrite(ino,&in)))return r;
    if(!keep)available_inodes++;
    r = sync_meta();
    return r;
}

static int myfs_rename_impl(u32 cwd, const char *src, const char *dst) {
    if (!mounted) return FS_ENODEV;
    u32 sparent, soff, dparent, existing;
    char sname[FS_NAME_MAX + 1], dname[FS_NAME_MAX + 1];
    int r = resolve_parent(cwd, src, &sparent, sname);
    if (r) return r;
    u32 ino;
    r = dir_lookup(sparent, sname, &ino, &soff);
    if (r) return r;
    if (!ino) return FS_ENOENT;

    /* mv file dir  ->  dir/file */
    if (myfs_resolve(cwd, dst, &existing) == FS_OK && dir_check(existing) == FS_OK) {
        dparent = existing;
        memcpy(dname, sname, sizeof(sname));
    } else if ((r = resolve_parent(cwd, dst, &dparent, dname))) {
        return r;
    }
    u32 taken, toff = 0;
    r = dir_lookup(dparent, dname, &taken, &toff);
    if (r) return r;
    if (taken) {
        /* POSIX: rename поверх существующего файла заменяет его. */
        if (taken == ino) return FS_OK;
        if (dir_check(taken) == FS_OK) return FS_EEXIST; /* каталог не затираем */
        struct inode victim;
        if ((r = iread(taken, &victim))) return r;
        if ((r = dir_set(dparent, toff, 0, NULL))) return r;
        victim.links = 0;
        free_all_blocks(&victim);
        victim.type = FS_TYPE_FREE;
        ino_note_free(taken);
        if ((r = iwrite(taken, &victim))) return r;
        available_inodes++;
    }

    /* нельзя переместить каталог внутрь самого себя */
    for (u32 p = dparent; ;) {
        if (p == ino) return FS_EINVAL;
        if (p == FS_ROOT) break;
        u32 parent;
        r = dir_lookup(p, "..", &parent, NULL);
        if (r) return r;
        if (!parent) return FS_EIO;
        p = parent;
    }

    if ((r = dir_add(dparent, dname, ino))) return r;
    if ((r = dir_set(sparent, soff, 0, NULL))) return r;

    u32 dotdot, dotdot_ino;
    if (dir_check(ino) == FS_OK && dparent != sparent) {
        r = dir_lookup(ino, "..", &dotdot_ino, &dotdot);
        if (r) return r;
        if (dotdot_ino) return dir_set(ino, dotdot, dparent, "..");
    }
    return FS_OK;
}

static int myfs_getpath_impl(u32 ino, char *buf, u32 len) {
    u32 chain[32];
    int depth = 0, r;
    while (ino != FS_ROOT) {
        if (depth == 32) return FS_EINVAL;
        chain[depth++] = ino;
        r = dir_lookup(ino, "..", &ino, NULL);
        if (r) return r;
        if (!ino) return FS_ENOENT;
    }
    u32 pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0; i--) {
        u32 parent = (i == depth - 1) ? FS_ROOT : chain[i + 1];
        struct fs_dirent de;
        u32 p = 0;
        bool found = false;
        while (myfs_readdir(parent, &p, &de) == 1) if (de.ino == chain[i]) { found = true; break; }
        if (!found) return FS_ENOENT;
        u32 n = strlen(de.name);
        if (pos + n + 2 > len) return FS_ENAMETOOLONG;
        memcpy(buf + pos, de.name, n);
        pos += n;
        if (i) buf[pos++] = '/';
    }
    buf[pos] = 0;
    return FS_OK;
}

/* ---------------------------------------------------------------- монтирование */

static int myfs_format_impl(u32 version) {
    static u8 zero[BS];
    cache_drop_all();
    u32 total = block_get(states[selected].device)->sectors;
    if (!total) return FS_ENODEV;
    if (version < VERSION_MIN || version > VERSION) return FS_EINVAL;
    if (total > MAX_BLOCKS) return FS_EFBIG;
    int allocation=prepare_bitmap(((total + BITS_PER_BLK - 1) / BITS_PER_BLK)*BS);
    if(allocation) return allocation;

    memset(&sb, 0, sizeof(sb));
    sb.magic         = MAGIC;
    sb.version       = version;
    sb.layout_version = version==1 ? 1 : 2;
    sb.total_blocks  = total;
    sb.bitmap_start  = 1;
    sb.bitmap_blocks = (total + BITS_PER_BLK - 1) / BITS_PER_BLK;
    sb.inode_start   = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_count   = total / 128;
    if(sb.inode_count < MIN_INODES) sb.inode_count=MIN_INODES;
    if(sb.inode_count > MAX_INODES) sb.inode_count=MAX_INODES;
    sb.inode_count = (sb.inode_count + INODES_PER_BLK - 1) / INODES_PER_BLK * INODES_PER_BLK;
    sb.inode_blocks  = sb.inode_count / INODES_PER_BLK;
    sb.data_start    = sb.inode_start + sb.inode_blocks;
    if (sb.data_start + 16 > total) return FS_ENOSPC;

    /* битмап: служебные блоки заняты */
    memset(bitmap, 0, sb.bitmap_blocks * BS);
    for (u32 b = 0; b < sb.data_start; b++) bit_set(b, true);
    for (u32 i = 0; i < sb.bitmap_blocks; i++)
        if (bwrite(sb.bitmap_start + i, bitmap + i * BS)) return FS_EIO;
    for (u32 i = 0; i < sb.inode_blocks; i++)
        if (bwrite(sb.inode_start + i, zero)) return FS_EIO;
    if (bwrite(0, &sb)) return FS_EIO;

    states[selected].hint=sb.data_start;
    states[selected].ino_hint=FS_ROOT+1;
    available_blocks = total - sb.data_start;
    available_inodes = sb.inode_count - 1;
    mounted = true;

    /* корневой каталог: inode 1, «..» указывает на себя */
    struct inode root;
    memset(&root, 0, sizeof(root));
    root.type = FS_TYPE_DIR;
    root.links = 1;
    if (iwrite(FS_ROOT, &root)) return FS_EIO;
    if(version==3 && permissions_store(FS_ROOT,0755,0,0)) return FS_EIO;
    available_inodes--;
    int r;
    if ((r = dir_add(FS_ROOT, ".", FS_ROOT)) || (r = dir_add(FS_ROOT, "..", FS_ROOT))) return r;
    return FS_OK;
}

static int myfs_init_impl(bool *formatted, bool allow_format) {
    cache_drop_all();
    *formatted = false;
    mounted = false;
    if (!block_get(states[selected].device)->sectors) return FS_ENODEV;
    if (bread(0, &sb)) return FS_EIO;

    if (sb.magic != MAGIC) {
        if (!allow_format) return FS_EINVAL;
        const u8 *raw = (const u8 *)&sb;
        for (u32 i=0; i<BS; i++) if (raw[i]) return FS_EINVAL;
        *formatted = true;
        return myfs_format();
    }

    if (sb.version < VERSION_MIN || sb.version > VERSION || sb.total_blocks > MAX_BLOCKS || sb.total_blocks > block_get(states[selected].device)->sectors ||
        sb.bitmap_start != 1 || sb.bitmap_blocks != (sb.total_blocks + BITS_PER_BLK - 1) / BITS_PER_BLK ||
        sb.inode_start != 1 + sb.bitmap_blocks || (sb.inode_count < MIN_INODES || sb.inode_count > MAX_INODES || sb.inode_count % INODES_PER_BLK) ||
        sb.inode_blocks != sb.inode_count / INODES_PER_BLK || sb.data_start != sb.inode_start + sb.inode_blocks ||
        sb.data_start >= sb.total_blocks) return FS_EINVAL;
    int allocation=prepare_bitmap(sb.bitmap_blocks * BS);
    if(allocation) return allocation;
    states[selected].hint=sb.data_start;
    states[selected].ino_hint=0;
    for (u32 i = 0; i < sb.bitmap_blocks; i++)
        if (bread(sb.bitmap_start + i, bitmap + i * BS)) return FS_EIO;
    if(sb.version==3 && ((sb.layout_version!=1 && sb.layout_version!=2) ||
       (sb.permissions_root && !permissions_block_valid(sb.permissions_root)))) return FS_EINVAL;
    mounted = true;

    available_blocks = 0;
    for (u32 b = sb.data_start; b < sb.total_blocks; b++) if (!bit_get(b)) available_blocks++;
    available_inodes = 0;
    struct inode in;
    for (u32 i = FS_ROOT; i < sb.inode_count; i++) {
        if(iread(i,&in))return FS_EIO;
        if(in.type==FS_TYPE_FREE){available_inodes++;if(!states[selected].ino_hint)states[selected].ino_hint=i;}
        else if(i!=FS_ROOT && !in.links){free_all_blocks(&in);in.type=FS_TYPE_FREE;ino_note_free(i);if(iwrite(i,&in))return FS_EIO;available_inodes++;}
    }
    return sync_meta();
}

bool myfs_ready(void) { return mounted; }

static void myfs_info_impl(struct fs_info *info) {
    info->total_blocks = sb.total_blocks - sb.data_start;
    info->free_blocks  = available_blocks;
    info->total_inodes = sb.inode_count - 1;
    info->free_inodes  = available_inodes;
}

const char *myfs_strerror(int err) {
    switch (err) {
    case FS_OK:           return "success";
    case FS_ENOENT:       return "no such file or directory";
    case FS_EEXIST:       return "already exists";
    case FS_ENOTDIR:      return "not a directory";
    case FS_EISDIR:       return "is a directory";
    case FS_ENOSPC:       return "no space left on disk";
    case FS_ENOTEMPTY:    return "directory not empty";
    case FS_EINVAL:       return "invalid argument";
    case FS_EIO:          return "disk I/O error";
    case FS_ENODEV:       return "no disk / file system not mounted";
    case FS_EFBIG:        return "file or filesystem too large";
    case FS_ENAMETOOLONG: return "name too long";
    case FS_ELOOP:        return "too many levels of symbolic links";
    case FS_EACCES: return "permission denied";
    case FS_EPERM: return "operation not permitted";
    default:              return "unknown error";
    }
}

/* ---------------------------------------------------------------- блокировка
 * Файловая система общая для всех задач, а операции с ней долгие (диск!),
 * поэтому вместо запрета прерываний — мьютекс: ждущая задача спит, а не крутится.
 * Мьютекс рекурсивный: myfs_create внутри себя вызывает myfs_write и т.п. */
/* Private backend: all kernel access is serialized by the VFS lock. */
int myfs_init(bool *formatted, bool allow_format) { int r = myfs_init_impl(formatted, allow_format); return r; }
int myfs_format(void) { int r = myfs_format_impl(VERSION); return r; }
#ifdef HOST_TEST
int myfs_format_legacy(void) { int r = myfs_format_impl(1); return r; }
#endif
int myfs_resolve(u32 cwd, const char *path, u32 *ino) { int r = myfs_resolve_impl(cwd, path, ino); return r; }
int myfs_create(u32 cwd, const char *path, int type, u32 *out) { int r = myfs_create_impl(cwd, path, type, out); return r; }
int myfs_remove(u32 cwd, const char *path) { int r = myfs_remove_impl(cwd, path,false); return r; }
int myfs_remove_keep(u32 cwd,const char *path,bool keep){return myfs_remove_impl(cwd,path,keep);}
int myfs_reap_orphan(u32 ino) {
    struct inode in;int ret=iread(ino,&in);if(ret)return ret;
    if(ino==FS_ROOT || in.type==FS_TYPE_FREE || in.links)return FS_EINVAL;
    free_all_blocks(&in);in.type=FS_TYPE_FREE;ino_note_free(ino);ret=iwrite(ino,&in);
    if(ret)return ret;
    available_inodes++;
    return sync_meta();
}
int myfs_rename(u32 cwd, const char *src, const char *dst) { int r = myfs_rename_impl(cwd, src, dst); return r; }
int myfs_read(u32 ino, u32 off, void *buf, u32 n) { int r = myfs_read_impl(ino, off, buf, n); return r; }
int myfs_write(u32 ino, u32 off, const void *buf, u32 n) { int r = myfs_write_impl(ino, off, buf, n); return r; }
int myfs_truncate(u32 ino) { int r = myfs_truncate_impl(ino); return r; }
int myfs_set_size(u32 ino, u32 size) {
    struct inode in;
    int r = iread(ino, &in);
    if (r) return r;
    if (in.type != FS_TYPE_FILE) return FS_EISDIR;
    if (size > in.size) return FS_EINVAL;
    in.size = size;
    myfs_touch(ino);
    r = iwrite(ino, &in);
    if (r) return r;
    return sync_meta();
}
int myfs_stat(u32 ino, struct fs_stat *st) { int r = myfs_stat_impl(ino, st); return r; }
int myfs_readdir(u32 dir, u32 *pos, struct fs_dirent *out) { int r = myfs_readdir_impl(dir, pos, out); return r; }
int myfs_getpath(u32 ino, char *buf, u32 len) { int r = myfs_getpath_impl(ino, buf, len); return r; }
void myfs_info(struct fs_info *info) { myfs_info_impl(info); }






