/* tests/desktop_host.c — рабочий стол (user/desktop.c) на Linux, без QEMU.
 * Системные вызовы MyOS заменены заглушками; ввод — сценарий из аргументов:
 *
 *   build/desktop-host WxH команда...
 *     m X Y          мышь в точку        d / u        нажать / отпустить левую кнопку
 *     k CODE         клавиша             w N          N оборотов главного цикла
 *     win PID W H T  программа PID открывает окно W x H с заголовком T
 *     s FILE.ppm     снимок экрана (то, что «показано» через gfx_flip_rect)
 *
 * Собирается с AddressSanitizer: make desktoptest. */
#define main desktop_main
#include "../user/desktop.c"
#undef main

/* -------- экран и мышь */
static int scr_w = 1024, scr_h = 768;
static color_t *screen;
static struct mouse cur_mouse;
static int pending_key;

int __syscall(int n, int a, int b, int c) {
    (void)c;
    switch (n) {
    case SYS_GFX_INFO: { unsigned *gi = (unsigned *)a; gi[0] = scr_w; gi[1] = scr_h; gi[2] = 32; gi[3] = scr_w * 4; return 0; }
    case SYS_GFX_INFO2: {
        struct gfx_info2 *g = (struct gfx_info2 *)a;
        memset(g, 0, sizeof(*g));
        g->width = scr_w; g->height = scr_h; g->bpp = 32; g->pitch = scr_w * 4;
        return 0;
    }
    case SYS_GFX_ACQUIRE: return 0;
    case SYS_GFX_FONT: return 0;
    case SYS_GFX_BLIT: {
        color_t *src = (color_t *)a;
        int *r = (int *)b;
        for (int y = r[1]; y < r[1] + r[3]; y++)
            for (int x = r[0]; x < r[0] + r[2]; x++)
                if (x >= 0 && y >= 0 && x < scr_w && y < scr_h) screen[y * scr_w + x] = src[y * scr_w + x];
        return 0;
    }
    case SYS_MOUSE: *(struct mouse *)a = cur_mouse; return 0;
    case SYS_GFX_ACCEL: {                         /* «видеокарта»: копирование на экране */
        int *p = (int *)b;
        if (a != 1) return -1;
        int sx = p[0], sy = p[1], dx = p[2], dy = p[3], w = p[4], h = p[5];
        color_t *tmp = malloc(w * h * 4);
        for (int y = 0; y < h; y++) memcpy(tmp + y * w, screen + (sy + y) * scr_w + sx, w * 4);
        for (int y = 0; y < h; y++) memcpy(screen + (dy + y) * scr_w + dx, tmp + y * w, w * 4);
        free(tmp);
        return 0;
    }
    default: return -1;
    }
}

/* -------- сообщения и общая память */
static struct msg queue[64];
static int nq;
static void *shms[64];
static int nshm;
static unsigned shm_bytes[64];
int shm_grant(int id,int pid,unsigned rights){(void)id;(void)pid;(void)rights;return 0;}
int shm_discard(int id){(void)id;return 0;}
int shm_size(int id){return id>=0 && id<nshm?(int)shm_bytes[id]:-1;}

int msg_send(int pid, struct msg *m) { (void)pid; (void)m; return 0; }
int msg_recv(struct msg *m, int timeout_ms);
void *shm_map(int id) { return id >= 0 && id < nshm ? shms[id] : NULL; }
int shm_unmap(void *p) { (void)p; return 0; }
int shm_alloc(unsigned size) { shm_bytes[nshm]=size;shms[nshm] = calloc(1, size); return nshm++; }
int service_find(const char *n) { (void)n; return 0; }
int service_register(const char *n) { (void)n; return 0; }
int account(int op, const char *name, const char *password) {
    (void)name; (void)password; return op == 4 ? 0 : -1;
}
int account_list(char *name, unsigned index) { (void)name; (void)index; return -1; }
int ttymode(int m) { return m; }
int readkey(void) { int k = pending_key; pending_key = 0; return k; }
static unsigned fake_ticks;
unsigned ticks(void) { return fake_ticks += 10; }
void sleep_ms(unsigned ms) { (void)ms; }
int datetime_get(struct datetime *t) { t->year = 2026; t->month = 11; t->day = 5; t->hour = 17; t->min = 42; t->sec = 0; return 0; }
int get_time(struct datetime *t) { return datetime_get(t); }
static int alive_pids[64], nalive;
int ps(struct proc_info *pi, int max) {
    int n = 0;
    for (int i = 0; i < nalive && n < max; i++) { memset(&pi[n], 0, sizeof(pi[n])); pi[n].pid = alive_pids[i]; pi[n].state = 2; n++; }
    return n;
}
int fork(void) { return -1; }
int spawn(const char *p, char **a, int fd) { (void)p; (void)a; (void)fd; return -1; }
int exec(const char *p, char **a) { (void)p; (void)a; return -1; }
int pipe(int f[2]) { (void)f; return -1; }
int dup2(int a, int b) { (void)a; (void)b; return -1; }
int waitpid(int p, int *s, int o) { (void)p; (void)s; (void)o; return 0; }
int kill(int p, ...) { (void)p; return 0; }

