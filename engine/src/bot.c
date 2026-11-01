/* bot.c - scripted headless input sources (input.h).
 *
 * --bot: aims at the nearest targetable actor (skipping 'friendlies', whose kill branch is a
 *        restart clip) (smallest |camera - position|^2) and fires within
 *        the gun's limits: weapon 0 backs off before the heat lockout and resumes once the gun
 *        has cooled, weapon 3 releases before the 7-shot cap, the semi-auto guns tap.
 *        Launches ACT when at least three targets are on screen. With nothing to shoot the
 *        crosshair drifts back to the centre (crosshair-position branches, e.g. LEVEL11 seg 7).
 *        In free-look look choices it steers the view onto the window the chain chose (hub: the
 *        highest unlocked mission; path choices: by --seed) and fires inside it to pick it.
 * --idle: never fires; the crosshair stays centred.
 * --spray: holds fire at the centre (exercises the heat lockout).
 */
#include "game.h"
#include "input.h"

#define BOT_MAX_T  64
#define HEAT_STOP  12    /* stop firing below this heat (lockout at < 1) */
#define HEAT_GO    150   /* resume once the gun has cooled to this */
#define ACT_MIN_T  3

void input_idle(void *ud, const Game *g, const FgBlock *objs, InputState *io)
{
    (void)ud; (void)g; (void)objs;
    io->x = 160; io->y = 100;
    io->buttons = 0;
    io->act = 0;
}

void input_spray(void *ud, const Game *g, const FgBlock *objs, InputState *io)
{
    (void)ud; (void)g; (void)objs;
    io->x = 160; io->y = 100;
    io->buttons = IN_FIRE;
    io->act = 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Free-look: put the crosshair where the camera's rate control turns the view centre onto
 * the goal window (mode 1: units/2 per pixel; modes 2/3: 27 / 16 pixels per degree). */
static void steer(const LookInfo *li, InputState *io)
{
    int dy = li->goal_yaw - li->yaw, dp = li->goal_pitch - li->pitch;
    if (dy > 1248) dy -= 2496;
    if (dy < -1248) dy += 2496;
    if (li->mode == 2 || li->mode == 3) {
        int k = li->mode == 2 ? 27 : 16;
        double ddy = dy * 360.0 / 2496, ddp = dp * 360.0 / 2496;
        io->x = clampi(160 + (int)(ddy * k), 0, 319);
        io->y = clampi(100 - (int)(ddp * k), 0, 199);
    } else {
        io->x = clampi(160 + 2 * dy, 0, 319);
        io->y = clampi(100 - 2 * dp, 0, 199);
    }
}

void input_bot(void *ud, const Game *g, const FgBlock *objs, InputState *io)
{
    static int cooling, tap;
    (void)ud;
    GameTarget t[BOT_MAX_T];
    int n = game_targets(g, objs, t, BOT_MAX_T);
    io->act = 0;
    int best = -1, hostile = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].avoid) continue;          /* friendlies: their kill branch restarts the level */
        hostile++;
        if (best < 0 || t[i].dist2 < t[best].dist2) best = i;
    }
    /* Look choice (hub mission, path window): once the window is open (or nothing is left to
     * shoot), turn the view onto the chosen window and press fire inside it. */
    const LookInfo *li = game_look(g);
    if (li->active && li->goal >= 0 && !li->turning && (li->choice_open || best < 0)) {
        steer(li, io);
        io->buttons = li->goal_inside && li->choice_open ? IN_FIRE : 0;
        return;
    }
    if (best < 0) {                       /* nothing to shoot: drift back to the screen centre */
        io->buttons = 0;
        io->x += io->x < 160 ? (160 - io->x < 16 ? 160 - io->x : 16) : -(io->x - 160 < 16 ? io->x - 160 : 16);
        io->y += io->y < 100 ? (100 - io->y < 16 ? 100 - io->y : 16) : -(io->y - 100 < 16 ? io->y - 100 : 16);
        return;
    }
    n = hostile;
    io->x = t[best].cx; io->y = t[best].cy;

    if (n >= ACT_MIN_T && game_act_ready(g)) { io->buttons = IN_ACT; return; }

    int fire = 1;
    switch (game_weapon(g)) {
    case -1: fire = 0; break;
    case 0: {
        int heat = game_heat(g);
        if (heat < HEAT_STOP) cooling = 1;
        if (cooling && heat >= HEAT_GO) cooling = 0;
        fire = !cooling;
        break;
    }
    case 3: case 5:
        fire = game_charge_count(g) < 6;   /* release before the 7th shot locks the gun out */
        break;
    default:                               /* semi-auto: press once the history is clear */
        tap = !tap && game_shot_hist(g) == 0;
        fire = tap;
        break;
    }
    io->buttons = fire ? IN_FIRE : 0;
}
