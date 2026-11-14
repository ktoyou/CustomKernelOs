/* desktop — рабочий стол MyOS в стиле Ubuntu (GNOME + тема Yaru).
 *
 * Сверху — панель: «Activities», дата и часы, значки сети/звука/питания с
 * системным меню. Слева — док с любимыми программами (точки слева от значка —
 * у программы есть окна). Внизу дока — кнопка «Show Applications»: сетка всех
 * программ поверх размытых обоев.
 *
 * Это обычная программа (сервер окон). Она забирает экран у консоли и рисует
 * всё сама: обои, тени и заголовки окон, панель, док, курсор. Программы с
 * окнами (win.h) — отдельные процессы: их пиксели лежат в общей памяти, а о
 * перерисовке они сообщают сообщением. Кадр собираем только в изменившихся
 * прямоугольниках («грязных»), с обрезкой по ним, и переносим на экран.
 *
 * Вся графика сглаженная: контуры и шрифты Ubuntu (gfx.h: vec.c и font.c).
 * Значки нарисованы кодом — векторно, поэтому любого размера.
 *
 * При перетаскивании используем аппаратное копирование, если оно доступно.
 * На framebuffer сдвигаем пиксели в RAM и записываем результат на экран;
 * заново рисуем только открывшиеся куски, скругления и тень. Разрешение может смениться в любой момент — тогда пересоздаём
 * обои и раскладку.
 *
 * Окна, которые это разрешили (win_set_resizable), можно растягивать за края
 * и разворачивать (кнопка, двойной щелчок по заголовку, перетащить к верхнему
 * краю). Буфер окна при этом новый: стол просит программу (WM_RESIZE), та
 * заводит общую память нужного размера и присылает её (WM_SETBUF). Пока ответа
 * нет, рамку рисуем уже нового размера, а недостающее — цветом фона окна.
 *
 * В обзоре (Activities) можно сразу печатать — это поиск программ, в том
 * числе по-русски. Справа в панели — раскладка клавиатуры (en/ru). */
#include "win.h"
#include "img.h"

static int iabs(int v) { return v < 0 ? -v : v; }

#define TOPBAR_H  32
#define HEAD_H    40                    /* заголовок окна */
#define RADIUS    10                    /* скругление верхних углов окна */
#define SH_BLUR   18                    /* тень вокруг окна */
#define SH_DY     5
#define SH_PAD    (SH_BLUR + SH_DY + 2) /* сколько тень занимает за краем окна */
#define MAXWIN    32
#define BTN_R     12                    /* круглые кнопки в заголовке */

static int SW, SH;                      /* размер экрана */
static color_t *wallpaper;              /* обои, нарисованные один раз */
static color_t *back;                   /* сюда собираем кадр */
static bool gpu_scene, gpu_wallpaper_ready;
static bool screen_copy_accelerated;
static bool hwcur;                      /* курсор рисует видеокарта */
static int drag = -1;                   /* окно, которое тащим */
static int capture = -1;                /* окно, где нажали кнопку */
static int resizing = -1, rs_edges, rs_mx, rs_my, rs_x, rs_y, rs_w, rs_h;   /* тянем край окна */
static int mouse_x, mouse_y;

/* ------------------------------------------------------------ программы */

#define MAX_APPS 64
#define APP_NAME 48
#define APP_PROG 32
#define APP_PATH 64
#define APP_KEYWORDS 128
enum { APP_GENERIC, APP_BROWSER, APP_FILES, APP_TERMINAL, APP_NOTEPAD, APP_CALC, APP_PAINT, APP_MINES, APP_BATTLE, APP_TASKMGR, APP_DISPLAY, APP_CODE, APP_ABOUT };
struct app { char name[APP_NAME], prog[APP_PROG], path[APP_PATH], keywords[APP_KEYWORDS]; int icon; bool fav; };
static struct app apps[MAX_APPS];
static int N_APPS;

static int app_icon_kind(const char *s) {
    if (!strcmp(s, "browser")) return APP_BROWSER;
    if (!strcmp(s, "files")) return APP_FILES;
    if (!strcmp(s, "terminal")) return APP_TERMINAL;
    if (!strcmp(s, "notepad")) return APP_NOTEPAD;
    if (!strcmp(s, "calc")) return APP_CALC;
    if (!strcmp(s, "paint")) return APP_PAINT;
    if (!strcmp(s, "mines")) return APP_MINES;
    if (!strcmp(s, "battle")) return APP_BATTLE;
    if (!strcmp(s, "taskmgr")) return APP_TASKMGR;
    if (!strcmp(s, "display")) return APP_DISPLAY;
    if (!strcmp(s, "code")) return APP_CODE;
    if (!strcmp(s, "about")) return APP_ABOUT;
    return APP_GENERIC;
}

static void app_field(char *dst, int cap, const char *src) {
    int n = strlen(src); if (n >= cap) n = cap - 1;
    memcpy(dst, src, n); dst[n] = 0;
}
static void app_append(char *dst, int cap, const char *src) {
    int n = strlen(dst);
    if (n && n < cap - 1) dst[n++] = ' ';
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
}
static void draw_generic_icon(const char *title, int x, int y, int size);

/* Читаем Desktop Entry из стандартного каталога приложений. */
static void load_apps(void) {
    struct dirent_info ent;
    for (int i = 0; N_APPS < MAX_APPS && dirread("/usr/share/applications", i, &ent) > 0; i++) {
        int en = strlen(ent.name);
        if (ent.type != 1 || en < 8 || strcmp(ent.name + en - 8, ".desktop")) continue;
        char path[64], line[192];
        snprintf(path, sizeof(path), "/usr/share/applications/%s", ent.name);
        int fd = open(path, O_READ); if (fd < 0) continue;
        struct app a; memset(&a, 0, sizeof(a)); a.icon = APP_GENERIC;
        while (readln(fd, line, sizeof(line)) >= 0) {
            int ll = strlen(line);
            while (ll && (line[ll - 1] == '\r' || line[ll - 1] == ' ' || line[ll - 1] == '\t')) line[--ll] = 0;
            char *eq = strchr(line, '='); if (!eq) continue; *eq++ = 0;
            if (!strcmp(line, "Name")) app_field(a.name, sizeof(a.name), eq);
            else if (!strcmp(line, "Exec")) {
                while (*eq == ' ' || *eq == '\t') eq++;
                char *end = eq; while (*end && *end != ' ' && *end != '\t') end++;
                *end = 0;
                const char *base = eq;
                if (!strncmp(eq, "/bin/", 5)) base = eq + 5;
                else if (!strncmp(eq, "/ext/bin/", 9)) base = eq + 9;
                else if (strchr(eq, '/')) continue;
                if (!*base || strchr(base, '/') || strstr(base, "..")) continue;
                app_field(a.prog, sizeof(a.prog), base);
                if (eq == base) snprintf(a.path, sizeof(a.path), "/bin/%s", base);
                else app_field(a.path, sizeof(a.path), eq);
            } else if (!strcmp(line, "Icon")) a.icon = app_icon_kind(eq);
            else if (!strcmp(line, "Keywords") || !strcmp(line, "Comment")) app_append(a.keywords, sizeof(a.keywords), eq);
            else if (!strcmp(line, "X-MyOS-Favorite") && (!strcmp(eq, "true") || !strcmp(eq, "1"))) a.fav = true;
        }
        close(fd);
        if (a.name[0] && a.prog[0] && a.path[0]) apps[N_APPS++] = a;
    }
}

static struct { int pid, app; } launched[32];
static int n_launched;

/* ------------------------------------------------------------ окна */

struct rect { int x, y, w, h; };

struct win {
    struct rect gpu_damage;
    bool gpu_dirty;
    bool used;
    int  owner;                         /* pid программы */
    int  shm;
    color_t *px;
    struct win_frame *frame;            /* buffered clients publish immutable snapshots */
    int  w, h;                          /* размер содержимого на экране (рамки) */
    int  bw, bh;                        /* размер буфера программы (отстаёт, пока она не ответит) */
    int  x, y;                          /* левый верхний угол заголовка */
    char title[40];
    bool minimized;
    int  app;                           /* номер в apps[] или -1 */
    bool resizable, maximized;
    int  min_w, min_h;
    int  rx, ry, rw, rh;                /* куда вернуть из развёрнутого */
    bool awaiting;                      /* попросили новый буфер, ждём WM_SETBUF */
    unsigned asked_at;
    bool motion, closing;
    struct rect visual, motion_from, motion_to;
    unsigned motion_started, motion_duration;
    int opacity, opacity_from, opacity_to;
};

static struct win wins[MAXWIN];
static int zorder[MAXWIN], nz;          /* снизу вверх */
static int focus = -1;
static int cascade;
static bool effects_enabled;
static unsigned motion_deadline;
static void remove_window_now(int idx);

static int outer_w(struct win *w) { return w->w; }
static int outer_h(struct win *w) { return w->h + HEAD_H; }

/* ------------------------------------------------------------ «грязные» прямоугольники */

#define MAXDIRTY 24
static struct rect dirty[MAXDIRTY];
static int ndirty;

static bool intersect(struct rect a, struct rect b, struct rect *out) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return false;
    if (out) { out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0; }
    return true;
}

