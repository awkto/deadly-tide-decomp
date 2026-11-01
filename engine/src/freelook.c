/* freelook.c - see freelook.h. Addresses refer to DTPLAYER.EXE (docs/FREELOOK_SPEC.md). */
#include "freelook.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cinepak.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FX_W   (FL_W * 16)      /* 0x1400: screen width in 28.4 */
#define FX_H   (FL_H * 16)      /* 0xc80 */
#define CUBE_HALF 2048.0        /* FreeLook_InitGrid 0x405310 */
#define CUBE_STEP 1024.0
#define TEXMAX 127.0f           /* vertex UVs run 0..127 (0x42fe0000) */

/* ---- life cycle / texture -------------------------------------------------------- */

int fl_init(Freelook *fl)
{
    memset(fl, 0, sizeof *fl);
    fl->strip = calloc((size_t)FL_TILE * FL_STRIP_H, sizeof(uint32_t));
    if (!fl->strip) return -1;
    fl->half_fov = 0x3c00;
    fl->mode = 1;
    fl->highest_level = 1;
    return 0;
}

void fl_free(Freelook *fl)
{
    free(fl->strip);
    free(fl->save);
    memset(fl, 0, sizeof *fl);
}

/* Array row of tile t's top image row: engine rows t*128..t*128+127 (bottom-up) map to array
 * rows 12287-row, i.e. the tile is a top-down image starting at array row (95-t)*128. */
static uint32_t *tile_top(const Freelook *fl, int t)
{
    return fl->strip + (size_t)(FL_NTILES - 1 - t) * FL_TILE * FL_TILE;
}

int fl_load_tile(Freelook *fl, int t, const uint8_t *slot, size_t n)
{
    if (t < 0 || t >= FL_NTILES || n < FL_TILE_HDR + 10) return -1;
    uint32_t len = slot[0] | slot[1] << 8 | slot[2] << 16 | (uint32_t)slot[3] << 24;
    const uint8_t *cp = slot + FL_TILE_HDR;
    if (len < 10 || FL_TILE_HDR + (size_t)len > n) return -1;
    int w = cp[4] << 8 | cp[5], h = cp[6] << 8 | cp[7];
    if (w != FL_TILE || h != FL_TILE) return -1;
    /* Every shipped tile is a key frame, so a fresh decoder per tile is equivalent to the
     * engine's single shared one. */
    CinepakDecoder d;
    if (cinepak_init(&d, FL_TILE, FL_TILE) != 0) return -1;
    int rc = cinepak_decode(&d, cp, len);
    if (rc == 0) memcpy(tile_top(fl, t), d.pixels, (size_t)FL_TILE * FL_TILE * sizeof(uint32_t));
    cinepak_free(&d);
    return rc == 0 ? 0 : -1;
}

/* ---- camera ------------------------------------------------------------------------- */

static int wrap_yaw(int y) { y %= FL_UNITS; return y < 0 ? y + FL_UNITS : y; }

void fl_view_set(Freelook *fl, int yaw, int pitch, int roll)
{
    fl->yaw = yaw; fl->pitch = pitch; fl->roll = roll;
    fl->m2_n = fl->m2_ph = fl->m2_k = 0;
    fl->m2_roll0 = 0;
}

void fl_view_clamp(Freelook *fl)   /* View_Clamp6 0x410a10 */
{
    if (fl->pitch > 0x270) fl->pitch = 0x270;
    if (fl->pitch < -0x270) fl->pitch = -0x270;
    fl->yaw = wrap_yaw(fl->yaw);
}

static int deg_to_units(double d)
{
    /* The engine truncates (x 6.9333); rounding avoids a 1-unit/frame drift from the
     * units -> degrees -> units round trip when the mouse is centred. */
    return (int)lround(d * FL_UNITS / 360.0);
}

/* FreeLook_Control1 0x40edd0, no pick / auto-target (the hub). */
static void control1(Freelook *fl, const FreelookInput *in)
{
    int rate = (in->cx - FL_W / 2) / 2;
    if (rate < -80) rate = -80;     /* max yaw rate 0x421288 = 80 */
    if (rate > 80) rate = 80;
    fl->pitch += (FL_H / 2 - in->cy) / 2;
    fl->yaw += rate;                /* the cos() pole correction always divides by 1.0 */
}

/* FreeLook_Control2 0x40fc10 core (planner 0x40f1c0 not emulated): yaw/pitch in degrees,
 * bank to 4x the yaw rate, gentle rocking when centred. Easing details are M. */
