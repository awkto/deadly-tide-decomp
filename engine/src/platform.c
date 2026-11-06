/* platform.c - SDL3 backend for platform.h. */
#include "platform.h"

#include <SDL3/SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <stdio.h>

static SDL_Window      *g_win;
static SDL_Renderer    *g_ren;
static SDL_Texture     *g_tex;
static int              g_tex_w, g_tex_h;
static SDL_AudioStream *g_audio;
static SDL_AudioDeviceID g_audio_dev;
static int              g_skip, g_act, g_any_input;
static uint32_t         g_keys;
static int              g_frame_w = 320, g_frame_h = 200;
static SDL_Joystick    *g_joy;
static SDL_Gamepad     *g_pad;          /* first SDL_Gamepad (hot-plugged) */
static int              g_pad_active;   /* the pad has taken over from the mouse (until it moves) */
static int              g_pad_btn_prev; /* A, for the menu click edges */
static int              g_warp_pending; /* mouse-motion events caused by our own warps */
static int              g_menu_mode;    /* plat_text_input(1): the launcher screen is up */
static int              g_fullscreen;
static float            g_cur_x, g_cur_y;   /* menu cursor in picture coordinates */
#define PAD_DEAD 6500   /* ~20% of the stick range */
static int              g_closed, g_vol = 255;
static char             g_text[64];
static unsigned         g_mkeys;
static int              g_mpress, g_mrel, g_mdown;

/* SFX mixer: a second stream on the same device, filled on demand from a few voices. */
#define N_VOICES 16
typedef struct { uint8_t *buf; size_t cap, n, pos; int gl, gr; } Voice;
static SDL_AudioStream *g_sfx;
static Voice            g_voice[N_VOICES];

static void SDLCALL sfx_fill(void *ud, SDL_AudioStream *s, int additional, int total)
{
    (void)ud; (void)total;
    int16_t buf[512 * 2];
    int frames = additional / 4;
    while (frames > 0) {
        int k = frames > 512 ? 512 : frames;
        for (int i = 0; i < k; i++) {
            int l = 0, r = 0;
            for (int v = 0; v < N_VOICES; v++) {
                Voice *vc = &g_voice[v];
                if (vc->pos >= vc->n) continue;
                int smp = ((int)vc->buf[vc->pos++] - 128) << 8;
                l += smp * vc->gl / 256; r += smp * vc->gr / 256;
            }
            buf[2 * i]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            buf[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        SDL_PutAudioStreamData(s, buf, k * 4);
        frames -= k;
    }
}

/* Open the first connected gamepad if none is open. */
static void pad_open_first(void)
{
    if (g_pad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) g_pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
}

/* TEST ONLY: DT_TEST_VPAD=1 attaches a virtual gamepad and drives a scripted stick/button
 * sequence from plat_quit_requested() (used to verify the gamepad path without hardware). */
static SDL_Joystick *g_vpad;
static void test_vpad_attach(void)
{
    if (!SDL_getenv("DT_TEST_VPAD")) return;
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "DT test pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    if (id) g_vpad = SDL_OpenJoystick(id);
}
static void test_vpad_script(void)
{
    if (!g_vpad) return;
    static uint64_t t0;
    if (!t0) t0 = SDL_GetTicks();
    unsigned s = (unsigned)((SDL_GetTicks() - t0) / 1000);   /* seconds since first poll */
    int lx = 0, ly = 0, a = 0, rt = -32768, b = 0;   /* triggers rest at the axis minimum */
    if (s >= 2 && s < 4) lx = 32767;                         /* stick right */
    if (s >= 4 && s < 6) { lx = -32768; ly = -32768; a = s == 5; }   /* up-left, A fires */
    if (s >= 6 && s < 8) { lx = 32767; ly = 32767; rt = 32767; }     /* down-right, RT fires */
    if (s >= 8 && s < 10) b = 1;                             /* B = ACT */
    SDL_SetJoystickVirtualAxis(g_vpad, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)lx);
    SDL_SetJoystickVirtualAxis(g_vpad, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)ly);
    SDL_SetJoystickVirtualAxis(g_vpad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (Sint16)rt);
    SDL_SetJoystickVirtualAxis(g_vpad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (Sint16)-32768);
    SDL_SetJoystickVirtualButton(g_vpad, SDL_GAMEPAD_BUTTON_SOUTH, a != 0);
    SDL_SetJoystickVirtualButton(g_vpad, SDL_GAMEPAD_BUTTON_EAST, b != 0);
    if (s == 11) SDL_SetJoystickVirtualButton(g_vpad, SDL_GAMEPAD_BUTTON_START, true);
}

