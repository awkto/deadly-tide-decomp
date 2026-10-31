/*
 * main.c - Deadly Tide front end.
 *
 *   deadlytide --data DIR --file LEVEL01.L96 --segment N [--scale 3] [--count N]
 *   deadlytide --data DIR --file LOGOS.L96 --segment 0 --dump-frames OUTDIR [--count N]
 *
 * The first form plays segment N (video paced to the audio clock). The second
 * is headless (no window, no audio) and writes decoded frames as PPM files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cinepak.h"
#include "l96.h"
#include "platform.h"

/* ---- Segment player ------------------------------------------------------
 * Structured as init / update / finish so the main loop can later become an
 * Emscripten main-loop callback without changes. */

#define RING 12  /* groups held in memory: being shown + ~3 s read ahead for audio */

typedef struct {
    const L96File *f;
    int seg;
    int max_frames;            /* 0 = whole segment */

    L96Group ring[RING];
    int loaded;                /* groups read (and their audio queued) so far */
    int vg;                    /* group currently being shown */
    int vf;                    /* next frame within that group */
    long frame_no;             /* frames shown so far (global clock tick) */
    uint64_t audio_put;        /* bytes of audio handed to the platform */
    CinepakDecoder dec;
    int dec_ready;
    int have_audio;
} Player;

/* Decode the next frame of the segment into p->dec.pixels. Returns 1 if a frame was
 * decoded, 0 if the segment is exhausted, -1 on error. Audio-only groups yield no pixels
 * but still consume 5 ticks, reported as 2. */
static int next_frame(Player *p, int *had_pixels)
{
    const L96File *f = p->f;
    int count = f->segments[p->seg].count;
    *had_pixels = 0;
    if (p->vg >= count) return 0;
    L96Group *g = &p->ring[p->vg % RING];
    int nticks = g->n_frames ? g->n_frames : L96_FRAMES_PER_GROUP;

    if (g->n_frames) {
        const uint8_t *d = g->frame[p->vf].data;
        int w = d[4] << 8 | d[5], h = d[6] << 8 | d[7];
        if (!p->dec_ready || w != p->dec.width || h != p->dec.height) {
            if (p->dec_ready) cinepak_free(&p->dec);
            if (cinepak_init(&p->dec, w, h) != 0) return -1;
            p->dec_ready = 1;
        }
        int rc = cinepak_decode(&p->dec, d, g->frame[p->vf].size);
        if (rc != 0) fprintf(stderr, "cinepak: frame %ld decode error %d\n", p->frame_no, rc);
        *had_pixels = 1;
    }
    p->frame_no++;
    if (++p->vf >= nticks) {
        l96_group_free(g);
        p->vf = 0;
        p->vg++;
    }
    return 1;
}

/* Read ahead and queue audio so the device never runs dry. */
static int fill_audio(Player *p, int windowed)
{
    int count = p->f->segments[p->seg].count;
    while (p->loaded < count && p->loaded - p->vg < RING - 1) {
        if (windowed && plat_audio_queued() > (RING - 2) * L96_AUDIO_BYTES) break;
        L96Group *g = &p->ring[p->loaded % RING];
        if (l96_read_group(p->f, p->seg, p->loaded, g) != 0) return -1;
        if (windowed && g->audio_size >= L96_AUDIO_BYTES) {
            plat_audio_queue(g->audio, L96_AUDIO_BYTES);
            p->audio_put += L96_AUDIO_BYTES;
            p->have_audio = 1;
        }
        p->loaded++;
    }
    return 0;
}