static void control2(Freelook *fl, const FreelookInput *in)
{
    double k = 360.0 / FL_UNITS;
    double yaw = fl->yaw * k, pitch = fl->pitch * k, roll = fl->roll * k;
    double r = (in->cx - FL_W / 2) / 27.0;
    yaw = fmod(yaw + r, 360.0);
    if (yaw < 0) yaw += 360.0;
    pitch += (FL_H / 2 - in->cy) / 27;
    if (pitch > 90) pitch = 90;
    if (pitch < -90) pitch = -90;
    if (r > 0.5 || r < -0.5) {
        if (fl->m2_ph > 15) fl->m2_k = 15;
        fl->m2_ph = 0;
        if (fl->m2_k) {
            roll += (4 * r - roll) * (15 - fl->m2_k) / 15.0;
            fl->m2_k--;
        } else {
            if (fl->m2_n == 0) fl->m2_roll0 = roll;
            fl->m2_n++;
            roll = fl->m2_n < 16 ? roll + (4 * r - fl->m2_roll0) / 16.0 : 4 * r;
        }
    } else {
        if (fl->m2_ph == 0) {      /* 0x418aec seeds the phase from the roll (L): acos(roll/5) */
            double q = roll / 5.0;
            fl->m2_ph = q >= -1 && q <= 1 ? (int)(acos(q) * 180.0 / M_PI) : 0;
        }
        fl->m2_ph += 3;
        if (fl->m2_n == 0) fl->m2_roll0 = roll;
        fl->m2_n++;
        double amp = fl->m2_ph >= 30 ? 5.0 : fl->m2_ph / 6.0;
        roll = fl->m2_n < 16 ? roll + (4 * r - fl->m2_roll0) / 16.0
                             : cos(fl->m2_ph * M_PI / 180.0) * amp;
    }
    fl->yaw = deg_to_units(yaw);
    fl->pitch = deg_to_units(pitch);
    fl->roll = deg_to_units(roll);
}

/* Mode 3 (0x4100f0, M/L): faster rates, pitch [-50, 60], pole-compensated whole-degree yaw. */
static void control3(Freelook *fl, const FreelookInput *in)
{
    double k = 360.0 / FL_UNITS;
    double yaw = fl->yaw * k, pitch = fl->pitch * k;
    double r = (in->cx - FL_W / 2) / 16.0;
    pitch += (FL_H / 2 - in->cy) / 16;
    if (pitch > 60) pitch = 60;
    if (pitch < -50) pitch = -50;
    double c = cos(pitch * M_PI / 180.0);
    yaw += trunc(r / (c > 0.5 ? c : 0.5));
    fl->yaw = deg_to_units(yaw);
    fl->pitch = deg_to_units(pitch);
    fl->roll = deg_to_units(r);       /* bank with the turn rate (sign L) */
}

void fl_control(Freelook *fl, const FreelookInput *in)
{
    switch (fl->mode) {
    case 2: control2(fl, in); break;
    case 3: control3(fl, in); break;
    default: control1(fl, in); break;   /* modes 0/1/4: hub-style rate control */
    }
    fl_view_clamp(fl);
}

/* ---- turns (§5.3) ------------------------------------------------------------------ */

static int wrap_half(int d)            /* yaw delta to -1248..1248 */
{
    if (d > FL_UNITS / 2) d -= FL_UNITS;
    if (d < -FL_UNITS / 2) d += FL_UNITS;
    return d;
}

void fl_turn_reset(Freelook *fl)
{
    fl->turn_n = fl->turn_cnt = 0;
    fl->auto_on = fl->plan_on = 0;
}

int fl_turn_begin(Freelook *fl, int ty, int tp, int tr, int frames_left)
{
    int n;
    if (fl->mode == 2 || fl->mode == 3) n = frames_left % 5 + 5;
    else { n = frames_left % 15; if (n < 5) n += 15; }
    fl->turn_n = fl->turn_cnt = n;
    fl->turn_ty = ty; fl->turn_tp = tp; fl->turn_tr = tr;
    fl->turn_dy = wrap_half(ty - fl->yaw);
    fl->turn_dp = tp - fl->pitch;
    /* Mode 1 leaves the roll alone; the 2/3 planner also steers it (to 0x48 x 0.25, L). */
    fl->turn_dr = fl->mode == 2 || fl->mode == 3 ? tr / 4 - fl->roll : 0;
    fl->turn_tr = fl->roll + fl->turn_dr;
    return n;
}