int plat_init(int w, int h, int scale, int audio_rate, const char *title)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    if (!SDL_CreateWindowAndRenderer(title, w * scale, h * scale, SDL_WINDOW_RESIZABLE, &g_win, &g_ren)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    /* Integer-scaled nearest-neighbour presentation of the logical frame. */
    SDL_SetRenderLogicalPresentation(g_ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (g_fullscreen) SDL_SetWindowFullscreen(g_win, true);
    SDL_HideCursor();   /* the game draws its own crosshair; the original hides the OS cursor */

    /* One logical device with two bound streams: the video soundtrack (U8 stereo) and the SFX
     * mixer (S16 stereo). A device opened by SDL_OpenAudioDeviceStream refuses extra bindings. */
    SDL_AudioSpec spec = { SDL_AUDIO_U8, 2, audio_rate };
    g_audio_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    g_audio = g_audio_dev ? SDL_CreateAudioStream(&spec, NULL) : NULL;
    if (!g_audio || !SDL_BindAudioStream(g_audio_dev, g_audio)) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec sspec = { SDL_AUDIO_S16, 2, audio_rate };
    g_sfx = SDL_CreateAudioStream(&sspec, NULL);
    if (!g_sfx || !SDL_BindAudioStream(g_audio_dev, g_sfx) ||
        !SDL_SetAudioStreamGetCallback(g_sfx, sfx_fill, NULL)) {
        fprintf(stderr, "sfx: %s (continuing without sound effects)\n", SDL_GetError());
        if (g_sfx) SDL_DestroyAudioStream(g_sfx);
        g_sfx = NULL;
    }
    SDL_ResumeAudioDevice(g_audio_dev);
    plat_set_volume(g_vol);
    g_frame_w = w; g_frame_h = h;

    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&n);
        for (int i = 0; ids && i < n && !g_joy; i++)      /* gamepads go through SDL_Gamepad */
            if (!SDL_IsGamepad(ids[i])) g_joy = SDL_OpenJoystick(ids[i]);
        SDL_free(ids);
        pad_open_first();
        test_vpad_attach();
    }
    return 0;
}

void plat_shutdown(void)
{
    if (g_sfx) SDL_DestroyAudioStream(g_sfx);
    g_sfx = NULL;
    for (int i = 0; i < N_VOICES; i++) { SDL_free(g_voice[i].buf); g_voice[i] = (Voice){ 0 }; }
    if (g_joy) SDL_CloseJoystick(g_joy);
    g_joy = NULL;
    if (g_pad) SDL_CloseGamepad(g_pad);
    g_pad = NULL;
    if (g_vpad) SDL_CloseJoystick(g_vpad);
    g_vpad = NULL;
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    if (g_audio_dev) SDL_CloseAudioDevice(g_audio_dev);
    g_audio_dev = 0;
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_audio = NULL; g_tex = NULL; g_ren = NULL; g_win = NULL;
    SDL_Quit();
}

void plat_present(const uint32_t *xrgb, int w, int h)
{
    if (!g_tex || w != g_tex_w || h != g_tex_h) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!g_tex) { fprintf(stderr, "texture: %s\n", SDL_GetError()); return; }
        SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);
        g_tex_w = w; g_tex_h = h;
        g_frame_w = w; g_frame_h = h;
        SDL_SetRenderLogicalPresentation(g_ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
#ifdef __EMSCRIPTEN__
        {   /* tell the page the picture size (4:3 launcher vs 8:5 game) so it can fit the canvas */
            char js[96];
            snprintf(js, sizeof js, "if (Module.onFrameSize) Module.onFrameSize(%d,%d)", w, h);
            emscripten_run_script(js);
        }
#endif
    }
    SDL_UpdateTexture(g_tex, NULL, xrgb, w * (int)sizeof(uint32_t));
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

