"""Deadly Tide (1996) .L96 container reader.

Driven entirely by the file header and its tables (no pattern scanning).
Full format description: docs/L96_FORMAT.md.

    python3 tools/l96.py info  FILE      header, sections, segment/stream summary
    python3 tools/l96.py check FILE      walk every group, report inconsistencies (exit 1 if any)

Library use:
    f = L96(path)
    f.header, f.names, f.palette, f.segments, f.index
    for g in f.groups(): ...           # every group (stored + virtual) in segment order
        g.audio  -> bytes  (u8 stereo PCM 22050 Hz, zero padded to the sector)
        g.frames -> list of (hdr28: bytes, cinepak: bytes)   320x200 Cinepak frames
        g.fg     -> bytes  ('FG ' block, gameplay/foreground data, raw)
        g.carry  -> bytes  (prefetch sectors carried for another group, raw)
"""
import mmap, struct, sys, collections

SECTOR = 0x800
SEG_REC = 100          # segment record size (FUN_00413c40 reads n x 100)
AUDIO_BYTES = 14700    # 1/3 s of u8 stereo 22050 Hz = 5 frames @ 15 fps
TYPE6_SKIP = 3         # DTPLAYER .data 0x4210f0: entries of a type-6 segment not at its offset
TYPE2_SKIP = 6         # DTPLAYER .data 0x4210e4: same for type 2 (no type-2 segments shipped)

# (offset, name, meaning)  -- all u32 little endian unless noted
HEADER_FIELDS = [
    (0x00, 'magic',        "'L96\\0'"),
    (0x04, 'name',         '8 bytes, file name without extension (char[8])'),
    (0x0c, 'version',      'must be 5'),
    (0x10, 'unk10',        'unknown, not read by DTPLAYER'),
    (0x14, 'unk14',        'unknown, not read by DTPLAYER'),
    (0x18, 'n_names',      'count of 8-byte linked file names'),
    (0x1c, 'n_sounds',     'count of sound effect records'),
    (0x20, 'n_sprites',    'count of sprite/animation records'),
    (0x24, 'tbl28_flag',   'if 0 (and n_tbl54 != 0) the 0x28 records get adjusted at load'),
    (0x28, 'unk28',        'unknown, not read'),
    (0x2c, 'n_segments',   'count of 100-byte segment records'),
    (0x30, 'n_tbl44',      'count of 0x44-byte records'),
    (0x34, 'n_tbl54',      'count of 0x54-byte records'),
    (0x38, 'n_tbl28',      'count of 0x28-byte records'),
    (0x3c, 'n_index',      'count of 8-byte stream index entries'),
    (0x40, 'off_names',    'offset of linked file names'),
    (0x44, 'off_lut555',   'offset of 0x8000-byte RGB555 -> palette index table'),
    (0x48, 'off_palette',  'offset of 256 x RGB palette'),
    (0x4c, 'off_sounds',   'offset of sound effect records'),
    (0x50, 'off_sprites',  'offset of sprite/animation records'),
    (0x54, 'unk54',        'unknown, 0 in all files, not read'),
    (0x58, 'unk58',        'unknown, 0 in all files, not read'),
    (0x5c, 'off_segments', 'offset of segment records'),
    (0x60, 'off_tbl44',    'offset of 0x44-byte records'),
    (0x64, 'off_tbl54',    'offset of 0x54-byte records'),
    (0x68, 'off_tbl28',    'offset of 0x28-byte records'),
    (0x6c, 'off_index',    'offset of stream index'),
    (0x70, 'fourcc1',      "'cvid' video codec FourCC (ICOpen)"),
    (0x74, 'fourcc2',      "'cvid'"),
    (0x78, 'off_lut565',   'offset of 0x10000-byte RGB565 -> palette index table'),
    (0x7c, 'n_shade',      'u8 count of 0x20000-byte tables; u8 @0x7d count of 0x2000-byte tables (u16 @0x7e unused)'),
    (0x80, 'off_shade',    'offset of 0x20000-byte tables (555 set, then 565 set)'),
    (0x84, 'off_shade2',   'offset of 0x2000-byte tables (555 set, then 565 set)'),
    (0x88, 'special_count','initial value of a per-level decrementing counter (special weapon?)'),
    (0x8c, 'unk8c',        'unknown, 0, not read'),
    (0x90, 'unk90',        'unknown, 0, not read'),
]
HEADER_SIZE = 0x94


