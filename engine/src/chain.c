/* chain.c - see chain.h. Flow rules: docs/SCENE_GRAPH.md §4-5. */
#include "chain.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cinepak.h"
#include "game.h"
#include "l96.h"
#include "scene.h"
#include "sprites.h"

#define RING 16
#define ESC_TICKS 15        /* Key_Skip is rate limited to once per second */
#define BRANCH_LEAD 2       /* with combat: Branch_Resolve samples the game state once at most this many
                             * groups are buffered (the original reader ran 1-2 groups ahead) */

typedef struct {
    L96Group g;
    int seg, k;
    int tex;                /* type-6 groups: free-look tile preload generation, -1 none */
    int auto_pick;          /* type 6: Branch_AutoPick result rolled by the reader (INT_MIN none) */
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
    int  accumulate;               /* collecting the free-look tiles for the following type-6 segment */
    uint8_t *pre[2];               /* FL_NTILES carry tile slots per preload, by generation & 1 */
    int  pre_gen;                  /* completed preloads (0 = none yet) */

    /* Display side. */
    int  vf, disp_seg;
    FgBlock blocks[L96_FRAMES_PER_GROUP];
    int  nblocks;
    CinepakDecoder dec;
    int  dec_ready;
    uint32_t frame[CHAIN_W * CHAIN_H];
    long ticks, last_esc;
    int  jumped;

    /* Free-look (freelook.h). */
    Freelook       fl;
    FreelookInput  look;
    FreelookScript script;
    int  fl_gen, fl_frame, fl_entry[3];

    /* Look-pick / auto-turn state (Player_Update 0x410740 case 6). */
    int  seg_frame;                /* g_sceneFrame of the type-6 segment on screen */
    int  auto_target;              /* 0x4212a8: Branch_AutoPick result, 0 = none */
    int  look_picked;              /* 0x4212ac: a pick (or a random roll) is final */
    int  look_target;              /* 0x4212a4: picked segment, -1 none */
    int  look_turn;                /* frames left in the pick turn, then the cut */
    int  look_branch;              /* cut pending: apply at the next step */
    int  goal;                     /* bot: look window to take (index), -1 none */

    int  demo_loops;               /* demo attract: completed passes back to LOGOS */

    /* Save state (SCENE_GRAPH §3). */
    int  cur_level;                /* g_curLevelNum 0x4215b4: NN of the last LevelNN file */
    int  ctrl_pick;                /* 0x4213ac: Ctrl held during the hub pick */
    long last_fkey;

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
        if (!l96_exists(path)) continue;
        if (l96_open(&c->f, path) != 0) return -1;
        if (scene_load(&c->sc, &c->f) != 0 || sprites_load(&c->spr, &c->f) != 0) {
            fprintf(stderr, "chain: cannot load tables of %s\n", path);
            scene_free(&c->sc); l96_close(&c->f);
            return -1;
        }
        snprintf(c->stem, sizeof c->stem, "%s", stem);
        c->open = 1;
        /* Session_Run 0x414420: g_curLevelNum = NN for LevelNN files (other files keep it). A
         * level entered from the hub with Ctrl held starts from its beginning on the frontier. */
        if (!strncmp(stem, "LEVEL", 5) || !strncmp(stem, "Level", 5) || !strncmp(stem, "level", 5)) {
            c->cur_level = atoi(stem + 5);
            if (c->ctrl_pick && c->cur_level != 0 && c->o.save &&
                c->o.save->highest_level == c->cur_level && c->o.save->save_point) {
                c->o.save->save_point = 0;
                save_store(c->o.save);
                if (c->o.verbose) fprintf(stderr, "save: Ctrl pick, %s restarts from its beginning\n", stem);
            }
        }
        c->ctrl_pick = 0;
        if (c->o.game && game_level_load(c->o.game, &c->f, &c->spr) != 0) {
            fprintf(stderr, "chain: cannot load the gameplay tables of %s\n", path);
            return -1;
        }
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

static int frontier(const Chain *c) { return c->o.save && c->cur_level == c->o.save->highest_level; }

/* Reader thread 0x401650: before streaming segment 0 or a `reset` segment, read SavePoint; on the
 * frontier level with SavePoint != 0 stream that segment instead. */
static void start_read(Chain *c, int seg)
{
    if (!c->o.single && frontier(c) && (seg == 0 || c->sc.seg[seg].reset)) {
        int sp = c->o.save->save_point;
        if (sp > 0 && sp < c->sc.n_segments && sp != seg) {
            if (c->o.verbose) fprintf(stderr, "save: %s:%d -> resume at save point %d\n", c->stem, seg, sp);
            seg = sp;
        }
    }
    c->rseg = seg;
    c->rk = 0;
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
    c->look = (FreelookInput){ CHAIN_W / 2, CHAIN_H / 2 };
    if (fl_init(&c->fl) != 0 || !(c->pre[0] = calloc(2, (size_t)FL_NTILES * FL_TILE_SLOT))) {
        fl_free(&c->fl); free(c); return NULL;
    }
    c->pre[1] = c->pre[0] + (size_t)FL_NTILES * FL_TILE_SLOT;
    c->last_esc = -ESC_TICKS;
    c->gs = scene_default_state();
    scene_srand(o->seed);
    stem_of(o->level, c->start_stem);
    if (open_level(c, c->start_stem) != 0) { chain_close(c); return NULL; }
    if (o->from_seg < 0 || (uint32_t)o->from_seg >= c->f.n_segments) {
        fprintf(stderr, "chain: segment %d out of range (%s has %u)\n", o->from_seg, c->stem, c->f.n_segments);
        chain_close(c); return NULL;
    }
    c->look_target = -1;
    c->goal = -1;
    c->last_fkey = -ESC_TICKS;
    start_read(c, o->from_seg);
    return c;
}

void chain_close(Chain *c)
{
    if (!c) return;
    drop_ring(c);
    close_level(c);
    if (c->dec_ready) cinepak_free(&c->dec);
    free(c->pre[0]);
    fl_free(&c->fl);
    free(c->hist);
    free(c);
}

/* ---- read side ---------------------------------------------------------------- */

/* The last 20 groups in front of a type-6 segment carry its 96 texture tiles, 5 per group
 * (1 in the last), FL_TILE_SLOT bytes each (Play_FreeLookEnter 0x407a70). Keep the raw slots;
 * they are decoded when the type-6 segment reaches the screen. */
static void collect_tiles(Chain *c, int count, int k, const L96Group *g)
{
    int gi = k - (count - FL_PRELOAD_GROUPS);
    if (gi < 0) return;
    uint8_t *dst = c->pre[(c->pre_gen + 1) & 1];
    for (int t = 0; t < (gi == FL_PRELOAD_GROUPS - 1 ? 1 : 5); t++) {
        size_t off = (size_t)t * FL_TILE_SLOT;
        uint8_t *d = dst + (size_t)(gi * 5 + t) * FL_TILE_SLOT;
        if (off + FL_TILE_SLOT <= g->carry_size) memcpy(d, g->carry + off, FL_TILE_SLOT);
        else memset(d, 0, FL_TILE_SLOT);
    }
}

/* The type 0/1 segment whose `next` is type-6 segment `seg` (the one carrying its tiles). */
static int freelook_pred(const Chain *c, int seg)
{
    if (seg > 0 && c->sc.seg[seg - 1].next == seg && c->sc.seg[seg - 1].type != SEG_FREELOOK) return seg - 1;
    for (int i = 0; i < c->sc.n_segments; i++)
        if (c->sc.seg[i].next == seg && c->sc.seg[i].type != SEG_FREELOOK) return i;
    return -1;
}

/* Starting the chain on a type-6 segment: read the tiles from its predecessor directly. */
static void preload_pred(Chain *c, int seg)
{
    int p = freelook_pred(c, seg);
    if (p < 0) return;
    int n = c->f.segments[p].count;
    for (int k = n > FL_PRELOAD_GROUPS ? n - FL_PRELOAD_GROUPS : 0; k < n; k++) {
        L96Group g;
        memset(&g, 0, sizeof g);
        if (l96_read_group(&c->f, p, k, &g) != 0) return;
        collect_tiles(c, n, k, &g);
        l96_group_free(&g);
    }
    c->pre_gen++;
}

static void set_exit(Chain *c, int target)
{
    c->read_done = 1;
    if (c->o.single) return;
    int idx = -1 - target;
    if (idx < 0 || (uint32_t)idx >= c->f.n_names) { fprintf(stderr, "chain: bad exit %d\n", target); return; }
    const char *name = c->f.names[idx];
    /* Demo attract (SCENE_GRAPH §5): LEVEL01 ends the pass; the reader replaces the file with
     * LOGOS so the loop repeats. */
    if (c->o.demo && !strcmp(c->stem, "LEVEL01")) name = "LOGOS";
    /* Level complete: the finished frontier level raises HighestLevel and clears SavePoint. */
    if (frontier(c)) {
        c->o.save->highest_level++;
        c->o.save->save_point = 0;
        save_store(c->o.save);
        fprintf(stderr, "save: %s complete -> HighestLevel %d, SavePoint 0\n", c->stem, c->o.save->highest_level);
    }
    if (!strcmp(name, "SUCCESS")) { fprintf(stderr, "chain: SUCCESS - game complete\n"); return; }
    if (c->o.hub_between_levels && strcmp(c->stem, "LEVEL00") && !strncmp(name, "LEVEL", 5)) name = "LEVEL00";
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
    if (c->o.demo && !strcmp(name, "LOGOS")) c->demo_loops++;   /* one attract pass done */
    start_read(c, c->pending_seg < (int)c->f.n_segments ? c->pending_seg : 0);
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
            if (si->type == SEG_FREELOOK && c->pre_gen == 0) preload_pred(c, c->rseg);
        }
        if (c->rk < sg->count) {
            Slot *s = &c->ring[(c->head + c->n) % RING];
            if (l96_read_group(&c->f, c->rseg, c->rk, &s->g) != 0) return -1;
            s->seg = c->rseg; s->k = c->rk;
            s->tex = si->type == SEG_FREELOOK ? c->pre_gen : -1;
            s->auto_pick = c->picked;
            if (c->accumulate) collect_tiles(c, sg->count, c->rk, &s->g);
            if (fn) {
                if (s->g.audio_size >= L96_AUDIO_BYTES) fn(ud, s->g.audio, L96_AUDIO_BYTES);
                else fn(ud, g_silence, L96_AUDIO_BYTES);   /* keep the audio clock in step */
            }
            c->n++;
            c->rk++;
        }
        if (c->rk >= sg->count) {
            if (c->o.game && !c->o.single) {
                /* Resolve only when the display side is within BRANCH_LEAD groups of the end, with no
                 * death / kill request pending (that one wins: chain_step jumps). */
                if (game_branch_pending(c->o.game) || c->n > BRANCH_LEAD) break;
                game_fill_state(c->o.game, &c->gs);
            }
            if (c->accumulate) c->pre_gen++;
            if (c->o.single) { c->read_done = 1; continue; }
            int nxt = scene_successor(&c->sc, c->rseg, &c->gs, c->picked);
            if (nxt == c->rseg && c->o.auto_look) {
                /* Without input a look-choice segment repeats forever: pick its first window. */
                const SegInfo *cs = &c->sc.seg[c->rseg];
                for (int i = 0; i < cs->hs_count; i++)
                    if (c->sc.hs[cs->hs_first + i].flags & HS_LOOK) {
                        nxt = c->sc.hs[cs->hs_first + i].target;
                        if (c->o.verbose) fprintf(stderr, "auto-look: %s:%d -> %d\n", c->stem, c->rseg, nxt);
                        break;
                    }
            }
            if (c->o.demo && nxt == 0 && !strcmp(c->stem, "LEVEL01")) {
                /* LEVEL01 streams its own segment 0 again: end the demo pass back at LOGOS. */
                snprintf(c->pending_name, sizeof c->pending_name, "%s", "LOGOS");
                c->pending = 1; c->pending_seg = 0; c->read_done = 1;
            } else if (nxt >= 0 && nxt < c->sc.n_segments) start_read(c, nxt);
            else set_exit(c, nxt);
        }
    }
    if (c->read_done && !c->pending && c->n == 0) c->ended = 1;
    return 0;
}

