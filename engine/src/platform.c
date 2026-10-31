/* platform.c - SDL3 backend for platform.h. */
#include "platform.h"

#include <SDL3/SDL.h>
#include <stdio.h>

static SDL_Window      *g_win;
static SDL_Renderer    *g_ren;
static SDL_Texture     *g_tex;
static int              g_tex_w, g_tex_h;
static SDL_AudioStream *g_audio;

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
    SDL_ResumeAudioStreamDevice(g_audio);
    return 0;
}

void plat_shutdown(void)
{
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
        if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_Q)) quit = 1;
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

void plat_audio_flush(void) { SDL_FlushAudioStream(g_audio); }

void plat_delay_ms(uint32_t ms) { SDL_Delay(ms); }
