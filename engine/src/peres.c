#include "peres.h"
#include <stdlib.h>

static int rd(size_t n, size_t off, size_t len)
{
    return off <= n && len <= n - off;
}
static uint32_t u16(const uint8_t *p, size_t o) { return (uint32_t)p[o] | (uint32_t)p[o + 1] << 8; }
static uint32_t u32(const uint8_t *p, size_t o) { return u16(p, o) | u16(p, o + 2) << 16; }

typedef struct { const uint8_t *b; size_t n; size_t rs, rn; size_t sec0; uint32_t nsec; } Pe;

/* RVA -> file offset, requiring [rva, rva+len) to lie within one section's raw data. */
static int rva2off(const Pe *pe, uint32_t rva, size_t len, size_t *off)
{
    for (uint32_t i = 0; i < pe->nsec; i++) {
        size_t s = pe->sec0 + (size_t)i * 40;
        uint32_t vs = u32(pe->b, s + 8), va = u32(pe->b, s + 12), rsz = u32(pe->b, s + 16), rp = u32(pe->b, s + 20);
        uint32_t span = (vs != 0 && vs < rsz) ? vs : rsz;
        if (rva >= va && rva - va <= span && len <= span - (rva - va)) {
            size_t o = (size_t)rp + (rva - va);
            if (!rd(pe->n, o, len)) return -1;
            *off = o;
            return 0;
        }
    }
    return -1;
}

static int pe_open(const uint8_t *b, size_t n, Pe *pe)
{
    if (!b || !rd(n, 0, 0x40) || b[0] != 'M' || b[1] != 'Z') return -1;
    size_t pp = u32(b, 0x3c);
    if (!rd(n, pp, 24) || u32(b, pp) != 0x4550u) return -1;
    uint32_t nsec = u16(b, pp + 6), osz = u16(b, pp + 20);
    size_t opt = pp + 24;
    if (!rd(n, opt, osz) || osz < 2) return -1;
    uint32_t magic = u16(b, opt);
    size_t dd;
    if (magic == 0x10b) dd = 96; else if (magic == 0x20b) dd = 112; else return -1;
    if (osz < dd + 8 * 3) return -1;
    if (u32(b, opt + dd - 4) < 3) return -1;
    size_t sec0 = opt + osz;
    if (!rd(n, sec0, (size_t)nsec * 40)) return -1;
    pe->b = b; pe->n = n; pe->nsec = nsec; pe->sec0 = sec0;
    uint32_t rrva = u32(b, opt + dd + 16), rsz = u32(b, opt + dd + 20);
    size_t o;
    if (rsz == 0 || rva2off(pe, rrva, rsz, &o) != 0) return -1;
    pe->rs = o; pe->rn = rsz;
    return 0;
}

/* Directory at offset d within rsrc. Returns entry count (named+id) or -1. */
static int dir_count(const Pe *pe, uint32_t d)
{
    if (!rd(pe->rn, d, 16)) return -1;
    return (int)(u16(pe->b + pe->rs, (size_t)d + 12) + u16(pe->b + pe->rs, (size_t)d + 14));
}
/* Entry i of dir d: *name = id/name field, *sub = 1 if subdirectory, *off = target offset. */
static int dir_entry(const Pe *pe, uint32_t d, int i, uint32_t *name, int *sub, uint32_t *off)
{
    size_t e = (size_t)d + 16 + (size_t)i * 8;
    if (!rd(pe->rn, e, 8)) return -1;
    const uint8_t *r = pe->b + pe->rs;
    *name = u32(r, e);
    uint32_t v = u32(r, e + 4);
    *sub = (v >> 31) != 0;
    *off = v & 0x7fffffffu;
    return 0;
}
/* Find the RT_BITMAP type directory offset. */
static int bitmap_dir(const Pe *pe, uint32_t *out)
{
    int c = dir_count(pe, 0);
    for (int i = 0; i < c; i++) {
        uint32_t nm, off; int sub;
        if (dir_entry(pe, 0, i, &nm, &sub, &off) != 0) return -1;
        if (nm == 2 && sub) { *out = off; return 0; }
    }
    return -1;
}

