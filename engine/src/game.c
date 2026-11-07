/* game.c - see game.h. Section numbers refer to docs/COMBAT_SPEC.md. */
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 200

#define N_LISTS     5          /* sprite-set lists: 0 explosions, 1 HUD, 2 crosshair, 3 gun/ACT, 4 streaks */
#define LIST_MAX    32
#define SET_FRAMES  64
#define N_SHOTS     8
#define N_LOOPS     8
#define N_STREAKS   8
#define CLS_SIZE    0x54
#define ACT_SIZE    0x28
#define HEAT_MAX    240
#define HEAT_LOCK   (-120)
#define ST_GONE     0x7f

typedef struct {
    int      hint, count, expanded;
    FgRecord fr[SET_FRAMES];    /* frame descriptors; pixels point into `data` */
    int      step[SET_FRAMES + 2];
    uint8_t *data;
} SprSet;

typedef struct { const uint8_t *pcm; size_t n; } Sound;

typedef struct {
    int16_t x0, y0, w, h;       /* bbox x0,y0 and draw w,h last frame */
    int32_t frame_seen;
    float   dist2;
    float   cx, cy;             /* free-look: bbox centre last frame */
    int8_t  cv;                 /* ... and it was on screen */
} ActorPrev;

typedef struct {
    int x, y, hit, active, actor;
} Shot;

typedef struct { int actor, sfx; long until; } LoopSlot;
typedef struct { int life; float sx, sy, ex, ey; int ring; int d3; double p[2][3]; } Streak;   /* d3: p = world S, E (type 1) */

/* A camera view as Mat_LookAt builds it: orthonormal basis, roll, and the projection scale. */
typedef struct { double c[3], f[3], r[3], u[3], cr, sr, k; } View;

struct Game {
    GameOpts  o;
    GameStats st;
    int       loaded;
    const SpriteCtx *spr;
    char      stem[9];

    /* Level tables. */
    uint8_t  *cls;  int n_cls;
    uint8_t  *cls_avoid;                    /* per class: kill branch leads to a restart clip */
    uint8_t  *act;  int n_act;              /* raw table28 */
    int8_t   *state;                        /* table28 +3, runtime */
    const SprSet **expl;                    /* per actor: explosion set picked at death */
    ActorPrev *prev;
    float    *dist2;
    SprSet   *lists[N_LISTS][LIST_MAX];
    int       n_list[N_LISTS];
    uint8_t  *snd_data; Sound *snd; int n_snd;
    int       act_start;                    /* header 0x88 */

    /* Segment. */
    uint8_t   rec[L96_SEGMENT_RECORD];
    int       seg, type, have_seg;

    /* Player (g_view gameplay part). */
    int       xhair_x, xhair_y, aim_x, aim_y;
    uint32_t  buttons;
    int       health, kills;
    InputState in;

    /* Weapons. */
    int       heat, heat_max, overheated, shot_idx;
    uint32_t  shot_hist;
    long      shots_fired;
    int       semi_armed, charge_armed, charge_gap, charge_count;
    uint32_t  act_anim; int act_ammo, act_req, act_chain_n, act_chain_hit, act_chain_timer;
    int       chain_x, chain_y;
    Shot      shots[N_SHOTS], act_shot;

    /* HUD / effects. */
    int       xh_counter;
    int       msg_id, msg_dur, msg_on, msg_off, msg_on_cnt, msg_off_cnt, msg_showing;
    int       ammo_frame;
    int       flash, shake;
    int       branch_req;
    int       death_latch, dead, death_timer, latched_prev;

    /* Health warnings (§7.2). */
    long      hw_tick, hw_t20, hw_t50, hw_t80; int hw_band;
    LoopSlot  loops[N_LOOPS];
    Streak    streaks[N_STREAKS];

    long      frame_no, scene_frame;
    LookInfo  look;
    /* Frame-header camera (type 1): current, and as latched by the previous Enemy_Update (6.1). */
    double    cam[3], lk[3], roll;  int cam_have;
    double    lat_cam[3], lat_lk[3], lat_roll;  int lat_have;
    double    pred_cam[3], pred_lk[3], pred_roll;
    int       fl_yaw, fl_pitch, fl_have;     /* free-look view at the previous Enemy_Update */
    float     fl_sx, fl_sy;                  /* screen shift of a static point from the view change */
    /* Presentation layers (game_frame_post): scene = frame without the HUD; ovl = brackets + gauges
     * (alpha 255 where drawn); xb / bb = the crosshair and the gauges while the frame is composed. */
    uint32_t *lay_scene, *lay_ovl;
    int       lay_split;
    FgRecord  xh_rec;  int xh_valid;         /* crosshair frame drawn this tick (live redraw at display rate) */
    uint32_t  xb[W * H], bb[W * H];
};

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t  rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static int      rnd(void) { return scene_rand(); }
static int      pct(void) { return rnd() * 100 / 0x7fff; }

/* ---- life cycle ---------------------------------------------------------- */

Game *game_new(const GameOpts *o)
{
    Game *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->o = *o;
    if (g->o.difficulty < 0 || g->o.difficulty > 2) g->o.difficulty = 0;
    g->health = 10000;
    g->xhair_x = W / 2; g->xhair_y = H / 2;
    g->in.x = W / 2; g->in.y = H / 2;
    g->heat = g->heat_max = HEAT_MAX;
    g->shot_idx = -1;
    g->semi_armed = g->charge_armed = g->charge_gap = 1;
    return g;
}

static void level_free(Game *g)
{
    for (int l = 0; l < N_LISTS; l++)
        for (int i = 0; i < g->n_list[l]; i++) { free(g->lists[l][i]->data); free(g->lists[l][i]); }
    memset(g->n_list, 0, sizeof g->n_list);
    free(g->cls); free(g->cls_avoid); g->cls_avoid = NULL; free(g->act); free(g->state); free(g->expl); free(g->prev); free(g->dist2);
    free(g->snd_data); free(g->snd);
    g->cls = g->act = NULL; g->state = NULL; g->expl = NULL; g->prev = NULL; g->dist2 = NULL;
    g->snd_data = NULL; g->snd = NULL;
    g->n_cls = g->n_act = g->n_snd = 0;
    g->loaded = 0;
}

void game_free(Game *g)
{
    if (!g) return;
    level_free(g);
    free(g);
}

static int read_at(const L96File *f, uint64_t off, void *dst, size_t n)
{
    return l96_read(f, off, dst, n);
}

/* Does [off, off + n) lie inside the file? (Header counts are untrusted: check before allocating.) */
static int fits(const L96File *f, uint64_t off, uint64_t n)
{
    return !f->rd.size || (off <= f->rd.size(f->rd.ctx) && n <= f->rd.size(f->rd.ctx) - off);
}

static int load_sets(Game *g, const L96File *f, uint32_t off, uint32_t n)
{
    uint64_t pos = off;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t h[16];
        if (read_at(f, pos, h, 16) != 0) return -1;
        pos += 16;
        uint32_t count = rd32(h + 4), size = rd32(h + 8);
        int type = rd16(h + 12), hint = rd16(h + 14);
        if (!fits(f, pos, size)) return -1;
        uint8_t *data = malloc(size ? size : 1);
        if (!data || read_at(f, pos, data, size) != 0) { free(data); return -1; }
        pos += size;
        int list = type < 0 ? -type : type;
        if (list >= N_LISTS || g->n_list[list] >= LIST_MAX || count > SET_FRAMES ||
            (size_t)count * FG_REC_SIZE > size) { free(data); continue; }
        SprSet *s = calloc(1, sizeof *s);
        if (!s) { free(data); return -1; }
        s->data = data; s->hint = hint; s->count = (int)count; s->expanded = type < 0;
        for (uint32_t k = 0; k < count; k++) {
            const uint8_t *r = data + FG_REC_SIZE * k;
            FgRecord *d = &s->fr[k];
            int32_t doff = (int32_t)rd32(r + 4), dsz = (int32_t)rd32(r + 8);
            d->kind = OBJ_EFFECT;
            d->x = rd16(r + 0x10); d->y = rd16(r + 0x12); d->w = rd16(r + 0x14); d->h = rd16(r + 0x16);
            d->tint = r[0x48];
            if (doff > 0 && dsz > 0 && (size_t)doff + (size_t)dsz <= size) {
                d->pixels = data + doff; d->pixels_size = (size_t)dsz;
            }
        }
        if (list == 3) {   /* Sprites_Load: step table S[0]=0, S[i] = S[i-1] + 2^(c+1-i) */
            int lim = s->count + s->expanded, inc = 1 << (s->count < 30 ? s->count : 30);
            s->step[0] = 0;
            for (int k = 1; k < lim && k < SET_FRAMES + 2; k++) { s->step[k] = s->step[k - 1] + inc; inc >>= 1; }
        }
        g->lists[list][g->n_list[list]++] = s;
    }
    return 0;
}