/* ---- display side ------------------------------------------------------------- */

static int rd16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static int seg_type_of(const Chain *c, int s) { return s >= 0 && s < c->sc.n_segments ? c->sc.seg[s].type : -1; }

static int highest(const Chain *c) { return c->o.save ? c->o.save->highest_level : 1; }

/* Centre of look window i of `seg` (yaw window may wrap). */
static void window_centre(const Chain *c, int seg, int i, int *yaw, int *pitch)
{
    const Hotspot *h = &c->sc.hs[c->sc.seg[seg].hs_first + i];
    int span = h->x1 - h->x0;
    if (span < 0) span += FL_UNITS;
    *yaw = (h->x0 + span / 2) % FL_UNITS;
    *pitch = (h->y0 + h->y1) / 2;
}

/* Bot goal for a look-choice segment: in the hub the highest unlocked mission (record
 * HighestLevel-1, records 0..14 launch LEVEL01..15); elsewhere an enabled window chosen by --seed. */
static int choose_goal(const Chain *c, int seg)
{
    const SegInfo *si = &c->sc.seg[seg];
    int hub = c->fl.hub, hl = highest(c), list[64], n = 0;
    for (int i = 0; i < si->hs_count && n < 64; i++)
        if (scene_look_enabled(&c->sc, seg, i, hub, hl)) list[n++] = i;
    if (!n) return -1;
    if (hub) {
        int want = hl - 1 < 0 ? 0 : hl - 1 > 14 ? 14 : hl - 1;
        return scene_look_enabled(&c->sc, seg, want, hub, hl) ? want : list[0];
    }
    return list[c->o.seed % (uint32_t)n];
}

