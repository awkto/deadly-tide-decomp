/*
 * platform.h - the only interface between the engine and the OS/SDL.
 * All SDL3 calls live in platform.c; swapping this file is all a different
 * backend (or an Emscripten build) needs.
 */
#ifndef DT_PLATFORM_H
#define DT_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Open a window of (w*scale) x (h*scale) and a u8 stereo audio output at `rate` Hz. */
int  plat_init(int w, int h, int scale, int audio_rate, const char *title);
void plat_shutdown(void);

/* Show a w*h XRGB8888 frame (scaled nearest, aspect kept). w/h may change between calls. */
void plat_present(const uint32_t *xrgb, int w, int h);

/* Process window events; returns nonzero when the user asked to quit (close, Q). */
int  plat_quit_requested(void);
/* Nonzero once per Esc press seen by plat_quit_requested() (the game's skip key). */
int  plat_skip_requested(void);

/* Audio: u8 interleaved stereo. queued() = bytes accepted but not yet consumed by the device. */
int    plat_audio_queue(const uint8_t *data, size_t n);
size_t plat_audio_queued(void);
void   plat_audio_flush(void);   /* no more audio coming: let the device play out the tail */
void   plat_audio_clear(void);   /* drop everything queued (branch / skip) */

void plat_delay_ms(uint32_t ms);

#endif
