/* Объектные файлы ELF (ET_REL) и компоновщик.
 *
 * Объектный файл в памяти (ObjFile, cc.h) один и тот же, откуда бы он ни взялся:
 * из генератора (gen.c), из своего .o или из .o, собранного gcc (-m32 -fno-pic).
 * obj_write пишет его как ELF ET_REL — такие файлы понимают и binutils (ld, objdump).
 * archive_read читает библиотеку .a (формат ar). link_program собирает программу ET_EXEC:
 *   0. из библиотек — только члены, которые определяют ещё не найденные символы;
 *   1. общие символы: одно определение на имя (библиотечное уступает слабому, слабое —
 *      обычному, COMMON — любому);
 *   2. отсев: остаются только секции, до которых можно дойти от точки входа;
 *   3. раскладка: код и константы с TEXT_BASE, данные и bss — с новой страницы;
 *   4. перемещения (A лежит в самих байтах):
 *      R_386_32 = S + A,  R_386_PC32 и R_386_PLT32 = S + A - P,
 *      R_386_GOTPC = GOT + A - P,  R_386_GOTOFF = S + A - GOT,
 *      R_386_GOT32 = смещение ячейки GOT. Для обычной программы PLT не строится:
 *      PLT32 считается как PC32, а ячейки GOT заполняются абсолютными адресами.
 * -shared и программа, которой нужна .so, идут другим путём (ET_DYN или ET_EXEC
 * с PT_INTERP). Пока .so нет и -shared не задан, байты программы те же. */
#include "cc.h"

#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_NOBITS        8
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_DYNSYM        11
#define SHT_REL           9
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP         17
#define SHF_WRITE         1
#define SHF_ALLOC         2
#define SHF_EXECINSTR     4
#define SHF_INFO_LINK     0x40
#define SHN_ABS           0xFFF1
#define SHN_COMMON        0xFFF2
#define GRP_COMDAT        1

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}
static void wr32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void buf_u16(Buf *b, unsigned v) { buf_byte(b, v); buf_byte(b, v >> 8); }
static unsigned align_up(unsigned n, unsigned a) { return (n + a - 1) / a * a; }

void write_file(const char *path, Buf *b) {
    unsigned mode=b->len>=18 && !memcmp(b->data,"\177ELF",4) && rd16(b->data+16)!=1?0755:0644;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,mode);
    if (fd < 0) error("cannot write %s", path);
    int off = 0;
    while (off < b->len) {
        int n = write(fd, b->data + off, b->len - off);
        if (n <= 0) error("write error: %s", path);
        off += n;
    }
    close(fd);
}

/* ------------------------------------------------------------ ObjFile */

ObjFile *obj_new(const char *name) {
    ObjFile *f = xcalloc(sizeof(ObjFile));
    f->name = strndup2(name, strlen(name));
    return f;
}

int obj_add_section(ObjFile *f, char *name, int kind, int align, unsigned char *data, int size) {
    if (f->n_secs == f->cap_secs) {
        f->cap_secs = f->cap_secs ? f->cap_secs * 2 : 64;
        Section **p = xcalloc(sizeof(Section *) * f->cap_secs);
        if (f->secs) memcpy(p, f->secs, sizeof(Section *) * f->n_secs);
        free(f->secs);
        f->secs = p;
    }
    Section *s = xcalloc(sizeof(Section));
    s->name = name;
    s->kind = kind;
    s->align = align < 1 ? 1 : align;
    s->size = size;
    s->file = f;
    if (kind != SEC_BSS) {
        s->data = xcalloc(size);
        if (data && size) memcpy(s->data, data, size);
    }
    f->secs[f->n_secs] = s;
    return f->n_secs++;
}

int obj_add_sym(ObjFile *f, char *name, int bind, int type, int sec, unsigned value, unsigned size) {
    if (f->n_syms == f->cap_syms) {
        f->cap_syms = f->cap_syms ? f->cap_syms * 2 : 64;
        LinkSym *p = xcalloc(sizeof(LinkSym) * f->cap_syms);
        if (f->syms) memcpy(p, f->syms, sizeof(LinkSym) * f->n_syms);
        free(f->syms);
        f->syms = p;
    }
    LinkSym *s = &f->syms[f->n_syms];
    s->name = name;
    s->bind = bind;
    s->type = type;
    s->sec = sec;
    s->value = value;
    s->size = size;
    return f->n_syms++;
}

void obj_add_reloc(Section *s, int offset, int type, int sym) {
    if (s->n_rels == s->cap_rels) {
        s->cap_rels = s->cap_rels ? s->cap_rels * 2 : 16;
        LinkReloc *p = xcalloc(sizeof(LinkReloc) * s->cap_rels);
        if (s->rels) memcpy(p, s->rels, sizeof(LinkReloc) * s->n_rels);
        free(s->rels);
        s->rels = p;
    }
    s->rels[s->n_rels].offset = offset;
    s->rels[s->n_rels].type = type;
    s->rels[s->n_rels].sym = sym;
    s->n_rels++;
}

/* ------------------------------------------------------------ запись .o */

typedef struct Shdr { unsigned name, type, flags, addr, offset, size, link, info, align, entsize; } Shdr;

static int str_add(Buf *b, const char *s) {
    int off = b->len;
    buf_put(b, s, strlen(s) + 1);
    return off;
}

/* body начинается сразу за заголовком ELF (52 байта): выровнять смещение в файле */
static unsigned body_align(Buf *body, int align) {
    while ((52 + body->len) % align) buf_byte(body, 0);
    return 52 + body->len;
}

void obj_write(ObjFile *f, const char *path) {
    int n_rel = 0;
    for (int i = 0; i < f->n_secs; i++) if (f->secs[i]->n_rels) n_rel++;
    /* [0] пустая, [1..n] секции, потом .rel.*, .note.GNU-stack, .symtab, .strtab, .shstrtab */
    int note_idx = f->n_secs + n_rel + 1;
    int symtab_idx = note_idx + 1, strtab_idx = note_idx + 2, shstr_idx = note_idx + 3;
    int shnum = shstr_idx + 1;
    Shdr *sh = xcalloc(sizeof(Shdr) * shnum);
    Buf body = {0}, shstr = {0}, str = {0}, syms = {0};
    buf_byte(&shstr, 0);
    buf_byte(&str, 0);

    /* символы: сначала локальные (так требует ELF), потом остальные */
    int *map = xcalloc(sizeof(int) * (f->n_syms + 1));
    for (int i = 0; i < 16; i++) buf_byte(&syms, 0);
    int k = 1, first_global = 1;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) first_global = k;
        for (int i = 0; i < f->n_syms; i++) {
            LinkSym *s = &f->syms[i];
            if ((s->bind == BIND_LOCAL) != (pass == 0)) continue;
            map[i] = k++;
            buf_u32(&syms, s->name && *s->name ? str_add(&str, s->name) : 0);
            buf_u32(&syms, s->value);
            buf_u32(&syms, s->size);
            buf_byte(&syms, (s->bind << 4) | s->type);
            buf_byte(&syms, 0);
            if (s->sec >= 0) buf_u16(&syms, s->sec + 1);
            else if (s->sec == SYM_COMMON) buf_u16(&syms, SHN_COMMON);
            else if (s->sec == SYM_ABS) buf_u16(&syms, SHN_ABS);
            else buf_u16(&syms, 0);
        }
    }

    for (int i = 0; i < f->n_secs; i++) {
        Section *s = f->secs[i];
        Shdr *h = &sh[i + 1];
        h->name = str_add(&shstr, s->name);
        h->type = s->kind == SEC_BSS ? SHT_NOBITS : SHT_PROGBITS;
        h->flags = SHF_ALLOC | (s->kind == SEC_TEXT ? SHF_EXECINSTR : 0) |
                   (s->kind == SEC_DATA || s->kind == SEC_BSS ? SHF_WRITE : 0);
        h->offset = body_align(&body, s->align);
        h->size = s->size;
        h->align = s->align;
        if (s->kind != SEC_BSS) buf_put(&body, s->data, s->size);
    }
    int r = f->n_secs + 1;
    for (int i = 0; i < f->n_secs; i++) {
        Section *s = f->secs[i];
        if (!s->n_rels) continue;
        Shdr *h = &sh[r++];
        h->name = str_add(&shstr, format(".rel%s", s->name));
        h->type = SHT_REL;
        h->flags = SHF_INFO_LINK;
        h->offset = body_align(&body, 4);
        h->size = s->n_rels * 8;
        h->link = symtab_idx;
        h->info = i + 1;
        h->align = 4;
        h->entsize = 8;
        for (int j = 0; j < s->n_rels; j++) {
            buf_u32(&body, s->rels[j].offset);
            buf_u32(&body, (map[s->rels[j].sym] << 8) | s->rels[j].type);
        }
    }
    /* без этой пустой секции ld считает, что стеку нужно право на выполнение */
    sh[note_idx].name = str_add(&shstr, ".note.GNU-stack");
    sh[note_idx].type = SHT_PROGBITS;
    sh[note_idx].offset = body_align(&body, 1);
    sh[note_idx].align = 1;

    sh[symtab_idx].name = str_add(&shstr, ".symtab");
    sh[symtab_idx].type = SHT_SYMTAB;
    sh[symtab_idx].offset = body_align(&body, 4);
    sh[symtab_idx].size = syms.len;
    sh[symtab_idx].link = strtab_idx;
    sh[symtab_idx].info = first_global;
    sh[symtab_idx].align = 4;
    sh[symtab_idx].entsize = 16;
    buf_put(&body, syms.data, syms.len);

    sh[strtab_idx].name = str_add(&shstr, ".strtab");
    sh[strtab_idx].type = SHT_STRTAB;
    sh[strtab_idx].offset = body_align(&body, 1);
    sh[strtab_idx].size = str.len;
    sh[strtab_idx].align = 1;
    buf_put(&body, str.data, str.len);

    sh[shstr_idx].name = str_add(&shstr, ".shstrtab");
    sh[shstr_idx].type = SHT_STRTAB;
    sh[shstr_idx].offset = body_align(&body, 1);
    sh[shstr_idx].size = shstr.len;
    sh[shstr_idx].align = 1;
    buf_put(&body, shstr.data, shstr.len);

    unsigned shoff = body_align(&body, 4);
    Buf out = {0};
    buf_put(&out, "\x7f" "ELF\x01\x01\x01\0\0\0\0\0\0\0\0\0", 16);
    buf_u16(&out, 1);                                           /* e_type = REL */
    buf_u16(&out, 3);                                           /* e_machine = 386 */
    buf_u32(&out, 1);                                           /* e_version */
    buf_u32(&out, 0);                                           /* e_entry */
    buf_u32(&out, 0);                                           /* e_phoff */
    buf_u32(&out, shoff);
    buf_u32(&out, 0);                                           /* e_flags */
    buf_u16(&out, 52);                                          /* e_ehsize */
    buf_u16(&out, 0); buf_u16(&out, 0);                         /* e_phentsize, e_phnum */
    buf_u16(&out, 40);                                          /* e_shentsize */
    buf_u16(&out, shnum);
    buf_u16(&out, shstr_idx);
    buf_put(&out, body.data, body.len);
    for (int i = 0; i < shnum; i++) {
        Shdr *h = &sh[i];
        buf_u32(&out, h->name); buf_u32(&out, h->type); buf_u32(&out, h->flags); buf_u32(&out, h->addr);
        buf_u32(&out, h->offset); buf_u32(&out, h->size); buf_u32(&out, h->link); buf_u32(&out, h->info);
        buf_u32(&out, h->align); buf_u32(&out, h->entsize);
    }
    write_file(path, &out);
}

