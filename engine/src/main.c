/*
 * main.c - Deadly Tide front end.
 *
 *   deadlytide --data DIR --level LEVEL01 [--from-seg N] [--scale 3] [--count N]
 *   deadlytide --data DIR --level LEVEL01 --from-seg 5 --dump-frames OUTDIR [--count N] [--every K]
 *   deadlytide --data DIR --file LEVEL01.L96 --segment N        (one segment, legacy)
 *
 * The first form plays the scene chain from segment N (video paced to the audio
 * clock; Esc = the game's skip key, Q = quit). The second is headless (no window,
 * no audio) and writes composited frames as PPM files. See usage() for options.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain.h"
#include "l96.h"
#include "platform.h"

/* ---- Player ---------------------------------------------------------------
 * The scene chain (chain.c) does the streaming, branching and compositing; this
 * loop only paces it against the audio clock. Structured as init / update /
 * finish so it can later become an Emscripten main-loop callback. */

static const char *g_look_script;   /* --look-script: scripted free-look camera */

#define AHEAD 10   /* groups read ahead of the one on screen (~3.3 s of audio) */

typedef struct {
    uint64_t audio_put;        /* bytes of audio handed to the platform */
    int pace;
} AudioSink;

static void sink_audio(void *ud, const uint8_t *pcm, size_t n)
{
    AudioSink *a = ud;
    if (!a->pace) return;
    plat_audio_queue(pcm, n);
    a->audio_put += n;
}

typedef struct {
    Chain *c;
    AudioSink sink;
    long frame_no, max_frames;
    int pace, rc, drain_ticks, ended;
} Player;

static int play_init(Player *p, const ChainOpts *co, int scale, long max_frames, int pace)
{
    memset(p, 0, sizeof *p);
    p->c = chain_open(co);
    if (p->c && g_look_script && chain_set_look_script(p->c, g_look_script) != 0) {
        fprintf(stderr, "bad --look-script %s\n", g_look_script); chain_close(p->c); p->c = NULL;
    }
    if (!p->c) return 1;
    if (plat_init(CHAIN_W, CHAIN_H, scale, L96_AUDIO_RATE, "Deadly Tide") != 0) {
        chain_close(p->c); p->c = NULL; return 1;
    }
    p->sink.pace = pace;
    p->pace = pace;
    p->max_frames = max_frames;
    return 0;
}

/* One iteration of the player. Returns nonzero when playback is over. */
static int play_tick(Player *p)
{
    Chain *c = p->c;
    if (plat_quit_requested()) return 1;
    if (plat_skip_requested()) {
        int r = chain_skip(c);
        if (r == 2) return 1;
        if (r == 1 && p->pace) {
            /* Rebase so the audio clock stays continuous after dropping the queue. */
            p->sink.audio_put -= plat_audio_queued();
            plat_audio_clear();
        }
    }
    if (!p->ended && chain_fill(c, AHEAD, sink_audio, &p->sink) != 0) { p->rc = 1; return 1; }
    if (p->max_frames && p->frame_no >= p->max_frames) return 1;   /* --count: stop right there */

    if (p->ended) {
        /* Chain over: flush and let the tail of the audio drain (bounded). */
        if (!p->pace) return 1;
        plat_audio_flush();
        return plat_audio_queued() == 0 || ++p->drain_ticks > 300;
    }
    /* Audio clock: bytes the device has consumed, in milliseconds. */
    uint64_t played = p->sink.audio_put - plat_audio_queued();
    uint64_t clock_ms = played * 1000 / (L96_AUDIO_RATE * 2);

    /* Decode every tick that is due (late ones are decoded, only the newest shown). */
    int shown = 0;
    while ((!p->pace || (uint64_t)p->frame_no * 1000 / L96_VIDEO_FPS <= clock_ms) &&
           !(p->max_frames && p->frame_no >= p->max_frames)) {
        int img = 0, r = chain_step(c, &img, sink_audio, &p->sink);
        if (r < 0) { p->rc = 1; break; }
        if (r == 0) { p->ended = 1; break; }
        p->frame_no++;
        if (img) shown = 1;
        if (!p->pace) break;
        if (chain_fill(c, AHEAD, sink_audio, &p->sink) != 0) { p->rc = 1; break; }
    }
    if (shown) plat_present(chain_frame(c), CHAIN_W, CHAIN_H);
    return p->rc != 0;
}

static int play_finish(Player *p)
{
    fprintf(stderr, "sequence: %s\n", chain_history(p->c));
    chain_close(p->c);
    plat_shutdown();
    return p->rc;
}