int game_level_load(Game *g, const L96File *f, const SpriteCtx *spr)
{
    level_free(g);
    g->spr = spr;
    g->have_seg = 0;
    memcpy(g->stem, f->name, 8); g->stem[8] = 0;
    uint8_t h[0x94];
    if (read_at(f, 0, h, sizeof h) != 0) return -1;
    uint32_t n_snd = rd32(h + 0x1c), n_spr = rd32(h + 0x20), flag24 = rd32(h + 0x24);
    uint32_t n54 = rd32(h + 0x34), n28 = rd32(h + 0x38);
    g->act_start = h[0x88];
    if (!fits(f, rd32(h + 0x64), (uint64_t)n54 * CLS_SIZE) || !fits(f, rd32(h + 0x68), (uint64_t)n28 * ACT_SIZE) ||
        n54 > INT32_MAX || n28 > INT32_MAX) return -1;
    g->n_cls = (int)n54; g->n_act = (int)n28;
    g->cls   = calloc(n54 ? n54 : 1, CLS_SIZE);
    g->act   = calloc(n28 ? n28 : 1, ACT_SIZE);
    g->state = calloc(n28 ? n28 : 1, 1);
    g->expl  = calloc(n28 ? n28 : 1, sizeof *g->expl);
    g->prev  = calloc(n28 ? n28 : 1, sizeof *g->prev);
    g->dist2 = calloc(n28 ? n28 : 1, sizeof *g->dist2);
    if (!g->cls || !g->act || !g->state || !g->expl || !g->prev || !g->dist2) return -1;
    if ((n54 && read_at(f, rd32(h + 0x64), g->cls, (size_t)n54 * CLS_SIZE) != 0) ||
        (n28 && read_at(f, rd32(h + 0x68), g->act, (size_t)n28 * ACT_SIZE) != 0)) return -1;
    if (flag24 == 0 && n54 != 0)       /* Level_Load rewrite: byte2 = byte0, byte0 -= n54/2, byte1 = byte0 */
        for (uint32_t i = 0; i < n28; i++) {
            uint8_t *a = g->act + ACT_SIZE * i;
            a[2] = a[0]; a[0] = (uint8_t)(a[0] - n54 / 2); a[1] = a[0];
        }
    for (uint32_t i = 0; i < n28; i++) g->state[i] = ST_GONE;
    g->cls_avoid = calloc(n54 ? n54 : 1, 1);
    if (!g->cls_avoid) return -1;
    for (uint32_t i = 0; i < n54; i++) {
        int kb = (int8_t)g->cls[CLS_SIZE * i + 0x34];
        if (kb >= 0 && (uint32_t)kb < f->n_segments && f->segments[kb].type == 0 && f->segments[kb].next == 0)
            g->cls_avoid[i] = 1;
    }

    /* Sounds: n x {0x2c header (u32 size at +8), u8 mono PCM}. */
    if (n_snd) {
        uint32_t off = rd32(h + 0x4c);
        uint64_t total = 0;
        if (!fits(f, off, (uint64_t)n_snd * 0x2c)) return -1;
        g->snd = calloc(n_snd, sizeof *g->snd);
        if (!g->snd) return -1;
        size_t cap = 0;
        uint32_t *sizes = calloc(n_snd, sizeof *sizes);
        if (!sizes) return -1;
        for (uint32_t i = 0; i < n_snd; i++) {
            uint8_t sh[0x2c];
            if (read_at(f, off + total, sh, sizeof sh) != 0) { free(sizes); return -1; }
            sizes[i] = rd32(sh + 8);
            total += 0x2c + sizes[i];
            cap += sizes[i];
            if (!fits(f, off, total)) { free(sizes); return -1; }
        }
        g->snd_data = malloc(cap ? cap : 1);
        if (!g->snd_data) { free(sizes); return -1; }
        size_t at = 0; uint64_t pos = off;
        for (uint32_t i = 0; i < n_snd; i++) {
            if (read_at(f, pos + 0x2c, g->snd_data + at, sizes[i]) != 0) { free(sizes); return -1; }
            g->snd[i].pcm = g->snd_data + at; g->snd[i].n = sizes[i];
            at += sizes[i]; pos += 0x2c + sizes[i];
        }
        free(sizes);
        g->n_snd = (int)n_snd;
    }
    if (n_spr && load_sets(g, f, rd32(h + 0x50), n_spr) != 0) return -1;
    g->loaded = 1;
    return 0;
}

/* ---- small helpers --------------------------------------------------------- */

static const uint8_t *cls_of(const Game *g, int id)
{
    if (id < 0 || id >= g->n_act) return NULL;
    int c = g->act[ACT_SIZE * id + g->o.difficulty];
    return c < g->n_cls ? g->cls + CLS_SIZE * c : NULL;
}

static const SprSet *set_get(const Game *g, int list, int i)
{
    return i >= 0 && i < g->n_list[list] ? g->lists[list][i] : NULL;
}

static int rec_skipped(const Game *g, const FgRecord *r)
{
    return r->kind == OBJ_OVERLAY || (g->type != SEG_RAIL && r->kind == OBJ_RAIL);
}

static int bbox_nonzero(const FgRecord *r) { return r->bbox[0] || r->bbox[1] || r->bbox[2] || r->bbox[3]; }

/* Obj_IsTargetable 0x4021a0 (§4.2). */
static int targetable(const Game *g, const FgRecord *r, int mode)
{
    const uint8_t *c = cls_of(g, r->actor);
    if (!c || g->state[r->actor] >= -0x40 || !(r->flags & 0x80000000u)) return 0;
    if (!(r->kind == OBJ_ENEMY || (g->type == SEG_RAIL && r->kind == OBJ_RAIL))) return 0;
    int s = c[0x1a], size = r->w >= s || r->h >= s;
    switch (mode) {
    case 0: case 2: return s < 0xfe && size;
    case 1:         return s < 0xfe;
    default:        return s != 0xff && size;
    }
}

static int box_grow(const uint8_t *c)
{
    switch (c[1] & 0x3f) { case 1: return 1; case 2: return 2; case 3: return -1; case 4: return -2; default: return 0; }
}

/* ---- sound ------------------------------------------------------------------- */

static void sfx(Game *g, int id, int pan, int vol)
{
    if (id == 0xff || id < 0 || id >= g->n_snd) return;
    g->st.sfx++;
    if (g->o.sfx) g->o.sfx(g->o.sfx_ud, g->snd[id].pcm, g->snd[id].n, vol, pan);
}

static int pick(const uint8_t *pool, int n)
{
    while (n > 0 && pool[n - 1] == 0xff) n--;
    if (n <= 0) return 0xff;
    int i = rnd() / (0x7fff / n);
    return pool[i < n ? i : n - 1];
}

static long snd_frames(const Game *g, int id)
{
    return id >= 0 && id < g->n_snd ? (long)(g->snd[id].n * 15 / 22050) + 1 : 0;
}

static int screen_pan(const FgRecord *r) { return ((r->bbox[0] + r->w / 2) * 5 - 800) * 2; }

static int size_vol(const FgRecord *r, const uint8_t *c)
{
    int m = r->w > r->h ? r->w : r->h;
    return m > 0 && m < 30 && c[0x1a] != 0xff ? -300 / m : 0;
}

static void loop_stop(Game *g, int actor)
{
    for (int i = 0; i < N_LOOPS; i++) if (g->loops[i].actor == actor) g->loops[i].until = 0;
}

static void sfx_enemy_loop(Game *g, const uint8_t *c, const FgRecord *r, int pan, int big)
{
    int free_slot = -1;
    for (int i = 0; i < N_LOOPS; i++) {
        LoopSlot *l = &g->loops[i];
        if (l->until > g->frame_no) { if (l->actor == r->actor) return; }   /* playing: pan update only */
        else if (free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;
    int id = big ? pick(c + 0x30, 2) : pick(c + 0x2d, 3);
    if (id == 0xff) return;
    for (int i = 0; i < N_LOOPS; i++) if (g->loops[i].until > g->frame_no && g->loops[i].sfx == id) return;
    g->loops[free_slot] = (LoopSlot){ r->actor, id, g->frame_no + snd_frames(g, id) };
    sfx(g, id, 3 * pan, 0);
}

static void sfx_health_warning(Game *g, int hp)
{
    g->hw_tick++;
    if (hp < 21 && g->hw_band > 20 && g->hw_tick - g->hw_t20 > 44) {
        sfx(g, g->rec[0x4d], 0, 0); g->hw_band = hp; g->hw_t20 = g->hw_tick;
    } else if (hp < 51 && g->hw_band > 50 && g->hw_tick - g->hw_t50 > 44) {
        sfx(g, g->rec[0x4c], 0, 0); g->hw_band = hp; g->hw_t50 = g->hw_tick;
    } else if (hp > 79 && g->hw_band < 80 && g->hw_tick - g->hw_t80 > 44) {
        sfx(g, g->rec[0x4e], 0, 0); g->hw_band = hp; g->hw_t80 = g->hw_tick;
    }
}

/* ---- HUD message / effects ------------------------------------------------------ */

static void hud_message(Game *g, int id, int dur, int on, int off)
{
    if (g->msg_id != id) { g->msg_showing = 1; g->msg_on_cnt = g->msg_off_cnt = 0; }
    g->msg_id = id; g->msg_dur = dur; g->msg_on = on; g->msg_off = off;
}

static void fx_flash(Game *g, int f, int s) { g->flash = f; g->shake = s; }

/* ---- checkpoints ------------------------------------------------------------------ */

static void weapon_reset(Game *g)
{
    g->shot_hist = 0; g->act_anim = 0;
    g->heat = g->heat_max = HEAT_MAX;
}

/* Game_ResetForScene 0x405f30 (§4.1). */
static void reset_for_scene(Game *g)
{
    for (int i = 0; i < g->n_act; i++) {
        const uint8_t *c = cls_of(g, i);
        if (!c) { g->state[i] = ST_GONE; continue; }
        g->state[i] = pct() > c[0x1b] ? ST_GONE : (int8_t)(-0x40 - c[0x1f]);
        g->expl[i] = NULL;
    }
    if (g->o.verbose > 2) { int n = 0; for (int i = 0; i < g->n_act; i++) n += g->state[i] < -0x40; fprintf(stderr, "reset %s:%d alive %d/%d\n", g->stem, g->seg, n, g->n_act); }
    memset(g->loops, 0, sizeof g->loops);
    g->hw_band = 100; g->hw_t20 = g->hw_t50 = g->hw_t80 = -0x8000;
    memset(g->streaks, 0, sizeof g->streaks); g->cam_have = g->lat_have = 0;
    weapon_reset(g);
    g->act_ammo = g->act_start; g->act_req = 0; g->shots_fired = 0;
    g->health = 10000; g->kills = 0;
    hud_message(g, 0, 0, 0, 0);
    g->death_latch = g->dead = 0; g->death_timer = 0; g->latched_prev = 0;
}

void game_segment_start(Game *g, int seg, const uint8_t *rec)
{
    int first = !g->have_seg;           /* first segment of this file (--from-seg N): roll the actors too */
    memcpy(g->rec, rec, sizeof g->rec);
    g->seg = seg;
    g->type = (int32_t)rd32(rec);
    g->have_seg = 1;
    g->scene_frame = 0;
    if (g->loaded && (seg == 0 || rec[0x55] || first)) reset_for_scene(g);
}

/* ---- actors (§4) ---------------------------------------------------------------------- */

/* Actor_PickExplosion 0x403910 (§4.4). */
static void pick_explosion(Game *g, const FgRecord *r)
{
    const uint8_t *c = cls_of(g, r->actor);
    if (!c) return;
    int size = r->w > r->h ? r->w : r->h, n = c[5] > 0x14 ? 0x14 : c[5];
    const SprSet *cand[8], *best = NULL;
    int nc = 0;
    for (int i = 0; i < n && nc < 8; i++) {
        const SprSet *s = set_get(g, 0, c[6 + i]);
        if (s && size < s->hint && s->hint < 2 * size) cand[nc++] = s;
    }
    if (nc > 0) best = cand[nc > 1 ? rnd() % nc : 0];
    if (!best) {
        int d = 0x7fffffff;
        for (int i = 0; i < n; i++) {
            const SprSet *s = set_get(g, 0, c[6 + i]);
            if (s && size < s->hint && s->hint - size < d) { d = s->hint - size; best = s; }
        }
    }
    if (!best) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const SprSet *s = set_get(g, 0, c[6 + i]);
            if (s && m < s->hint) { m = s->hint; best = s; }
        }
    }
    g->expl[r->actor] = best;
}