/* ------------------------------------------------------------ чтение .o */

/* поля Elf32_Shdr по порядку (по 4 байта) */
enum { F_NAME, F_TYPE, F_FLAGS, F_ADDR, F_OFFSET, F_SIZE, F_LINK, F_INFO, F_ALIGN, F_ENTSIZE };

/* группы COMDAT (gcc кладёт в них, например, __x86.get_pc_thunk.*): из одинаковых берём первую */
static char **comdat_seen;
static int n_comdat, cap_comdat;

static bool comdat_add(char *sig) {
    for (int i = 0; i < n_comdat; i++) if (!strcmp(comdat_seen[i], sig)) return false;
    if (n_comdat == cap_comdat) {
        cap_comdat = cap_comdat ? cap_comdat * 2 : 16;
        char **p = xcalloc(sizeof(char *) * cap_comdat);
        if (comdat_seen) memcpy(p, comdat_seen, sizeof(char *) * n_comdat);
        comdat_seen = p;
    }
    comdat_seen[n_comdat++] = strndup2(sig, strlen(sig));     /* буфер файла освободят */
    return true;
}

static ObjFile *obj_parse(const char *path, unsigned char *d, int len) {
    if (len < 52 || memcmp(d, "\x7f" "ELF", 4) || d[4] != 1 || d[5] != 1)
        error("%s: not an ELF32 little-endian file", path);
    if (rd16(d + 16) != 1) error("%s: not a relocatable object file (ET_REL)", path);
    if (rd16(d + 18) != 3) error("%s: not an i386 object file", path);
    unsigned shoff = rd32(d + 32);
    int shnum = rd16(d + 48), shstrndx = rd16(d + 50);
    if (rd16(d + 46) != 40 || shnum == 0 || shoff + (unsigned)shnum * 40 > (unsigned)len || shstrndx >= shnum)
        error("%s: broken section header table", path);
    #define SH(i, field) rd32(d + shoff + (i) * 40 + (field) * 4)
    for (int i = 1; i < shnum; i++)
        if (SH(i, F_TYPE) != SHT_NOBITS && SH(i, F_OFFSET) + SH(i, F_SIZE) > (unsigned)len)
            error("%s: section %d is out of the file", path, i);
    char *shstr = (char *)d + SH(shstrndx, F_OFFSET);

    int symtab = 0;
    for (int i = 1; i < shnum; i++) if (SH(i, F_TYPE) == SHT_SYMTAB) symtab = i;
    unsigned char *symd = symtab ? d + SH(symtab, F_OFFSET) : NULL;
    int n_syms = symtab ? SH(symtab, F_SIZE) / 16 : 0;
    char *strtab = symtab ? (char *)d + SH(SH(symtab, F_LINK), F_OFFSET) : NULL;

    int *secmap = xcalloc(sizeof(int) * shnum);
    bool *dropped = xcalloc(sizeof(bool) * shnum);
    for (int i = 0; i < shnum; i++) secmap[i] = -1;

    for (int i = 1; i < shnum; i++) {
        if (SH(i, F_TYPE) != SHT_GROUP || !symd) continue;
        unsigned char *g = d + SH(i, F_OFFSET);
        int n = SH(i, F_SIZE) / 4;
        if (n < 1 || !(rd32(g) & GRP_COMDAT)) continue;
        char *sig = strtab + rd32(symd + SH(i, F_INFO) * 16);
        if (comdat_add(sig)) continue;
        for (int j = 1; j < n; j++) {
            unsigned m = rd32(g + j * 4);
            if (m < (unsigned)shnum) dropped[m] = true;
        }
    }

    ObjFile *f = obj_new(path);
    for (int i = 1; i < shnum; i++) {
        unsigned type = SH(i, F_TYPE), flags = SH(i, F_FLAGS), size = SH(i, F_SIZE);
        char *name = shstr + SH(i, F_NAME);
        if (!(flags & SHF_ALLOC) || dropped[i]) continue;
        if (type == SHT_INIT_ARRAY || type == SHT_FINI_ARRAY || type == SHT_PREINIT_ARRAY ||
            !strcmp(name, ".ctors") || !strcmp(name, ".dtors")) {
            if (size) error("%s: %s: constructors and destructors are not supported", path, name);
            continue;
        }
        if (type != SHT_PROGBITS && type != SHT_NOBITS) continue;     /* .note.gnu.property и т.п. */
        int kind = (flags & SHF_EXECINSTR) ? SEC_TEXT : type == SHT_NOBITS ? SEC_BSS :
                   (flags & SHF_WRITE) ? SEC_DATA : SEC_RODATA;
        secmap[i] = obj_add_section(f, strndup2(name, strlen(name)), kind, SH(i, F_ALIGN),
                                    type == SHT_NOBITS ? NULL : d + SH(i, F_OFFSET), size);
    }

    /* символы — с теми же номерами, что в файле: на них ссылаются перемещения */
    for (int j = 0; j < n_syms; j++) {
        unsigned char *s = symd + j * 16;
        unsigned info = s[12], shndx = rd16(s + 14);
        int bind = (info >> 4) == 0 ? BIND_LOCAL : (info >> 4) == 2 ? BIND_WEAK : BIND_GLOBAL;
        int type = (info & 15) <= 3 ? (int)(info & 15) : STY_NOTYPE;
        char *name = strtab + rd32(s);
        int sec;
        if (shndx == 0) sec = SYM_UNDEF;
        else if (shndx == SHN_ABS) sec = SYM_ABS;
        else if (shndx == SHN_COMMON) sec = SYM_COMMON;
        else if (shndx < (unsigned)shnum) sec = secmap[shndx] >= 0 ? secmap[shndx] : SYM_UNDEF;
        else error("%s: symbol %s: unsupported section index %u", path, name, shndx);
        if (type == STY_SECTION && shndx < (unsigned)shnum) name = shstr + SH(shndx, F_NAME);
        obj_add_sym(f, strndup2(name, strlen(name)), j == 0 ? BIND_LOCAL : bind, type, sec, rd32(s + 4), rd32(s + 8));
    }

    for (int i = 1; i < shnum; i++) {
        unsigned type = SH(i, F_TYPE), target = SH(i, F_INFO);
        if (type != SHT_REL && type != SHT_RELA) continue;
        if (target >= (unsigned)shnum || secmap[target] < 0) continue;
        if (type == SHT_RELA) error("%s: RELA relocations are not supported on i386", path);
        Section *sec = f->secs[secmap[target]];
        unsigned char *r = d + SH(i, F_OFFSET);
        int n = SH(i, F_SIZE) / 8;
        for (int j = 0; j < n; j++) {
            unsigned off = rd32(r + j * 8), rinfo = rd32(r + j * 8 + 4);
            int sym = rinfo >> 8;
            if (sym >= n_syms) error("%s: relocation refers to symbol %d out of range", path, sym);
            if (sec->kind == SEC_BSS || off + 4 > (unsigned)sec->size)
                error("%s: relocation outside of section %s", path, sec->name);
            obj_add_reloc(sec, off, rinfo & 255, sym);
        }
    }
    #undef SH
    free(secmap);
    free(dropped);
    return f;
}

ObjFile *obj_read(const char *path) {
    int len;
    unsigned char *d = (unsigned char *)read_file(path, &len);
    if (!d) return NULL;
    ObjFile *f = obj_parse(path, d, len);
    free(d);
    return f;
}

/* ------------------------------------------------------------ архивы .a */

typedef struct ArMember {
    char *name;
    int   off, size;            /* данные члена в Archive.data */
    bool  loaded;
} ArMember;

typedef struct IndexSym {
    char *name;
    int   member;
} IndexSym;

struct Archive {
    char *path;
    unsigned char *data;
    int   len;
    ArMember *members;
    int   n_members;
    IndexSym *index;
    int   n_index;
};

static int ar_num(const unsigned char *p, int n) {
    int v = 0;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10 + (p[i] - '0');
    return v;
}