class Group:
    """One index entry = one stream group (5 video frames + 1/3 s of audio, normally)."""
    __slots__ = ('seg', 'k', 'entry', 'offset', 'virtual', 'raw', 'b0', 'b1', 'b2', 'b3')

    def __init__(self, seg, k, entry, offset, raw, virtual):
        self.seg, self.k, self.entry, self.offset, self.raw, self.virtual = seg, k, entry, offset, raw, virtual
        self.b0, self.b1, self.b2, self.b3 = entry[0], entry[1], entry[2], entry[3]

    # layout inside a group: [audio b3][video b0][fg b1][carry b2]  (sectors)
    def _part(self, start, n):
        return self.raw[start * SECTOR:(start + n) * SECTOR]

    @property
    def audio(self): return self._part(0, self.b3)
    @property
    def video(self): return self._part(self.b3, self.b0)
    @property
    def fg(self): return self._part(self.b3 + self.b0, self.b1)
    @property
    def carry(self): return self._part(self.b3 + self.b0 + self.b1, self.b2)

    @property
    def frames(self):
        return list(parse_frames(self.video)[0])


FRAMES_PER_GROUP = 5   # 15 fps, 1/3 s of audio per group; bytes after the 5th frame are mastering junk


def parse_frames(v):
    """Split a video part into (hdr28, cinepak) records. Returns (frames, end_offset, error)."""
    out = []; o = 0
    while o + 0x44 <= len(v) and len(out) < FRAMES_PER_GROUP:
        sz = struct.unpack_from('<I', v, o)[0]
        if sz == 0:
            break
        f = o + 0x44
        if f + 10 > len(v) or f + sz > len(v):
            return out, o, 'frame %d at +0x%x: size 0x%x overruns video part' % (len(out), o, sz)
        if int.from_bytes(v[f + 1:f + 4], 'big') != sz or v[f] not in (0, 1):
            return out, o, 'frame %d at +0x%x: cinepak header does not match size 0x%x' % (len(out), o, sz)
        out.append((bytes(v[o + 4:o + 0x20]), bytes(v[f:f + sz])))
        o = f + sz
    return out, o, None


