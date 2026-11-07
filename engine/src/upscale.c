/* upscale.c - see upscale.h. */
#include "upscale.h"

#include <stddef.h>

static int clampb(int v)
{
    v = (v + 128) >> 8;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Weighted sum of four XRGB pixels, per channel. */
static uint32_t f4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, int w0, int w1, int w2, int w3)
{
    int r = w0 * (int)(a >> 16 & 255) + w1 * (int)(b >> 16 & 255) + w2 * (int)(c >> 16 & 255) + w3 * (int)(d >> 16 & 255);
    int g = w0 * (int)(a >> 8 & 255) + w1 * (int)(b >> 8 & 255) + w2 * (int)(c >> 8 & 255) + w3 * (int)(d >> 8 & 255);
    int u = w0 * (int)(a & 255) + w1 * (int)(b & 255) + w2 * (int)(c & 255) + w3 * (int)(d & 255);
    return (uint32_t)clampb(r) << 16 | (uint32_t)clampb(g) << 8 | (uint32_t)clampb(u);
}

static int clampi(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

void upscale2x_catmull(const uint32_t *src, int w, int h, uint32_t *dst, uint32_t *tmp)
{
    int w2 = w * 2;
    for (int y = 0; y < h; y++) {                      /* horizontal: h rows of 2w */
        const uint32_t *s = src + (size_t)y * (size_t)w;
        uint32_t *t = tmp + (size_t)y * (size_t)w2;
        for (int x = 0; x < w; x++) {
            uint32_t a = s[clampi(x - 2, w - 1)], b = s[clampi(x - 1, w - 1)], c = s[x];
            uint32_t d = s[clampi(x + 1, w - 1)], e = s[clampi(x + 2, w - 1)];
            t[2 * x]     = f4(a, b, c, d, -6, 58, 222, -18);   /* centre x - 1/4 */
            t[2 * x + 1] = f4(b, c, d, e, -18, 222, 58, -6);   /* centre x + 1/4 */
        }
    }
    for (int y = 0; y < h; y++) {                      /* vertical: 2h rows */
        const uint32_t *r0 = tmp + (size_t)clampi(y - 2, h - 1) * (size_t)w2;
        const uint32_t *r1 = tmp + (size_t)clampi(y - 1, h - 1) * (size_t)w2;
        const uint32_t *r2 = tmp + (size_t)y * (size_t)w2;
        const uint32_t *r3 = tmp + (size_t)clampi(y + 1, h - 1) * (size_t)w2;
        const uint32_t *r4 = tmp + (size_t)clampi(y + 2, h - 1) * (size_t)w2;
        uint32_t *o0 = dst + (size_t)(2 * y) * (size_t)w2, *o1 = o0 + w2;
        for (int x = 0; x < w2; x++) {
            o0[x] = f4(r0[x], r1[x], r2[x], r3[x], -6, 58, 222, -18);
            o1[x] = f4(r1[x], r2[x], r3[x], r4[x], -18, 222, 58, -6);
        }
    }
}
