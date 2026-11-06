/* l96.c - see l96.h. Logic ported from tools/l96.py (verified there). */
#include "l96.h"

#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <dirent.h>
#include <strings.h>
#define DT_CI_PATHS 1
#endif

/* Entries of a flag-1 type-6/2 segment that precede the segment offset (DTPLAYER data). */
#define SKIP_TYPE6 3
#define SKIP_TYPE2 6

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int stdio_read_at(void *ctx, uint64_t off, void *dst, size_t n)
{
    FILE *fp = ctx;
    if (fseek(fp, (long)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, n, fp) == n ? 0 : -1;
}

static uint64_t stdio_size(void *ctx)
{
    FILE *fp = ctx;
    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long n = ftell(fp);
    return n < 0 ? 0 : (uint64_t)n;
}

static void stdio_close(void *ctx) { fclose(ctx); }

#ifdef DT_CI_PATHS
/* Case-insensitive fallback for POSIX (CD files copied as logos.l96, CD1/ vs cd1/ ...): resolve
 * each path component against its directory listing. 1 = resolved into out. */
static int ci_resolve(const char *path, char *out, size_t n)
{
    size_t len = strlen(path);
    if (len == 0 || len >= n) return 0;
    char cur[1024];
    size_t cl = 0;
    const char *p = path;
    if (*p == '/') { cur[cl++] = '/'; p++; }
    while (*p) {
        const char *e = strchr(p, '/');
        size_t cn = e ? (size_t)(e - p) : strlen(p);
        if (cn == 0) { p++; continue; }
        char comp[256];
        if (cn >= sizeof comp) return 0;
        memcpy(comp, p, cn); comp[cn] = 0;
        char dir[1024];
        if (cl) { memcpy(dir, cur, cl); dir[cl] = 0; } else snprintf(dir, sizeof dir, ".");
        int found = 0;
        DIR *d = opendir(dir);
        if (!d) return 0;
        struct dirent *de;
        char best[256] = "";
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, comp)) { snprintf(best, sizeof best, "%s", de->d_name); found = 1; break; }
            if (!found && !best[0] && !strcasecmp(de->d_name, comp)) snprintf(best, sizeof best, "%s", de->d_name);
        }
        closedir(d);
        if (!best[0]) return 0;
        if (cl && cur[cl - 1] != '/') cur[cl++] = '/';
        size_t bl = strlen(best);
        if (cl + bl + 2 >= sizeof cur) return 0;
        memcpy(cur + cl, best, bl); cl += bl; cur[cl] = 0;
        p += cn;
    }
    cur[cl] = 0;
    if (cl >= n) return 0;
    memcpy(out, cur, cl + 1);
    return 1;
}
#endif

static int stdio_open(const char *path, L96Reader *out)
{
    FILE *fp = fopen(path, "rb");
#ifdef DT_CI_PATHS
    if (!fp) {
        char real[1024];
        if (ci_resolve(path, real, sizeof real)) fp = fopen(real, "rb");
    }
#endif
    if (!fp) return -1;
    out->ctx = fp; out->read_at = stdio_read_at; out->size = stdio_size; out->close = stdio_close;
    return 0;
}

static L96OpenFn g_open = stdio_open;

void l96_set_open_fn(L96OpenFn fn) { g_open = fn ? fn : stdio_open; }

int l96_exists(const char *path)
{
    L96Reader r;
    if (g_open(path, &r) != 0) return 0;
    if (r.close) r.close(r.ctx);
    return 1;
}

int l96_read(const L96File *f, uint64_t off, void *dst, size_t n)
{
    return f->rd.read_at(f->rd.ctx, off, dst, n);
}

static int read_at(const L96Reader *r, int64_t off, void *dst, size_t n)
{
    return r->read_at(r->ctx, (uint64_t)off, dst, n);
}

static size_t entry_bytes(const uint8_t *e)
{
    return ((size_t)e[0] + e[1] + e[2] + e[3]) * L96_SECTOR;
}

#define WHERE_VIRTUAL (-1)   /* L96Segment.where: assembled from carry sectors */
#define WHERE_BAD     (-2)   /* before the start of the file: unreadable */

