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
#include "game.h"
#include "input.h"
#include "l96.h"
#include "platform.h"
#include "save.h"

/* ---- Combat options --------------------------------------------------------
 * --difficulty takes the registry value DifficultyLevel (COMBAT_SPEC §11): 1 (default) -> index 0
 * "normal", 2 -> index 2 "hard", anything else (0) -> index 1 "easy". */
typedef struct {
    int off, bot, idle, spray, god, demo, difficulty, max_deaths, verbose;
} CombatOpts;

static CombatOpts g_cx = { 0, 0, 0, 0, 0, 0, 1, 0, 0 };

static int difficulty_index(int reg) { return reg == 1 ? 0 : reg == 2 ? 2 : 1; }

/* Windowed input: absolute mouse mapped onto the 320x200 frame; left = fire, right = alt,
 * middle = ACT; Space = ACT request. A joystick, if present, overrides the mouse position. */
static void input_mouse(void *ud, const Game *g, const FgBlock *objs, InputState *io)
{
    (void)ud; (void)g; (void)objs;
    int x, y; uint32_t b;
    if (plat_mouse(&x, &y, &b)) { io->x = x; io->y = y; io->buttons = b; }
    if (plat_ctrl_held()) io->buttons |= IN_CTRL;
    if (plat_joystick(&x, &y, &b)) {
        /* Flight-style default (§3.1): push forward -> crosshair goes down. */
        io->x = (x + 32768) * CHAIN_W / 65536;
        io->y = CHAIN_H - 1 - (y + 32768) * CHAIN_H / 65536;
        io->buttons |= b;
    }
    if (plat_act_requested()) io->act = 1;
}

/* DirectSound volume (hundredths of a dB) and pan -> linear 0..256 gain and -256..256 balance. */
static int db100_to_lin(int db) { double v = 256.0; for (int i = 0; i < -db / 100 && i < 100; i++) v *= 0.891251; return (int)v; }

static void sfx_out(void *ud, const uint8_t *pcm, size_t n, int vol, int pan)
{
    (void)ud;
    int p = pan > 0 ? 256 - db100_to_lin(-pan) : pan < 0 ? db100_to_lin(pan) - 256 : 0;
    plat_sfx_play(pcm, n, db100_to_lin(vol), p);
}

static Game *combat_new(int windowed)
{
    if (g_cx.off) return NULL;
    GameOpts go = { 0 };
    go.difficulty = difficulty_index(g_cx.difficulty);
    go.god = g_cx.god;
    go.verbose = g_cx.verbose;
    if (g_cx.idle) go.input = input_idle;
    else if (g_cx.spray) go.input = input_spray;
    else if (g_cx.bot || !windowed) go.input = input_bot;    /* headless default: the bot */
    else go.input = input_mouse;
    if (windowed) go.sfx = sfx_out;
    Game *g = game_new(&go);
    if (!g) fprintf(stderr, "out of memory\n");
    return g;
}

/* ---- Player ---------------------------------------------------------------
 * The scene chain (chain.c) does the streaming, branching and compositing; this
 * loop only paces it against the audio clock. Structured as init / update /
 * finish so it can later become an Emscripten main-loop callback. */

static const char *g_look_script;   /* --look-script: scripted free-look camera */
static const char *g_stop_level;    /* --stop-level: end the run when the chain enters this file */
static long g_demo_loops;           /* --demo-loops: headless stop after N attract passes (0 = never) */

/* --keys "F5@300,F2@900": headless function-key presses at the given ticks. */
#define MAX_KEYS 16
static struct { long tick; int key; } g_keys[MAX_KEYS];
static int g_nkeys;

static int parse_keys(const char *s)
{
    while (*s && g_nkeys < MAX_KEYS) {
        int k;
        if (!strncmp(s, "F2@", 3)) k = 2; else if (!strncmp(s, "F5@", 3)) k = 5; else return -1;
        char *e;
        g_keys[g_nkeys].tick = strtol(s + 3, &e, 10);
        if (e == s + 3) return -1;
        g_keys[g_nkeys++].key = k;
        s = *e == ',' ? e + 1 : e;
        if (*e && *e != ',') return -1;
    }
    return 0;
}

