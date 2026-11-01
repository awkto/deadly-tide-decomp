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
} SaveData;

/* Load `pilot` from `path` (missing file / section / key -> defaults). path may be NULL. */
void save_load(SaveData *s, const char *path, const char *pilot);
/* Rewrite the file with this pilot's section replaced (other pilots kept). 0 = ok. */
int  save_store(const SaveData *s);

#endif
