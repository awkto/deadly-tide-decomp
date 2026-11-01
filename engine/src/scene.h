/*
 * scene.h - segment records, the table44 branch/hotspot table and the
 * successor rules of the reader thread (docs/SCENE_GRAPH.md).
 * Reference implementation: tools/l96.py (decode_segment, decode_hotspot, graph).
 *
 * Pure logic: no SDL, no I/O after scene_load().
 */
#ifndef DT_SCENE_H
#define DT_SCENE_H

#include <stdint.h>

#include "l96.h"

#define SCENE_HS_REC 0x44

enum { SEG_VIDEO = 0, SEG_RAIL = 1, SEG_PANO = 2, SEG_CUTSCENE = 5, SEG_FREELOOK = 6 };

/* Table44 flag bits (SCENE_GRAPH §2). */
#define HS_XHAIR      0x001u
#define HS_HP_GT      0x002u
#define HS_HP_LT      0x004u
#define HS_KILLS_GT   0x008u
#define HS_KILLS_LT   0x010u
#define HS_ANY_ALIVE  0x020u
#define HS_NONE_ALIVE 0x040u
#define HS_LOOK       0x080u
#define HS_FIRST_DEP  0x100u
#define HS_RANDOM     0x200u
#define HS_OR         0x80000000u

typedef struct {
    int32_t  target;          /* segment, negative = linked name names[-1-target] */
    uint32_t flags;
    int32_t  hp, kills;       /* thresholds */
    int16_t  actors_a[6], actors_b[6];   /* table28 indices, -1 unused */
    int16_t  x0, y0, x1, y1;  /* look window or crosshair rectangle */
    uint8_t  chance;          /* % for HS_RANDOM */
} Hotspot;

typedef struct {
    int     type, next, esc, death;
    int     hs_first, hs_count;
    int     save, reset;
    int     aim, xhair;
    int16_t half_fov;         /* 1/512 degree */
} SegInfo;

typedef struct {
    int       n_segments, n_hotspots;
    SegInfo  *seg;
    Hotspot  *hs;
} Scene;

/* Gameplay state the branch conditions look at. Until combat exists the caller
 * passes scene_default_state(): full health, no kills, crosshair centred, all
 * listed actors destroyed. */
typedef struct {
    int health;               /* 0..10000 */
    int kills;                /* player+0x200 */
    int frames_shown, frames_dropped;
    int xhair_x, xhair_y;     /* player+0x1d0 / +0x1d4 */
    const int8_t *actor_state;/* table28 runtime state byte per actor (< -0x40 = alive), may be NULL */
    int n_actors;
} GameState;

int  scene_load(Scene *sc, const L96File *f);   /* 0 on success */
void scene_free(Scene *sc);
GameState scene_default_state(void);

/* Deterministic PRNG (the original uses MSVC rand(); we only need the same distribution). */
void scene_srand(uint32_t seed);
int  scene_rand(void);        /* 0..0x7fff like MSVC rand() */

/* Branch_Resolve 0x40e380: first table44 record of `seg` whose condition holds, else `next`. */
int  scene_branch_resolve(const Scene *sc, int seg, const GameState *gs);

/* Branch_AutoPick 0x40e7f0: type-6 start roll over the random records (last winning one,
 * else `next`). Returns INT_MIN when the segment has no random record (nothing is cached). */
int  scene_auto_pick(const Scene *sc, int seg);

/* Successor of `seg` after its last group, per the reader thread rules (SCENE_GRAPH §5).
 * For type 6 segments pass the result of scene_auto_pick() rolled at its start in `picked`
 * (INT_MIN = resolve now). */
int  scene_successor(const Scene *sc, int seg, const GameState *gs, int picked);

/* Hotspot_Pick 0x40df80 for a type-6 view: index (0-based within the segment) of the first
 * record with the 0x80 bit whose yaw window (x0..x1, inclusive, wraps when x0 > x1) and pitch
 * window (y0..y1) contain (yaw, pitch); -1 if none. `hub` (file LEVEL00.L96): record i is only
 * enabled when i == 0, i == 15 or i < highest (the level-select lock). */
int  scene_hotspot_pick(const Scene *sc, int seg, int yaw, int pitch, int hub, int highest);
/* Is record i of `seg` a look window that is enabled under the hub lock? */
int  scene_look_enabled(const Scene *sc, int seg, int i, int hub, int highest);

/* Key_Skip 0x414130 inside a level file. Returns:
 *   >= 0           branch to that segment
 *   SCENE_ESC_IGNORE  nothing happens
 *   SCENE_ESC_HUB     leave the level for LEVEL00 */
#define SCENE_ESC_IGNORE (-1000)
#define SCENE_ESC_HUB    (-1001)
int  scene_esc(const Scene *sc, int seg);

#endif