int fl_turn_step(Freelook *fl)
{
    if (fl->turn_cnt <= 0) return 0;
    fl->turn_cnt--;
    fl->yaw = fl->turn_ty - fl->turn_dy * fl->turn_cnt / fl->turn_n;
    fl->pitch = fl->turn_tp - fl->turn_dp * fl->turn_cnt / fl->turn_n;
    fl->roll = fl->turn_tr - fl->turn_dr * fl->turn_cnt / fl->turn_n;
    fl_view_clamp(fl);
    return fl->turn_cnt;
}

/* Frames the mode-2/3 planner needs for a turn of |d| degrees: an accelerate / decelerate ramp
 * over the cumulative table {3,6,10,...,78} (1 deg/frame^2), 12 deg/frame cruise beyond it. */
static int plan_frames(double d)
{
    static const int ramp[12] = { 0, 3, 6, 10, 15, 21, 28, 36, 45, 55, 66, 78 };
    d = fabs(d);
    for (int i = 0; i < 12; i++) if (2.0 * ramp[i] >= d) return 2 * i;
    return 22 + (int)ceil((d - 2.0 * 78) / 12.0);
}

int fl_auto_turn(Freelook *fl, int ty, int tp, int tr, int frames_left)
{
    if (fl->mode != 2 && fl->mode != 3) {
        /* FreeLook_Control1: y = ty - dy * framesLeft / N, N = framesLeft at the first call. */
        if (!fl->auto_on) {
            fl->auto_on = 1;
            fl->auto_ty = ty; fl->auto_tp = tp;
            fl->auto_dy = wrap_half(ty - fl->yaw);
            fl->auto_dp = tp - fl->pitch;
            fl->auto_n = frames_left > 0 ? frames_left : 1;
        }
        int f = frames_left < 0 ? 0 : frames_left;
        fl->yaw = fl->auto_ty - fl->auto_dy * f / fl->auto_n;
        fl->pitch = fl->auto_tp - fl->auto_dp * f / fl->auto_n;
        fl_view_clamp(fl);
        return 1;
    }
    /* Planner 0x40f1c0 (approximation): keep free control while there is time, then take
     * over and ease (smoothstep) from the pose at take-over to the exit pose, arriving on the
     * segment's last frame. The exit roll is scaled by 0.25 (const 0x420248). */
    if (!fl->plan_on) {
        double k = 360.0 / FL_UNITS;
        double dy = wrap_half(ty - fl->yaw) * k, dp = (tp - fl->pitch) * k;
        int need = plan_frames(dy), np = plan_frames(dp);
        if (np > need) need = np;
        if (frames_left > need) return 0;
        fl->plan_on = 1;
        fl->plan_n = frames_left + 1;
        fl->plan_y0 = fl->yaw; fl->plan_p0 = fl->pitch; fl->plan_r0 = fl->roll;
        fl->plan_dy = wrap_half(ty - fl->yaw);
        fl->plan_dp = tp - fl->pitch;
        fl->plan_dr = tr / 4 - fl->roll;
    }
    int f = frames_left < 0 ? 0 : frames_left;
    double t = (double)(fl->plan_n - f) / fl->plan_n;
    if (t > 1) t = 1;
    double e = t * t * (3 - 2 * t);
    fl->yaw = fl->plan_y0 + (int)lround(fl->plan_dy * e);
    fl->pitch = fl->plan_p0 + (int)lround(fl->plan_dp * e);
    fl->roll = fl->plan_r0 + (int)lround(fl->plan_dr * e);
    fl_view_clamp(fl);
    return 1;
}

/* ---- FreeLook_FixupObjects 0x402d30 ---------------------------------------------------- */

static void face_point(int f, double x, double y, double p[3])   /* half-size 256 cube */
{
    switch (f) {
    case 0:  p[0] = x - 256; p[1] = 256 - y; p[2] = 256;     break;
    case 1:  p[0] = -256;    p[1] = 256 - y; p[2] = x - 256; break;
    case 2:  p[0] = 256 - x; p[1] = 256 - y; p[2] = -256;    break;
    case 3:  p[0] = 256;     p[1] = 256 - y; p[2] = 256 - x; break;
    case 4:  p[0] = x - 256; p[1] = -256;    p[2] = 256 - y; break;
    default: p[0] = x - 256; p[1] = 256;     p[2] = y - 256; break;
    }
}

static void view_matrix(const Freelook *fl, double m[3][3]);
static double round_half(double v);