/* Compute the physical offset of every group of a segment (see docs section 6). */
static int layout_segment(const L96File *f, L96Segment *sg)
{
    const uint8_t *es = f->index + 8 * (size_t)sg->first;
    int n = sg->count;
    sg->where = malloc((size_t)(n ? n : 1) * sizeof *sg->where);
    if (!sg->where) return -1;
    if ((sg->type == 6 || sg->type == 2) && sg->flag == 1) {
        int skip = sg->type == 6 ? SKIP_TYPE6 : SKIP_TYPE2;
        int64_t o = sg->offset;
        for (int k = 0; k < skip - 1 && k < n; k++) o -= (int64_t)entry_bytes(es + 8 * k);
        for (int k = 0; k < n; k++) {
            if (k == skip - 1 || k == n - 1) { sg->where[k] = WHERE_VIRTUAL; continue; }
            sg->where[k] = o < 0 ? WHERE_BAD : o;   /* a corrupt offset must not look like a virtual group */
            o += (int64_t)entry_bytes(es + 8 * k);
        }
    } else {
        int64_t o = sg->offset;
        for (int k = 0; k < n; k++) { sg->where[k] = o; o += (int64_t)entry_bytes(es + 8 * k); }
    }
    return 0;
}

int l96_open(L96File *f, const char *path)
{
    memset(f, 0, sizeof *f);
    if (g_open(path, &f->rd) != 0) { fprintf(stderr, "l96: cannot open %s\n", path); return -1; }

    uint8_t h[0x94];
    if (read_at(&f->rd, 0, h, sizeof h) != 0 || memcmp(h, "L96\0", 4) != 0) {
        fprintf(stderr, "l96: %s: not an L96 file\n", path); goto fail;
    }
    memcpy(f->name, h + 4, 8);
    f->version = rd32(h + 0x0c);
    if (f->version != 5) { fprintf(stderr, "l96: unsupported version %u\n", f->version); goto fail; }

    f->n_names    = rd32(h + 0x18);
    f->n_segments = rd32(h + 0x2c);
    f->n_index    = rd32(h + 0x3c);
    {   /* The tables must fit in the file: refuse absurd counts before allocating for them. */
        uint64_t fsz = f->rd.size ? f->rd.size(f->rd.ctx) : UINT64_MAX;
        if ((uint64_t)f->n_names * 8 > fsz || (uint64_t)f->n_index * 8 > fsz ||
            (uint64_t)f->n_segments * L96_SEGMENT_RECORD > fsz) {
            fprintf(stderr, "l96: %s: table sizes exceed the file\n", path); goto fail;
        }
    }
    uint32_t off_names = rd32(h + 0x40), off_pal = rd32(h + 0x48);
    uint32_t off_segs = rd32(h + 0x5c), off_index = rd32(h + 0x6c);

    f->names = calloc(f->n_names ? f->n_names : 1, sizeof *f->names);
    f->index = malloc((size_t)f->n_index * 8 + 1);
    f->segments = calloc(f->n_segments ? f->n_segments : 1, sizeof *f->segments);
    if (!f->names || !f->index || !f->segments) goto fail;

    for (uint32_t i = 0; i < f->n_names; i++) {
        if (read_at(&f->rd, (int64_t)off_names + 8 * i, f->names[i], 8) != 0) goto fail;
        f->names[i][8] = 0;
    }
    if (read_at(&f->rd, off_pal, f->palette, sizeof f->palette) != 0) goto fail;
    if (read_at(&f->rd, off_index, f->index, (size_t)f->n_index * 8) != 0) goto fail;

    for (uint32_t s = 0; s < f->n_segments; s++) {
        L96Segment *sg = &f->segments[s];
        if (read_at(&f->rd, (int64_t)off_segs + (int64_t)L96_SEGMENT_RECORD * s, sg->raw, sizeof sg->raw) != 0)
            goto fail;
        sg->type   = (int32_t)rd32(sg->raw);
        sg->flag   = (int32_t)rd32(sg->raw + 4);
        sg->offset = rd32(sg->raw + 8);
        sg->first  = (int16_t)(sg->raw[0x0c] | sg->raw[0x0d] << 8);
        sg->next   = (int8_t)sg->raw[0x0e];
        sg->count  = sg->raw[0x0f];
        if (sg->first < 0 || (uint32_t)(sg->first + sg->count) > f->n_index) {
            fprintf(stderr, "l96: segment %u index range out of bounds\n", s); goto fail;
        }
        if (layout_segment(f, sg) != 0) goto fail;
    }
    return 0;
fail:
    l96_close(f);
    return -1;
}

void l96_close(L96File *f)
{
    if (f->segments) for (uint32_t s = 0; s < f->n_segments; s++) free(f->segments[s].where);
    free(f->segments); free(f->index); free(f->names);
    if (f->rd.close) f->rd.close(f->rd.ctx);
    memset(f, 0, sizeof *f);
}

