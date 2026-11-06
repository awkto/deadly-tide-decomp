/* fuzz_peres - libFuzzer harness for the PE resource reader (DT.EXE is user supplied). */
#include <stdint.h>
#include <stdlib.h>

#include "peres.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int ids[64];
    int n = pe_list_bitmaps(data, size, ids, 64);
    for (int i = 0; i < n && i < 64; i++) {
        PeImage img;
        if (pe_load_bitmap(data, size, ids[i], &img) == 0) pe_image_free(&img);
    }
    return 0;
}