static unsigned rd32be(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* индекс "/" — номера заголовков членов; если его нет, символы берём из самих членов */
Archive *archive_read(const char *path) {
    int len;
    unsigned char *d = (unsigned char *)read_file(path, &len);
    if (!d) return NULL;
    if (len < 8 || memcmp(d, "!<arch>\n", 8)) error("%s: not an archive", path);
    Archive *a = xcalloc(sizeof(Archive));
    a->path = strndup2(path, strlen(path));
    a->data = d;
    a->len = len;

    int cap = 0, pos = 8;
    const unsigned char *idx = NULL, *longnames = NULL;
    int idx_size = 0, longnames_len = 0;
    int *hdr_off = NULL;
    while (pos + 60 <= len) {
        unsigned char *h = d + pos;
        if (h[58] != '`' || h[59] != '\n') error("%s: broken archive member header", path);
        int size = ar_num(h + 48, 10), here = pos;
        unsigned char *data = h + 60;
        if (pos + 60 + size > len) error("%s: truncated archive", path);
        pos += 60 + size + (size & 1);
        if (h[0] == '/' && h[1] == ' ') { idx = data; idx_size = size; continue; }
        if (!memcmp(h, "/SYM64/", 7)) continue;
        if (h[0] == '/' && h[1] == '/') { longnames = data; longnames_len = size; continue; }
        char *name;
        if (h[0] == '/' && h[1] >= '0' && h[1] <= '9') {
            int off = ar_num(h + 1, 15), e = off;
            if (!longnames || off >= longnames_len) error("%s: bad long member name", path);
            while (e < longnames_len && longnames[e] != '/' && longnames[e] != '\n') e++;
            name = strndup2((char *)longnames + off, e - off);
        } else if (!memcmp(h, "#1/", 3)) {
            int n = ar_num(h + 3, 13);
            if (n > size) error("%s: bad member name", path);
            name = strndup2((char *)data, n);
            data += n;
            size -= n;
        } else {
            int n = 0;
            while (n < 16 && h[n] != '/' && h[n] != ' ') n++;
            name = strndup2((char *)h, n);
        }
        if (a->n_members == cap) {
            cap = cap ? cap * 2 : 16;
            ArMember *m = xcalloc(sizeof(ArMember) * cap);
            int *o = xcalloc(sizeof(int) * cap);
            if (a->members) { memcpy(m, a->members, sizeof(ArMember) * a->n_members); memcpy(o, hdr_off, sizeof(int) * a->n_members); }
            a->members = m;
            hdr_off = o;
        }
        a->members[a->n_members].name = name;
        a->members[a->n_members].off = data - d;
        a->members[a->n_members].size = size;
        hdr_off[a->n_members] = here;
        a->n_members++;
    }

    if (idx && idx_size >= 4) {
        int n = rd32be(idx);
        if (4 + 4 * n > idx_size) error("%s: broken symbol index", path);
        a->index = xcalloc(sizeof(IndexSym) * (n + 1));
        const char *s = (const char *)idx + 4 + 4 * n, *end = (const char *)idx + idx_size;
        for (int i = 0; i < n && s < end; i++) {
            int off = rd32be(idx + 4 + 4 * i), m = 0;
            while (m < a->n_members && hdr_off[m] != off) m++;
            int sl = strlen(s);
            if (m < a->n_members) {
                a->index[a->n_index].name = (char *)s;
                a->index[a->n_index].member = m;
                a->n_index++;
            }
            s += sl + 1;
        }
    } else {
        int icap = 0;
        for (int m = 0; m < a->n_members; m++) {
            unsigned char *md = d + a->members[m].off;
            if (a->members[m].size < 4 || memcmp(md, "\x7f" "ELF", 4)) continue;
            ObjFile *f = obj_parse(format("%s(%s)", path, a->members[m].name), md, a->members[m].size);
            for (int j = 0; j < f->n_syms; j++) {
                LinkSym *s = &f->syms[j];
                if (s->bind == BIND_LOCAL || s->sec == SYM_UNDEF) continue;
                if (a->n_index == icap) {
                    icap = icap ? icap * 2 : 64;
                    IndexSym *p = xcalloc(sizeof(IndexSym) * icap);
                    if (a->index) memcpy(p, a->index, sizeof(IndexSym) * a->n_index);
                    a->index = p;
                }
                a->index[a->n_index].name = s->name;
                a->index[a->n_index].member = m;
                a->n_index++;
            }
        }
    }
    free(hdr_off);
    return a;
}

/* ------------------------------------------------------------ компоновщик */

/* насколько определение «сильное»: побеждает большее, два обычных — ошибка */
enum { DEF_NONE, DEF_COMMON, DEF_LIBRARY, DEF_WEAK, DEF_STRONG };

struct GSym {
    GSym *next;
    char *name;
    int   state;                /* DEF_* */
    ObjFile *file;              /* где определение */
    int   idx;
    unsigned common_size, common_align;
    Section *common;            /* выделенное место для COMMON */
    int saw_call, saw_addr;     /* живая ссылка: вызов или взятие адреса */
    int dyn;                    /* DYN_*: участие в динамической таблице */
    int plt_index;              /* 0 — нет PLT, иначе номер заглушки с единицы */
    unsigned dyn_size;          /* размер объекта из .so, для COPY */
};

#define GBUCKETS 4096
static GSym *gtab[GBUCKETS];

static GSym *gsym_get(char *name, bool create) {
    unsigned h = 2166136261u;
    for (char *p = name; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h %= GBUCKETS;
    for (GSym *g = gtab[h]; g; g = g->next)
        if (!strcmp(g->name, name)) return g;
    if (!create) return NULL;
    GSym *g = xcalloc(sizeof(GSym));
    g->name = name;
    g->next = gtab[h];
    gtab[h] = g;
    return g;
}

static const char *file_name(ObjFile *f) { return *f->name ? f->name : "C sources"; }

/* файл из библиотеки: его определения уступают всем остальным */
static void add_globals(ObjFile *f) {
    bool from_lib = f->from_lib;
    for (int i = 0; i < f->n_syms; i++) {
        LinkSym *s = &f->syms[i];
        if (s->bind == BIND_LOCAL) continue;
        GSym *g = gsym_get(s->name, true);
        s->global = g;
        if (s->sec == SYM_UNDEF) continue;
        if (s->sec == SYM_COMMON) {
            if (g->state == DEF_NONE) { g->state = DEF_COMMON; g->file = f; g->idx = i; }
            if (g->state == DEF_COMMON) {
                if (s->size > g->common_size) g->common_size = s->size;
                if (s->value > g->common_align) g->common_align = s->value;
            }
            continue;
        }
        int st = from_lib ? DEF_LIBRARY : s->bind == BIND_WEAK ? DEF_WEAK : DEF_STRONG;
        if (g->state == DEF_STRONG && st == DEF_STRONG)
            error("multiple definition of '%s' (%s and %s)", s->name, file_name(g->file), file_name(f));
        if (g->state >= st) continue;
        g->state = st;
        g->file = f;
        g->idx = i;
    }
}

/* куда указывает символ idx файла f: секция (или NULL — абсолютный) и смещение. false — некуда */
static bool sym_target(ObjFile *f, int idx, Section **sec, unsigned *value) {
    LinkSym *s = &f->syms[idx];
    *sec = NULL;
    *value = 0;
    if (s->bind != BIND_LOCAL) {
        GSym *g = s->global;
        if (g->state == DEF_NONE) return s->bind == BIND_WEAK;  /* слабая ссылка без определения = 0 */
        if (g->state == DEF_COMMON) { *sec = g->common; return true; }
        f = g->file;
        s = &f->syms[g->idx];
    }
    if (s->sec >= 0) { *sec = f->secs[s->sec]; *value = s->value; return true; }
    if (s->sec == SYM_ABS) { *value = s->value; return true; }
    return false;
}

/* где ссылка: имя функции для своих секций (.text.имя), иначе имя секции; и файл, если это .o */
static const char *where(Section *s) {
    const char *n = strncmp(s->name, ".text.", 6) ? s->name : s->name + 6;
    if (!*s->file->name) return n;
    return format("%s (%s)", n, s->file->name);
}

static Section **work;
static int n_work, cap_work;

static void mark(Section *s) {
    if (!s || s->live) return;
    s->live = true;
    if (n_work == cap_work) {
        cap_work = cap_work ? cap_work * 2 : 256;
        Section **p = xcalloc(sizeof(Section *) * cap_work);
        if (work) memcpy(p, work, sizeof(Section *) * n_work);
        free(work);
        work = p;
    }
    work[n_work++] = s;
}

static bool is_pic_reloc(int type) {
    return type == R_386_GOT32 || type == R_386_GOTOFF || type == R_386_GOTPC;
}

static bool reloc_ok(int type) {
    return type == R_386_32 || type == R_386_PC32 || type == R_386_PLT32 || is_pic_reloc(type);
}

static void mark_from_relocs(bool allow_undef) {
    while (n_work) {
        Section *s = work[--n_work];
        for (int i = 0; i < s->n_rels; i++) {
            LinkReloc *r = &s->rels[i];
            if (r->type == R_386_NONE) continue;
            if (!reloc_ok(r->type))
                error("unsupported relocation type %d in %s", r->type, where(s));
            Section *t;
            unsigned v;
            if (!sym_target(s->file, r->sym, &t, &v)) {
                if (allow_undef) continue;
                error("undefined reference to '%s' in %s", s->file->syms[r->sym].name, where(s));
            }
            mark(t);
        }
    }
}

/* GOT для -fPIC. Первые три ячейки зарезервированы, как в System V i386:
 * их заполнит динамический загрузчик, а в обычной программе они нули. */
static Section *got_sec;
typedef struct { GSym *g; int off; } GotSlot;
static GotSlot *got_slots;
static int n_got_slots, cap_got_slots;

static bool any_pic(ObjFile **files, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < files[i]->n_secs; j++) {
            Section *s = files[i]->secs[j];
            for (int r = 0; r < s->n_rels; r++)
                if (is_pic_reloc(s->rels[r].type)) return true;
        }
    return false;
}

static void ensure_got(ObjFile *common) {
    int sec = obj_add_section(common, ".got", SEC_DATA, 4, NULL, 12);
    got_sec = common->secs[sec];
    int idx = obj_add_sym(common, "_GLOBAL_OFFSET_TABLE_", BIND_GLOBAL, STY_OBJECT, sec, 0, 12);
    GSym *g = gsym_get("_GLOBAL_OFFSET_TABLE_", true);
    if (g->state == DEF_STRONG)
        error("multiple definition of '_GLOBAL_OFFSET_TABLE_'");
    g->state = DEF_STRONG;
    g->file = common;
    g->idx = idx;
    common->syms[idx].global = g;
}

static void got_add(GSym *g) {
    for (int i = 0; i < n_got_slots; i++)
        if (got_slots[i].g == g) return;
    if (n_got_slots == cap_got_slots) {
        cap_got_slots = cap_got_slots ? cap_got_slots * 2 : 32;
        GotSlot *p = xcalloc(sizeof(GotSlot) * cap_got_slots);
        if (got_slots) memcpy(p, got_slots, sizeof(GotSlot) * n_got_slots);
        free(got_slots);
        got_slots = p;
    }
    got_slots[n_got_slots].g = g;
    got_slots[n_got_slots].off = 12 + n_got_slots * 4;
    n_got_slots++;
}

static int got_slot_off(ObjFile *f, int sym) {
    LinkSym *s = &f->syms[sym];
    if (s->bind == BIND_LOCAL || !s->global)
        error("R_386_GOT32 against local symbol '%s'", s->name);
    for (int i = 0; i < n_got_slots; i++)
        if (got_slots[i].g == s->global) return got_slots[i].off;
    error("internal: '%s' has no GOT entry", s->name);
}

/* Ячейка GOT — абсолютный адрес символа. Пишем его тем же R_386_32, что и указатели в данных. */
static void fill_got(ObjFile **files, int n, ObjFile *common) {
    if (!got_sec) return;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < files[i]->n_secs; j++) {
            Section *s = files[i]->secs[j];
            if (!s->live) continue;
            for (int r = 0; r < s->n_rels; r++) {
                if (s->rels[r].type != R_386_GOT32) continue;
                LinkSym *ls = &s->file->syms[s->rels[r].sym];
                if (ls->bind == BIND_LOCAL || !ls->global)
                    error("R_386_GOT32 against local symbol '%s' in %s", ls->name, where(s));
                got_add(ls->global);
                mark(got_sec);
            }
        }
    if (!got_sec->live) return;
    int size = 12 + n_got_slots * 4;
    free(got_sec->data);
    got_sec->data = xcalloc(size);
    got_sec->size = size;
    common->syms[gsym_get("_GLOBAL_OFFSET_TABLE_", false)->idx].size = size;
    for (int i = 0; i < n_got_slots; i++) {
        int idx = obj_add_sym(common, got_slots[i].g->name, BIND_GLOBAL, STY_NOTYPE, SYM_UNDEF, 0, 0);
        common->syms[idx].global = got_slots[i].g;
        obj_add_reloc(got_sec, got_slots[i].off, R_386_32, idx);
    }
}

