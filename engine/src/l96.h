/*
 * l96.h - reader for Deadly Tide ".L96" level containers.
 *
 * Format reference: docs/L96_FORMAT.md. Reference implementation: tools/l96.py.
 *
 * The file is never loaded whole: the header tables (segments, index, names)
 * are read at open time, and every group is fetched with one seek + read.
 */
#ifndef DT_L96_H
#define DT_L96_H

#include <stdint.h>
#include <stdio.h>

#define L96_SECTOR          0x800u
#define L96_SEGMENT_RECORD  100u
#define L96_FRAME_HDR_SIZE  0x44u   /* per-frame header preceding each Cinepak frame */
#define L96_FRAMES_PER_GROUP 5
#define L96_AUDIO_BYTES     14700u  /* 1/3 s of u8 stereo @ 22050 Hz */
#define L96_AUDIO_RATE      22050
#define L96_VIDEO_FPS       15

typedef struct {
    int32_t  type;      /* 0 video, 1 video+gameplay, 6 no video; see docs */
    int32_t  flag;      /* 2 stored normally, 1 head is prefetched elsewhere */
    uint32_t offset;    /* file offset of the segment's stream data */
    int      first;     /* first index entry */
    int      next;      /* next segment; negative = linked file names[-1-next] */
    int      count;     /* number of index entries (= groups) */
    uint8_t  raw[L96_SEGMENT_RECORD]; /* full record, for fields not decoded yet */
    int64_t *where;     /* per-group file offset, -1 for a virtual group */
} L96Segment;

typedef struct {
    FILE       *fp;
    char        name[9];
    uint32_t    version;
    uint32_t    n_names, n_segments, n_index;
    char      (*names)[9];        /* linked file names, NUL terminated */
    uint8_t    *index;            /* n_index x 8 bytes */
    L96Segment *segments;
    uint8_t     palette[256][3];  /* RGB */
} L96File;

/* One group: 1/3 s of audio + up to 5 video frames (+ gameplay + carry). */
typedef struct {
    uint8_t *raw;         /* whole group, owned */
    size_t   size;
    int      virtual_;    /* assembled from carry sectors of other groups */
    uint8_t  n_video, n_fg, n_carry, n_audio;   /* sector counts */
    /* Raw sub-blocks (point into raw; sizes are sector multiples). */
    const uint8_t *audio;  size_t audio_size;   /* u8 stereo 22050 Hz, first L96_AUDIO_BYTES valid */
    const uint8_t *video;  size_t video_size;
    const uint8_t *fg;     size_t fg_size;      /* gameplay block, undecoded */
    const uint8_t *carry;  size_t carry_size;
    /* Parsed video frames. */
    int n_frames;
    struct {
        const uint8_t *hdr;      /* L96_FRAME_HDR_SIZE raw header bytes */
        const uint8_t *data;     /* Cinepak frame */
        uint32_t       size;
    } frame[L96_FRAMES_PER_GROUP];
} L96Group;

/* Returns 0 on success, -1 on failure (message on stderr). */
int  l96_open(L96File *f, const char *path);
void l96_close(L96File *f);

/* Read group k of segment s. Free with l96_group_free. 0 on success. */
int  l96_read_group(const L96File *f, int s, int k, L96Group *g);
void l96_group_free(L96Group *g);

#endif
