/* chain.c - see chain.h. Flow rules: docs/SCENE_GRAPH.md §4-5. */
#include "chain.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cinepak.h"
#include "l96.h"
#include "scene.h"
#include "sprites.h"

#define RING 16
#define TEX  128            /* type-6 preload texture: 128x128 Cinepak key frame */
#define ESC_TICKS 15        /* Key_Skip is rate limited to once per second */

typedef struct {
    L96Group g;
    int seg, k;
    int tex;                /* type-6 groups: preload texture slot, -1 none */
} Slot;

struct Chain {
    ChainOpts o;
    char      start_stem[16], stem[16];
    int       open;
    L96File   f;
    Scene     sc;
    SpriteCtx spr;
    GameState gs;

    /* Read side. */
    Slot ring[RING];
    int  head, n;
    int  rseg, rk, read_done, picked;
    int  pending, pending_seg;     /* switch to file `pending_name` once the ring is empty */
    char pending_name[16];
    int  ended;
    int  accumulate;               /* collecting the preload for the following type-6 segment */
    uint8_t *pre; size_t pre_size, pre_cap;
    uint32_t tex[2][TEX * TEX];
    int  tex_ok[2], tex_next, cur_tex;

    /* Display side. */
    int  vf, disp_seg;
    FgBlock blocks[L96_FRAMES_PER_GROUP];
    int  nblocks;
    CinepakDecoder dec;
    int  dec_ready;
    uint32_t frame[CHAIN_W * CHAIN_H];
    long ticks, last_esc;

    char  *hist; size_t hist_len, hist_cap;
};

static uint8_t g_silence[L96_AUDIO_BYTES];   /* u8 PCM silence (0x80), set in chain_open */

/* ---- files ---------------------------------------------------------------- */

static void stem_of(const char *name, char out[16])
{
    size_t i = 0;
    for (; name[i] && name[i] != '.' && i < 15; i++) out[i] = name[i];
    out[i] = 0;
}

static int open_level(Chain *c, const char *stem)
{
    char path[1024];
    const char *dirs[] = { "", "cd1/", "cd2/", "cd3/", "cd4/", "CD1/", "CD2/", "CD3/", "CD4/" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s/%s%s.L96", c->o.data_dir, dirs[i], stem);
        FILE *t = fopen(path, "rb");
        if (!t) continue;
        fclose(t);
        if (l96_open(&c->f, path) != 0) return -1;
        if (scene_load(&c->sc, &c->f) != 0 || sprites_load(&c->spr, &c->f) != 0) {
            fprintf(stderr, "chain: cannot load tables of %s\n", path);
            scene_free(&c->sc); l96_close(&c->f);
            return -1;
        }
        snprintf(c->stem, sizeof c->stem, "%s", stem);
        c->open = 1;
        return 0;
    }
    fprintf(stderr, "chain: %s.L96 not found under %s (or its cd*/ dirs)\n", stem, c->o.data_dir);
    return -1;
}

static void close_level(Chain *c)
{
    if (!c->open) return;
    sprites_free(&c->spr);
    scene_free(&c->sc);
    l96_close(&c->f);
    c->open = 0;
}

static void drop_ring(Chain *c)
{
    for (int i = 0; i < RING; i++) l96_group_free(&c->ring[i].g);
    c->head = c->n = 0;
    c->vf = 0;
}

static void hist_add(Chain *c, const char *stem, int seg)
{
    char tmp[48];
    int len = snprintf(tmp, sizeof tmp, "%s%s:%d", c->hist_len ? " " : "", stem, seg);
    if (len < 0) return;
    if (c->hist_len + (size_t)len + 1 > c->hist_cap) {
        size_t cap = c->hist_cap ? c->hist_cap * 2 : 256;
        while (cap < c->hist_len + (size_t)len + 1) cap *= 2;
        char *h = realloc(c->hist, cap);
        if (!h) return;
        c->hist = h; c->hist_cap = cap;
    }
    memcpy(c->hist + c->hist_len, tmp, (size_t)len + 1);
    c->hist_len += (size_t)len;
}

/* ---- life cycle ------------------------------------------------------------- */