#ifndef __EMSCRIPTEN__
static int play(const ChainOpts *co, int scale, long max_frames, int pace)
{
    Player p;
    if (play_init(&p, co, scale, max_frames, pace) != 0) return 1;
    while (!play_tick(&p)) plat_delay_ms(p.ended ? 10 : p.pace ? 2 : 1);
    return play_finish(&p);
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

/* Headless: run the chain without a window or audio. With an output dir, every
 * `every`-th tick that has a picture is written as frame_NNNNNN.ppm (NNNNNN = tick). */
static int run_headless(const ChainOpts *co, const char *outdir, long max_frames, int every)
{
    Chain *c = chain_open(co);
    if (c && g_look_script && chain_set_look_script(c, g_look_script) != 0) {
        fprintf(stderr, "bad --look-script %s\n", g_look_script); chain_close(c); return 2;
    }
    if (!c) return 1;
    long tick = 0, written = 0;
    int rc = 0;
    while (!(max_frames && tick >= max_frames)) {
        int img = 0, r = chain_step(c, &img, NULL, NULL);
        if (r < 0) { rc = 1; break; }
        if (r == 0) break;
        if (outdir && tick % every == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s/frame_%06ld.ppm", outdir, tick);
            if (write_ppm(path, chain_frame(c), CHAIN_W, CHAIN_H) != 0) {
                fprintf(stderr, "cannot write %s\n", path); rc = 1; break;
            }
            written++;
        }
        tick++;
    }
    fprintf(stderr, "ticks %ld, wrote %ld frames\n", tick, written);
    printf("sequence: %s\n", chain_history(c));
    chain_close(c);
    return rc;
}

/* ---- CLI ---------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: deadlytide --data DIR --level LEVEL01 [--from-seg N] [options]   play the scene chain\n"
        "       deadlytide --data DIR --file NAME.L96 --segment N [options]      play one segment\n"
        "options:\n"
        "  --scale S          window scale (default 3)\n"
        "  --count N          stop after N ticks (1/15 s each)\n"
        "  --dump-frames DIR  headless: write composited frames as PPM\n"
        "  --every N          with --dump-frames: write every Nth tick only\n"
        "  --headless         run the chain with no window, audio or output (prints the sequence)\n"
        "  --no-pace          windowed: run as fast as possible, no audio\n"
        "  --one-level        stop when the chain leaves the starting file\n"
        "  --chain            with --file: follow the scene graph after the segment\n"
        "  --seed N           branch PRNG seed (default 1)\n"
        "  --verbose          print each segment as it starts\n"
        "  --no-auto-look     let look-choice segments loop (default: take their first window)\n"
        "  --look-script S    free-look camera: \"yaw,pitch[,roll];...\" (pose per frame, last held)\n"
        "                     or \"pan[:RATE]\" (yaw += RATE units/frame; 2496 units = 360 deg)\n");
}

int main(int argc, char **argv)
{
    const char *data = ".", *file = NULL, *level = NULL, *dump = NULL;
    int seg = 0, scale = 3, every = 1, headless = 0, pace = 1, one_level = 0, chain = 0, verbose = 0, no_auto_look = 0;
    long count = 0;
    unsigned long seed = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--data") && more) data = argv[++i];
        else if (!strcmp(a, "--file") && more) file = argv[++i];
        else if (!strcmp(a, "--level") && more) level = argv[++i];
        else if ((!strcmp(a, "--segment") || !strcmp(a, "--from-seg")) && more) seg = atoi(argv[++i]);
        else if (!strcmp(a, "--scale") && more) scale = atoi(argv[++i]);
        else if (!strcmp(a, "--count") && more) count = atol(argv[++i]);
        else if (!strcmp(a, "--dump-frames") && more) dump = argv[++i];
        else if (!strcmp(a, "--every") && more) every = atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && more) seed = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--headless")) headless = 1;
        else if (!strcmp(a, "--no-pace")) pace = 0;
        else if (!strcmp(a, "--one-level")) one_level = 1;
        else if (!strcmp(a, "--chain")) chain = 1;
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else if (!strcmp(a, "--no-auto-look")) no_auto_look = 1;
        else if (!strcmp(a, "--look-script") && more) g_look_script = argv[++i];
        else { usage(); return 2; }
    }
    if ((!file && !level) || scale < 1 || every < 1) { usage(); return 2; }

    ChainOpts co;
    memset(&co, 0, sizeof co);
    co.data_dir = data;
    co.level = level ? level : file;
    co.from_seg = seg;
    co.single = !level && !chain;     /* legacy --file/--segment plays just that segment */
    co.one_level = one_level;
    co.seed = (uint32_t)seed;
    co.verbose = verbose;
    co.no_auto_look = no_auto_look;
    if (dump || headless) return run_headless(&co, dump, count, every);
    return play(&co, scale, count, pace);
}
#endif /* !__EMSCRIPTEN__ */

#ifdef __EMSCRIPTEN__
#include "web_entry.inc"
#endif
