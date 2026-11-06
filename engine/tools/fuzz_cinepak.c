/*
 * fuzz_cinepak - libFuzzer harness for cinepak_decode (dev tool, built with -DDT_FUZZ=ON).
 * Input: bytes 0/1 pick the frame size (multiples of 4, up to 320x200); the rest is cut into
 * frames at 0xFF 0xFF boundaries so inter-frame state (codebooks) is exercised.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cinepak.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3) return 0;
    int w = 4 * (data[0] % 80 + 1), h = 4 * (data[1] % 50 + 1);
    CinepakDecoder d;
    if (cinepak_init(&d, w, h) != 0) return 0;
    const uint8_t *p = data + 2, *end = data + size;
    while (p < end) {
        const uint8_t *q = p;
        while (q + 1 < end && !(q[0] == 0xFF && q[1] == 0xFF)) q++;
        if (q + 1 >= end) q = end;
        cinepak_decode(&d, p, (size_t)(q - p));
        p = q + 2 > end ? end : q + 2;
    }
    cinepak_free(&d);
    return 0;
}
