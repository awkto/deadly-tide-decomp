/* menu.c - the "Pilot Control Center" launcher screen, see menu.h and docs/LAUNCHER.md. */
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "peres.h"
#include "save.h"

/* ---- bitmap ids (docs/LAUNCHER.md §6) ---- */
enum {
    B_ABOUTPIC = 104, B_PANEL = 105, B_CREDITS = 110,
    B_ABOUT_H = 117, B_ADD_H = 118, B_CAL = 119, B_DEL_H = 120, B_HELP_H = 129,
    B_JOY_SEL = 130, B_MOUSE_SEL = 131, B_CAL_H = 141, B_JOY_H = 150, B_MOUSE_H = 151,
    B_ADD_DIS = 152, B_CAL_DIS = 153, B_DEL_DIS = 154, B_MOUSE_N = 157, B_JOY_DIS = 182,
    B_ARROW_P = 124, B_ARROW_N = 155, B_FLIP_OFF = 145, B_FLIP_ON = 146, B_FLIP_P = 125,
    /* I/II/III squares: frame 0 is the idle panel itself, frame 1 the mid sprite, frame 2 the lit sprite (+ 0..2);
     * quality row = 126/147, difficulty row = 121/142 (DT.EXE 0x403273 tables, LAUNCHER.md 6) */
    B_SQ_Q_MID = 126, B_SQ_Q_LIT = 147, B_SQ_D_MID = 121, B_SQ_D_LIT = 142,
    B_EXIT_N = 132, B_EXIT_P = 133, B_EXIT_A = 134, B_EXIT_B = 136, B_EXIT_C = 137, B_EXIT_D = 138,
    B_NO_H = 139, B_YES_H = 140,
    B_START_PRS = 166,                                     /* START pressed (161..164 play only after the click) */
    B_METER = 158, B_KNOB = 159,
    B_DLG_ADD = 172, B_DLG_DEL = 179,
    B_CANCEL = 170, B_CANCEL_H = 171, B_OK = 173, B_OK_H = 174, B_NO = 175, B_NO_H2 = 176,
    B_YES = 177, B_YES_H2 = 178,
    B_MAXID = 216
};

/* ---- layout (pixels in the 640x480 picture; found by matching the sprites against bmp105) ---- */
typedef struct { int x, y, w, h; } Rect;
static const Rect R_FIELD = {126, 65, 226, 22}, R_ARROW = {352, 65, 19, 22};
static const Rect R_ADD = {185, 41, 93, 23}, R_DEL = {279, 41, 94, 23};
static const Rect R_HELP = {412, 41, 103, 23}, R_ABOUT = {412, 65, 103, 23};
static const Rect R_MOUSE = {133, 138, 123, 39}, R_JOY = {133, 177, 123, 37}, R_CAL = {139, 214, 113, 32};
static const Rect R_FLIP = {133, 246, 34, 36};
static const Rect R_SLIDER = {318, 340, 44, 82};
static const Rect R_EXIT = {121, 293, 153, 159}, R_START = {369, 303, 151, 142};
#define SQ_X0 290             /* hit box; the idle bevel starts at x 292/364/436, y 138 / 227 */
#define SQ_DX 72
#define SQ_Y_Q 138
#define SQ_Y_D 227
#define METER_X 287
#define METER_Y 351
#define KNOB_X 322
#define KNOB_Y0 344           /* knob top at volume 255 */
#define KNOB_TRAVEL 60        /* H in the original formula (docs/LAUNCHER.md §3) */
#define LIST_ROW 14
#define EXF_X 121             /* "ARE YOU SURE?" flap frame (bmp138) origin; bottom-aligned to the EXIT well */
#define EXF_Y 193
#define DLG_X ((MENU_W - 273) / 2)
#define DLG_Y ((MENU_H - 115) / 2)
#define ABOUT_X ((MENU_W - 386) / 2)
#define ABOUT_Y ((MENU_H - 346) / 2)

#define COL_FIELD 0xFF0080C0u    /* RGB(0,128,192): the original's pilot text colour */
#define COL_WHITE 0xFFF7F7F7u
#define COL_GREEN 0xFF52FFCEu