static struct rect bounding(struct rect a, struct rect b) {
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

static struct rect mkrect(int x, int y, int w, int h) { struct rect r = { x, y, w, h }; return r; }

static void add_dirty(int x, int y, int w, int h) {
    struct rect screen = { 0, 0, SW, SH }, r = { x, y, w, h };
    if (!intersect(r, screen, &r)) return;
    for (int i = 0; i < ndirty; i++) {                  /* пересекается — объединяем */
        if (intersect(dirty[i], r, NULL)) {
            dirty[i] = bounding(dirty[i], r);
            return;
        }
    }
    if (ndirty == MAXDIRTY) {                           /* слишком много — один общий */
        for (int i = 1; i < ndirty; i++) dirty[0] = bounding(dirty[0], dirty[i]);
        ndirty = 1;
        dirty[0] = bounding(dirty[0], r);
        return;
    }
    dirty[ndirty++] = r;
}

static void add_dirty_r(struct rect r) { add_dirty(r.x, r.y, r.w, r.h); }
/* окно вместе с тенью */
static struct rect win_full(struct win *w) {
    if(w->motion) {
        int px=(SH_PAD*w->visual.w+w->w-1)/w->w;
        int py=(SH_PAD*w->visual.h+outer_h(w)-1)/outer_h(w);
        return mkrect(w->visual.x-px,w->visual.y-py,w->visual.w+2*px,w->visual.h+2*py);
    }
    return mkrect(w->x - SH_PAD, w->y - SH_PAD, outer_w(w) + 2 * SH_PAD, outer_h(w) + 2 * SH_PAD);
}
static void dirty_win(struct win *w) { add_dirty_r(win_full(w)); }
static struct rect window_rect(struct win *w) { return mkrect(w->x,w->y,w->w,outer_h(w)); }
static void motion_start(struct win *w,struct rect from,struct rect to,int a0,int a1,unsigned duration) {
    dirty_win(w);
    w->motion=true;w->visual=w->motion_from=from;w->motion_to=to;
    w->opacity=w->opacity_from=a0;w->opacity_to=a1;
    w->motion_started=ticks();w->motion_duration=duration;
    motion_deadline=w->motion_started;
    dirty_win(w);
}
static bool motions_active(void) {
    for(int i=0;i<MAXWIN;i++)if(wins[i].used && wins[i].motion)return true;
    return false;
}
static void motions_finish(void) {
    for(int i=0;i<MAXWIN;i++) {
        struct win *w=&wins[i];if(!w->used || !w->motion)continue;
        dirty_win(w);w->motion=false;
        if(w->closing)remove_window_now(i);
        else if(!w->minimized)dirty_win(w);
    }
}
static void motions_tick(void) {
    if(!gpu_scene) { motions_finish(); return; }
    unsigned now=ticks();
    if((int)(now-motion_deadline)<0)return;
    motion_deadline+=16;
    if((int)(now-motion_deadline)>32)motion_deadline=now+16;
    for(int i=0;i<MAXWIN;i++) {
        struct win *w=&wins[i];if(!w->used || !w->motion)continue;
        dirty_win(w);
        unsigned elapsed=now-w->motion_started;
        if(elapsed>=w->motion_duration) {
            w->motion=false;
            if(w->closing)remove_window_now(i);
            else if(!w->minimized)dirty_win(w);
            continue;
        }
        int t=1024-(int)(elapsed*1024/w->motion_duration);
        int eased=1024-t*t*t/(1024*1024);
        w->visual.x=w->motion_from.x+(w->motion_to.x-w->motion_from.x)*eased/1024;
        w->visual.y=w->motion_from.y+(w->motion_to.y-w->motion_from.y)*eased/1024;
        w->visual.w=w->motion_from.w+(w->motion_to.w-w->motion_from.w)*eased/1024;
        w->visual.h=w->motion_from.h+(w->motion_to.h-w->motion_from.h)*eased/1024;
        w->opacity=w->opacity_from+(w->opacity_to-w->opacity_from)*eased/1024;
        dirty_win(w);
    }
}
static int motion_wait(void) {
    /* Mouse state is polled after IPC waiting; mouse IRQs do not wake this
     * receive. Keep input latency within one PIT tick even without animation. */
    if(!motions_active())return 10;
    int left=(int)(motion_deadline-ticks());
    return left>10?10:left>0?left:0;
}
static void dirty_head(struct win *w) { add_dirty(w->x - 2, w->y - 2, outer_w(w) + 4, HEAD_H + 2); }
/* с запасом: у стрелки «горячая точка» в углу, у двойных стрелок — в центре */
static void dirty_cursor(int x, int y) { add_dirty(x - 14, y - 14, CURSOR_W + 16, CURSOR_H + 16); }

/* ------------------------------------------------------------ значки (векторные, сетка 48x48) */

static int IX, IY, IS;                  /* где и какого размера рисуем значок */

static void ic_path(gfx_path *p) { path_begin(p, IX, IY, IS, 48 * 16); }   /* единицы: 1/16 клетки сетки */
static int P16(int v) { return IX * 16 + v * IS / 48; }
static int Q16(int v) { return IY * 16 + v * IS / 48; }

static void rr(int x, int y, int w, int h, int r, color_t c) {
    gfx_path p; ic_path(&p);
    path_round_rect(&p, x * 16, y * 16, w * 16, h * 16, r * 16);
    path_fill(&p, c, 255); path_free(&p);
}

static void rr_grad(int x, int y, int w, int h, int r, color_t c0, color_t c1) {
    gfx_path p; ic_path(&p);
    path_round_rect(&p, x * 16, y * 16, w * 16, h * 16, r * 16);
    path_fill_grad(&p, c0, IY + y * IS / 48, c1, IY + (y + h) * IS / 48, 255); path_free(&p);
}

static void ell(int x, int y, int w, int h, color_t c, int a) {
    gfx_path p; ic_path(&p);
    path_ellipse(&p, x * 16, y * 16, w * 16, h * 16);
    path_fill(&p, c, a); path_free(&p);
}

static void ell_grad(int x, int y, int w, int h, color_t c0, color_t c1) {
    gfx_path p; ic_path(&p);
    path_ellipse(&p, x * 16, y * 16, w * 16, h * 16);
    path_fill_grad(&p, c0, IY + y * IS / 48, c1, IY + (y + h) * IS / 48, 255); path_free(&p);
}

/* линия толщиной t (всё в 1/2 клетки сетки — для точности) */
static void ln(int x0, int y0, int x1, int y1, int t, color_t c) {
    gfx_line16(P16(x0 * 8), Q16(y0 * 8), P16(x1 * 8), Q16(y1 * 8), t * 8 * IS / 48, c, 255);
    gfx_disc16(P16(x0 * 8), Q16(y0 * 8), t * 4 * IS / 48, c, 255);                  /* круглые концы */
    gfx_disc16(P16(x1 * 8), Q16(y1 * 8), t * 4 * IS / 48, c, 255);
}

static void tri(int x0, int y0, int x1, int y1, int x2, int y2, color_t c) {    /* в 1/2 клетки */
    gfx_path p; ic_path(&p);
    path_move(&p, x0 * 8, y0 * 8); path_line(&p, x1 * 8, y1 * 8); path_line(&p, x2 * 8, y2 * 8);
    path_close(&p); path_fill(&p, c, 255); path_free(&p);
}

static void draw_app_icon(int app, int x, int y, int size) {
    IX = x; IY = y; IS = size;
    switch (apps[app].icon) {
    case APP_TERMINAL:
        rr_grad(3, 5, 42, 38, 7, RGB(94, 94, 94), RGB(52, 52, 52));
        rr(6, 10, 36, 30, 4, RGB(48, 10, 36));
        ln(24, 36, 34, 44, 3, C_WHITE);
        ln(34, 44, 24, 52, 3, C_WHITE);
        ln(40, 53, 54, 53, 3, RGB(233, 84, 32));
        ell(9, 6, 3, 3, RGB(233, 84, 32), 255);
        ell(14, 6, 3, 3, RGB(170, 170, 170), 255);
        break;
    case APP_BROWSER:
        ell_grad(3, 3, 42, 42, RGB(240, 240, 240), RGB(180, 184, 190));
        ell_grad(6, 6, 36, 36, RGB(80, 170, 245), RGB(25, 85, 200));
        /* риски по сторонам света */
        ln(48, 14, 48, 18, 1, RGB(220, 235, 255));
        ln(48, 78, 48, 82, 1, RGB(220, 235, 255));
        ln(14, 48, 18, 48, 1, RGB(220, 235, 255));
        ln(78, 48, 82, 48, 1, RGB(220, 235, 255));
        tri(66, 30, 54, 54, 42, 42, RGB(236, 64, 52));                          /* стрелка компаса */
        tri(30, 66, 42, 42, 54, 54, RGB(250, 250, 250));
        ell(22, 22, 4, 4, RGB(60, 60, 60), 255);
        break;
    case APP_FILES:
        rr(4, 8, 18, 10, 3, RGB(186, 72, 30));
        rr(4, 11, 40, 31, 4, RGB(196, 80, 38));
        rr(7, 14, 34, 18, 2, RGB(250, 250, 250));
        rr_grad(4, 18, 40, 25, 4, RGB(250, 145, 88), RGB(229, 92, 38));
        ln(20, 70, 76, 70, 2, RGB(255, 196, 160));
        break;
    case APP_CODE:
    case APP_NOTEPAD:
        rr(8, 4, 32, 41, 4, RGB(170, 170, 170));
        rr(9, 5, 30, 39, 3, RGB(252, 252, 252));
        {
            gfx_path p; ic_path(&p);
            path_round_rect_ex(&p, 9 * 16, 5 * 16, 30 * 16, 7 * 16, 3 * 16, 3);
            path_fill(&p, RGB(233, 84, 32), 255); path_free(&p);
        }
        for (int i = 0; i < 5; i++) ln(28, 34 + i * 10, i == 4 ? 52 : 66, 34 + i * 10, 2, RGB(150, 150, 150));
        break;
    case APP_CALC:
        rr_grad(8, 3, 32, 42, 6, RGB(96, 96, 96), RGB(56, 56, 56));
        rr(12, 7, 24, 10, 2, RGB(222, 238, 222));
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                rr(12 + i * 8, 21 + j * 7, 7, 5, 1, i == 2 ? RGB(233, 84, 32) : RGB(205, 205, 205));
        break;
    case APP_PAINT:
        ell_grad(3, 7, 42, 34, RGB(247, 228, 190), RGB(220, 192, 140));
        ell(10, 13, 8, 8, RGB(226, 50, 50), 255);
        ell(20, 10, 8, 8, RGB(250, 205, 40), 255);
        ell(30, 13, 8, 8, RGB(70, 180, 70), 255);
        ell(33, 23, 8, 8, RGB(50, 110, 220), 255);
        ell(10, 24, 8, 8, RGB(150, 70, 190), 255);
        ln(50, 84, 84, 40, 4, RGB(120, 72, 36));
        ln(84, 40, 90, 32, 5, RGB(60, 60, 60));
        break;
    case APP_MINES:
        rr_grad(4, 4, 40, 40, 7, RGB(225, 225, 225), RGB(180, 180, 180));
        ln(48, 18, 48, 78, 3, RGB(30, 30, 30));
        ln(18, 48, 78, 48, 3, RGB(30, 30, 30));
        ln(27, 27, 69, 69, 3, RGB(30, 30, 30));
        ln(69, 27, 27, 69, 3, RGB(30, 30, 30));
        ell(14, 14, 20, 20, RGB(30, 30, 30), 255);
        ell(18, 18, 6, 6, C_WHITE, 220);
        break;
    case APP_BATTLE:
        rr_grad(4, 4, 40, 40, 7, RGB(80, 160, 230), RGB(25, 85, 170));
        tri(18, 56, 78, 56, 70, 70, RGB(70, 70, 76));                           /* корпус */
        tri(18, 56, 70, 70, 26, 70, RGB(70, 70, 76));
        rr(17, 21, 12, 7, 1, RGB(110, 110, 118));                              /* рубка */
        rr(22, 16, 3, 6, 1, RGB(110, 110, 118));
        ell(29, 9, 10, 10, RGB(250, 205, 40), 255);                            /* взрыв */
        ell(31, 11, 6, 6, RGB(239, 41, 41), 255);
        ln(10, 78, 86, 78, 2, RGB(190, 220, 250));                             /* волна */
        break;
    case APP_TASKMGR:
        rr_grad(4, 6, 40, 36, 6, RGB(70, 70, 70), RGB(38, 38, 38));
        for (int i = 1; i < 4; i++) ln(14, 14 + i * 16, 82, 14 + i * 16, 1, RGB(85, 85, 85));
        ln(14, 64, 26, 52, 3, RGB(233, 84, 32));
        ln(26, 52, 38, 60, 3, RGB(233, 84, 32));
        ln(38, 60, 50, 32, 3, RGB(233, 84, 32));
        ln(50, 32, 62, 46, 3, RGB(233, 84, 32));
        ln(62, 46, 74, 24, 3, RGB(233, 84, 32));
        ln(74, 24, 82, 30, 3, RGB(233, 84, 32));
        break;
    case APP_DISPLAY:
        rr(4, 7, 40, 29, 4, RGB(60, 60, 64));
        rr_grad(7, 10, 34, 23, 2, RGB(120, 180, 240), RGB(60, 95, 190));
        tri(40, 72, 56, 72, 60, 82, RGB(150, 150, 150));
        tri(40, 72, 60, 82, 36, 82, RGB(150, 150, 150));
        rr(14, 40, 20, 4, 2, RGB(120, 120, 120));
        break;
    case APP_ABOUT:
        ell_grad(3, 3, 42, 42, RGB(90, 160, 245), RGB(30, 90, 200));
        ell(21, 10, 6, 6, C_WHITE, 255);
        rr(21, 19, 6, 19, 3, C_WHITE);
        break;
    default: draw_generic_icon(apps[app].name, x, y, size); break;
    }
}

/* значок окна неизвестной программы: серый квадрат с первой буквой */
static void draw_generic_icon(const char *title, int x, int y, int size) {
    IX = x; IY = y; IS = size;
    rr_grad(4, 4, 40, 40, 8, RGB(140, 140, 150), RGB(90, 90, 100));
    char s[8];
    const char *e = title;
    if (*e) utf8_next(&e);
    int n = (int)(e - title);
    if (!n) { s[0] = '?'; n = 1; } else memcpy(s, title, n);
    s[n] = 0;
    int fs = size / 2;
    text_draw(FONT_UI_BOLD, fs, x + (size - text_width(FONT_UI_BOLD, fs, s)) / 2,
              y + (size - text_line_h(FONT_UI_BOLD, fs)) / 2, s, C_WHITE);
}

/* значки, готовые картинки 0xAARRGGBB (рисуются один раз на размер) */
static uint32_t *icon_cache[MAX_APPS][2];
static int icon_cache_size[2];

static uint32_t *app_icon(int app, int which, int size) {
    if (icon_cache[app][which] && icon_cache_size[which] == size) return icon_cache[app][which];
    if (icon_cache_size[which] != size) {
        for (int i = 0; i < MAX_APPS; i++) { free(icon_cache[i][which]); icon_cache[i][which] = NULL; }
        icon_cache_size[which] = size;
    }
    struct gfx_canvas cv;
    for (int pass = 0; pass < 2; pass++) {
        gfx_canvas_begin(&cv, size, size, pass);
        draw_app_icon(app, 0, 0, size);
    }
    icon_cache[app][which] = gfx_canvas_end(&cv);
    return icon_cache[app][which];
}

/* ------------------------------------------------------------ обои */

static color_t lerp3(int t) {                                      /* t 0..1023: баклажан -> пурпур -> оранжевый */
    static const color_t stops[4] = { RGB(44, 0, 30), RGB(94, 22, 80), RGB(170, 40, 70), RGB(233, 84, 32) };
    if (t < 0) t = 0;
    if (t > 1023) t = 1023;
    int seg = t * 3 / 1024, f = (t * 3 - seg * 1024) * 255 / 1023;
    return gfx_mix(stops[seg], stops[seg + 1], f);
}

static void custom_wallpaper(void) {
    char name[128];
    strcpy(name,"/usr/share/wallpapers/moon.jpg");
    int fd=open("/etc/wallpaper",O_READ);
    if(fd>=0) {
        char configured[128]; int n=read(fd,configured,sizeof(configured)-1); close(fd);
        if(n>0) { configured[n]=0; strcpy(name,configured); }
    }
    fd=open(name,O_READ); if(fd<0) return;
    int len=lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
    if(len<=0 || len>8*1024*1024) { close(fd); return; }
    unsigned char *data=malloc(len); img im;
    int ok=data && read(fd,data,len)==len && img_decode(data,len,&im,2048)==0;
    close(fd); free(data); if(!ok) return;
    /* Fill the screen, keeping the aspect ratio and cropping centrally. */
    int cw=im.w, ch=im.h;
    if(im.w*SH>im.h*SW) cw=im.h*SW/SH;
    else ch=im.w*SH/SW;
    if(cw<1) cw=1;
    if(ch<1) ch=1;
    for(int y=0;y<SH;y++) for(int x=0;x<SW;x++) {
        unsigned c=im.px[((im.h-ch)/2+y*ch/SH)*im.w+(im.w-cw)/2+x*cw/SW];
        wallpaper[y*SW+x]=gfx_mix(RGB(40,30,40),c,c>>24);
    }
    img_free(&im);
}

static void make_wallpaper(void) {
    gpu_wallpaper_ready = false;
    wallpaper = malloc(SW * SH * 4);
    color_t *save = gfx_buf;
    int sw = gfx_w, sh = gfx_h;
    gfx_buf = wallpaper; gfx_w = SW; gfx_h = SH;
    gfx_no_clip();
    int diag = SW + SH;
    for (int y = 0; y < SH; y++) {
        color_t *row = wallpaper + y * SW;
        for (int x = 0; x < SW; x++) {
            int t = (x + y * 2) * 1024 / (SW + SH * 2);           /* по диагонали */
            /* тёплое свечение справа снизу */
            int dx = (SW - x) * 1000 / diag, dy = (SH - y) * 1000 / diag;
            int glow = 420 - (dx * dx + dy * dy) / 600;
            if (glow > 0) t += glow / 2;
            /* лёгкий шум против полос в градиенте */
            unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            unsigned n = h >> 28;
            color_t c = lerp3(t);
            row[x] = gfx_mix(c, n & 1 ? C_WHITE : C_BLACK, n & 2 ? 2 : 0);
        }
    }
    /* мягкие полупрозрачные «ленты» */
    gfx_path p;
    for (int i = 0; i < 4; i++) {
        path_begin(&p, 0, 0, 1, 1);
        int y0 = SH * (30 + i * 14) / 100, y1 = SH * (62 + i * 10) / 100;
        path_move(&p, -10, y0);
        path_cubic(&p, SW / 3, y0 - SH / 4 + i * 20, SW * 2 / 3, y1 + SH / 6, SW + 10, y1 - SH / 5 + i * 30);
        path_line(&p, SW + 10, y1 - SH / 5 + i * 30 + SH / 9 + i * 8);
        path_cubic(&p, SW * 2 / 3, y1 + SH / 6 + SH / 10, SW / 3, y0 - SH / 4 + i * 20 + SH / 7, -10, y0 + SH / 12 + i * 6);
        path_close(&p);
        path_fill(&p, i & 1 ? RGB(255, 170, 120) : RGB(255, 255, 255), 14 + i * 3);
        path_free(&p);
    }
    /* подпись в углу */
    const char *s = "MyOS";
    text_draw(FONT_UI_BOLD, 30, SW - text_width(FONT_UI_BOLD, 30, s) - 34, SH - 76, s, RGB(255, 220, 200));
    custom_wallpaper();
    gfx_buf = save; gfx_w = sw; gfx_h = sh;
}

/* ------------------------------------------------------------ верхняя панель */

static char clock_text[40];
static int hover_top = -1;              /* 0 — Activities, 1 — часы, 2 — системные значки, 3 — раскладка */
static int kb_lang;                     /* раскладка клавиатуры: 0 en, 1 ru */
static bool sysmenu_open, overview_open;

static struct rect act_rect(void) { return mkrect(6, 4, text_width(FONT_UI_MEDIUM, 14, "Activities") + 24, TOPBAR_H - 8); }
static struct rect tray_rect(void) { return mkrect(SW - 108, 4, 100, TOPBAR_H - 8); }
static struct rect lang_rect(void) { return mkrect(SW - 108 - 46, 4, 42, TOPBAR_H - 8); }
static struct rect clock_rect(void) {
    int w = text_width(FONT_UI_MEDIUM, 14, clock_text) + 24;
    return mkrect((SW - w) / 2, 4, w, TOPBAR_H - 8);
}

static color_t TOP_BG = RGB(22, 22, 22);

/* мелкие белые значки панели (16x16) */
static void tray_icons(int x, int y) {
    color_t c = RGB(240, 240, 240);
    /* сеть (провод): три столбика */
    for (int i = 0; i < 3; i++) gfx_round_rect(x + 1 + i * 5, y + 11 - i * 4, 4, 5 + i * 4, 1, c);
    x += 26;
    /* звук: динамик и две дуги */
    gfx_path p;
    path_begin(&p, x, y, 1, 16);
    path_move(&p, 16, 96); path_line(&p, 64, 96); path_line(&p, 120, 40); path_line(&p, 120, 216);
    path_line(&p, 64, 160); path_line(&p, 16, 160); path_close(&p);
    path_fill(&p, c, 255); path_free(&p);
    gfx_ellipse(x + 4, y + 1, 14, 14, c, 255);
    gfx_ellipse(x + 6, y + 3, 10, 10, TOP_BG, 255);
    gfx_ellipse(x + 7, y + 4, 7, 7, c, 255);
    gfx_ellipse(x + 9, y + 6, 3, 3, TOP_BG, 255);
    gfx_fill_rect(x + 8, y, 3, 16, TOP_BG);
    gfx_fill_rect(x + 2, y + 6, 6, 5, c);
    path_begin(&p, x, y, 1, 16);
    path_move(&p, 16, 96); path_line(&p, 64, 96); path_line(&p, 120, 40); path_line(&p, 120, 216);
    path_line(&p, 64, 160); path_line(&p, 16, 160); path_close(&p);
    path_fill(&p, c, 255); path_free(&p);
    x += 28;
    /* питание: кольцо с разрывом и чёрточка */
    gfx_ellipse(x + 1, y + 1, 15, 15, c, 255);
    gfx_ellipse(x + 3, y + 3, 11, 11, TOP_BG, 255);
    gfx_fill_rect(x + 6, y, 5, 7, TOP_BG);
    gfx_round_rect(x + 7, y, 3, 8, 1, c);
    x += 24;
    /* стрелка меню */
    gfx_line16((x) * 16, (y + 6) * 16, (x + 4) * 16, (y + 10) * 16, 24, c, 255);
    gfx_line16((x + 4) * 16, (y + 10) * 16, (x + 8) * 16, (y + 6) * 16, 24, c, 255);
}

static void draw_topbar(void) {
    gfx_fill_rect(0, 0, SW, TOPBAR_H, TOP_BG);
    struct rect a = act_rect(), c = clock_rect(), t = tray_rect(), l = lang_rect();
    if (hover_top == 3) gfx_round_rect_a(l.x, l.y, l.w, l.h, l.h / 2, C_WHITE, 36);
    const char *ls = kb_lang ? "ru" : "en";
    text_draw(FONT_UI_MEDIUM, 14, l.x + (l.w - text_width(FONT_UI_MEDIUM, 14, ls)) / 2,
              (TOPBAR_H - text_line_h(FONT_UI_MEDIUM, 14)) / 2, ls, RGB(245, 245, 245));
    if (hover_top == 0 || overview_open) gfx_round_rect_a(a.x, a.y, a.w, a.h, a.h / 2, C_WHITE, overview_open ? 60 : 36);
    if (hover_top == 1) gfx_round_rect_a(c.x, c.y, c.w, c.h, c.h / 2, C_WHITE, 36);
    if (hover_top == 2 || sysmenu_open) gfx_round_rect_a(t.x, t.y, t.w, t.h, t.h / 2, C_WHITE, sysmenu_open ? 60 : 36);
    int ty = (TOPBAR_H - text_line_h(FONT_UI_MEDIUM, 14)) / 2;
    text_draw(FONT_UI_MEDIUM, 14, a.x + 12, ty, "Activities", RGB(245, 245, 245));
    text_draw(FONT_UI_MEDIUM, 14, c.x + 12, ty, clock_text, RGB(245, 245, 245));
    tray_icons(t.x + 12, 8);
}

/* ------------------------------------------------------------ док */

#define DOCK_MAX 24
struct dock_item { int app, win; };     /* программа (app >= 0) или окно без программы */
static struct dock_item dock[DOCK_MAX];
static int n_dock, hover_dock = -1;
static int DOCK_W = 72, DOCK_CELL = 56, DOCK_ICON = 44;

static int app_windows(int app) {
    int n = 0;
    for (int i = 0; i < MAXWIN; i++) if (wins[i].used && wins[i].app == app) n++;
    return n;
}

static void dock_build(void) {
    n_dock = 0;
    for (int a = 0; a < N_APPS && n_dock < DOCK_MAX; a++)
        if (apps[a].fav) { dock[n_dock].app = a; dock[n_dock].win = -1; n_dock++; }
    for (int a = 0; a < N_APPS && n_dock < DOCK_MAX; a++)
        if (!apps[a].fav && app_windows(a)) { dock[n_dock].app = a; dock[n_dock].win = -1; n_dock++; }
    for (int i = 0; i < MAXWIN && n_dock < DOCK_MAX; i++)
        if (wins[i].used && wins[i].app < 0) { dock[n_dock].app = -1; dock[n_dock].win = i; n_dock++; }
    /* размер клеток — чтобы всё влезло по высоте */
    int avail = SH - TOPBAR_H - 16;
    int cell = avail / (n_dock + 1);
    if (cell > 56) cell = 56;
    if (cell < 24) cell = 24;
    DOCK_CELL = cell;
    DOCK_ICON = cell * 44 / 56;
    DOCK_W = cell + 16;
}

static struct rect dock_rect(void) { return mkrect(0, TOPBAR_H, DOCK_W, SH - TOPBAR_H); }
static struct rect dock_cell(int i) {                    /* i == n_dock — кнопка «все программы» */
    if (i >= n_dock) return mkrect(8, SH - DOCK_CELL - 8, DOCK_CELL, DOCK_CELL);
    return mkrect(8, TOPBAR_H + 8 + i * DOCK_CELL, DOCK_CELL, DOCK_CELL);
}

static int dock_item_windows(int i) {
    if (dock[i].app < 0) return 1;
    return app_windows(dock[i].app);
}

static bool dock_item_focused(int i) {
    if (focus < 0 || wins[focus].minimized) return false;
    if (dock[i].app < 0) return dock[i].win == focus;
    return wins[focus].app == dock[i].app;
}

static void draw_dock(void) {
    struct rect d = dock_rect();
    gfx_fill_rect_a(d.x, d.y, d.w, d.h, RGB(22, 22, 22), 200);
    for (int i = 0; i <= n_dock; i++) {
        struct rect c = dock_cell(i);
        bool focused = i < n_dock && dock_item_focused(i);
        if (i == hover_dock || focused || (i == n_dock && overview_open))
            gfx_round_rect_a(c.x, c.y + 1, c.w, c.h - 2, 10, C_WHITE, focused ? 46 : 30);
        int ix = c.x + (c.w - DOCK_ICON) / 2, iy = c.y + (c.h - DOCK_ICON) / 2;
        if (i == n_dock) {                                   /* «все программы»: 3x3 точки */
            int st = DOCK_ICON / 4, r = DOCK_ICON * 2;        /* в 1/16 пикселя: радиус ~ 1/8 значка */
            for (int yy = 0; yy < 3; yy++)
                for (int xx = 0; xx < 3; xx++)
                    gfx_disc16((ix + DOCK_ICON / 2 + (xx - 1) * st) * 16, (iy + DOCK_ICON / 2 + (yy - 1) * st) * 16,
                               r / 8 + 16, RGB(240, 240, 240), 255);
            continue;
        }
        if (dock[i].app >= 0) gfx_draw_argb(ix, iy, app_icon(dock[i].app, 0, DOCK_ICON), DOCK_ICON, DOCK_ICON);
        else draw_generic_icon(wins[dock[i].win].title, ix, iy, DOCK_ICON);
        int n = dock_item_windows(i);                        /* точки: сколько окон */
        if (n > 3) n = 3;
        for (int k = 0; k < n; k++) {
            int cy = c.y + c.h / 2 + (k * 2 - (n - 1)) * 4;
            gfx_disc16(3 * 16, cy * 16, 40, RGB(233, 84, 32), 255);
        }
    }
}

static int dock_at(int x, int y) {
    if (!ui_inside(x, y, 0, TOPBAR_H, DOCK_W, SH - TOPBAR_H)) return -2;
    for (int i = 0; i <= n_dock; i++) {
        struct rect c = dock_cell(i);
        if (ui_inside(x, y, c.x, c.y, c.w, c.h)) return i;
    }
    return -1;
}

static void dock_changed(void) {
    dock_build();
    add_dirty_r(dock_rect());
}

/* подсказка рядом с доком */
static struct rect tip_rect(void) {
    if (hover_dock < 0 || drag >= 0) return mkrect(0, 0, 0, 0);
    const char *s = hover_dock == n_dock ? "Show Applications" :
                    dock[hover_dock].app >= 0 ? apps[dock[hover_dock].app].name : wins[dock[hover_dock].win].title;
    struct rect c = dock_cell(hover_dock);
    int w = text_width(FONT_UI, 14, s) + 24;
    return mkrect(DOCK_W + 6, c.y + (c.h - 30) / 2, w, 30);
}

static void draw_tip(void) {
    struct rect r = tip_rect();
    if (!r.w) return;
    const char *s = hover_dock == n_dock ? "Show Applications" :
                    dock[hover_dock].app >= 0 ? apps[dock[hover_dock].app].name : wins[dock[hover_dock].win].title;
    gfx_round_rect_a(r.x, r.y, r.w, r.h, 8, RGB(30, 30, 30), 235);
    text_draw(FONT_UI, 14, r.x + 12, r.y + (r.h - text_line_h(FONT_UI, 14)) / 2, s, RGB(245, 245, 245));
}

/* ------------------------------------------------------------ системное меню */

#define SM_W 260
static const char *sm_items[] = { "Settings", "About MyOS", "", "Log Out", "Close Desktop" };
#define SM_N 5
static int hover_sm = -1;

static struct rect sysmenu_rect(void) { return mkrect(SW - SM_W - 8, TOPBAR_H + 6, SM_W, 16 + 4 * 38 + 13); }
static struct rect sm_item(int i) {
    struct rect r = sysmenu_rect();
    int y = r.y + 8;
    for (int k = 0; k < i; k++) y += k == 2 ? 13 : 38;
    return mkrect(r.x + 8, y, r.w - 16, i == 2 ? 13 : 38);
}

static void draw_sysmenu(void) {
    struct rect r = sysmenu_rect();
    gfx_shadow(r.x, r.y, r.w, r.h, 14, 14, 3, 120, r.x, r.y, r.w, r.h);
    gfx_round_rect(r.x, r.y, r.w, r.h, 14, RGB(48, 48, 48));
    for (int i = 0; i < SM_N; i++) {
        struct rect it = sm_item(i);
        if (i == 2) { gfx_fill_rect(it.x + 6, it.y + 6, it.w - 12, 1, RGB(80, 80, 80)); continue; }
        if (i == hover_sm) gfx_round_rect_a(it.x, it.y, it.w, it.h, 8, C_WHITE, 26);
        int icon_x = it.x + 14, icon_y = it.y + it.h / 2;
        if (i == 0) {                                                       /* монитор */
            gfx_round_frame(icon_x, icon_y - 7, 16, 11, 2, RGB(235, 235, 235), RGB(48, 48, 48));
            gfx_fill_rect(icon_x + 5, icon_y + 5, 6, 2, RGB(235, 235, 235));
        } else if (i == 1) {                                                /* i в круге */
            gfx_ellipse(icon_x, icon_y - 8, 16, 16, RGB(235, 235, 235), 255);
            gfx_fill_rect(icon_x + 7, icon_y - 2, 2, 6, RGB(48, 48, 48));
            gfx_fill_rect(icon_x + 7, icon_y - 5, 2, 2, RGB(48, 48, 48));
        } else if (i == 3) {
            gfx_round_frame(icon_x, icon_y - 8, 11, 16, 2, C_WHITE, RGB(48, 48, 48));
            gfx_fill_rect(icon_x + 6, icon_y - 1, 15, 2, C_WHITE);
            gfx_line16((icon_x+16)*16, (icon_y-5)*16, (icon_x+21)*16, icon_y*16, 24, C_WHITE, 255);
            gfx_line16((icon_x+21)*16, icon_y*16, (icon_x+16)*16, (icon_y+5)*16, 24, C_WHITE, 255);
        } else {                                                            /* питание */
            gfx_ellipse(icon_x, icon_y - 7, 16, 16, RGB(235, 235, 235), 255);
            gfx_ellipse(icon_x + 2, icon_y - 5, 12, 12, RGB(48, 48, 48), 255);
            if (i == hover_sm) gfx_ellipse(icon_x + 2, icon_y - 5, 12, 12, gfx_mix(RGB(48, 48, 48), C_WHITE, 26), 255);
            gfx_fill_rect(icon_x + 5, icon_y - 9, 6, 7, i == hover_sm ? gfx_mix(RGB(48, 48, 48), C_WHITE, 26) : RGB(48, 48, 48));
            gfx_round_rect(icon_x + 7, icon_y - 9, 2, 8, 1, RGB(235, 235, 235));
        }
        text_draw(FONT_UI, 14, it.x + 42, it.y + (it.h - text_line_h(FONT_UI, 14)) / 2, sm_items[i], RGB(240, 240, 240));
    }
}

static int sysmenu_at(int x, int y) {
    struct rect r = sysmenu_rect();
    if (!ui_inside(x, y, r.x, r.y, r.w, r.h)) return -2;
    for (int i = 0; i < SM_N; i++) {
        struct rect it = sm_item(i);
        if (i != 2 && ui_inside(x, y, it.x, it.y, it.w, it.h)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------ обзор: все программы */

/* фон обзора: обои, уменьшенные в 8 раз (это и есть размытие) и затемнённые;
 * на экран растягиваем с плавной интерполяцией прямо при сборке кадра */
static color_t *overview_bg;
static int ov_sw, ov_sh;
static int hover_ov = -1;
#define OV_CELL_W 136
#define OV_CELL_H 132
#define OV_ICON   72

/* поиск: что напечатано и какие программы подходят */
static char search[64];
static int  search_len;
static int  ov_list[MAX_APPS], ov_n, ov_sel;
static bool ov_kbd;                     /* выбирали стрелками — показать выбранное */

static int lower_cp(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;                /* А-Я */
    if (c == 0x401 || c == 0x451) return 0x435;                   /* Ё, ё -> е */
    return c;
}

static int decode_lower(const char *s, int *out, int max) {
    int n = 0;
    while (*s && n < max) out[n++] = lower_cp(utf8_next(&s));
    return n;
}

static bool contains(const char *hay, const int *nd, int nn) {
    int h[160];
    int hn = decode_lower(hay, h, 160);
    for (int i = 0; i + nn <= hn; i++) {
        if (i > 0 && h[i - 1] != ' ') continue;                   /* только с начала слова */
        int k = 0;
        while (k < nn && h[i + k] == nd[k]) k++;
        if (k == nn) return true;
    }
    return false;
}

static void ov_filter(void) {
    int nd[64];
    int nn = decode_lower(search, nd, 64);
    ov_n = 0;
    for (int a = 0; a < N_APPS; a++)
        if (!nn || contains(apps[a].name, nd, nn) || contains(apps[a].prog, nd, nn) || contains(apps[a].keywords, nd, nn))
            ov_list[ov_n++] = a;
    ov_sel = 0;
}

#define OV_TOP 96                       /* сверху — поле поиска */
static struct rect search_rect(void) {
    int w = 380;
    return mkrect(DOCK_W + (SW - DOCK_W - w) / 2, TOPBAR_H + 26, w, 40);
}

static int ov_cols(void) {
    int c = (SW - DOCK_W - 80) / OV_CELL_W;
    if (c > 6) c = 6;
    if (c < 1) c = 1;
    return c;
}

static struct rect ov_cell(int i) {                               /* i — номер среди найденных */
    int cols = ov_cols(), rows = (ov_n + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int gw = cols * OV_CELL_W, gh = rows * OV_CELL_H;
    int top = TOPBAR_H + OV_TOP;
    int x0 = DOCK_W + (SW - DOCK_W - gw) / 2, y0 = top + (SH - top - gh) / 3;
    if (y0 < top) y0 = top;
    return mkrect(x0 + (i % cols) * OV_CELL_W, y0 + (i / cols) * OV_CELL_H, OV_CELL_W, OV_CELL_H);
}

static void make_overview_bg(void) {
    free(overview_bg);
    ov_sw = (SW + 7) / 8; ov_sh = (SH + 7) / 8;
    overview_bg = malloc(ov_sw * ov_sh * 4);
    if (!overview_bg) return;
    for (int by = 0; by < ov_sh; by++)
        for (int bx = 0; bx < ov_sw; bx++) {
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int y = by * 8; y < by * 8 + 8 && y < SH; y += 2)
                for (int x = bx * 8; x < bx * 8 + 8 && x < SW; x += 2) {
                    color_t c = wallpaper[y * SW + x];
                    r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255; n++;
                }
            overview_bg[by * ov_sw + bx] = RGB(r / n * 55 / 100, g / n * 55 / 100, b / n * 55 / 100);
        }
}

static void draw_overview(struct rect r) {
    for (int y = r.y; y < r.y + r.h; y++) {
        int fy = y * 32 - 128;                            /* y / 8 - 0.5 в 1/256 */
        if (fy < 0) fy = 0;
        int y0 = fy >> 8, wy = fy & 255, y1 = y0 + 1 < ov_sh ? y0 + 1 : y0;
        const color_t *r0 = overview_bg + y0 * ov_sw, *r1 = overview_bg + y1 * ov_sw;
        color_t *out = back + y * SW;
        for (int x = r.x; x < r.x + r.w; x++) {
            int fxx = x * 32 - 128;
            if (fxx < 0) fxx = 0;
            int x0 = fxx >> 8, wx = fxx & 255, x1 = x0 + 1 < ov_sw ? x0 + 1 : x0;
            out[x] = gfx_mix(gfx_mix(r0[x0], r0[x1], wx), gfx_mix(r1[x0], r1[x1], wx), wy);
        }
    }
    /* поле поиска */
    struct rect sr = search_rect();
    if (intersect(r, sr, NULL)) {
        gfx_round_rect_a(sr.x, sr.y, sr.w, sr.h, sr.h / 2, C_WHITE, search_len ? 44 : 30);
        int mx = sr.x + 22, my = sr.y + sr.h / 2;                 /* лупа */
        for (int k = 0; k < 24; k++) {                            /* кольцо отрезками */
            static const short cs[25] = { 256, 247, 222, 181, 128, 66, 0, -66, -128, -181, -222, -247,
                                          -256, -247, -222, -181, -128, -66, 0, 66, 128, 181, 222, 247, 256 };
            int k2 = (k + 1) % 24, c0 = cs[k], s0 = cs[(k + 18) % 24], c1 = cs[k2], s1 = cs[(k2 + 18) % 24];
            gfx_line16((mx - 1) * 16 + c0 * 5 * 16 / 256, (my - 2) * 16 + s0 * 5 * 16 / 256,
                       (mx - 1) * 16 + c1 * 5 * 16 / 256, (my - 2) * 16 + s1 * 5 * 16 / 256, 30, RGB(235, 235, 235), 255);
        }
        gfx_line16((mx + 3) * 16, (my + 2) * 16, (mx + 8) * 16, (my + 7) * 16, 36, RGB(235, 235, 235), 255);
        int tx = sr.x + 42, ty = sr.y + (sr.h - text_line_h(FONT_UI, 15)) / 2;
        if (search_len) {
            int tw = text_draw_fit(FONT_UI, 15, tx, ty, search, sr.w - 62, RGB(250, 250, 250));
            if (tw > sr.w - 62) tw = sr.w - 62;
            gfx_fill_rect(tx + tw + 1, sr.y + 10, 2, sr.h - 20, RGB(250, 250, 250));      /* каретка */
        } else {
            text_draw(FONT_UI, 15, tx, ty, "Type to search", RGB(190, 190, 190));
        }
    }
    if (!ov_n) {
        const char *s = "No results";
        struct rect c = ov_cell(0);
        text_draw(FONT_UI_MEDIUM, 18, DOCK_W + (SW - DOCK_W - text_width(FONT_UI_MEDIUM, 18, s)) / 2, c.y + 40, s,
                  RGB(220, 220, 220));
    }
    for (int i = 0; i < ov_n; i++) {
        struct rect c = ov_cell(i);
        if (!intersect(r, c, NULL)) continue;
        int app = ov_list[i];
        bool sel = (search_len || ov_kbd) && i == ov_sel;
        if (i == hover_ov || sel) gfx_round_rect_a(c.x + 6, c.y + 6, c.w - 12, c.h - 12, 16, C_WHITE, sel ? 52 : 34);
        int ix = c.x + (c.w - OV_ICON) / 2, iy = c.y + 14;
        gfx_draw_argb(ix, iy, app_icon(app, 1, OV_ICON), OV_ICON, OV_ICON);
        const char *s = apps[app].name;
        int tw = text_width(FONT_UI, 14, s);
        text_draw(FONT_UI, 14, c.x + (c.w - tw) / 2, iy + OV_ICON + 12, s, RGB(245, 245, 245));
    }
}

static int overview_at(int x, int y) {                            /* -> номер среди найденных */
    for (int i = 0; i < ov_n; i++) {
        struct rect c = ov_cell(i);
        if (ui_inside(x, y, c.x + 6, c.y + 6, c.w - 12, c.h - 12)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------ рамки окон */

static int hover_close = -1, hover_min = -1, hover_max = -1;

/* кнопки справа: закрыть, развернуть (только у растягиваемых), свернуть */
static void btn_centers(struct win *w, int *close_x, int *max_x, int *min_x, int *cy) {
    int right = w->x + outer_w(w);
    *close_x = right - 8 - BTN_R;
    if (w->resizable) { *max_x = *close_x - 2 * BTN_R - 10; *min_x = *max_x - 2 * BTN_R - 10; }
    else { *max_x = -10000; *min_x = *close_x - 2 * BTN_R - 10; }
    *cy = w->y + HEAD_H / 2;
}

/* 0 — мимо, 1 — закрыть, 2 — развернуть, 3 — свернуть */
static int head_button(struct win *w, int x, int y) {
    int cxc, cxx, cxm, cy;
    btn_centers(w, &cxc, &cxx, &cxm, &cy);
    int r2 = (BTN_R + 2) * (BTN_R + 2), dy = y - cy;
    if ((x - cxc) * (x - cxc) + dy * dy <= r2) return 1;
    if ((x - cxx) * (x - cxx) + dy * dy <= r2) return 2;
    if ((x - cxm) * (x - cxm) + dy * dy <= r2) return 3;
    return 0;
}

static int win_radius(struct win *w) { return w->maximized ? 0 : RADIUS; }

static void draw_frame(int idx) {
    struct win *w = &wins[idx];
    bool active = idx == focus;
    bool code_theme = w->app >= 0 && apps[w->app].icon == APP_CODE;
    int ow = outer_w(w), oh = outer_h(w), rad = win_radius(w);
    /* тень и тонкая обводка (у развёрнутого — не нужны: вокруг панель и док) */
    if (!w->maximized) {
        gfx_shadow(w->x, w->y, ow, oh, RADIUS, active ? SH_BLUR : SH_BLUR - 6, active ? SH_DY : 2,
                   active ? 120 : 70, w->x, w->y, ow, oh);
        gfx_round_rect_ex(w->x - 1, w->y - 1, ow + 2, RADIUS + 2, RADIUS + 1, 3, C_BLACK, 40);
        gfx_fill_rect_a(w->x - 1, w->y + RADIUS + 1, 1, oh - RADIUS, C_BLACK, 40);
        gfx_fill_rect_a(w->x + ow, w->y + RADIUS + 1, 1, oh - RADIUS, C_BLACK, 40);
        gfx_fill_rect_a(w->x - 1, w->y + oh, ow + 2, 1, C_BLACK, 40);
    }
    /* заголовок */
    color_t hb = code_theme ? RGB(24,24,24) : active ? RGB(235, 235, 235) : RGB(250, 250, 250);
    if (rad) gfx_round_rect_ex(w->x, w->y, ow, HEAD_H, rad, 3, hb, 255);
    else gfx_fill_rect(w->x, w->y, ow, HEAD_H, hb);
    gfx_fill_rect(w->x, w->y + HEAD_H - 1, ow, 1, code_theme ? RGB(43,43,43) : active ? RGB(214, 214, 214) : RGB(228, 228, 228));
    int cxc, cxx, cxm, cy;
    btn_centers(w, &cxc, &cxx, &cxm, &cy);
    color_t ic = code_theme ? RGB(205,205,205) : active ? RGB(46, 52, 54) : RGB(150, 150, 150);
    /* закрыть */
    bool hc = hover_close == idx;
    gfx_disc16(cxc * 16, cy * 16, BTN_R * 16, hc ? RGB(196,43,28) : code_theme ? RGB(24,24,24) : active ? RGB(214, 214, 214) : RGB(236, 236, 236), 255);
    color_t xc = hc ? C_WHITE : ic;
    gfx_line16((cxc - 4) * 16, (cy - 4) * 16, (cxc + 4) * 16, (cy + 4) * 16, 26, xc, 255);
    gfx_line16((cxc + 4) * 16, (cy - 4) * 16, (cxc - 4) * 16, (cy + 4) * 16, 26, xc, 255);
    /* развернуть: квадрат; у развёрнутого — два квадрата («вернуть») */
    if (w->resizable) {
        if (hover_max == idx) gfx_disc16(cxx * 16, cy * 16, BTN_R * 16, code_theme ? RGB(60,60,60) : active ? RGB(214, 214, 214) : RGB(236, 236, 236), 255);
        int q = w->maximized ? 3 : 4, ox = w->maximized ? -1 : 0, oy = w->maximized ? 1 : 0;
        int x0 = (cxx - q + ox) * 16, y0 = (cy - q + oy) * 16, x1 = (cxx + q + ox) * 16, y1 = (cy + q + oy) * 16;
        gfx_line16(x0, y0, x1, y0, 24, ic, 255); gfx_line16(x1, y0, x1, y1, 24, ic, 255);
        gfx_line16(x1, y1, x0, y1, 24, ic, 255); gfx_line16(x0, y1, x0, y0, 24, ic, 255);
        if (w->maximized) {                                       /* задний квадрат: только видимые края */
            gfx_line16(x0 + 48, y0 - 48, x1 + 48, y0 - 48, 24, ic, 255);
            gfx_line16(x1 + 48, y0 - 48, x1 + 48, y1 - 48, 24, ic, 255);
        }
    }
    /* свернуть */
    if (hover_min == idx) gfx_disc16(cxm * 16, cy * 16, BTN_R * 16, code_theme ? RGB(60,60,60) : active ? RGB(214, 214, 214) : RGB(236, 236, 236), 255);
    gfx_line16((cxm - 5) * 16, (cy + 3) * 16 + 8, (cxm + 5) * 16, (cy + 3) * 16 + 8, 26, ic, 255);
    /* название — по центру, жирным */
    int pad = w->x + ow - (cxm - BTN_R) + 8;                     /* справа заняты кнопки — столько же слева */
    int avail = ow - 2 * pad;
    if (avail > 20) {
        int tw = text_width(FONT_UI_BOLD, 14, w->title);
        if (tw > avail) tw = avail;
        text_draw_fit(FONT_UI_BOLD, 14, w->x + (ow - tw) / 2, w->y + (HEAD_H - text_line_h(FONT_UI_BOLD, 14)) / 2,
                      w->title, avail, code_theme ? RGB(205,205,205) : active ? RGB(46, 52, 54) : RGB(140, 140, 140));
    }
}

/* ------------------------------------------------------------ сборка кадра */

static void compose(struct rect r) {
    gfx_set_clip(r.x, r.y, r.w, r.h);
    if (overview_open && overview_bg) {
        draw_overview(r);
    } else {
        for (int y = r.y; y < r.y + r.h; y++)                     /* обои */
            memcpy(back + y * SW + r.x, wallpaper + y * SW + r.x, r.w * 4);
        for (int k = 0; k < nz; k++) {                            /* окна снизу вверх */
            struct win *w = &wins[zorder[k]];
            if (w->minimized) continue;
            if (!intersect(r, win_full(w), NULL)) continue;
            draw_frame(zorder[k]);
            struct rect cr = { w->x, w->y + HEAD_H, w->w, w->h }, part;
            if (intersect(r, cr, &part)) {                        /* содержимое — только нужный кусок */
                color_t fill = w->px[w->bw * w->bh - 1];          /* буфер ещё старого размера — добить фоном */
                for (int y = part.y; y < part.y + part.h; y++) {
                    int ly = y - cr.y, lx = part.x - cr.x, n = 0;
                    color_t *d = back + y * SW + part.x;
                    if (ly < w->bh && lx < w->bw) {
                        n = w->bw - lx < part.w ? w->bw - lx : part.w;
                        memcpy(d, w->px + ly * w->bw + lx, n * 4);
                    }
                    for (; n < part.w; n++) d[n] = fill;
                }
            }
        }
    }
    if (intersect(r, dock_rect(), NULL)) draw_dock();
    if (r.y < TOPBAR_H) draw_topbar();
    if (sysmenu_open && intersect(r, mkrect(SW - SM_W - 40, TOPBAR_H, SM_W + 40, sysmenu_rect().h + 30), NULL)) draw_sysmenu();
    if (intersect(r, tip_rect(), NULL)) draw_tip();
    gfx_no_clip();
    struct rect cur = { mouse_x - 14, mouse_y - 14, CURSOR_W + 16, CURSOR_H + 16 };
    if (!hwcur && intersect(r, cur, NULL)) {
        gfx_set_clip(r.x, r.y, r.w, r.h);
        gfx_cursor_draw(mouse_x, mouse_y);
        gfx_no_clip();
    }
}

#include "desktop_gpu.inc"

static void flush(void) {
    gfx_frame_begin();
    for (int i = 0; i < ndirty; i++) {
        bool scene_ok = gpu_scene && !overview_open;
        /* During resize the app may still own its previous, smaller buffer. */
        for (int k = 0; k < nz && scene_ok; k++) {
            struct win *w = &wins[zorder[k]];
            if (!w->minimized && !w->motion && (w->bw < w->w || w->bh < w->h)) scene_ok = false;
        }
        if (scene_ok && gpu_compose(dirty[i])) continue;
        if (scene_ok) {
            /* Rebuild a complete, coherent CPU frame if a GPU resource upload
             * cannot be satisfied. Never show a half-composited scene. */
            gpu_scene = false;
            dprintf(STDERR, "desktop: GPU surface failed; software composition fallback\n");
            compose(mkrect(0, 0, SW, SH));
            gfx_flip();
            break;
        }
        compose(dirty[i]);
        gfx_flip_rect(dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h);
    }
    ndirty = 0;
    gfx_frame_end();
}

/* ------------------------------------------------------------ сообщения программам */

static void send(int pid, int type, int a, int b, int c, int d) {
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = type; m.a = a; m.b = b; m.c = c; m.d = d;
    msg_send(pid, &m);
}

static void set_focus(int idx) {
    if (idx == focus) return;
    if (focus >= 0 && wins[focus].used) {
        send(wins[focus].owner, WM_FOCUS, focus, 0, 0, 0);
        dirty_win(&wins[focus]);                                  /* тень другая */
    }
    focus = idx;
    if (idx >= 0) {
        send(wins[idx].owner, WM_FOCUS, idx, 1, 0, 0);
        dirty_win(&wins[idx]);
    }
    add_dirty_r(dock_rect());
}

static void raise(int idx) {
    int k = 0;
    while (k < nz && zorder[k] != idx) k++;
    if (k == nz) return;
    if (k != nz - 1) {
        for (; k < nz - 1; k++) zorder[k] = zorder[k + 1];
        zorder[nz - 1] = idx;
        dirty_win(&wins[idx]);
    }
    set_focus(idx);
}

/* самое верхнее видимое окно (или -1) */
static int top_visible(void) {
    for (int k = nz - 1; k >= 0; k--) if (!wins[zorder[k]].minimized && !wins[zorder[k]].closing) return zorder[k];
    return -1;
}

static void remove_window_now(int idx) {
    struct win *w = &wins[idx];
    if (!w->used) return;
    if (!w->minimized) dirty_win(w);
    if (gpu_scene) { gfx_surface_upload(idx * 2, NULL, 0, 0, false); gfx_surface_upload(idx * 2 + 1, NULL, 0, 0, false); }
    if (w->frame) { shm_unmap(w->frame); free(w->px); }
    else shm_unmap(w->px);
    w->used = false;
    int k = 0, j = 0;
    for (; k < nz; k++) if (zorder[k] != idx) zorder[j++] = zorder[k];
    nz = j;
    if (hover_close == idx) hover_close = -1;
    if (hover_min == idx) hover_min = -1;
    if (hover_max == idx) hover_max = -1;
    if (drag == idx) drag = -1;
    if (resizing == idx) resizing = -1;
    if (capture == idx) capture = -1;
    if (focus == idx) { focus = -1; set_focus(top_visible()); }
    hover_dock = -1;
    dock_changed();
}

static void remove_window(int idx) {
    struct win *w=&wins[idx];
    if(!w->used || w->closing)return;
    if(effects_enabled && gpu_scene && !overview_open && !w->minimized) {
        struct rect from=w->motion?w->visual:window_rect(w), to=window_rect(w);
        to.x+=to.w/40;to.y+=to.h/40;to.w-=to.w/20;to.h-=to.h/20;
        int opacity=w->motion?w->opacity:255;
        w->closing=true;motion_start(w,from,to,opacity,0,120);
        if(focus==idx) { focus=-1;set_focus(top_visible()); }
        return;
    }
    remove_window_now(idx);
}

static struct rect minimize_target(int idx) {
    int cell=n_dock;
    for(int i=0;i<n_dock;i++)if(dock[i].win==idx ||
        (wins[idx].app>=0 && dock[i].app==wins[idx].app)) { cell=i;break; }
    return dock_cell(cell);
}

static void minimize(int idx) {
    struct win *w=&wins[idx];
    struct rect from=w->motion?w->visual:window_rect(w);
    if(effects_enabled && gpu_scene && !overview_open)
        motion_start(w,from,minimize_target(idx),w->motion?w->opacity:255,0,180);
    wins[idx].minimized = true;
    dirty_win(&wins[idx]);
    if (focus == idx) { focus = -1; set_focus(top_visible()); send(wins[idx].owner, WM_FOCUS, idx, 0, 0, 0); }
    add_dirty_r(dock_rect());
}

static void restore(int idx) {
    if (wins[idx].minimized) {
        struct win *w=&wins[idx];
        struct rect from=w->motion?w->visual:minimize_target(idx);
        int opacity=w->motion?w->opacity:0;
        w->minimized = false;
        if(effects_enabled && gpu_scene && !overview_open)motion_start(w,from,window_rect(w),opacity,255,180);
        dirty_win(w);
    }
    raise(idx);
    add_dirty_r(dock_rect());
}

static void set_hw_cursor(bool on) {
    if (on == hwcur) return;
    if (on && gfx_hw_cursor(1) < 0) return;                       /* карта не умеет */
    if (!on) gfx_hw_cursor(0);
    hwcur = on;
    dirty_cursor(mouse_x, mouse_y);                               /* убрать/нарисовать программный */
}

/* окно целиком в пределах экрана (насколько влезает), правее дока и ниже панели */
static void fit_window(struct win *w) {
    if (w->x + outer_w(w) > SW) w->x = SW - outer_w(w);
    if (w->y + outer_h(w) > SH) w->y = SH - outer_h(w);
    if (w->x < DOCK_W) w->x = DOCK_W;
    if (w->y < TOPBAR_H) w->y = TOPBAR_H;
}

static void close_popups(void);

/* ------------------------------------------------------------ размер окон */

/* попросить у программы буфер под нынешний размер рамки (по одной просьбе за раз) */
static void request_buffer(int idx) {
    struct win *w = &wins[idx];
    if (w->awaiting || (w->w == w->bw && w->h == w->bh)) return;
    send(w->owner, WM_RESIZE, idx, w->w, w->h, 0);
    w->awaiting = true;
    w->asked_at = ticks();
}

static void set_geometry(int idx, int x, int y, int cw, int ch) {
    struct win *w = &wins[idx];
    if (x == w->x && y == w->y && cw == w->w && ch == w->h) return;
    if (!w->minimized) dirty_win(w);
    w->x = x; w->y = y; w->w = cw; w->h = ch;
    if (!w->minimized) dirty_win(w);
    request_buffer(idx);
}

static void maximize(int idx, bool on) {
    struct win *w = &wins[idx];
    if (!w->resizable || on == w->maximized) return;
    struct rect from=w->motion?w->visual:window_rect(w);
    dirty_win(w);
    if (on) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->maximized = true;
        set_geometry(idx, DOCK_W, TOPBAR_H, SW - DOCK_W, SH - TOPBAR_H - HEAD_H);
    } else {
        w->maximized = false;
        set_geometry(idx, w->rx, w->ry, w->rw, w->rh);
    }
    if(effects_enabled && gpu_scene && !overview_open)motion_start(w,from,window_rect(w),255,255,180);
    dirty_win(w);
}

/* края окна, за которые тянут: где мышь — какие края (E_*) и у какого окна */
#define EDGE_OUT 8                      /* снаружи окна (в тени) */
#define EDGE_IN  4                      /* и чуть внутри */
#define CORNER   18                     /* у углов зона длиннее */
enum { E_L = 1, E_R = 2, E_T = 4, E_B = 8 };

static int edge_at(int px, int py, int *widx) {
    if (overview_open || py < TOPBAR_H || px < DOCK_W) return 0;
    for (int k = nz - 1; k >= 0; k--) {
        struct win *w = &wins[zorder[k]];
        if (w->minimized) continue;
        int ox = w->x, oy = w->y, ow = outer_w(w), oh = outer_h(w);
        if (!ui_inside(px, py, ox - EDGE_OUT, oy - EDGE_OUT, ow + 2 * EDGE_OUT, oh + 2 * EDGE_OUT)) continue;
        bool inside = ui_inside(px, py, ox, oy, ow, oh);
        if (!w->resizable || w->maximized) { if (inside) return 0; continue; }
        int e = 0;
        if (px < ox + EDGE_IN) e |= E_L;
        if (px >= ox + ow - EDGE_IN) e |= E_R;
        if (py < oy + 3) e |= E_T;
        if (py >= oy + oh - EDGE_IN) e |= E_B;
        if (e & (E_L | E_R)) { if (py < oy + CORNER) e |= E_T; if (py >= oy + oh - CORNER) e |= E_B; }
        if (e & (E_T | E_B)) { if (px < ox + CORNER) e |= E_L; if (px >= ox + ow - CORNER) e |= E_R; }
        if (!e) return 0;                                         /* внутри окна */
        *widx = zorder[k];
        return e;
    }
    return 0;
}

static int edge_cursor(int e) {
    if (e == (E_L | E_T) || e == (E_R | E_B)) return GFX_CURSOR_RESIZE_D1;
    if (e == (E_R | E_T) || e == (E_L | E_B)) return GFX_CURSOR_RESIZE_D2;
    if (e & (E_L | E_R)) return GFX_CURSOR_RESIZE_H;
    if (e & (E_T | E_B)) return GFX_CURSOR_RESIZE_V;
    return GFX_CURSOR_ARROW;
}

static void set_cursor_shape(int shape) {
    if (shape == gfx_cursor_get_shape()) return;
    dirty_cursor(mouse_x, mouse_y);
    gfx_cursor_shape(shape);
    if (hwcur) gfx_hw_cursor(1);                                  /* новая картинка — в карту */
}

/* сменилось разрешение экрана */
static void screen_resized(void) {
    motions_finish();
    SW = gfx_w; SH = gfx_h;
    back = gfx_buf;
    gpu_reset();
    free(wallpaper);
    make_wallpaper();
    close_popups();
    if (overview_open) { overview_open = false; free(overview_bg); overview_bg = NULL; }
    drag = -1;
    dock_build();
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used) continue;
        if (wins[i].maximized) {
            wins[i].x = DOCK_W; wins[i].y = TOPBAR_H;
            wins[i].w = SW - DOCK_W; wins[i].h = SH - TOPBAR_H - HEAD_H;
            request_buffer(i);
        } else fit_window(&wins[i]);
    }
    struct mouse ms;
    mouse_get(&ms);
    mouse_x = ms.x; mouse_y = ms.y;
    if (hwcur) { hwcur = false; set_hw_cursor(true); }
    ndirty = 0;
    add_dirty(0, 0, SW, SH);
}

/* прямоугольник a без b: до четырёх полосок — в «грязные» */
static void dirty_minus(struct rect a, struct rect b) {
    struct rect in;
    if (!intersect(a, b, &in)) { add_dirty(a.x, a.y, a.w, a.h); return; }
    if (in.y > a.y) add_dirty(a.x, a.y, a.w, in.y - a.y);
    if (in.y + in.h < a.y + a.h) add_dirty(a.x, in.y + in.h, a.w, a.y + a.h - in.y - in.h);
    if (in.x > a.x) add_dirty(a.x, in.y, in.x - a.x, in.h);
    if (in.x + in.w < a.x + a.w) add_dirty(in.x + in.w, in.y, a.x + a.w - in.x - in.w, in.h);
}

/* Move an overlapping rectangle in the compositor's cacheable RAM shadow.
 * Firmware framebuffers may be write-combined or uncached: reading them to
 * move a window is expensive. Upload the moved RAM pixels instead. */
static void copy_window_pixels(struct rect dst,int dx,int dy) {
    int first=dy>0?dst.h-1:0, end=dy>0?-1:dst.h, step=dy>0?-1:1;
    for(int j=first;j!=end;j+=step)
        memmove(back+(dst.y+j)*SW+dst.x,
                back+(dst.y+j-dy)*SW+dst.x-dx,dst.w*sizeof(*back));
    gfx_flip_rect(dst.x,dst.y,dst.w,dst.h);
}

/* Сдвинуть перетаскиваемое окно. Окно — самое верхнее: его непрозрачную
 * часть копируем аппаратно либо через RAM-буфер; заново собираем
 * только тень, скруглённые углы и открывшиеся куски. */
static void move_window(struct win *w, int nx, int ny) {
    if (nx == w->x && ny == w->y) return;
    struct rect old_full = win_full(w);
    struct rect body = { w->x, w->y + RADIUS, outer_w(w), outer_h(w) - RADIUS };   /* без углов */
    struct rect desk = { DOCK_W, TOPBAR_H, SW - DOCK_W, SH - TOPBAR_H }, src, dst;
    int dx = nx - w->x, dy = ny - w->y;
    bool copied = false;
    if (intersect(body, desk, &src)) {
        struct rect moved = { src.x + dx, src.y + dy, src.w, src.h };
        if (intersect(moved, desk, &dst)) {
            flush();                                              /* на экране — актуальный кадр */
            bool moved=false;
            if(screen_copy_accelerated)
                moved=gfx_hw_copy(dst.x-dx,dst.y-dy,dst.x,dst.y,dst.w,dst.h)==0;
            else {copy_window_pixels(dst,dx,dy);moved=true;}
            if (moved) {
                copied = true;
                w->x = nx; w->y = ny;
                dirty_minus(old_full, dst);
                dirty_minus(win_full(w), dst);
                if (!hwcur) {                                     /* курсор уехал вместе с окном — вернуть */
                    dirty_cursor(mouse_x + dx, mouse_y + dy);
                    dirty_cursor(mouse_x, mouse_y);
                }
            }
        }
    }
    if (!copied) {
        dirty_win(w);
        w->x = nx; w->y = ny;
        dirty_win(w);
    }
}

static int app_by_pid(int pid) {
    for (int i = 0; i < n_launched; i++) if (launched[i].pid == pid) return launched[i].app;
    return -1;
}

/* программа по заголовку окна: «Notepad», «file.txt - Notepad» */
static int app_by_title(const char *t) {
    int tl = strlen(t);
    for (int a = 0; a < N_APPS; a++) {
        int n = strlen(apps[a].name);
        if (!strncmp(t, apps[a].name, n)) return a;
        if (tl > n + 2 && !strcmp(t + tl - n, apps[a].name) && t[tl - n - 2] == '-') return a;
    }
    return -1;
}

static int app_by_prog(const char *prog) {
    for (int a = 0; a < N_APPS; a++) if (!strcmp(apps[a].prog, prog)) return a;
    return -1;
}

static void handle_msg(struct msg *m) {
    switch (m->type) {
    case WM_CREATE: {
        int idx = -1;
        for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { idx = i; break; }
        int needed=m->b>0 && m->c>0 && m->b<=2560 && m->c<=1600?m->b*m->c*4+((unsigned)m->d==WM_BUFFERED?WM_FRAME_BYTES:0):-1;
        color_t *px=idx>=0 && needed>0 && shm_size(m->a)>=needed?shm_map(m->a):NULL;
        if (!px || m->b <= 0 || m->c <= 0 || m->b > 2560 || m->c > 1600) {
            if (px) shm_unmap(px);
            send(m->from, WM_CREATED, -1, 0, 0, 0);
            return;
        }
        bool buffered = (unsigned)m->d == WM_BUFFERED;
        color_t *snapshot = buffered ? calloc(m->b * m->c, 4) : px;
        if (!snapshot) {
            shm_unmap(px); send(m->from, WM_CREATED, -1, 0, 0, 0); return;
        }
        struct win *w = &wins[idx];
        memset(w, 0, sizeof(*w));
        w->used = true;
        w->gpu_dirty = true; gpu_content_ready[idx] = false; gpu_frame_hash[idx] = 0;
        w->owner = m->from;
        w->shm = m->a;
        w->px = snapshot;
        if (buffered) w->frame = (struct win_frame *)px;
        w->w = w->bw = m->b; w->h = w->bh = m->c;
        memcpy(w->title, m->data, 39);
        w->title[39] = 0;
        w->app = app_by_pid(m->from);
        if (w->app < 0) w->app = app_by_title(w->title);
        /* по центру рабочей области, каждое следующее — чуть правее и ниже */
        w->x = DOCK_W + (SW - DOCK_W - outer_w(w)) / 2 - 90 + (cascade % 7) * 30;
        w->y = TOPBAR_H + (SH - TOPBAR_H - outer_h(w)) / 2 - 80 + (cascade % 7) * 26;
        cascade++;
        fit_window(w);
        zorder[nz++] = idx;
        send(m->from, WM_CREATED, idx, buffered ? WM_BUFFERED : 0, 0, 0);
        if (overview_open) { overview_open = false; add_dirty(0, 0, SW, SH); }
        dirty_win(w);
        set_focus(idx);
        dock_changed();
        if(effects_enabled && gpu_scene && !overview_open) {
            struct rect to=window_rect(w),from=to;
            from.x+=to.w/50;from.y+=to.h/50+6;from.w-=to.w/25;from.h-=to.h/25;
            motion_start(w,from,to,0,255,150);
        }
        return;
    }
    case WM_UPDATE: {
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        struct win *w = &wins[m->a];
        int rw = m->d & 0xFFFF, rh = (m->d >> 16) & 0xFFFF;
        struct rect r = { m->b, m->c, rw, rh }, all = { 0, 0, w->bw, w->bh };
        if (!intersect(r, all, &r)) return;
        if (w->frame) {
            unsigned submitted = w->frame->submitted;
            __asm__ volatile ("mfence" : : : "memory");
            const color_t *shared = (const color_t *)((const char *)w->frame + WM_FRAME_BYTES);
            for (int row = r.y; row < r.y + r.h; row++)
                memcpy(w->px + row * w->bw + r.x, shared + row * w->bw + r.x, r.w * 4);
            /* Composition only reads our private snapshot. Releasing shm here
             * is safe even when presentation is delayed or the window hidden. */
            __asm__ volatile ("mfence" : : : "memory");
            w->frame->consumed = submitted;
        }
        w->gpu_dirty = true;
        w->gpu_damage = w->gpu_damage.w ? bounding(w->gpu_damage, r) : r;
        if (w->minimized || overview_open) return;
        struct rect visible = { 0, 0, w->w, w->h };
        if (!intersect(r, visible, &r)) return;
        add_dirty(w->x + r.x, w->y + HEAD_H + r.y, r.w, r.h);
        return;
    }
    case WM_TITLE:
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        memcpy(wins[m->a].title, m->data, 39);
        wins[m->a].title[39] = 0;
        dirty_head(&wins[m->a]);
        if (wins[m->a].app < 0) add_dirty_r(dock_rect());
        return;
    case WM_DESTROY:
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        remove_window(m->a);
        return;
    case WM_RESIZABLE: {
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        struct win *w = &wins[m->a];
        w->resizable = true;
        w->min_w = m->b < 120 ? 120 : m->b;
        w->min_h = m->c < 60 ? 60 : m->c;
        dirty_head(w);
        return;
    }
    case WM_SETBUF: {                                             /* программа завела буфер нового размера */
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        struct win *w = &wins[m->a];
        w->awaiting = false;
        if (m->c <= 0 || m->d <= 0 || m->c > 2560 || m->d > 1600) return;
        unsigned marker;memcpy(&marker,m->data,sizeof(marker));
        int needed=m->c*m->d*4+(marker==WM_BUFFERED?WM_FRAME_BYTES:0);
        color_t *px=shm_size(m->b)>=needed?shm_map(m->b):NULL;
        if (!px) return;
        bool buffered = marker == WM_BUFFERED;
        color_t *snapshot = buffered ? malloc(m->c * m->d * 4) : px;
        if (!snapshot) { shm_unmap(px); return; }
        if (buffered) {
            struct win_frame *frame = (struct win_frame *)px;
            unsigned submitted = frame->submitted;
            __asm__ volatile ("mfence" : : : "memory");
            memcpy(snapshot, (char *)px + WM_FRAME_BYTES, m->c * m->d * 4);
            __asm__ volatile ("mfence" : : : "memory");
            frame->consumed = submitted;
        }
        if (w->frame) { shm_unmap(w->frame); free(w->px); }
        else shm_unmap(w->px);
        w->frame = buffered ? (struct win_frame *)px : NULL;
        w->px = snapshot; w->shm = m->b;
        w->bw = m->c; w->bh = m->d;
        w->gpu_dirty = true; gpu_content_ready[m->a] = false;
        w->gpu_damage = mkrect(0, 0, 0, 0);
        if (!w->minimized && !overview_open) add_dirty(w->x, w->y + HEAD_H, w->w, w->h);
        request_buffer(m->a);                                     /* пока ждали, рамку могли ещё растянуть */
        return;
    }
    case WM_WALLPAPER:
        free(wallpaper);
        make_wallpaper();
        if (overview_bg) { free(overview_bg); overview_bg = NULL; overview_open = false; }
        add_dirty(0, 0, SW, SH);
        return;
    case WM_DISPLAY:                                              /* Display / mode: курсор */
        set_hw_cursor(m->a != 0);
        return;
    }
}

/* ------------------------------------------------------------ запуск программ */

static void launch(int app) {
    char path[64];
    if (app < 0 || app >= N_APPS) return;
    snprintf(path, sizeof(path), "%s", apps[app].path);
    /* Start directly: copying the desktop's framebuffer heap with fork()
     * only to discard it in exec() stalls input and wastes memory. */
    int input = open("/dev/null", O_READ);
    if (input < 0) return;
    char *argv[2] = { apps[app].prog, NULL };
    int pid = spawn(path, argv, input);
    close(input);
    if (pid > 0) {
        if (n_launched == 32) { memmove(launched, launched + 1, sizeof(launched[0]) * 31); n_launched = 31; }
        launched[n_launched].pid = pid;
        launched[n_launched].app = app;
        n_launched++;
    }
}

/* окна умерших программ — убрать */
static void check_owners(void) {
    struct proc_info pi[64];
    int n = ps(pi, 64);
    if (n <= 0) return;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used) continue;
        bool alive = false;
        for (int j = 0; j < n; j++)
            if ((int)pi[j].pid == wins[i].owner && pi[j].state != 4) { alive = true; break; }
        if (!alive) remove_window(i);
    }
}

static void update_clock(void) {
    static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    struct datetime t;
    if (datetime_get(&t) < 0) return;
    /* день недели (Сакамото) */
    static const int k[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = t.year - (t.month < 3);
    int dw = (y + y / 4 - y / 100 + y / 400 + k[(t.month + 11) % 12] + t.day) % 7;
    char c[40];
    snprintf(c, sizeof(c), "%s %s %u  %02u:%02u", wd[dw], mon[(t.month + 11) % 12], t.day, t.hour, t.min);
    if (strcmp(c, clock_text)) {
        strcpy(clock_text, c);
        add_dirty(0, 0, SW, TOPBAR_H);
    }
}

/* ------------------------------------------------------------ мышь */

static int drag_dx, drag_dy, drag_px, drag_py;
static unsigned last_click_t;                                     /* двойной щелчок по заголовку */
static int last_click_win = -1, last_click_x, last_click_y;
static unsigned prev_buttons;
static bool quit, logout_requested;

static int window_at(int px, int py) {
    if (overview_open) return -1;
    if (py < TOPBAR_H || px < DOCK_W) return -1;
    for (int k = nz - 1; k >= 0; k--) {
        struct win *w = &wins[zorder[k]];
        if (!w->minimized && !w->closing && ui_inside(px, py, w->x, w->y, outer_w(w), outer_h(w))) return zorder[k];
    }
    return -1;
}

static void set_hover(int *var, int value) {
    if (*var == value) return;
    if (*var >= 0 && wins[*var].used) dirty_head(&wins[*var]);
    *var = value;
    if (value >= 0) dirty_head(&wins[value]);
}

static void set_hover_top(int v) {
    if (v == hover_top) return;
    hover_top = v;
    add_dirty(0, 0, SW, TOPBAR_H);
}

static void set_hover_dock(int v) {
    if (v == hover_dock) return;
    add_dirty_r(tip_rect());
    hover_dock = v;
    add_dirty_r(tip_rect());
    add_dirty_r(dock_rect());
}

static void close_sysmenu(void) {
    if (!sysmenu_open) return;
    sysmenu_open = false;
    struct rect r = sysmenu_rect();
    add_dirty(r.x - 24, r.y - 20, r.w + 48, r.h + 44);
    add_dirty(0, 0, SW, TOPBAR_H);
}

static void set_overview(bool on) {
    if (on == overview_open) return;
    motions_finish();
    if (on) {
        make_overview_bg();
        if (!overview_bg) return;
        hover_ov = -1;
        search[0] = 0; search_len = 0;
        ov_kbd = false;
        ov_filter();
    } else {
        free(overview_bg);
        overview_bg = NULL;
    }
    overview_open = on;
    add_dirty(0, 0, SW, SH);
}

static void close_popups(void) { close_sysmenu(); }

/* клавиши в обзоре: печать — поиск, Enter — запустить, стрелки — выбрать */
static void overview_key(int k) {
    struct rect area = mkrect(DOCK_W, TOPBAR_H, SW - DOCK_W, SH - TOPBAR_H);
    if (k == 27) {
        if (search_len) { search[0] = 0; search_len = 0; ov_filter(); add_dirty_r(area); }
        else set_overview(false);
        return;
    }
    if (k == '\r' || k == '\n') {
        if (ov_n) { int a = ov_list[ov_sel]; set_overview(false); launch(a); }
        return;
    }
    if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_UP || k == KEY_DOWN) {
        if (!ov_n) return;
        int d = k == KEY_LEFT ? -1 : k == KEY_RIGHT ? 1 : k == KEY_UP ? -ov_cols() : ov_cols();
        int n = ov_sel + d;
        if (n < 0 || n >= ov_n) return;
        ov_kbd = true;
        add_dirty_r(ov_cell(ov_sel));
        ov_sel = n;
        add_dirty_r(ov_cell(ov_sel));
        return;
    }
    if (k == 8 || k == 127) {
        if (!search_len) return;
        search_len--;
        while (search_len > 0 && ((unsigned char)search[search_len] & 0xC0) == 0x80) search_len--;
        search[search_len] = 0;
    } else if ((k >= 32 && k < 127) || (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS))) {
        char u[4];
        int n = utf8_put(u, k);
        if (search_len + n >= (int)sizeof(search)) return;
        memcpy(search + search_len, u, n);
        search_len += n;
        search[search_len] = 0;
    } else return;
    ov_filter();
    hover_ov = -1;
    add_dirty_r(area);
}

static void dock_click(int i) {
    if (i == n_dock) { set_overview(!overview_open); return; }
    set_overview(false);
    int target = -1;
    if (dock[i].app < 0) target = dock[i].win;
    else
        for (int k = nz - 1; k >= 0; k--)                         /* самое верхнее окно программы */
            if (wins[zorder[k]].app == dock[i].app) { target = zorder[k]; break; }
    if (target < 0) { launch(dock[i].app); return; }
    if (target == focus && !wins[target].minimized) minimize(target);
    else restore(target);
}

static void mouse_down(int x, int y) {
    motions_finish();
    if (sysmenu_open) {
        int it = sysmenu_at(x, y);
        if (it >= 0) {
            close_sysmenu();
            if (it == 0) launch(app_by_prog("settings"));
            else if (it == 1) launch(app_by_prog("about"));
            else if (it == 3) { logout_requested = true; quit = true; }
            else if (it == 4) quit = true;
            return;
        }
        if (it == -1) return;                                     /* по меню, мимо пунктов */
        close_sysmenu();
        if (ui_inside(x, y, tray_rect().x, 0, tray_rect().w, TOPBAR_H)) return;
    }
    if (y < TOPBAR_H) {                                           /* верхняя панель */
        struct rect a = act_rect(), t = tray_rect(), l = lang_rect();
        if (ui_inside(x, y, l.x, 0, l.w, TOPBAR_H)) { kb_lang = kblayout(!kb_lang); add_dirty(0, 0, SW, TOPBAR_H); return; }
        if (x < a.x + a.w) set_overview(!overview_open);
        else if (x >= t.x) {
            sysmenu_open = true;
            hover_sm = -1;
            struct rect r = sysmenu_rect();
            add_dirty(r.x - 24, r.y - 20, r.w + 48, r.h + 44);
            add_dirty(0, 0, SW, TOPBAR_H);
        }
        return;
    }
    int d = dock_at(x, y);
    if (d >= 0) { dock_click(d); return; }
    if (d == -1) return;
    int ew, e = edge_at(x, y, &ew);
    if (e) {                                                      /* тянем край окна */
        struct win *w = &wins[ew];
        raise(ew);
        resizing = ew; rs_edges = e;
        rs_mx = x; rs_my = y;
        rs_x = w->x; rs_y = w->y; rs_w = w->w; rs_h = w->h;
        return;
    }
    if (overview_open) {
        if (ui_inside(x, y, search_rect().x, search_rect().y, search_rect().w, search_rect().h)) return;
        int a = overview_at(x, y);
        set_overview(false);
        if (a >= 0) launch(ov_list[a]);
        return;
    }
    int idx = window_at(x, y);
    if (idx >= 0) {
        struct win *w = &wins[idx];
        raise(idx);
        if (y < w->y + HEAD_H) {                                  /* заголовок */
            int b = head_button(w, x, y);
            if (b == 1) { send(w->owner, WM_CLOSE, idx, 0, 0, 0); return; }
            if (b == 2) { maximize(idx, !w->maximized); return; }
            if (b == 3) { minimize(idx); return; }
            unsigned now = ticks();
            if (w->resizable && last_click_win == idx && now - last_click_t < 450 &&
                iabs(x - last_click_x) < 6 && iabs(y - last_click_y) < 6) {
                last_click_win = -1;
                maximize(idx, !w->maximized);
                return;
            }
            last_click_win = idx; last_click_t = now; last_click_x = x; last_click_y = y;
            drag = idx;
            drag_dx = x - w->x;
            drag_dy = y - w->y;
            drag_px = x; drag_py = y;
            return;
        }
        capture = idx;
        send(w->owner, WM_MOUSE, idx, x - w->x, y - w->y - HEAD_H, prev_buttons | (1 << 8));
        return;
    }
    if (focus >= 0) set_focus(-1);                                /* клик по обоям */
}

static void mouse_move(int ox, int oy, int x, int y) {
    if (!hwcur) { dirty_cursor(ox, oy); dirty_cursor(x, y); }
    if (resizing >= 0) {
        struct win *w = &wins[resizing];
        int dx = x - rs_mx, dy = y - rs_my;
        int nw = rs_w, nh = rs_h;
        if (rs_edges & E_R) nw = rs_w + dx;
        if (rs_edges & E_L) nw = rs_w - dx;
        if (rs_edges & E_B) nh = rs_h + dy;
        if (rs_edges & E_T) nh = rs_h - dy;
        if (nw < w->min_w) nw = w->min_w;
        if (nh < w->min_h) nh = w->min_h;
        if (nw > 2560) nw = 2560;
        if (nh > 1600) nh = 1600;
        int nx = rs_edges & E_L ? rs_x + rs_w - nw : rs_x;
        int ny = rs_edges & E_T ? rs_y + rs_h - nh : rs_y;
        if (ny < TOPBAR_H) { nh -= TOPBAR_H - ny; ny = TOPBAR_H; }
        set_geometry(resizing, nx, ny, nw, nh);
        return;
    }
    if (drag >= 0 && wins[drag].maximized) {                      /* потянули развёрнутое — вернуть размер */
        struct win *w = &wins[drag];
        if (iabs(x - drag_px) < 8 && iabs(y - drag_py) < 8) return;
        int ratio = drag_dx * 256 / (w->w > 0 ? w->w : 1);
        bool animate=effects_enabled;effects_enabled=false;
        maximize(drag, false);
        effects_enabled=animate;
        int nx = x - ratio * w->w / 256, ny = y - drag_dy;
        drag_dx = x - nx;
        set_geometry(drag, nx, ny < TOPBAR_H ? TOPBAR_H : ny, w->w, w->h);
        return;
    }
    if (drag >= 0) {
        struct win *w = &wins[drag];
        int nx = x - drag_dx, ny = y - drag_dy;
        if (ny < TOPBAR_H) ny = TOPBAR_H;
        if (ny > SH - HEAD_H) ny = SH - HEAD_H;
        if (nx < DOCK_W + 60 - outer_w(w)) nx = DOCK_W + 60 - outer_w(w);
        if (nx > SW - 60) nx = SW - 60;
        move_window(w, nx, ny);
        return;
    }
    if (sysmenu_open) {
        int it = sysmenu_at(x, y);
        if (it < 0) it = -1;
        if (it != hover_sm) { hover_sm = it; add_dirty_r(sysmenu_rect()); }
    }
    int ht = -1;
    if (y < TOPBAR_H) {
        struct rect a = act_rect(), c = clock_rect(), t = tray_rect();
        struct rect l = lang_rect();
        if (x < a.x + a.w) ht = 0;
        else if (ui_inside(x, y, c.x, 0, c.w, TOPBAR_H)) ht = 1;
        else if (x >= t.x) ht = 2;
        else if (ui_inside(x, y, l.x, 0, l.w, TOPBAR_H)) ht = 3;
    }
    set_hover_top(ht);
    int d = dock_at(x, y);
    set_hover_dock(d >= 0 ? d : -1);
    if (overview_open) {
        int a = overview_at(x, y);
        if (a != hover_ov) {
            if (hover_ov >= 0 && hover_ov < ov_n) add_dirty_r(ov_cell(hover_ov));
            hover_ov = a;
            if (a >= 0) add_dirty_r(ov_cell(a));
        }
        return;
    }
    int ew, e = edge_at(x, y, &ew);
    set_cursor_shape(edge_cursor(e));
    int idx = e ? -1 : window_at(x, y);
    int hc = -1, hm = -1, hx = -1;
    if (idx >= 0 && y < wins[idx].y + HEAD_H) {
        int b = head_button(&wins[idx], x, y);
        if (b == 1) hc = idx; else if (b == 2) hx = idx; else if (b == 3) hm = idx;
    }
    set_hover(&hover_close, hc);
    set_hover(&hover_min, hm);
    set_hover(&hover_max, hx);
    int target = capture >= 0 ? capture : (idx == focus ? idx : -1);
    if (target >= 0 && wins[target].used) {
        struct win *w = &wins[target];
        int lx = x - w->x, ly = y - w->y - HEAD_H;
        if (capture >= 0 || (lx >= 0 && ly >= 0 && lx < w->w && ly < w->h))
            send(w->owner, WM_MOUSE, target, lx, ly, prev_buttons | (3 << 8));
    }
}

static void mouse_up(int x, int y) {
    if (resizing >= 0) { request_buffer(resizing); resizing = -1; }
    if (drag >= 0 && y < TOPBAR_H + 2 && wins[drag].resizable && !wins[drag].maximized) {
        struct win *w = &wins[drag];                              /* дотащили до верха — развернуть */
        w->y = TOPBAR_H;
        maximize(drag, true);
    }
    drag = -1;
    if (capture >= 0 && wins[capture].used) {
        struct win *w = &wins[capture];
        send(w->owner, WM_MOUSE, capture, x - w->x, y - w->y - HEAD_H, prev_buttons | (2 << 8));
    }
    capture = -1;
}

/* ------------------------------------------------------------ главный цикл */

/* No desktop actions or client messages are dispatched until login succeeds. */
static void login_wipe(char *p, unsigned n) {
    volatile char *v = p;
    while (n--) *v++ = 0;
}

static bool desktop_login(void) {
    if (account(4, NULL, NULL) >= 0) return true;
    char names[16][24], password[128] = {0}, confirm[128] = {0};
    int count = 0, selected = 0, len = 0, stage = 0;
    int setup = account(0, NULL, NULL);
    const char *error = "";
    if (setup < 0) error = "Cannot read accounts. Press Escape to exit.";
    while (count < 16 && account_list(names[count], count) == 0) count++;
    if (!count) error = "No accounts available. Press Escape to exit.";
    unsigned buttons = prev_buttons;
    int field = 1; /* Account, password, submit; password starts focused. */
    unsigned caret_since = ticks();
    for (;;) {
        if (gfx_check_mode()) {
            SW = gfx_w; SH = gfx_h; back = gfx_buf;
            make_wallpaper(); dock_build();
        }
        int x = (SW - 352) / 2, y = (SH - 300) / 2;
        gfx_no_clip();
        memcpy(back, wallpaper, SW * SH * 4);
        gfx_fill_rect_a(0, 0, SW, SH, C_BLACK, 95);
        gfx_round_rect(x, y, 352, 300, 18, RGB(42, 39, 45));
        const char *title = setup > 0 ? (stage ? "Confirm password" : "Set a password") : "Sign in";
        text_draw(FONT_UI_BOLD, 24, x+28, y+24, title, C_WHITE);
        color_t accent = RGB(235, 103, 58), input = RGB(62, 58, 66);
        gfx_round_rect(x+28, y+78, 296, 44, 8, field == 0 ? accent : input);
        gfx_round_rect(x+30, y+80, 292, 40, 6, input);
        if (count) {
            int tw=text_width(FONT_UI_MEDIUM,17,names[selected]);
            text_draw(FONT_UI_MEDIUM,17,x+176-tw/2,y+89,names[selected],C_WHITE);
        }
        if (count > 1 && !setup) {
            text_draw(FONT_UI,19,x+42,y+88,"<",RGB(205,200,210));
            text_draw(FONT_UI,19,x+299,y+88,">",RGB(205,200,210));
        }
        gfx_round_rect(x+28,y+142,296,46,8,field == 1 ? accent : input);
        gfx_round_rect(x+30,y+144,292,42,6,input);
        int chars=0;
        for (int i=0;i<len;i++) if (((unsigned char)password[i]&0xc0)!=0x80) chars++;
        int visible=chars < 26 ? chars : 26;
        if (!len) text_draw(FONT_UI,15,x+42,y+155,"Password",RGB(165,158,171));
        for (int i=0;i<visible;i++) gfx_ellipse(x+44+i*10,y+162,5,5,C_WHITE,255);
        if (field == 1 && (ticks()-caret_since)%1000 < 550)
            gfx_fill_rect(x+42+visible*10,y+154,2,22,C_WHITE);
        text_draw_fit(FONT_UI,12,x+28,y+199,error,296,RGB(255,170,155));
        gfx_round_rect(x+28,y+234,296,42,8,field == 2 ? C_WHITE : accent);
        gfx_round_rect(x+30,y+236,292,38,6,accent);
        const char *button=setup > 0 ? "Continue" : "Sign in";
        int bw=text_width(FONT_UI_MEDIUM,16,button);
        text_draw(FONT_UI_MEDIUM,16,x+176-bw/2,y+245,button,C_WHITE);
        struct mouse ms; mouse_get(&ms);
        mouse_x = ms.x; mouse_y = ms.y;
        if (!hwcur) gfx_cursor_draw(mouse_x, mouse_y);
        gfx_flip();
        ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG);
        int k = readkey();
        if ((ms.buttons & MOUSE_LEFT) && !(buttons & MOUSE_LEFT)) {
            field = -1;
            if (ms.x >= x+28 && ms.x < x+324) {
                if (ms.y >= y+78 && ms.y < y+122) {
                    field = 0;
                    if (count > 1 && !setup) k = ms.x < x+176 ? KEY_UP : KEY_DOWN;
                }
                if (ms.y >= y+142 && ms.y < y+188) { field=1; caret_since=ticks(); }
                if (ms.y >= y+234 && ms.y < y+276) { field=2; k='\n'; }
            }
        }
        buttons = ms.buttons;
        if (k == 27) { login_wipe(password,sizeof(password)); login_wipe(confirm,sizeof(confirm)); return false; }
        if (setup >= 0 && count) {
            if (k == 9) {
                field = (field+1)%3; caret_since=ticks();
            } else if (setup == 0 && (k == KEY_UP || k == KEY_DOWN)) {
                selected = (selected + (k == KEY_UP ? count-1 : 1)) % count;
                login_wipe(password,sizeof(password)); len = 0; error = "";
            } else if ((k == '\n' || k == '\r') && field >= 0) {
                if (field == 0) { field=1; caret_since=ticks(); continue; }
                field=1; caret_since=ticks();
                if (setup > 0 && !stage) {
                    if (!len) error = "Enter a password.";
                    else { memcpy(confirm,password,sizeof(confirm)); login_wipe(password,sizeof(password)); len=0; stage=1; error=""; }
                } else if (setup > 0 && strcmp(confirm,password)) {
                    error="Passwords do not match. Try again."; stage=0;
                    login_wipe(password,sizeof(password)); login_wipe(confirm,sizeof(confirm)); len=0;
                } else {
                    int result = setup > 0 ? account(3,"root",password) : 0;
                    if (!result) result=account(1,names[selected],password);
                    login_wipe(password,sizeof(password)); login_wipe(confirm,sizeof(confirm)); len=0;
                    if (!result) return true;
                    error=setup > 0 ? "Could not set password. Try again." : "Incorrect password. Try again.";
                    setup=account(0,NULL,NULL); stage=0;
                }
            } else if (field == 1 && (k == 8 || k == 127)) {
                caret_since=ticks();
                if (len) {
                    do { len--; } while (len && ((unsigned char)password[len] & 0xc0) == 0x80);
                    password[len]=0;
                }
            } else if (field == 1 && ((k >= 32 && k < 127) || (k >= 0x400 && k <= 0xffff))) {
                caret_since=ticks();
                char encoded[4]; int bytes=utf8_put(encoded,k);
                if (bytes > 0 && len+bytes < (int)sizeof(password)) {
                    memcpy(password+len,encoded,bytes); len+=bytes; password[len]=0;
                }
            }
        }
        ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG);
        sleep_ms(20);
    }
}