static void actor_remove(Game *g, FgRecord *r, const uint8_t *c)
{
    r->pixels = NULL; r->pixels_size = 0;
    g->state[r->actor] = ST_GONE;
    if ((int8_t)c[0x34] != -1) {
        g->branch_req = (int8_t)c[0x34];
        if (g->o.verbose) fprintf(stderr, "game: actor %d removed -> kill branch %d\n", r->actor, g->branch_req);
    }
    memset(&g->prev[r->actor], 0, sizeof g->prev[r->actor]);
}

/* Actor_UpdateStates 0x403a80 (§4.3). */
static void actors_update(Game *g, FgBlock *b)
{
    for (int i = 0; i < b->count; i++) {
        FgRecord *r = &b->rec[i];
        if (rec_skipped(g, r)) continue;
        const uint8_t *c = cls_of(g, r->actor);
        if (!c) continue;
        int id = r->actor, st = g->state[id];
        if (st >= -0x40 && (r->flags & 0x100) && pct() <= c[0x1b]) st = g->state[id] = (int8_t)(-0x40 - c[0x1f]);
        const uint8_t *a = g->act + ACT_SIZE * id;
        int link = (int32_t)rd32(a + 8), mode = (int32_t)rd32(a + 12);
        if (st == 0) {
            if (link > 0 && link < g->n_act && mode == 1 && g->state[link] < 0) {
                g->state[link]++;
                if (g->state[link] == -0x40) {
                    const uint8_t *pc = cls_of(g, link);
                    if (link == 0xfa) {
                        if (pc && (int8_t)pc[0x34] != -1) {
                            g->branch_req = (int8_t)pc[0x34];
                            if (g->o.verbose) fprintf(stderr, "game: linked parent 250 destroyed -> branch %d\n", g->branch_req);
                        }
                    }
                    else g->state[link] = 0;
                }
            }
        } else if (st < -0x40) {
            if (g->prev[id].frame_seen - g->frame_no != -1 && link > 0 && link < g->n_act && mode == 0 &&
                g->state[link] >= -0x40)
                g->state[id] = ST_GONE;
        }
    }
    for (int i = 0; i < b->count; i++) {
        FgRecord *r = &b->rec[i];
        if (rec_skipped(g, r)) continue;
        const uint8_t *c = cls_of(g, r->actor);
        if (!c) continue;
        int id = r->actor;
        int seen = g->prev[id].frame_seen - g->frame_no == -1;
        if (r->flags & 0x40) g->state[id] = ST_GONE;
        int st = g->state[id];
        if (st == ST_GONE) { r->pixels = NULL; r->pixels_size = 0; }
        else if (st < 0) { if (st >= -0x40) g->state[id]++; }
        else if (c[0x1a] == 0xff || (seen && bbox_nonzero(r))) {
            const SprSet *e = g->expl[id];
            const FgRecord *fr = e && st < e->count ? &e->fr[st] : NULL;
            if (st == 0) {
                loop_stop(g, id);
                sfx(g, pick(c + 0x20, 5), screen_pan(r), size_vol(r, c));
                fx_flash(g, 2, 10);
            }
            if (!fr || !fr->pixels) actor_remove(g, r, c), st = ST_GONE;
            else {
                if (g->type == SEG_FREELOOK) {
                    /* On the bbox centre. The fixed-up bbox is top-down screen space while sprites
                     * are drawn bottom-up, so flip y (the effect is drawn in screen space). */
                    r->x = (int16_t)((r->bbox[0] + r->bbox[2]) / 2 - fr->w / 2);
                    r->y = (int16_t)(H - 1 - (r->bbox[1] + r->bbox[3]) / 2 - fr->h / 2);
                } else {
                    r->x = (int16_t)(r->x + (r->w / 2 - fr->w / 2));
                    r->y = (int16_t)(r->y + (r->h / 2 - fr->h / 2));
                }
                r->pixels = fr->pixels; r->pixels_size = fr->pixels_size;
                r->w = fr->w; r->h = fr->h; r->tint = fr->tint;
                r->kind = OBJ_EFFECT;
                g->state[id]++;
            }
        } else { actor_remove(g, r, c); st = ST_GONE; }
        if (st > -0x41 && st != ST_GONE)          /* tracers follow a falling target */
            for (int k = 0; k < N_SHOTS; k++)
                if (g->shots[k].actor == id) {
                    g->shots[k].x = (r->bbox[0] + r->bbox[2]) / 2;
                    g->shots[k].y = (r->bbox[1] + r->bbox[3]) / 2;
                }
        g->prev[id].frame_seen = (int32_t)g->frame_no;
    }
}

void game_frame_pre(Game *g, FgBlock *b, const uint8_t *hdr)
{
    if (!g->loaded || !g->have_seg || !b) return;
    if (hdr && g->type != SEG_FREELOOK && g->type != SEG_PANO) {
        /* Obj_ProjectAll 0x403780: flip the bbox to top-down, store |cam - pos|^2. */
        int vh = (int32_t)rd32(g->rec + 0x40);
        int32_t cam[3] = { (int32_t)rd32(hdr + 4), (int32_t)rd32(hdr + 8), (int32_t)rd32(hdr + 12) };
        if (g->type == SEG_RAIL) {
            for (int i = 0; i < 3; i++) { g->cam[i] = cam[i]; g->lk[i] = (int32_t)rd32(hdr + 16 + 4 * i); }
            g->roll = (int32_t)rd32(hdr + 0x1c); g->cam_have = 1;
        }
        for (int i = 0; i < b->count; i++) {
            FgRecord *r = &b->rec[i];
            int16_t y0 = r->bbox[1];
            r->bbox[1] = (int16_t)(vh - r->bbox[3] - 1);
            r->bbox[3] = (int16_t)(vh - y0 - 1);
            if (r->actor >= 0 && r->actor < g->n_act) {
                double dx = (double)cam[0] - r->pos[0], dy = (double)cam[1] - r->pos[1], dz = (double)cam[2] - r->pos[2];
                g->dist2[r->actor] = (float)(dx * dx + dy * dy + dz * dz);
            }
        }
    }
    actors_update(g, b);
}

/* ---- hit testing (§5.5) ------------------------------------------------------------------ */

/* Spr_HitTestPixel 0x410fa0: is (px, py) (bottom-up) on an opaque pixel of the record's RLE? */
static int hit_pixel(const FgRecord *r, int px, int py)
{
    const uint8_t *s = r->pixels, *end = s + r->pixels_size;
    if (!s) return 0;
    int row = r->y;
    while (row < py) {
        if (s >= end) return 0;
        int8_t op = (int8_t)*s;
        if (op < 0) s += 1 + (-op);
        else if (op > 0) s++;
        else {
            if (s + 3 > end) return 0;
            row += s[1] << 8 | s[2]; s += 3;
            if (row == r->y + r->h) return 0;
        }
    }
    int x = r->x;
    while (s < end) {
        int8_t op = (int8_t)*s;
        if (op < 0) {
            int n = -op;
            if (x <= px && px <= x + n) return 1;
            x += n; s += 1 + n;
        } else if (op > 0) {
            x += op; s++;
            if (px < x) return 0;
        } else {
            if (s + 3 > end || (s[1] << 8 | s[2]) != 0) return 0;
            s += 3;
        }
    }
    return 0;
}