typedef enum { MD_NONE, MD_ADD, MD_DEL, MD_NEEDPILOT, MD_MSG, MD_ABOUT, MD_HELP } Modal;
enum {
    W_NONE, W_FIELD, W_LIST, W_ADD, W_DEL, W_HELP, W_ABOUT, W_MOUSE, W_JOY, W_CAL, W_FLIP, W_QUAL, W_DIFF = W_QUAL + 3,
    W_SLIDER = W_DIFF + 3, W_EXIT, W_START, W_DLG_L, W_DLG_R, W_XYES, W_XNO, W_ANY
};
enum { EX_CLOSED, EX_OPENING, EX_OPEN, EX_CLOSING };
#define EX_STEPS 6            /* the flap sequences have 6 steps of 100 ms (DT.EXE 0x4056a5) */
#define EX_TICKS (MENU_TICK_HZ / 10)
#define SQ_TICKS (MENU_TICK_HZ / 5)   /* the square flip-books advance one frame per 200 ms */
/* bitmap shown at each step: EXIT pressed (opening), NO pressed (closing) */
static const int EX_OPEN_SEQ[EX_STEPS] = {134, 135, 136, 138, 137, 138};
static const int EX_CLOSE_SEQ[EX_STEPS] = {136, 135, 134, 133, 133, 132};

struct Menu {
    PeImage img[B_MAXID];
    char    path[1024];
    char    names[MENU_MAX_PILOTS][64];
    int     npil, cur;               /* cur -1 = no pilot */
    SaveData sv;                     /* current pilot's data (defaults when none) */
    int     joy_avail;
    unsigned tick;
    int     mx, my, down;
    int     active, active_row;      /* widget the button went down on */
    int     drop_open;
    Modal   modal;
    Modal   msg_next;
    char    msg[64];
    char    edit[MENU_NAME_MAX + 1];
    int     exit_state, exit_t;
    int     sqf[2][3], sq_t;         /* square flip-book frame (0..2) and its tick counter */
    unsigned about_t;
    int     list_row;                /* hovered row (set by update) */
};

/* ------------------------------------------------------------------ tiny 5x7 font (our own; the
 * original draws pilot names with the Windows GUI font). Column-major, bit0 = top row, bit7 = descender. */
static const uint8_t FONT[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},
    {0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x73},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},
    {0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43},{0x00,0x7F,0x41,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x41,0x7F},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x03,0x07,0x08,0x00},{0x20,0x54,0x54,0x78,0x40},{0x7F,0x28,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x28},
    {0x38,0x44,0x44,0x28,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x00,0x08,0x7E,0x09,0x02},{0x18,0xA4,0xA4,0x9C,0x78},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x40,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x78,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0xFC,0x18,0x24,0x24,0x18},{0x18,0x24,0x24,0x18,0xFC},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x24},
    {0x04,0x04,0x3F,0x44,0x24},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x4C,0x90,0x90,0x90,0x7C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x77,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02}
};

static void put(uint32_t *dst, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < MENU_W && y < MENU_H) dst[y * MENU_W + x] = c;
}

static void fill(uint32_t *dst, Rect r, uint32_t c)
{
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) put(dst, x, y, c);
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

static void text(uint32_t *dst, int x, int y, const char *s, uint32_t c, int scale)
{
    for (; *s; s++, x += 6 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) continue;
        for (int cx = 0; cx < 5; cx++) {
            unsigned col = FONT[ch - 32][cx];
            for (int cy = 0; cy < 8; cy++)
                if (col >> cy & 1u)
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) put(dst, x + cx * scale + sx, y + cy * scale + sy, c);
        }
    }
}

void menu_draw_text(uint32_t *dst, int w, int h, int x, int y, const char *s, uint32_t c, int scale)
{
    for (; *s; s++, x += 6 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) continue;
        for (int cx = 0; cx < 5; cx++) {
            unsigned col = FONT[ch - 32][cx];
            for (int cy = 0; cy < 8; cy++)
                if (col >> cy & 1u)
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + cx * scale + sx, py = y + cy * scale + sy;
                            if (px >= 0 && py >= 0 && px < w && py < h) dst[py * w + px] = c;
                        }
        }
    }
}

/* Draw a part of an image (alpha 0 = transparent). */
static void blit_part(uint32_t *dst, const PeImage *im, int dx, int dy, int sx, int sy, int sw, int sh)
{
    if (!im || !im->px) return;
    for (int y = 0; y < sh; y++) {
        int yy = dy + y, srow = sy + y;
        if (yy < 0 || yy >= MENU_H || srow < 0 || srow >= im->h) continue;
        for (int x = 0; x < sw; x++) {
            int xx = dx + x, scol = sx + x;
            if (xx < 0 || xx >= MENU_W || scol < 0 || scol >= im->w) continue;
            uint32_t p = im->px[srow * im->w + scol];
            if (p >> 24) dst[yy * MENU_W + xx] = p;
        }
    }
}
static void blit(uint32_t *dst, const PeImage *im, int x, int y)
{
    if (im) blit_part(dst, im, x, y, 0, 0, im->w, im->h);
}