void fl_fixup_objects(const Freelook *fl, FgBlock *b)
{
    if (!b) return;
    double m[3][3];
    view_matrix(fl, m);
    double k = (FL_W / 2) / tan(fl->half_fov * M_PI / 92160.0);
    for (int i = 0; i < b->count; i++) {
        FgRecord *r = &b->rec[i];
        if (r->kind == OBJ_OVERLAY) continue;
        int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN, vis = 0, ri = 0;
        for (int f = 0; f < 6 && ri < 3; f++) {
            if (!(r->face_mask >> f & 1)) continue;
            const int16_t *rc = r->face_rect[ri++];
            for (int c = 0; c < 4; c++) {
                double p[3];
                face_point(f, rc[(c & 1) ? 2 : 0], rc[(c & 2) ? 3 : 1], p);
                double vx = p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0];
                double vy = p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1];
                double vz = p[0] * m[0][2] + p[1] * m[1][2] + p[2] * m[2][2];
                if (vz < 1.0) continue;
                int sx = (int)(FL_W / 2 + round_half(vx / vz * k)), sy = (int)(FL_H / 2 + round_half(vy / vz * k));
                if (sx < x0) x0 = sx;
                if (sx > x1) x1 = sx;
                if (sy < y0) y0 = sy;
                if (sy > y1) y1 = sy;
                vis = 1;
            }
        }
        if (vis) {
            if (x0 < 1) x0 = 0;
            if (x1 > FL_W - 1) x1 = FL_W - 1;
            if (y0 < 1) y0 = 0;
            if (y1 > FL_H - 1) y1 = FL_H - 1;
            if (x0 >= FL_W - 1 || x1 < 1 || y0 >= FL_H - 1 || y1 < 1) vis = 0;
            if (x0 == 0 && x1 == FL_W - 1 && y0 == 0 && y1 == FL_H - 1) vis = 0;
        }
        if (!vis) x0 = y0 = x1 = y1 = 0;
        r->bbox[0] = (int16_t)x0; r->bbox[1] = (int16_t)y0; r->bbox[2] = (int16_t)x1; r->bbox[3] = (int16_t)y1;
    }
}

void fl_script_pose(Freelook *fl, const FreelookScript *s, int i, int ey, int ep, int er)
{
    if (s->pan) {
        fl->yaw = wrap_yaw(ey + s->pan * i); fl->pitch = ep; fl->roll = er;
    } else if (s->n > 0) {
        const int *p = s->pose[i < s->n ? i : s->n - 1];
        fl->yaw = wrap_yaw(p[0]); fl->pitch = p[1]; fl->roll = p[2];
    }
}

int fl_script_parse(FreelookScript *s, const char *text)
{
    memset(s, 0, sizeof *s);
    if (!strncmp(text, "pan", 3)) {
        s->pan = text[3] == ':' ? atoi(text + 4) : 8;
        return s->pan ? 0 : -1;
    }
    const char *p = text;
    while (*p && s->n < FL_SCRIPT_MAX) {
        char *e;
        int v[3] = { 0, 0, 0 }, nv = 0;
        while (nv < 3) {
            long x = strtol(p, &e, 10);
            if (e == p) return -1;
            v[nv++] = (int)x;
            p = e;
            if (*p != ',') break;
            p++;
        }
        if (nv < 2) return -1;
        memcpy(s->pose[s->n++], v, sizeof v);
        if (*p == ';') p++;
        else if (*p) return -1;
    }
    return s->n ? 0 : -1;
}

/* ---- projection --------------------------------------------------------------------- */

typedef struct { int x, y; float z, u, v; } Vert;   /* Tri_Raster vertex: 28.4 x/y, float z u v */

/* Mat_FromEuler 0x40acf0 for type 6: M = Ry(-yaw) . Rx(-pitch) . Rz(-roll), p' = p . M. */
static void view_matrix(const Freelook *fl, double m[3][3])
{
    double k = 2 * M_PI / FL_UNITS;
    double a = -fl->yaw * k, b = -fl->pitch * k, g = -fl->roll * k;
    double ry[3][3] = { { cos(a), 0, -sin(a) }, { 0, 1, 0 }, { sin(a), 0, cos(a) } };
    double rx[3][3] = { { 1, 0, 0 }, { 0, cos(b), sin(b) }, { 0, -sin(b), cos(b) } };
    double rz[3][3] = { { cos(g), sin(g), 0 }, { -sin(g), cos(g), 0 }, { 0, 0, 1 } };
    double t[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t[i][j] = ry[i][0] * rx[0][j] + ry[i][1] * rx[1][j] + ry[i][2] * rx[2][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = t[i][0] * rz[0][j] + t[i][1] * rz[1][j] + t[i][2] * rz[2][j];
}

static double round_half(double v) { return trunc(v >= 0 ? v + 0.5 : v - 0.5); }

/* Proj_WorldToScreen 0x40b4d0 with W/H in 28.4. Returns status 0 (behind), 1, 2 (in frustum). */
static int project(const double p[3], double m[3][3], double kproj, Vert *o)
{
    double vx = p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0];
    double vy = p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1];
    double vz = p[0] * m[0][2] + p[1] * m[1][2] + p[2] * m[2][2];
    if (vz < 1.0) { o->x = -1; o->y = -1; o->z = -1.0f; return 0; }
    o->x = (int)(FX_W / 2 + round_half(vx / vz * kproj));
    o->y = (int)(FX_H / 2 + round_half(vy / vz * kproj));
    o->z = (float)vz;
    return fabs(vx) <= vz && fabs(vy) <= vz ? 2 : 1;
}

