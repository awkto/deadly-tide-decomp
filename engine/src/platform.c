/* platform.c - SDL3 backend for platform.h. */
#include "platform.h"

#include <SDL3/SDL.h>
#include <stdio.h>

static SDL_Window      *g_win;
static SDL_Renderer    *g_ren;
static SDL_Texture     *g_tex;
static int              g_tex_w, g_tex_h;
static SDL_AudioStream *g_audio;
static int              g_skip, g_act, g_any_input;
static uint32_t         g_keys;
static int              g_frame_w = 320, g_frame_h = 200;
static SDL_Joystick    *g_joy;

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

int plat_init(int w, int h, int scale, int audio_rate, const char *title)
{
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

    SDL_AudioSpec spec = { SDL_AUDIO_U8, 2, audio_rate };
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_audio) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
        return -1;
    }
    /* Sound effects: S16 stereo, bound to the same (logical) device as the stream audio. */
    SDL_AudioSpec sspec = { SDL_AUDIO_S16, 2, audio_rate };
    g_sfx = SDL_CreateAudioStream(&sspec, &sspec);
    if (!g_sfx || !SDL_BindAudioStream(SDL_GetAudioStreamDevice(g_audio), g_sfx) ||
        !SDL_SetAudioStreamGetCallback(g_sfx, sfx_fill, NULL)) {
        fprintf(stderr, "sfx: %s (continuing without sound effects)\n", SDL_GetError());
        if (g_sfx) SDL_DestroyAudioStream(g_sfx);
        g_sfx = NULL;
    }
    SDL_ResumeAudioStreamDevice(g_audio);
    g_frame_w = w; g_frame_h = h;

    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&n);
        if (ids && n > 0) g_joy = SDL_OpenJoystick(ids[0]);
        SDL_free(ids);
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
    if (g_audio) SDL_DestroyAudioStream(g_audio);
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
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) quit = 1;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_Q) quit = 1;
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