static bool link_dyn(ObjFile **files, int n_files, ObjFile *common,
                     char **so_paths, int n_so, const char *out_path);

/* файловое смещение секции в готовой программе */
static unsigned file_off(Section *s, unsigned data_vaddr, unsigned data_off) {
    if (s->kind == SEC_TEXT || s->kind == SEC_RODATA) return s->addr - TEXT_BASE;
    return data_off + (s->addr - data_vaddr);
}

void link_program(ObjFile **in, int n_in, Archive **libs, int n_libs,
                  char **so_paths, int n_so, const char *out_path) {
    int n_files = n_in, cap_files = n_in + 16;
    ObjFile **files = xcalloc(sizeof(ObjFile *) * (cap_files + 1));
    for (int i = 0; i < n_in; i++) {
        files[i] = in[i];
        add_globals(files[i]);
    }
    gsym_get("__libc_start", true);                             /* на неё ссылается заглушка входа */

    /* Библиотеки: брать члены, пока они определяют что-то нужное и ещё не найденное.
     * Новый член может сослаться на символ из уже просмотренной библиотеки — поэтому по кругу. */
    for (bool more = true; more; ) {
        more = false;
        for (int l = 0; l < n_libs; l++) {
            Archive *a = libs[l];
            for (int e = 0; e < a->n_index; e++) {
                ArMember *m = &a->members[a->index[e].member];
                if (m->loaded) continue;
                GSym *g = gsym_get(a->index[e].name, false);
                if (!g || g->state != DEF_NONE) continue;
                m->loaded = true;
                if (n_files == cap_files) {
                    cap_files *= 2;
                    ObjFile **p = xcalloc(sizeof(ObjFile *) * (cap_files + 1));
                    memcpy(p, files, sizeof(ObjFile *) * n_files);
                    files = p;
                }
                ObjFile *f = obj_parse(format("%s(%s)", a->path, m->name), a->data + m->off, m->size);
                f->from_lib = true;
                files[n_files++] = f;
                add_globals(f);
                more = true;
            }
        }
    }

    /* COMMON: место в bss — в порядке первого появления, чтобы раскладка не зависела от хеша */
    ObjFile *common = obj_new("COMMON");
    for (int i = 0; i < n_files; i++)
        for (int j = 0; j < files[i]->n_syms; j++) {
            GSym *g = files[i]->syms[j].global;
            if (!g || g->state != DEF_COMMON || g->common || g->file != files[i] || g->idx != j) continue;
            int s = obj_add_section(common, g->name, SEC_BSS, g->common_align, NULL, g->common_size);
            g->common = common->secs[s];
        }
    got_sec = NULL;
    n_got_slots = 0;
    /* .so меняет результат только когда она и правда нужна. Иначе — тот же статический файл. */
    if ((opt_shared || n_so) && link_dyn(files, n_files, common, so_paths, n_so, out_path))
        return;
    if (any_pic(files, n_files)) ensure_got(common);

    /* точка входа: заглушка зовёт __libc_start(argc, argv) */
    GSym *start = gsym_get("__libc_start", false);
    Section *start_sec;
    unsigned start_val;
    if (start->state == DEF_NONE) error("undefined reference to '__libc_start'");
    sym_target(start->file, start->idx, &start_sec, &start_val);
    mark(start_sec);
    mark_from_relocs(false);
    fill_got(files, n_files, common);

    /* место под заголовки ELF: 52 + 2*32 = 116 байт, код с 0x80 */
    /* _start(argc, argv, envp): push envp, argv, argc */
    static const unsigned char stub_myos[] = { 0xFF, 0x74, 0x24, 0x0C, 0xFF, 0x74, 0x24, 0x0C, 0xFF, 0x74, 0x24, 0x0C };
    /* стек Linux: argc, argv, envp. lea edx, [argv+argc*4+4] */
    static const unsigned char stub_linux[] = { 0x8B, 0x04, 0x24, 0x8D, 0x4C, 0x24, 0x04, 0x8D, 0x54, 0x81, 0x04, 0x52, 0x51, 0x50 };
    const unsigned char *stub = opt_linux ? stub_linux : stub_myos;
    int stub_len = opt_linux ? sizeof(stub_linux) : sizeof(stub_myos);
    unsigned entry = 0x80;
    unsigned pos = entry + stub_len + 5 + 2;                     /* + call __libc_start; jmp $ */

    ObjFile **all = xcalloc(sizeof(ObjFile *) * (n_files + 1));
    for (int i = 0; i < n_files; i++) all[i] = files[i];
    all[n_files] = common;
    int n_all = n_files + 1;

    static const int order[] = { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS };
    unsigned data_vaddr = 0, data_len = 0, text_len = 0;
    for (int k = 0; k < 4; k++) {
        if (order[k] == SEC_DATA) {
            text_len = pos;
            data_vaddr = align_up(TEXT_BASE + pos, 4096);
            pos = 0;
        }
        if (order[k] == SEC_BSS) {
            if (!pos) pos = 4;                                  /* сегмент данных не бывает пустым */
            data_len = pos;
        }
        for (int i = 0; i < n_all; i++)
            for (int j = 0; j < all[i]->n_secs; j++) {
                Section *s = all[i]->secs[j];
                if (!s->live || s->kind != order[k]) continue;
                pos = align_up(pos, s->align);
                s->addr = (k < 2 ? TEXT_BASE : data_vaddr) + pos;
                pos += s->size;
            }
    }
    unsigned bss_end = pos;
    unsigned data_off = align_up(text_len, 4096);

    Buf out = {0};
    out.cap = data_off + data_len;
    out.len = out.cap;
    out.data = xcalloc(out.cap);
    unsigned char *o = out.data;
    memcpy(o + entry, stub, stub_len);
    o[entry + stub_len] = 0xE8;
    unsigned call_end = TEXT_BASE + entry + stub_len + 5;
    wr32(o + entry + stub_len + 1, (start_sec ? start_sec->addr : 0) + start_val - call_end);
    o[entry + stub_len + 5] = 0xEB;                             /* jmp $: __libc_start не возвращается */
    o[entry + stub_len + 6] = 0xFE;

    for (int i = 0; i < n_all; i++)
        for (int j = 0; j < all[i]->n_secs; j++) {
            Section *s = all[i]->secs[j];
            if (!s->live || s->kind == SEC_BSS) continue;
            unsigned char *p = o + file_off(s, data_vaddr, data_off);
            memcpy(p, s->data, s->size);
            unsigned got = got_sec && got_sec->live ? got_sec->addr : 0;
            for (int r = 0; r < s->n_rels; r++) {
                LinkReloc *rl = &s->rels[r];
                if (rl->type == R_386_NONE) continue;
                if (!reloc_ok(rl->type))
                    error("unsupported relocation type %d in %s", rl->type, where(s));
                Section *t;
                unsigned v;
                sym_target(s->file, rl->sym, &t, &v);
                unsigned S = (t ? t->addr : 0) + v;
                unsigned A = rd32(p + rl->offset);
                unsigned P = s->addr + rl->offset;
                unsigned val;
                switch (rl->type) {
                case R_386_32: val = S + A; break;
                case R_386_PC32: case R_386_PLT32: val = S + A - P; break;
                case R_386_GOTPC:
                case R_386_GOTOFF:
                    if (!got) error("relocation against the GOT in %s, but there is no GOT", where(s));
                    val = rl->type == R_386_GOTPC ? got + A - P : S + A - got;
                    break;
                default: val = (unsigned)got_slot_off(s->file, rl->sym) + A; break;   /* R_386_GOT32 */
                }
                wr32(p + rl->offset, val);
            }
        }

    /* заголовки ELF */
    Buf h = {0};
    buf_put(&h, "\x7f" "ELF\x01\x01\x01\0\0\0\0\0\0\0\0\0", 16);
    buf_u16(&h, 2);                                             /* e_type = EXEC */
    buf_u16(&h, 3);                                             /* e_machine = 386 */
    buf_u32(&h, 1);                                             /* e_version */
    buf_u32(&h, TEXT_BASE + entry);
    buf_u32(&h, 52);                                            /* e_phoff */
    buf_u32(&h, 0);                                             /* e_shoff */
    buf_u32(&h, 0);                                             /* e_flags */
    buf_u16(&h, 52);                                            /* e_ehsize */
    buf_u16(&h, 32);                                            /* e_phentsize */
    buf_u16(&h, 2);                                             /* e_phnum */
    buf_u32(&h, 0);                                             /* shentsize, shnum */
    buf_u16(&h, 0);                                             /* shstrndx */
    /* сегмент кода: R-X */
    buf_u32(&h, 1); buf_u32(&h, 0); buf_u32(&h, TEXT_BASE); buf_u32(&h, TEXT_BASE);
    buf_u32(&h, text_len); buf_u32(&h, text_len); buf_u32(&h, 5); buf_u32(&h, 0x1000);
    /* сегмент данных: RW- */
    buf_u32(&h, 1); buf_u32(&h, data_off); buf_u32(&h, data_vaddr); buf_u32(&h, data_vaddr);
    buf_u32(&h, data_len); buf_u32(&h, bss_end); buf_u32(&h, 6); buf_u32(&h, 0x1000);
    memcpy(o, h.data, h.len);
    write_file(out_path, &out);
}

/* ------------------------------------------------------------ динамическая компоновка
 *
 * -shared: ET_DYN с базой 0 (смещение в файле совпадает с виртуальным адресом).
 * Программа с .so: обычный ET_EXEC с TEXT_BASE, PT_INTERP = /lib/ld.so и PT_DYNAMIC.
 * PLT не ленивый: в библиотеке это jmp *смещение(%ebx) — ebx уже указывает на GOT, —
 * в программе jmp *абсолютный адрес ячейки. DT_BIND_NOW обязателен.
 * Объект из .so, на который ссылается программа, копируется в её bss (R_386_COPY).
 * Загрузчик при COPY ищет определение в библиотеках, а не эту копию. */

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_SONAME   14
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTREL   20
#define DT_JMPREL   23
#define DT_BIND_NOW 24
#define DT_FLAGS    30
#define DF_BIND_NOW 8

enum { DYN_NONE, DYN_PLT, DYN_COPY, DYN_SO };
enum { PREL_REL = 1, PREL_ABS, PREL_GLOB, PREL_JMP, PREL_COPY, PREL_GOT0 };

typedef struct {
    char *name;
    unsigned value, size;
    int info;
} SoSym;

typedef struct {
    unsigned char *bytes;
    char *path;
    char *soname;
    SoSym *syms;
    int n, cap;
    bool used;
    int need_off;
} SoFile;

