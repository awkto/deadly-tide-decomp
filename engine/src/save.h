/*
 * save.h - per-pilot progress, the equivalent of the original's registry values
 * (docs/SCENE_GRAPH.md §3, docs/COMBAT_SPEC.md §11): HighestLevel, SavePoint, ACTNum and
 * DifficultyLevel, kept in a small INI file (one [pilot NAME] section per pilot).
 *
 * The original stores each value as an obfuscated DWORD under HKCU\...\<CurrentPlayer>
 * (Reg_NameHash 0x40d240): fold = XOR of the pilot name as little-endian u16 words,
 * enc = value ^ kA ^ fold, chk = enc ^ kB ^ fold, dword = enc << 16 | chk, with
 *   HighestLevel kA 0xbeef kB 0xcafe (default 0, max 20)
 *   SavePoint    kA 0x9021 kB 0x2001 (default 0, max 1000)
 *   ACTNum       kA 0x3772 kB 0x1169 (default 5, max 7)
 * DifficultyLevel is stored plain. Our file stores plain numbers; the keys are documented only.
 *
 * Platform free: the caller supplies the path (platform.h: plat_user_dir()).
 */
#ifndef DT_SAVE_H
#define DT_SAVE_H

typedef struct {
    char path[1024];          /* "" = nothing is persisted (in-memory only) */
    char pilot[64];
    int  highest_level;       /* 0..20 */
    int  save_point;          /* 0..1000, segment index in the frontier level */
    int  act_num;             /* 0..7, ACT charges at the save point */
    int  difficulty;          /* registry DifficultyLevel 0..2 (1 = default) */
    /* launcher settings, per pilot like the original (docs/LAUNCHER.md §3) */
    int  quality;             /* GraphicQuality 0..2 (default 2) */
    int  use_joystick;        /* UseJoystick 0/1 (default 0) */
    int  flip;                /* FlipControl 0/1 (default 0) */
    int  volume;              /* Volume 0..255 (default 128) */
} SaveData;

/* Load `pilot` from `path` (missing file / section / key -> defaults). path may be NULL. */
void save_load(SaveData *s, const char *path, const char *pilot);
/* Rewrite the file with this pilot's section replaced (other pilots kept). 0 = ok.
 * Original quirk (LAUNCHER.md §3): the pilot "Poseidon" (any case) is saved with HighestLevel 20. */
int  save_store(const SaveData *s);

/* Pilot list: names of all [pilot X] sections, sorted case-insensitively, unique, at most `max`.
 * Returns the count (0 if the file is missing). */
int  save_list_pilots(const char *path, char names[][64], int max);
/* Remove a pilot's section (progress included, like RegDeleteKey). 0 = ok (also if absent). */
int  save_delete_pilot(const char *path, const char *pilot);
/* The "current pilot" global key (registry CurrentPlayer). get: "" if unset; returns length. */
int  save_get_current(const char *path, char *out, int n);
int  save_set_current(const char *path, const char *pilot);

#endif
