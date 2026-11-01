/*
 * input.h - player input abstraction (COMBAT_SPEC §3).
 *
 * The game polls an InputFn on the frames where the original calls Input_Poll (type-1 aim
 * frames). Sources: the SDL mouse (main.c over platform.h), and the scripted headless
 * sources in bot.c (--bot aimbot, --idle).
 */
#ifndef DT_INPUT_H
#define DT_INPUT_H

#include <stdint.h>

#define IN_FIRE 1u    /* buttons bit 0: fire (left mouse, joystick button 1) */
#define IN_ALT  2u    /* bit 1: alt / path pick (right mouse, button 2) */
#define IN_ACT  4u    /* bit 2: ACT secondary weapon (button 3) */

typedef struct {
    int      x, y;        /* crosshair, 0..319 x 0..199, top-down */
    uint32_t buttons;     /* IN_* bits */
    int      act;         /* Space: sets the ACT request directly */
} InputState;

struct Game;
struct FgBlock;

/* Fill `io` (it holds the previous poll's values on entry). `objs` is the frame just drawn. */
typedef void (*InputFn)(void *ud, const struct Game *g, const struct FgBlock *objs, InputState *io);

/* Headless sources (bot.c). ud = NULL. */
void input_bot(void *ud, const struct Game *g, const struct FgBlock *objs, InputState *io);
void input_spray(void *ud, const struct Game *g, const struct FgBlock *objs, InputState *io);
void input_idle(void *ud, const struct Game *g, const struct FgBlock *objs, InputState *io);

#endif
