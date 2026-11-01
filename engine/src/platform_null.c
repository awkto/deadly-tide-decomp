/*
 * platform_null.c - platform backend with no window and no audio device.
 * Used by the DT_HEADLESS build (cmake -DDT_HEADLESS=ON): no SDL, so the engine can be
 * built and its headless modes (--headless, --dump-frames) run on machines without SDL3,
 * e.g. CI or a quick check. Windowed playback just reports that it isn't available.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "platform.h"

int plat_init(int w, int h, int scale, int audio_rate, const char *title)
{
    (void)w; (void)h; (void)scale; (void)audio_rate; (void)title;
    fprintf(stderr, "platform: headless build, no window/audio (use --headless or --dump-frames)\n");
    return -1;
}

void plat_shutdown(void) {}
void plat_present(const uint32_t *xrgb, int w, int h) { (void)xrgb; (void)w; (void)h; }
int  plat_quit_requested(void) { return 1; }
int  plat_skip_requested(void) { return 0; }
int  plat_audio_queue(const uint8_t *data, size_t n) { (void)data; (void)n; return 0; }
size_t plat_audio_queued(void) { return 0; }
void plat_audio_flush(void) {}
void plat_audio_clear(void) {}

int  plat_mouse(int *x, int *y, uint32_t *buttons) { (void)x; (void)y; (void)buttons; return 0; }
int  plat_joystick(int *x, int *y, uint32_t *buttons) { (void)x; (void)y; (void)buttons; return 0; }
int  plat_act_requested(void) { return 0; }
void plat_sfx_play(const uint8_t *pcm_u8_mono, size_t n, int vol, int pan)
{
    (void)pcm_u8_mono; (void)n; (void)vol; (void)pan;
}

void plat_delay_ms(uint32_t ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t plat_take_keys(void) { return 0; }
int plat_ctrl_held(void) { return 0; }

/* $XDG_DATA_HOME/deadlytide/ or ~/.local/share/deadlytide/ (created on first use). */
const char *plat_user_dir(void)
{
    static char dir[1024];
    if (dir[0]) return dir;
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    char base[900];
    if (x && *x) snprintf(base, sizeof base, "%s", x);
    else if (h && *h) {
        snprintf(base, sizeof base, "%s/.local", h); mkdir(base, 0755);
        snprintf(base, sizeof base, "%s/.local/share", h); mkdir(base, 0755);
    } else return "";
    snprintf(dir, sizeof dir, "%s/deadlytide", base);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/deadlytide/", base);
    return dir;
}
