/* Пользовательские процессы: загрузка ELF и подготовка адресного пространства.
 *
 * Адресное пространство новой программы:
 *
 *   0x40000000  .text .rodata      (только чтение)    ┐ из ELF-файла
 *   0x4000x000  .data .bss         (чтение и запись)  ┘
 *   brk ...     куча программы, растёт через sbrk() до 0x80000000
 *   0x80000000  динамический загрузчик, если в ELF есть PT_INTERP
 *   0xA0000000  общая память shm
 *   0xB0000000  mmap (анонимные страницы и частные копии файлов)
 *   0xBFF00000  стек программы, 1 МиБ, растёт вниз
 *   0xBFFFFFFF  на вершине: строки аргументов, argv[], argc
 *               у программы с PT_INTERP ещё envp и auxv
 *
 * Файл, который начинается с #!, — скрипт. Запускается его интерпретатор,
 * в argv попадают путь интерпретатора, необязательный аргумент из первой
 * строки и путь самого скрипта. Цепочка скриптов ограничена четырьмя шагами.
 */
#include "process.h"
#include "ipc.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "fs.h"
#include "string.h"
#include "vga.h"
#include "file.h"

/* ---- формат ELF (только то, что нужно) ---- */
struct elf_header {
    u8  ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_phdr {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));

#define PT_LOAD   1
#define PT_INTERP 3
#define PF_W      2
#define ET_EXEC   2
#define ET_DYN    3
#define EM_386    3

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

#define STACK_BOTTOM USER_STACK_BOTTOM
#define EXEC_MAX      (8u * 1024 * 1024)   /* больше в кучу ядра не читаем */
#define EXEC_MAX_PHDR 64
#define SHEBANG_LINE  256                  /* первая строка #! не длиннее этого */
#define SHEBANG_DEPTH 4                    /* сколько скриптов можно вложить */

static volatile u32 foreground;

const char *proc_strerror(int err) {
    switch (err) {
    case PROC_ENOENT:  return "no such file";
    case PROC_ENOEXEC: return "not a MyOS executable (ELF32, i386)";
    case PROC_ENOMEM:  return "out of memory";
    case PROC_EAGAIN:  return "too many tasks";
    case PROC_EISDIR:  return "is a directory";
    case PROC_ELOOP:   return "too many shebang interpreters";
    default:           return "error";
    }
}

/* скопировать байты в чужое адресное пространство — через физические адреса */
static bool copy_to_dir(u32 dir, u32 va, const void *src, u32 n) {
    const u8 *s = src;
    while (n) {
        u32 phys;
        if (!vmm_lookup(dir, va, &phys, NULL)) return false;
        u32 chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        memcpy((void *)phys, s, chunk);          /* RAM < 1 ГБ отображена 1:1 */
        va += chunk; s += chunk; n -= chunk;
    }
    return true;
}

/* отобразить новые обнулённые страницы в [start, end).
 * reuse: страница от соседнего сегмента того же файла — только дополнить права. */
static bool map_zero_pages(u32 dir, u32 start, u32 end, u32 flags, u32 *pages, bool reuse) {
    for (u32 va = start & ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
        u32 old_flags, phys;
        if (vmm_lookup(dir, va, &phys, &old_flags)) {
            if (!reuse) return false;
            if ((flags & PG_WRITE) && !(old_flags & PG_WRITE))
                vmm_map_in(dir, va, phys & ~(PAGE_SIZE - 1), flags);
            continue;
        }
        u32 frame = pmm_alloc();
        if (!frame) return false;
        memset((void *)frame, 0, PAGE_SIZE);
        if (vmm_map_in(dir, va, frame, flags)) { pmm_free(frame); return false; }
        (*pages)++;
    }
    return true;
}