static int frames_left_of(const Chain *c, const Slot *s)
{
    return (c->f.segments[s->seg].count - s->k) * L96_FRAMES_PER_GROUP - c->vf - 1;
}

/* Start of a type-6 tick: on entry decode its tiles and View_Set the entry pose (previous
 * segment's 0x18/0x1a/0x46), reset the look-pick state per segment (and per self-loop pass),
 * then FreeLook_FixupObjects so the actor update and hit tests see this frame's screen boxes. */
static void freelook_begin(Chain *c, const Slot *s, int prev_seg, FgBlock *blk)
{
    Freelook *fl = &c->fl;
    if (c->vf == 0 && s->seg != prev_seg) {
        if (s->tex != c->fl_gen && s->tex > 0) {
            const uint8_t *src = c->pre[s->tex & 1];
            for (int t = 0; t < FL_NTILES; t++)
                if (fl_load_tile(fl, t, src + (size_t)t * FL_TILE_SLOT, FL_TILE_SLOT) != 0)
                    fprintf(stderr, "freelook: tile %d of %s:%d not decodable\n", t, c->stem, s->seg);
            c->fl_gen = s->tex;
        }
        int p = prev_seg >= 0 ? prev_seg : freelook_pred(c, s->seg);
        const uint8_t *raw = p >= 0 ? c->f.segments[p].raw : NULL;
        c->fl_entry[0] = raw ? rd16s(raw + 0x18) : 0;
        c->fl_entry[1] = raw ? rd16s(raw + 0x1a) : 0;
        c->fl_entry[2] = raw ? rd16s(raw + 0x46) : 0;
        fl_view_set(fl, c->fl_entry[0], c->fl_entry[1], c->fl_entry[2]);
        fl->mode = c->f.segments[s->seg].raw[0x21];
        fl->half_fov = c->sc.seg[s->seg].half_fov;
        fl->hub = !strcmp(c->stem, "LEVEL00");
        fl->highest_level = highest(c);
        c->fl_frame = 0;
        c->look_turn = 0;
        c->look_picked = 0;
        c->look_target = -1;
        fl_turn_reset(fl);
        c->goal = choose_goal(c, s->seg);
    }
    if (c->script.n || c->script.pan)      /* headless scripted camera overrides the controls */
        fl_script_pose(fl, &c->script, c->fl_frame, c->fl_entry[0], c->fl_entry[1], c->fl_entry[2]);
    if (c->vf == 0 && s->k == 0) {
        /* New pass (Player_Update: framesLeft grew): clear the pick state, unless a pick turn is
         * still running across the hub's self-loop. */
        c->seg_frame = 0;
        if (!c->look_turn) {
            c->look_picked = 0;
            c->look_target = -1;
            fl->auto_on = fl->plan_on = 0;
        }
        /* Branch_AutoPick 0x40e7f0: no records -> 0; else the random winner or `next`. A random
         * roll makes the choice final (0x4212ac = 1): no look-picks in that segment. */
        const SegInfo *si = &c->sc.seg[s->seg];
        c->auto_target = !si->hs_count ? 0 : s->auto_pick != INT_MIN ? s->auto_pick : si->next;
        if (s->auto_pick != INT_MIN) c->look_picked = 1;
    }
    fl_fixup_objects(fl, blk);
}