typedef struct {
    char *name;
    GSym *g;
    Section *sec;
    unsigned value, size;
    int info, name_off;
    bool undef;
} DSym;

typedef struct {
    Section *sec;
    int offset, kind;
    GSym *g;
} Pend;

static Pend *pends;
static int npend, cap_pend;

static unsigned elf_hash(const char *name) {
    unsigned h = 0;
    while (*name) {
        unsigned g;
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

static unsigned char *read_bin(const char *path, int *out_len) {
#ifdef CC_HOST
    int fd = open(path, O_RDONLY);
#else
    int fd = open(path, O_READ);
#endif
    if (fd < 0) return NULL;
    int cap = 4096, len = 0;
    unsigned char *buf = xcalloc(cap);
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            unsigned char *nb = xcalloc(cap);
            memcpy(nb, buf, len);
            free(buf);
            buf = nb;
        }
        int n = read(fd, buf + len, cap - len);
        if (n < 0) { close(fd); free(buf); return NULL; }
        if (n == 0) break;
        len += n;
    }
    close(fd);
    *out_len = len;
    return buf;
}

/* Виртуальный адрес загружаемого сегмента -> смещение в файле. -1, если адрес не в файле. */
static int elf_v2off(unsigned char *d, int len, unsigned vaddr) {
    if (len < 52) return -1;
    unsigned phoff = rd32(d + 28), phsz = rd16(d + 42), phnum = rd16(d + 44);
    if (phsz < 32) return -1;
    for (unsigned i = 0; i < phnum; i++) {
        unsigned off = phoff + i * phsz;
        if (off + 32 > (unsigned)len) return -1;
        if (rd32(d + off) != 1) continue;
        unsigned poff = rd32(d + off + 4), pv = rd32(d + off + 8), filesz = rd32(d + off + 16);
        if (vaddr >= pv && vaddr - pv < filesz) {
            unsigned fo = poff + (vaddr - pv);
            if (fo >= (unsigned)len) return -1;
            return (int)fo;
        }
    }
    return -1;
}

static char *elf_name(unsigned char *d, int len, int base, int size, unsigned name_off) {
    if ((int)name_off >= size || size < 0 || base < 0) return NULL;
    int off = base + (int)name_off;
    int lim = base + size;
    if (off < 0 || off >= len || lim > len) return NULL;
    int end = off;
    while (end < lim && d[end]) end++;
    if (end >= lim) return NULL;
    return (char *)(d + off);
}

static bool so_load(const char *path, SoFile *sf) {
    int len = 0;
    unsigned char *d = read_bin(path, &len);
    if (!d) return false;
    sf->bytes = d;
    sf->path = (char *)path;
    sf->need_off = -1;
    if (len < 52 || d[0] != 0x7f || d[1] != 'E' || d[2] != 'L' || d[3] != 'F' || d[4] != 1 || d[5] != 1)
        error("%s: not an ELF file", path);
    if (rd16(d + 16) != 3 || rd16(d + 18) != 3)
        error("%s: not an i386 shared library", path);
    unsigned phoff = rd32(d + 28), phsz = rd16(d + 42), phnum = rd16(d + 44);
    if (phsz < 32 || phnum > 64 || phoff + phnum * phsz > (unsigned)len)
        error("%s: bad program headers", path);
    unsigned dyn_off = 0, dyn_sz = 0;
    for (unsigned i = 0; i < phnum; i++) {
        unsigned off = phoff + i * phsz;
        if (rd32(d + off) != 2) continue;
        dyn_off = rd32(d + off + 4);
        dyn_sz = rd32(d + off + 16);
    }
    if (!dyn_sz || dyn_off + dyn_sz > (unsigned)len || (dyn_sz % 8))
        error("%s: no dynamic segment", path);
    unsigned str_addr = 0, sym_addr = 0, hash_addr = 0, strsz = 0, syment = 16, soname_off = 0;
    bool have_soname = false;
    for (unsigned i = 0; i + 8 <= dyn_sz; i += 8) {
        unsigned tag = rd32(d + dyn_off + i), val = rd32(d + dyn_off + i + 4);
        if (tag == DT_NULL) break;
        if (tag == DT_STRTAB) str_addr = val;
        else if (tag == DT_SYMTAB) sym_addr = val;
        else if (tag == DT_HASH) hash_addr = val;
        else if (tag == DT_STRSZ) strsz = val;
        else if (tag == DT_SYMENT) syment = val;
        else if (tag == DT_SONAME) { soname_off = val; have_soname = true; }
    }
    if (syment != 16) error("%s: unexpected symbol size", path);
    if (!str_addr || !sym_addr || !hash_addr || !strsz) error("%s: dynamic symbol table is incomplete", path);
    int str_off = elf_v2off(d, len, str_addr);
    int sym_off = elf_v2off(d, len, sym_addr);
    int hash_off = elf_v2off(d, len, hash_addr);
    if (str_off < 0 || sym_off < 0 || hash_off < 0 || hash_off + 8 > len)
        error("%s: cannot locate dynamic sections", path);
    unsigned nchain = rd32(d + hash_off + 4);
    if (nchain < 1 || nchain > 100000 || sym_off + nchain * 16 > (unsigned)len)
        error("%s: bad symbol hash", path);
    if (have_soname) {
        sf->soname = elf_name(d, len, str_off, (int)strsz, soname_off);
        if (!sf->soname) error("%s: bad DT_SONAME", path);
    } else sf->soname = (char *)base_name(path);
    for (unsigned i = 1; i < nchain; i++) {
        unsigned char *p = d + sym_off + i * 16;
        unsigned name_off = rd32(p);
        int info = p[12];
        int stt = info & 0xf;
        unsigned shndx = rd16(p + 14);
        if (!shndx || stt == 3 || stt == 4) continue;
        char *name = elf_name(d, len, str_off, (int)strsz, name_off);
        if (!name || !name[0]) continue;
        if (sf->n == sf->cap) {
            sf->cap = sf->cap ? sf->cap * 2 : 16;
            SoSym *ns = xcalloc(sizeof(SoSym) * sf->cap);
            if (sf->syms) memcpy(ns, sf->syms, sizeof(SoSym) * sf->n);
            free(sf->syms);
            sf->syms = ns;
        }
        sf->syms[sf->n].name = name;
        sf->syms[sf->n].value = rd32(p + 4);
        sf->syms[sf->n].size = rd32(p + 8);
        sf->syms[sf->n].info = info;
        sf->n++;
    }
    return true;
}

static SoSym *so_find(SoFile *sos, int n, const char *name, int *idx) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < sos[i].n; j++)
            if (!strcmp(sos[i].syms[j].name, name)) {
                if (idx) *idx = i;
                return &sos[i].syms[j];
            }
    return NULL;
}

static bool so_is_needed(ObjFile **files, int n, SoFile *sos, int nso) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < files[i]->n_secs; j++) {
            Section *s = files[i]->secs[j];
            for (int r = 0; r < s->n_rels; r++) {
                LinkSym *ls = &s->file->syms[s->rels[r].sym];
                if (ls->bind != BIND_GLOBAL || !ls->global || ls->global->state != DEF_NONE) continue;
                if (so_find(sos, nso, ls->name, NULL)) return true;
            }
        }
    return false;
}

static void sort_gsyms(GSym **a, int n) {
    for (int i = 1; i < n; i++) {
        GSym *key = a[i];
        int j = i;
        while (j > 0 && strcmp(a[j - 1]->name, key->name) > 0) { a[j] = a[j - 1]; j--; }
        a[j] = key;
    }
}

static void sort_dsyms(DSym *a, int n) {
    for (int i = 2; i < n; i++) {
        DSym key = a[i];
        int j = i;
        while (j > 1 && strcmp(a[j - 1].name, key.name) > 0) { a[j] = a[j - 1]; j--; }
        a[j] = key;
    }
}

static GSym **gsym_all(int *out_n) {
    int n = 0;
    for (int b = 0; b < GBUCKETS; b++)
        for (GSym *g = gtab[b]; g; g = g->next) n++;
    GSym **a = xcalloc(sizeof(GSym *) * (n + 1));
    int k = 0;
    for (int b = 0; b < GBUCKETS; b++)
        for (GSym *g = gtab[b]; g; g = g->next) a[k++] = g;
    *out_n = k;
    return a;
}

static void pend_add(Section *sec, int offset, int kind, GSym *g) {
    if (npend == cap_pend) {
        cap_pend = cap_pend ? cap_pend * 2 : 64;
        Pend *p = xcalloc(sizeof(Pend) * cap_pend);
        if (pends) memcpy(p, pends, sizeof(Pend) * npend);
        free(pends);
        pends = p;
    }
    pends[npend].sec = sec;
    pends[npend].offset = offset;
    pends[npend].kind = kind;
    pends[npend].g = g;
    npend++;
}

static Section *new_sec(ObjFile *file, char *name, int kind, int align, unsigned char *data, int size) {
    Section *s = file->secs[obj_add_section(file, name, kind, align, data, size)];
    mark(s);
    return s;
}

/* 1 — RELATIVE, 2 — динамический R_386_32, 0 — решается сейчас, -1 — так в .so нельзя. */
static int abs_kind(Section *s, LinkSym *ls, bool shared) {
    bool local = ls->bind == BIND_LOCAL || !ls->global;
    bool wr = s->kind == SEC_DATA || s->kind == SEC_BSS;
    if (shared) {
        if (!wr) return -1;
        return local ? 1 : 2;
    }
    if (!local && wr && ls->global && (ls->global->dyn == DYN_PLT || ls->global->dyn == DYN_SO))
        return 2;
    return 0;
}

static bool slot_globdat(GSym *g) {
    if (opt_shared) return true;
    return g->dyn == DYN_PLT || g->dyn == DYN_SO;
}

static unsigned plt_vaddr(Section *plt, GSym *g) {
    if (!plt || !g->plt_index) error("internal: no PLT entry for '%s'", g->name);
    return plt->addr + (unsigned)(g->plt_index - 1) * 16u;
}

static int dsym_find(DSym *ds, int n, GSym *g) {
    for (int i = 1; i < n; i++) if (ds[i].g == g) return i;
    return 0;
}

static int dsym_add(DSym *ds, int *n, int cap, GSym *g, Section *sec, unsigned value,
                    unsigned size, int info, bool undef) {
    int have = dsym_find(ds, *n, g);
    if (have) return have;
    if (*n >= cap) error("too many dynamic symbols");
    ds[*n].name = g->name;
    ds[*n].g = g;
    ds[*n].sec = sec;
    ds[*n].value = value;
    ds[*n].size = size;
    ds[*n].info = info;
    ds[*n].undef = undef;
    return (*n)++;
}