class L96:
    def __init__(self, path):
        self.path = path
        self._f = open(path, 'rb')
        self.d = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.d
        if d[:4] != b'L96\0':
            raise ValueError('not an L96 file')
        self.header = {}
        for off, name, _ in HEADER_FIELDS:
            if name == 'magic': continue
            if name == 'name':
                self.header[name] = d[4:12].split(b'\0')[0].decode('latin-1'); continue
            if name in ('fourcc1', 'fourcc2'):
                self.header[name] = d[off:off + 4].decode('latin-1'); continue
            self.header[name] = struct.unpack_from('<I', d, off)[0]
        h = self.header
        if h['version'] != 5:
            raise ValueError('version %d != 5' % h['version'])
        h['n_shade1'] = d[0x7c]; h['n_shade2'] = d[0x7d]
        self.names = [d[h['off_names'] + 8 * i:h['off_names'] + 8 * i + 8].split(b'\0')[0].decode('latin-1')
                      for i in range(h['n_names'])]
        p = h['off_palette']
        self.palette = [tuple(d[p + 3 * i:p + 3 * i + 3]) for i in range(256)]
        self.index = [bytes(d[h['off_index'] + 8 * i:h['off_index'] + 8 * i + 8]) for i in range(h['n_index'])]
        self.segments = []
        for s in range(h['n_segments']):
            r = bytes(d[h['off_segments'] + SEG_REC * s:h['off_segments'] + SEG_REC * (s + 1)])
            typ, flag, off = struct.unpack_from('<iiI', r, 0)
            first = struct.unpack_from('<h', r, 0x0c)[0]
            nxt = struct.unpack_from('<b', r, 0x0e)[0]
            self.segments.append(dict(type=typ, flag=flag, offset=off, first=first, next=nxt,
                                      count=r[0x0f], b55=r[0x55], raw=r))
        self.stream_start = (h['off_index'] + 8 * h['n_index'] + SECTOR - 1) & ~(SECTOR - 1)
        self._layout()

    # ---- header sections -------------------------------------------------
    def sounds(self):
        """Sound effects: 0x2c-byte record header (u32 @+8 = data size) + u8 mono 22050 Hz PCM."""
        o = self.header['off_sounds']; out = []
        for _ in range(self.header['n_sounds']):
            hdr = bytes(self.d[o:o + 0x2c]); size = struct.unpack_from('<I', hdr, 8)[0]
            out.append((o, hdr, size)); o += 0x2c + size
        return out, o

    def sprites(self):
        """Sprite/animation sets: 16-byte header (tag[4], u32 count, u32 size, i16 type, i16 unk) + size bytes."""
        o = self.header['off_sprites']; out = []
        for _ in range(self.header['n_sprites']):
            tag = self.d[o:o + 4].rstrip(b'\0 ').decode('latin-1')
            count, size, typ, unk = struct.unpack_from('<IIhh', self.d, o + 4)
            out.append(dict(offset=o, tag=tag, count=count, size=size, type=typ, unk=unk)); o += 16 + size
        return out, o

    def sections(self):
        """(name, offset, size, expected_next_offset_field) in file order."""
        h = self.header
        snd_end = self.sounds()[1]; spr_end = self.sprites()[1]
        return [
            ('header', 0, HEADER_SIZE),
            ('names', h['off_names'], 8 * h['n_names']),
            ('lut555', h['off_lut555'], 0x8000),
            ('lut565', h['off_lut565'], 0x10000),
            ('palette', h['off_palette'], 0x300),
            ('sounds', h['off_sounds'], snd_end - h['off_sounds']),
            ('sprites', h['off_sprites'], spr_end - h['off_sprites']),
            ('shade', h['off_shade'], 2 * 0x20000 * h['n_shade1']),
            ('shade2', h['off_shade2'], 2 * 0x2000 * h['n_shade2']),
            ('segments', h['off_segments'], SEG_REC * h['n_segments']),
            ('tbl44', h['off_tbl44'], 0x44 * h['n_tbl44']),
            ('tbl54', h['off_tbl54'], 0x54 * h['n_tbl54']),
            ('tbl28', h['off_tbl28'], 0x28 * h['n_tbl28']),
            ('index', h['off_index'], 8 * h['n_index']),
        ]

    # ---- stream layout ---------------------------------------------------
    def entries(self, s):
        sg = self.segments[s]
        return [self.index[sg['first'] + k] for k in range(sg['count'])]

    def _layout(self):
        """Compute the physical offset of every stored group; mark virtual groups.

        flag 2: entries stored back to back from segment offset.
        flag 1, type 6: offset points at entry 3; entries 0,1 immediately precede it;
                entry 2 and the last entry are virtual (assembled from carry sectors).
        flag 1, type 0/1: entries stored from offset like flag 2 (entry 0 is also duplicated
                in another segment's carry sectors, so the engine can branch without seeking).
        """
        self.where = {}      # (seg, k) -> offset (stored) or None (virtual)
        for s, sg in enumerate(self.segments):
            es = self.entries(s); full = [sum(e[:4]) * SECTOR for e in es]
            n = len(es)
            if sg['type'] in (6, 2) and sg['flag'] == 1:
                skip = TYPE6_SKIP if sg['type'] == 6 else TYPE2_SKIP
                o = sg['offset'] - sum(full[:skip - 1])
                for k in range(n):
                    if k == skip - 1 or k == n - 1:
                        self.where[(s, k)] = None; continue
                    self.where[(s, k)] = o; o += full[k]
            else:
                o = sg['offset']
                for k in range(n):
                    self.where[(s, k)] = o; o += full[k]

    def _virtual_donors(self, s, k):
        es = self.entries(s); n = len(es)
        skip = TYPE6_SKIP if self.segments[s]['type'] == 6 else TYPE2_SKIP
        if k == skip - 1:
            return list(range(0, skip - 1))
        # last entry: carried by the stored entries with b2 > 0 after the head
        return [j for j in range(skip, n - 1) if es[j][2]]

    def group(self, s, k):
        e = self.entries(s)[k]; o = self.where[(s, k)]
        if o is not None:
            return Group(s, k, e, o, self.d[o:o + sum(e[:4]) * SECTOR], False)
        raw = b''.join(self.group(s, j).carry for j in self._virtual_donors(s, k))
        return Group(s, k, e, None, raw, True)

    def groups(self):
        for s, sg in enumerate(self.segments):
            for k in range(sg['count']):
                yield self.group(s, k)

    def carry_stream(self, s):
        """All carry (b2) sectors of a segment's stored groups, in entry order."""
        return b''.join(self.group(s, k).carry for k in range(self.segments[s]['count'])
                        if self.where[(s, k)] is not None)