static void grown_box(const FgRecord *r, const uint8_t *c, int *x0, int *y0, int *x1, int *y1)
{
    int gr = box_grow(c);
    *x0 = r->bbox[0] - gr; *x1 = r->bbox[2] + gr; *y0 = r->bbox[1] - gr; *y1 = r->bbox[3] + gr;
    if (*x1 < *x0) *x0 = *x1 = (*x0 + *x1) / 2;
    if (*y1 < *y0) *y0 = *y1 = (*y0 + *y1) / 2;
}

/* Obj_HitTestPoint 0x403ff0. */
static int hit_point(const Game *g, const FgRecord *r, int sx, int sy, int px, int py)
{
    const uint8_t *c = cls_of(g, r->actor);
    int x0, y0, x1, y1;
    grown_box(r, c, &x0, &y0, &x1, &y1);
    if (sx < x0 || sx > x1 || sy < y0 || sy > y1) return 0;
    if ((c[1] & 0x80) && g->type != SEG_FREELOOK) return hit_pixel(r, px, py);
    return 1;
}

static void kill_actor(Game *g, const FgRecord *r, int new_state)
{
    g->state[r->actor] = (int8_t)new_state;
    g->kills++;
    g->st.kills++;
    pick_explosion(g, r);
}

static void sfx_enemy_hit(Game *g, const FgRecord *r)
{
    const uint8_t *c = cls_of(g, r->actor);
    sfx(g, pick(c + 0x2b, 2), screen_pan(r), size_vol(r, c));
}

/* Weapon_HitScan 0x404140, plus Shot_Spawn for a miss. */
static void hitscan(Game *g, FgBlock *b, int idx, int sx, int sy, int px, int py, int gp)
{
    for (int i = b ? b->count - 1 : -1; i >= 0; i--) {
        FgRecord *r = &b->rec[i];
        if (rec_skipped(g, r) || !targetable(g, r, 1) || !hit_point(g, r, sx, sy, px, py)) continue;
        int id = r->actor;
        g->state[id]++;
        if (g->state[id] < -0x40) sfx_enemy_hit(g, r);
        else {
            kill_actor(g, r, -gp);
            if (g->o.verbose > 1) fprintf(stderr, "game: %s:%d (type %d) frame %ld kill actor %d at %d,%d\n", g->stem, g->seg, g->type, g->frame_no, id, sx, sy);
        }
        g->shots[idx] = (Shot){ g->aim_x, g->aim_y, 1, 1, id };
        g->st.hits++;
        return;
    }
    /* Miss: the original keeps a world point at depth 10000 and reprojects it every frame; we
     * keep the screen point (stand-in, cosmetic). */
    g->shots[idx] = (Shot){ sx, sy, 0, 1, -1 };
}

/* Weapon_NearestTarget 0x4042a0 (ACT). */
static int nearest_target(Game *g, FgBlock *b, int px, int py, int *cx, int *cy)
{
    FgRecord *best = NULL;
    long bd = 0x1900;
    for (int i = 0; b && i < b->count; i++) {
        FgRecord *r = &b->rec[i];
        if (rec_skipped(g, r) || !targetable(g, r, 2)) continue;
        long dx = (r->bbox[0] + r->bbox[2]) / 2 - px, dy = (r->bbox[1] + r->bbox[3]) / 2 - py;
        if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = r; }
    }
    if (!best) return 0;
    g->state[best->actor]++;
    if (g->state[best->actor] < -0x40) { sfx_enemy_hit(g, best); return 0; }
    kill_actor(g, best, -1);
    g->st.act_kills++;
    *cx = (best->bbox[0] + best->bbox[2]) / 2; *cy = (best->bbox[1] + best->bbox[3]) / 2;
    return 1;
}

/* ---- weapons (§5) ------------------------------------------------------------------------ */

static void sfx_overheat(Game *g, int ok) { sfx(g, g->rec[ok ? 0x50 : 0x4f], 0, 0); }

static void shoot(Game *g, FgBlock *b, int n, int gp)
{
    sfx(g, g->rec[0x10], 0, -80);
    g->shots_fired++;
    g->st.shots++;
    g->aim_x = g->xhair_x; g->aim_y = g->xhair_y;
    int px = g->aim_x, py = (int32_t)rd32(g->rec + 0x40) - g->aim_y;     /* Aim_GetRay, type 1 */
    int prev = g->shot_idx - 1 < 0 ? n - 1 : g->shot_idx - 1;
    hitscan(g, b, g->shot_idx, g->aim_x, g->aim_y, px, py, gp);
    g->shot_hist |= 1u << (n - 1);
    g->shots[g->shot_idx].active = 1;
    /* Sticky tracer. */
    Shot *cur = &g->shots[g->shot_idx], *pv = &g->shots[prev];
    if (n != 1 && !cur->hit && pv->hit && pv->actor >= 0 && pv->actor < g->n_act) {
        int st = g->state[pv->actor];
        const SprSet *e = g->expl[pv->actor];
        if (st < 0 || (st != ST_GONE && e && e->count / 2 - st > n)) *cur = *pv;
    }
}

static void advance(Game *g, int n)
{
    if (++g->shot_idx >= n) g->shot_idx = 0;
    g->shot_hist >>= 1;
}

static void no_shot(Game *g) { g->shots[g->shot_idx].active = g->shots[g->shot_idx].hit = 0; }

static void heat_cool(Game *g)
{
    g->heat++;
    if (g->heat == 0) g->heat = g->heat_max;
    if (g->heat > g->heat_max) g->heat = g->heat_max;
}

/* Gun_Rapid 0x415af0 (weapon 0, §5.3). */
static void gun_rapid(Game *g, FgBlock *b, int mask, int n, int gp)
{
    heat_cool(g);
    if (g->overheated && g->heat >= 0) {
        hud_message(g, 2, 30, 30, 0); sfx_overheat(g, 1); g->overheated = 0;
    }
    advance(g, n);
    if (!(g->buttons & IN_FIRE) || g->heat < 2) { heat_cool(g); no_shot(g); return; }
    shoot(g, b, n, gp);
    uint32_t m = n >= 3 ? (uint32_t)mask << (n - 3) : (uint32_t)mask >> (3 - n);
    if ((g->shot_hist & m) == m) g->heat -= 4;
    if (g->o.difficulty != 1) g->heat -= 2;
    if (g->heat < 1) {
        g->heat = HEAT_LOCK;
        hud_message(g, 1, -1000, 5, 5);
        sfx_overheat(g, 0);
        g->overheated = 1;
        g->st.lockouts++;
        if (g->o.verbose) fprintf(stderr, "game: LASER OVERHEAT (frame %ld)\n", g->frame_no);
    }
}

/* Gun_Burst 0x415dc0 (weapons 1, 2, 4). */
static void gun_burst(Game *g, FgBlock *b, int n, int gp)
{
    advance(g, n);
    if (!(g->buttons & IN_FIRE) && g->shot_hist == 0) g->semi_armed = 1;
    if (g->semi_armed && (g->buttons & IN_FIRE) && g->shot_hist == 0) {
        g->semi_armed = 0;
        shoot(g, b, n, gp);
    } else no_shot(g);
}

/* Gun_Charge 0x416210 (weapon 3). Gun_Pulse (5) is unused in the data and mapped here too. */
static void gun_charge(Game *g, FgBlock *b, int n, int gp)
{
    advance(g, n);
    if (!(g->buttons & IN_FIRE)) { g->charge_armed = 1; g->charge_count = 0; }
    if (g->charge_armed && g->charge_gap >= 1 && (g->buttons & IN_FIRE)) {
        g->charge_gap = 0;
        g->charge_count++;
        shoot(g, b, n, gp);
        if (g->charge_count == 7) {
            hud_message(g, 1, 20, 2, 3); sfx_overheat(g, 0);
            g->charge_armed = 0; g->charge_gap = -30;
            g->st.lockouts++;
        }
    } else { g->charge_gap++; no_shot(g); }
}

/* ACT_Fire 0x4155b0 (§5.6). */
static void act_fire(Game *g, FgBlock *b)
{
    const SprSet *s = set_get(g, 3, 1);
    int m = s ? s->count : 0;
    g->act_anim >>= 1;
    g->aim_x = g->xhair_x; g->aim_y = g->xhair_y;
    if (g->act_req && g->act_anim == 0 && g->act_ammo > 0 && m > 0) {
        g->act_anim = 1u << (m - 1);
        sfx(g, g->rec[0x11], 0, -80);
        g->act_shot = (Shot){ g->aim_x, g->aim_y, 0, 1, -1 };
        g->act_chain_n = 0;
        g->act_ammo--;
        g->st.act_fired++;
    }
    g->act_req = 0;
    if (g->act_anim == 0x2000) fx_flash(g, 0, 20);
    if (((g->act_anim == 0 || g->act_anim > 0x1ff) && g->act_chain_timer == 0) || g->act_chain_hit) {
        g->act_chain_hit = 0;
    } else {
        if (g->act_chain_timer == 0) g->act_chain_timer = 6;
        int cx, cy, t = nearest_target(g, b, g->act_shot.x, g->act_shot.y, &cx, &cy);
        if (!t && g->act_chain_n > 0) t = nearest_target(g, b, g->chain_x, g->chain_y, &cx, &cy);
        if (t) {
            g->act_chain_hit = 1;
            if (g->act_chain_n++ == 0) { g->chain_x = cx; g->chain_y = cy; }
        }
    }
    if (g->act_chain_timer > 0) g->act_chain_timer--;
    g->ammo_frame = g->act_ammo + 5 < 6 ? 0 : g->act_ammo + 5;   /* Hud_SetAmmoBlink */
}