/* F2 / F5 for this tick (windowed: the keyboard; headless: --keys). 1 = the chain jumped. */
static int fkeys(Chain *c, long tick, uint32_t plat)
{
    int j = 0;
    for (int i = 0; i < g_nkeys; i++)
        if (g_keys[i].tick == tick) plat |= g_keys[i].key == 2 ? PLAT_KEY_F2 : PLAT_KEY_F5;
    if (plat & PLAT_KEY_F5) j |= chain_restart_level(c);
    if (plat & PLAT_KEY_F2) j |= chain_to_hub(c);
    return j;
}

static int stop_level_reached(const Chain *c)
{
    return g_stop_level && !strcmp(chain_level(c), g_stop_level);
}

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
    Game  *game;
    AudioSink sink;
    long frame_no, max_frames;
    int pace, rc, drain_ticks, ended;
} Player;

static int play_init(Player *p, const ChainOpts *co, int scale, long max_frames, int pace)
{
    memset(p, 0, sizeof *p);
    ChainOpts o = *co;
    p->game = o.game = combat_new(1);
    p->c = chain_open(&o);
    if (p->c && g_look_script && chain_set_look_script(p->c, g_look_script) != 0) {
        fprintf(stderr, "bad --look-script %s\n", g_look_script); chain_close(p->c); p->c = NULL;
    }
    if (!p->c) { game_free(p->game); p->game = NULL; return 1; }
    if (plat_init(CHAIN_W, CHAIN_H, scale, L96_AUDIO_RATE, "Deadly Tide") != 0) {
        chain_close(p->c); p->c = NULL; game_free(p->game); p->game = NULL; return 1;
    }
    p->sink.pace = pace;
    p->pace = pace;
    p->max_frames = max_frames;
    return 0;
}

/* One iteration of the player. Returns nonzero when playback is over. */
static unsigned g_presented;   /* video frames handed to plat_present (dt_frames() on the web) */

static int play_tick(Player *p)
{
    Chain *c = p->c;
    if (plat_quit_requested()) return 1;
    if (g_cx.demo && plat_any_input()) return 1;   /* any key / click ends the attract loop */
    if (plat_skip_requested()) {
        int r = chain_skip(c);
        if (r == 2) return 1;
        if (r == 1 && p->pace) {
            /* Rebase so the audio clock stays continuous after dropping the queue. */
            p->sink.audio_put -= plat_audio_queued();
            plat_audio_clear();
        }
    }
    if (fkeys(c, p->frame_no, plat_take_keys()) && p->pace) {
        p->sink.audio_put -= plat_audio_queued();
        plat_audio_clear();
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
        if (chain_take_jump(c) && p->pace) {      /* death / kill branch dropped buffered groups */
            p->sink.audio_put -= plat_audio_queued();
            plat_audio_clear();
        }
        p->frame_no++;
        if (p->game && g_cx.max_deaths && game_stats(p->game)->deaths >= g_cx.max_deaths) { p->ended = 1; break; }
        if (img) shown = 1;
        if (!p->pace) break;
        if (chain_fill(c, AHEAD, sink_audio, &p->sink) != 0) { p->rc = 1; break; }
    }
    if (shown) { plat_present(chain_frame(c), CHAIN_W, CHAIN_H); g_presented++; }
    return p->rc != 0;
}

