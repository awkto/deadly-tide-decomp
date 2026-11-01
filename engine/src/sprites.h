/*
 * sprites.h - FG object blocks and the signed-byte RLE sprite codec
 * (docs/GAMEPLAY_DATA.md §3-4; reference implementation tools/sprites.py).
 *
 * Sprites are composited onto an XRGB8888 frame. Plain records map palette
 * indices through the level palette. Tinted records emulate the engine's 8-bit
 * path (Spr_SetTint 0x40db00 + Px_Plot8Tinted 0x40dec0):
 *     out = shade[(src << 8) | dst_index]          (RGB555)
 * where dst_index is the frame pixel mapped through the level's RGB555 -> index
 * table (lut555). We expand the RGB555 result straight to RGB instead of
 * mapping it back through lut555 (the engine had to, its buffer was 8-bit).
 */
#ifndef DT_SPRITES_H
#define DT_SPRITES_H

#include <stddef.h>
#include <stdint.h>

#include "l96.h"

#define FG_REC_SIZE 0x58
#define FG_MAX_RECS 64

enum { OBJ_RAIL = 1, OBJ_ENEMY = 2, OBJ_EFFECT = 4, OBJ_OVERLAY = 8 };

typedef struct {
    int16_t  actor;
    uint8_t  kind;
    uint32_t flags;
    int16_t  x, y, w, h;          /* y counted from the bottom row */
    int16_t  bbox[4];             /* x0, y0, x1, y1 inclusive, y from the bottom */
    int32_t  pos[3];
    uint8_t  face_mask;           /* +0x2c, type 6: bit f = rect on cube face f (FREELOOK_SPEC §4.2) */
    int16_t  face_rect[3][4];     /* +0x30: x0,y0,x1,y1 in 512x512 face pixels, one per set bit (tint at 0x48 follows) */
    uint8_t  tint;
    const uint8_t *pixels;        /* RLE data (points into the group), NULL if none */
    size_t   pixels_size;
} FgRecord;

typedef struct FgBlock {
    int      count;               /* records kept (<= FG_MAX_RECS) */
    FgRecord rec[FG_MAX_RECS];
} FgBlock;

typedef struct {
    uint8_t   palette[256][3];
    uint32_t  pal32[256];         /* palette as XRGB8888 */
    uint8_t  *lut555;             /* 0x8000: RGB555 -> palette index */
    uint8_t  *shade;              /* n1 x 0x20000 RGB555 shade tables, then n2 x 0x2000 shade2 */
    size_t    shade_size;
    int       n_shade1, n_shade2;
} SpriteCtx;

int  sprites_load(SpriteCtx *c, const L96File *f);   /* 0 on success */
void sprites_free(SpriteCtx *c);

/* Split a group's FG part into per-frame blocks. Returns the number of blocks found
 * (<= max_blocks). Records point into `fg`, which must outlive the blocks. */
int  fg_parse(const uint8_t *fg, size_t fg_size, FgBlock *blocks, int max_blocks);

/* Draw one record onto an XRGB8888 frame (w x h, top-down), clipped. */
void sprite_draw(const SpriteCtx *c, const FgRecord *r, uint32_t *dst, int w, int h);

/* Same, drawn with stride +w as the engine's gun flash does: RLE row k (the sprite's bottom row
 * first) lands on top-down row top + k, so the sprite appears upside down. */
void sprite_draw_flip(const SpriteCtx *c, const FgRecord *r, uint32_t *dst, int w, int h, int top);

/* Draw a block in engine order: kinds 1|2, then kind 4 (Obj_DrawLayer), then kind 8
 * (Obj_DrawStatic, HUD) if `hud` is set. */
void fg_composite(const SpriteCtx *c, const FgBlock *b, uint32_t *dst, int w, int h, int hud);

#endif
