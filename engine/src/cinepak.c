/* cinepak.c - see cinepak.h. Independent implementation from the public format notes. */
#include "cinepak.h"

#include <stdlib.h>
#include <string.h>

static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YUV codebook colour to XRGB: R = Y + 2V, G = Y - U/2 - V, B = Y + 2U (U,V signed, U/2 truncates toward zero). */
static uint32_t yuv_to_xrgb(int y, int u, int v)
{
    return (uint32_t)clamp8(y + 2 * v) << 16 | (uint32_t)clamp8(y - u / 2 - v) << 8 | clamp8(y + 2 * u);
}

int cinepak_init(CinepakDecoder *d, int width, int height)
{
    memset(d, 0, sizeof *d);
    if (width <= 0 || height <= 0 || (width & 3) || (height & 3)) return -1;
    d->width = width; d->height = height;
    d->pixels = calloc((size_t)width * (size_t)height, sizeof *d->pixels);
    return d->pixels ? 0 : -1;
}

void cinepak_free(CinepakDecoder *d)
{
    free(d->pixels);
    memset(d, 0, sizeof *d);
}

/* Colour of one 2x2 pixel group from codebook entry `e` (4 luma, then u, v). */
typedef struct { uint32_t px[4]; } Quad;

static Quad entry_quad(const uint8_t *e, int grey)
{
    Quad q;
    int u = grey ? 0 : (int8_t)e[4], v = grey ? 0 : (int8_t)e[5];
    for (int i = 0; i < 4; i++) q.px[i] = yuv_to_xrgb(e[i], u, v);
    return q;
}

/* Codebook chunk. `partial` selects an update of selected entries vs a full replace. */
static int read_codebook(uint8_t book[256][6], const uint8_t *p, size_t n, int grey, int partial)
{
    size_t esz = grey ? 4 : 6;
    if (!partial) {
        size_t cnt = n / esz;
        if (cnt > 256) cnt = 256;
        for (size_t i = 0; i < cnt; i++) {
            memcpy(book[i], p + i * esz, esz);
            if (grey) book[i][4] = book[i][5] = 0;
        }
        return 0;
    }
    /* Partial: 32-bit big-endian masks (MSB first), each followed by entries for set bits. */
    size_t pos = 0;
    int idx = 0;
    while (pos + 4 <= n && idx < 256) {
        uint32_t mask = (uint32_t)p[pos] << 24 | (uint32_t)p[pos + 1] << 16 | (uint32_t)p[pos + 2] << 8 | p[pos + 3];
        pos += 4;
        for (int b = 0; b < 32 && idx < 256; b++, idx++) {
            if (!(mask & 0x80000000u)) { mask <<= 1; continue; }
            mask <<= 1;
            if (pos + esz > n) return -1;
            memcpy(book[idx], p + pos, esz);
            if (grey) book[idx][4] = book[idx][5] = 0;
            pos += esz;
        }
    }
    return 0;
}

/* Paint a 4x4 block at (x, y) of the output from a V4 quadruple or a single V1 entry. */
static void put_v1(CinepakDecoder *d, int x, int y, const uint8_t *e, int grey)
{
    Quad q = entry_quad(e, grey);
    uint32_t *o = d->pixels + (size_t)y * (size_t)d->width + (size_t)x;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[(size_t)r * (size_t)d->width + (size_t)c] = q.px[(r >> 1) * 2 + (c >> 1)];
}

static void put_v4(CinepakDecoder *d, int x, int y, uint8_t book[256][6], const uint8_t *idx, int grey)
{
    uint32_t *o = d->pixels + (size_t)y * (size_t)d->width + (size_t)x;
    for (int k = 0; k < 4; k++) {
        Quad q = entry_quad(book[idx[k]], grey);
        uint32_t *s = o + (size_t)(k >> 1) * 2 * (size_t)d->width + (size_t)(k & 1) * 2;
        s[0] = q.px[0]; s[1] = q.px[1];
        s[d->width] = q.px[2]; s[d->width + 1] = q.px[3];
    }
}