static int play_finish(Player *p)
{
    fprintf(stderr, "sequence: %s\n", chain_history(p->c));
    if (p->game) game_print_stats(p->game, stderr);
    chain_close(p->c);
    game_free(p->game);
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
static int run_headless(const ChainOpts *co0, const char *outdir, long max_frames, int every)
{
    ChainOpts o = *co0;
    const ChainOpts *co = &o;
    Game *game = o.game = combat_new(0);
    if (!g_cx.off && !game) return 1;
    Chain *c = chain_open(co);
    if (c && g_look_script && chain_set_look_script(c, g_look_script) != 0) {
        fprintf(stderr, "bad --look-script %s\n", g_look_script); chain_close(c); game_free(game); return 2;
    }
    if (!c) { game_free(game); return 1; }
    long tick = 0, written = 0;
    int rc = 0;
    while (!(max_frames && tick >= max_frames)) {
        fkeys(c, tick, 0);
        int img = 0, r = chain_step(c, &img, NULL, NULL);
        if (r < 0) { rc = 1; break; }
        if (r == 0) break;
        chain_take_jump(c);
        if (game && g_cx.max_deaths && game_stats(game)->deaths >= g_cx.max_deaths) {
            fprintf(stderr, "stopping after %d deaths\n", g_cx.max_deaths);
            tick++;
            break;
        }
        if (stop_level_reached(c)) { fprintf(stderr, "reached %s, stopping\n", g_stop_level); tick++; break; }
        if (g_demo_loops && chain_demo_loops(c) >= g_demo_loops && !strcmp(chain_level(c), "LOGOS")) {
            fprintf(stderr, "demo: %d attract loop(s) done\n", chain_demo_loops(c));
            tick++;
            break;
        }
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
    if (game) game_print_stats(game, stdout);
    chain_close(c);
    game_free(game);
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
        "  --auto-look        look-choice segments take their first window without a pick\n"
        "  --save FILE        progress file (default <user dir>/dt_save.ini; \"none\" = in memory)\n"
        "  --pilot NAME       pilot (save section, the registry's CurrentPlayer), default Player\n"
        "  --highest N        set the pilot's HighestLevel (hub lock) before starting\n"
        "  --save-point N     set the pilot's SavePoint before starting\n"
        "  --hub-between-levels  a finished level returns to the hub (default: next level, as the original)\n"
        "  --stop-level STEM  stop when the chain enters file STEM (e.g. LEVEL02)\n"
        "  --keys LIST        headless key presses, e.g. \"F5@300,F2@900\" (tick numbers)\n"
        "  --demo             attract mode: start at LOGOS, bot + god, loop LEVEL01 back to LOGOS\n"
        "                     (windowed: any key or click quits; headless: see --demo-loops)\n"
        "  --demo-loops N     headless demo: stop after N passes back to LOGOS (default 1)\n"
        "combat:\n"
        "  --bot              aimbot input (default when headless)\n"
        "  --idle             never fire\n"
        "  --spray            hold fire at the screen centre (heat lockout test)\n"
        "  --god              attract-mode regeneration: the player cannot die\n"
        "  --difficulty R     registry DifficultyLevel 0..2: 1 normal (default), 0 easy, 2 hard\n"
        "  --max-deaths N     stop after N deaths (headless default 3, 0 = never)\n"
        "  --no-combat        no gameplay layer (branches see a perfect player)\n"
        "  --combat-log       log deaths, kill branches and lockouts\n\n"
        "  --look-script S    free-look camera: \"yaw,pitch[,roll];...\" (pose per frame, last held)\n"
        "                     or \"pan[:RATE]\" (yaw += RATE units/frame; 2496 units = 360 deg)\n");
}

int main(int argc, char **argv)
{
    const char *data = ".", *file = NULL, *level = NULL, *dump = NULL;
    int seg = 0, scale = 3, every = 1, headless = 0, pace = 1, one_level = 0, chain = 0, verbose = 0, auto_look = 0;
    int hub_between = 0, set_highest = -1, set_sp = -1, diff_given = 0;
    const char *save_path = NULL, *pilot = "Player";
    long count = 0, demo_loops = -1;
    unsigned long seed = 1;
    int max_deaths = -1;
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
        else if (!strcmp(a, "--auto-look")) auto_look = 1;
        else if (!strcmp(a, "--no-auto-look")) auto_look = 0;      /* old spelling: now the default */
        else if (!strcmp(a, "--save") && more) save_path = argv[++i];
        else if (!strcmp(a, "--pilot") && more) pilot = argv[++i];
        else if (!strcmp(a, "--highest") && more) set_highest = atoi(argv[++i]);
        else if (!strcmp(a, "--save-point") && more) set_sp = atoi(argv[++i]);
        else if (!strcmp(a, "--hub-between-levels")) hub_between = 1;
        else if (!strcmp(a, "--stop-level") && more) g_stop_level = argv[++i];
        else if (!strcmp(a, "--keys") && more) { if (parse_keys(argv[++i]) != 0) { usage(); return 2; } }
        else if (!strcmp(a, "--demo")) { g_cx.demo = 1; g_cx.god = 1; g_cx.bot = 1; }
        else if (!strcmp(a, "--demo-loops") && more) demo_loops = atol(argv[++i]);
        else if (!strcmp(a, "--bot")) g_cx.bot = 1;
        else if (!strcmp(a, "--idle")) g_cx.idle = 1;
        else if (!strcmp(a, "--spray")) g_cx.spray = 1;
        else if (!strcmp(a, "--god")) g_cx.god = 1;
        else if (!strcmp(a, "--no-combat")) g_cx.off = 1;
        else if (!strcmp(a, "--combat-log")) g_cx.verbose++;
        else if (!strcmp(a, "--difficulty") && more) { g_cx.difficulty = atoi(argv[++i]); diff_given = 1; }
        else if (!strcmp(a, "--max-deaths") && more) max_deaths = atoi(argv[++i]);
        else if (!strcmp(a, "--look-script") && more) g_look_script = argv[++i];
        else { usage(); return 2; }
    }
    if (g_cx.demo && !file && !level) level = "LOGOS";   /* the attract loop starts at the logo films */
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
    co.auto_look = auto_look;
    co.hub_between_levels = hub_between;
    co.demo = g_cx.demo;
    if (g_cx.difficulty < 0 || g_cx.difficulty > 2) { usage(); return 2; }

    /* Pilot progress (the registry values, save.h). */
    static SaveData sv;
    char path[1200];
    if (save_path && !strcmp(save_path, "none")) path[0] = 0;
    else if (save_path) snprintf(path, sizeof path, "%s", save_path);
    else snprintf(path, sizeof path, "%sdt_save.ini", plat_user_dir());
    save_load(&sv, path, pilot);
    if (diff_given) sv.difficulty = g_cx.difficulty; else g_cx.difficulty = sv.difficulty;
    if (set_highest >= 0) sv.highest_level = set_highest > 20 ? 20 : set_highest;
    if (set_sp >= 0) sv.save_point = set_sp;
    if (!co.single && save_store(&sv) != 0) fprintf(stderr, "warning: cannot write %s\n", sv.path);
    if (verbose || set_highest >= 0)
        fprintf(stderr, "pilot %s: HighestLevel %d SavePoint %d ACTNum %d DifficultyLevel %d (%s)\n", sv.pilot,
                sv.highest_level, sv.save_point, sv.act_num, sv.difficulty, sv.path[0] ? sv.path : "not saved");
    co.save = co.single ? NULL : &sv;
    if (g_cx.demo) co.save = NULL;   /* attract mode is not the pilot's progress */
    g_cx.max_deaths = max_deaths >= 0 ? max_deaths : (dump || headless) ? 3 : 0;
    g_demo_loops = g_cx.demo ? (demo_loops >= 0 ? demo_loops : (headless ? 1 : 0)) : 0;
    if (dump || headless) return run_headless(&co, dump, count, every);
    return play(&co, scale, count, pace);
}
#endif /* !__EMSCRIPTEN__ */

#ifdef __EMSCRIPTEN__
#include "web_entry.inc"
#endif