static int play(const L96File *f, int seg, int scale, int max_frames)
{
    Player p;
    memset(&p, 0, sizeof p);
    p.f = f; p.seg = seg; p.max_frames = max_frames;
    if (plat_init(320, 200, scale, L96_AUDIO_RATE, "Deadly Tide") != 0) return 1;

    int count = f->segments[seg].count, rc = 0, drain_ticks = 0;
    for (;;) {
        if (plat_quit_requested()) break;
        if (fill_audio(&p, 1) != 0) { rc = 1; break; }

        /* Audio clock: bytes the device has consumed, in milliseconds. */
        uint64_t played = p.audio_put - plat_audio_queued();
        uint64_t clock_ms = played * 1000 / (L96_AUDIO_RATE * 2);

        if (max_frames && p.frame_no >= max_frames) break;   /* --count: stop right there */
        if (p.vg >= count) {
            /* Segment over: flush and let the tail of the audio drain (bounded, in case a
             * device stops consuming). */
            plat_audio_flush();
            if (plat_audio_queued() == 0 || ++drain_ticks > 300) break;
            plat_delay_ms(10);
            continue;
        }
        /* Decode every frame that is due (late ones are decoded, only the newest shown). */
        int shown = 0, px = 0;
        while ((uint64_t)p.frame_no * 1000 / L96_VIDEO_FPS <= clock_ms && p.vg < count &&
               !(max_frames && p.frame_no >= max_frames)) {
            int r = next_frame(&p, &px);
            if (r <= 0) { rc = r < 0; break; }
            if (px) shown = 1;
            if (fill_audio(&p, 1) != 0) { rc = 1; break; }
        }
        if (shown) plat_present(p.dec.pixels, p.dec.width, p.dec.height);
        if (rc) break;
        plat_delay_ms(2);
    }
    for (int i = 0; i < RING; i++) l96_group_free(&p.ring[i]);
    if (p.dec_ready) cinepak_free(&p.dec);
    plat_shutdown();
    return rc;
}

/* ---- Headless frame dump ---------------------------------------------- */

static int write_ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    if (!row) { fclose(fp); return -1; }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t v = px[(size_t)y * (size_t)w + (size_t)x];
            row[x * 3] = (uint8_t)(v >> 16); row[x * 3 + 1] = (uint8_t)(v >> 8); row[x * 3 + 2] = (uint8_t)v;
        }
        fwrite(row, 3, (size_t)w, fp);
    }
    free(row);
    return fclose(fp);
}

static int dump_frames(const L96File *f, int seg, const char *outdir, int max_frames)
{
    CinepakDecoder dec;
    int dec_ready = 0, n = 0, rc = 0;
    int count = f->segments[seg].count;
    for (int k = 0; k < count && !rc && !(max_frames && n >= max_frames); k++) {
        L96Group g;
        if (l96_read_group(f, seg, k, &g) != 0) return 1;
        for (int i = 0; i < g.n_frames && !(max_frames && n >= max_frames); i++) {
            const uint8_t *d = g.frame[i].data;
            int w = d[4] << 8 | d[5], h = d[6] << 8 | d[7];
            if (!dec_ready || w != dec.width || h != dec.height) {
                if (dec_ready) cinepak_free(&dec);
                if (cinepak_init(&dec, w, h) != 0) { rc = 1; break; }
                dec_ready = 1;
            }
            int e = cinepak_decode(&dec, d, g.frame[i].size);
            if (e != 0) fprintf(stderr, "frame %d: decode error %d\n", n, e);
            char path[1024];
            snprintf(path, sizeof path, "%s/frame_%05d.ppm", outdir, n);
            if (write_ppm(path, dec.pixels, w, h) != 0) { fprintf(stderr, "cannot write %s\n", path); rc = 1; break; }
            n++;
        }
        l96_group_free(&g);
    }
    if (dec_ready) cinepak_free(&dec);
    fprintf(stderr, "wrote %d frames\n", n);
    return rc;
}

/* ---- CLI ---------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: deadlytide --data DIR --file NAME.L96 --segment N [--scale S] [--count N]\n"
        "       deadlytide --data DIR --file NAME.L96 --segment N --dump-frames OUTDIR [--count N]\n");
}

int main(int argc, char **argv)
{
    const char *data = ".", *file = NULL, *dump = NULL;
    int seg = 0, scale = 3, count = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--data") && more) data = argv[++i];
        else if (!strcmp(a, "--file") && more) file = argv[++i];
        else if (!strcmp(a, "--segment") && more) seg = atoi(argv[++i]);
        else if (!strcmp(a, "--scale") && more) scale = atoi(argv[++i]);
        else if (!strcmp(a, "--count") && more) count = atoi(argv[++i]);
        else if (!strcmp(a, "--dump-frames") && more) dump = argv[++i];
        else { usage(); return 2; }
    }
    if (!file || scale < 1) { usage(); return 2; }

    char path[1024];
    snprintf(path, sizeof path, "%s/%s", data, file);
    L96File f;
    if (l96_open(&f, path) != 0) return 1;
    if (seg < 0 || (uint32_t)seg >= f.n_segments) {
        fprintf(stderr, "segment %d out of range (file has %u)\n", seg, f.n_segments);
        l96_close(&f);
        return 1;
    }
    int rc = dump ? dump_frames(&f, seg, dump, count) : play(&f, seg, scale, count);
    l96_close(&f);
    return rc;
}
