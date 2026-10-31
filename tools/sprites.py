"""Deadly Tide (1996) per-frame gameplay data: frame headers, FG object blocks, sprite RLE.

Format description: docs/GAMEPLAY_DATA.md. Container access: tools/l96.py (not modified).

    python3 tools/sprites.py hdr     FILE SEG [STEP]          per-frame 0x44 header fields of a segment
    python3 tools/sprites.py fg      FILE SEG GROUP           FG object blocks (one per video frame) of a group
    python3 tools/sprites.py sets    FILE                     sprite sets in the header section
    python3 tools/sprites.py setpng  FILE SET OUTDIR [FRAMES] decode a sprite set's frames to PNG (level palette)
    python3 tools/sprites.py fgpng   FILE SEG GROUP OUTDIR    decode the streamed sprites of a group to PNG
    python3 tools/sprites.py overlay FILE SEG GROUP FRAME OUT.png [--scale N]
                                  decoded video frame + streamed sprites composited where the records say,
                                  green = hit bbox (+0x18), yellow = draw rect, red cross = projected 3D pos

Library use:
    frame_header(hdr68)            -> dict(size, cam, look, roll, flags, rest)
    fg_blocks(fg_bytes)            -> list of blocks: dict(offset, count, size, records=[obj_record...])
    obj_record(rec88)              -> dict of the 0x58-byte object / sprite-frame record
    rle_decode(data, w, h)         -> (indices bytearray w*h, mask bytearray w*h, bytes used)
    sprite_sets(l96)               -> list of sets with frame descriptors and pixel bytes
    project(cam, look, roll, fov_half_deg, pt, W=320, H=200) -> (sx, sy, depth) or None
"""
import math, os, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l96 import L96, parse_frames   # noqa: E402

FRAME_HDR = 0x44
OBJ_REC = 0x58
FG_MAGIC = b'FG \0'

# ---- 0x44-byte per-video-frame header ---------------------------------------------------------
FLAG_AIM = 1        # dword 8 bit 0: player may aim, enemies update, HUD drawn (type-1 scenes)
FLAG_FIRE = 2       # dword 8 bit 1: player weapon may fire


def frame_header(h):
    """Decode a 0x44-byte frame header (bytes starting at the header, i.e. including dword 0)."""
    v = struct.unpack_from('<17i', h, 0)
    return dict(size=v[0], cam=v[1:4], look=v[4:7], roll=v[7], flags=v[8] & 0xffffffff, rest=v[9:17])


def segment_frames(f, s, groups=None):
    """Yield (group k, frame i, header68 bytes, cinepak bytes) for a segment (video groups only)."""
    for k in (range(f.segments[s]['count']) if groups is None else groups):
        g = f.group(s, k)
        v = g.video; o = 0
        for i in range(5):
            if o + FRAME_HDR > len(v): break
            sz = struct.unpack_from('<I', v, o)[0]
            if sz == 0: break
            yield k, i, bytes(v[o:o + FRAME_HDR]), bytes(v[o + FRAME_HDR:o + FRAME_HDR + sz])
            o += FRAME_HDR + sz


# ---- FG object blocks -------------------------------------------------------------------------
# kind byte (+2): bit mask tested by the draw passes (Obj_DrawLayer 0x412e40 masks 3 then 4;
# Obj_DrawStatic 0x40aaa0 kind == 8).
KINDS = {1: 'rail-enemy', 2: 'enemy', 4: 'effect', 8: 'overlay'}


def obj_record(r):
    """0x58-byte object record (stream FG block) / frame descriptor (sprite set)."""
    (actor, kind, b3, off, size, flags, x, y, w, h, bx0, by0, bx1, by1, px, py, pz) = \
        struct.unpack_from('<hBBiiIhhhhhhhhiii', r, 0)
    return dict(actor=actor, kind=kind, b3=b3, data_off=off, data_size=size, flags=flags,
                x=x, y=y, w=w, h=h, bbox=(bx0, by0, bx1, by1), pos=(px, py, pz),
                targetable=bool(flags & 0x80000000),
                fire_sure=bool(flags & 0x80), fire_rand=bool(flags & 0x40000000),
                remove=bool(flags & 0x40), respawn=bool(flags & 0x100),
                muzzle=((flags >> 18) & 0xfff, (flags >> 9) & 0x1ff),
                cube_faces=struct.unpack_from('<i', r, 0x2c)[0],
                cube_rects=struct.unpack_from('<20h', r, 0x30),
                tint=r[0x48], raw=bytes(r))