static void export_file(DSym *ds, int *nd, int cap, ObjFile *f) {
    if (!f) return;
    for (int j = 0; j < f->n_syms; j++) {
        LinkSym *ls = &f->syms[j];
        GSym *g = ls->global;
        if (!g || ls->bind == BIND_LOCAL || g->file != f || g->idx != j || g->state == DEF_NONE) continue;
        if (!strcmp(g->name, "_GLOBAL_OFFSET_TABLE_")) continue;
        Section *sec = NULL;
        unsigned val = 0;
        if (!sym_target(f, j, &sec, &val)) continue;
        if (sec && !sec->live) continue;
        unsigned sz = (g->dyn == DYN_COPY && g->dyn_size) ? g->dyn_size : ls->size;
        dsym_add(ds, nd, cap, g, sec, val, sz, (ls->bind << 4) | ls->type, false);
    }
}

static Section *ref_site(ObjFile **files, int n, GSym *g) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < files[i]->n_secs; j++) {
            Section *s = files[i]->secs[j];
            if (!s->live) continue;
            for (int r = 0; r < s->n_rels; r++)
                if (s->file->syms[s->rels[r].sym].global == g) return s;
        }
    return NULL;
}

static unsigned dyn_foff(Section *s, unsigned base, unsigned data_vaddr, unsigned data_off) {
    if (s->kind == SEC_TEXT || s->kind == SEC_RODATA) return s->addr - base;
    return data_off + (s->addr - data_vaddr);
}

static void put_tag(unsigned char *p, int *n, unsigned tag, unsigned val) {
    wr32(p + *n, tag);
    wr32(p + *n + 4, val);
    *n += 8;
}

static void shdr(Buf *b, unsigned name, unsigned type, unsigned flags, unsigned addr, unsigned off,
                 unsigned size, unsigned link, unsigned info, unsigned align, unsigned entsize) {
    buf_u32(b, name); buf_u32(b, type); buf_u32(b, flags); buf_u32(b, addr);
    buf_u32(b, off); buf_u32(b, size); buf_u32(b, link); buf_u32(b, info);
    buf_u32(b, align); buf_u32(b, entsize);
}

static void phdr(Buf *b, unsigned type, unsigned off, unsigned addr, unsigned filesz,
                 unsigned memsz, unsigned flags, unsigned align) {
    buf_u32(b, type); buf_u32(b, off); buf_u32(b, addr); buf_u32(b, addr);
    buf_u32(b, filesz); buf_u32(b, memsz); buf_u32(b, flags); buf_u32(b, align);
}

static void apply_dyn(unsigned char *image, unsigned load_end, ObjFile **all, int n_all,
                      Section *plt, unsigned base, unsigned data_vaddr, unsigned data_off) {
    unsigned got = got_sec ? got_sec->addr : 0;
    for (int i = 0; i < n_all; i++)
        for (int j = 0; j < all[i]->n_secs; j++) {
            Section *s = all[i]->secs[j];
            if (!s->live || s->kind == SEC_BSS) continue;
            unsigned fo = dyn_foff(s, base, data_vaddr, data_off);
            for (int r = 0; r < s->n_rels; r++) {
                LinkReloc *rl = &s->rels[r];
                if (rl->type == R_386_NONE) continue;
                if (!reloc_ok(rl->type))
                    error("unsupported relocation type %d in %s", rl->type, where(s));
                if (fo + (unsigned)rl->offset + 4 > load_end)
                    error("internal: relocation past the end of the file in %s", s->name);
                unsigned char *p = image + fo;
                LinkSym *ls = &s->file->syms[rl->sym];
                GSym *g = (ls->bind != BIND_LOCAL && ls->global) ? ls->global : NULL;
                unsigned A = rd32(p + rl->offset);
                unsigned P = s->addr + (unsigned)rl->offset;
                Section *t = NULL;
                unsigned v = 0, S = 0;
                switch (rl->type) {
                case R_386_PC32:
                case R_386_PLT32: {
                    bool via = false;
                    if (g && g->dyn == DYN_PLT) {
                        if (rl->type == R_386_PLT32) via = true;
                        else if (g->state == DEF_NONE) {
                            if (opt_shared)
                                error("relocation R_386_PC32 against '%s' in %s; compile with -fPIC",
                                      ls->name, where(s));
                            via = true;
                        }
                    }
                    if (via) S = plt_vaddr(plt, g);
                    else if (!sym_target(s->file, rl->sym, &t, &v))
                        error("undefined reference to '%s' in %s", ls->name, where(s));
                    else S = (t ? t->addr : 0) + v;
                    wr32(p + rl->offset, S + A - P);
                    break;
                }
                case R_386_GOTPC:
                case R_386_GOTOFF:
                    if (!got) error("relocation against the GOT in %s, but there is no GOT", where(s));
                    if (rl->type == R_386_GOTOFF && !sym_target(s->file, rl->sym, &t, &v))
                        error("undefined reference to '%s' in %s", ls->name, where(s));
                    S = (t ? t->addr : 0) + v;
                    wr32(p + rl->offset, rl->type == R_386_GOTPC ? got + A - P : S + A - got);
                    break;
                case R_386_GOT32:
                    wr32(p + rl->offset, (unsigned)got_slot_off(s->file, rl->sym) + A);
                    break;
                case R_386_32: {
                    int ak = abs_kind(s, ls, opt_shared);
                    if (ak < 0)
                        error("relocation R_386_32 against '%s' in %s; a shared library needs -fPIC",
                              ls->name ? ls->name : "a local symbol", where(s));
                    if (ak == 2) break;
                    if (g && g->dyn == DYN_PLT && g->state == DEF_NONE) S = plt_vaddr(plt, g);
                    else if (!sym_target(s->file, rl->sym, &t, &v))
                        error("undefined reference to '%s' in %s", ls->name, where(s));
                    else S = (t ? t->addr : 0) + v;
                    wr32(p + rl->offset, S + A);
                    break;
                }
                default:
                    error("unsupported relocation type %d in %s", rl->type, where(s));
                }
            }
        }
}