# ---- CLI ------------------------------------------------------------------
def cmd_info(path):
    f = L96(path); h = f.header
    print('%s  (%d bytes)' % (path, len(f.d)))
    for off, name, meaning in HEADER_FIELDS:
        if name == 'magic': continue
        v = h[name]
        vs = ('0x%x' % v) if isinstance(v, int) else repr(v)
        print('  0x%02x %-13s %-12s %s' % (off, name, vs, meaning))
    print('  linked names:', ' '.join(f.names) or '-')
    print('  sections:')
    for name, off, size in f.sections():
        print('    %-9s @0x%08x  size 0x%x' % (name, off, size))
    print('  stream @0x%x .. 0x%x' % (f.stream_start, len(f.d)))
    spr = f.sprites()[0]
    if spr:
        print('  sprite sets:', ', '.join('%s(%d,t%d)' % (x['tag'], x['count'], x['type']) for x in spr))
    print('  segments:')
    for s, sg in enumerate(f.segments):
        es = f.entries(s)
        nvirt = sum(1 for k in range(sg['count']) if f.where[(s, k)] is None)
        print('    %2d type %d flag %d off 0x%08x next %3d entries %3d (virtual %d)  sectors v/fg/carry/a %d/%d/%d/%d'
              % (s, sg['type'], sg['flag'], sg['offset'], sg['next'], sg['count'], nvirt,
                 sum(e[0] for e in es), sum(e[1] for e in es), sum(e[2] for e in es), sum(e[3] for e in es)))
    st = walk(f)
    print('  totals: groups %d (virtual %d), video frames %d, audio blocks %d (%.1f min), FG blocks %d, carry blocks %d'
          % (st['groups'], st['virtual'], st['frames'], st['audio'], st['audio'] / 3 / 60, st['fg'], st['carry']))


def walk(f, problems=None, warnings=None):
    P = problems if problems is not None else []
    W = warnings if warnings is not None else []
    st = collections.Counter()
    fpg = collections.Counter()
    for g in f.groups():
        tag = 'seg %d entry %d%s' % (g.seg, g.k, ' (virtual)' if g.virtual else '')
        st['groups'] += 1; st['virtual'] += g.virtual
        if len(g.raw) != sum(g.entry[:4]) * SECTOR:
            P.append('%s: group data truncated (%d bytes)' % (tag, len(g.raw))); continue
        if g.entry[4:] != b'\0\0\0\0':
            P.append('%s: index bytes 4..7 not zero: %s' % (tag, g.entry.hex()))
        if g.b3:
            a = g.audio; st['audio'] += 1
            if len(a) < AUDIO_BYTES or a[AUDIO_BYTES:].strip(b'\0'):
                P.append('%s: audio part not %d bytes + zero pad' % (tag, AUDIO_BYTES))
            elif not a[:AUDIO_BYTES].strip(b'\0'):
                W.append('%s: audio all zero' % tag)
        if g.b0:
            v = g.video
            fr, end, err = parse_frames(v)
            if err: P.append('%s: %s' % (tag, err))
            elif (len(v) - end) >= SECTOR:
                st['spare_video_sectors'] += (len(v) - end) // SECTOR   # benign: b0 rounded up
            for _, c in fr:
                if struct.unpack_from('>HH', c, 4) != (320, 200):
                    P.append('%s: frame is %dx%d, not 320x200' % ((tag,) + struct.unpack_from('>HH', c, 4)))
                    break
            st['frames'] += len(fr); fpg[len(fr)] += 1
        if g.b1:
            st['fg'] += 1
            if g.fg[:4] != b'FG \0':
                P.append("%s: FG part does not start with 'FG '" % tag)
        if g.b2:
            st['carry'] += 1
    st['frames_per_group'] = dict(fpg)
    return st