static const char *basename(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

struct image {
    u32 dir, entry, sp, brk, pages;
};

struct elfmap {
    u32 base, entry, phdr, phnum, end;
};

/* ET_DYN с адресами меньше базы грузится от неё; уже абсолютный образ — как есть */
static u32 elf_bias(struct elf_phdr *ph, u32 nph, u32 preferred) {
    u32 top = 0;
    for (u32 i = 0; i < nph; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        u32 end = ph[i].vaddr + ph[i].memsz;
        if (end > top) top = end;
    }
    return top <= preferred ? preferred : 0;
}

static int read_exact(u32 ino, u32 off, void *buf, u32 n) {
    return fs_read(ino, off, buf, n) == (int)n ? 0 : PROC_ENOEXEC;
}

/* Заголовки программы. Сам файл в кучу ядра не копируется. */
static int load_phdrs(u32 ino, u32 size, struct elf_header *eh, struct elf_phdr **out) {
    if (size < sizeof(*eh)) return PROC_ENOEXEC;
    if (size > EXEC_MAX) return PROC_ENOMEM;
    if (read_exact(ino, 0, eh, sizeof(*eh))) return PROC_ENOEXEC;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != 1 || eh->ident[5] != 1 || eh->machine != EM_386 ||
        eh->phentsize != sizeof(struct elf_phdr) || !eh->phnum || eh->phnum > EXEC_MAX_PHDR)
        return PROC_ENOEXEC;
    u32 bytes = eh->phnum * (u32)sizeof(struct elf_phdr);
    if (eh->phoff > size || bytes > size - eh->phoff) return PROC_ENOEXEC;
    struct elf_phdr *ph = kmalloc(bytes);
    if (!ph) return PROC_ENOMEM;
    if (read_exact(ino, eh->phoff, ph, bytes)) { kfree(ph); return PROC_ENOEXEC; }
    *out = ph;
    return 0;
}

static int map_elf(u32 ino, u32 size, const struct elf_header *eh, const struct elf_phdr *ph,
                   u32 dir, u32 bias, bool reuse, u32 *pages, struct elfmap *out) {
    u32 end_max = 0;
    u8 chunk[512];
    for (u32 i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        u32 start = bias + ph[i].vaddr, end = start + ph[i].memsz;
        bool interp = start >= INTERP_BASE;
        if (end < start || start < USER_BASE || ph[i].filesz > ph[i].memsz ||
            ph[i].filesz > size || ph[i].offset > size - ph[i].filesz ||
            (interp ? end > SHM_BASE : end > INTERP_BASE))
            return PROC_ENOEXEC;
        u32 flags = PG_USER | ((ph[i].flags & PF_W) ? PG_WRITE : 0);
        if (!map_zero_pages(dir, start, end, flags, pages, reuse)) return PROC_ENOMEM;
        u32 left = ph[i].filesz, off = ph[i].offset, va = start;
        while (left) {
            u32 n = left > sizeof(chunk) ? (u32)sizeof(chunk) : left;
            if (read_exact(ino, off, chunk, n)) return PROC_ENOEXEC;
            if (!copy_to_dir(dir, va, chunk, n)) return PROC_ENOMEM;
            off += n; va += n; left -= n;
        }
        if (end > end_max) end_max = end;
    }
    if (!end_max) return PROC_ENOEXEC;
    u32 entry = bias + eh->entry;
    if (entry < USER_BASE || entry >= SHM_BASE) return PROC_ENOEXEC;
    out->base = bias;
    out->entry = entry;
    /* e_phoff — смещение в файле. У ET_EXEC виртуальный адрес заголовков — это
     * vaddr сегмента, в который они попали, а не само смещение. */
    u32 phdr = bias + eh->phoff;
    for (u32 i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || !ph[i].filesz) continue;
        if (eh->phoff < ph[i].offset || ph[i].filesz > size || ph[i].offset > size - ph[i].filesz ||
            eh->phoff >= ph[i].offset + ph[i].filesz) continue;
        phdr = bias + ph[i].vaddr + (eh->phoff - ph[i].offset);
        break;
    }
    out->phdr = phdr;
    out->phnum = eh->phnum;
    out->end = end_max;
    return 0;
}