Chain *chain_open(const ChainOpts *o)
{
    memset(g_silence, 0x80, sizeof g_silence);
    Chain *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->o = *o;
    c->disp_seg = -1;
    c->cur_tex = -1;
    c->last_esc = -ESC_TICKS;
    c->gs = scene_default_state();
    scene_srand(o->seed);
    stem_of(o->level, c->start_stem);
    if (open_level(c, c->start_stem) != 0) { free(c); return NULL; }
    if (o->from_seg < 0 || (uint32_t)o->from_seg >= c->f.n_segments) {
        fprintf(stderr, "chain: segment %d out of range (%s has %u)\n", o->from_seg, c->stem, c->f.n_segments);
        chain_close(c); return NULL;
    }
    c->rseg = o->from_seg;
    return c;
}

void chain_close(Chain *c)
{
    if (!c) return;
    drop_ring(c);
    close_level(c);
    if (c->dec_ready) cinepak_free(&c->dec);
    free(c->pre);
    free(c->hist);
    free(c);
}

/* ---- read side ---------------------------------------------------------------- */

/* The 20 groups in front of a type-6 segment carry a 0x44 header + 128x128 Cinepak key
 * frame (L96_FORMAT §6.4). Decode it as the free-look placeholder. */
static void decode_preload(Chain *c)
{
    int slot = c->tex_next;
    c->tex_next ^= 1;
    c->tex_ok[slot] = 0;
    c->cur_tex = slot;
    if (c->pre_size < L96_FRAME_HDR_SIZE + 10) return;
    const uint8_t *cp = c->pre + L96_FRAME_HDR_SIZE;
    uint32_t sz = cp[1] << 16 | cp[2] << 8 | cp[3];
    int w = cp[4] << 8 | cp[5], h = cp[6] << 8 | cp[7];
    if (cp[0] > 1 || w != TEX || h != TEX || L96_FRAME_HDR_SIZE + sz > c->pre_size) return;
    CinepakDecoder d;
    if (cinepak_init(&d, TEX, TEX) != 0) return;
    if (cinepak_decode(&d, cp, sz) == 0) {
        memcpy(c->tex[slot], d.pixels, sizeof c->tex[slot]);
        c->tex_ok[slot] = 1;
    }
    cinepak_free(&d);
}

static void set_exit(Chain *c, int target)
{
    c->read_done = 1;
    if (c->o.single) return;
    int idx = -1 - target;
    if (idx < 0 || (uint32_t)idx >= c->f.n_names) { fprintf(stderr, "chain: bad exit %d\n", target); return; }
    const char *name = c->f.names[idx];
    if (!strcmp(name, "SUCCESS")) { fprintf(stderr, "chain: SUCCESS - game complete\n"); return; }
    if (c->o.one_level) { hist_add(c, name, 0); return; }
    snprintf(c->pending_name, sizeof c->pending_name, "%s", name);
    c->pending = 1;
    c->pending_seg = 0;
}

static int switch_file(Chain *c)
{
    char name[16];
    snprintf(name, sizeof name, "%s", c->pending_name);
    c->pending = 0;
    close_level(c);
    if (open_level(c, name) != 0) { c->ended = 1; return -1; }
    c->rseg = c->pending_seg < (int)c->f.n_segments ? c->pending_seg : 0;
    c->rk = 0;
    c->read_done = 0;
    c->disp_seg = -1;
    return 0;
}