static int hit(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
static int icmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------------ pilots */

static void persist(Menu *m)
{
    if (m->cur < 0 || !m->path[0]) return;
    save_store(&m->sv);
    save_set_current(m->path, m->names[m->cur]);
}

static void load_pilot(Menu *m, int i)
{
    m->cur = i;
    save_load(&m->sv, m->path, i >= 0 ? m->names[i] : "");
    for (int i2 = 0; i2 < 3; i2++) {                /* the squares snap to the loaded pilot's choice */
        m->sqf[0][i2] = m->sv.quality == i2 ? 2 : 0;
        m->sqf[1][i2] = m->sv.difficulty == i2 ? 2 : 0;
    }
}

static void select_pilot(Menu *m, int i)
{
    if (i == m->cur || i < 0 || i >= m->npil) return;
    persist(m);
    load_pilot(m, i);
    if (m->path[0]) save_set_current(m->path, m->names[i]);
}

static Modal open_msg(Menu *m, const char *s, Modal next)
{

    strncpy(m->msg, s, sizeof m->msg - 1);
    m->msg[sizeof m->msg - 1] = 0;
    m->msg_next = next;
    return MD_MSG;
}

static void add_confirm(Menu *m)
{
    size_t n = strlen(m->edit);
    while (n && m->edit[n - 1] == ' ') m->edit[--n] = 0;
    char *name = m->edit;
    while (*name == ' ') name++;
    if (!*name) { m->modal = MD_NONE; return; }
    for (int i = 0; i < m->npil; i++)
        if (ieq(m->names[i], name)) { m->modal = open_msg(m, "Name already in use", MD_ADD); return; }
    if (m->npil >= MENU_MAX_PILOTS) { m->modal = open_msg(m, "The Pilot name list is full", MD_NONE); return; }
    persist(m);                                      /* the old pilot's settings first, like the original */
    int i = m->npil++;                               /* insert sorted, case-insensitive */
    while (i > 0 && icmp(m->names[i - 1], name) > 0) { memcpy(m->names[i], m->names[i - 1], 64); i--; }
    memset(m->names[i], 0, 64);
    memcpy(m->names[i], name, strlen(name));
    load_pilot(m, i);                                /* new pilot: the defaults show ... */
    persist(m);                                      /* ... and are written straight back (creates the key) */
    m->edit[0] = 0;
    m->modal = MD_NONE;
}

static void delete_confirm(Menu *m)
{
    if (m->cur >= 0) {
        if (m->path[0]) save_delete_pilot(m->path, m->names[m->cur]);
        for (int i = m->cur; i + 1 < m->npil; i++) memcpy(m->names[i], m->names[i + 1], 64);
        m->npil--;
        load_pilot(m, m->npil ? 0 : -1);
        if (m->path[0]) save_set_current(m->path, m->cur >= 0 ? m->names[m->cur] : "");
    }
    m->modal = MD_NONE;
}

/* ------------------------------------------------------------------ create / free */

/* The selected I/II/III squares carry a 4 px dark well around the bevel; drawn opaquely it shows as a dark
 * rectangle that the idle panel does not have. Make the dark pixels of that outer ring transparent. */
static void key_rim(PeImage *im)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            if (x >= 4 && y >= 4 && x < im->w - 4 && y < im->h - 4) continue;
            uint32_t p = im->px[y * im->w + x];
            unsigned lum = (((p >> 16) & 255u) * 3u + ((p >> 8) & 255u) * 6u + (p & 255u)) / 10u;
            if (lum <= 58) im->px[y * im->w + x] = 0;
        }
}

static const int NEEDED[] = {104, 105, 110, 111, 112, 117, 118, 119, 120, 121, 122, 123, 124, 125, 129, 130, 131, 132, 133, 134,
                             135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
                             126, 127, 128, 154, 155, 157, 158, 159, 166, 170, 171, 172, 173, 174, 175, 176, 177, 178,
                             179, 182};

Menu *menu_create(const uint8_t *dt_exe, size_t n, const char *save_path)
{
    Menu *m = (Menu *)calloc(1, sizeof *m);
    if (!m) return NULL;
    for (size_t i = 0; i < sizeof NEEDED / sizeof NEEDED[0]; i++) {
        int id = NEEDED[i];
        if (pe_load_bitmap(dt_exe, n, id, &m->img[id]) != 0) { m->img[id].px = NULL; continue; }
        if (id == B_PANEL || id == B_ABOUTPIC || id == B_DLG_ADD || id == B_DLG_DEL) continue;
        PeImage *im = &m->img[id];                 /* navy (0,0,128) is the transparent key (docs §6) */
        for (int k = 0; k < im->w * im->h; k++)
            if ((im->px[k] & 0xFFFFFFu) == 0x000080u) im->px[k] = 0;
        if ((id >= B_SQ_D_MID && id < B_SQ_D_MID + 3) || (id >= B_SQ_D_LIT && id < B_SQ_D_LIT + 3) ||
            (id >= B_SQ_Q_MID && id < B_SQ_Q_MID + 3) || (id >= B_SQ_Q_LIT && id < B_SQ_Q_LIT + 3)) key_rim(im);
    }
    if (!m->img[B_PANEL].px || m->img[B_PANEL].w != MENU_W || m->img[B_PANEL].h != MENU_H) { menu_free(m); return NULL; }
    if (save_path)
        strncpy(m->path, save_path, sizeof m->path - 1);
    m->npil = m->path[0] ? save_list_pilots(m->path, m->names, MENU_MAX_PILOTS) : 0;
    char curname[64] = "";
    if (m->path[0]) save_get_current(m->path, curname, (int)sizeof curname);
    int c = m->npil ? 0 : -1;
    for (int i = 0; i < m->npil; i++) if (ieq(m->names[i], curname)) c = i;
    load_pilot(m, c);
    return m;
}