/* Weapon_Fire 0x416460. */
static void weapon_fire(Game *g, FgBlock *b)
{
    act_fire(g, b);
    const SprSet *gun = set_get(g, 3, g->rec[0x59]);
    int n = gun ? gun->count : 0;
    if (n == 0) return;
    if (n > N_SHOTS) n = N_SHOTS;
    if (g->shot_idx >= n) g->shot_idx = n - 1;
    int gp = (int8_t)g->rec[0x56];
    switch (g->rec[0x12]) {
    case 0:          gun_rapid(g, b, 7, n, gp); break;
    case 1: case 2: case 4: gun_burst(g, b, n, gp); break;
    default:         gun_charge(g, b, n, gp); break;
    }
}

static void input_poll(Game *g, const FgBlock *b)
{
    if (g->o.input) g->o.input(g->o.input_ud, g, b, &g->in);
    int x = g->in.x, y = g->in.y;
    g->xhair_x = x < 0 ? 0 : x >= W ? W - 1 : x;
    g->xhair_y = y < 0 ? 0 : y >= H ? H - 1 : y;
    g->buttons = g->in.buttons;
    if ((g->buttons & IN_ACT) && !(g->buttons & (IN_FIRE | IN_ALT))) g->act_req = 1;
    if (g->in.act) { g->act_req = 1; g->in.act = 0; }
}

/* ---- enemy fire (§6) ----------------------------------------------------------------------- */

static void view_make(const Game *g, View *v, const double cam[3], const double look[3], double roll)
{
    /* Same construction as tools/sprites.py project(): forward, right = up x forward (up = +Y), up. */
    double f[3], fl = 0;
    for (int i = 0; i < 3; i++) { v->c[i] = cam[i]; f[i] = look[i] - cam[i]; fl += f[i] * f[i]; }
    fl = sqrt(fl); if (fl < 1e-9) fl = 1;
    for (int i = 0; i < 3; i++) v->f[i] = f[i] / fl;
    double rx = v->f[2], rz = -v->f[0], rl = sqrt(rx * rx + rz * rz); if (rl < 1e-9) rl = 1;
    v->r[0] = rx / rl; v->r[1] = 0; v->r[2] = rz / rl;
    v->u[0] = -(v->f[1] * v->r[2] - v->f[2] * v->r[1]);
    v->u[1] = -(v->f[2] * v->r[0] - v->f[0] * v->r[2]);
    v->u[2] = -(v->f[0] * v->r[1] - v->f[1] * v->r[0]);
    v->cr = cos(roll * 3.14159265358979323846 / 180); v->sr = sin(roll * 3.14159265358979323846 / 180);
    v->k = (W / 2) / tan((int16_t)rd16(g->rec + 0x4a) * 3.14159265358979323846 / 92160.0);
}

/* Proj_ScreenToWorld 0x40b630: the point `dist` along the ray through top-down screen (sx, sy). */
static void view_unproject(const View *v, double sx, double sy, double dist, double out[3])
{
    double xp = (sx - W / 2.0) / v->k, yp = (sy - H / 2.0) / v->k;
    double x = xp * v->cr + yp * v->sr, y = -xp * v->sr + yp * v->cr, d[3], l = 0;
    for (int i = 0; i < 3; i++) { d[i] = x * v->r[i] + y * v->u[i] + v->f[i]; l += d[i] * d[i]; }
    l = sqrt(l);
    for (int i = 0; i < 3; i++) out[i] = v->c[i] + d[i] / l * dist;
}

/* World point -> top-down screen and depth (Proj_WorldToScreen 0x40b370). */
static double view_project(const View *v, const double p[3], double *sx, double *sy)
{
    double d[3] = { p[0] - v->c[0], p[1] - v->c[1], p[2] - v->c[2] }, X = 0, Y = 0, Z = 0;
    for (int i = 0; i < 3; i++) { X += d[i] * v->r[i]; Y += d[i] * v->u[i]; Z += d[i] * v->f[i]; }
    double x2 = X * v->cr - Y * v->sr, y2 = X * v->sr + Y * v->cr, zc = Z < 1e-6 ? 1e-6 : Z;
    *sx = W / 2.0 + x2 * v->k / zc; *sy = H / 2.0 + y2 * v->k / zc;
    return Z;
}

static void streak_spawn(Game *g, const FgRecord *r, float mx, float my)
{
    int slot = -1;
    for (int i = 0; i < N_STREAKS; i++) if (g->streaks[i].life <= 0) { slot = i; break; }
    if (slot < 0) return;
    /* Direction: the actor's on-screen motion (reversed when it recedes), else outward from the
     * screen centre (6.3 step 1). */
    const ActorPrev *p = &g->prev[r->actor];
    float dx = 0, dy = 0;
    float ax = (r->bbox[0] + r->bbox[2]) / 2.0f, ay = (r->bbox[1] + r->bbox[3]) / 2.0f;   /* actor's screen position */
    if (g->type == SEG_FREELOOK) {
        /* Free-look: use the fixup bbox centre and take out the shift caused by the player turning
         * the view, so only the actor's own motion (relative to the world) gives the direction. */
        if (p->cv) {
            dx = ax - p->cx - g->fl_sx;
            dy = ay - p->cy - g->fl_sy;
            if (p->dist2 <= g->dist2[r->actor]) { dx = -dx; dy = -dy; }
        }
    } else if (p->w) {
        dx = ax - (float)(p->x0 + p->w / 2);
        dy = ay - (float)(p->y0 + p->h / 2);
        if (p->dist2 <= g->dist2[r->actor]) { dx = -dx; dy = -dy; }
    }
    int d3 = g->type == SEG_RAIL && g->cam_have;
    float ox = d3 ? ax : mx, oy = d3 ? ay : my;     /* ray origin: the actor (type 1) or the muzzle (free-look) */
    if (dx * dx + dy * dy < 0.25f) { dx = ox - W / 2.0f; dy = oy - H / 2.0f + 40; }
    if (dx * dx + dy * dy < 0.25f) dy = 1;
    float t = 1e9f;
    if (dx > 0) t = (W - 1 - ox) / dx < t ? (W - 1 - ox) / dx : t;
    if (dx < 0) t = -ox / dx < t ? -ox / dx : t;
    if (dy > 0) t = (H - 1 - oy) / dy < t ? (H - 1 - oy) / dy : t;
    if (dy < 0) t = -oy / dy < t ? -oy / dy : t;
    if (t < 0) t = 0;
    if (d3) {
        /* E: the clipped ray end unprojected at camDist through the predicted view; S: the muzzle
         * through the current view at sqrt(dist2). The segments are projected per frame (draw_streaks). */
        View cur, pre;
        view_make(g, &cur, g->cam, g->lk, g->roll);
        view_make(g, &pre, g->pred_cam, g->pred_lk, g->pred_roll);
        double cd = 0;
        for (int i = 0; i < 3; i++) cd += (g->cam[i] - g->lk[i]) * (g->cam[i] - g->lk[i]);
        Streak *k = &g->streaks[slot];
        *k = (Streak){ 3, 0, 0, 0, 0, 0, 1, { { 0, 0, 0 }, { 0, 0, 0 } } };
        view_unproject(&cur, mx, my, sqrt((double)g->dist2[r->actor]), k->p[0]);
        view_unproject(&pre, ox + dx * t, oy + dy * t, sqrt(cd), k->p[1]);
        return;
    }
    g->streaks[slot] = (Streak){ 3, mx, my, mx + dx * t, my + dy * t, 0, 0, { { 0, 0, 0 }, { 0, 0, 0 } } };
}

