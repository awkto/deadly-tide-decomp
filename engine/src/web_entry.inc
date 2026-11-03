/* web_entry.inc - Emscripten entry points, included at the end of main.c (needs Player,
 * play_init/tick/finish, g_cx and the CombatOpts/SaveData types). JS writes the user's .L96
 * files into MEMFS at /NAME.L96, then calls dt_start() from a click handler (browsers only
 * allow audio after a user gesture). */
#include <emscripten.h>

static Player g_player;
static int g_running;
static SaveData g_web_save;   /* in-memory pilot (save path ""), as main.c's --save none */

static void web_tick(void)
{
    if (!g_running) return;
    if (play_tick(&g_player)) {
        int rc = play_finish(&g_player);
        g_running = 0;
        emscripten_cancel_main_loop();
        char js[96];
        snprintf(js, sizeof js, "if (Module.onPlayerDone) Module.onPlayerDone(%d)", rc);
        emscripten_run_script(js);   /* EM_ASM needs gnu mode; we build -std=c11 */
    }
}

EMSCRIPTEN_KEEPALIVE
int dt_segment_count(const char *path)
{
    L96File f;
    if (l96_open(&f, path) != 0) return -1;
    int n = (int)f.n_segments;
    l96_close(&f);
    return n;
}

/* Start a windowed run, set up exactly like main()'s native path: combat on, SDL mouse input,
 * the default pilot "Player", an in-memory save, and the registry difficulty (0 easy, 1 normal,
 * 2 hard -- see --difficulty). `level` is a file stem/name (e.g. LEVEL01 or LOGOS); it is used
 * only when start_mode is 0.
 *
 * start_mode: 0 = play `level` from segment `seg`; 1 = the full game from LOGOS; 2 = demo
 * (attract mode, if main.c has --demo: LOGOS, bot + god, any key/click ends it). */
EMSCRIPTEN_KEEPALIVE
int dt_start(const char *level, int seg, int difficulty, int start_mode)
{
    if (g_running) return -1;
    if (difficulty < 0 || difficulty > 2) difficulty = 1;
    if (start_mode < 0 || start_mode > 2) start_mode = 0;

    /* CombatOpts: off, bot, idle, spray, god, demo, difficulty, max_deaths, verbose. */
    g_cx = (CombatOpts){ 0, 0, 0, 0, 0, 0, difficulty, 0, 0 };

    static char lvl[64];
    const char *stem = level;
    int from = seg;
    if (start_mode == 1 || start_mode == 2) { stem = "LOGOS"; from = 0; }
    if (start_mode == 2) { g_cx.demo = 1; g_cx.god = 1; g_cx.bot = 1; }
    snprintf(lvl, sizeof lvl, "%s", stem);

    /* main.c's headless-only helpers/counters are compiled into this translation unit but have
     * no web use; reference them so the web build stays warning-free. */
    (void)parse_keys;
    (void)stop_level_reached;
    (void)g_demo_loops;

    ChainOpts co;
    memset(&co, 0, sizeof co);
    co.data_dir = "";
    co.level = lvl;
    co.from_seg = from;
    co.one_level = 0;
    co.seed = (uint32_t)emscripten_get_now();
    co.demo = g_cx.demo;

    /* Pilot progress in memory: no persistence, but the chain still reads it for the hub lock
     * and save points, exactly as main() does with --save none. */
    save_load(&g_web_save, "", "Player");
    g_web_save.difficulty = difficulty;
    co.save = g_cx.demo ? NULL : &g_web_save;   /* attract mode is not the pilot's progress */

    if (play_init(&g_player, &co, 2, 0, 1) != 0) return -2;
    g_running = 1;
    emscripten_set_main_loop(web_tick, 0, 0);   /* 0 fps = requestAnimationFrame */
    return 0;
}

int main(void) { return 0; }   /* runtime stays alive; JS drives dt_start() */