int plat_quit_requested(void)
{
    SDL_Event ev;
    int quit = 0;
    int warps = g_warp_pending;     /* motion events our own SDL_WarpMouseInWindow() will cause */
    g_warp_pending = 0;
    test_vpad_script();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            if (warps > 0) warps--; else g_pad_active = 0;     /* the mouse takes back over */
        }
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED) pad_open_first();
        if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && g_pad && SDL_GetGamepadID(g_pad) == ev.gdevice.which) {
            SDL_CloseGamepad(g_pad);
            g_pad = NULL; g_pad_active = 0;
            pad_open_first();
        }
        if (g_pad && ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev.gbutton.which == SDL_GetGamepadID(g_pad)) {
            g_pad_active = 1; g_any_input = 1;
            if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) g_skip = 1;
            if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) g_keys |= PLAT_KEY_F2;
            if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) g_mkeys |= PLAT_MK_UP;
            if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) g_mkeys |= PLAT_MK_DOWN;
        }
        if (g_pad && ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && ev.gaxis.which == SDL_GetGamepadID(g_pad) &&
            (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) &&
            (ev.gaxis.value > PAD_DEAD || ev.gaxis.value < -PAD_DEAD))
            g_pad_active = 1;
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
            (ev.key.key == SDLK_F11 || ((ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) && (ev.key.mod & SDL_KMOD_ALT)))) {
            plat_set_fullscreen(!g_fullscreen);
            continue;               /* not a menu Enter / any-key */
        }
        if (ev.type == SDL_EVENT_QUIT) { quit = 1; g_closed = 1; }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_Q && !(g_win && SDL_TextInputActive(g_win)))
            quit = 1;
        if (ev.type == SDL_EVENT_TEXT_INPUT) SDL_strlcat(g_text, ev.text.text, sizeof g_text);
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
            if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) g_mkeys |= PLAT_MK_ENTER;
            if (ev.key.key == SDLK_ESCAPE) g_mkeys |= PLAT_MK_ESC;
            if (ev.key.key == SDLK_UP) g_mkeys |= PLAT_MK_UP;
            if (ev.key.key == SDLK_DOWN) g_mkeys |= PLAT_MK_DOWN;
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_BACKSPACE) g_mkeys |= PLAT_MK_BACKSPACE; /* repeats too */
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) { g_mpress = 1; g_mdown = 1; }
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_LEFT) { g_mrel = 1; g_mdown = 0; }
        if ((ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            g_any_input = 1;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !ev.key.repeat) g_skip = 1;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_SPACE && !ev.key.repeat) g_act = 1;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F2 && !ev.key.repeat) g_keys |= PLAT_KEY_F2;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F5 && !ev.key.repeat) g_keys |= PLAT_KEY_F5;
    }
    return quit;
}

int plat_audio_queue(const uint8_t *data, size_t n)
{
    return SDL_PutAudioStreamData(g_audio, data, (int)n) ? 0 : -1;
}

size_t plat_audio_queued(void)
{
    int q = SDL_GetAudioStreamQueued(g_audio);
    return q < 0 ? 0 : (size_t)q;
}

int plat_skip_requested(void)
{
    int s = g_skip;
    g_skip = 0;
    return s;
}

void plat_audio_flush(void) { SDL_FlushAudioStream(g_audio); }

void plat_audio_clear(void) { SDL_ClearAudioStream(g_audio); }

int plat_act_requested(void)
{
    int a = g_act;
    g_act = 0;
    return a;
}

int plat_any_input(void)
{
    int a = g_any_input;
    g_any_input = 0;
    return a;
}

