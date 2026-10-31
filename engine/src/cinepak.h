/*
 * cinepak.h - Cinepak ("cvid") video decoder, written from the public format
 * description (MultimediaWiki "Cinepak"). Handles colour (YUV, 6-byte
 * codebook entries) and greyscale (4-byte entries) streams, 1..n strips,
 * V1/V4 codebooks with partial updates, intra/inter/V1-only vector chunks.
 *
 * Output is XRGB8888 (0x00RRGGBB per uint32_t), width*height pixels, kept
 * between calls: inter frames only touch the blocks they update.
 */
#ifndef DT_CINEPAK_H
#define DT_CINEPAK_H

#include <stddef.h>
#include <stdint.h>

#define CINEPAK_MAX_STRIPS 32

typedef struct {
    int width, height;          /* set by cinepak_init; frame header must agree */
    int grey;                   /* stream uses 4-byte greyscale codebook entries */
    uint32_t *pixels;           /* width*height XRGB8888, persists across frames */
    /* Codebooks persist across frames (per strip slot). Entry = 4 luma + u + v. */
    struct {
        uint8_t v1[256][6];
        uint8_t v4[256][6];
    } strip[CINEPAK_MAX_STRIPS];
} CinepakDecoder;

/* Dimensions must be multiples of 4. Returns 0 on success. */
int  cinepak_init(CinepakDecoder *d, int width, int height);
void cinepak_free(CinepakDecoder *d);

/* Decode one frame into d->pixels. Returns 0 on success, <0 on malformed
 * data (pixels may be partially updated). */
int  cinepak_decode(CinepakDecoder *d, const uint8_t *data, size_t size);

#endif