def fg_blocks(fg):
    """Split a group's FG part into its per-frame object blocks.

    Block: 'FG \\0', i32 count, i32 body size (= count*0x58 + sum of record data sizes),
    count x 0x58 records, pixel data; next block at (0xc + body + 3) & ~3 (Obj_BlockSize 0x4038d0).
    Record data offsets (+4) are relative to the block start.
    """
    out = []; o = 0
    while o + 12 <= len(fg) and fg[o:o + 4] == FG_MAGIC:
        n, body = struct.unpack_from('<ii', fg, o + 4)
        recs = []
        for i in range(n):
            rec = obj_record(fg[o + 0xc + OBJ_REC * i:o + 0xc + OBJ_REC * (i + 1)])
            rec['pixels'] = bytes(fg[o + rec['data_off']:o + rec['data_off'] + rec['data_size']])
            recs.append(rec)
        out.append(dict(offset=o, count=n, size=body, records=recs))
        o += (0xc + body + 3) & ~3
    return out


# ---- sprite RLE (Spr_BlitRLE8 0x410ae0; same codec for set frames and streamed sprites) --------
def rle_decode(src, w, h):
    """Decode one RLE sprite. Rows top-down. Opcodes (signed byte c):
       c < 0: -c literal palette indices follow;  c > 0: skip c transparent pixels;
       c == 0: u16 big-endian n follows: go down n rows, back to x = 0; stop when row == h."""
    pix = bytearray(w * h); mask = bytearray(w * h)
    i = 0; x = 0; y = 0; n_src = len(src)
    while i < n_src:
        c = src[i]
        if c >= 0x80:
            n = 256 - c
            if 0 <= y < h:
                a = max(x, 0); b = min(x + n, w)
                if b > a:
                    pix[y * w + a:y * w + b] = src[i + 1 + a - x:i + 1 + b - x]
                    mask[y * w + a:y * w + b] = b'\1' * (b - a)
            x += n; i += 1 + n
        elif c:
            x += c; i += 1
        else:
            y += (src[i + 1] << 8) | src[i + 2]; x = 0; i += 3
            if y >= h: break
    return pix, mask, i


def sprite_sets(f):
    """Sprite / animation sets in the header section (Sprites_Load 0x4022b0)."""
    sets = []
    for x in f.sprites()[0]:
        p = x['offset'] + 16
        frames = []
        for k in range(x['count']):
            rec = obj_record(f.d[p + OBJ_REC * k:p + OBJ_REC * (k + 1)])
            rec['pixels'] = bytes(f.d[p + rec['data_off']:p + rec['data_off'] + rec['data_size']])
            frames.append(rec)
        sets.append(dict(x, frames=frames, list=abs(x['type']), preexpand=x['type'] < 0))
    return sets


# ---- camera -----------------------------------------------------------------------------------
def fov_half_deg(seg_raw):
    """Scene short @0x4a: half field of view in 1/512 degree (Cam_UpdateFromHeader: tan(v*pi/92160))."""
    return struct.unpack_from('<h', seg_raw, 0x4a)[0] / 512.0


def project(cam, look, roll, fov_half, pt, W=320, H=200):
    """World point -> top-down screen (sx, sy, depth); Mat_LookAt 0x40ab40 / Proj_WorldToScreen 0x40b370.
    View from cam towards look, roll in degrees, scale (W/2)/tan(fov_half). Axis signs were fitted to the
    data (record 3D positions vs. bboxes, median error 2-3 px): world +Y appears as screen-down."""
    fx, fy, fz = (look[0] - cam[0], look[1] - cam[1], look[2] - cam[2])
    fl = math.sqrt(fx * fx + fy * fy + fz * fz) or 1.0
    fx, fy, fz = fx / fl, fy / fl, fz / fl
    # right = up x forward with up = +Y (left-handed, +Z forward like the engine's yaw-from-x/z)
    rx, ry, rz = fz, 0.0, -fx
    rl = math.sqrt(rx * rx + rz * rz) or 1.0
    rx, rz = rx / rl, rz / rl
    ux, uy, uz = (fy * rz - fz * ry, fz * rx - fx * rz, fx * ry - fy * rx)   # forward x right
    ux, uy, uz = -ux, -uy, -uz
    dx, dy, dz = pt[0] - cam[0], pt[1] - cam[1], pt[2] - cam[2]
    X = dx * rx + dy * ry + dz * rz
    Y = dx * ux + dy * uy + dz * uz
    Z = dx * fx + dy * fy + dz * fz
    a = math.radians(roll)
    X, Y = X * math.cos(a) - Y * math.sin(a), X * math.sin(a) + Y * math.cos(a)
    if Z < 1.0:
        return None
    k = (W / 2.0) / math.tan(math.radians(fov_half))
    # world +Y maps to screen-down: result is a top-down screen y
    return W / 2.0 + X * k / Z, H / 2.0 + Y * k / Z, Z


