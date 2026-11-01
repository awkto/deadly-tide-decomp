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

Game *game_new(const GameOpts *o);
void  game_free(Game *g);

/* Level file opened: classes (table54), actors (table28), sprite sets, sounds, ACT ammo. 0 = ok. */
int  game_level_load(Game *g, const L96File *f, const SpriteCtx *spr);
/* Display side reached segment `seg` (record = its 100 raw bytes). Applies checkpoints. */
void game_segment_start(Game *g, int seg, const uint8_t *rec);

void game_frame_pre(Game *g, FgBlock *objs, const uint8_t *hdr);
/* `frame` = the composited 320x200 XRGB image (NULL / drawn=0 on frames without a picture). */
void game_frame_post(Game *g, FgBlock *objs, const uint8_t *hdr, uint32_t *frame, int drawn);

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