/* ---- rasteriser (Hecker sub-affine: 0x4080e0 / 0x407ee0 / 0x4083c0 / 0x408500 / 0x43f000) ---- */

typedef struct {
    float oz[3], uz[3], vz[3];
    float dozdx, dozdx8, dozdy, duzdx, duzdx8, duzdy, dvzdx, dvzdx8, dvzdy;
    int   umod, vmod;
} Grad;

typedef struct {
    int   x, xstep, num, den, err, y, h;
    float oz, ozs, oze, uz, uzs, uze, vz, vzs, vze;
} Edge;

static void grad_init(Grad *g, const Vert *v)
{
    int t1 = (v[0].x - v[2].x) * (v[2].y - v[1].y);
    int t2 = (v[0].y - v[2].y) * (v[1].x - v[2].x);
    float odx = 1.0f / ((float)(t1 / 16 + t2 / 16) * (1.0f / 16));
    float ody = -odx;
    for (int i = 0; i < 3; i++) {
        float oz = 1.0f / v[i].z;
        g->oz[i] = oz; g->uz[i] = v[i].u * oz; g->vz[i] = v[i].v * oz;
    }
    float y02 = (float)(v[0].y - v[2].y) / 16, y12 = (float)(v[1].y - v[2].y) / 16;
    float x02 = (float)(v[0].x - v[2].x) / 16, x12 = (float)(v[1].x - v[2].x) / 16;
#define GRAD(a, dx, dy) do { \
        float d1 = a[1] - a[2], d0 = a[0] - a[2]; \
        dx = (y02 * d1 - y12 * d0) * odx; \
        dy = (x02 * d1 - x12 * d0) * ody; } while (0)
    GRAD(g->oz, g->dozdx, g->dozdy);
    GRAD(g->uz, g->duzdx, g->duzdy);
    GRAD(g->vz, g->dvzdx, g->dvzdy);
#undef GRAD
    g->dozdx8 = g->dozdx * 8; g->duzdx8 = g->duzdx * 8; g->dvzdx8 = g->dvzdx * 8;
    /* Rounding bias: 0x8000, or 0x7fff when the coordinate decreases along x (Hecker's
     * dUdXModifier); ties broken by the y direction. */
    float ind = g->duzdx * g->oz[0] - g->dozdx * g->uz[0];
    if (ind > 0) g->umod = 0x8000;
    else if (ind < 0) g->umod = 0x7fff;
    else g->umod = g->duzdy * g->oz[0] - g->dozdy * g->uz[0] < 0 ? 0x7fff : 0x8000;
    ind = g->dvzdx * g->oz[0] - g->dozdx * g->vz[0];
    if (ind > 0) g->vmod = 0x8000;
    else if (ind < 0) g->vmod = 0x7fff;
    else g->vmod = g->dvzdy * g->oz[0] - g->dozdy * g->vz[0] < 0 ? 0x7fff : 0x8000;
}

static int ceil16(int v) { int n = v + 15; return n >= 0 ? n / 16 : -((-n + 15) / 16); }

static void floordivmod(long long n, long long d, int *q, int *r)
{
    if (n >= 0) { *q = (int)(n / d); *r = (int)(n % d); return; }
    long long qq = n / d, rr = (-n) % d;
    if (rr) { qq--; rr = d - rr; }
    *q = (int)qq; *r = (int)rr;
}