int chain_fill(Chain *c, int ahead, ChainAudioFn fn, void *ud)
{
    while (!c->ended && c->n < RING && c->n <= ahead) {
        if (c->read_done) {
            if (c->pending && c->n == 0) { if (switch_file(c) != 0) return -1; continue; }
            break;
        }
        const L96Segment *sg = &c->f.segments[c->rseg];
        const SegInfo *si = &c->sc.seg[c->rseg];
        if (c->rk == 0) {
            c->picked = si->type == SEG_FREELOOK ? scene_auto_pick(&c->sc, c->rseg) : INT_MIN;
            int nt = si->next >= 0 && si->next < c->sc.n_segments ? c->sc.seg[si->next].type : -1;
            c->accumulate = (si->type == SEG_VIDEO || si->type == SEG_RAIL) && nt == SEG_FREELOOK;
            c->pre_size = 0;
        }
        if (c->rk < sg->count) {
            Slot *s = &c->ring[(c->head + c->n) % RING];
            if (l96_read_group(&c->f, c->rseg, c->rk, &s->g) != 0) return -1;
            s->seg = c->rseg; s->k = c->rk;
            s->tex = si->type == SEG_FREELOOK ? c->cur_tex : -1;
            if (c->accumulate && s->g.carry_size) {
                if (c->pre_size + s->g.carry_size > c->pre_cap) {
                    size_t cap = c->pre_cap ? c->pre_cap : 0x100000;
                    while (cap < c->pre_size + s->g.carry_size) cap *= 2;
                    uint8_t *p = realloc(c->pre, cap);
                    if (!p) return -1;
                    c->pre = p; c->pre_cap = cap;
                }
                memcpy(c->pre + c->pre_size, s->g.carry, s->g.carry_size);
                c->pre_size += s->g.carry_size;
            }
            if (fn) {
                if (s->g.audio_size >= L96_AUDIO_BYTES) fn(ud, s->g.audio, L96_AUDIO_BYTES);
                else fn(ud, g_silence, L96_AUDIO_BYTES);   /* keep the audio clock in step */
            }
            c->n++;
            c->rk++;
        }
        if (c->rk >= sg->count) {
            if (c->accumulate) decode_preload(c);
            if (c->o.single) { c->read_done = 1; continue; }
            int nxt = scene_successor(&c->sc, c->rseg, &c->gs, c->picked);
            if (nxt == c->rseg && !c->o.no_auto_look) {
                /* Without input a look-choice segment repeats forever: pick its first window. */
                const SegInfo *cs = &c->sc.seg[c->rseg];
                for (int i = 0; i < cs->hs_count; i++)
                    if (c->sc.hs[cs->hs_first + i].flags & HS_LOOK) {
                        nxt = c->sc.hs[cs->hs_first + i].target;
                        if (c->o.verbose) fprintf(stderr, "auto-look: %s:%d -> %d\n", c->stem, c->rseg, nxt);
                        break;
                    }
            }
            if (nxt >= 0 && nxt < c->sc.n_segments) { c->rseg = nxt; c->rk = 0; }
            else set_exit(c, nxt);
        }
    }
    if (c->read_done && !c->pending && c->n == 0) c->ended = 1;
    return 0;
}

/* ---- display side ------------------------------------------------------------- */

static void draw_placeholder(Chain *c, int tex)
{
    memset(c->frame, 0, sizeof c->frame);
    int ox = (CHAIN_W - TEX) / 2, oy = (CHAIN_H - TEX) / 2;
    /* Thin frame so an all-dark texture still reads as "placeholder". */
    for (int x = ox - 2; x < ox + TEX + 2; x++) {
        c->frame[(oy - 2) * CHAIN_W + x] = 0x406080;
        c->frame[(oy + TEX + 1) * CHAIN_W + x] = 0x406080;
    }
    for (int y = oy - 2; y < oy + TEX + 2; y++) {
        c->frame[y * CHAIN_W + ox - 2] = 0x406080;
        c->frame[y * CHAIN_W + ox + TEX + 1] = 0x406080;
    }
    if (tex < 0 || !c->tex_ok[tex]) return;
    for (int y = 0; y < TEX; y++)
        memcpy(c->frame + (size_t)(oy + y) * CHAIN_W + ox, c->tex[tex] + (size_t)y * TEX, TEX * sizeof(uint32_t));
}