static void freelook_render(Chain *c, const FgBlock *blk)
{
    fl_frame(&c->fl, &c->spr, blk, c->frame, 1);
}

static void publish_look(Chain *c, int seg, int active)
{
    if (!c->o.game) return;
    LookInfo li;
    memset(&li, 0, sizeof li);
    li.goal = -1;
    if (active) {
        const Freelook *fl = &c->fl;
        int choice = rd16s(c->f.segments[seg].raw + 0x5c);
        li.active = 1;
        li.mode = fl->mode;
        li.yaw = fl->yaw; li.pitch = fl->pitch;
        li.on_window = scene_hotspot_pick(&c->sc, seg, fl->yaw, fl->pitch, fl->hub, highest(c)) >= 0;
        li.choice_open = !c->look_picked && c->seg_frame + 1 > choice;
        li.turning = c->look_turn > 0;
        if (c->goal >= 0 && !c->look_picked) {
            li.goal = c->goal;
            window_centre(c, seg, c->goal, &li.goal_yaw, &li.goal_pitch);
            const Hotspot *h = &c->sc.hs[c->sc.seg[seg].hs_first + c->goal];
            int in_yaw = h->x0 < h->x1 ? h->x0 <= fl->yaw && fl->yaw <= h->x1 : h->x0 <= fl->yaw || fl->yaw <= h->x1;
            li.goal_inside = in_yaw && h->y0 <= fl->pitch && fl->pitch <= h->y1;
        }
    }
    game_set_look(c->o.game, &li);
}