static void edge_init(Edge *e, const Grad *g, const Vert *v, int top, int bot)
{
    memset(e, 0, sizeof *e);
    const Vert *t = &v[top], *b = &v[bot];
    e->y = ceil16(t->y);
    e->h = ceil16(b->y) - e->y;
    if (!e->h) return;
    long long dn = b->y - t->y, dm = b->x - t->x;
    long long init = (long long)(e->y * 16 - t->y) * dm - 1 + (long long)(t->x + 16) * dn;
    floordivmod(init, dn * 16, &e->x, &e->err);
    floordivmod(dm * 16, dn * 16, &e->xstep, &e->num);
    e->den = (int)(dn * 16);
    float yp = (float)(e->y * 16 - t->y) * (1.0f / 16), xp = (float)(e->x * 16 - t->x) * (1.0f / 16);
    float xs = (float)e->xstep;
    e->oz = g->dozdy * yp + g->dozdx * xp + g->oz[top];
    e->ozs = g->dozdx * xs + g->dozdy; e->oze = g->dozdx;
    e->uz = g->duzdy * yp + g->duzdx * xp + g->uz[top];
    e->uzs = g->duzdx * xs + g->duzdy; e->uze = g->duzdx;
    e->vz = g->dvzdy * yp + g->dvzdx * xp + g->vz[top];
    e->vzs = g->dvzdx * xs + g->dvzdy; e->vze = g->dvzdx;
}

static void edge_step(Edge *e)
{
    e->x += e->xstep; e->y++; e->h--;
    e->oz += e->ozs; e->uz += e->uzs; e->vz += e->vzs;
    e->err += e->num;
    if (e->err >= e->den) {
        e->x++; e->err -= e->den;
        e->oz += e->oze; e->uz += e->uze; e->vz += e->vze;
    }
}

/* Tri_Raster's vertical clip of one edge: advance to row 0, cut at row 199. */
static void edge_clip(Edge *e)
{
    if (e->den == 0 || e->y + e->h < 0) { e->h = 0; return; }
    if (e->y < 0) {
        int n = -e->y;
        float fn = (float)n;
        e->h += e->y;
        e->err += n * e->num;
        int q = e->err / e->den;
        e->x += n * e->xstep + q;
        e->err -= e->den * q;
        float fq = (float)q;
        e->uz = e->uze * fq + e->uzs * fn + e->uz;
        e->y = 0;
        e->vz = e->vze * fq + e->vzs * fn + e->vz;
        e->oz = fq * e->oze + fn * e->ozs + e->oz;
    }
    if (e->y > FL_H) { e->h = 0; return; }
    if (e->y + e->h > FL_H - 1) e->h = FL_H - e->y;
}

typedef struct {
    uint32_t *dst;
    const uint32_t *tex;    /* tile's top image row; texel (u,v) = tex[(127-v)*128 + u] */
} Target;

static inline uint32_t texel(const uint32_t *tex, int u, int v)
{
    u >>= 16; v >>= 16;
    if (u < 0) u = 0; else if (u > FL_TILE - 1) u = FL_TILE - 1;
    if (v < 0) v = 0; else if (v > FL_TILE - 1) v = FL_TILE - 1;
    return tex[(FL_TILE - 1 - v) * FL_TILE + u];
}

/* Span 0x43f000: perspective divide every 8 pixels, 16.16 affine in between (FPU truncation). */
static void span(const Target *t, const Grad *g, const Edge *l, const Edge *r)
{
    int w = r->x - l->x;
    if (w == 0 || l->x > r->x) return;
    uint32_t *d = t->dst + (size_t)l->y * FL_W + l->x;
    int nsub = w >> 3, rem = w & 7;
    if (rem == 0) { nsub--; rem = 8; }
    double z = 1.0 / l->oz;
    double ozr = (double)l->oz + g->dozdx8, uzr = (double)l->uz + g->duzdx8, vzr = (double)l->vz + g->dvzdx8;
    double vl = z * l->vz, ul = z * l->uz;
    double vr = (1.0 / ozr) * vzr, ur = (1.0 / ozr) * uzr;
    int du = 0, dv = 0;
    for (; nsub > 0; nsub--) {
        dv = (int)((vr - vl) * 8192.0);
        du = (int)((ur - ul) * 8192.0);
        vzr += g->dvzdx8; ozr += g->dozdx8; uzr += g->duzdx8;
        int u = (int)(ul * 65536.0) + g->umod, v = (int)(vl * 65536.0) + g->vmod;
        for (int i = 0; i < 8; i++) { d[i] = texel(t->tex, u, v); u += du; v += dv; }
        d += 8;
        vl = vr; ul = ur;
        vr = (1.0 / ozr) * vzr; ur = (1.0 / ozr) * uzr;
    }
    int c = rem - 1;
    if (c) {
        double zr = 1.0 / ((double)r->oz - g->dozdx);
        double ure = ((double)r->uz - g->duzdx) * zr, vre = zr * ((double)r->vz - g->dvzdx);
        dv = (int)((vre - vl) / c * 65536.0);
        du = (int)((ure - ul) / c * 65536.0);
    }
    int u = (int)(ul * 65536.0) + g->umod, v = (int)(vl * 65536.0) + g->vmod;
    for (int i = 0; i <= c; i++) { d[i] = texel(t->tex, u, v); u += du; v += dv; }
}