static bool link_dyn(ObjFile **files, int n_files, ObjFile *common,
                     char **so_paths, int n_so, const char *out_path) {
    npend = 0;
    if (n_so > 64) error("too many shared libraries");
    SoFile *sos = NULL;
    int nso = 0;
    if (n_so) {
        sos = xcalloc(sizeof(SoFile) * n_so);
        for (int i = 0; i < n_so; i++)
            if (!so_load(so_paths[i], &sos[i])) error("cannot read %s", so_paths[i]);
        nso = n_so;
    }
    if (!opt_shared && !so_is_needed(files, n_files, sos, nso)) return false;

    if (opt_shared) {
        for (int i = 0; i < n_files; i++) {
            if (files[i]->from_lib) continue;
            for (int j = 0; j < files[i]->n_syms; j++) {
                LinkSym *ls = &files[i]->syms[j];
                if (ls->bind == BIND_LOCAL) continue;
                if (ls->sec >= 0) mark(files[i]->secs[ls->sec]);
                else if (ls->sec == SYM_COMMON && ls->global && ls->global->common)
                    mark(ls->global->common);
            }
        }
    } else {
        GSym *st = gsym_get("__libc_start", false);
        if (st && st->state != DEF_NONE) {
            Section *ss = NULL;
            unsigned sv = 0;
            if (!sym_target(st->file, st->idx, &ss, &sv))
                error("undefined reference to '__libc_start'");
            mark(ss);
        }
    }
    mark_from_relocs(true);

    for (int i = 0; i < n_files; i++)
        for (int j = 0; j < files[i]->n_secs; j++) {
            Section *s = files[i]->secs[j];
            if (!s->live) continue;
            for (int r = 0; r < s->n_rels; r++) {
                LinkSym *ls = &s->file->syms[s->rels[r].sym];
                if (ls->bind == BIND_LOCAL || !ls->global) continue;
                if (ls->bind == BIND_WEAK && ls->global->state == DEF_NONE) continue;
                int ty = s->rels[r].type;
                if (ty == R_386_PC32 || ty == R_386_PLT32) ls->global->saw_call = 1;
                else if (ty == R_386_32 || ty == R_386_GOT32) ls->global->saw_addr = 1;
            }
        }
    GSym *start = gsym_get("__libc_start", false);
    if (!opt_shared && start && start->state == DEF_NONE) start->saw_call = 1;

    int ng = 0;
    GSym **allg = gsym_all(&ng);
    for (int i = 0; i < ng; i++) {
        GSym *g = allg[i];
        if (!g->saw_call && !g->saw_addr) continue;
        if (g->state != DEF_NONE) {
            if (opt_shared && g->saw_call && g->state != DEF_COMMON) {
                LinkSym *ls = &g->file->syms[g->idx];
                if (ls->type == STY_OBJECT) error("'%s' is not a function", g->name);
                g->dyn = DYN_PLT;
            }
            continue;
        }
        int si = -1;
        SoSym *so = so_find(sos, nso, g->name, &si);
        if (!so) {
            /* Функция без библиотеки остаётся неопределённой: её найдёт ld.so
             * (например dlopen) или библиотека, подгруженная позже. */
            if (!opt_shared && g->saw_call) {
                g->dyn = DYN_PLT;
                g->dyn_size = 0;
                continue;
            }
            Section *site = ref_site(files, n_files, g);
            if (site) error("undefined reference to '%s' in %s", g->name, where(site));
            error("undefined reference to '%s'", g->name);
        }
        int stt = so->info & 0xf;
        bool data = stt == STY_OBJECT || (stt != STY_FUNC && !g->saw_call);
        if (data && g->saw_call) error("'%s' is not a function", g->name);
        sos[si].used = true;
        g->dyn_size = so->size;
        if (data && !opt_shared) g->dyn = DYN_COPY;
        else if (data) g->dyn = DYN_SO;
        else g->dyn = DYN_PLT;
    }
    if (!opt_shared) {
        bool need = false;
        for (int i = 0; i < ng; i++) if (allg[i]->dyn != DYN_NONE) need = true;
        if (!need) {
            for (int i = 0; i < n_files; i++)
                for (int j = 0; j < files[i]->n_secs; j++) files[i]->secs[j]->live = false;
            for (int j = 0; j < common->n_secs; j++) common->secs[j]->live = false;
            for (int i = 0; i < ng; i++) {
                allg[i]->dyn = DYN_NONE;
                allg[i]->saw_call = allg[i]->saw_addr = allg[i]->plt_index = 0;
                allg[i]->dyn_size = 0;
            }
            n_work = 0;
            return false;
        }
    }

    GSym **copies = xcalloc(sizeof(GSym *) * (ng + 1));
    int ncopy = 0;
    for (int i = 0; i < ng; i++) if (allg[i]->dyn == DYN_COPY) copies[ncopy++] = allg[i];
    sort_gsyms(copies, ncopy);
    for (int i = 0; i < ncopy; i++) {
        GSym *g = copies[i];
        unsigned sz = g->dyn_size ? g->dyn_size : 4;
        if (sz > 1024u * 1024u) error("symbol '%s' is too large to copy", g->name);
        int al = sz <= 4 ? 4 : sz <= 8 ? 8 : 16;
        int si = obj_add_section(common, g->name, SEC_BSS, al, NULL, (int)sz);
        mark(common->secs[si]);
        int idx = obj_add_sym(common, g->name, BIND_GLOBAL, STY_OBJECT, si, 0, sz);
        common->syms[idx].global = g;
        g->state = DEF_STRONG;
        g->file = common;
        g->idx = idx;
        g->dyn_size = sz;
        pend_add(common->secs[si], 0, PREL_COPY, g);
    }

    ensure_got(common);
    mark(got_sec);
    for (int i = 0; i < n_files; i++)
        for (int j = 0; j < files[i]->n_secs; j++) {
            Section *s = files[i]->secs[j];
            if (!s->live) continue;
            for (int r = 0; r < s->n_rels; r++) {
                if (s->rels[r].type != R_386_GOT32) continue;
                LinkSym *ls = &s->file->syms[s->rels[r].sym];
                if (ls->bind == BIND_LOCAL || !ls->global)
                    error("R_386_GOT32 against local symbol '%s' in %s", ls->name, where(s));
                got_add(ls->global);
            }
        }
    GSym **plt_gs = xcalloc(sizeof(GSym *) * (ng + 1));
    int nplt = 0;
    for (int i = 0; i < ng; i++) if (allg[i]->dyn == DYN_PLT) plt_gs[nplt++] = allg[i];
    sort_gsyms(plt_gs, nplt);
    for (int i = 0; i < nplt; i++) plt_gs[i]->plt_index = i + 1;
    int gsz = 12 + n_got_slots * 4 + nplt * 4;
    free(got_sec->data);
    got_sec->data = xcalloc(gsz);
    got_sec->size = gsz;
    {
        GSym *gg = gsym_get("_GLOBAL_OFFSET_TABLE_", false);
        if (gg && gg->file) gg->file->syms[gg->idx].size = (unsigned)gsz;
    }
    Section *plt = nplt ? new_sec(common, ".plt", SEC_TEXT, 16, NULL, nplt * 16) : NULL;

    if (opt_shared) {
        for (int i = 0; i < n_files; i++)
            for (int j = 0; j < files[i]->n_secs; j++) {
                Section *s = files[i]->secs[j];
                if (!s->live || s->kind != SEC_RODATA) continue;
                for (int r = 0; r < s->n_rels; r++)
                    if (s->rels[r].type == R_386_32) { s->kind = SEC_DATA; break; }
            }
    }
    for (int i = 0; i < n_files; i++)
        for (int j = 0; j < files[i]->n_secs; j++) {
            Section *s = files[i]->secs[j];
            if (!s->live) continue;
            for (int r = 0; r < s->n_rels; r++) {
                if (s->rels[r].type != R_386_32) continue;
                LinkSym *ls = &s->file->syms[s->rels[r].sym];
                int ak = abs_kind(s, ls, opt_shared);
                if (ak < 0)
                    error("relocation R_386_32 against '%s' in %s; a shared library needs -fPIC",
                          ls->name ? ls->name : "a local symbol", where(s));
                if (!ak) continue;
                GSym *g = (ls->bind != BIND_LOCAL && ls->global) ? ls->global : NULL;
                if (ak == 2 && !g) error("internal: absolute relocation without a symbol in %s", where(s));
                pend_add(s, s->rels[r].offset, ak == 1 ? PREL_REL : PREL_ABS, g);
            }
        }
    for (int i = 0; i < n_got_slots; i++)
        if (slot_globdat(got_slots[i].g))
            pend_add(got_sec, got_slots[i].off, PREL_GLOB, got_slots[i].g);
    for (int i = 0; i < nplt; i++)
        pend_add(got_sec, 12 + n_got_slots * 4 + i * 4, PREL_JMP, plt_gs[i]);
    if (opt_shared) pend_add(got_sec, 0, PREL_GOT0, NULL);

    int dcap = ng + common->n_syms + 2;
    for (int i = 0; i < n_files; i++) dcap += files[i]->n_syms;
    DSym *ds = xcalloc(sizeof(DSym) * dcap);
    ds[0].name = "";
    ds[0].undef = true;
    int nd = 1;
    for (int i = 0; i < n_files; i++) export_file(ds, &nd, dcap, files[i]);
    export_file(ds, &nd, dcap, common);
    for (int i = 0; i < ng; i++) {
        GSym *g = allg[i];
        if (g->dyn == DYN_NONE || dsym_find(ds, nd, g)) continue;
        if (g->state != DEF_NONE) {
            Section *sec = NULL;
            unsigned val = 0;
            if (!sym_target(g->file, g->idx, &sec, &val) || (sec && !sec->live))
                error("internal: '%s' has no dynamic symbol", g->name);
            LinkSym *ls = &g->file->syms[g->idx];
            unsigned sz = (g->dyn == DYN_COPY && g->dyn_size) ? g->dyn_size : ls->size;
            dsym_add(ds, &nd, dcap, g, sec, val, sz, (ls->bind << 4) | ls->type, false);
        } else {
            int stt = g->dyn == DYN_PLT ? STY_FUNC : STY_OBJECT;
            dsym_add(ds, &nd, dcap, g, NULL, 0, g->dyn_size, (BIND_GLOBAL << 4) | stt, true);
        }
    }
    sort_dsyms(ds, nd);

    const char *soname = opt_soname ? opt_soname : base_name(out_path);
    Buf str = {0};
    buf_byte(&str, 0);
    int soname_off = 0;
    if (opt_shared) {
        soname_off = str.len;
        buf_put(&str, soname, (int)strlen(soname) + 1);
    }
    int nneed = 0;
    for (int i = 0; i < nso; i++) if (sos[i].used) {
        sos[i].need_off = str.len;
        buf_put(&str, sos[i].soname, (int)strlen(sos[i].soname) + 1);
        nneed++;
    }
    for (int i = 1; i < nd; i++) {
        ds[i].name_off = str.len;
        buf_put(&str, ds[i].name, (int)strlen(ds[i].name) + 1);
    }
    Section *dynstr = new_sec(common, ".dynstr", SEC_RODATA, 1, str.data, str.len);
    int nbucket = nd;
    Section *hash = new_sec(common, ".hash", SEC_RODATA, 4, NULL, (2 + nbucket * 2) * 4);
    wr32(hash->data, (unsigned)nbucket);
    wr32(hash->data + 4, (unsigned)nd);
    for (int i = 1; i < nd; i++) {
        unsigned h = elf_hash(ds[i].name) % (unsigned)nbucket;
        unsigned next = rd32(hash->data + 8 + (int)h * 4);
        wr32(hash->data + 8 + nbucket * 4 + i * 4, next);
        wr32(hash->data + 8 + (int)h * 4, (unsigned)i);
    }
    Section *dynsym = new_sec(common, ".dynsym", SEC_RODATA, 4, NULL, nd * 16);
    int nrel = 0, njmp = 0;
    for (int i = 0; i < npend; i++)
        if (pends[i].kind == PREL_JMP) njmp++;
        else nrel++;
    if (njmp != nplt) error("internal: PLT relocation count");
    Section *reldyn = nrel ? new_sec(common, ".rel.dyn", SEC_RODATA, 4, NULL, nrel * 8) : NULL;
    Section *relplt = njmp ? new_sec(common, ".rel.plt", SEC_RODATA, 4, NULL, njmp * 8) : NULL;
    int ntags = nneed + (opt_shared ? 1 : 0) + 6 + (nrel ? 3 : 0) + (nplt ? 3 : 0) + 3;
    Section *dynamic = new_sec(common, ".dynamic", SEC_DATA, 4, NULL, ntags * 8);
    char interp_buf[12] = "/lib/ld.so";
    Section *interp = opt_shared ? NULL : new_sec(common, ".interp", SEC_RODATA, 1, (unsigned char *)interp_buf, 12);

    int phnum = opt_shared ? 3 : 4;
    unsigned rx_org = align_up(52u + 32u * (unsigned)phnum, 16);
    unsigned base = opt_shared ? 0 : TEXT_BASE;
    /* Программу с PT_INTERP ядро запускает со стеком auxv, поэтому заглушка
     * читает argc/argv как Linux. У ld.so (-shared и есть _start) заглушка
     * передаёт указатель на этот стек и не зависит от адреса загрузки. */
    const unsigned char *stub = NULL;
    int stub_len = 0;
    unsigned entry_off = 0;
    unsigned pos = rx_org;
    GSym *entry_fn = NULL;
    if (opt_shared) {
        GSym *st = gsym_get("_start", false);
        if (st && st->state != DEF_NONE) {
            static const unsigned char stub_pic[] = { 0x89, 0xE0, 0x50 }; /* mov eax, esp; push eax */
            stub = stub_pic;
            stub_len = 3;
            entry_fn = st;
        }
    } else {
        static const unsigned char stub_aux[] = { 0x8B, 0x04, 0x24, 0x8D, 0x4C, 0x24, 0x04, 0x8D, 0x54, 0x81, 0x04, 0x52, 0x51, 0x50 };
        stub = stub_aux;
        stub_len = (int)sizeof(stub_aux);
        entry_fn = start;
    }
    if (stub) {
        entry_off = pos;
        pos = align_up(pos + (unsigned)stub_len + 7, 16);
    }
    ObjFile **all = xcalloc(sizeof(ObjFile *) * (n_files + 1));
    for (int i = 0; i < n_files; i++) all[i] = files[i];
    all[n_files] = common;
    int n_all = n_files + 1;
    static const int order[] = { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS };
    unsigned text_filesz = 0, data_filesz = 0, data_vaddr = 0, bss_end = 0;
    for (int k = 0; k < 4; k++) {
        if (order[k] == SEC_DATA) {
            text_filesz = pos;
            data_vaddr = align_up(base + pos, 4096);
            pos = 0;
        }
        if (order[k] == SEC_BSS) {
            if (!pos) pos = 4;
            data_filesz = pos;
        }
        for (int i = 0; i < n_all; i++)
            for (int j = 0; j < all[i]->n_secs; j++) {
                Section *s = all[i]->secs[j];
                if (!s->live || s->kind != order[k]) continue;
                pos = align_up(pos, (unsigned)s->align);
                s->addr = (k < 2 ? base : data_vaddr) + pos;
                pos += (unsigned)s->size;
            }
    }
    bss_end = pos;
    unsigned data_off = align_up(text_filesz, 4096);
    if (text_filesz < rx_org) error("internal: text segment does not cover the header");

    int ntab = 0;
    for (int i = 0; i < n_all; i++)
        for (int j = 0; j < all[i]->n_secs; j++)
            if (all[i]->secs[j]->live) ntab++;
    if (ntab + 2 > 65535) error("too many sections");
    Section **tab = xcalloc(sizeof(Section *) * (ntab + 1));
    for (int i = 0, k = 0; i < n_all; i++)
        for (int j = 0; j < all[i]->n_secs; j++)
            if (all[i]->secs[j]->live) tab[k++] = all[i]->secs[j];
    for (int i = 0; i < ntab; i++) tab[i]->shndx = i + 1;

    for (int i = 0; i < nd; i++) {
        unsigned S = 0;
        int ndx = 0;
        if (i && !ds[i].undef && ds[i].sec) {
            S = ds[i].sec->addr + ds[i].value;
            ndx = ds[i].sec->shndx;
            if (!ndx) error("internal: section of '%s' was not emitted", ds[i].name);
        } else if (i && !ds[i].undef) {
            S = ds[i].value;
            ndx = 0xfff1;
        }
        unsigned char *p = dynsym->data + i * 16;
        wr32(p, i ? (unsigned)ds[i].name_off : 0);
        wr32(p + 4, S);
        wr32(p + 8, ds[i].size);
        p[12] = (unsigned char)ds[i].info;
        p[14] = (unsigned char)ndx;
        p[15] = (unsigned char)(ndx >> 8);
    }
    int idyn = 0, iplt = 0;
    for (int i = 0; i < npend; i++) {
        Pend *r = &pends[i];
        unsigned off = r->sec->addr + (unsigned)r->offset;
        int sym = 0, typ = R_386_RELATIVE;
        if (r->kind != PREL_REL && r->kind != PREL_GOT0) {
            sym = dsym_find(ds, nd, r->g);
            if (!sym) error("internal: '%s' is missing from .dynsym", r->g->name);
        }
        if (r->kind == PREL_ABS) typ = R_386_32;
        else if (r->kind == PREL_GLOB) typ = R_386_GLOB_DAT;
        else if (r->kind == PREL_JMP) typ = R_386_JMP_SLOT;
        else if (r->kind == PREL_COPY) typ = R_386_COPY;
        Section *dest = r->kind == PREL_JMP ? relplt : reldyn;
        if (!dest) error("internal: missing relocation section");
        int at = r->kind == PREL_JMP ? iplt : idyn;
        if (at + 8 > dest->size) error("internal: relocation overflow");
        wr32(dest->data + at, off);
        wr32(dest->data + at + 4, ((unsigned)sym << 8) | (unsigned)typ);
        if (r->kind == PREL_JMP) iplt += 8;
        else idyn += 8;
    }
    if (reldyn && idyn != reldyn->size) error("internal: .rel.dyn size");
    if (relplt && iplt != relplt->size) error("internal: .rel.plt size");
    wr32(got_sec->data, dynamic->addr);
    for (int i = 0; i < n_got_slots; i++) {
        if (slot_globdat(got_slots[i].g)) continue;
        Section *t = NULL;
        unsigned v = 0;
        GSym *g = got_slots[i].g;
        if (!sym_target(g->file, g->idx, &t, &v)) error("undefined reference to '%s'", g->name);
        wr32(got_sec->data + got_slots[i].off, (t ? t->addr : 0) + v);
    }
    for (int i = 0; i < nplt; i++) {
        unsigned char *p = plt->data + i * 16;
        unsigned slot = (unsigned)(12 + n_got_slots * 4 + i * 4);
        if (opt_shared) { p[0] = 0xff; p[1] = 0xa3; wr32(p + 2, slot); }
        else { p[0] = 0xff; p[1] = 0x25; wr32(p + 2, got_sec->addr + slot); }
        for (int k = 6; k < 16; k++) p[k] = 0x90;
    }
    {
        int n = 0;
        unsigned char *p = dynamic->data;
        for (int i = 0; i < nso; i++) if (sos[i].used) put_tag(p, &n, DT_NEEDED, (unsigned)sos[i].need_off);
        if (opt_shared) put_tag(p, &n, DT_SONAME, (unsigned)soname_off);
        put_tag(p, &n, DT_SYMTAB, dynsym->addr);
        put_tag(p, &n, DT_SYMENT, 16);
        put_tag(p, &n, DT_STRTAB, dynstr->addr);
        put_tag(p, &n, DT_STRSZ, (unsigned)dynstr->size);
        put_tag(p, &n, DT_HASH, hash->addr);
        put_tag(p, &n, DT_PLTGOT, got_sec->addr);
        if (reldyn) {
            put_tag(p, &n, DT_REL, reldyn->addr);
            put_tag(p, &n, DT_RELSZ, (unsigned)reldyn->size);
            put_tag(p, &n, DT_RELENT, 8);
        }
        if (relplt) {
            put_tag(p, &n, DT_JMPREL, relplt->addr);
            put_tag(p, &n, DT_PLTRELSZ, (unsigned)relplt->size);
            put_tag(p, &n, DT_PLTREL, DT_REL);
        }
        put_tag(p, &n, DT_BIND_NOW, 0);
        put_tag(p, &n, DT_FLAGS, DF_BIND_NOW);
        put_tag(p, &n, DT_NULL, 0);
        if (n != dynamic->size) error("internal: dynamic section size");
    }

    Buf shstr = {0};
    buf_byte(&shstr, 0);
    int *nameoff = xcalloc(sizeof(int) * (ntab + 1));
    for (int i = 0; i < ntab; i++) {
        nameoff[i] = shstr.len;
        buf_put(&shstr, tab[i]->name, (int)strlen(tab[i]->name) + 1);
    }
    int shstr_name = shstr.len;
    buf_put(&shstr, ".shstrtab", 10);

    if (data_off + data_filesz > 64u * 1024u * 1024u) error("output is too large");
    int load_end = (int)(data_off + data_filesz);
    int shstr_off = (load_end + 3) & ~3;
    int shnum = ntab + 2;
    int shdr_off = shstr_off + shstr.len;
    int tail = shdr_off + shnum * 40;
    unsigned char *image = xcalloc(tail);
    for (int i = 0; i < n_all; i++)
        for (int j = 0; j < all[i]->n_secs; j++) {
            Section *s = all[i]->secs[j];
            if (!s->live || s->kind == SEC_BSS) continue;
            unsigned fo = dyn_foff(s, base, data_vaddr, data_off);
            if (fo + (unsigned)s->size > (unsigned)load_end)
                error("internal: section '%s' does not fit", s->name);
            if (s->size && s->data) memcpy(image + fo, s->data, (unsigned)s->size);
        }
    if (stub) {
        unsigned char *ip = image + entry_off;
        memcpy(ip, stub, (unsigned)stub_len);
        ip[stub_len] = 0xE8;
        unsigned target;
        if (!opt_shared && entry_fn && entry_fn->dyn == DYN_PLT) target = plt_vaddr(plt, entry_fn);
        else {
            Section *ss = NULL;
            unsigned sv = 0;
            if (!entry_fn || !sym_target(entry_fn->file, entry_fn->idx, &ss, &sv))
                error("undefined reference to '%s'", entry_fn ? entry_fn->name : "entry");
            target = (ss ? ss->addr : 0) + sv;
        }
        wr32(ip + stub_len + 1, target - (base + entry_off + (unsigned)stub_len + 5));
        ip[stub_len + 5] = 0xEB;
        ip[stub_len + 6] = 0xFE;
    }
    apply_dyn(image, (unsigned)load_end, all, n_all, plt, base, data_vaddr, data_off);

    Buf shb = {0};
    shdr(&shb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < ntab; i++) {
        Section *s = tab[i];
        unsigned type = SHT_PROGBITS, link = 0, info = 0, entsize = 0;
        unsigned flags = SHF_ALLOC;
        if (s->kind == SEC_DATA || s->kind == SEC_BSS) flags |= SHF_WRITE;
        if (s->kind == SEC_TEXT) flags |= SHF_EXECINSTR;
        if (s == dynsym) { type = SHT_DYNSYM; link = (unsigned)dynstr->shndx; info = 1; entsize = 16; }
        else if (s == dynstr) type = SHT_STRTAB;
        else if (s == hash) { type = SHT_HASH; link = (unsigned)dynsym->shndx; entsize = 4; }
        else if (s == reldyn) { type = SHT_REL; link = (unsigned)dynsym->shndx; entsize = 8; }
        else if (s == relplt) {
            type = SHT_REL; link = (unsigned)dynsym->shndx;
            info = plt ? (unsigned)plt->shndx : 0; entsize = 8;
        } else if (s == dynamic) { type = SHT_DYNAMIC; link = (unsigned)dynstr->shndx; entsize = 8; }
        else if (s->kind == SEC_BSS) type = SHT_NOBITS;
        shdr(&shb, (unsigned)nameoff[i], type, flags, s->addr, dyn_foff(s, base, data_vaddr, data_off),
             (unsigned)s->size, link, info, (unsigned)s->align, entsize);
    }
    shdr(&shb, (unsigned)shstr_name, SHT_STRTAB, 0, 0, (unsigned)shstr_off, (unsigned)shstr.len, 0, 0, 1, 0);
    if (shb.len != shnum * 40) error("internal: section header size");
    memcpy(image + shstr_off, shstr.data, (unsigned)shstr.len);
    memcpy(image + shdr_off, shb.data, (unsigned)shb.len);

    Buf h = {0};
    buf_put(&h, "\x7f" "ELF\x01\x01\x01\0\0\0\0\0\0\0\0\0", 16);
    buf_u16(&h, opt_shared ? 3 : 2);
    buf_u16(&h, 3);
    buf_u32(&h, 1);
    buf_u32(&h, stub ? base + entry_off : 0);
    buf_u32(&h, 52);
    buf_u32(&h, (unsigned)shdr_off);
    buf_u32(&h, 0);
    buf_u16(&h, 52);
    buf_u16(&h, 32);
    buf_u16(&h, (unsigned)phnum);
    buf_u16(&h, 40);
    buf_u16(&h, (unsigned)shnum);
    buf_u16(&h, (unsigned)(ntab + 1));
    if (interp)
        phdr(&h, 3, dyn_foff(interp, base, data_vaddr, data_off), interp->addr,
             (unsigned)interp->size, (unsigned)interp->size, 4, 1);
    phdr(&h, 1, 0, base, text_filesz, text_filesz, 5, 0x1000);
    phdr(&h, 1, data_off, data_vaddr, data_filesz, bss_end, 6, 0x1000);
    phdr(&h, 2, dyn_foff(dynamic, base, data_vaddr, data_off), dynamic->addr,
         (unsigned)dynamic->size, (unsigned)dynamic->size, 6, 4);
    if (h.len != 52 + 32 * phnum) error("internal: program header size");
    memcpy(image, h.data, (unsigned)h.len);

    Buf out = {0};
    out.data = image;
    out.len = tail;
    out.cap = tail;
    write_file(out_path, &out);
    return true;
}