/* Player_Update 0x410740 case 6, after the frame was shown: Input_Poll, camera control (pick
 * turn / auto-turn / free look), View_Clamp6, the look-pick check, then Weapon_Fire. */
static void freelook_update(Chain *c, const Slot *s, FgBlock *blk)
{
    Freelook *fl = &c->fl;
    const SegInfo *si = &c->sc.seg[s->seg];
    int frames_left = frames_left_of(c, s);
    InputState in = { c->look.cx, c->look.cy, 0, 0 };
    if (c->o.game) in = *game_poll_input(c->o.game, blk);

    if (c->look_turn > 0) {
        c->look_turn = fl_turn_step(fl);
        if (c->look_turn == 0) {
            c->look_branch = 1;          /* g_branchReq = pick, applied at the next step */
            if (c->o.verbose) fprintf(stderr, "look-pick: %s:%d -> %d\n", c->stem, s->seg, c->look_target);
        }
    } else if (c->script.n || c->script.pan) {
        /* pose set by freelook_begin */
    } else {
        int driven = 0;
        if (c->look_target < 0) {
            /* Control1 auto-turns only to autoTarget; the 2/3 planner falls back to `next`. */
            int tgt = c->auto_target;
            if (tgt < 1 && fl->mode >= 2 && fl->mode <= 3) tgt = si->next;
            if (tgt >= 1 && seg_type_of(c, tgt) != SEG_FREELOOK && seg_type_of(c, tgt) >= 0) {
                const uint8_t *tr = c->f.segments[tgt].raw;
                driven = fl_auto_turn(fl, rd16s(tr + 0x1c), rd16s(tr + 0x1e), rd16s(tr + 0x48), frames_left);
            }
        }
        if (!driven) fl_control(fl, &(FreelookInput){ in.x, in.y });
    }

    /* Pick: fire or alt inside a window once g_sceneFrame > seg[0x5c] (Branch_PickWithCheat). */
    int choice = rd16s(c->f.segments[s->seg].raw + 0x5c);
    if (!c->look_picked && (in.buttons & (IN_FIRE | IN_ALT)) && c->seg_frame > choice) {
        int i = scene_hotspot_pick(&c->sc, s->seg, fl->yaw, fl->pitch, fl->hub, highest(c));
        if (i >= 0) {
            int tgt = c->sc.hs[si->hs_first + i].target;
            c->look_picked = 1;
            c->look_target = tgt;
            if (fl->hub && (in.buttons & IN_CTRL)) c->ctrl_pick = 1;
            int ty = fl->yaw, tp = fl->pitch, tr = 0;
            if (tgt >= 0 && tgt < c->sc.n_segments) {
                const uint8_t *r = c->f.segments[tgt].raw;
                ty = rd16s(r + 0x1c); tp = rd16s(r + 0x1e); tr = rd16s(r + 0x48);
            }
            int n = fl_turn_begin(fl, ty, tp, tr, frames_left);
            c->look_turn = n;
            if (c->o.verbose)
                fprintf(stderr, "look-pick: %s:%d window %d -> %d (turn %d frames%s)\n", c->stem, s->seg, i, tgt, n,
                        c->ctrl_pick ? ", Ctrl" : "");
            publish_look(c, s->seg, 1);
            c->fl_frame++; c->seg_frame++;
            return;                       /* this press fires nothing */
        }
    }
    if (c->o.game && c->look_target < 0) game_look_fire(c->o.game, blk);
    if (c->o.verbose && frames_left == 0)
        fprintf(stderr, "freelook %s:%d last frame pose %d,%d,%d (auto target %d)\n", c->stem, s->seg, fl->yaw,
                fl->pitch, fl->roll, c->auto_target);
    c->fl_frame++; c->seg_frame++;
    publish_look(c, s->seg, 1);
}

