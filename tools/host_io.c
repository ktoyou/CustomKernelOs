/* host_io.c — та часть утилиты myfs, что работает с файлами компьютера.
 * Отдельный файл, потому что здесь стандартные заголовки ОС, а в myfs.c —
 * заголовки ядра MyOS (у них свой size_t, вместе их не скрестить). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *img;
static unsigned img_sectors;
void host_close_image(void);

int host_open_image(const char *path) {
    host_close_image();
    img = fopen(path, "r+b");
    if (!img) return -1;
    fseek(img, 0, SEEK_END);
    img_sectors = (unsigned)(ftell(img) / 512);
    return 0;
}

unsigned host_sectors(void) { return img_sectors; }

int host_read_sector(unsigned lba, void *buf) {
    if (lba >= img_sectors || fseek(img, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 1, 512, img) == 512 ? 0 : -1;
}

int host_write_sector(unsigned lba, const void *buf) {
    if (lba >= img_sectors || fseek(img, (long)lba * 512, SEEK_SET)) return -1;
    return fwrite(buf, 1, 512, img) == 512 ? 0 : -1;
}

void host_close_image(void) {
    extern int block_flush(void);
    extern void block_cache_invalidate(void);
    if (img) { block_flush(); fclose(img); img = NULL; }
    /* The cache is keyed by LBA, not by image. Drop it before another file is opened. */
    block_cache_invalidate();
}

/* новый пустой образ: его размер и станет размером ФС при форматировании */
int host_create_image(const char *path, unsigned sectors) {
    FILE *f = fopen(path, "w+b");
    if (!f) return -1;
    if (ftruncate(fileno(f), (off_t)sectors * 512)) { fclose(f); return -1; }
    fclose(f);
    return host_open_image(path);
}

void host_remove_file(const char *path) { remove(path); }

/* подменить образ новым; прежний остаётся рядом как path.bak */
int host_replace_image(const char *path, const char *neu) {
    char bak[4096];
    size_t n = strlen(path);
    if (n + 5 >= sizeof(bak)) return -1;
    memcpy(bak, path, n);
    memcpy(bak + n, ".bak", 5);
    remove(bak);
    if (rename(path, bak)) return -1;
    if (rename(neu, path)) { rename(bak, path); return -1; }
    return 0;
}

/* прочитать файл компьютера целиком; *len = размер */
int host_save_file(const char *path, const void *data, unsigned len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

void *host_load_file(const char *path, unsigned *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (unsigned)n;
    return buf;
}

void host_print(const char *s) { fputs(s, stdout); }
void host_error(const char *s) { fputs(s, stderr); }

void *host_alloc(unsigned n) {
    void *p = malloc(n ? n : 1);
    if (!p) { host_error("myfs: out of memory\n"); exit(1); }
    return p;
}

void host_free(void *p) { free(p); }