int chain_step(Chain *c, int *new_image, ChainAudioFn fn, void *ud)
{
    *new_image = 0;
    if (c->n == 0 && chain_fill(c, 1, fn, ud) != 0) return -1;
    if (c->n == 0) return 0;

    Slot *s = &c->ring[c->head];
    const SegInfo *si = &c->sc.seg[s->seg];
    int nticks = s->g.n_frames ? s->g.n_frames : L96_FRAMES_PER_GROUP;

    if (c->vf == 0) {
        if (s->seg != c->disp_seg || s->k == 0) {
            c->disp_seg = s->seg;
            hist_add(c, c->stem, s->seg);
            if (c->o.verbose)
                fprintf(stderr, "segment %s:%d type %d groups %d (tick %ld)\n", c->stem, s->seg, si->type,
                        c->f.segments[s->seg].count, c->ticks);
        }
        c->nblocks = s->g.fg_size ? fg_parse(s->g.fg, s->g.fg_size, c->blocks, L96_FRAMES_PER_GROUP) : 0;
    }

    if (s->g.n_frames) {
        const uint8_t *d = s->g.frame[c->vf].data;
        int w = d[4] << 8 | d[5], h = d[6] << 8 | d[7];
        if (!c->dec_ready || w != c->dec.width || h != c->dec.height) {
            if (c->dec_ready) cinepak_free(&c->dec);
            c->dec_ready = 0;
            if (cinepak_init(&c->dec, w, h) != 0) return -1;
            c->dec_ready = 1;
        }
        int rc = cinepak_decode(&c->dec, d, s->g.frame[c->vf].size);
        if (rc != 0) fprintf(stderr, "cinepak: tick %ld decode error %d\n", c->ticks, rc);
        if (w == CHAIN_W && h == CHAIN_H) memcpy(c->frame, c->dec.pixels, sizeof c->frame);
        else {
            memset(c->frame, 0, sizeof c->frame);
            for (int y = 0; y < h && y < CHAIN_H; y++)
                memcpy(c->frame + (size_t)y * CHAIN_W, c->dec.pixels + (size_t)y * (size_t)w,
                       (size_t)(w < CHAIN_W ? w : CHAIN_W) * sizeof(uint32_t));
        }
        /* Sprites: types 2/6 draw their records into the panorama texture instead (not done yet). */
        if (si->type != SEG_FREELOOK && si->type != SEG_PANO && c->vf < c->nblocks) {
            const uint8_t *hdr = s->g.frame[c->vf].hdr;
            uint32_t flags = hdr[0x20] | hdr[0x21] << 8 | hdr[0x22] << 16 | (uint32_t)hdr[0x23] << 24;
            /* Hud_Draw (and with it Obj_DrawStatic, kind 8) runs only on aim frames. */
            fg_composite(&c->spr, &c->blocks[c->vf], c->frame, CHAIN_W, CHAIN_H, (flags & 1) != 0);
        }
        *new_image = 1;
    } else if (si->type == SEG_FREELOOK && c->vf == 0) {
        draw_placeholder(c, s->tex);
        *new_image = 1;
    }

    c->ticks++;
    if (++c->vf >= nticks) {
        l96_group_free(&s->g);
        c->head = (c->head + 1) % RING;
        c->n--;
        c->vf = 0;
    }
    return 1;
}

const uint32_t *chain_frame(const Chain *c) { return c->frame; }
const char *chain_level(const Chain *c) { return c->stem; }
int chain_segment(const Chain *c) { return c->disp_seg >= 0 ? c->disp_seg : c->rseg; }
const char *chain_history(const Chain *c) { return c->hist ? c->hist : ""; }

/* ---- ESC ----------------------------------------------------------------------- */

static int jump_file(Chain *c, const char *name)
{
    drop_ring(c);
    if (c->o.one_level) { hist_add(c, name, 0); c->read_done = 1; c->ended = 1; return 1; }
    snprintf(c->pending_name, sizeof c->pending_name, "%s", name);
    c->pending = 1;
    c->pending_seg = 0;
    c->read_done = 1;
    return 1;
}

int chain_skip(Chain *c)
{
    if (c->ended || c->ticks - c->last_esc < ESC_TICKS) return 0;
    c->last_esc = c->ticks;
    /* Non-level files: the intro films chain LOGOS -> INTRO -> INTROB -> LEVEL01; the hub quits. */
    if (!strcmp(c->stem, "LEVEL00")) { drop_ring(c); c->ended = 1; return 2; }
    if (!strcmp(c->stem, "LOGOS"))  return jump_file(c, "INTRO");
    if (!strcmp(c->stem, "INTRO"))  return jump_file(c, "INTROB");
    if (!strcmp(c->stem, "INTROB")) return jump_file(c, "LEVEL01");

    int seg = chain_segment(c);
    int t = scene_esc(&c->sc, seg);
    if (t == SCENE_ESC_IGNORE) return 0;
    if (t == SCENE_ESC_HUB) return jump_file(c, "LEVEL00");
    drop_ring(c);
    c->pending = 0;
    if (t >= 0 && t < c->sc.n_segments) {
        c->rseg = t; c->rk = 0; c->read_done = 0;
        c->disp_seg = -1;
    } else {
        set_exit(c, t);
    }
    return 1;
}
