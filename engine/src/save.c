/* save.c - see save.h. */
#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clampi(int v, int lo, int hi, int def) { return v < lo || v > hi ? def : v; }

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

/* Is `line` the header of `pilot`'s section? */
static int is_section(const char *line, const char *pilot)
{
    char want[96];
    snprintf(want, sizeof want, "[pilot %s]", pilot);
    return !strcmp(line, want);
}

void save_load(SaveData *s, const char *path, const char *pilot)
{
    memset(s, 0, sizeof *s);
    snprintf(s->path, sizeof s->path, "%s", path ? path : "");
    snprintf(s->pilot, sizeof s->pilot, "%s", pilot && *pilot ? pilot : "Player");
    s->act_num = 5;
    s->difficulty = 1;
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
    }
    fclose(fp);
}

int save_store(const SaveData *s)
{
    if (!s->path[0]) return 0;
    /* Keep every other pilot's section. */
    char *keep = NULL;
    size_t len = 0, cap = 0;
    FILE *fp = fopen(s->path, "r");
    if (fp) {
        char line[256];
        int skip = 0;
        while (fgets(line, sizeof line, fp)) {
            char t[256];
            snprintf(t, sizeof t, "%s", line);
            trim(t);
            if (t[0] == '[') skip = is_section(t, s->pilot);
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
    snprintf(tmp, sizeof tmp, "%s.tmp", s->path);
    fp = fopen(tmp, "w");
    if (!fp) { free(keep); return -1; }
    fprintf(fp, "; Deadly Tide progress (registry equivalent: HighestLevel, SavePoint, ACTNum, DifficultyLevel)\n");
    if (keep) fputs(keep, fp);
    fprintf(fp, "[pilot %s]\nHighestLevel=%d\nSavePoint=%d\nACTNum=%d\nDifficultyLevel=%d\n",
            s->pilot, s->highest_level, s->save_point, s->act_num, s->difficulty);
    free(keep);
    if (fclose(fp) != 0) return -1;
    if (rename(tmp, s->path) != 0) {          /* Windows: rename does not replace */
        remove(s->path);
        if (rename(tmp, s->path) != 0) { remove(tmp); return -1; }
    }
    return 0;
}