void menu_free(Menu *m)
{
    if (!m) return;
    for (int i = 0; i < B_MAXID; i++) if (m->img[i].px) pe_image_free(&m->img[i]);
    free(m);
}

void menu_set_joystick_available(Menu *m, int available)
{
    m->joy_avail = available != 0;
    if (!m->joy_avail && m->sv.use_joystick) m->sv.use_joystick = 0;
}

/* ------------------------------------------------------------------ hit testing */

static Rect sq_rect(int row, int i) { Rect r = {SQ_X0 + i * SQ_DX, row ? SQ_Y_D : SQ_Y_Q, 70, 70}; return r; }
static Rect list_rect(const Menu *m)
{
    Rect r = {R_FIELD.x, R_FIELD.y + R_FIELD.h, R_FIELD.w + R_ARROW.w, m->npil * LIST_ROW + 4};
    return r;
}
static Rect dlg_btn(int right) { Rect r = {DLG_X + (right ? 149 : 24), DLG_Y + 75, 102, 22}; return r; }
static Rect x_no(void) { Rect r = {EXF_X + 34, EXF_Y + 68, 74, 23}; return r; }
static Rect x_yes(void) { Rect r = {EXF_X + 30, EXF_Y + 150, 90, 70}; return r; }

/* Which widget is under (x,y), given the current modal state. */
static int pick(const Menu *m, int x, int y, int *row)
{
    if (row) *row = -1;
    switch (m->modal) {
    case MD_ADD: case MD_DEL: case MD_NEEDPILOT: case MD_MSG:
        if (hit(dlg_btn(0), x, y)) return W_DLG_L;
        if (hit(dlg_btn(1), x, y)) return W_DLG_R;
        return W_NONE;
    case MD_ABOUT: case MD_HELP: return W_ANY;
    case MD_NONE: break;
    }
    if (m->exit_state == EX_OPEN) {
        if (hit(x_no(), x, y)) return W_XNO;
        if (hit(x_yes(), x, y)) return W_XYES;
        return W_NONE;
    }
    if (m->exit_state != EX_CLOSED) return W_NONE;
    if (m->drop_open) {
        Rect l = list_rect(m);
        if (hit(l, x, y)) {
            int r = (y - l.y - 2) / LIST_ROW;
            if (row) *row = r < m->npil ? r : m->npil - 1;
            return W_LIST;
        }
        if (hit(R_FIELD, x, y) || hit(R_ARROW, x, y)) return W_FIELD;
        return W_NONE;
    }
    if (hit(R_FIELD, x, y) || hit(R_ARROW, x, y)) return W_FIELD;
    if (hit(R_ADD, x, y)) return W_ADD;
    if (hit(R_DEL, x, y)) return W_DEL;
    if (hit(R_HELP, x, y)) return W_HELP;
    if (hit(R_ABOUT, x, y)) return W_ABOUT;
    if (hit(R_MOUSE, x, y)) return W_MOUSE;
    if (hit(R_JOY, x, y)) return W_JOY;
    if (hit(R_CAL, x, y)) return W_CAL;
    if (hit(R_FLIP, x, y)) return W_FLIP;
    for (int i = 0; i < 3; i++) {
        if (hit(sq_rect(0, i), x, y)) return W_QUAL + i;
        if (hit(sq_rect(1, i), x, y)) return W_DIFF + i;
    }
    if (hit(R_SLIDER, x, y)) return W_SLIDER;
    if (hit(R_EXIT, x, y)) return W_EXIT;
    if (hit(R_START, x, y)) return W_START;
    return W_NONE;
}

static void set_slider(Menu *m, int y)
{
    int off = clampi(y - KNOB_Y0 - 7, 0, KNOB_TRAVEL);                      /* knob centre follows the mouse */
    m->sv.volume = clampi((KNOB_TRAVEL * 256 - off * 255 - 1) / KNOB_TRAVEL, 0, 255);   /* original formula */
}