int pe_list_bitmaps(const uint8_t *exe, size_t n, int *ids, int max)
{
    Pe pe; uint32_t bd;
    if (pe_open(exe, n, &pe) != 0 || bitmap_dir(&pe, &bd) != 0) return 0;
    int c = dir_count(&pe, bd), k = 0;
    for (int i = 0; i < c; i++) {
        uint32_t nm, off; int sub;
        if (dir_entry(&pe, bd, i, &nm, &sub, &off) != 0) break;
        if (nm & 0x80000000u) continue;   /* named, not numeric */
        if (ids && k < max) ids[k] = (int)nm;
        k++;
    }
    return k;
}

int pe_load_bitmap(const uint8_t *exe, size_t n, int id, PeImage *out)
{
    Pe pe; uint32_t bd, nm, off = 0;
    int sub;
    if (!out) return -1;
    out->w = out->h = 0; out->px = NULL;
    if (id < 0 || pe_open(exe, n, &pe) != 0 || bitmap_dir(&pe, &bd) != 0) return -1;
    int c = dir_count(&pe, bd), found = 0;
    for (int i = 0; i < c && !found; i++) {
        if (dir_entry(&pe, bd, i, &nm, &sub, &off) != 0) return -1;
        if (!(nm & 0x80000000u) && nm == (uint32_t)id && sub) found = 1;
    }
    if (!found) return -1;
    /* language level: take the first entry */
    if (dir_count(&pe, off) < 1 || dir_entry(&pe, off, 0, &nm, &sub, &off) != 0 || sub) return -1;
    if (!rd(pe.rn, off, 16)) return -1;
    uint32_t drva = u32(pe.b + pe.rs, off), dsz = u32(pe.b + pe.rs, (size_t)off + 4);
    size_t o;
    if (dsz < 40 || rva2off(&pe, drva, dsz, &o) != 0) return -1;
    const uint8_t *d = pe.b + o;
    size_t hsz = u32(d, 0);
    if (hsz < 40 || hsz > dsz) return -1;
    int32_t w = (int32_t)u32(d, 4), h = (int32_t)u32(d, 8);
    uint32_t bpp = u16(d, 14), comp = u32(d, 16), clr = u32(d, 32);
    int topdown = h < 0;
    if (topdown) { if (h == INT32_MIN) return -1; h = -h; }
    if (comp != 0 || w <= 0 || h <= 0 || w > 16384 || h > 16384) return -1;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24) return -1;
    uint32_t pal[256] = {0};
    size_t pos = hsz;
    if (bpp <= 8) {
        uint32_t np = clr ? clr : 1u << bpp;
        if (np > (1u << bpp)) return -1;
        if (pos + (size_t)np * 4 > dsz) return -1;
        for (uint32_t i = 0; i < np; i++)
            pal[i] = 0xff000000u | (u32(d, pos + (size_t)i * 4) & 0xffffffu);   /* BGRX -> 0xFFRRGGBB */
        pos += (size_t)np * 4;
    }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (stride * (size_t)h > dsz - pos) return -1;
    uint32_t *px = malloc((size_t)w * (size_t)h * sizeof *px);
    if (!px) return -1;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + pos + stride * (size_t)(topdown ? y : h - 1 - y);
        uint32_t *dst = px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t v;
            if (bpp == 24)
                v = 0xff000000u | (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3];
            else if (bpp == 8) v = pal[row[x]];
            else if (bpp == 4) v = pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
            else v = pal[(row[x >> 3] >> (7 - (x & 7))) & 1];
            dst[x] = v;
        }
    }
    out->w = w; out->h = h; out->px = px;
    return 0;
}

void pe_image_free(PeImage *img)
{
    if (img) { free(img->px); img->px = NULL; img->w = img->h = 0; }
}
