/* save.c - see save.h. */
#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clampi(int v, int lo, int hi, int def) { return v < lo || v > hi ? def : v; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return !*a && !*b;
}
static int icmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

/* Is `line` the header of `pilot`'s section? (names are case-insensitive like registry keys) */
static int is_section(const char *line, const char *pilot)
{
    char want[96];
    snprintf(want, sizeof want, "[pilot %s]", pilot);
    return ieq(line, want);
}

#define GLOBAL_SEC "[global]"

void save_load(SaveData *s, const char *path, const char *pilot)
{
    memset(s, 0, sizeof *s);
    snprintf(s->path, sizeof s->path, "%s", path ? path : "");
    snprintf(s->pilot, sizeof s->pilot, "%s", pilot && *pilot ? pilot : "Player");
    s->act_num = 5;
    s->difficulty = 1;
    s->quality = 2;
    s->volume = 128;
    FILE *fp = path && *path ? fopen(path, "r") : NULL;
    if (!fp) return;
    char line[256];
    int in = 0;
    while (fgets(line, sizeof line, fp)) {
        trim(line);
        if (line[0] == '[') { in = is_section(line, s->pilot); continue; }
        if (!in) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "HighestLevel"))         s->highest_level = clampi(v, 0, 20, 0);
        else if (!strcmp(line, "SavePoint"))       s->save_point = clampi(v, 0, 1000, 0);
        else if (!strcmp(line, "ACTNum"))          s->act_num = clampi(v, 0, 7, 5);
        else if (!strcmp(line, "DifficultyLevel")) s->difficulty = clampi(v, 0, 2, 1);
        else if (!strcmp(line, "GraphicQuality"))  s->quality = clampi(v, 0, 2, 2);
        else if (!strcmp(line, "UseJoystick"))     s->use_joystick = clampi(v, 0, 1, 0);
        else if (!strcmp(line, "FlipControl"))     s->flip = clampi(v, 0, 1, 0);
        else if (!strcmp(line, "Volume"))          s->volume = clampi(v, 0, 255, 128);
    }
    fclose(fp);
}

/* Rewrite `path`: drop the pilot section `skip_pilot` and the section with header `skip_hdr` (either may
 * be NULL) plus comments / blank lines, keep the rest, then append `add` (may be NULL). */
static void (*g_hook)(const char *path);
void save_set_hook(void (*fn)(const char *path)) { g_hook = fn; }

static int rewrite(const char *path, const char *skip_pilot, const char *skip_hdr, const char *add)
{
    char *keep = NULL;
    size_t len = 0, cap = 0;
    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        int skip = 0;
        while (fgets(line, sizeof line, fp)) {
            char t[256];
            snprintf(t, sizeof t, "%s", line);
            trim(t);
            if (t[0] == '[') skip = (skip_pilot && is_section(t, skip_pilot)) || (skip_hdr && ieq(t, skip_hdr));
            if (skip || t[0] == ';' || !t[0]) continue;
            size_t n = strlen(line);
            if (len + n + 2 > cap) {
                size_t nc = cap ? cap * 2 : 1024;
                while (nc < len + n + 2) nc *= 2;
                char *k = realloc(keep, nc);
                if (!k) { free(keep); fclose(fp); return -1; }
                keep = k; cap = nc;
            }
            memcpy(keep + len, line, n);
            len += n;
            if (line[n - 1] != '\n') keep[len++] = '\n';
            keep[len] = 0;
        }
        fclose(fp);
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (!fp) { free(keep); return -1; }
    fprintf(fp, "; Deadly Tide progress (registry equivalent: HighestLevel, SavePoint, ACTNum, DifficultyLevel,\n"
                "; GraphicQuality, UseJoystick, FlipControl, Volume; [global] CurrentPilot = CurrentPlayer)\n");
    if (keep) fputs(keep, fp);
    if (add) fputs(add, fp);
    free(keep);
    if (fclose(fp) != 0) return -1;
    if (rename(tmp, path) != 0) {          /* Windows: rename does not replace */
        remove(path);
        if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    }
    if (g_hook) g_hook(path);
    return 0;
}

int save_store(const SaveData *s)
{
    if (!s->path[0]) return 0;
    int hl = s->highest_level;
    if (ieq(s->pilot, "Poseidon")) hl = 20;   /* original launcher quirk: always everything unlocked */
    char add[512];
    snprintf(add, sizeof add,
             "[pilot %s]\nHighestLevel=%d\nSavePoint=%d\nACTNum=%d\nDifficultyLevel=%d\n"
             "GraphicQuality=%d\nUseJoystick=%d\nFlipControl=%d\nVolume=%d\n",
             s->pilot, hl, s->save_point, s->act_num, s->difficulty, s->quality, s->use_joystick, s->flip,
             s->volume);
    return rewrite(s->path, s->pilot, NULL, add);
}

int save_list_pilots(const char *path, char names[][64], int max)
{
    int n = 0;
    FILE *fp = path && *path ? fopen(path, "r") : NULL;
    if (!fp) return 0;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        trim(line);
        if (strncmp(line, "[pilot ", 7) != 0) continue;
        size_t l = strlen(line);
        if (l < 9 || line[l - 1] != ']') continue;
        line[l - 1] = 0;
        const char *nm = line + 7;
        if (strlen(nm) > 63) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (ieq(names[i], nm)) dup = 1;
        if (dup || n >= max) continue;
        snprintf(names[n++], 64, "%s", nm);
    }
    fclose(fp);
    for (int i = 1; i < n; i++) {                /* insertion sort, case-insensitive */
        char t[64];
        memcpy(t, names[i], 64);
        int j = i - 1;
        while (j >= 0 && icmp(names[j], t) > 0) { memcpy(names[j + 1], names[j], 64); j--; }
        memcpy(names[j + 1], t, 64);
    }
    return n;
}

int save_delete_pilot(const char *path, const char *pilot)
{
    if (!path || !*path) return 0;
    return rewrite(path, pilot, NULL, NULL);
}

int save_get_current(const char *path, char *out, int n)
{
    out[0] = 0;
    FILE *fp = path && *path ? fopen(path, "r") : NULL;
    if (!fp) return 0;
    char line[256];
    int in = 0;
    while (fgets(line, sizeof line, fp)) {
        trim(line);
        if (line[0] == '[') { in = ieq(line, GLOBAL_SEC); continue; }
        if (in && !strncmp(line, "CurrentPilot=", 13)) { snprintf(out, (size_t)n, "%s", line + 13); break; }
    }
    fclose(fp);
    return (int)strlen(out);
}

int save_set_current(const char *path, const char *pilot)
{
    if (!path || !*path) return 0;
    char add[128];
    snprintf(add, sizeof add, GLOBAL_SEC "\nCurrentPilot=%s\n", pilot ? pilot : "");
    return rewrite(path, NULL, GLOBAL_SEC, add);
}
