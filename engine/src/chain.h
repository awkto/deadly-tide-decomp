/*
 * chain.h - segment-chain playback: streams groups across segments and level
 * files following the scene graph (scene.h), decodes video and composites the
 * FG sprites (sprites.h) into a 320x200 XRGB8888 frame, one tick (1/15 s) at a time.
 *
 * Platform free: audio is handed out through a callback, the caller paces.
 * Read side (chain_fill) and display side (chain_step) are decoupled by a small
 * ring of groups so audio can be queued ahead of the picture.
 */
#ifndef DT_CHAIN_H
#define DT_CHAIN_H

#include <stddef.h>
#include <stdint.h>

#include "freelook.h"
#include "save.h"

#define CHAIN_W 320
#define CHAIN_H 200

struct Game;

typedef struct {
    const char *data_dir;   /* searched as DIR/NAME.L96 and DIR/cd* /NAME.L96 */
    const char *level;      /* file stem or file name, e.g. LEVEL01 or LEVEL01.L96 */
    int         from_seg;
    int         single;     /* stop after the first segment (legacy --file/--segment) */
    int         one_level;  /* stop when the chain leaves the starting file */
    uint32_t    seed;       /* branch PRNG seed */
    int         verbose;    /* print each segment as it starts (stderr) */
    int         auto_look;  /* headless shortcut (--auto-look): a segment that would loop onto
                             * itself (hub, LEVEL02 seg 8 ...) takes its first look-only (0x80)
                             * hotspot. Off by default: the view picks (Hotspot_Pick). */
    int         hub_between_levels; /* non-original: a finished LEVELnn returns to the hub LEVEL00
                                     * instead of chaining straight into the next level */
    int         demo;       /* attract mode (CurrentPlayer == "Demo"): start at LOGOS, follow
                             * LOGOS -> INTRO -> INTROB -> LEVEL01, then loop back to LOGOS */
    SaveData   *save;       /* pilot progress (save.h), may be NULL: no save points, hub lock 1 */
    struct Game *game;      /* combat layer (game.h), may be NULL: then branch conditions see a
                             * 'perfect player' (full health, every listed actor destroyed) */
} ChainOpts;

typedef struct Chain Chain;

/* pcm == NULL, n == 0: flush - drop the audio queued so far (the ring was dropped by a jump); the next
 * call carries the first group of the new stream. */
typedef void (*ChainAudioFn)(void *ud, const uint8_t *pcm, size_t n);

Chain *chain_open(const ChainOpts *o);   /* NULL on failure (message on stderr) */
void   chain_close(Chain *c);

/* Read groups ahead (at most `ahead` groups beyond the one on screen) and hand their
 * audio (u8 stereo, 22050 Hz, L96_AUDIO_BYTES per group; silence for audio-less groups)
 * to `fn` (may be NULL). Returns 0, or -1 on a read error. */
int chain_fill(Chain *c, int ahead, ChainAudioFn fn, void *ud);

/* Advance one tick. Returns 1 if a tick was played (*new_image set when the frame
 * buffer changed), 0 when the chain has ended, -1 on error. Calls chain_fill itself
 * only when nothing is buffered. */
int chain_step(Chain *c, int *new_image, ChainAudioFn fn, void *ud);

const uint32_t *chain_frame(const Chain *c);   /* CHAIN_W x CHAIN_H XRGB8888, the full composite */
/* The same picture as two layers (valid once a tick had a picture): the scene (video + in-world
 * sprites, free-look output) and the UI overlay (alpha 255 where drawn; no crosshair, see
 * game_draw_crosshair_at). Compositing overlay over scene gives chain_frame minus the crosshair. */
const uint32_t *chain_scene(const Chain *c);
const uint32_t *chain_overlay(const Chain *c);

/* ESC (Key_Skip). Returns 0 ignored, 1 jumped (buffered groups dropped: the caller
 * should drop queued audio too), 2 quit requested. */
int chain_skip(Chain *c);

/* F5 (0x414310): restart the current level from segment 0; on the frontier level SavePoint is
 * cleared. F2 (0x4143a0): back to the hub LEVEL00 (ignored in the hub). Both rate limited like
 * ESC. Return 1 when the chain jumped (drop queued audio), else 0. */
int chain_restart_level(Chain *c);
int chain_to_hub(Chain *c);

/* Nonzero once after the chain jumped on its own (death / kill branch): groups were dropped,
 * so the caller should drop queued audio too, as after chain_skip. */
int chain_take_jump(Chain *c);
/* The frame chain_step handled last: its group (counted from the last audio flush) and tick in it. */
void chain_shown(const Chain *c, int *group, int *tick);

/* Current position, for logs: file stem and segment on screen. */
const char *chain_level(const Chain *c);
int         chain_segment(const Chain *c);
/* Demo attract mode: completed passes back to LOGOS (0 unless ChainOpts.demo). */
int         chain_demo_loops(const Chain *c);
/* Space separated log of every segment started, e.g. "LEVEL01:0 LEVEL01:1 ... LEVEL02:0". */
const char *chain_history(const Chain *c);

/* Free-look (type 6) camera input: crosshair position in 320x200, default centred (160,100). */
void chain_set_look(Chain *c, const FreelookInput *in);
/* Headless scripted camera, "yaw,pitch[,roll];..." or "pan[:RATE]" (see FreelookScript).
 * Overrides chain_set_look. Returns 0, -1 on a syntax error. */
int  chain_set_look_script(Chain *c, const char *script);

#endif
