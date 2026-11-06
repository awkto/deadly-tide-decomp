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

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "chain.h"
#include "game.h"
#include "input.h"
#include "l96.h"
#include "menu.h"
#include "platform.h"
#include "save.h"

/* ---- Combat options --------------------------------------------------------
 * --difficulty takes the registry value DifficultyLevel (COMBAT_SPEC §11): 1 (default) -> index 0
 * "normal", 2 -> index 2 "hard", anything else (0) -> index 1 "easy". */
typedef struct {
    int off, bot, idle, spray, god, demo, difficulty, max_deaths, verbose;
} CombatOpts;

static CombatOpts g_cx = { 0, 0, 0, 0, 0, 0, 1, 0, 0 };

/* Launcher settings (native menu): UseJoystick (-1 = no menu: a joystick, if present, is used) and
 * FlipControl. */
static int g_use_joy = -1, g_flip;

static int difficulty_index(int reg) { return reg == 1 ? 0 : reg == 2 ? 2 : 1; }

/* Windowed input: absolute mouse mapped onto the 320x200 frame; left = fire, right = alt,
 * middle = ACT; Space = ACT request. A joystick, if present, overrides the mouse position. */
static void input_mouse(void *ud, const Game *g, const FgBlock *objs, InputState *io)
{
    (void)ud; (void)g; (void)objs;
    int x, y; uint32_t b;
    if (plat_mouse(&x, &y, &b)) { io->x = x; io->y = g_flip ? CHAIN_H - 1 - y : y; io->buttons = b; }
    if (plat_ctrl_held()) io->buttons |= IN_CTRL;
    if (g_use_joy != 0 && plat_joystick(&x, &y, &b)) {
        /* Flight-style default (§3.1): push forward -> crosshair goes down; FlipControl inverts Y. */
        io->x = (x + 32768) * CHAIN_W / 65536;
        io->y = (y + 32768) * CHAIN_H / 65536;
        if (!g_flip) io->y = CHAIN_H - 1 - io->y;
        io->buttons |= b;
    }
    if (plat_gamepad(&x, &y, &b)) {
        /* Gamepad: like the original joystick the stick maps ABSOLUTELY onto the frame (§3.1: no
         * rate, centre = middle of the screen), with a deadzone. Unlike the original's flight-style
         * default, up is up; FlipControl restores the flight-style inversion. */
        io->x = (x + 32768) * CHAIN_W / 65536;
        io->y = (y + 32768) * CHAIN_H / 65536;
        if (g_flip) io->y = CHAIN_H - 1 - io->y;
        io->buttons = (io->buttons & IN_CTRL) | b;
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
    if (!plat_is_up() && plat_init(CHAIN_W, CHAIN_H, scale, L96_AUDIO_RATE, "Deadly Tide") != 0) {
        chain_close(p->c); p->c = NULL; game_free(p->game); p->game = NULL; return 1;
    }
    plat_show_cursor(0);   /* the game draws its own crosshair; the original hides the OS cursor */
    plat_text_input(0);
    plat_skip_requested(); plat_act_requested(); plat_any_input(); plat_take_keys();
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

static int g_keep_window;   /* native menu: the window outlives a game (menu -> game -> menu) */

static int play_finish(Player *p)
{
    fprintf(stderr, "sequence: %s\n", chain_history(p->c));
    if (p->game) game_print_stats(p->game, stderr);
    chain_close(p->c);
    game_free(p->game);
    if (!g_keep_window) plat_shutdown();
    return p->rc;
}

/* Pacing sleep: the browser build must yield to the event loop (Asyncify); native just sleeps. */
static void wait_ms(uint32_t ms)
{
#ifdef __EMSCRIPTEN__
    emscripten_sleep(ms);
#else
    plat_delay_ms(ms);
#endif
}

static int is_demo_pilot(const char *s)
{
    const char *d = "demo";
    for (; *d; d++, s++) if ((*s | 0x20) != *d) return 0;
    return *s == 0;
}

/* Run the menu until START or quit. 0 = START (res filled), 1 = quit. */
static int menu_run(Menu *m, MenuResult *res)
{
    static uint32_t fb[MENU_W * MENU_H];
    int jx, jy;
    uint32_t jb;
    plat_show_cursor(1);
    plat_text_input(1);
    menu_set_joystick_available(m, plat_joystick(&jx, &jy, &jb));
    uint64_t t0 = plat_time_ms();
    long done = 0;
    for (;;) {
        if (plat_quit_requested() && plat_close_requested()) return 1;   /* Q is typed text here */
        long due = (long)((plat_time_ms() - t0) * MENU_TICK_HZ / 1000);
        if (due - done > 6) done = due - 6;            /* after a stall: do not fast-forward */
        int redraw = 0;
        while (done < due) {
            char text[64];
            unsigned keys;
            int pressed, released, down, mx = 0, my = 0;
            uint32_t b;
            plat_take_menu_input(text, sizeof text, &keys, &pressed, &released, &down);
            plat_mouse(&mx, &my, &b);    /* picture coordinates: the last frame presented is 640x480 */
            MenuInput in = { mx, my, down, pressed, released, text[0] ? text : NULL, 0 };
            if (keys & PLAT_MK_ENTER) in.keys |= MENU_KEY_ENTER;
            if (keys & PLAT_MK_BACKSPACE) in.keys |= MENU_KEY_BACKSPACE;
            if (keys & PLAT_MK_ESC) in.keys |= MENU_KEY_ESC;
            if (keys & PLAT_MK_UP) in.keys |= MENU_KEY_UP;
            if (keys & PLAT_MK_DOWN) in.keys |= MENU_KEY_DOWN;
            *res = menu_update(m, &in);
            done++;
            redraw = 1;
            if (res->action == MENU_START) return 0;
            if (res->action == MENU_QUIT) return 1;
        }
        if (redraw) {
            menu_draw(m, fb);
            plat_present(fb, MENU_W, MENU_H);
        }
        wait_ms(4);
    }
}

/* Apply the chosen pilot's settings and set up the run. The menu has already stored them in the INI. */
static void menu_apply(const MenuResult *r, const char *path, SaveData *sv, ChainOpts *co)
{
    save_load(sv, path, r->pilot);
    save_set_current(path, sv->pilot);
    g_cx = (CombatOpts){ 0, 0, 0, 0, 0, 0, sv->difficulty, 0, 0 };
    g_use_joy = sv->use_joystick;
    g_flip = sv->flip;
    plat_set_volume(sv->volume);
    /* Quality I/II/III (GraphicQuality) picks a DirectDraw video mode in the original; here the window is
     * freely resizable with nearest scaling, so it has no effect. */
    int demo = is_demo_pilot(sv->pilot);
    if (demo) { g_cx.demo = 1; g_cx.god = 1; g_cx.bot = 1; }
    co->level = (demo || sv->highest_level == 0) ? "LOGOS" : "LEVEL00";   /* as Game_Main */
    co->from_seg = 0;
    co->single = 0;
    co->demo = g_cx.demo;
    co->save = demo ? NULL : sv;
}

/* menu -> game -> menu until quit (or EXIT). The window is created here and closed on return. */
static int menu_session(const ChainOpts *co0, const uint8_t *exe, size_t n, const char *path, int scale, long max_frames, int pace)
{
    if (plat_init(CHAIN_W, CHAIN_H, scale, L96_AUDIO_RATE, "Deadly Tide") != 0) return 1;
    g_keep_window = 1;
    int rc = 0;
    for (;;) {
        Menu *m = menu_create(exe, n, path);      /* fresh each time: the game changed the pilot's progress */
        if (!m) { fprintf(stderr, "cannot load the launcher art from DT.EXE\n"); rc = 1; break; }
        MenuResult res;
        int quit = menu_run(m, &res);
        menu_free(m);
        if (quit) break;
        static SaveData sv;
        ChainOpts co = *co0;
        menu_apply(&res, path, &sv, &co);
        Player p;
        if (play_init(&p, &co, scale, max_frames, pace) != 0) { rc = 1; break; }
        while (!play_tick(&p)) wait_ms(p.ended ? 10 : p.pace ? 2 : 1);
        play_finish(&p);
        plat_audio_clear();
        if (plat_close_requested()) break;
    }
    g_keep_window = 0;
    plat_shutdown();
    return rc;
}

#ifndef __EMSCRIPTEN__
static int play(const ChainOpts *co, int scale, long max_frames, int pace)
{
    Player p;
    if (play_init(&p, co, scale, max_frames, pace) != 0) return 1;
    while (!play_tick(&p)) plat_delay_ms(p.ended ? 10 : p.pace ? 2 : 1);
    return play_finish(&p);
}

/* ---- Launcher screen (native windowed build) ---------------------------------------------
 * The original DT.EXE launcher ("Pilot Control Center", docs/LAUNCHER.md) is drawn by menu.c at
 * 640x480; START applies the pilot's settings and plays the game, which returns to the menu when
 * it ends. Closing the window quits everything; so does EXIT -> YES. */

/* DT.EXE in the data dir or its cd1/ (any case); 1 = found. */
static int find_dt_exe(const char *data, char *out, size_t n)
{
    char sub[1100];
    if (plat_find_file(data, "DT.EXE", out, n)) return 1;
    snprintf(sub, sizeof sub, "%s/cd1", data);
    if (plat_find_file(sub, "DT.EXE", out, n)) return 1;
    snprintf(sub, sizeof sub, "%s/CD1", data);
    return plat_find_file(sub, "DT.EXE", out, n);
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = NULL;
    long l = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (l > 0 && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)l)) != NULL && fread(b, 1, (size_t)l, f) != (size_t)l) {
        free(b); b = NULL;
    }
    fclose(f);
    *n = b ? (size_t)l : 0;
    return b;
}

