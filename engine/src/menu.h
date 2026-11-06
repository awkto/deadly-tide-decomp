/*
 * menu.h - the original launcher screen ("Pilot Control Center", docs/LAUNCHER.md) as a platform-free
 * module: no SDL, no windowing. The caller feeds it mouse / keyboard events in 640x480 picture
 * coordinates and blits the 640x480 frame it draws. All art is read at start-up from the user's own
 * DT.EXE (RT_BITMAP resources, peres.h); nothing is embedded.
 *
 * Timing contract: call menu_update() exactly once per tick at MENU_TICK_HZ (60 Hz); animations (START
 * glow, EXIT flap, credits scroll, caret blink) count ticks. Call menu_draw() whenever a frame is shown.
 *
 * Pilots and per-pilot settings live in the INI save file (save.h) at `save_path`; the menu rewrites it
 * on every pilot / START / EXIT action. save_path may be NULL or "" (nothing persisted).
 */
#ifndef DT_MENU_H
#define DT_MENU_H

#include <stddef.h>
#include <stdint.h>

#define MENU_W 640
#define MENU_H 480
#define MENU_TICK_HZ 60
#define MENU_MAX_PILOTS 12
#define MENU_NAME_MAX 25          /* characters in a pilot name */

/* MenuInput.keys bits: keys that went down this tick (no auto-repeat needed for Enter/Esc/Up/Down). */
#define MENU_KEY_ENTER     1u
#define MENU_KEY_BACKSPACE 2u
#define MENU_KEY_ESC       4u
#define MENU_KEY_UP        8u
#define MENU_KEY_DOWN      16u

typedef struct {
    int         x, y;         /* mouse in 640x480 picture coordinates (may lie outside) */
    int         down;         /* left button currently held */
    int         pressed;      /* left button went down since the previous update (edge) */
    int         released;     /* left button went up since the previous update (edge) */
    const char *text;         /* UTF-8 text typed since the previous update, or NULL */
    unsigned    keys;         /* MENU_KEY_* pressed since the previous update */
} MenuInput;

typedef enum { MENU_NONE = 0, MENU_START, MENU_QUIT } MenuAction;

typedef struct {
    MenuAction action;
    /* valid for MENU_START (also filled for MENU_QUIT when a pilot is selected): */
    char pilot[64];
    int  difficulty;          /* registry DifficultyLevel 0..2 (docs/LAUNCHER.md §5), as in save.h */
    int  quality;             /* GraphicQuality 0..2 */
    int  use_joystick;        /* 0/1 */
    int  flip;                /* 0/1 */
    int  volume;              /* 0..255 */
} MenuResult;

typedef struct Menu Menu;

/* Load the art from DT.EXE (in memory) and the pilot list from the save file. NULL if the art cannot be
 * loaded (the caller then skips the menu). The current pilot is the save file's global key (or the first). */
Menu *menu_create(const uint8_t *dt_exe, size_t n, const char *save_path);
void  menu_free(Menu *m);

/* Say whether a joystick exists; without one the JOYSTICK radio and CALIBRATE are greyed. Default: none. */
void  menu_set_joystick_available(Menu *m, int available);

/* One tick (see MENU_TICK_HZ). */
MenuResult menu_update(Menu *m, const MenuInput *in);
/* Render the current state into dst (MENU_W*MENU_H pixels, 0xAARRGGBB, top-down). */
void  menu_draw(const Menu *m, uint32_t *dst);

/* Draw printable-ASCII text with the launcher's 5x7 font into any w*h 0xAARRGGBB buffer (6*scale px advance). */
void  menu_draw_text(uint32_t *dst, int w, int h, int x, int y, const char *s, uint32_t c, int scale);

#endif