static void fill_result(const Menu *m, MenuResult *r)
{
    if (m->cur >= 0)
        strncpy(r->pilot, m->names[m->cur], sizeof r->pilot - 1);
    r->difficulty = m->sv.difficulty;
    r->quality = m->sv.quality;
    r->use_joystick = m->sv.use_joystick;
    r->flip = m->sv.flip;
    r->volume = m->sv.volume;
}

static void trigger(Menu *m, int w, int row, MenuResult *res)
{
    switch (w) {
    case W_FIELD: m->drop_open = !m->drop_open; break;
    case W_LIST: m->drop_open = 0; if (row >= 0) select_pilot(m, row); break;
    case W_ADD: m->edit[0] = 0; m->modal = m->npil >= MENU_MAX_PILOTS ? open_msg(m, "The Pilot name list is full", MD_NONE) : MD_ADD; break;
    case W_DEL: if (m->cur >= 0) m->modal = MD_DEL; break;
    case W_HELP: m->modal = MD_HELP; break;
    case W_ABOUT: m->modal = MD_ABOUT; m->about_t = 0; break;
    case W_MOUSE: m->sv.use_joystick = 0; break;
    case W_JOY: if (m->joy_avail) m->sv.use_joystick = 1; break;
    case W_CAL: break;    /* the original runs the Windows joystick control panel; platform business, a no-op here */
    case W_FLIP: m->sv.flip = !m->sv.flip; break;
    case W_EXIT: m->exit_state = EX_OPENING; m->exit_t = 0; break;
    case W_XNO: m->exit_state = EX_CLOSING; m->exit_t = 0; break;
    case W_XYES: persist(m); res->action = MENU_QUIT; fill_result(m, res); break;
    case W_START:
        if (m->cur < 0) { m->modal = MD_NEEDPILOT; break; }
        persist(m);
        res->action = MENU_START;
        fill_result(m, res);
        break;
    default:
        if (w >= W_QUAL && w < W_QUAL + 3) m->sv.quality = w - W_QUAL;
        else if (w >= W_DIFF && w < W_DIFF + 3) m->sv.difficulty = w - W_DIFF;
        break;
    }
    if (w >= W_MOUSE && w < W_SLIDER) persist(m);
}

/* Dialog button pressed: right = CANCEL / NO, left = OK / YES. */
static void dialog_button(Menu *m, int right)
{
    switch (m->modal) {
    case MD_ADD: if (right) { m->edit[0] = 0; m->modal = MD_NONE; } else add_confirm(m); break;
    case MD_DEL: if (right) m->modal = MD_NONE; else delete_confirm(m); break;
    case MD_NEEDPILOT: if (right) m->modal = MD_NONE; else { m->edit[0] = 0; m->modal = MD_ADD; } break;
    case MD_MSG: m->modal = m->msg_next; break;
    default: break;
    }
}

