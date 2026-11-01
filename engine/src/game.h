/*
 * game.h - combat and player logic of DTPLAYER.EXE (docs/COMBAT_SPEC.md): persistent actors fed
 * from the streamed FG records, player weapons and hit testing, enemy fire, health/death, HUD,
 * SFX triggers, and the live state the table44 branch conditions read.
 *
 * Platform free. The scene chain (chain.c) calls, per drawn frame:
 *   game_frame_pre   before compositing (Obj_ProjectAll + Actor_UpdateStates: swaps explosion sprites)
 *   game_frame_post  after compositing  (Enemy_Update, Hud_Draw, flash/shake, Player_Update, death timer)
 * Sound effects go out through a callback; input comes from an InputFn (input.h).
 */
#ifndef DT_GAME_H
#define DT_GAME_H

#include <stddef.h>
#include <stdint.h>

#include "input.h"
#include "l96.h"
#include "scene.h"
#include "sprites.h"

typedef struct Game Game;

/* pcm: u8 mono 22050 Hz. vol: hundredths of a dB (0 = full, negative = quieter).
 * pan: DirectSound units, -10000 (left) .. 10000 (right). */
typedef void (*SfxFn)(void *ud, const uint8_t *pcm, size_t n, int vol, int pan);

typedef struct {
    int     difficulty;      /* index d (COMBAT_SPEC §11): 0 normal, 1 easy, 2 hard */
    int     god;             /* attract-mode regeneration, never cleared */
    InputFn input;  void *input_ud;
    SfxFn   sfx;    void *sfx_ud;
    int     verbose;         /* log deaths, kill branches, lockouts to stderr */
} GameOpts;

typedef struct {
    long kills;              /* targets destroyed by the player, whole run (player.kills resets at checkpoints) */
    long shots, hits;        /* primary gun */
    long act_fired, act_kills;
    long damage;             /* health points lost (x100 units, i.e. 10000 = one full bar) */
    long deaths, lockouts;   /* death latches, heat lockouts */
    long enemy_shots, sfx;
} GameStats;

/* One targetable record as seen by the bot. */
typedef struct {
    int   cx, cy;            /* bbox centre, top-down */
    int   w, h;
    float dist2;             /* |camera - position|^2 */
    int   actor;
    int   avoid;             /* killing it branches to a restart clip (type 0, next 0): a friendly */
} GameTarget;

/* Type-6 view state, published by the scene chain every free-look frame (for the HUD's choice
 * window and for input sources that steer the camera, e.g. the bot). Angles in engine units. */
typedef struct {
    int active;              /* the segment on screen is type 6 */
    int mode;                /* control sub-mode, segment byte 0x21 */
    int yaw, pitch;          /* view centre */
    int on_window;           /* Hotspot_Pick != -1 (choice-window "on target") */
    int choice_open;         /* look-picks are accepted this frame (g_sceneFrame > seg[0x5c]) */
    int goal;                /* a look window the bot should take (index), -1 none */
    int goal_yaw, goal_pitch;/* its window centre */
    int goal_inside;         /* the view is inside the goal window */
    int turning;             /* a pick / auto turn drives the camera: input is ignored */
} LookInfo;

Game *game_new(const GameOpts *o);
void  game_free(Game *g);

/* Level file opened: classes (table54), actors (table28), sprite sets, sounds, ACT ammo. 0 = ok. */
int  game_level_load(Game *g, const L96File *f, const SpriteCtx *spr);
/* Display side reached segment `seg` (record = its 100 raw bytes). Applies checkpoints. */
void game_segment_start(Game *g, int seg, const uint8_t *rec);

void game_frame_pre(Game *g, FgBlock *objs, const uint8_t *hdr);
/* `frame` = the composited 320x200 XRGB image (NULL / drawn=0 on frames without a picture). */
void game_frame_post(Game *g, FgBlock *objs, const uint8_t *hdr, uint32_t *frame, int drawn);

/* Type-6 Player_Update (0x410740 case 6) pieces; the chain runs the camera and the pick between
 * them. poll: Input_Poll (crosshair + buttons). fire: Weapon_Fire, when no pick is pending. */
const InputState *game_poll_input(Game *g, const FgBlock *objs);
void game_look_fire(Game *g, FgBlock *objs);
void game_set_look(Game *g, const LookInfo *li);
const LookInfo *game_look(const Game *g);

/* Save points (SCENE_GRAPH §3). checkpoint: the resume reset of Play_SceneLoop (gun, health =
 * 10000, kills = 0, actors re-rolled). ACT charges: get / set. */
void game_checkpoint(Game *g);
int  game_act_ammo(const Game *g);
void game_set_act_ammo(Game *g, int n);

/* g_branchReq: nonzero when a death / kill branch is pending. take() consumes it. */
int  game_branch_pending(const Game *g);
int  game_take_branch(Game *g, int *target);

/* Live inputs of Branch_Resolve (COMBAT_SPEC §10). */
void game_fill_state(const Game *g, GameState *gs);

const GameStats *game_stats(const Game *g);
void game_print_stats(const Game *g, FILE *fp);

/* Read-only queries for input sources. */
int  game_targets(const Game *g, const FgBlock *objs, GameTarget *out, int max);
int  game_weapon(const Game *g);       /* seg[0x12], -1 when there is no primary gun */
int  game_heat(const Game *g);         /* -120..240 */
int  game_charge_count(const Game *g);
int  game_act_ready(const Game *g);    /* ammo > 0 and no ACT in flight */
int  game_shot_hist(const Game *g);

#endif
