/* web_entry.inc - Emscripten entry points, included at the end of main.c (needs Player,
 * play_init/tick/finish, g_cx and the CombatOpts/SaveData types). JS registers the user's .L96
 * File objects in Module.l96Files (read lazily, never copied), then calls dt_start() from a click handler (browsers only
 * allow audio after a user gesture). */
#include <emscripten.h>

static Player g_player;
static int g_running;
static SaveData g_web_save;   /* in-memory pilot (save path ""), as main.c's --save none */

/* Lazy .L96 access: JS keeps name -> File in Module.l96Files; reads are File.slice().arrayBuffer()
 * (async, via Asyncify), so nothing is copied into MEMFS. */
EM_JS(int, js_l96_open, (const char *path), {
    var p = UTF8ToString(path), base = p.split('/').pop().toUpperCase();
    var f = Module.l96Files && Module.l96Files[base];
    if (!f) return -1;
    Module.l96Handles = Module.l96Handles || [];
    for (var i = 0; i < Module.l96Handles.length; i++)
        if (!Module.l96Handles[i]) { Module.l96Handles[i] = f; return i; }
    Module.l96Handles.push(f);
    return Module.l96Handles.length - 1;
});
EM_JS(double, js_l96_size, (int h), { return Module.l96Handles[h].size; });
EM_JS(void, js_l96_close, (int h), { Module.l96Handles[h] = null; });
EM_ASYNC_JS(int, js_l96_read, (int h, double off, void *buf, double n), {
    try {
        var ab = await Module.l96Handles[h].slice(off, off + n).arrayBuffer();
        if (ab.byteLength !== n) return -1;
        HEAPU8.set(new Uint8Array(ab), buf);
        return 0;
    } catch (e) { console.error('l96 read failed', e); return -1; }
});

static int web_rd_read(void *ctx, uint64_t off, void *buf, size_t n)
{
    if (n == 0) return 0;
    return js_l96_read((int)(intptr_t)ctx, (double)off, buf, (double)n);
}
static uint64_t web_rd_size(void *ctx) { return (uint64_t)js_l96_size((int)(intptr_t)ctx); }
static void web_rd_close(void *ctx) { js_l96_close((int)(intptr_t)ctx); }

static int web_l96_open(const char *path, L96Reader *out)
{
    int h = js_l96_open(path);
    if (h < 0) return -1;
    out->ctx = (void *)(intptr_t)h;
    out->read_at = web_rd_read;
    out->size = web_rd_size;
    out->close = web_rd_close;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int dt_segment_count(const char *path)
{
    l96_set_open_fn(web_l96_open);
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

    l96_set_open_fn(web_l96_open);
    if (play_init(&g_player, &co, 2, 0, 1) != 0) return -2;
    g_running = 1;
    /* Asyncify: L96 reads suspend the wasm stack, so no set_main_loop; yield to the browser each
     * tick instead. Frame pacing is driven by the audio clock inside play_tick, as natively. */
    while (!play_tick(&g_player)) emscripten_sleep(1);
    int rc = play_finish(&g_player);
    g_running = 0;
    char js[96];
    snprintf(js, sizeof js, "if (Module.onPlayerDone) Module.onPlayerDone(%d)", rc);
    emscripten_run_script(js);   /* EM_ASM needs gnu mode; we build -std=c11 */
    return rc;
}

int main(void) { return 0; }   /* runtime stays alive; JS drives dt_start() */