static const char *default_env[] = { "PATH=/bin:/ext/bin", "HOME=/", "TERM=myos", 0 };

/* Упаковать KEY=VAL в один блок ядра. vec == NULL оставляет пустое окружение. */
static int pack_env(const char **vec, char **buf_out, u32 *len_out) {
    u32 n = 0;
    int count = 0;
    if (vec) for (; vec[count]; count++) n += strlen(vec[count]) + 1;
    char *buf = NULL;
    if (n) {
        buf = kmalloc(n);
        if (!buf) return PROC_ENOMEM;
        char *p = buf;
        for (int i = 0; i < count; i++) {
            u32 L = strlen(vec[i]) + 1;
            memcpy(p, vec[i], L);
            p += L;
        }
    }
    *buf_out = buf;
    *len_out = n;
    return 0;
}

static int unpack_env(const char *buf, u32 len, char ***out, int *n_out) {
    int count = 0;
    for (u32 i = 0; i < len; i++) if (buf[i] == 0) count++;
    char **vec = kmalloc((count + 1) * sizeof(char *));
    if (!vec) return PROC_ENOMEM;
    int k = 0;
    u32 start = 0;
    for (u32 i = 0; i < len; i++) if (buf[i] == 0) {
        vec[k++] = (char *)(buf + start);
        start = i + 1;
    }
    vec[k] = 0;
    *out = vec;
    *n_out = k;
    return 0;
}

static void env_replace(struct task *t, char *buf, u32 len) {
    t = task_process(t);
    kfree(t->env_buf);
    t->env_buf = buf;
    t->env_len = len;
}

static int place_strings(u32 dir, u32 *sp, char **v, int n, u32 *ptrs) {
    ptrs[n] = 0;
    for (int i = n - 1; i >= 0; i--) {
        u32 len = strlen(v[i]) + 1;
        if (*sp < STACK_BOTTOM + len + 4096) return PROC_ENOMEM;
        *sp -= len;
        if (!copy_to_dir(dir, *sp, v[i], len)) return PROC_ENOMEM;
        ptrs[i] = *sp;
    }
    return 0;
}

/* Динамическая программа: стек Linux (argc, argv, envp, auxv).
 * Статическая: _start(argc, argv, envp), esp+4 кратен 16. */
static int build_user_stack(u32 dir, int argc, char **argv, int envc, char **env,
                            struct elfmap *prog, u32 interp, int with_aux, u32 *sp_out) {
    u32 sp = USER_TOP;
    u32 *ap = kmalloc(((u32)argc + 1) * 4);
    u32 *ep = kmalloc(((u32)envc + 1) * 4);
    if (!ap || !ep) { kfree(ap); kfree(ep); return PROC_ENOMEM; }
    int err = place_strings(dir, &sp, argv, argc, ap);
    if (!err) err = place_strings(dir, &sp, env, envc, ep);
    if (err) { kfree(ap); kfree(ep); return err; }
    if (!with_aux) {
        u32 argv_addr = (sp - 4 * ((u32)argc + 1)) & ~15u;
        u32 env_addr = (argv_addr - 4 * ((u32)envc + 1)) & ~15u;
        if (env_addr < STACK_BOTTOM + 64 ||
            !copy_to_dir(dir, argv_addr, ap, 4 * ((u32)argc + 1)) ||
            !copy_to_dir(dir, env_addr, ep, 4 * ((u32)envc + 1))) {
            kfree(ap); kfree(ep); return PROC_ENOMEM;
        }
        u32 frame[4] = { 0, (u32)argc, argv_addr, env_addr };
        u32 dest = env_addr - 20;               /* dest+4 кратен 16, кадр не задевает envp */
        if (!copy_to_dir(dir, dest, frame, sizeof(frame))) { kfree(ap); kfree(ep); return PROC_ENOMEM; }
        *sp_out = dest;
        kfree(ap); kfree(ep);
        return 0;
    }
    int nvec = 1 + argc + 1 + envc + 1 + 14;
    u32 *vec = kmalloc((u32)nvec * 4);
    if (!vec) { kfree(ap); kfree(ep); return PROC_ENOMEM; }
    int n = 0;
    vec[n++] = (u32)argc;
    for (int i = 0; i <= argc; i++) vec[n++] = ap[i];
    for (int i = 0; i <= envc; i++) vec[n++] = ep[i];
    vec[n++] = AT_PHDR;   vec[n++] = prog->phdr;
    vec[n++] = AT_PHENT;  vec[n++] = sizeof(struct elf_phdr);
    vec[n++] = AT_PHNUM;  vec[n++] = prog->phnum;
    vec[n++] = AT_PAGESZ; vec[n++] = PAGE_SIZE;
    vec[n++] = AT_BASE;   vec[n++] = interp;
    vec[n++] = AT_ENTRY;  vec[n++] = prog->entry;
    vec[n++] = AT_NULL;   vec[n++] = 0;
    u32 bytes = (u32)n * 4;
    u32 dest = (sp - bytes) & ~15u;
    int rc = PROC_ENOMEM;
    if (dest >= STACK_BOTTOM && copy_to_dir(dir, dest, vec, bytes)) { *sp_out = dest; rc = 0; }
    kfree(vec); kfree(ap); kfree(ep);
    return rc;
}

