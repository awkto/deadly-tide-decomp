/*
 * freelook.h - type-6 "free-look" renderer (docs/FREELOOK_SPEC.md; prototype tools/freelook.py).
 *
 * The camera sits inside a textured cube: 6 faces x 4x4 tiles of 128x128 texels, drawn as
 * 192 triangles by a Hecker-style perspective-correct rasteriser (28.4 vertices, float
 * gradients, a divide every 8 pixels, 16.16 affine steps in between).
 *
 * Texture ("strip"): 96 tiles of 128x128 = 128 x 12288 texels. Engine memory keeps each tile
 * bottom-up (memory row 0 = bottom image row) and FG sprites are drawn into it with their
 * draw rect in strip coordinates. We store the strip upside down (array row = 12287 - engine
 * row) so tiles are plain top-down images and sprite_draw() (y counted from the bottom) can
 * paint into it unchanged.
 *
 * Platform free, no I/O.
 */
#ifndef DT_FREELOOK_H
#define DT_FREELOOK_H

#include <stddef.h>
#include <stdint.h>

#include "sprites.h"

#define FL_W            320
#define FL_H            200
#define FL_TILE         128
#define FL_NTILES       96                      /* 6 faces x 16 */
#define FL_STRIP_H      (FL_TILE * FL_NTILES)   /* 12288 */
#define FL_TILE_SLOT    0x1800                  /* .data 0x4210dc: carry bytes per tile */
#define FL_TILE_HDR     0x44                    /* u32 cinepak length + unused, then Cinepak */
#define FL_PRELOAD_GROUPS 20                    /* .data 0x4210ec */
#define FL_UNITS        2496                    /* angle units per turn */

/* Crosshair position in 320x200 screen space (Input_Poll 0x40ec10: g_view+0x1d0/0x1d4).
 * The camera turns at a rate set by its offset from the centre (160,100). */
typedef struct {
    int cx, cy;
} FreelookInput;

/* Scripted camera for headless runs: either a list of poses (frame i of a free-look
 * segment uses pose min(i, n-1)) or an automatic pan of `pan` units per frame. */
#define FL_SCRIPT_MAX 64
typedef struct {
    int n;
    int pose[FL_SCRIPT_MAX][3];    /* yaw, pitch, roll */
    int pan;                       /* != 0: yaw += pan per frame from the entry pose */
} FreelookScript;

typedef struct {
    uint32_t *strip;          /* 128 x 12288 XRGB8888, flipped (see above) */
    int       yaw, pitch, roll;   /* g_view +0x28/+0x2c/+0x30, engine units */
    int       half_fov;       /* segment i16 0x4a, 1/512 degree */
    int       mode;           /* segment byte 0x21 (control sub-mode) */
    int       hub;            /* LEVEL00: hub lock on kind-1 records */
    int       highest_level;  /* 0x4215ac, for the hub lock (default 1) */
    /* Mode 2 rate / bank state (0x4212a0 n, 0x421298 ph, 0x42129c k, 0x4287b8 roll0). */
    int       m2_n, m2_ph, m2_k;
    double    m2_roll0;
    /* Save-under buffer for kind-2 records (Spr_RestoreBackground). */
    uint32_t *save;
    size_t    save_cap;
} Freelook;

int  fl_init(Freelook *fl);        /* 0 on success; strip cleared to black */
void fl_free(Freelook *fl);

/* Decode carry tile slot `slot` (FL_TILE_SLOT bytes: u32 len, 0x40 pad, 128x128 Cinepak key
 * frame) into tile t (0..95). Returns 0, or -1 if the slot is not a valid tile. */
int  fl_load_tile(Freelook *fl, int t, const uint8_t *slot, size_t n);

/* View_Set 0x40e1d0: entry pose, resets the rate state. */
void fl_view_set(Freelook *fl, int yaw, int pitch, int roll);

/* One frame of Player_Update case 6 (modes 1/2/3; auto-turn and pick are not done here),
 * followed by View_Clamp6. */
void fl_control(Freelook *fl, const FreelookInput *in);

/* Apply a script pose for free-look frame `i` (counted from the segment's first frame);
 * entry pose (ey, ep, er) is the base for pan scripts. */
void fl_script_pose(Freelook *fl, const FreelookScript *s, int i, int ey, int ep, int er);
/* Parse "yaw,pitch[,roll];..." or "pan" / "pan:RATE". Returns 0, -1 on a syntax error. */
int  fl_script_parse(FreelookScript *s, const char *text);

/* FreeLook_Render 0x405710: draw the cube from the current pose into dst (320x200 XRGB8888). */
void fl_render(const Freelook *fl, uint32_t *dst);

/* FreeLook_Frame 0x412ab0: paint kind-1 records of `b` into the strip (permanent), kind-2
 * with save-under, render the cube into dst, draw kind-4 records on screen, then restore
 * the kind-2 save-unders. `b` may be NULL. With draw = 0 only the kind-1 paint happens. */
void fl_frame(Freelook *fl, const SpriteCtx *spr, const FgBlock *b, uint32_t *dst, int draw);

#endif
