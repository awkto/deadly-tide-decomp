/*
 * upscale.h - smooth 2x upscaler for the 320x200 scene layer (platform free).
 *
 * Separable Catmull-Rom (a sharper cousin of bicubic: it keeps edges and the video's detail
 * crisp) in integer arithmetic. A 2x upscale only ever samples two phases (1/4 and 3/4), so the
 * weights are four constants per phase, precomputed: (-6,58,222,-18) and (-18,222,58,-6) / 256.
 */
#ifndef DT_UPSCALE_H
#define DT_UPSCALE_H

#include <stdint.h>

/* src: w x h XRGB8888. dst: 2w x 2h. tmp: scratch of 2w x h pixels. The alpha byte is dropped. */
void upscale2x_catmull(const uint32_t *src, int w, int h, uint32_t *dst, uint32_t *tmp);

#endif