static char *kdup(const char *s) {
    u32 n = strlen(s) + 1;
    char *p = kmalloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* 0 — это не скрипт. 1 — разобрали интерпретатор. Иначе PROC_*.
 * Необязательный аргумент — весь хвост строки одним словом, как в Linux. */
static int parse_shebang(u32 ino, u32 size, char *interp, char *arg, bool *has_arg) {
    char buf[SHEBANG_LINE];
    *has_arg = false;
    interp[0] = arg[0] = 0;
    if (size < 2) return 0;
    u32 n = size < SHEBANG_LINE ? size : SHEBANG_LINE;
    if (read_exact(ino, 0, buf, n)) return PROC_ENOEXEC;
    if (buf[0] != '#' || buf[1] != '!') return 0;
    u32 end = 2;
    while (end < n && buf[end] != '\n') end++;
    if (end == n && size > n) return PROC_ENOEXEC;
    while (end > 2 && (buf[end - 1] == ' ' || buf[end - 1] == '\t' || buf[end - 1] == '\r')) end--;
    u32 i = 2;
    while (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i >= end) return PROC_ENOEXEC;
    u32 start = i;
    while (i < end && buf[i] != ' ' && buf[i] != '\t') i++;
    u32 ilen = i - start;
    if (!ilen || ilen >= SHEBANG_LINE) return PROC_ENOEXEC;
    memcpy(interp, buf + start, ilen);
    interp[ilen] = 0;
    while (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i < end) {
        u32 alen = end - i;
        if (alen >= SHEBANG_LINE) return PROC_ENOEXEC;
        memcpy(arg, buf + i, alen);
        arg[alen] = 0;
        *has_arg = true;
    }
    return 1;
}

/* Загрузить ELF в НОВОЕ адресное пространство и положить на стек argc/argv.
 * argv — строки в памяти ядра. Файл читается кусками по 512 байт: в куче
 * ядра остаются только заголовки программы, не весь образ.
 * script и user_argv — то, что передал exec: при вложенном #! имя скрипта
 * и исходные аргументы не подменяются промежуточным интерпретатором. */
static int load_image_impl(const char *path, u32 cwd, int argc, char **argv, char **env, struct image *im,
                           const char *script, int user_argc, char **user_argv, int depth) {
    u32 ino;
    struct fs_stat st;
    int resolved=fs_resolve(cwd,path,&ino);
    if(resolved)return resolved==FS_EACCES?PROC_EACCES:PROC_ENOENT;
    if(fs_stat(ino,&st))return PROC_ENOENT;
    if (st.type == FS_TYPE_DIR) return PROC_EISDIR;
    if(fs_access(ino,1,false))return PROC_EACCES;

    char bang_interp[SHEBANG_LINE], bang_arg[SHEBANG_LINE];
    bool bang_arg_set = false;
    int bang = parse_shebang(ino, st.size, bang_interp, bang_arg, &bang_arg_set);
    if (bang < 0) return bang;
    if (bang > 0) {
        if (depth >= SHEBANG_DEPTH) return PROC_ELOOP;
        int tail = user_argc > 1 ? user_argc - 1 : 0;
        int nargc = 2 + (bang_arg_set ? 1 : 0) + tail;
        if (nargc > PROC_MAX_ARGS) return PROC_ENOMEM;
        char **nav = kmalloc((u32)(nargc + 1) * sizeof(char *));
        char *ip = kdup(bang_interp);
        char *ap = bang_arg_set ? kdup(bang_arg) : NULL;
        if (!nav || !ip || (bang_arg_set && !ap)) {
            kfree(nav); kfree(ip); kfree(ap);
            return PROC_ENOMEM;
        }
        int k = 0;
        nav[k++] = ip;
        if (bang_arg_set) nav[k++] = ap;
        nav[k++] = (char *)script;
        for (int i = 1; i < user_argc; i++) nav[k++] = user_argv[i];
        nav[k] = NULL;
        int err = load_image_impl(bang_interp, cwd, k, nav, env, im, script, user_argc, user_argv, depth + 1);
        kfree(nav); kfree(ip); kfree(ap);
        return err;
    }

    struct elf_header eh;
    struct elf_phdr *ph = NULL, *lph = NULL;
    u32 dir = 0, pages = 0;
    int err = load_phdrs(ino, st.size, &eh, &ph);
    if (err) return err;
    if (eh.type != ET_EXEC && eh.type != ET_DYN) { err = PROC_ENOEXEC; goto fail; }

    err = PROC_ENOMEM;
    dir = vmm_create_dir();
    if (!dir) goto fail;

    u32 bias = eh.type == ET_DYN ? elf_bias(ph, eh.phnum, USER_BASE) : 0;
    struct elfmap prog;
    err = map_elf(ino, st.size, &eh, ph, dir, bias, true, &pages, &prog);
    if (err) goto fail;
    u32 brk = (prog.end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (brk > INTERP_BASE) brk = INTERP_BASE;

    /* PT_INTERP: путь к загрузчику внутри файла, сам загрузчик — ET_DYN от INTERP_BASE */
    char interp[128];
    interp[0] = 0;
    for (u32 i = 0; i < eh.phnum; i++) {
        if (ph[i].type != PT_INTERP) continue;
        if (!ph[i].filesz || ph[i].filesz >= sizeof(interp) ||
            ph[i].filesz > st.size || ph[i].offset > st.size - ph[i].filesz) {
            err = PROC_ENOEXEC;
            goto fail;
        }
        if (read_exact(ino, ph[i].offset, interp, ph[i].filesz)) { err = PROC_ENOEXEC; goto fail; }
        bool nul = false;
        for (u32 k = 0; k < ph[i].filesz; k++) if (interp[k] == 0) nul = true;
        if (!nul) { err = PROC_ENOEXEC; goto fail; }
        break;
    }

    u32 entry = prog.entry;
    u32 sp = 0;
    if (!map_zero_pages(dir, STACK_BOTTOM, USER_TOP, PG_USER | PG_WRITE, &pages, false)) {
        err = PROC_ENOMEM;
        goto fail;
    }
    if (argc > PROC_MAX_ARGS) argc = PROC_MAX_ARGS;
    int envc = 0;
    if (env) while (env[envc]) envc++;

    if (interp[0]) {
        u32 lino;
        struct fs_stat lst;
        struct elf_header leh;
        if (fs_resolve(cwd, interp, &lino) || fs_stat(lino, &lst) || lst.type != FS_TYPE_FILE || fs_access(lino,1,false)) {
            err = PROC_ENOENT;
            goto fail;
        }
        err = load_phdrs(lino, lst.size, &leh, &lph);
        if (err) goto fail;
        if (leh.type != ET_DYN) { err = PROC_ENOEXEC; goto fail; }
        struct elfmap lm;
        u32 lbias = elf_bias(lph, leh.phnum, INTERP_BASE);
        err = map_elf(lino, lst.size, &leh, lph, dir, lbias, false, &pages, &lm);
        if (err) goto fail;
        entry = lm.entry;
        u32 phys;
        if (!vmm_lookup(dir, prog.phdr, &phys, NULL)) { err = PROC_ENOEXEC; goto fail; }
        err = build_user_stack(dir, argc, argv, envc, env, &prog, lm.base, 1, &sp);
        if (err) goto fail;
    } else {
        err = build_user_stack(dir, argc, argv, envc, env, &prog, 0, 0, &sp);
        if (err) goto fail;
    }

    im->dir = dir;
    im->entry = entry;
    im->sp = sp;
    im->brk = brk;
    im->pages = pages;
    kfree(ph);
    kfree(lph);
    return 0;

fail:
    if (dir) vmm_destroy_dir(dir);
    kfree(ph);
    kfree(lph);
    return err;
}

/* Pin the namespace while ELF headers, segments and interpreter are read.
 * Otherwise unlink/reuse between reads could substitute another owner's inode. */
static int load_image(const char *path,u32 cwd,int argc,char **argv,char **env,struct image *im) {
    fs_namespace_lock();
    int ret=load_image_impl(path,cwd,argc,argv,env,im,path,argc,argv,0);
    fs_namespace_unlock();
    return ret;
}

static int spawn(const char *path, u32 cwd, int argc, char **argv, char **env, bool fg, struct file *log, struct file *input, struct task *parent, bool service_root) {
    char **use = env;
    char **owned = NULL;
    int envc = 0;
    if (!use && parent && task_process(parent)->env_buf) {
        if (unpack_env(task_process(parent)->env_buf, task_process(parent)->env_len, &owned, &envc)) return PROC_ENOMEM;
        use = owned;
    }
    if (!use) use = (char **)default_env;
    struct image im;
    memset(&im, 0, sizeof(im));
    int err = load_image(path, cwd, argc, argv, use, &im);
    char *packed = NULL;
    u32 plen = 0;
    if (!err) err = pack_env((const char **)use, &packed, &plen);
    kfree(owned);
    if (err) {
        if (im.dir) vmm_destroy_dir(im.dir);
        kfree(packed);
        return err;
    }
    struct task *t = task_alloc_user(basename(path), im.dir, im.entry, im.sp);
    if (!t) { kfree(packed); vmm_destroy_dir(im.dir); return PROC_ENOMEM; }
    task_process(t)->brk_start = task_process(t)->brk = im.brk;
    task_process(t)->user_pages = im.pages;
    task_process(t)->cwd = cwd;
    task_process(t)->creation_mask=022;
    env_replace(t, packed, plen);
    if (parent) {
        task_process(t)->uid = task_process(parent)->uid;
    task_process(t)->real_uid = task_process(parent)->real_uid;
    task_process(t)->saved_uid = task_process(parent)->saved_uid;
    task_process(t)->cloexec = task_process(parent)->cloexec;
    task_process(t)->fs_root = task_process(parent)->fs_root;
    task_process(t)->gid = task_process(parent)->gid;
    task_process(t)->real_gid = task_process(parent)->real_gid;
    task_process(t)->saved_gid = task_process(parent)->saved_gid;
    task_process(t)->creation_mask = task_process(parent)->user?task_process(parent)->creation_mask:022;
        task_process(t)->authenticated = task_process(parent)->authenticated;
        t->parent = parent->id;
        t->pgid = parent->pgid;
        for (int i=0; i<MAX_FDS; i++) {
            file_close(task_process(t)->fds[i]);
            task_process(t)->fds[i] = file_dup(task_process(parent)->fds[i]);
        }
    }
    /* Only the service manager requests this, after validating its root-owned unit.
     * Set the identity before publishing the task to any CPU. */
    if (service_root) {
        t->authenticated=true;
        t->uid=t->real_uid=t->saved_uid=0;
        t->gid=t->real_gid=t->saved_gid=0;
    }
    if (input) { file_close(task_process(t)->fds[0]); task_process(t)->fds[0] = file_dup(input); }
    if (log) {
        for (int i = 1; i <= 2; i++) { file_close(task_process(t)->fds[i]); task_process(t)->fds[i] = file_dup(log); }
    }
    t->keep_zombie = fg;                                 /* оболочка заберёт код выхода */
    u32 id = task_start(t);                              /* при неудаче сама освободит всё */
    if (!id) return PROC_EAGAIN;
    return id;
}

int proc_spawn(const char *path, u32 cwd, int argc, char **argv, bool fg) {
    return spawn(path, cwd, argc, argv, NULL, fg, NULL, NULL, NULL, false);
}
int proc_spawn_service(const char *path, int argc, char **argv, struct file *log, struct file *input, bool root) {
    return spawn(path, FS_ROOT, argc, argv, NULL, true, log, input, NULL, root);
}

int proc_spawn_child(const char *path, int argc, char **argv, char **env, struct file *input) {
    struct task *parent = task_process(task_current());
    return spawn(path, task_process(parent)->cwd, argc, argv, env, true, NULL, input, parent, false);
}

/* ---------------------------------------------------------------- fork
 *
 * Копия текущего процесса. Регистры потомка — это копия кадра системного
 * вызова родителя (r), только eax = 0. Поэтому потомок «возвращается» из
 * того же самого fork(), но получает 0, а родитель — номер потомка. */
int proc_fork(struct regs *r) {
    struct task *parent = task_current();
    /* Как Linux: fork только у лидера; другие потоки в ребёнка не копируем.
     * Раньше task_has_threads() давал EBUSY — git+libpthread не мог index-pack. */
    if (parent->leader) return PROC_EBUSY;
    parent = task_process(parent);
    u32 pages;
    u32 dir = vmm_clone_user(parent->page_dir, &pages);
    if (!dir) return PROC_ENOMEM;

    struct task *t = task_alloc_user(parent->name, dir, 0, 0);
    if (!t) { vmm_destroy_dir(dir); return PROC_ENOMEM; }
    struct regs *cr = (struct regs *)t->esp;
    *cr = *r;                                            /* те же регистры... */
    cr->eax = 0;                                         /* ...но fork() вернёт 0 */

    task_process(t)->brk_start  = task_process(parent)->brk_start;
    task_process(t)->brk        = task_process(parent)->brk;
    task_process(t)->user_pages = pages;
    task_process(t)->uid = task_process(parent)->uid;
    task_process(t)->real_uid = task_process(parent)->real_uid;
    task_process(t)->saved_uid = task_process(parent)->saved_uid;
    task_process(t)->cloexec = task_process(parent)->cloexec;
    task_process(t)->fs_root = task_process(parent)->fs_root;
    task_process(t)->gid = task_process(parent)->gid;
    task_process(t)->real_gid = task_process(parent)->real_gid;
    task_process(t)->saved_gid = task_process(parent)->saved_gid;
    task_process(t)->creation_mask = task_process(parent)->user?task_process(parent)->creation_mask:022;
    task_process(t)->authenticated = task_process(parent)->authenticated;
    task_process(t)->cwd        = task_process(parent)->cwd;
    if (parent->env_len) {
        char *copy = kmalloc(parent->env_len);
        if (copy) { memcpy(copy, parent->env_buf, parent->env_len); env_replace(t, copy, parent->env_len); }
    }
    t->parent     = parent->id;
    t->pgid       = parent->pgid;
    t->tcb        = parent->tcb;
    t->keep_zombie = true;                               /* родитель заберёт код в waitpid */
    fpu_copy_current(t);                                 /* и регистры FPU тоже копия */
    for (int i = 0; i < MAX_FDS; i++) task_process(t)->fds[i] = file_dup(task_process(parent)->fds[i]);

    u32 id = task_start(t);
    return id ? (int)id : PROC_EAGAIN;
}

/* ---------------------------------------------------------------- exec
 *
 * Заменить программу в ТЕКУЩЕМ процессе. Номер, родитель, открытые файлы и
 * текущий каталог остаются, а память — новая. Если новую программу загрузить
 * не удалось, процесс продолжает жить со старой и получает ошибку. */
int proc_exec(struct regs *r, const char *path, int argc, char **argv, char **env) {
    struct task *t = task_current();
    if (t->leader || task_has_threads(t)) return PROC_EBUSY;
    char **use = env;
    char **owned = NULL;
    int envc = 0;
    if (!use && t->env_buf) {
        if (unpack_env(t->env_buf, t->env_len, &owned, &envc)) return PROC_ENOMEM;
        use = owned;
    }
    if (!use) use = (char **)default_env;
    struct image im;
    memset(&im, 0, sizeof(im));
    int err = load_image(path, task_process(t)->cwd, argc, argv, use, &im);
    char *packed = NULL;
    u32 plen = 0;
    if (!err) err = pack_env((const char **)use, &packed, &plen);
    kfree(owned);
    if (err) {
        if (im.dir) vmm_destroy_dir(im.dir);
        kfree(packed);
        return err;
    }
    env_replace(t, packed, plen);

    for(int fd=0;fd<MAX_FDS;fd++) if(t->cloexec&(1u<<fd)) {file_close(t->fds[fd]);t->fds[fd]=NULL;}
    t->cloexec=0;
    u32 old = t->page_dir;
    t->page_dir = im.dir;
    vmm_switch(im.dir);                                  /* уходим со старого каталога... */
    shm_release_all(t, old);                             /* release mappings before destroying the old directory */
    ipc_identity_changed(t);                             /* invalidate capabilities held by the old image */
    vmm_destroy_dir(old);                                /* ...и только теперь его освобождаем */
    task_process(t)->brk_start = task_process(t)->brk = im.brk;
    task_process(t)->user_pages = im.pages;
    t->tcb = 0;                                          /* старый блок errno указывал в прежнюю память */
    fpu_reset_current();                                 /* новой программе — чистый FPU */
    const char *b = basename(path);
    u32 n = strlen(b);
    if (n >= TASK_NAME) n = TASK_NAME - 1;
    memset(t->name, 0, TASK_NAME);
    memcpy(t->name, b, n);

    /* iret из системного вызова уйдёт в начало новой программы */
    u32 cs = r->cs, ss = r->ss, ds = r->ds;
    memset(r, 0, sizeof(*r));
    r->cs = cs; r->ss = ss;
    r->ds = r->es = r->fs = r->gs = ds;
    r->eip = im.entry;
    r->useresp = im.sp;
    r->eflags = 0x202;
    return 0;
}

/* ---------------------------------------------------------------- передний план */

void proc_set_foreground(u32 pgid) { foreground = pgid; }
u32  proc_foreground(void) { return foreground; }

/* Ctrl+C: убить всю группу переднего плана (например, весь конвейер) */
bool proc_ctrl_c(void) {
    if (!foreground || !console_isig()) return false;   /* программа просила отдать ^C ей */
    if (!task_kill_group(foreground)) return false;
    vga_puts("^C\n");
    return true;
}