static void apply_branch(Chain *c, int target)
{
    if (c->o.verbose) fprintf(stderr, "branch request: %s:%d -> %d\n", c->stem, chain_segment(c), target);
    drop_ring(c);
    c->pending = 0;
    c->jumped = 1;
    if (target >= 0 && target < c->sc.n_segments) {
        start_read(c, target); c->read_done = 0;
        c->disp_seg = -1;
    } else {
        set_exit(c, target);
    }
}

int chain_take_jump(Chain *c)
{
    int j = c->jumped;
    c->jumped = 0;
    return j;
}

void chain_set_look(Chain *c, const FreelookInput *in) { c->look = *in; }
int  chain_set_look_script(Chain *c, const char *script) { return fl_script_parse(&c->script, script); }

/* Play_SceneLoop 0x406020 on entering a segment, frontier level only: a `save` segment past
 * SavePoint becomes the new SavePoint (ACT charges saved with it); reaching the SavePoint itself
 * (we resumed there) resets gun, health, kills and the actors and reloads the ACT charges.
 * Checkpoints (segment 0 / `reset`) take the ACT charges from the save when one exists. */
static void save_enter(Chain *c, int seg)
{
    if (!frontier(c)) return;
    SaveData *sv = c->o.save;
    const SegInfo *si = &c->sc.seg[seg];
    Game *g = c->o.game;
    if (si->save) {
        if (seg > sv->save_point) {
            sv->save_point = seg;
            if (g) sv->act_num = game_act_ammo(g);
            save_store(sv);
            fprintf(stderr, "save: %s:%d is the new save point (ACT %d)\n", c->stem, seg, sv->act_num);
        } else if (seg == sv->save_point) {
            if (g) { game_checkpoint(g); game_set_act_ammo(g, sv->act_num); }
            fprintf(stderr, "save: resumed %s at save point %d\n", c->stem, seg);
        }
    } else if ((seg == 0 || si->reset) && sv->save_point && g) {
        game_set_act_ammo(g, sv->act_num);
    }
}