/* Enemy_Update 0x4043f0 (§6.2). */
static void enemy_update(Game *g, FgBlock *b)
{
    int heavy = 0, light = 0, frame_dmg = 0;
    const uint8_t *hc = NULL, *lc = NULL;
    if (g->type == SEG_FREELOOK && g->look.active) {
        /* Pixels per engine unit at the view centre (2496 units = 360 degrees). */
        double k = (W / 2) / tan((int16_t)rd16(g->rec + 0x4a) * 3.14159265358979323846 / 92160.0), ppu = k * 2 * 3.14159265358979323846 / 2496.0;
        int dyaw = g->look.yaw - g->fl_yaw, dpit = g->look.pitch - g->fl_pitch;
        dyaw = (dyaw % 2496 + 3744) % 2496 - 1248;
        if (!g->fl_have) dyaw = dpit = 0;
        g->fl_sx = (float)(-dyaw * ppu); g->fl_sy = (float)(dpit * ppu);
        g->fl_yaw = g->look.yaw; g->fl_pitch = g->look.pitch; g->fl_have = 1;
    } else g->fl_have = 0;
    if (g->type == SEG_RAIL && g->cam_have) {
        /* 6.1: camera velocity and roll rate from the previous Enemy_Update (x2.0), predicted view = cam + v. */
        for (int i = 0; i < 3; i++) {
            double v = g->lat_have ? (g->cam[i] - g->lat_cam[i]) * 2.0 : 0;
            g->pred_cam[i] = g->cam[i] + v; g->pred_lk[i] = g->lk[i] + v;
            g->lat_cam[i] = g->cam[i]; g->lat_lk[i] = g->lk[i];
        }
        g->pred_roll = g->roll + (g->lat_have ? (g->roll - g->lat_roll) * 2.0 : 0);
        g->lat_roll = g->roll; g->lat_have = 1;
    }
    for (int i = 0; b && i < b->count; i++) {
        FgRecord *r = &b->rec[i];
        if (rec_skipped(g, r)) continue;
        const uint8_t *c = cls_of(g, r->actor);
        if (!c) continue;
        int id = r->actor, dmg = 0, fires = 0, hit = 0;
        ActorPrev *p = &g->prev[id];
        float mx = 0, my = 0;
        if (g->state[id] < 0) {
            int pan = screen_pan(r);
            if (r->w >= 21 || r->h >= 21) sfx_enemy_loop(g, c, r, pan, 1);
            else if (r->w >= 16 && r->h >= 16 && r->w > p->w && r->h > p->h) sfx_enemy_loop(g, c, r, pan, 0);
        }
        if (targetable(g, r, 3)) {
            if (!(r->flags & 0x80) && !(r->flags & 0x40000000u)) {
                if (pct() < c[0x1c]) {
                    int roll = pct();
                    if (roll < c[0x1d]) { hit = 1; fires = 1; dmg = c[0x1d] ? c[2] * roll / c[0x1d] : 0; }
                    if (rnd() < 0x2aaa) fires = 1;
                    mx = (r->bbox[0] + r->bbox[2]) / 2.0f; my = (r->bbox[1] + r->bbox[3]) / 2.0f;
                }
            } else {
                fires = hit = 1;
                dmg = (r->flags & 0x80) ? c[2] : (c[0x1d] ? pct() * c[2] / c[0x1d] : 0);
                mx = (float)((r->flags >> 18) & 0xfff); my = (float)(199 - (int)((r->flags >> 9) & 0x1ff));
            }
            if (fires) {
                g->st.enemy_shots++;
                if (mx > 0 && mx < W && my > 0 && my < H) streak_spawn(g, r, mx, my);
                sfx(g, pick(c + 0x32, 2), screen_pan(r), size_vol(r, c));
                if (hit && dmg > c[0x1e]) {
                    if (g->o.verbose > 2) fprintf(stderr, "dmg %ld actor %d cls fire %d hit %d dmg %d: %d (%dx%d)\n", g->frame_no, id, c[0x1c], c[0x1d], c[2], dmg, r->w, r->h);
                    g->health -= dmg * 100;            /* --god: the attract regen refills it */
                    g->st.damage += dmg * 100;
                    frame_dmg += dmg;
                }
            }
        }
        if (dmg > c[3] || frame_dmg > c[3]) { heavy = 1; hc = c; }
        if (dmg > c[0x1e] || frame_dmg > c[0x1e]) { light = 1; lc = c; }
        p->x0 = r->bbox[0]; p->y0 = r->bbox[1]; p->w = r->w; p->h = r->h; p->dist2 = g->dist2[id];
        p->cx = (r->bbox[0] + r->bbox[2]) / 2.0f; p->cy = (r->bbox[1] + r->bbox[3]) / 2.0f;
        p->cv = r->bbox[2] > 0 || r->bbox[3] > 0;
    }
    if (heavy) {
        fx_flash(g, 2, 20);
        int id = pick(hc + 0x28, 3);
        sfx(g, id != 0xff ? id : pick(hc + 0x25, 3), 0, 0);
    } else if (light) sfx(g, pick(lc + 0x25, 3), 0, 0);
    sfx_health_warning(g, g->health / 100);
}

/* ---- drawing ---------------------------------------------------------------------------------- */

/* Draw a set frame with its bottom-left at (x, y) bottom-up (Spr_Draw with stride -W), or, with
 * `flip`, its first RLE row at top-down row y (stride +W, used by the gun flash). */
static void draw_frame(const Game *g, const FgRecord *fr, int x, int y, uint32_t *dst, int flip)
{
    FgRecord r = *fr;
    r.x = (int16_t)x;
    r.y = (int16_t)(flip ? H - y - r.h : y);   /* flip: emulate by mirroring rows below */
    if (!flip) { sprite_draw(g->spr, &r, dst, W, H); return; }
    /* Stride +W: RLE row k lands on top-down row y + k (the sprite appears upside down). */
    sprite_draw_flip(g->spr, &r, dst, W, H, y);
}

static void put_px(uint32_t *dst, int x, int y, uint32_t v)
{
    if (x >= 0 && x < W && y >= 0 && y < H) dst[y * W + x] = v;
}

static uint32_t frame_colour(const Game *g, const FgRecord *fr)
{
    /* Middle opaque pixel of the first literal run: the colour of a textured tracer. */
    const uint8_t *s = fr->pixels, *end = s ? s + fr->pixels_size : NULL;
    uint32_t v = 0xffe080;
    for (int best = 0; s && s < end; ) {
        int8_t op = (int8_t)*s;
        if (op < 0) { int n = -op; if (n > best && s + 1 + n <= end) { best = n; v = g->spr->pal32[s[1 + n / 2]]; } s += 1 + n; }
        else if (op > 0) s++;
        else s += 3;
    }
    return v;
}

