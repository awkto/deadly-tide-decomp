/* Dev tool: peres_dump EXE OUTDIR -- write every RT_BITMAP of a PE file as bmpNNN.ppm. */
#include "peres.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: peres_dump EXE OUTDIR\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = sz > 0 ? (size_t)sz : 0;
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf || (n && fread(buf, 1, n, f) != n)) { fclose(f); return 1; }
    fclose(f);
    int ids[1024], cnt = pe_list_bitmaps(buf, n, ids, 1024), bad = 0;
    if (cnt > 1024) cnt = 1024;
    printf("%d bitmaps\n", cnt);
    for (int i = 0; i < cnt; i++) {
        PeImage im;
        if (pe_load_bitmap(buf, n, ids[i], &im) != 0) { printf("id %d: unsupported/bad\n", ids[i]); bad++; continue; }
        char path[1024];
        snprintf(path, sizeof path, "%s/bmp%d.ppm", argv[2], ids[i]);
        FILE *o = fopen(path, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", im.w, im.h);
            for (int p = 0; p < im.w * im.h; p++) {
                uint8_t c[3] = { (uint8_t)(im.px[p] >> 16), (uint8_t)(im.px[p] >> 8), (uint8_t)im.px[p] };
                fwrite(c, 1, 3, o);
            }
            fclose(o);
        }
        printf("id %d: %dx%d\n", ids[i], im.w, im.h);
        pe_image_free(&im);
    }
    free(buf);
    return bad ? 3 : 0;
}
