/* scene.c - see scene.h. Logic follows docs/SCENE_GRAPH.md and tools/l96.py. */
#include "scene.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t  rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int scene_load(Scene *sc, const L96File *f)
{
    memset(sc, 0, sizeof *sc);
    uint8_t h[0x94];
    if (l96_read(f, 0, h, sizeof h) != 0) return -1;
    uint32_t n_hs = rd32(h + 0x30), off_hs = rd32(h + 0x60);

    if (f->rd.size && (uint64_t)off_hs + (uint64_t)n_hs * SCENE_HS_REC > f->rd.size(f->rd.ctx)) return -1;   /* table past EOF */

    sc->n_segments = (int)f->n_segments;
    sc->n_hotspots = (int)n_hs;
    sc->seg = calloc(f->n_segments ? f->n_segments : 1, sizeof *sc->seg);
    sc->hs  = calloc(n_hs ? n_hs : 1, sizeof *sc->hs);
    if (!sc->seg || !sc->hs) { scene_free(sc); return -1; }

    for (uint32_t s = 0; s < f->n_segments; s++) {
        const uint8_t *r = f->segments[s].raw;
        SegInfo *si = &sc->seg[s];
        si->type     = (int32_t)rd32(r);
        si->next     = (int8_t)r[0x0e];
        si->hs_first = r[0x16];
        si->hs_count = r[0x17];
        si->esc      = (int8_t)r[0x22];
        si->death    = (int8_t)r[0x23];
        si->aim      = r[0x44];
        si->xhair    = r[0x45];
        si->half_fov = rd16(r + 0x4a);
        si->save     = r[0x54];
        si->reset    = r[0x55];
        if (si->hs_first + si->hs_count > (int)n_hs) si->hs_count = 0;   /* defensive */
    }
    for (uint32_t i = 0; i < n_hs; i++) {
        uint8_t r[SCENE_HS_REC];
        if (l96_read(f, (uint64_t)off_hs + (uint64_t)SCENE_HS_REC * i, r, sizeof r) != 0) { scene_free(sc); return -1; }
        Hotspot *hp = &sc->hs[i];
        hp->target = (int32_t)rd32(r);
        hp->flags  = rd32(r + 4);
        hp->hp     = (int32_t)rd32(r + 8);
        hp->kills  = (int32_t)rd32(r + 12);
        for (int k = 0; k < 6; k++) {
            hp->actors_a[k] = rd16(r + 0x10 + 2 * k);
            hp->actors_b[k] = rd16(r + 0x24 + 2 * k);
        }
        hp->x0 = rd16(r + 0x1c); hp->y0 = rd16(r + 0x1e);
        hp->x1 = rd16(r + 0x20); hp->y1 = rd16(r + 0x22);
        hp->chance = r[0x30];
    }
    return 0;
}

void scene_free(Scene *sc)
{
    free(sc->seg); free(sc->hs);
    memset(sc, 0, sizeof *sc);
}

GameState scene_default_state(void)
{
    GameState g;
    memset(&g, 0, sizeof g);
    g.health = 10000;
    g.xhair_x = 160; g.xhair_y = 100;
    return g;
}

static uint32_t g_seed = 1;
void scene_srand(uint32_t seed)
{
    /* Mix the seed: small consecutive seeds would otherwise all give a tiny first rand(). */
    seed ^= seed >> 16; seed *= 0x7feb352du; seed ^= seed >> 15; seed *= 0x846ca68bu; seed ^= seed >> 16;
    g_seed = seed;
}
int scene_rand(void)
{
    g_seed = g_seed * 214013u + 2531011u;          /* MSVC rand() recurrence */
    return (int)((g_seed >> 16) & 0x7fff);
}

static int alive(const GameState *gs, int16_t a)
{
    if (a < 0 || !gs->actor_state || a >= gs->n_actors) return 0;
    return gs->actor_state[a] < -0x40;
}