def cmd_check(path):
    f = L96(path); h = f.header; P = []; W = []
    d = f.d
    # 1. header sections must follow each other without gaps
    secs = f.sections()
    o = 0
    for name, off, size in secs:
        if size == 0 and off != o:
            pass   # empty section: offset value not checked strictly
        elif off != o:
            P.append('section %s at 0x%x, expected 0x%x' % (name, off, o))
        o = max(o, off + size)
    if f.stream_start != ((o + SECTOR - 1) & ~(SECTOR - 1)):
        P.append('stream start 0x%x does not follow index end 0x%x' % (f.stream_start, o))
    if d[o:f.stream_start].strip(b'\0'):
        P.append('non-zero padding between index and stream')
    if h['fourcc1'] != 'cvid':
        P.append('fourcc %r' % h['fourcc1'])
    for x in f.sprites()[0]:
        if x['count'] * 0x58 > x['size']:
            P.append('sprite set %s: %d descriptors do not fit in %d bytes' % (x['tag'], x['count'], x['size']))
    # 2. index coverage
    used = sorted((sg['first'], sg['first'] + sg['count']) for sg in f.segments)
    cur = 0
    for a, b in used:
        if a != cur: P.append('index entries %d..%d not used by any segment' % (cur, a - 1) if a > cur
                              else 'index entries overlap at %d' % a)
        cur = max(cur, b)
    if cur != h['n_index']:
        P.append('index entries %d..%d not used' % (cur, h['n_index'] - 1))
    # 3. stored groups tile the stream exactly
    ext = sorted((o, o + sum(f.entries(s)[k][:4]) * SECTOR, s, k) for (s, k), o in f.where.items() if o is not None)
    cur = f.stream_start
    for a, b, s, k in ext:
        if a > cur:
            # not a parse error: data the index never points at (seen once, LEVEL02 cd1)
            W.append('unreferenced %d sectors at 0x%x..0x%x (before seg %d entry %d)%s'
                     % ((a - cur) // SECTOR, cur, a, s, k, '' if d[cur:a].strip(b'\0') else ' [all zero]'))
        elif a < cur:
            P.append('seg %d entry %d at 0x%x overlaps previous group (ends 0x%x)' % (s, k, a, cur))
        cur = max(cur, b)
    if cur != len(d):
        P.append('stream ends at 0x%x, file size 0x%x' % (cur, len(d)))
    # 4. virtual groups: donors' carry adds up to the group size
    for (s, k), o in f.where.items():
        if o is None:
            need = sum(f.entries(s)[k][:4]); got = sum(f.entries(s)[j][2] for j in f._virtual_donors(s, k))
            if need != got:
                P.append('seg %d entry %d virtual: needs %d sectors, donors carry %d' % (s, k, need, got))
    # 5. carry sectors: flag-1 type 0/1 segments have entry 0 duplicated in some carry stream
    carries = {s: f.carry_stream(s) for s in range(len(f.segments))}
    for s, sg in enumerate(f.segments):
        if sg['flag'] == 1 and sg['type'] in (0, 1):
            g0 = f.group(s, 0)
            if any(g0.raw in c for c in carries.values()):
                continue
            # copies may differ only in the per-frame header (u32 @+0x20 of the 0x44 header)
            ok = False
            for t, c in carries.items():
                i = c.find(g0.audio)
                if i < 0 or i % SECTOR: continue
                cp = Group(s, 0, g0.entry, None, c[i:i + len(g0.raw)], True)
                if len(cp.raw) == len(g0.raw) and cp.fg == g0.fg and \
                        [x[1] for x in cp.frames] == [x[1] for x in g0.frames]:
                    ok = True
                    W.append('seg %d entry 0: copy in seg %d carry differs only in frame headers' % (s, t))
                    break
            if not ok:
                P.append('seg %d (flag 1): entry 0 not found in any carry stream' % s)
    # 6. every group's contents
    st = walk(f, P, W)
    print('%s: segments %d, groups %d (virtual %d), video frames %d, audio blocks %d, FG blocks %d, carry blocks %d'
          % (path, len(f.segments), st['groups'], st['virtual'], st['frames'], st['audio'], st['fg'], st['carry']))
    print('  frames per group:', dict(sorted(st['frames_per_group'].items())),
          ' spare video sectors:', st['spare_video_sectors'])
    for w in W[:20]: print('  warning:', w)
    if len(W) > 20: print('  ... %d more warnings' % (len(W) - 20))
    for p in P[:50]: print('  PROBLEM:', p)
    print('  %d inconsistencies, %d warnings' % (len(P), len(W)))
    return 1 if P else 0


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('info', 'check'):
        print(__doc__); sys.exit(2)
    sys.exit(cmd_info(sys.argv[2]) if sys.argv[1] == 'info' else cmd_check(sys.argv[2]))