MenuResult menu_update(Menu *m, const MenuInput *in)
{
    MenuResult res;
    memset(&res, 0, sizeof res);
    m->tick++;
    m->mx = in->x; m->my = in->y; m->down = in->down;
    if (m->modal == MD_ABOUT) m->about_t++;
    /* EXIT flap animation */
    if (m->exit_state == EX_OPENING && ++m->exit_t >= EX_STEPS * EX_TICKS) m->exit_state = EX_OPEN;
    else if (m->exit_state == EX_CLOSING && ++m->exit_t >= EX_STEPS * EX_TICKS) m->exit_state = EX_CLOSED;
    /* I/II/III flip-books: step one frame per 200 ms toward lit (selected) or idle */
    if (++m->sq_t >= SQ_TICKS) {
        m->sq_t = 0;
        for (int row = 0; row < 2; row++)
            for (int i = 0; i < 3; i++) {
                int tgt = (row ? m->sv.difficulty : m->sv.quality) == i ? 2 : 0;
                int w = (row ? W_DIFF : W_QUAL) + i;
                if (m->down && m->active == w && pick(m, m->mx, m->my, NULL) == w) tgt = tgt ? 0 : 2;
                if (m->sqf[row][i] < tgt) m->sqf[row][i]++;
                else if (m->sqf[row][i] > tgt) m->sqf[row][i]--;
            }
    }

    /* keyboard */
    if (in->text && m->modal == MD_ADD) {
        for (const char *p = in->text; *p; p++) {
            unsigned c = (unsigned char)*p;
            size_t n = strlen(m->edit);
            /* printable ASCII only (our font); the characters that would break the INI section name are refused */
            if (c >= 32 && c < 127 && c != '[' && c != ']' && c != '\\' && c != '/' && n < MENU_NAME_MAX) {
                m->edit[n] = (char)c;
                m->edit[n + 1] = 0;
            }
        }
    }
    unsigned k = in->keys;
    if (k) {
        if (m->modal == MD_ADD) {
            if (k & MENU_KEY_BACKSPACE) { size_t n = strlen(m->edit); if (n) m->edit[n - 1] = 0; }
            if (k & MENU_KEY_ENTER) dialog_button(m, 0);
            else if (k & MENU_KEY_ESC) dialog_button(m, 1);
        } else if (m->modal == MD_ABOUT || m->modal == MD_HELP) {
            if (k & (MENU_KEY_ENTER | MENU_KEY_ESC)) m->modal = MD_NONE;
        } else if (m->modal != MD_NONE) {
            if (k & MENU_KEY_ENTER) dialog_button(m, 0);
            else if (k & MENU_KEY_ESC) dialog_button(m, 1);
        } else if (m->exit_state == EX_OPEN) {
            if (k & MENU_KEY_ENTER) trigger(m, W_XYES, -1, &res);
            else if (k & MENU_KEY_ESC) trigger(m, W_XNO, -1, &res);
        } else if (m->exit_state == EX_CLOSED) {
            if (k & MENU_KEY_ESC) { if (m->drop_open) m->drop_open = 0; else trigger(m, W_EXIT, -1, &res); }
            else if (k & MENU_KEY_ENTER) trigger(m, W_START, -1, &res);
            if (k & MENU_KEY_UP) select_pilot(m, m->cur - 1);
            if (k & MENU_KEY_DOWN) select_pilot(m, m->cur + 1);
        }
    }

    /* mouse */
    int row;
    if (in->pressed) {
        m->active = pick(m, in->x, in->y, &row);
        m->active_row = row;
        if (m->drop_open && m->active == W_NONE) m->drop_open = 0;    /* click outside closes the list */
        if (m->active == W_SLIDER) set_slider(m, in->y);
    }
    if ((in->down || in->released) && m->active == W_SLIDER) {
        set_slider(m, in->y);
    }
    if (in->released) {
        int w = pick(m, in->x, in->y, &row);
        if (m->active == W_SLIDER) persist(m);
        else if (w && w == m->active && res.action == MENU_NONE) {
            if (w == W_ANY) m->modal = MD_NONE;
            else if (w == W_DLG_L || w == W_DLG_R) dialog_button(m, w == W_DLG_R);
            else trigger(m, w, row, &res);
        }
        m->active = W_NONE;
    }
    return res;
}

/* ------------------------------------------------------------------ drawing */

static const PeImage *I(const Menu *m, int id) { return id >= 0 && id < B_MAXID ? &m->img[id] : NULL; }

static void draw_dialog_btn(const Menu *m, uint32_t *dst, int right, int id_n, int id_h)
{
    Rect r = dlg_btn(right);
    int hov = hit(r, m->mx, m->my);
    blit(dst, I(m, hov ? id_h : id_n), r.x, r.y);
}

static void centered(uint32_t *dst, Rect box, int y, const char *s, uint32_t c, int scale)
{
    text(dst, box.x + (box.w - text_w(s, scale)) / 2, y, s, c, scale);
}

static void draw_dialog(const Menu *m, uint32_t *dst)
{
    Rect box = {DLG_X, DLG_Y, 273, 115};
    int add_style = m->modal == MD_ADD || m->modal == MD_MSG;
    blit(dst, I(m, add_style ? B_DLG_ADD : B_DLG_DEL), DLG_X, DLG_Y);
    if (add_style) {
        Rect f = {DLG_X + 25, DLG_Y + 38, 225, 21};
        if (m->modal == MD_ADD) {
            text(dst, f.x + 4, f.y + 6, m->edit, COL_FIELD, 1);
            if ((m->tick / 30) % 2 == 0) {
                Rect c = {f.x + 4 + text_w(m->edit, 1), f.y + 5, 5, 10};
                fill(dst, c, COL_FIELD);
            }
        } else {
            centered(dst, f, f.y + 6, m->msg, COL_FIELD, 1);
        }
        draw_dialog_btn(m, dst, 0, B_OK, B_OK_H);
        draw_dialog_btn(m, dst, 1, B_CANCEL, B_CANCEL_H);
        return;
    }
    if (m->modal == MD_DEL) {
        char nm[96];
        centered(dst, box, box.y + 14, "Are you sure you want to delete", COL_WHITE, 1);

        strncpy(nm, m->cur >= 0 ? m->names[m->cur] : "", sizeof nm - 3);
        nm[sizeof nm - 3] = 0;
        strcat(nm, " ?");
        centered(dst, box, box.y + 30, nm, COL_WHITE, 1);
    } else {
        centered(dst, box, box.y + 10, "You need to enter a pilot name", COL_WHITE, 1);
        centered(dst, box, box.y + 24, "before playing the game.", COL_WHITE, 1);
        centered(dst, box, box.y + 38, "Would you like to enter one now?", COL_WHITE, 1);
    }
    draw_dialog_btn(m, dst, 0, B_YES, B_YES_H2);
    draw_dialog_btn(m, dst, 1, B_NO, B_NO_H2);
}