/* Native launcher entry: read DT.EXE, then the shared menu session. */
static int run_menu(const ChainOpts *co0, const char *dt_exe, const char *path, int scale, long max_frames, int pace)
{
    size_t n;
    uint8_t *exe = slurp(dt_exe, &n);
    if (!exe) { fprintf(stderr, "cannot read %s\n", dt_exe); return 1; }
    int rc = menu_session(co0, exe, n, path, scale, max_frames, pace);
    free(exe);
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
        "  --menu             show the launcher screen (needs DT.EXE in DIR or DIR/cd1); default when no\n"
        "                     --level/--file/--demo and DT.EXE is found\n"
        "  --no-menu          never show the launcher screen\n"
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
        "  --fullscreen       start in borderless fullscreen (F11 / Alt+Enter toggle)\n"
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
    int fullscreen = 0;
    int menu_flag = 0;   /* 1 --menu, -1 --no-menu */
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
        else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
        else if (!strcmp(a, "--scale") && more) scale = atoi(argv[++i]);
        else if (!strcmp(a, "--count") && more) count = atol(argv[++i]);
        else if (!strcmp(a, "--dump-frames") && more) dump = argv[++i];
        else if (!strcmp(a, "--every") && more) every = atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && more) seed = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--menu")) menu_flag = 1;
        else if (!strcmp(a, "--no-menu")) menu_flag = -1;
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
    if (fullscreen) plat_set_fullscreen(1);   /* applied when the window opens */
    /* Launcher screen: windowed, no explicit level/file/demo. */
    char dt_exe[1100] = "";
    int want_menu = 0;
    if (menu_flag >= 0 && !file && !level && !g_cx.demo && !dump && !headless) {
        if (find_dt_exe(data, dt_exe, sizeof dt_exe)) want_menu = 1;
        else if (menu_flag > 0) { fprintf(stderr, "--menu: DT.EXE not found in %s or %s/cd1\n", data, data); return 1; }
        else fprintf(stderr, "note: DT.EXE not found in %s (no launcher screen); give --level or --file\n", data);
    } else if (menu_flag > 0) {
        fprintf(stderr, "--menu cannot be combined with --level/--file/--demo/--headless/--dump-frames\n");
        return 2;
    }
    if (want_menu) level = "LOGOS";   /* placeholder so the checks below pass; the menu picks the start */
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
    if (!co.single && !want_menu && save_store(&sv) != 0) fprintf(stderr, "warning: cannot write %s\n", sv.path);
    if (verbose || set_highest >= 0)
        fprintf(stderr, "pilot %s: HighestLevel %d SavePoint %d ACTNum %d DifficultyLevel %d (%s)\n", sv.pilot,
                sv.highest_level, sv.save_point, sv.act_num, sv.difficulty, sv.path[0] ? sv.path : "not saved");
    co.save = co.single ? NULL : &sv;
    if (g_cx.demo) co.save = NULL;   /* attract mode is not the pilot's progress */
    g_cx.max_deaths = max_deaths >= 0 ? max_deaths : (dump || headless) ? 3 : 0;
    g_demo_loops = g_cx.demo ? (demo_loops >= 0 ? demo_loops : (headless ? 1 : 0)) : 0;
    if (want_menu) {
        if (!co.save) return 1;
        g_cx.max_deaths = 0;
        return run_menu(&co, dt_exe, path, scale, count, pace);
    }
    if (dump || headless) return run_headless(&co, dump, count, every);
    return play(&co, scale, count, pace);
}
#endif /* !__EMSCRIPTEN__ */

#ifdef __EMSCRIPTEN__
#include "web_entry.inc"
#endif
