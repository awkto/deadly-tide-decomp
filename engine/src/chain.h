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
    int         no_auto_look; /* stand-in until look-picking exists: a segment that would loop
                               * onto itself (hub, LEVEL02 seg 8 ...) takes its first look-only
                               * (0x80) hotspot instead. Set to keep looping like the engine. */
    struct Game *game;      /* combat layer (game.h), may be NULL: then branch conditions see a
                             * 'perfect player' (full health, every listed actor destroyed) */
} ChainOpts;

typedef struct Chain Chain;

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

const uint32_t *chain_frame(const Chain *c);   /* CHAIN_W x CHAIN_H XRGB8888 */

/* ESC (Key_Skip). Returns 0 ignored, 1 jumped (buffered groups dropped: the caller
 * should drop queued audio too), 2 quit requested. */
int chain_skip(Chain *c);

/* Nonzero once after the chain jumped on its own (death / kill branch): groups were dropped,
 * so the caller should drop queued audio too, as after chain_skip. */
int chain_take_jump(Chain *c);

/* Current position, for logs: file stem and segment on screen. */
const char *chain_level(const Chain *c);
int         chain_segment(const Chain *c);
/* Space separated log of every segment started, e.g. "LEVEL01:0 LEVEL01:1 ... LEVEL02:0". */
const char *chain_history(const Chain *c);

#endif