int main(void) {
    load_apps();
    if (service_find("desktop")) { dprintf(STDERR, "desktop: already running\n"); return 1; }
    int r = gfx_open();
    if (r < 0) { dprintf(STDERR, "desktop: no graphics (%s)\n", strerror(-r)); return 1; }
    if (service_register("desktop") < 0) { gfx_close(); dprintf(STDERR, "desktop: cannot register\n"); return 1; }
    ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG);
    SW = gfx_w; SH = gfx_h;
    back = gfx_buf;
    gpu_reset();
    make_wallpaper();
    dock_build();
    struct mouse ms;
    mouse_get(&ms);
    mouse_x = ms.x; mouse_y = ms.y;
    prev_buttons = ms.buttons;
    update_clock();
    kb_lang = kblayout(-1);
    int cw, ch, want_hw = gpu_scene;
    gfx_load_config(&cw, &ch, &want_hw);
    if (want_hw) set_hw_cursor(true);
    dprintf(STDERR, "desktop: %s cursor\n", hwcur ? "independent hardware" : "software");
start_session:
    quit = false; logout_requested = false;
    if (!desktop_login()) {
        if (hwcur) gfx_hw_cursor(0);
        gfx_close();
        return 1;
    }
    mouse_get(&ms);
    prev_buttons = ms.buttons;
    unsigned last_wheel = ms.wheel;
    gpu_reset();
    dprintf(STDERR, "desktop: %s compositor\n", gpu_scene ? "GPU/VirGL" : "software");
    effects_enabled=gpu_scene;
    add_dirty(0, 0, SW, SH);
    flush();

    unsigned last_check = ticks();
    while (!quit) {
        struct msg m;
        int received=msg_recv(&m,motion_wait());
        gfx_frame_begin();
        if (received == 0) {
            handle_msg(&m);
            /* Give input and presentation a turn even under continuous app updates. */
            for (int batch=0; batch<15 && msg_recv(&m,0)==0; batch++) handle_msg(&m);
        }
        if (gfx_check_mode()) screen_resized();
        mouse_get(&ms);
        if (ms.x != mouse_x || ms.y != mouse_y) {
            int ox = mouse_x, oy = mouse_y;
            mouse_x = ms.x; mouse_y = ms.y;
            mouse_move(ox, oy, mouse_x, mouse_y);
        }
        int wheel = (int)(ms.wheel - last_wheel);
        last_wheel = ms.wheel;
        if (wheel) {
            int idx = window_at(mouse_x, mouse_y);
            if (idx >= 0 && capture < 0 &&
                ui_inside(mouse_x, mouse_y, wins[idx].x, wins[idx].y + HEAD_H, wins[idx].w, wins[idx].h))
                send(wins[idx].owner, WM_MOUSE_WHEEL, idx, mouse_x - wins[idx].x,
                     mouse_y - wins[idx].y - HEAD_H, wheel);
        }
        unsigned pressed = ms.buttons & ~prev_buttons, released = prev_buttons & ~ms.buttons;
        prev_buttons = ms.buttons;
        if (pressed & MOUSE_LEFT) mouse_down(mouse_x, mouse_y);
        else if (pressed && capture < 0) {                        /* правая кнопка — тоже в окно */
            int idx = window_at(mouse_x, mouse_y);
            if (idx >= 0 && mouse_y >= wins[idx].y + HEAD_H) {
                raise(idx);
                capture = idx;
                send(wins[idx].owner, WM_MOUSE, idx, mouse_x - wins[idx].x,
                     mouse_y - wins[idx].y - HEAD_H, ms.buttons | (1 << 8));
            }
        }
        if (released && !ms.buttons) mouse_up(mouse_x, mouse_y);

        /* режим терминала общий на всех: если кто-то (оболочка в консоли) его сменил,
         * readkey() заснул бы до нажатия клавиши — возвращаем свой */
        ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG);
        int lang = kblayout(-1);                                  /* Alt+Shift переключили в ядре */
        if (lang != kb_lang) { kb_lang = lang; add_dirty(0, 0, SW, TOPBAR_H); }
        int k;
        while ((k = readkey()) > 0) {
            if (overview_open) { close_sysmenu(); overview_key(k); continue; }
            if (k == 27 && sysmenu_open) { close_sysmenu(); continue; }
            if (focus >= 0 && wins[focus].used && !overview_open) send(wins[focus].owner, WM_KEY, focus, k, 0, 0);
        }

        while (waitpid(-1, NULL, WNOHANG) > 0) {}                 /* прибрать запущенные нами программы */
        for (int i = 0; i < MAXWIN; i++)                          /* программа не ответила на WM_RESIZE */
            if (wins[i].used && wins[i].awaiting && ticks() - wins[i].asked_at > 1500) wins[i].awaiting = false;
        if (ticks() - last_check > 500) {
            last_check = ticks();
            update_clock();
            check_owners();
        }
        motions_tick();
        flush();
        gfx_frame_end();
    }

    /* Include terminal shells and background children, even without windows. */
    struct proc_info pi[128]; bool member[128] = {0};
    int n = ps(pi,128), self = getpid();
    for (int pass=0;pass<n;pass++) for (int i=0;i<n;i++) {
        if (pi[i].ppid == (unsigned)self) member[i]=true;
        for (int j=0;j<n;j++) if (member[j] && pi[i].ppid==pi[j].pid) member[i]=true;
    }

    /* выход: попросить программы закрыться, подождать, остальных — kill */
    int owners[MAXWIN], n_owners = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used) continue;
        bool seen = false;
        for (int j = 0; j < n_owners; j++) if (owners[j] == wins[i].owner) seen = true;
        if (!seen) { owners[n_owners++] = wins[i].owner; send(wins[i].owner, WM_QUIT, i, 0, 0, 0); }
    }
    sleep_ms(300);
    for (int j = 0; j < n_owners; j++) kill(owners[j]);
    for (int i = 0; i < MAXWIN; i++) if (wins[i].used) {
        if (wins[i].frame) { shm_unmap(wins[i].frame); free(wins[i].px); }
        else shm_unmap(wins[i].px);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    if (logout_requested) {
        for (int i=n-1;i>=0;i--) if (member[i]) kill(pi[i].pid);
        while (waitpid(-1,NULL,WNOHANG)>0) {}
        account(6,NULL,NULL);
        memset(wins,0,sizeof(wins)); nz=0; focus=-1; cascade=0;
        drag=capture=resizing=-1;
        hover_top=hover_dock=hover_sm=hover_ov=-1;
        hover_close=hover_min=hover_max=-1; last_click_win=-1;
        sysmenu_open=false; overview_open=false;
        free(overview_bg); overview_bg=NULL;
        search_len=0; search[0]=0;
        ndirty=0; dock_build();
        struct msg stale;
        while (msg_recv(&stale,0)==0) {}
        goto start_session;
    }
    if (hwcur) gfx_hw_cursor(0);
    gfx_close();
    printf(ANSI_CLEAR "Desktop closed.\n");
    return 0;
}
