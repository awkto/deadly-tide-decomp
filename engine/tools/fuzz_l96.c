/*
 * fuzz_l96 - libFuzzer harness for the .L96 parsing path (dev tool, built with -DDT_FUZZ=ON).
 * The input is served as every .L96 file through an in-memory L96Reader, then: l96_open, every
 * group of every segment, Cinepak decode of each frame, fg_parse + sprite compositing, scene_load,
 * and a bounded run of the whole scene chain with the combat layer and the bot.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain.h"
#include "cinepak.h"
#include "game.h"
#include "l96.h"
#include "scene.h"
#include "sprites.h"

static const uint8_t *g_data;
static size_t g_size;

static int mem_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    (void)ctx;
    if (off > g_size || n > g_size - off) return -1;
    memcpy(dst, g_data + off, n);
    return 0;
}
static uint64_t mem_size(void *ctx) { (void)ctx; return g_size; }
static void mem_close(void *ctx) { (void)ctx; }
static int mem_open(const char *path, L96Reader *out)
{
    (void)path;
    out->ctx = NULL; out->read_at = mem_read; out->size = mem_size; out->close = mem_close;
    return 0;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    /* The readers log every rejection to stdio's stderr; leave fd 2 alone so sanitizer reports show. */
    FILE *nul = fopen("/dev/null", "w");
    if (!nul) return 1;
    stderr = nul;
    l96_set_open_fn(mem_open);
    return 0;
}

static void walk(const L96File *f)
{
    static FgBlock blocks[8];
    static uint32_t frame[CHAIN_W * CHAIN_H];
    CinepakDecoder dec;
    int have_dec = cinepak_init(&dec, CHAIN_W, CHAIN_H) == 0;
    SpriteCtx spr;
    int have_spr = sprites_load(&spr, f) == 0;
    int budget = 300;                      /* groups per input */
    for (uint32_t s = 0; s < f->n_segments && budget > 0; s++) {
        const L96Segment *sg = &f->segments[s];
        for (int k = 0; k < sg->count && budget > 0; k++, budget--) {
            L96Group g;
            if (l96_read_group(f, (int)s, k, &g) != 0) continue;
            for (int i = 0; i < g.n_frames && have_dec; i++) cinepak_decode(&dec, g.frame[i].data, g.frame[i].size);
            if (g.fg_size) {
                int n = fg_parse(g.fg, g.fg_size, blocks, 8);
                for (int i = 0; i < n && have_spr; i++) fg_composite(&spr, &blocks[i], frame, CHAIN_W, CHAIN_H, 1);
            }
            l96_group_free(&g);
        }
    }
    if (have_spr) sprites_free(&spr);
    if (have_dec) cinepak_free(&dec);
    Scene sc;
    if (scene_load(&sc, f) == 0) {
        GameState gs = scene_default_state();
        for (int s = 0; s < sc.n_segments && s < 64; s++) {
            scene_branch_resolve(&sc, s, &gs);
            scene_auto_pick(&sc, s);
            scene_successor(&sc, s, &gs, INT32_MIN);
            scene_hotspot_pick(&sc, s, 100, 100, s & 1, s & 15);
            scene_look_enabled(&sc, s, 0, s & 1, 3);
            scene_esc(&sc, s);
        }
        scene_free(&sc);
    }
}

static void run_chain(const uint8_t *d, size_t n)
{
    static const char *const lv[] = { "LEVEL01", "LEVEL00", "LOGOS", "LEVEL02" };
    GameOpts go = { 0 };
    go.god = d[0] & 1; go.difficulty = (d[0] >> 1) % 3;
    go.input = input_bot;
    Game *game = game_new(&go);
    ChainOpts co = { 0 };
    co.data_dir = "mem"; co.level = lv[(n > 1 ? d[1] : 0) & 3];
    co.seed = n > 2 ? d[2] : 1u;
    co.game = game;
    co.demo = n > 3 && (d[3] & 7) == 0;
    co.from_seg = n > 4 ? d[4] & 31 : 0;
    co.auto_look = n > 3 && (d[3] & 8);
    Chain *c = chain_open(&co);
    if (c) {
        for (int t = 0; t < 120; t++) {
            int img = 0;
            if (chain_step(c, &img, NULL, NULL) <= 0) break;
            chain_take_jump(c);
            if (t == 40) chain_skip(c);
            if (t == 80 && (d[0] & 4)) chain_to_hub(c);
        }
        chain_close(c);
    }
    game_free(game);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    g_data = data; g_size = size;
    L96File f;
    if (l96_open(&f, "mem.L96") != 0) return 0;
    walk(&f);
    l96_close(&f);
    if (size >= 0x100) run_chain(data + 0x7c, size - 0x7c);   /* header bytes double as chain knobs */
    return 0;
}