int chain_step(Chain *c, int *new_image, ChainAudioFn fn, void *ud)
{
    *new_image = 0;
    int req;
    if (c->look_branch) {
        c->look_branch = 0;
        int t = c->look_target;
        c->look_target = -1;
        apply_branch(c, t);
    }
    if (c->o.game && game_take_branch(c->o.game, &req)) apply_branch(c, req);
    if (c->n == 0 && chain_fill(c, 1, fn, ud) != 0) return -1;
    if (c->n == 0) return 0;

    Slot *s = &c->ring[c->head];
    const SegInfo *si = &c->sc.seg[s->seg];
    int nticks = s->g.n_frames ? s->g.n_frames : L96_FRAMES_PER_GROUP;

    int prev_seg = c->disp_seg;
    if (c->vf == 0) {
        if (s->seg != c->disp_seg || s->k == 0) {
            c->disp_seg = s->seg;
            hist_add(c, c->stem, s->seg);
            if (c->o.game) game_segment_start(c->o.game, s->seg, c->f.segments[s->seg].raw);
            save_enter(c, s->seg);
            if (si->type != SEG_FREELOOK) publish_look(c, s->seg, 0);
            if (c->o.verbose)
                fprintf(stderr, "segment %s:%d type %d groups %d (tick %ld)\n", c->stem, s->seg, si->type,
                        c->f.segments[s->seg].count, c->ticks);
        }
        c->nblocks = s->g.fg_size ? fg_parse(s->g.fg, s->g.fg_size, c->blocks, L96_FRAMES_PER_GROUP) : 0;
    }

    FgBlock *blk = c->vf < c->nblocks ? &c->blocks[c->vf] : NULL;
    const uint8_t *fhdr = s->g.n_frames ? s->g.frame[c->vf].hdr : NULL;
    int freelook = !s->g.n_frames && si->type == SEG_FREELOOK;
    if (freelook) freelook_begin(c, s, prev_seg, blk);
    if (c->o.game) game_frame_pre(c->o.game, blk, fhdr);   /* actor states, explosion sprites */
    int drawn = 0;

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
            /* With combat, kind 8 is drawn by the game's Hud_Draw (after the gun flash). */
            fg_composite(&c->spr, &c->blocks[c->vf], c->frame, CHAIN_W, CHAIN_H, !c->o.game && (flags & 1) != 0);
        }
        *new_image = 1;
        drawn = 1;
    } else if (freelook) {
        freelook_render(c, blk);
        *new_image = 1;
        drawn = 1;
    }

    if (c->o.game) game_frame_post(c->o.game, blk, fhdr, c->frame, drawn && si->type != SEG_PANO);
    if (freelook) freelook_update(c, s, blk);

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
int chain_demo_loops(const Chain *c) { return c->demo_loops; }
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
    if (t == SCENE_ESC_HUB) return jump_file(c, c->o.demo ? "LOGOS" : "LEVEL00");
    drop_ring(c);
    c->pending = 0;
    if (t >= 0 && t < c->sc.n_segments) {
        start_read(c, t); c->read_done = 0;
        c->disp_seg = -1;
    } else {
        set_exit(c, t);
    }
    return 1;
}

int chain_restart_level(Chain *c)
{
    if (c->ended || c->ticks - c->last_fkey < ESC_TICKS) return 0;
    if (c->o.game && game_branch_pending(c->o.game)) return 0;
    c->last_fkey = c->ticks;
    if (frontier(c) && c->o.save->save_point) { c->o.save->save_point = 0; save_store(c->o.save); }
    if (c->o.verbose) fprintf(stderr, "F5: restart %s\n", c->stem);
    drop_ring(c);
    c->pending = 0;
    c->jumped = 1;
    start_read(c, 0);
    c->read_done = 0;
    c->disp_seg = -1;
    return 1;
}

int chain_to_hub(Chain *c)
{
    if (c->ended || c->ticks - c->last_fkey < ESC_TICKS || !strcmp(c->stem, "LEVEL00")) return 0;
    c->last_fkey = c->ticks;
    if (c->o.verbose) fprintf(stderr, "F2: %s -> LEVEL00\n", c->stem);
    c->jumped = 1;
    return jump_file(c, "LEVEL00");
}
