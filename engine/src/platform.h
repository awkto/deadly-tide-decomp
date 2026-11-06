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

/* Input. Mouse position in frame coordinates (0..w-1 x 0..h-1 of the last presented frame,
 * clamped); buttons bit 0 left, bit 1 right, bit 2 middle. Returns 0 when there is no pointer. */
int  plat_mouse(int *x, int *y, uint32_t *buttons);
/* First joystick, if any: axes -32768..32767, buttons bits 0..2 = buttons 1..3. 0 = none. */
int  plat_joystick(int *x, int *y, uint32_t *buttons);
/* First SDL_Gamepad, if one is connected AND has taken over from the mouse (stick moved past the
 * deadzone or a button pressed, until the mouse moves again). x,y: left stick, deadzoned and
 * rescaled to -32767..32767 (up = negative y); buttons: bit 0 fire (A / right trigger), bit 2 ACT
 * (B, X or left trigger). Start counts as Esc, Back as F2 (plat_skip_requested / plat_take_keys). */
int  plat_gamepad(int *x, int *y, uint32_t *buttons);
/* Borderless desktop fullscreen on/off (F11 and Alt+Enter toggle it inside plat_quit_requested). */
void plat_set_fullscreen(int on);
/* Nonzero once per Space press seen by plat_quit_requested() (the ACT key). */
int  plat_act_requested(void);
/* Nonzero once per any key-down or mouse-button-down seen by plat_quit_requested() (demo exit). */
int  plat_any_input(void);

/* Sound effects: u8 mono PCM at the audio rate, mixed over the streamed audio.
 * vol 0..256 (256 = full), pan -256 (left) .. 256 (right). The PCM is copied. */
void plat_sfx_play(const uint8_t *pcm_u8_mono, size_t n, int vol, int pan);

void plat_delay_ms(uint32_t ms);

/* Function keys seen by plat_quit_requested(), each reported once: PLAT_KEY_* bits. */
#define PLAT_KEY_F2 1u
#define PLAT_KEY_F5 2u
#define PLAT_KEY_F1 4u
#define PLAT_KEY_PAUSE 8u   /* Pause or F3, or the window lost focus */
#define PLAT_KEY_F4 16u
#define PLAT_KEY_F12 32u
uint32_t plat_take_keys(void);
/* Ctrl currently held (the hub's "restart the level" pick modifier). */
int  plat_ctrl_held(void);
/* ---- Launcher-screen support (native windowed build only; the web build has its own UI) ---- */
/* Show / hide the OS mouse cursor (hidden in the game, visible in the menu). */
void plat_show_cursor(int show);
/* Pause / resume all audio output (queued audio is kept, so the stream position does not move). */
void plat_audio_pause(int paused);
/* Current volume 0..255. */
int  plat_get_volume(void);
/* Save a w*h XRGB8888 frame as a BMP file. 0 = ok. */
int  plat_save_bmp(const uint32_t *xrgb, int w, int h, const char *path);
/* Volume 0..255 for everything the platform plays (soundtrack + sound effects). Default 255. */
void plat_set_volume(int vol);
/* Text-input mode (SDL_StartTextInput). While on, the Q key no longer quits (it is typed text);
 * the window close button still does. */
void plat_text_input(int on);
/* Menu events gathered by plat_quit_requested() since the previous call. text: UTF-8 typed, may be
 * empty; keys: PLAT_MK_* bits; pressed/released: left-button edges (never both in one call: a
 * click shorter than a poll reports the release on the next call); down: left button held. */
#define PLAT_MK_ENTER 1u
#define PLAT_MK_BACKSPACE 2u
#define PLAT_MK_ESC 4u
#define PLAT_MK_UP 8u
#define PLAT_MK_DOWN 16u
void plat_take_menu_input(char *text, size_t n, unsigned *keys, int *pressed, int *released, int *down);
/* Nonzero (sticky) once the window's close button was used. */
int  plat_close_requested(void);
/* Monotonic milliseconds. */
uint64_t plat_time_ms(void);
/* Find the file `name` (case-insensitive) in `dir`; writes the real path to out. 1 = found. */
int  plat_find_file(const char *dir, const char *name, char *out, size_t n);
/* Whether plat_init() already succeeded (window open). */
int  plat_is_up(void);

/* Per-user writable directory for the save file, ending in a path separator ("" if none). */
const char *plat_user_dir(void);

/* ---- First-run helpers (native build) ---- */
/* Folder of the executable, with a trailing separator ("" if unknown). */
const char *plat_base_dir(void);
/* Modal message box (works before plat_init). */
void plat_message(const char *title, const char *text);
/* Native "choose folder" dialog; blocks (pumping events) until answered. 1 = chosen, written to out. */
int  plat_choose_folder(const char *start, char *out, size_t n);

#endif