/* Span_ClipX 0x4083c0 around the span; the edges are restored afterwards. */
static void clipped_span(const Target *t, const Grad *g, Edge *l, Edge *r)
{
    Edge sl = *l, sr = *r;
    int x = l->x, w = r->x - x;
    if (x < 0) {
        float f = (float)-x;
        l->oz = g->dozdx * f + l->oz; l->uz = g->duzdx * f + l->uz; l->vz = f * g->dvzdx + l->vz;
        w += x; l->x = 0;
        if (w > FL_W) w = FL_W;
    } else if (x + w > FL_W) {
        w = FL_W - x;
    }
    if (w >= 1) {
        if (r->x > FL_W) {
            float f = (float)(FL_W - r->x);
            r->oz = g->dozdx * f + r->oz; r->uz = g->duzdx * f + r->uz; r->vz = f * g->dvzdx + r->vz;
            r->x = FL_W;
        }
        span(t, g, l, r);
    }
    *l = sl; *r = sr;
}

/* Tri_Raster 0x408500. */
static void raster(const Target *t, const Vert *v)
{
    int y0 = v[0].y, y1 = v[1].y, y2 = v[2].y;
    if ((y0 < 0 && y1 < 0 && y2 < 0) || (y0 > FX_H && y1 > FX_H && y2 > FX_H) ||
        (v[0].x < 0 && v[1].x < 0 && v[2].x < 0) || (v[0].x > FX_W && v[1].x > FX_W && v[2].x > FX_W))
        return;
    int top, mid, bot, midc, botc;
    if (y0 < y1) {
        if (y2 < y0) { top = 2; mid = 0; bot = 1; midc = 0; botc = 1; }
        else {
            top = 0;
            if (y1 < y2) { mid = 1; bot = 2; midc = 1; botc = 2; }
            else         { mid = 2; bot = 1; midc = 2; botc = 1; }
        }
    } else if (y2 < y1) { top = 2; mid = 1; bot = 0; midc = 1; botc = 0; }
    else {
        top = 1;
        if (y0 < y2) { mid = 0; bot = 2; midc = 3; botc = 2; }
        else         { mid = 2; bot = 0; midc = 2; botc = 3; }
    }
    Grad g;
    grad_init(&g, v);
    Edge tb, tm, mb;
    edge_init(&tb, &g, v, top, bot);
    edge_init(&tm, &g, v, top, mid);
    edge_init(&mb, &g, v, mid, bot);
    int mid_left = botc <= midc;

    edge_clip(&tm);
    edge_clip(&tb);
    Edge *l = mid_left ? &tm : &tb, *r = mid_left ? &tb : &tm;
    for (int n = tm.h; n > 0; n--) {
        if (l->y >= 0 && l->y < FL_H) clipped_span(t, &g, l, r);
        edge_step(&tm); edge_step(&tb);
    }
    edge_clip(&mb);
    edge_clip(&tb);
    l = mid_left ? &mb : &tb; r = mid_left ? &tb : &mb;
    for (int n = mb.h; n > 0; n--) {
        if (l->y >= 0 && l->y < FL_H) clipped_span(t, &g, l, r);
        edge_step(&mb); edge_step(&tb);
    }
}

/* ---- FreeLook_Render 0x405710 ----------------------------------------------------- */

static void cube_vertex(int f, int r, int c, double p[3])
{
    double a = -CUBE_HALF + CUBE_STEP * c, b = CUBE_HALF - CUBE_STEP * r, h = CUBE_HALF;
    switch (f) {
    case 0:  p[0] = a;  p[1] = b;  p[2] = h;  break;   /* +Z forward */
    case 1:  p[0] = -h; p[1] = b;  p[2] = a;  break;   /* -X */
    case 2:  p[0] = -a; p[1] = b;  p[2] = -h; break;   /* -Z */
    case 3:  p[0] = h;  p[1] = b;  p[2] = -a; break;   /* +X */
    case 4:  p[0] = a;  p[1] = -h; p[2] = b;  break;   /* -Y ceiling (world +Y is down) */
    default: p[0] = a;  p[1] = h;  p[2] = -b; break;   /* +Y floor */
    }
}

static int in_guard(const Vert *v)
{
    for (int i = 0; i < 3; i++)
        if (v[i].x < -0x2800 || v[i].x > 0x3c00 || v[i].y < -0x2800 || v[i].y > 0x2800) return 0;
    return 1;
}

