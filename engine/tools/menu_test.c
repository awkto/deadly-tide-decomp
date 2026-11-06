/*
 * menu_test - headless scripted run of the launcher screen (menu.c). Dev tool, not part of the game.
 *   menu_test DT.EXE OUTDIR
 * Feeds MenuInput events, dumps a PPM per step to OUTDIR/NN_name.ppm and checks the save file.
 * Exit status 0 = all checks passed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu.h"
#include "save.h"

static Menu *g_m;
static uint32_t g_fb[MENU_W * MENU_H];
static const char *g_dir;
static int g_n, g_fail, g_mx, g_my;
static MenuResult g_last;

static void dump(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%02d_%s.ppm", g_dir, g_n++, name);
    menu_draw(g_m, g_fb);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); exit(2); }
    fprintf(f, "P6\n%d %d\n255\n", MENU_W, MENU_H);
    for (int i = 0; i < MENU_W * MENU_H; i++) {
        uint32_t c = g_fb[i];
        unsigned char rgb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static MenuResult tick(int x, int y, int down, int pressed, int released, const char *txt, unsigned keys)
{
    MenuInput in = {x, y, down, pressed, released, txt, keys};
    g_mx = x; g_my = y;
    MenuResult r = menu_update(g_m, &in);
    if (r.action != MENU_NONE) g_last = r;
    return r;
}
static void idle(int n) { for (int i = 0; i < n; i++) tick(g_mx, g_my, 0, 0, 0, NULL, 0); }
static void move(int x, int y) { tick(x, y, 0, 0, 0, NULL, 0); }
static MenuResult click(int x, int y)
{
    tick(x, y, 0, 0, 0, NULL, 0);
    tick(x, y, 1, 1, 0, NULL, 0);
    tick(x, y, 1, 0, 0, NULL, 0);
    return tick(x, y, 0, 0, 1, NULL, 0);
}
static void type(const char *s) { tick(g_mx, g_my, 0, 0, 0, s, 0); }
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static uint8_t *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)l);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: menu_test DT.EXE OUTDIR\n"); return 2; }
    g_dir = argv[2];
    size_t n;
    uint8_t *exe = slurp(argv[1], &n);
    if (!exe) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    char sp[512];
    snprintf(sp, sizeof sp, "%s/save.ini", g_dir);
    remove(sp);
    g_m = menu_create(exe, n, sp);
    if (!g_m) { fprintf(stderr, "menu_create failed\n"); return 2; }
    menu_set_joystick_available(g_m, 0);
    idle(2);
    dump("idle");

    click(230, 52);                         /* ADD */
    dump("add_dialog");
    type("Maverick");
    dump("add_typed");
    click(257, 268);                        /* OK */
    dump("after_add");
    click(230, 52); type("alpha"); tick(0, 0, 0, 0, 0, NULL, MENU_KEY_ENTER);   /* second pilot via Enter */
    click(230, 52); type("ALPHA"); tick(0, 0, 0, 0, 0, NULL, MENU_KEY_ENTER);   /* duplicate (case-insens.) */
    dump("name_in_use");
    click(257, 268);                        /* OK on the message -> back to ADD */
    tick(0, 0, 0, 0, 0, NULL, MENU_KEY_ESC);
    move(360, 76);
    click(360, 76);                         /* open drop-down */
    move(200, 112);
    dump("dropdown");
    click(200, 98);                         /* first row = alpha */
    click(200, 98 - 98 + 76);               /* field: open again */
    click(200, 115);                        /* second row = Maverick */
    dump("pilot_picked");

    click(469, 260);                        /* DIFFICULTY III */
    click(400, 170);                        /* GRAPHIC QUALITY II */
    click(150, 262);                        /* FLIP */
    tick(340, 385, 0, 0, 0, NULL, 0);       /* volume: grab the knob and drag up */
    tick(340, 385, 1, 1, 0, NULL, 0);
    for (int y = 385; y >= 365; y -= 4) tick(340, y, 1, 0, 0, NULL, 0);
    tick(340, 365, 0, 0, 1, NULL, 0);
    dump("settings");
    move(450, 380);
    idle(8);
    dump("hover_start");
    move(460, 75);
    idle(2);
    dump("hover_about");

    click(460, 75);                         /* ABOUT */
    idle(2);
    dump("about_0");
    idle(60 * 8);
    dump("about_credits");
    click(300, 200);                        /* close */
    click(200, 380);                        /* EXIT */
    idle(10);
    dump("exit_opening");
    idle(30);
    move(190, 272);
    dump("exit_open");
    click(190, 272);                        /* NO */
    idle(30);
    dump("exit_cancelled");
    click(460, 52 + 0);                     /* HELP is at 411,41: x 460 hits it */
    dump("help");
    click(300, 200);
    MenuResult r = click(450, 380);         /* START */
    check(r.action == MENU_START, "START returns MENU_START");
    check(!strcmp(r.pilot, "Maverick"), "START pilot is Maverick");
    check(r.difficulty == 2 && r.quality == 1 && r.flip == 1 && r.use_joystick == 0, "settings in the result");
    check(r.volume > 128 && r.volume < 256, "volume dragged up");
    printf("result: pilot=%s diff=%d qual=%d joy=%d flip=%d vol=%d\n", r.pilot, r.difficulty, r.quality, r.use_joystick, r.flip, r.volume);

    /* the save file */
    SaveData sd;
    save_load(&sd, sp, "Maverick");
    check(sd.difficulty == 2 && sd.quality == 1 && sd.flip == 1 && sd.volume == r.volume, "save file has Maverick's settings");
    char names[12][64];
    int np = save_list_pilots(sp, names, 12);
    check(np == 2 && !strcmp(names[0], "alpha") && !strcmp(names[1], "Maverick"), "pilot list is alpha, Maverick");
    char cur[64];
    save_get_current(sp, cur, 64);
    check(!strcmp(cur, "Maverick"), "current pilot key");
    save_load(&sd, sp, "alpha");
    check(sd.difficulty == 1 && sd.quality == 2 && sd.volume == 128, "alpha has the defaults");

    /* DELETE (Maverick is current) */
    click(325, 52);
    dump("delete_confirm");
    click(257, 268);                        /* YES */
    dump("after_delete");
    np = save_list_pilots(sp, names, 12);
    check(np == 1 && !strcmp(names[0], "alpha"), "DELETE removed Maverick");
    save_get_current(sp, cur, 64);
    check(!strcmp(cur, "alpha"), "current pilot falls back to alpha");

    /* Poseidon quirk */
    SaveData ps;
    save_load(&ps, sp, "poseidon");
    save_store(&ps);
    save_load(&ps, sp, "POSEIDON");
    check(ps.highest_level == 20, "Poseidon pilot saved with HighestLevel 20");
    save_delete_pilot(sp, "poseidon");

    r = click(450, 380);
    check(r.action == MENU_START && !strcmp(r.pilot, "alpha"), "START with alpha");
    menu_free(g_m);
    free(exe);
    printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