/* -------- сценарий */
static char **script;
static int nscript, pos, wait_left;

static void snapshot(const char *file) {
    int fd = 1;
    (void)fd;
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", scr_w, scr_h);
    unsigned char *buf = malloc(scr_w * scr_h * 3 + n);
    memcpy(buf, hdr, n);
    for (int i = 0; i < scr_w * scr_h; i++) {
        buf[n + i * 3] = screen[i] >> 16; buf[n + i * 3 + 1] = screen[i] >> 8; buf[n + i * 3 + 2] = screen[i];
    }
    extern void *fopen(const char *, const char *);
    extern unsigned fwrite(const void *, unsigned, unsigned, void *);
    extern int fclose(void *);
    void *f = fopen(file, "wb");
    fwrite(buf, 1, n + scr_w * scr_h * 3, f);
    fclose(f);
    free(buf);
    printf("snapshot %s\n", file);
}

static int fake_window(int pid, int w, int h, const char *title) {
    int id = shm_alloc(w * h * 4);
    color_t *px = shms[id];
    for (int y = 0; y < h; y++)                      /* содержимое: светлый фон и немного текста */
        for (int x = 0; x < w; x++) px[y * w + x] = RGB(250, 250, 250);
    color_t *sb = gfx_buf; int sw = gfx_w, sh = gfx_h;
    gfx_buf = px; gfx_w = w; gfx_h = h;
    gfx_no_clip();
    gfx_text(10, 10, "Hello from a window! Привет!", RGB(40, 40, 40), TRANSPARENT);
    ui_button(10, 40, 100, 32, "Button", false);
    ui_button_accent(120, 40, 100, 32, "OK", false);
    ui_sunken(10, 84, w - 20, 30, C_WHITE);
    ui_text(18, 90, "Text field", UI_TEXT);
    gfx_buf = sb; gfx_w = sw; gfx_h = sh;
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.from = pid; m.type = WM_CREATE; m.a = id; m.b = w; m.c = h;
    strncpy(m.data, title, 39);
    queue[nq++] = m;
    alive_pids[nalive++] = pid;
    return id;
}

static void run_script(void) {
    while (pos < nscript && !wait_left) {
        const char *c = script[pos++];
        if (!strcmp(c, "m")) { cur_mouse.x = atoi(script[pos]); cur_mouse.y = atoi(script[pos + 1]); pos += 2; }
        else if (!strcmp(c, "d")) cur_mouse.buttons = MOUSE_LEFT;
        else if (!strcmp(c, "u")) cur_mouse.buttons = 0;
        else if (!strcmp(c, "k")) pending_key = atoi(script[pos++]);
        else if (!strcmp(c, "w")) wait_left = atoi(script[pos++]);
        else if (!strcmp(c, "s")) snapshot(script[pos++]);
        else if (!strcmp(c, "win")) {
            fake_window(atoi(script[pos]), atoi(script[pos + 1]), atoi(script[pos + 2]), script[pos + 3]);
            pos += 4;
        } else { printf("bad command %s\n", c); exit(1); }
    }
}

int msg_recv(struct msg *m, int timeout_ms) {
    if (timeout_ms != 0) {                          /* один оборот главного цикла */
        if (wait_left) wait_left--;
        run_script();
        if (pos >= nscript && !wait_left) { quit = true; }
    }
    if (nq) {
        *m = queue[0];
        memmove(queue, queue + 1, sizeof(queue[0]) * --nq);
        return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: desktop-host WxH commands...\n"); return 1; }
    scr_w = atoi(argv[1]);
    char *x = strchr(argv[1], 'x');
    scr_h = x ? atoi(x + 1) : 768;
    screen = calloc(scr_w * scr_h, 4);
    font_set_dir("fonts");
    script = argv + 2;
    nscript = argc - 2;
    return desktop_main();
}
