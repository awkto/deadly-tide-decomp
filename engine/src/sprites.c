/* sprites.c - see sprites.h. Ported from tools/sprites.py (fg_blocks, rle_decode, overlay). */
#include "sprites.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t  rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static int read_at(FILE *fp, uint32_t off, void *dst, size_t n)
{
    if (fseek(fp, (long)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, n, fp) == n ? 0 : -1;
}

int sprites_load(SpriteCtx *c, const L96File *f)
{
    memset(c, 0, sizeof *c);
    memcpy(c->palette, f->palette, sizeof c->palette);
    for (int i = 0; i < 256; i++)
        c->pal32[i] = (uint32_t)c->palette[i][0] << 16 | (uint32_t)c->palette[i][1] << 8 | c->palette[i][2];
    uint8_t h[0x94];
    if (read_at(f->fp, 0, h, sizeof h) != 0) return -1;
    uint32_t off_lut555 = rd32(h + 0x44), off_shade = rd32(h + 0x80), off_shade2 = rd32(h + 0x84);
    /* Lut_Load 0x40d960 clamps both counts to 4 and, on an 8-bit / RGB555 display, reads the
     * first (RGB555) half of each section into one contiguous buffer. */
    c->n_shade1 = h[0x7c] > 4 ? 4 : h[0x7c];
    c->n_shade2 = h[0x7d] > 4 ? 4 : h[0x7d];
    size_t s1 = (size_t)c->n_shade1 * 0x20000, s2 = (size_t)c->n_shade2 * 0x2000;
    c->lut555 = malloc(0x8000);
    c->shade_size = s1 + s2;
    c->shade = malloc(c->shade_size ? c->shade_size : 1);
    if (!c->lut555 || !c->shade) { sprites_free(c); return -1; }
    if (read_at(f->fp, off_lut555, c->lut555, 0x8000) != 0 ||
        (s1 && read_at(f->fp, off_shade, c->shade, s1) != 0) ||
        (s2 && read_at(f->fp, off_shade2, c->shade + s1, s2) != 0)) {
        fprintf(stderr, "sprites: cannot read colour tables\n");
        sprites_free(c); return -1;
    }
    return 0;
}

void sprites_free(SpriteCtx *c)
{
    free(c->lut555); free(c->shade);
    memset(c, 0, sizeof *c);
}

int fg_parse(const uint8_t *fg, size_t fg_size, FgBlock *blocks, int max_blocks)
{
    size_t o = 0;
    int nb = 0;
    while (nb < max_blocks && o + 12 <= fg_size && !memcmp(fg + o, "FG \0", 4)) {
        int32_t n = (int32_t)rd32(fg + o + 4), body = (int32_t)rd32(fg + o + 8);
        if (n < 0 || body < 0 || o + 12 + (size_t)body > fg_size) break;
        FgBlock *b = &blocks[nb++];
        b->count = 0;
        for (int32_t i = 0; i < n && (size_t)(0xc + FG_REC_SIZE * (i + 1)) <= 12 + (size_t)body; i++) {
            const uint8_t *r = fg + o + 0xc + (size_t)FG_REC_SIZE * (size_t)i;
            if (b->count >= FG_MAX_RECS) break;
            FgRecord *d = &b->rec[b->count++];
            d->actor = rd16(r);
            d->kind  = r[2];
            int32_t doff = (int32_t)rd32(r + 4), dsz = (int32_t)rd32(r + 8);
            d->flags = rd32(r + 0x0c);
            d->x = rd16(r + 0x10); d->y = rd16(r + 0x12); d->w = rd16(r + 0x14); d->h = rd16(r + 0x16);
            for (int k = 0; k < 4; k++) d->bbox[k] = rd16(r + 0x18 + 2 * k);
            for (int k = 0; k < 3; k++) d->pos[k] = (int32_t)rd32(r + 0x20 + 4 * k);
            d->tint = r[0x48];
            /* Data offset is relative to the block start. */
            if (doff > 0 && dsz > 0 && o + (size_t)doff + (size_t)dsz <= fg_size) {
                d->pixels = fg + o + doff; d->pixels_size = (size_t)dsz;
            } else {
                d->pixels = NULL; d->pixels_size = 0;
            }
        }
        o += ((size_t)0xc + (size_t)body + 3) & ~(size_t)3;
    }
    return nb;
}

static uint32_t rgb555_to_xrgb(uint16_t v)
{
    uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
    return r << 16 | g << 8 | b;
}

static uint8_t xrgb_to_index(const SpriteCtx *c, uint32_t v)
{
    uint32_t r = (v >> 19) & 31, g = (v >> 11) & 31, b = (v >> 3) & 31;
    return c->lut555[r << 10 | g << 5 | b];
}

void sprite_draw(const SpriteCtx *c, const FgRecord *r, uint32_t *dst, int w, int h)
{
    if (!r->pixels || r->w <= 0 || r->h <= 0) return;

    /* Spr_SetTint: >= 0x80 selects shade1 #(tint-0x80); 1..0x7f selects shade2 #(tint-1), which
     * lives after the shade1 tables. Both are indexed (src << 8 | dst) as u16 RGB555. */
    const uint8_t *tab = NULL;
    size_t tab_avail = 0;
    if (r->tint) {
        size_t base = r->tint >= 0x80 ? (size_t)(r->tint - 0x80) * 0x20000
                                      : (size_t)c->n_shade1 * 0x20000 + (size_t)(r->tint - 1) * 0x2000;
        if (base < c->shade_size) { tab = c->shade + base; tab_avail = c->shade_size - base; }
    }
    const uint32_t *pal = c->pal32;

    const uint8_t *s = r->pixels, *end = r->pixels + r->pixels_size;
    int x = 0, row = 0;
    while (s < end) {
        int8_t op = (int8_t)*s;
        if (op < 0) {
            int n = -op;
            if (s + 1 + n > end) n = (int)(end - s - 1);
            /* Row 0 is the bottom row of the sprite and y counts up from the bottom of the screen. */
            int sy = h - 1 - (r->y + row);
            if (row < r->h && sy >= 0 && sy < h) {
                uint32_t *line = dst + (size_t)sy * (size_t)w;
                for (int i = 0; i < n; i++) {
                    int sx = r->x + x + i;
                    if (sx < 0 || sx >= w) continue;
                    uint8_t src = s[1 + i];
                    if (!tab) { line[sx] = pal[src]; continue; }
                    size_t idx = ((size_t)src << 8 | xrgb_to_index(c, line[sx])) * 2;
                    if (idx + 1 < tab_avail) line[sx] = rgb555_to_xrgb((uint16_t)(tab[idx] | tab[idx + 1] << 8));
                    else line[sx] = pal[src];
                }
            }
            x += n; s += 1 + n;
        } else if (op > 0) {
            x += op; s++;
        } else {
            if (s + 3 > end) break;
            row += s[1] << 8 | s[2];
            x = 0; s += 3;
            if (row >= r->h) break;
        }
    }
}

void fg_composite(const SpriteCtx *c, const FgBlock *b, uint32_t *dst, int w, int h, int hud)
{
    for (int i = 0; i < b->count; i++)
        if (b->rec[i].kind & (OBJ_RAIL | OBJ_ENEMY)) sprite_draw(c, &b->rec[i], dst, w, h);
    for (int i = 0; i < b->count; i++)
        if (b->rec[i].kind & OBJ_EFFECT) sprite_draw(c, &b->rec[i], dst, w, h);
    if (hud)
        for (int i = 0; i < b->count; i++)
            if (b->rec[i].kind == OBJ_OVERLAY) sprite_draw(c, &b->rec[i], dst, w, h);
}