/* Slice a raw group buffer into its audio/video/fg/carry parts and find the frames. */
static int parse_group(L96Group *g, const uint8_t *e)
{
    g->n_video = e[0]; g->n_fg = e[1]; g->n_carry = e[2]; g->n_audio = e[3];
    size_t a = (size_t)g->n_audio * L96_SECTOR, v = (size_t)g->n_video * L96_SECTOR;
    size_t fg = (size_t)g->n_fg * L96_SECTOR, c = (size_t)g->n_carry * L96_SECTOR;
    if (a + v + fg + c > g->size) return -1;
    g->audio = g->raw;          g->audio_size = a;
    g->video = g->raw + a;      g->video_size = v;
    g->fg    = g->raw + a + v;  g->fg_size = fg;
    g->carry = g->raw + a + v + fg; g->carry_size = c;

    /* Exactly 5 records {0x44 header, Cinepak frame}; bytes after them are mastering junk. */
    size_t o = 0;
    g->n_frames = 0;
    while (o + L96_FRAME_HDR_SIZE <= v && g->n_frames < L96_FRAMES_PER_GROUP) {
        uint32_t sz = rd32(g->video + o);
        if (sz == 0) break;
        size_t fo = o + L96_FRAME_HDR_SIZE;
        const uint8_t *cp = g->video + fo;
        if (fo + 10 > v || fo + sz > v) return -1;
        /* Cinepak's own 24-bit big-endian length must agree with the record header. */
        if ((uint32_t)(cp[1] << 16 | cp[2] << 8 | cp[3]) != sz || cp[0] > 1) return -1;
        g->frame[g->n_frames].hdr  = g->video + o;
        g->frame[g->n_frames].data = cp;
        g->frame[g->n_frames].size = sz;
        g->n_frames++;
        o = fo + sz;
    }
    return 0;
}

int l96_read_group(const L96File *f, int s, int k, L96Group *g)
{
    memset(g, 0, sizeof *g);
    if (s < 0 || (uint32_t)s >= f->n_segments) return -1;
    const L96Segment *sg = &f->segments[s];
    if (k < 0 || k >= sg->count) return -1;
    const uint8_t *e = f->index + 8 * (size_t)(sg->first + k);

    if (sg->where[k] == WHERE_BAD) return -1;
    if (sg->where[k] >= 0) {
        g->size = entry_bytes(e);
        g->raw = malloc(g->size ? g->size : 1);
        if (!g->raw || read_at(&f->rd, sg->where[k], g->raw, g->size) != 0) {
            fprintf(stderr, "l96: short read for segment %d group %d\n", s, k);
            l96_group_free(g); return -1;
        }
    } else {
        /* Virtual group: concatenated carry sectors of its donors. */
        int skip = sg->type == 6 ? SKIP_TYPE6 : SKIP_TYPE2;
        int donors[256], nd = 0;
        if (k == skip - 1) { for (int j = 0; j < skip - 1; j++) donors[nd++] = j; }
        else for (int j = skip; j < sg->count - 1; j++)
            if (f->index[8 * (size_t)(sg->first + j) + 2]) donors[nd++] = j;
        g->virtual_ = 1;
        size_t total = 0;
        for (int i = 0; i < nd; i++) total += (size_t)f->index[8 * (size_t)(sg->first + donors[i]) + 2] * L96_SECTOR;
        g->raw = malloc(total ? total : 1);
        if (!g->raw) return -1;
        for (int i = 0; i < nd; i++) {
            L96Group d;
            if (l96_read_group(f, s, donors[i], &d) != 0) { l96_group_free(g); return -1; }
            memcpy(g->raw + g->size, d.carry, d.carry_size);
            g->size += d.carry_size;
            l96_group_free(&d);
        }
        /* A virtual group is raw carry data in the same layout as a stored group. */
        if (g->size != entry_bytes(e)) {
            /* Size mismatch: keep the raw bytes but expose no sub-blocks. */
            g->audio = g->raw;
            return 0;
        }
    }
    if (parse_group(g, e) != 0) {
        fprintf(stderr, "l96: malformed group (segment %d group %d)\n", s, k);
        l96_group_free(g); return -1;
    }
    return 0;
}

void l96_group_free(L96Group *g)
{
    free(g->raw);
    memset(g, 0, sizeof *g);
}