/* Fx_DrawTracer stand-in: a plain line in the frame's dominant colour. */
static void draw_line(uint32_t *dst, int x0, int y0, int x1, int y1, uint32_t v, int thick)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (int guard = 0; guard < 2000; guard++) {
        put_px(dst, x0, y0, v);
        if (thick) { put_px(dst, x0 + 1, y0, v); put_px(dst, x0, y0 + 1, v); }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

/* Hud_DrawACT 0x4169f0. */
static void draw_act(const Game *g, uint32_t *dst)
{
    const SprSet *s = set_get(g, 3, 1);
    if (!g->act_anim || !s) return;
    int m = s->count, d = (1 << (m + 1)) - 1;
    for (int k = 0; k < m; k++) {
        if (!(g->act_anim & (1u << (m - k - 1)))) continue;
        const FgRecord *fr = &s->fr[k];
        long sk = s->step[k];
        int x = (int)((long)(g->act_shot.x - 160) * sk / d) - fr->w / 2 + 160;
        int y = (int)((long)(100 - g->act_shot.y) * sk / d);
        draw_frame(g, fr, x, y, dst, 0);
    }
}

/* Hud_DrawGunFlash 0x416610 (§5.4). */
static void draw_gun_flash(const Game *g, uint32_t *dst)
{
    if (!g->shot_hist) return;
    const SprSet *s = set_get(g, 3, g->rec[0x59]);
    if (!s || s->count <= 0) return;
    int n = s->count > N_SHOTS ? N_SHOTS : s->count, ox, oy, two, thick;
    switch (g->rec[0x12]) {
    case 3: case 4: case 5:
        ox = (g->aim_x * 8 - 0x500) / 10 + 0xa0; oy = s->fr[0].h / 2 + 199; two = 0; thick = 1; break;
    case 2:  ox = 0; oy = 180; two = 1; thick = 1; break;
    default: ox = 0; oy = 180; two = 1; thick = 0; break;
    }
    long sn1 = s->step[n - 1] ? s->step[n - 1] : 1, sn = s->step[n] ? s->step[n] : 1;
    for (int k = 0; k < n; k++) {
        if (!(g->shot_hist & (1u << (n - k - 1)))) continue;
        int slot = g->shot_idx - k;
        if (slot < 0) slot += n;
        const FgRecord *fr = &s->fr[k];
        int tx = g->shots[slot].x, ty = g->shots[slot].y;
        long a = s->step[k], b = s->step[k + 1];
        for (int gun = 0; gun < 1 + two; gun++) {
            int o = gun ? W : ox;
            long dx = gun ? tx - ox - W : tx - ox, dy = ty - oy;
            if (!s->expanded) {
                draw_frame(g, fr, (int)(a * dx / sn1) - fr->w / 2 + o, (int)(dy * a / sn1) - fr->h / 2 + oy, dst, 1);
            } else {
                draw_line(dst, (int)(a * dx / sn) + o, oy + (int)(a * dy / sn), (int)(b * dx / sn) + o,
                          oy + (int)(b * dy / sn), frame_colour(g, fr), thick);
            }
        }
    }
}

/* Clip a screen segment to the frame (Liang-Barsky) and draw it. */
static void draw_clipped(uint32_t *dst, double x0, double y0, double x1, double y1, uint32_t v, int thick)
{
    double t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
    double pp[4] = { -dx, dx, -dy, dy }, qq[4] = { x0, W - 1 - x0, y0, H - 1 - y0 };
    for (int i = 0; i < 4; i++) {
        if (pp[i] == 0) { if (qq[i] < 0) return; continue; }
        double t = qq[i] / pp[i];
        if (pp[i] < 0) { if (t > t1) return; if (t > t0) t0 = t; }
        else { if (t < t0) return; if (t < t1) t1 = t; }
    }
    draw_line(dst, (int)(x0 + dx * t0), (int)(y0 + dy * t0), (int)(x0 + dx * t1), (int)(y0 + dy * t1), v, thick);
}

/* Proj_Segment 0x40b160 for a world segment: near-plane clip, project, clip to the frame, draw. */
static void draw_world_segment(const View *cur, uint32_t *dst, double p0[3], double p1[3], uint32_t v, int thick)
{
    double sx, sy, z0 = view_project(cur, p0, &sx, &sy), z1 = view_project(cur, p1, &sx, &sy);
    if (z0 < 1 && z1 < 1) return;
    if (z0 < 1 || z1 < 1) {
        double *bad = z0 < 1 ? p0 : p1, *good = z0 < 1 ? p1 : p0, zb = z0 < 1 ? z0 : z1, zg = z0 < 1 ? z1 : z0;
        double t = (1 - zb) / (zg - zb);
        for (int j = 0; j < 3; j++) bad[j] += (good[j] - bad[j]) * t;
    }
    double x0, y0, x1, y1;
    view_project(cur, p0, &x0, &y0); view_project(cur, p1, &x1, &y1);
    draw_clipped(dst, x0, y0, x1, y1, v, thick);
}

static void draw_streaks(Game *g, uint32_t *dst)
{
    const SprSet *s = set_get(g, 4, 0);
    uint32_t v = s && s->count ? frame_colour(g, &s->fr[s->count - 1]) : 0xff8040;
    static const float seg[3][2] = { { 0.0f, 0.1f }, { 0.4f, 0.5f }, { 0.6f, 1.0f } };
    View cur;
    int have_view = g->cam_have && g->type == SEG_RAIL;
    if (have_view) view_make(g, &cur, g->cam, g->lk, g->roll);
    for (int i = 0; i < N_STREAKS; i++) {
        Streak *k = &g->streaks[i];
        if (k->life <= 0) continue;
        double a = seg[k->ring][0], b = seg[k->ring][1];
        if (k->d3) {
            if (have_view) {
                double p0[3], p1[3];
                for (int j = 0; j < 3; j++) {
                    p0[j] = k->p[0][j] + (k->p[1][j] - k->p[0][j]) * a;
                    p1[j] = k->p[0][j] + (k->p[1][j] - k->p[0][j]) * b;
                }
                draw_world_segment(&cur, dst, p0, p1, v, g->rec[0x12] == 3);
            }
        } else {
            draw_line(dst, (int)(k->sx + (k->ex - k->sx) * (float)a), (int)(k->sy + (k->ey - k->sy) * (float)a),
                      (int)(k->sx + (k->ex - k->sx) * (float)b), (int)(k->sy + (k->ey - k->sy) * (float)b), v, g->rec[0x12] == 3);
        }
        k->life--; k->ring = (k->ring + 1) % 3;
    }
}

/* Hud_DrawBar 0x40a9d0: n pips upward from top-down row 199 - y. */
static void draw_bar(const Game *g, uint32_t *dst, int n, int colour, int x, int y)
{
    uint32_t v = g->spr->pal32[colour & 0xff];
    int row = H - y - 1;
    for (int p = 0; p < n; p++, row -= 8) {
        for (int i = 0; i < 6; i++) put_px(dst, x + 1 + i, row, v);
        for (int i = 0; i < 8; i++) { put_px(dst, x + i, row - 1, v); put_px(dst, x + i, row - 2, v); }
        for (int i = 0; i < 6; i++) put_px(dst, x + 1 + i, row - 3, v);
    }
}

/* Hud_DrawBracket 0x40a110: 3-pixel L corners. */
static void draw_bracket(const Game *g, uint32_t *dst, const FgRecord *r, const uint8_t *c)
{
    int x0, y0, x1, y1;
    grown_box(r, c, &x0, &y0, &x1, &y1);
    uint32_t v = g->spr->pal32[0xfb];
    for (int i = 0; i < 3; i++) {
        put_px(dst, x0 + i, y0, v); put_px(dst, x0, y0 + i, v);
        put_px(dst, x1 - i, y0, v); put_px(dst, x1, y0 + i, v);
        put_px(dst, x0 + i, y1, v); put_px(dst, x0, y1 - i, v);
        put_px(dst, x1 - i, y1, v); put_px(dst, x1, y1 - i, v);
    }
}

static void draw_crosshair(Game *g, uint32_t *dst, int on_target, int style)
{
    const SprSet *s = set_get(g, 2, style - 1);
    if (!s || s->count < 2) return;
    if (++g->xh_counter >= s->count / 2) g->xh_counter = 0;
    const FgRecord *fr = &s->fr[g->xh_counter + (on_target ? s->count / 2 : 0)];
    g->xh_rec = *fr; g->xh_valid = 1;
    draw_frame(g, fr, g->xhair_x - fr->w / 2, H - g->xhair_y - fr->h / 2, dst, 0);
}

void game_draw_crosshair_at(const Game *g, uint32_t *ovl, int x, int y)
{
    if (!g->xh_valid) return;
    x = x < 0 ? 0 : x >= W ? W - 1 : x;
    y = y < 0 ? 0 : y >= H ? H - 1 : y;
    draw_frame(g, &g->xh_rec, x - g->xh_rec.w / 2, H - y - g->xh_rec.h / 2, ovl, 0);
}

/* The scene is complete in `dst`: keep a copy as the scene layer and start the (transparent) UI layers. */
static void split_layers(Game *g, const uint32_t *dst)
{
    if (g->lay_split) return;
    g->lay_split = 1;
    if (g->lay_scene) memcpy(g->lay_scene, dst, sizeof(uint32_t) * W * H);
    if (g->lay_ovl) memset(g->lay_ovl, 0, sizeof(uint32_t) * W * H);
    memset(g->xb, 0, sizeof g->xb);
    memset(g->bb, 0, sizeof g->bb);
}

/* dst = src where src is drawn (alpha set). */
static void over(uint32_t *dst, const uint32_t *src)
{
    for (int i = 0; i < W * H; i++) if (src[i] >> 24) dst[i] = src[i];
}

/* Hud_DrawGauges0 0x40a610 (+ message blink). */
static void draw_gauges0(Game *g, uint32_t *dst)
{
    int hp = g->health / 100, colour;
    if (hp > 80) colour = 0xfc;
    else if (hp > 50) colour = 0xfa;
    else if (hp > 20) colour = 0xfb;
    else { colour = 0xf9; hud_message(g, 3, 60, 5, 5); }
    if (g->rec[0x12] == 0) {
        int h = g->heat, n = h < 0 ? h / -12 : 10 - h / 24;
        if (h >= 1 || (g->frame_no & 4)) {
            draw_bar(g, dst, n, 0xf9, 8, 12);
            draw_bar(g, dst, 10 - n, 4, 8, n * 8 + 12);
        }
    }
    draw_bar(g, dst, hp / 5, colour, W - 17, 12);
    const SprSet *s = set_get(g, 1, g->rec[0x44] - 1);
    if (!s || s->count == 0) return;
    draw_frame(g, &s->fr[0], 0, 0, dst, 0);
    if (s->count < 2) return;
    if (g->ammo_frame && g->ammo_frame < s->count) draw_frame(g, &s->fr[g->ammo_frame], 0, 0, dst, 0);
    if (!g->msg_id || !g->msg_dur) return;
    if (!g->msg_on_cnt && !g->msg_off_cnt) { g->msg_on_cnt = g->msg_on; g->msg_off_cnt = g->msg_off; g->msg_showing = 1; }
    if (g->msg_showing && g->msg_on_cnt > 0) {
        if (g->msg_id < s->count) draw_frame(g, &s->fr[g->msg_id], 0, 0, dst, 0);
        if (--g->msg_on_cnt == 0) { g->msg_on_cnt = g->msg_on; g->msg_off_cnt = g->msg_off; g->msg_showing = 0; }
    } else if (!g->msg_showing && g->msg_off_cnt > 0 && --g->msg_off_cnt == 0) {
        g->msg_off_cnt = g->msg_off; g->msg_on_cnt = g->msg_on; g->msg_showing = 1;
    }
    if (--g->msg_dur == 0) g->msg_on_cnt = g->msg_off_cnt = 0;
}

/* Hud_DrawGauges1 0x40a4a0 (LEVEL08). */
static void draw_gauges1(Game *g, uint32_t *dst)
{
    const SprSet *s = set_get(g, 1, g->rec[0x44] - 1);
    if (!s || s->count == 0) return;
    int hp = g->health / 100;
    draw_frame(g, &s->fr[0], 0, 0, dst, 0);
    if (s->count < 2) return;
    int a = hp < 30 ? 0xc : 0xb, b = hp < 30 ? 0xe : 0xd, off = (100 - hp) * 0x78 / 100;
    if (a < s->count) draw_frame(g, &s->fr[a], -off, 0, dst, 0);
    if (b < s->count) draw_frame(g, &s->fr[b], off, 0, dst, 0);
}

/* Hud_Draw 0x409e20. `dst` NULL: logic only (death check, regen, message state). */
static void hud_draw(Game *g, FgBlock *b, int fire_allowed, uint32_t *dst)
{
    int on_target = 0;
    if (dst) {
        draw_act(g, dst);
        draw_gun_flash(g, dst);
        draw_streaks(g, dst);
        for (int i = 0; b && i < b->count; i++)     /* Obj_DrawStatic: kind 8 */
            if (b->rec[i].kind == OBJ_OVERLAY) sprite_draw(g->spr, &b->rec[i], dst, W, H);
        split_layers(g, dst);    /* everything from here on is UI */
    }
    uint32_t *ov = dst ? g->lay_ovl : NULL;
    int choice = rd16(g->rec + 0x5c);
    if (b && (choice == 0 || g->scene_frame < choice)) {
        for (int i = 0; i < b->count; i++) {
            const FgRecord *r = &b->rec[i];
            if (rec_skipped(g, r) || !targetable(g, r, 0) || !bbox_nonzero(r)) continue;
            const uint8_t *c = cls_of(g, r->actor);
            if (dst && (c[1] & 0x40)) draw_bracket(g, ov, r, c);
            if (r->bbox[0] <= g->xhair_x && g->xhair_x <= r->bbox[2] && r->bbox[1] <= g->xhair_y && g->xhair_y <= r->bbox[3])
                on_target = 1;
        }
    }
    if (g->o.verbose > 2) fprintf(stderr, "hp %s:%d %ld %d\n", g->stem, g->seg, g->frame_no, g->health);
    if (!g->death_latch && !g->dead && g->health < 1) {
        g->death_latch = 1;
        g->branch_req = (int8_t)g->rec[0x23];
        g->st.deaths++;
        if (g->o.verbose) fprintf(stderr, "game: player died in %s:%d -> death clip %d\n", g->stem, g->seg, g->branch_req);
    } else if (g->health > 0) {
        g->health += g->o.god ? 0x639c : g->rec[0x51 + g->o.difficulty];      /* Player_Regen */
        if (g->health > 10000) g->health = 10000;
    }
    int style = g->rec[0x45];
    if (choice && g->scene_frame >= choice && g->rec[0x5b]) {     /* choice window */
        style = g->rec[0x5b];
        on_target = g->look.on_window;
    }
    if (!strcmp(g->stem, "LEVEL11")) {
        if (g->seg == 3 || g->seg == 4) { if (g->msg_id == 0) hud_message(g, 5, 0x3c0, 5, 5); }
        else if (g->msg_id == 5) hud_message(g, 0, 0, 0, 0);
    }
    if (!dst) return;
    if (style && fire_allowed) draw_crosshair(g, g->xb, on_target, style);
    if (g->rec[0x44]) { if (g->rec[0x58] == 1) draw_gauges1(g, g->bb); else draw_gauges0(g, g->bb); }
    /* The composite keeps the original draw order: brackets, crosshair, gauges. */
    over(dst, ov); over(dst, g->xb); over(dst, g->bb);
    over(ov, g->bb);
}

/* DD_Present: flash tint and shake. The tint colours are unknown (§7.4); a warm white blend. */
static void fx_tint(uint32_t *dst, int level)
{
    unsigned a = (unsigned)(level > 3 ? 3 : level) * 40;   /* /256 */
    for (int i = 0; i < W * H; i++) {
        uint32_t v = dst[i];
        unsigned r = (v >> 16) & 255, gg = (v >> 8) & 255, bb = v & 255;
        r += ((255 - r) * a) >> 8; gg += ((224 - (gg > 224 ? 224 : gg)) * a) >> 8; bb += ((176 - (bb > 176 ? 176 : bb)) * a) >> 8;
        dst[i] = (v & 0xFF000000u) | r << 16 | gg << 8 | bb;
    }
}

static void fx_shift(uint32_t *dst, int dx, int dy)
{
    static uint32_t tmp[W * H];
    memcpy(tmp, dst, sizeof tmp);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int sx = x - dx, sy = y - dy;
            dst[y * W + x] = sx >= 0 && sx < W && sy >= 0 && sy < H ? tmp[sy * W + sx] : 0;
        }
}

