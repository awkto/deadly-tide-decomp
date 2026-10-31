/* web_entry.inc - Emscripten entry points, included at the end of main.c (needs Player,
 * play_init/tick/finish). JS writes the user's .L96 files into MEMFS at /NAME.L96, then
 * calls dt_start() from a click handler (browsers only allow audio after a user gesture). */
#include <emscripten.h>

static Player g_player;
static int g_running;

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

/* Play the scene chain of `level` (e.g. "LEVEL01") from segment `seg`, following branches
 * into any other level files the user loaded. one_level: stop at the end of this file. */
EMSCRIPTEN_KEEPALIVE
int dt_start(const char *level, int seg, int scale, int one_level)
{
    if (g_running) return -1;
    static char lvl[64];
    snprintf(lvl, sizeof lvl, "%s", level);
    ChainOpts co;
    memset(&co, 0, sizeof co);
    co.data_dir = "";
    co.level = lvl;
    co.from_seg = seg;
    co.one_level = one_level;
    co.seed = (uint32_t)emscripten_get_now();
    if (play_init(&g_player, &co, scale, 0, 1) != 0) return -2;
    g_running = 1;
    emscripten_set_main_loop(web_tick, 0, 0);   /* 0 fps = requestAnimationFrame */
    return 0;
}

int main(void) { return 0; }   /* runtime stays alive; JS drives dt_start() */