static void draw_about(const Menu *m, uint32_t *dst)
{
    blit(dst, I(m, B_ABOUTPIC), ABOUT_X, ABOUT_Y);
    const PeImage *cr = I(m, B_CREDITS);
    if (!cr || !cr->px) return;
    /* the credits strip scrolls up through the empty lower part of the picture (original: M, not decoded) */
    int vy = 226, vh = 346 - 226 - 4;
    int total = cr->h + vh;
    int pos = (int)((m->about_t / 2) % (unsigned)total);       /* 30 px/s at 60 Hz */
    int src = pos - vh;                                       /* strip row at the top of the window */
    int sy = src < 0 ? 0 : src;
    int dy = ABOUT_Y + vy + (sy - src);
    int hh = vh - (sy - src);
    if (sy + hh > cr->h) hh = cr->h - sy;
    if (hh > 0) blit_part(dst, cr, ABOUT_X, dy, 0, sy, cr->w, hh);
}

static void draw_help(uint32_t *dst)
{
    for (int i = 0; i < MENU_W * MENU_H; i++) {                /* dim the screen behind */
        uint32_t p = dst[i];
        dst[i] = 0xFF000000u | ((p >> 1) & 0x7F7F7Fu);
    }
    Rect box = {20, 60, 600, 360};
    fill(dst, box, 0xFF081820u);
    Rect b1 = {box.x, box.y, box.w, 2}, b2 = {box.x, box.y + box.h - 2, box.w, 2};
    Rect b3 = {box.x, box.y, 2, box.h}, b4 = {box.x + box.w - 2, box.y, 2, box.h};
    fill(dst, b1, COL_GREEN); fill(dst, b2, COL_GREEN); fill(dst, b3, COL_GREEN); fill(dst, b4, COL_GREEN);
    centered(dst, box, box.y + 16, "DEADLY TIDE - HELP", COL_GREEN, 3);
    static const char *const L[] = {
        "PILOT: pick a pilot in the list, ADD a new one",
        "(up to 12) or DELETE the selected one.",
        "CONTROLS: MOUSE or JOYSTICK; FLIP DIRECTION",
        "inverts the vertical aim.",
        "GRAPHIC QUALITY and DIFFICULTY: I, II or III.",
        "VOLUME: drag the slider.",
        "START begins the game with the chosen pilot.",
        "EXIT asks for confirmation (Esc also exits).",
        "",
        "In game: mouse aims, left button fires,",
        "right button picks the path, middle or",
        "Space uses the ACT weapon.",
        "",
        "Keys here: Enter = START, Esc = EXIT,",
        "Up / Down = previous / next pilot.",
        "Settings are saved per pilot.",
    };
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) text(dst, box.x + 20, box.y + 60 + (int)i * 16, L[i], COL_WHITE, 2);
    centered(dst, box, box.y + box.h - 22, "Click or press Enter to close", COL_GREEN, 1);
}