/* Applied to the composite (dumps) and to the scene layer; the UI layer stays steady. */
static void present_fx(Game *g, uint32_t *dst, uint32_t *scene)
{
    int level = g->flash;
    if (g->flash > 0) g->flash--;
    if (level > 0) { fx_tint(dst, level); if (scene) fx_tint(scene, level); }
    if (g->shake) {
        int s = g->shake;
        int dx = (rnd() * s / 0x7fff - s / 2) & ~3, dy = rnd() * s / 0x7fff - s / 2;
        g->shake--;
        if (dx || dy) { fx_shift(dst, dx, dy); if (scene) fx_shift(scene, dx, dy); }
    }
}

/* Sfx_PlayerDeath 0x417d40. */
static void sfx_player_death(Game *g)
{
    const uint8_t *c0 = g->n_cls ? g->cls : NULL;
    if (c0) { sfx(g, pick(c0 + 0x32, 2), 0, 0); sfx(g, pick(c0 + 0x28, 3), 0, 0); }
    sfx(g, g->rec[0x14], 0, 0);
}

int game_frame_post(Game *g, FgBlock *b, const uint8_t *hdr, uint32_t *frame, int drawn, const GameLayers *lay)
{
    if (!g->loaded || !g->have_seg) return 0;
    g->lay_scene = lay ? lay->scene : NULL; g->lay_ovl = lay ? lay->ovl : NULL;
    g->lay_split = 0;
    uint32_t flags = hdr ? rd32(hdr + 0x20) : 0;
    uint32_t *dst = drawn ? frame : NULL;
    if (dst) g->xh_valid = 0;
    switch (g->type) {
    case SEG_RAIL:
        if (flags & 1) { enemy_update(g, b); hud_draw(g, b, (flags & 2) != 0, dst); }
        break;
    case SEG_FREELOOK:
        enemy_update(g, b);
        hud_draw(g, b, 1, dst);
        break;
    case SEG_PANO:
        /* Panorama renderer not implemented (no shipped type-2 segment): death check and regen. */
        hud_draw(g, NULL, 0, NULL);
        break;
    default:
        g->health += g->o.god ? 0x639c : g->rec[0x51 + g->o.difficulty];
        if (g->health > 10000) g->health = 10000;
        break;
    }
    int layered = 0;
    if (dst && lay) {
        split_layers(g, dst);   /* no HUD this tick: all scene, empty UI */
        layered = 1;
    }
    if (dst) present_fx(g, dst, layered ? g->lay_scene : NULL);
    /* Player_Update 0x410740. */
    if (g->type == SEG_RAIL && (flags & 1) && g->rec[0x44]) {
        input_poll(g, b);
        if ((flags & 2) && g->rec[0x45]) weapon_fire(g, b);
    } else if (g->type != SEG_PANO && g->type != SEG_FREELOOK) weapon_reset(g);
    /* Death timer (Play_Chunk). */
    if (g->death_latch && !g->latched_prev) g->death_timer = 9;
    g->latched_prev = g->death_latch;
    if (g->death_latch && g->death_timer > 0 && --g->death_timer == 0) {
        sfx_player_death(g);
        fx_flash(g, 3, 20);
        g->dead = 1; g->death_latch = 0; g->latched_prev = 0;
    }
    g->frame_no++; g->scene_frame++;
    return layered;
}

/* ---- type 6 / save points ------------------------------------------------------------------ */

const InputState *game_poll_input(Game *g, const FgBlock *b)
{
    input_poll(g, b);
    return &g->in;
}

void game_look_fire(Game *g, FgBlock *b)
{
    if (!g->loaded || !g->have_seg) return;
    weapon_fire(g, b);
}

void game_set_look(Game *g, const LookInfo *li) { g->look = *li; }
const LookInfo *game_look(const Game *g) { return &g->look; }

void game_checkpoint(Game *g) { if (g->loaded) reset_for_scene(g); }
int  game_act_ammo(const Game *g) { return g->act_ammo; }
void game_set_act_ammo(Game *g, int n) { g->act_ammo = n; g->ammo_frame = n + 5 < 6 ? 0 : n + 5; }

/* ---- queries ------------------------------------------------------------------------------- */

int game_branch_pending(const Game *g) { return g->branch_req != 0; }

int game_take_branch(Game *g, int *target)
{
    if (!g->branch_req) return 0;
    *target = g->branch_req;
    g->branch_req = 0;
    return 1;
}

void game_fill_state(const Game *g, GameState *gs)
{
    gs->health = g->health;
    gs->kills = g->kills;
    gs->frames_shown = gs->frames_dropped = 0;
    gs->xhair_x = g->xhair_x; gs->xhair_y = g->xhair_y;
    gs->actor_state = g->loaded ? g->state : NULL;
    gs->n_actors = g->loaded ? g->n_act : 0;
}

const GameStats *game_stats(const Game *g) { return &g->st; }

void game_print_stats(const Game *g, FILE *fp)
{
    const GameStats *s = &g->st;
    fprintf(fp, "stats: kills %ld (ACT %ld), shots %ld, hits %ld, ACT fired %ld, enemy shots %ld, "
                "damage taken %ld (%.1f bars), deaths %ld, heat lockouts %ld, sfx %ld\n",
            s->kills, s->act_kills, s->shots, s->hits, s->act_fired, s->enemy_shots, s->damage,
            (double)s->damage / 10000.0, s->deaths, s->lockouts, s->sfx);
}

/* An aim point on the record that passes Obj_HitTestPoint: the bbox centre, or for pixel-exact
 * classes the opaque pixel closest to it. Returns 0 if none was found. */
static int aim_point(const Game *g, const FgRecord *r, int *ax, int *ay)
{
    const uint8_t *c = cls_of(g, r->actor);
    int x0, y0, x1, y1;
    grown_box(r, c, &x0, &y0, &x1, &y1);
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, vh = (int32_t)rd32(g->rec + 0x40);
    *ax = cx; *ay = cy;
    if (!(c[1] & 0x80) || g->type == SEG_FREELOOK) return 1;
    long best = -1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            long d = (long)(x - cx) * (x - cx) + (long)(y - cy) * (y - cy);
            if (best >= 0 && d >= best) continue;
            if (hit_pixel(r, x, vh - y)) { best = d; *ax = x; *ay = y; }
        }
    return best >= 0;
}

int game_targets(const Game *g, const FgBlock *b, GameTarget *out, int max)
{
    int n = 0;
    if (!g->loaded || !g->have_seg || !b) return 0;
    for (int i = 0; i < b->count && n < max; i++) {
        const FgRecord *r = &b->rec[i];
        if (rec_skipped(g, r) || !targetable(g, r, 1) || !bbox_nonzero(r)) continue;
        int ax, ay;
        if (!aim_point(g, r, &ax, &ay)) continue;
        if (ax < 0 || ax >= W || ay < 0 || ay >= H) continue;
        int ci = g->act[ACT_SIZE * r->actor + g->o.difficulty];
        out[n++] = (GameTarget){ ax, ay, r->w, r->h, g->dist2[r->actor], r->actor, g->cls_avoid[ci] };
    }
    return n;
}

int game_weapon(const Game *g)
{
    const SprSet *gun = g->have_seg ? set_get(g, 3, g->rec[0x59]) : NULL;
    return gun && gun->count ? g->rec[0x12] : -1;
}
int game_heat(const Game *g) { return g->heat; }
int game_charge_count(const Game *g) { return g->charge_count; }
int game_act_ready(const Game *g) { return g->act_ammo > 0 && g->act_anim == 0; }
int game_shot_hist(const Game *g) { return (int)g->shot_hist; }
