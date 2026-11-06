/* Tiny PE resource reader: RT_BITMAP only. Input is untrusted; every access is bounds-checked. */
#ifndef DT_PERES_H
#define DT_PERES_H
#include <stddef.h>
#include <stdint.h>

typedef struct { int w, h; uint32_t *px; } PeImage;   /* w*h pixels, 0xAARRGGBB, top-down rows; alpha 255 */

/* Load RT_BITMAP resource `id` (any language) from a PE file in memory. 0 = ok, -1 = fail. */
int  pe_load_bitmap(const uint8_t *exe, size_t n, int id, PeImage *out);
void pe_image_free(PeImage *img);
/* List the RT_BITMAP ids present (returns count, fills up to max). */
int  pe_list_bitmaps(const uint8_t *exe, size_t n, int *ids, int max);

#endif