/* Vector chunk (0x30 intra, 0x31 inter, 0x32 V1-only) for the strip rectangle. */
static int read_vectors(CinepakDecoder *d, int sidx, int id, const uint8_t *p, size_t n,
                        int x0, int y0, int x1, int y1, int grey)
{
    uint8_t (*v1)[6] = d->strip[sidx].v1;
    uint8_t (*v4)[6] = d->strip[sidx].v4;
    size_t pos = 0;
    uint32_t mask = 0;     /* flag bits, consumed MSB first */
    int bits = 0;

#define NEXT_BIT(out) do { \
        if (!bits) { if (pos + 4 > n) return -1; \
            mask = (uint32_t)p[pos] << 24 | (uint32_t)p[pos + 1] << 16 | (uint32_t)p[pos + 2] << 8 | p[pos + 3]; \
            pos += 4; bits = 32; } \
        (out) = (mask >> 31) & 1u; mask <<= 1; bits--; } while (0)

    for (int y = y0; y < y1; y += 4) {
        for (int x = x0; x < x1; x += 4) {
            unsigned use_v4 = 0, coded = 1;
            if (id == 0x30) {
                NEXT_BIT(use_v4);
            } else if (id == 0x31) {
                NEXT_BIT(coded);
                if (coded) NEXT_BIT(use_v4);
            }   /* 0x32: every block is V1 */
            if (!coded) continue;
            if (use_v4) {
                if (pos + 4 > n) return -1;
                put_v4(d, x, y, v4, p + pos, grey);
                pos += 4;
            } else {
                if (pos + 1 > n) return -1;
                put_v1(d, x, y, v1[p[pos]], grey);
                pos += 1;
            }
        }
    }
#undef NEXT_BIT
    return 0;
}

int cinepak_decode(CinepakDecoder *d, const uint8_t *data, size_t size)
{
    /* Frame header: flags(1) length(3, BE) width(2) height(2) strips(2). */
    if (size < 10) return -1;
    unsigned w = be16(data + 4), h = be16(data + 6), nstrips = be16(data + 8);
    if ((int)w != d->width || (int)h != d->height || nstrips == 0 || nstrips > CINEPAK_MAX_STRIPS) return -2;

    size_t pos = 10;
    int y = 0;
    for (unsigned s = 0; s < nstrips; s++) {
        /* Strip header: id(2) size(2, includes header) y1 x1 y2 x2 (2 each). */
        if (pos + 12 > size) return -3;
        unsigned id = be16(data + pos), ssz = be16(data + pos + 2);
        if (ssz < 12 || pos + ssz > size) return -3;
        if (id != 0x1000 && id != 0x1100) return -3;
        int sh = (int)be16(data + pos + 8) - (int)be16(data + pos + 4);   /* strip height */
        if (sh <= 0 || y + sh > d->height) sh = d->height - y;

        /* Frame flag bit 0 clear (key frame): strips chain their codebooks from the previous strip.
         * Set (inter frame): each strip keeps its own codebooks from the previous frame. */
        if (s > 0 && !(data[0] & 1)) d->strip[s] = d->strip[s - 1];

        size_t cpos = pos + 12, cend = pos + ssz;
        while (cpos + 4 <= cend) {
            /* Chunk id: 16-bit BE, only the high byte is significant (0x2000, 0x2100, ...). */
            unsigned cid = be16(data + cpos) >> 8, csz = be16(data + cpos + 2);
            if (csz < 4 || cpos + csz > cend) return -4;
            const uint8_t *cp = data + cpos + 4;
            size_t cn = csz - 4;
            int rc = 0;
            switch (cid) {
            case 0x20: case 0x21: case 0x22: case 0x23:   /* colour: bit1 = V1 book, bit0 = partial update */
            case 0x24: case 0x25: case 0x26: case 0x27: { /* greyscale */
                uint8_t (*book)[6] = (cid & 2) ? d->strip[s].v1 : d->strip[s].v4;
                d->grey = (cid & 4) != 0;   /* greyscale streams are flagged by their codebook chunks */
                rc = read_codebook(book, cp, cn, d->grey, (cid & 1) != 0);
                break;
            }
            case 0x30: case 0x31: case 0x32: {
                rc = read_vectors(d, (int)s, (int)cid, cp, cn, 0, y, d->width, y + sh, d->grey);
                break;
            }
            default:
                break;   /* unknown chunk: skip (size is known) */
            }
            if (rc != 0) return -5;
            cpos += csz;
        }
        y += sh;
        pos += ssz;
    }
    return 0;
}