int plat_mouse(int *x, int *y, uint32_t *buttons)
{
    if (!g_ren) return 0;
    float wx, wy, lx, ly;
    SDL_MouseButtonFlags b = SDL_GetMouseState(&wx, &wy);
    if (!SDL_RenderCoordinatesFromWindow(g_ren, wx, wy, &lx, &ly)) return 0;
    int ix = (int)lx, iy = (int)ly;
    *x = ix < 0 ? 0 : ix >= g_frame_w ? g_frame_w - 1 : ix;
    *y = iy < 0 ? 0 : iy >= g_frame_h ? g_frame_h - 1 : iy;
    *buttons = ((b & SDL_BUTTON_LMASK) ? 1u : 0u) | ((b & SDL_BUTTON_RMASK) ? 2u : 0u) |
               ((b & SDL_BUTTON_MMASK) ? 4u : 0u);
    return 1;
}

/* One deadzoned stick axis: 0 inside the zone, rescaled to the full range outside. */
static int pad_axis(SDL_GamepadAxis a)
{
    int v = SDL_GetGamepadAxis(g_pad, a);
    if (v > -PAD_DEAD && v < PAD_DEAD) return 0;
    int m = v < 0 ? -v : v;
    m = (m - PAD_DEAD) * 32767 / (32767 - PAD_DEAD);
    if (m > 32767) m = 32767;
    return v < 0 ? -m : m;
}

int plat_gamepad(int *x, int *y, uint32_t *buttons)
{
    if (!g_pad || !g_pad_active) return 0;
    *x = pad_axis(SDL_GAMEPAD_AXIS_LEFTX);
    *y = pad_axis(SDL_GAMEPAD_AXIS_LEFTY);
    uint32_t b = 0;
    if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
        SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= 1u;
    if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_EAST) || SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_WEST) ||
        SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= 4u;
    *buttons = b;
    return 1;
}

void plat_set_fullscreen(int on)
{
    g_fullscreen = on != 0;
    if (g_win) SDL_SetWindowFullscreen(g_win, g_fullscreen);   /* borderless desktop fullscreen */
}

int plat_joystick(int *x, int *y, uint32_t *buttons)
{
    if (!g_joy) return 0;
    *x = SDL_GetJoystickAxis(g_joy, 0);
    *y = SDL_GetJoystickAxis(g_joy, 1);
    *buttons = 0;
    for (int i = 0; i < 3; i++) if (SDL_GetJoystickButton(g_joy, i)) *buttons |= 1u << i;
    return 1;
}

void plat_sfx_play(const uint8_t *pcm, size_t n, int vol, int pan)
{
    if (!g_sfx || !pcm || !n) return;
    SDL_LockAudioStream(g_sfx);
    int best = 0;
    size_t left = (size_t)-1;
    for (int i = 0; i < N_VOICES; i++) {      /* a free voice, else the one closest to its end */
        size_t rem = g_voice[i].n - g_voice[i].pos;
        if (rem < left) { left = rem; best = i; }
    }
    Voice *v = &g_voice[best];
    if (v->cap < n) {
        uint8_t *b = SDL_realloc(v->buf, n);
        if (!b) { SDL_UnlockAudioStream(g_sfx); return; }
        v->buf = b; v->cap = n;
    }
    SDL_memcpy(v->buf, pcm, n);
    v->n = n; v->pos = 0;
    v->gl = vol * (pan > 0 ? 256 - pan : 256) / 256;
    v->gr = vol * (pan < 0 ? 256 + pan : 256) / 256;
    SDL_UnlockAudioStream(g_sfx);
}

void plat_delay_ms(uint32_t ms) { SDL_Delay(ms); }

uint32_t plat_take_keys(void)
{
    uint32_t k = g_keys;
    g_keys = 0;
    return k;
}

int plat_ctrl_held(void) { return (SDL_GetModState() & SDL_KMOD_CTRL) != 0; }

const char *plat_user_dir(void)
{
    static char dir[1024];
    if (!dir[0]) {
        char *p = SDL_GetPrefPath("DeadlyTide", "deadlytide");
        if (p) { SDL_strlcpy(dir, p, sizeof dir); SDL_free(p); }
    }
    return dir;
}

void plat_show_cursor(int show) { if (show) SDL_ShowCursor(); else SDL_HideCursor(); }