void menu_draw(const Menu *m, uint32_t *dst)
{
    memcpy(dst, m->img[B_PANEL].px, sizeof(uint32_t) * MENU_W * MENU_H);
    int hover = pick(m, m->mx, m->my, NULL);
    int pressed = m->down ? m->active : W_NONE;
    int modal = m->modal != MD_NONE;
    if (modal) hover = W_NONE;
    #define HOV(w) (hover == (w) || (pressed == (w) && hover == (w)))

    /* pilot field */
    if (m->cur >= 0) text(dst, R_FIELD.x + 4, R_FIELD.y + 7, m->names[m->cur], COL_FIELD, 1);
    blit(dst, I(m, (hover == W_FIELD && pressed == W_FIELD) ? B_ARROW_P : B_ARROW_N), R_ARROW.x, R_ARROW.y);

    /* ADD / DELETE / HELP / ABOUT: lit on hover, dim when unusable */
    if (m->npil >= MENU_MAX_PILOTS) blit(dst, I(m, B_ADD_DIS), R_ADD.x, R_ADD.y);
    else if (HOV(W_ADD)) blit(dst, I(m, B_ADD_H), R_ADD.x, R_ADD.y);
    if (m->cur < 0) blit(dst, I(m, B_DEL_DIS), R_DEL.x, R_DEL.y);
    else if (HOV(W_DEL)) blit(dst, I(m, B_DEL_H), R_DEL.x, R_DEL.y);
    if (HOV(W_HELP)) blit(dst, I(m, B_HELP_H), R_HELP.x, R_HELP.y);
    if (HOV(W_ABOUT)) blit(dst, I(m, B_ABOUT_H), R_ABOUT.x, R_ABOUT.y);

    /* CONTROLS. Mapping (unknown in the original): selected = blue sprite, hover = green, an unselected
     * MOUSE uses the bright idle variant, an unavailable JOYSTICK the dim one. */
    int joy = m->sv.use_joystick;
    if (!joy) blit(dst, I(m, HOV(W_MOUSE) ? B_MOUSE_H : B_MOUSE_SEL), 133, 138);
    else blit(dst, I(m, HOV(W_MOUSE) ? B_MOUSE_H : B_MOUSE_N), 133, 138);
    if (!m->joy_avail) blit(dst, I(m, B_JOY_DIS), 133, 177);
    else if (joy) blit(dst, I(m, B_JOY_SEL), 133, 177);
    else if (HOV(W_JOY)) blit(dst, I(m, B_JOY_H), 133, 177);
    if (joy && m->joy_avail) {                        /* the hover sprite is 31 px tall, the normal one 32 */
        if (HOV(W_CAL)) blit(dst, I(m, B_CAL_H), 139, 215);
        else blit(dst, I(m, B_CAL), 139, 214);
    }
    blit(dst, I(m, m->sv.flip ? B_FLIP_ON : B_FLIP_OFF), 133, 246);
    if (pressed == W_FLIP && hover == W_FLIP) blit(dst, I(m, B_FLIP_P), 133, 246);

    /* radio rows: flip-book per square (frame 0 = the idle panel, 1 = mid sprite, 2 = lit sprite) */
    for (int row = 0; row < 2; row++) {
        for (int i = 0; i < 3; i++) {
            int f = m->sqf[row][i];
            if (f > 0) blit(dst, I(m, (f == 1 ? (row ? B_SQ_D_MID : B_SQ_Q_MID) : (row ? B_SQ_D_LIT : B_SQ_Q_LIT)) + i),
                            291 + i * SQ_DX, row ? 227 : 138);
        }
    }

    /* volume: lit meter rows from the bottom, knob */
    {
        const PeImage *me = I(m, B_METER);
        if (me && me->px) {
            int vh = (me->h * m->sv.volume + 127) / 255;
            blit_part(dst, me, METER_X, METER_Y + me->h - vh, 0, me->h - vh, me->w, vh);
        }
        int off = (KNOB_TRAVEL * (255 - m->sv.volume) + 127) / 255;
        blit(dst, I(m, B_KNOB), KNOB_X, KNOB_Y0 + off);
    }

    /* START: pressed look while the button is held (the 161..164 glow plays only after the click) */
    if (pressed == W_START && hover == W_START) blit(dst, I(m, B_START_PRS), 369, 303);

    /* EXIT flap */
    if (m->exit_state == EX_CLOSED) {
        if (hover == W_EXIT) blit(dst, I(m, pressed == W_EXIT ? B_EXIT_P : B_EXIT_N), 121, 293);
    } else {
        int st = clampi(m->exit_t / EX_TICKS, 0, EX_STEPS - 1);
        int fid = m->exit_state == EX_OPEN ? B_EXIT_D : m->exit_state == EX_OPENING ? EX_OPEN_SEQ[st] : EX_CLOSE_SEQ[st];
        const PeImage *im = I(m, fid);
        if (im && im->px) blit(dst, im, EXF_X, EXF_Y + 259 - im->h);
        if (m->exit_state == EX_OPEN) {
            if (hover == W_XNO) blit(dst, I(m, B_NO_H), x_no().x, x_no().y);
            if (hover == W_XYES) blit(dst, I(m, B_YES_H), EXF_X, EXF_Y + 135);
        }
    }

    /* pilot drop-down list: black box, RGB(0,128,192) text, hovered row inverted */
    if (m->drop_open && m->exit_state == EX_CLOSED && !modal) {
        Rect l = list_rect(m);
        Rect o = {l.x - 1, l.y - 1, l.w + 2, l.h + 2};
        fill(dst, o, COL_FIELD);
        fill(dst, l, 0xFF000000u);
        int row;
        int hr = pick(m, m->mx, m->my, &row) == W_LIST ? row : -1;
        for (int i = 0; i < m->npil; i++) {
            Rect rr = {l.x + 2, l.y + 2 + i * LIST_ROW, l.w - 4, LIST_ROW};
            int on = i == hr || (hr < 0 && i == m->cur);
            if (on) fill(dst, rr, COL_FIELD);
            text(dst, rr.x + 3, rr.y + 3, m->names[i], on ? 0xFF000000u : COL_FIELD, 1);
        }
    }

    if (m->modal == MD_ADD || m->modal == MD_DEL || m->modal == MD_NEEDPILOT || m->modal == MD_MSG) draw_dialog(m, dst);
    else if (m->modal == MD_ABOUT) draw_about(m, dst);
    else if (m->modal == MD_HELP) draw_help(dst);
    #undef HOV
}