# ---- video decode via ffmpeg ------------------------------------------------------------------
def _is_key(c):
    """Cinepak frame is intra-only if every strip id is 0x10 (strip chunk at frame+10)."""
    n = struct.unpack_from('>H', c, 8)[0]; o = 10
    for _ in range(n):
        if o + 4 > len(c) or c[o] != 0x10: return False
        o += struct.unpack_from('>H', c, o + 2)[0]
    return n > 0


def decode_frame(f, s, k, i):
    """RGB bytes (320*200*3) of frame i of group k in segment s, decoding from the last key frame."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from l96_cvid import write_avi
    frames = [(kk, ii, c) for kk, ii, _, c in segment_frames(f, s, range(k + 1))]
    t = [j for j, (kk, ii, _) in enumerate(frames) if (kk, ii) == (k, i)][0]
    start = max([j for j in range(t + 1) if _is_key(frames[j][2])] or [0])
    with tempfile.TemporaryDirectory() as td:
        avi = os.path.join(td, 'x.avi')
        write_avi(avi, [c for _, _, c in frames[start:t + 1]], 320, 200)
        raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', avi, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                             check=True, capture_output=True).stdout
    return raw[-320 * 200 * 3:]


# ---- PNG helpers ------------------------------------------------------------------------------
def sprite_image(rec, palette, preexpanded=False):
    from PIL import Image
    w, h = rec['w'], rec['h']
    if w <= 0 or h <= 0: return None
    if preexpanded:
        pix = rec['pixels'][:w * h]; mask = bytes(1 if p else 0 for p in pix)
    else:
        pix, mask, _ = rle_decode(rec['pixels'], w, h)
    im = Image.frombytes('P', (w, h), bytes(pix))
    im.putpalette([c for rgb in palette for c in rgb])
    im = im.convert('RGBA')
    im.putalpha(Image.frombytes('L', (w, h), bytes(255 if m else 0 for m in mask)))
    # rows are stored bottom row first (Spr_Draw callers pass base = last buffer row, stride = -W)
    return im.transpose(Image.FLIP_TOP_BOTTOM)


H = 200


def screen_bbox(r, h=H):
    """Record bbox (+0x18, y counted from the bottom row) -> top-down screen bbox (as Obj_ProjectAll 0x403780)."""
    x0, y0, x1, y1 = r['bbox']
    return x0, h - 1 - y1, x1, h - 1 - y0


def overlay(f, s, k, i, out, scale=3):
    from PIL import Image, ImageDraw
    rgb = decode_frame(f, s, k, i)
    bg = Image.frombytes('RGB', (320, 200), rgb).convert('RGBA')
    hdr = frame_header([h for kk, ii, h, _ in segment_frames(f, s, [k]) if ii == i][0])
    blocks = fg_blocks(f.group(s, k).fg)
    blk = blocks[i] if i < len(blocks) else dict(records=[])
    recs = blk['records']
    # draw order: Obj_DrawLayer mask 3 (kinds 1,2), mask 4 (kind 4), then Obj_DrawStatic kind 8
    for mask in (3, 4, 8):
        for r in recs:
            if (r['kind'] & mask) if mask != 8 else r['kind'] == 8:
                im = sprite_image(r, f.palette)
                if im:
                    bg.paste(im, (r['x'], H - r['y'] - r['h']), im)   # record y counts from the bottom
    big = bg.resize((320 * scale, 200 * scale), Image.NEAREST)
    dr = ImageDraw.Draw(big)
    fov = fov_half_deg(f.segments[s]['raw'])
    for r in recs:
        S = scale
        ty = H - r['y'] - r['h']
        dr.rectangle([r['x'] * S, ty * S, (r['x'] + r['w']) * S - 1, (ty + r['h']) * S - 1],
                     outline=(255, 255, 0, 255))
        x0, y0, x1, y1 = screen_bbox(r)
        if r['kind'] != 8:
            dr.rectangle([x0 * S, y0 * S, (x1 + 1) * S - 1, (y1 + 1) * S - 1], outline=(0, 255, 0, 255))
            p = project(hdr['cam'], hdr['look'], hdr['roll'], fov, r['pos'])
            if p:
                px, py = p[0] * S, p[1] * S
                dr.line([px - 6, py, px + 6, py], fill=(255, 0, 0, 255), width=2)
                dr.line([px, py - 6, px, py + 6], fill=(255, 0, 0, 255), width=2)
        dr.text((x0 * S + 2, y0 * S - 11), '%d%s' % (r['actor'], '' if r['kind'] == 2 else '/k%d' % r['kind']),
                fill=(255, 255, 255, 255))
    dr.text((4, 4), 'seg %d grp %d frm %d  cam %s look %s roll %d flags %d  objs %d' %
            (s, k, i, hdr['cam'], hdr['look'], hdr['roll'], hdr['flags'], len(recs)), fill=(255, 255, 255, 255))
    big.convert('RGB').save(out)
    return hdr, recs


# ---- CLI --------------------------------------------------------------------------------------
def cmd_hdr(f, s, step=1):
    print('seg %d type %d  fov_half %.2f deg' % (s, f.segments[s]['type'], fov_half_deg(f.segments[s]['raw'])))
    print('grp frm  cam(x,y,z)            look(x,y,z)           roll flags  dist  rest(dw9..16)')
    n = 0
    for k, i, h, _ in segment_frames(f, s):
        if n % step == 0:
            d = frame_header(h)
            dist = math.dist(d['cam'], d['look'])
            print('%3d %d  %-20s  %-20s %4d  %#4x  %5.1f  %s' % (k, i, d['cam'], d['look'], d['roll'], d['flags'],
                  dist, '' if not any(d['rest']) else d['rest']))
        n += 1


def cmd_fg(f, s, k):
    for bi, b in enumerate(fg_blocks(f.group(s, k).fg)):
        print('frame %d @+0x%x  count %d  body 0x%x' % (bi, b['offset'], b['count'], b['size']))
        for r in b['records']:
            print('   actor %3d kind %d flags %08x  draw (%d,%d %dx%d)  bbox %s  pos %s  tint %d  data %d@+0x%x%s%s'
                  % (r['actor'], r['kind'], r['flags'], r['x'], r['y'], r['w'], r['h'], r['bbox'], r['pos'],
                     r['tint'], r['data_size'], r['data_off'],
                     '  FIRES(%s) muzzle %s' % ('sure' if r['fire_sure'] else 'rand', r['muzzle'])
                     if (r['fire_sure'] or r['fire_rand']) else '',
                     '  cube faces %#x rects %s' % (r['cube_faces'], r['cube_rects']) if r['cube_faces'] else ''))


def cmd_sets(f):
    for si, x in enumerate(sprite_sets(f)):
        print('set %2d %-3s type %2d count %2d size %6d size_hint %3d%s' %
              (si, x['tag'], x['type'], x['count'], x['size'], x['unk'], '  (pre-expanded at load)' if x['preexpand'] else ''))
        for k, r in enumerate(x['frames']):
            print('    %2d %3dx%-3d at (%d,%d) bbox %s tint %#x data %d' % (k, r['w'], r['h'], r['x'], r['y'],
                  r['bbox'], r['tint'], r['data_size']))


def cmd_setpng(f, si, outdir, frames=None):
    os.makedirs(outdir, exist_ok=True)
    x = sprite_sets(f)[si]
    for k, r in enumerate(x['frames']):
        if frames and k not in frames: continue
        im = sprite_image(r, f.palette)
        if im is None: continue
        bb = im.getbbox()
        if bb: im = im.crop(bb)
        p = os.path.join(outdir, '%s_set%02d_t%d_f%02d.png' % (f.header['name'], si, x['type'], k))
        im.save(p); print(p, im.size)


def cmd_fgpng(f, s, k, outdir):
    os.makedirs(outdir, exist_ok=True)
    for bi, b in enumerate(fg_blocks(f.group(s, k).fg)):
        for r in b['records']:
            im = sprite_image(r, f.palette)
            if im is None: continue
            p = os.path.join(outdir, '%s_s%d_g%d_f%d_actor%d.png' % (f.header['name'], s, k, bi, r['actor']))
            im.save(p); print(p, im.size)


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a or a[0] not in ('hdr', 'fg', 'sets', 'setpng', 'fgpng', 'overlay'):
        print(__doc__); sys.exit(2)
    f = L96(a[1])
    if a[0] == 'hdr': cmd_hdr(f, int(a[2]), int(a[3]) if len(a) > 3 else 1)
    elif a[0] == 'fg': cmd_fg(f, int(a[2]), int(a[3]))
    elif a[0] == 'sets': cmd_sets(f)
    elif a[0] == 'setpng': cmd_setpng(f, int(a[2]), a[3], [int(x) for x in a[4].split(',')] if len(a) > 4 else None)
    elif a[0] == 'fgpng': cmd_fgpng(f, int(a[2]), int(a[3]), a[4])
    elif a[0] == 'overlay':
        sc = int(a[a.index('--scale') + 1]) if '--scale' in a else 3
        hdr, recs = overlay(f, int(a[2]), int(a[3]), int(a[4]), a[5], sc)
        print(a[5], 'objects', len(recs))