void plat_set_volume(int vol)
{
    g_vol = vol < 0 ? 0 : vol > 255 ? 255 : vol;
    float gain = (float)g_vol / 255.0f;
    if (g_audio) SDL_SetAudioStreamGain(g_audio, gain);
    if (g_sfx) SDL_SetAudioStreamGain(g_sfx, gain);
}

void plat_text_input(int on)
{
    if (!g_win) return;
    if (on) SDL_StartTextInput(g_win); else SDL_StopTextInput(g_win);
    g_menu_mode = on;
    g_text[0] = 0; g_mkeys = 0; g_mpress = g_mrel = 0;
#ifdef __EMSCRIPTEN__
    emscripten_run_script(on ? "Module.textInput=true" : "Module.textInput=false");   /* index.html key handling */
#endif
}

void plat_take_menu_input(char *text, size_t n, unsigned *keys, int *pressed, int *released, int *down)
{
    if (g_menu_mode && g_pad && g_ren) {
        /* Gamepad in the launcher: the left stick moves the (real) mouse cursor at a rate, A clicks. */
        int sx = pad_axis(SDL_GAMEPAD_AXIS_LEFTX), sy = pad_axis(SDL_GAMEPAD_AXIS_LEFTY);
        int a = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_SOUTH) != 0;
        if (sx || sy) {
            float wx, wy;
            SDL_GetMouseState(&wx, &wy);
            if (!SDL_RenderCoordinatesFromWindow(g_ren, wx, wy, &g_cur_x, &g_cur_y)) g_cur_x = g_cur_y = 0;
            g_cur_x += (float)sx / 32767.0f * 10.0f;       /* up to 10 picture px per tick */
            g_cur_y += (float)sy / 32767.0f * 10.0f;
            g_cur_x = g_cur_x < 0 ? 0 : g_cur_x > (float)(g_frame_w - 1) ? (float)(g_frame_w - 1) : g_cur_x;
            g_cur_y = g_cur_y < 0 ? 0 : g_cur_y > (float)(g_frame_h - 1) ? (float)(g_frame_h - 1) : g_cur_y;
            if (SDL_RenderCoordinatesToWindow(g_ren, g_cur_x, g_cur_y, &wx, &wy)) {
                SDL_WarpMouseInWindow(g_win, wx, wy);
                g_warp_pending++;
            }
        }
        if (a && !g_pad_btn_prev) { g_mpress = 1; g_mdown = 1; }
        if (!a && g_pad_btn_prev) { g_mrel = 1; g_mdown = 0; }
        g_pad_btn_prev = a;
    }
    if (n) SDL_strlcpy(text, g_text, n);
    g_text[0] = 0;
    *keys = g_mkeys; g_mkeys = 0;
    *pressed = g_mpress; *released = g_mrel; *down = g_mdown;
    g_mpress = 0;
    if (*pressed && *released) { g_mrel = 1; *released = 0; *down = 1; }   /* release next call */
    else g_mrel = 0;
}

int plat_close_requested(void) { return g_closed; }
uint64_t plat_time_ms(void) { return SDL_GetTicks(); }
int plat_is_up(void) { return g_win != NULL; }

typedef struct { const char *name; char *out; size_t n; int found; } FindCtx;

static SDL_EnumerationResult SDLCALL find_cb(void *ud, const char *dirname, const char *fname)
{
    FindCtx *f = ud;
    if (SDL_strcasecmp(fname, f->name) != 0) return SDL_ENUM_CONTINUE;
    size_t dl = SDL_strlen(dirname);
    SDL_snprintf(f->out, f->n, "%s%s%s", dirname, (dl && dirname[dl - 1] != '/' && dirname[dl - 1] != '\\') ? "/" : "", fname);
    f->found = 1;
    return SDL_ENUM_SUCCESS;
}

int plat_find_file(const char *dir, const char *name, char *out, size_t n)
{
    FindCtx f = { name, out, n, 0 };
    SDL_EnumerateDirectory(dir, find_cb, &f);
    return f.found;
}
