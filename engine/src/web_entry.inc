/* web_entry.inc - Emscripten entry points, included at the end of main.c (needs Player,
 * play_init/tick/finish). JS writes the user's .L96 into MEMFS, then calls dt_start() from a
 * click handler (browsers only allow audio after a user gesture). */
#include <emscripten.h>

static Player g_player;
static L96File g_file;
static int g_running;

static void web_tick(void)
{
    if (!g_running) return;
    if (play_tick(&g_player)) {
        int rc = play_finish(&g_player);
        l96_close(&g_file);
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

EMSCRIPTEN_KEEPALIVE
int dt_start(const char *path, int seg, int scale)
{
    if (g_running) return -1;
    if (l96_open(&g_file, path) != 0) return -2;
    if (seg < 0 || (uint32_t)seg >= g_file.n_segments) { l96_close(&g_file); return -3; }
    if (play_init(&g_player, &g_file, seg, scale, 0) != 0) { l96_close(&g_file); return -4; }
    g_running = 1;
    emscripten_set_main_loop(web_tick, 0, 0);   /* 0 fps = requestAnimationFrame */
    return 0;
}

int main(void) { return 0; }   /* runtime stays alive; JS drives dt_start() */