/* One table44 condition (non-random, non-look-only), as in Branch_Resolve. */
static int hs_holds(const Hotspot *h, int seg_type, const GameState *gs)
{
    uint32_t fl = h->flags;
    int or_mode = (fl & HS_OR) != 0;
    int xh, kl, hpok, act;

    if (seg_type == SEG_PANO) xh = !(fl & HS_XHAIR);
    else if (!(fl & HS_XHAIR)) xh = !or_mode;
    else xh = gs->xhair_x >= h->x0 && gs->xhair_x <= h->x1 && gs->xhair_y >= h->y0 && gs->xhair_y <= h->y1;

    int shown = gs->frames_shown;
    int thr = shown ? (int)((int64_t)(shown - gs->frames_dropped) * h->kills / shown) : h->kills;
    if (fl & HS_KILLS_GT)      kl = gs->kills > thr;
    else if (fl & HS_KILLS_LT) kl = gs->kills < thr;
    else                       kl = !or_mode;

    if (fl & HS_HP_GT)      hpok = gs->health / 100 > h->hp;
    else if (fl & HS_HP_LT) hpok = gs->health / 100 < h->hp;
    else                    hpok = !or_mode;

    if (fl & HS_ANY_ALIVE) {
        act = 0;
        for (int k = 0; k < 6; k++) if (alive(gs, h->actors_a[k]) || alive(gs, h->actors_b[k])) act = 1;
    } else if (fl & HS_NONE_ALIVE) {
        act = 1;
        for (int k = 0; k < 6; k++) if (alive(gs, h->actors_a[k]) || alive(gs, h->actors_b[k])) act = 0;
    } else if (fl & HS_FIRST_DEP) {
        if (alive(gs, h->actors_a[0])) {
            act = 0;
            for (int k = 1; k < 6; k++) if (alive(gs, h->actors_a[k])) act = 1;
            for (int k = 0; k < 6; k++) if (alive(gs, h->actors_b[k])) act = 1;
        } else act = 1;
    } else act = !or_mode;

    if (or_mode) return xh || kl || hpok || act;
    return xh && kl && hpok && act;
}

int scene_branch_resolve(const Scene *sc, int seg, const GameState *gs)
{
    const SegInfo *si = &sc->seg[seg];
    for (int i = 0; i < si->hs_count; i++) {
        const Hotspot *h = &sc->hs[si->hs_first + i];
        if (h->flags == HS_RANDOM) {
            if (scene_rand() * 100 / 0x7fff < h->chance) return h->target;
        } else if (h->flags != HS_LOOK) {
            if (hs_holds(h, si->type, gs)) return h->target;
        }
    }
    return si->next;
}

int scene_auto_pick(const Scene *sc, int seg)
{
    const SegInfo *si = &sc->seg[seg];
    int r = si->next, any = 0;
    for (int i = 0; i < si->hs_count; i++) {
        const Hotspot *h = &sc->hs[si->hs_first + i];
        if (h->flags != HS_RANDOM) continue;
        any = 1;
        if (scene_rand() * 100 / 0x7fff < h->chance) r = h->target;
    }
    return any ? r : INT_MIN;   /* no random record: nothing cached, resolve at the end */
}

static int seg_type(const Scene *sc, int s)
{
    return s >= 0 && s < sc->n_segments ? sc->seg[s].type : -1;
}

int scene_successor(const Scene *sc, int seg, const GameState *gs, int picked)
{
    const SegInfo *si = &sc->seg[seg];
    int nt = seg_type(sc, si->next);
    switch (si->type) {
    case SEG_VIDEO: case SEG_RAIL:
        if (nt == SEG_PANO || nt == SEG_FREELOOK) return si->next;
        return scene_branch_resolve(sc, seg, gs);
    case SEG_PANO: case SEG_FREELOOK:
        if (nt == SEG_CUTSCENE) return si->next;
        if (picked != INT_MIN) return picked;
        return scene_branch_resolve(sc, seg, gs);
    default:
        return si->next;
    }
}

int scene_esc(const Scene *sc, int seg)
{
    const SegInfo *si = &sc->seg[seg];
    if (seg < si->esc || si->next == 0) return si->esc;
    if (si->esc == seg) return SCENE_ESC_IGNORE;
    return SCENE_ESC_HUB;
}

int scene_look_enabled(const Scene *sc, int seg, int i, int hub, int highest)
{
    const SegInfo *si = &sc->seg[seg];
    if (i < 0 || i >= si->hs_count || !(sc->hs[si->hs_first + i].flags & HS_LOOK)) return 0;
    return !hub || i == 0 || i == 15 || i < highest;
}

int scene_hotspot_pick(const Scene *sc, int seg, int yaw, int pitch, int hub, int highest)
{
    const SegInfo *si = &sc->seg[seg];
    for (int i = 0; i < si->hs_count; i++) {
        if (!scene_look_enabled(sc, seg, i, hub, highest)) continue;
        const Hotspot *h = &sc->hs[si->hs_first + i];
        int in_yaw = h->x0 < h->x1 ? h->x0 <= yaw && yaw <= h->x1 : h->x0 <= yaw || yaw <= h->x1;
        if (in_yaw && h->y0 <= pitch && pitch <= h->y1) return i;
    }
    return -1;
}