void fl_render(const Freelook *fl, uint32_t *dst)
{
    double m[3][3];
    view_matrix(fl, m);
    double kproj = (FX_W / 2) / tan(fl->half_fov * M_PI / 92160.0);
    Vert pv[6][25];
    int st[6][25];
    for (int f = 0; f < 6; f++)
        for (int i = 0; i < 25; i++) {
            double p[3];
            cube_vertex(f, i / 5, i % 5, p);
            st[f][i] = project(p, m, kproj, &pv[f][i]);
        }
    memset(dst, 0, (size_t)FL_W * FL_H * sizeof *dst);
    Target t = { dst, NULL };
    for (int f = 0; f < 6; f++)
        for (int q = 0; q < 16; q++) {
            int v0 = (q / 4) * 5 + q % 4;
            t.tex = tile_top(fl, f * 16 + q);
            /* T1: v(0,0) v+6(127,127) v+1(127,0);  T2: v(0,0) v+5(0,127) v+6(127,127). */
            static const int tri[2][3] = { { 0, 6, 1 }, { 0, 5, 6 } };
            static const float uv[2][3][2] = { { { 0, 0 }, { TEXMAX, TEXMAX }, { TEXMAX, 0 } },
                                               { { 0, 0 }, { 0, TEXMAX }, { TEXMAX, TEXMAX } } };
            for (int k = 0; k < 2; k++) {
                Vert v[3];
                int vis = 0;
                for (int j = 0; j < 3; j++) {
                    int i = v0 + tri[k][j];
                    v[j] = pv[f][i];
                    v[j].u = uv[k][j][0]; v[j].v = uv[k][j][1];
                    vis |= st[f][i] == 2;
                }
                if (vis && in_guard(v)) raster(&t, v);
            }
        }
}

/* ---- FreeLook_Frame 0x412ab0 ------------------------------------------------------- */

/* Save the strip rows a kind-2 record can touch (engine rows y..y+h-1) for the restore. */
typedef struct { int row0, rows; size_t off; } SaveRun;

void fl_frame(Freelook *fl, const SpriteCtx *spr, const FgBlock *b, uint32_t *dst, int draw)
{
    if (b) {
        /* Pass 1, mask 1: permanent paint. Hub lock (0x412e40): in LEVEL00 a kind-1 record with
         * actor id > 1 and > 2*HighestLevel-1 ends the pass. */
        for (int i = 0; i < b->count; i++) {
            const FgRecord *r = &b->rec[i];
            if (!(r->kind & OBJ_RAIL)) continue;
            if (fl->hub && r->actor > 1 && r->actor > 2 * fl->highest_level - 1) break;
            sprite_draw(spr, r, fl->strip, FL_TILE, FL_STRIP_H);
        }
    }
    if (!draw) return;

    SaveRun runs[FG_MAX_RECS];
    int nruns = 0;
    size_t used = 0;
    if (b) {
        for (int i = 0; i < b->count; i++) {
            const FgRecord *r = &b->rec[i];
            if (!(r->kind & OBJ_ENEMY) || (r->kind & OBJ_RAIL) || !r->pixels) continue;
            int lo = r->y, hi = r->y + r->h;          /* engine rows [lo, hi) */
            if (lo < 0) lo = 0;
            if (hi > FL_STRIP_H) hi = FL_STRIP_H;
            if (hi <= lo) continue;
            int row0 = FL_STRIP_H - hi, rows = hi - lo;  /* array rows */
            size_t need = used + (size_t)rows * FL_TILE;
            if (need > fl->save_cap) {
                size_t cap = fl->save_cap ? fl->save_cap : 1 << 16;
                while (cap < need) cap *= 2;
                uint32_t *p = realloc(fl->save, cap * sizeof *p);
                if (!p) break;
                fl->save = p; fl->save_cap = cap;
            }
            memcpy(fl->save + used, fl->strip + (size_t)row0 * FL_TILE, (size_t)rows * FL_TILE * sizeof(uint32_t));
            runs[nruns++] = (SaveRun){ row0, rows, used };
            used = need;
            sprite_draw(spr, r, fl->strip, FL_TILE, FL_STRIP_H);
        }
    }
    fl_render(fl, dst);
    if (b)
        for (int i = 0; i < b->count; i++)
            if (b->rec[i].kind & OBJ_EFFECT) sprite_draw(spr, &b->rec[i], dst, FL_W, FL_H);
    /* Spr_RestoreBackground 0x40daa0, newest first. */
    while (nruns--) {
        SaveRun *s = &runs[nruns];
        memcpy(fl->strip + (size_t)s->row0 * FL_TILE, fl->save + s->off, (size_t)s->rows * FL_TILE * sizeof(uint32_t));
    }
}
