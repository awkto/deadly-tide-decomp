"""Prototype of Deadly Tide's type-6 "free-look" renderer (see docs/FREELOOK_SPEC.md).

Usage:
  python3 tools/freelook.py FILE.L96 SEG OUTDIR [yaw,pitch,roll ...] [--fg K [--highest N]] [--tiles]

* Collects the 96 texture tiles (128x128 Cinepak key frames) from the carry sectors of the last
  20 groups of the segment that precedes SEG (as Play_FreeLookEnter 0x407a70 does), decodes them
  with ffmpeg and stacks them into the 128 x 12288 "strip" texture buffer (6 faces x 16 tiles).
* Optionally (--fg K) draws FG records into the strip buffer as FreeLook_Frame 0x412ab0 does with
  Obj_DrawLayer (draw rect = strip coordinates): kind-1 records of all frames of groups 0..K
  (permanent paint), kind-2 records of group K frame 0 (save-under sprites). --highest N applies
  the LEVEL00 hub lock for HighestLevel = N.
* Renders 320x200 views at the given yaw/pitch/roll (engine units: 2496 per turn) by casting a ray
  per pixel against the 6x4x4-quad cube (exactly what the perspective-correct triangle rasteriser
  converges to; the engine's 8-pixel affine sub-spans are not emulated).
* Angles default to a few views. Output: OUTDIR/<stem>_s<SEG>_y<yaw>_p<pitch>.png, plus
  OUTDIR/<stem>_s<SEG>_faces.png (the 6 faces as 512x512 squares) when --tiles is given.
"""
import math, os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l96 import L96                      # noqa: E402
from l96_cvid import write_avi           # noqa: E402

TILE = 128                 # tile edge, texels
TILE_SLOT = 0x1800         # .data 0x4210dc: bytes per tile in the carry stream (3 sectors)
N_GROUPS = 20              # .data 0x4210ec: groups before a type-6 segment that carry tiles
N_TILES = 96               # 19 groups x 5 tiles + 1 tile in the last group = 6 faces x 16
HALF = 2048.0              # cube half size (FreeLook_InitGrid 0x405310)
STEP = 1024.0              # grid step: 4 x 4 quads per face, one tile per quad
UNITS_PER_TURN = 2496      # 0x9c0
W, H = 320, 200


def collect_tiles(f, seg):
    """Return the 96 Cinepak frames (bytes) that preload the texture buffer for type-6 segment seg."""
    prev = seg - 1
    if f.segments[prev]['next'] != seg:
        prev = next(i for i, s in enumerate(f.segments) if s['next'] == seg and s['type'] != 6)
    n = f.segments[prev]['count']
    out = []
    for gi, k in enumerate(range(n - N_GROUPS, n)):
        carry = f.group(prev, k).carry
        ntiles = 1 if gi == N_GROUPS - 1 else 5          # 0x407a70: last group carries 1 tile
        for t in range(ntiles):
            o = t * TILE_SLOT
            size = struct.unpack_from('<I', carry, o)[0]
            cv = bytes(carry[o + 0x44:o + 0x44 + size])
            assert int.from_bytes(cv[1:4], 'big') == size and cv[4:8] == b'\0\x80\0\x80', (k, t)
            out.append(cv)
    assert len(out) == N_TILES
    return out


def decode_tiles(frames, tmp):
    avi = os.path.join(tmp, 'tiles.avi')
    write_avi(avi, frames, TILE, TILE, bpp=24)
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', avi, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         check=True, stdout=subprocess.PIPE).stdout
    a = np.frombuffer(raw, np.uint8).reshape(-1, TILE, TILE, 3)
    assert a.shape[0] == N_TILES, a.shape
    # Engine memory holds each tile bottom-up (memory row 0 = bottom image row), see spec 2.3.
    return a[:, ::-1].reshape(N_TILES * TILE, TILE, 3).copy()   # strip buffer: tile i at rows i*128..


def _blit(strip, f, r):
    from sprites import rle_decode
    pal = np.array(f.palette, np.uint8)
    pix, mask, _ = rle_decode(r['pixels'], r['w'], r['h'])
    pix = np.frombuffer(bytes(pix), np.uint8).reshape(r['h'], r['w'])
    m = np.frombuffer(bytes(mask), np.uint8).reshape(r['h'], r['w']).astype(bool)
    x, y = r['x'], r['y']
    sub = strip[y:y + r['h'], x:x + r['w']]          # draw rect = strip coordinates (128 x 12288)
    hh, ww = sub.shape[:2]
    sub[m[:hh, :ww]] = pal[pix[:hh, :ww][m[:hh, :ww]]]


def draw_fg(strip, f, seg, k, highest=None):
    """Emulate FreeLook_Frame's Obj_DrawLayer passes into the strip buffer up to group k, frame 0.

    kind 1 records are painted without save-under, so they accumulate (every frame of groups 0..k);
    kind 2 records are save-under sprites restored after Present, so only the current frame's
    (group k, frame 0) are drawn. LEVEL00 hub lock: kind-1 records with actor id > 1 and
    > 2*highest-1 are not painted (Obj_DrawLayer 0x412e40)."""
    from sprites import fg_blocks
    hub = os.path.basename(f.path).upper().startswith('LEVEL00')
    n = 0
    for kk in range(k + 1):
        for bi, blk in enumerate(fg_blocks(f.group(seg, kk).fg)):
            for r in blk['records']:
                if not r['pixels']:
                    continue
                if r['kind'] & 1:
                    if hub and highest is not None and r['actor'] > 1 and r['actor'] > 2 * highest - 1:
                        continue
                    _blit(strip, f, r); n += 1
                elif r['kind'] & 2 and kk == k and bi == 0:
                    _blit(strip, f, r); n += 1
    return n


def rot(axis, a):
    """Row-vector rotation matrices as built by 0x4111e0 (Y), 0x411150 (X), 0x411270 (Z)."""
    c, s = math.cos(a), math.sin(a)
    if axis == 'y':
        return np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]])
    if axis == 'x':
        return np.array([[1, 0, 0], [0, c, s], [0, -s, c]])
    return np.array([[c, s, 0], [-s, c, 0], [0, 0, 1]])


def view_matrix(yaw, pitch, roll):
    """Mat_FromEuler 0x40acf0 for type 6: M = Ry(-yaw) . Rx(-pitch) . Rz(-roll), p_view = p_world . M."""
    k = 2 * math.pi / UNITS_PER_TURN
    return rot('y', -yaw * k) @ rot('x', -pitch * k) @ rot('z', -roll * k)


def render(strip, yaw, pitch, roll, half_fov_units, flip_y=False):
    k = (W / 2) / math.tan(half_fov_units * math.pi / 92160.0)   # Proj 0x40b4d0, tan const 0x420020
    px, py = np.meshgrid(np.arange(W) + 0.5, np.arange(H) + 0.5)
    sy = (H / 2 - py) if flip_y else (py - H / 2)
    v = np.stack([(px - W / 2) / k, sy / k, np.ones_like(px)], -1)   # view space, z forward
    d = v @ view_matrix(yaw, pitch, roll).T                             # back to world (M orthonormal)
    x, y, z = d[..., 0], d[..., 1], d[..., 2]
    ax, ay, az = np.abs(x), np.abs(y), np.abs(z)
    face = np.where((az >= ax) & (az >= ay), np.where(z > 0, 0, 2),
                    np.where(ax >= ay, np.where(x < 0, 1, 3), np.where(y < 0, 4, 5)))
    m = np.choose(face, [az, ax, az, ax, ay, ay])
    X, Y, Z = x * HALF / m, y * HALF / m, z * HALF / m
    # (column, row) on the 4x4 grid of each face, from the vertex layout of FreeLook_InitGrid
    c = np.choose(face, [X + HALF, Z + HALF, HALF - X, HALF - Z, X + HALF, X + HALF]) / STEP
    r = np.choose(face, [HALF - Y, HALF - Y, HALF - Y, HALF - Y, HALF - Z, Z + HALF]) / STEP
    c = np.clip(c, 0, 3.99999); r = np.clip(r, 0, 3.99999)
    qc, qr = np.floor(c).astype(int), np.floor(r).astype(int)
    u = np.clip(np.floor((c - qc) * 127 + 0.5), 0, 127).astype(int)   # vertex UVs are 0 / 127.0
    vv = np.clip(np.floor((r - qr) * 127 + 0.5), 0, 127).astype(int)
    tile = face * 16 + qr * 4 + qc
    return strip[tile * TILE + vv, u]


def faces_png(strip):
    img = np.zeros((2 * 512, 3 * 512, 3), np.uint8)
    for fidx in range(6):
        for q in range(16):
            t = strip[(fidx * 16 + q) * TILE:(fidx * 16 + q + 1) * TILE]
            oy, ox = (fidx // 3) * 512 + (q // 4) * TILE, (fidx % 3) * 512 + (q % 4) * TILE
            img[oy:oy + TILE, ox:ox + TILE] = t
    for fidx in range(6):      # memory is bottom-up: flip each face for viewing
        oy, ox = (fidx // 3) * 512, (fidx % 3) * 512
        img[oy:oy + 512, ox:ox + 512] = img[oy:oy + 512, ox:ox + 512][::-1]
    return img


def main(argv):
    flags = [a for a in argv if a.startswith('--')]
    args = [a for a in argv if not a.startswith('--')]
    fg_k = highest = None
    if '--fg' in argv:
        fg_k = int(argv[argv.index('--fg') + 1]); args.remove(str(fg_k))
    if '--highest' in argv:
        highest = int(argv[argv.index('--highest') + 1]); args.remove(str(highest))
    path, seg, outdir = args[0], int(args[1]), args[2]
    views = [tuple(int(t) for t in a.split(',')) for a in args[3:]] or \
        [(0, 0, 0), (624, 0, 0), (1248, 0, 0), (1872, 0, 0), (0, 300, 0), (0, -300, 0)]
    os.makedirs(outdir, exist_ok=True)
    f = L96(path)
    sg = f.segments[seg]
    assert sg['type'] == 6, 'segment %d is type %d' % (seg, sg['type'])
    half_fov = struct.unpack_from('<h', sg['raw'], 0x4a)[0]
    with tempfile.TemporaryDirectory() as tmp:
        strip = decode_tiles(collect_tiles(f, seg), tmp)
    if fg_k is not None:
        print('fg records drawn:', draw_fg(strip, f, seg, fg_k, highest))
    stem = '%s_s%d' % (os.path.splitext(os.path.basename(path))[0], seg)
    if '--tiles' in flags:
        Image.fromarray(faces_png(strip)).save(os.path.join(outdir, stem + '_faces.png'))
    for v in views:
        yaw, pitch = v[0], v[1]
        roll = v[2] if len(v) > 2 else 0
        img = render(strip, yaw, pitch, roll, half_fov, flip_y='--flipy' in flags)
        name = '%s_y%d_p%d%s.png' % (stem, yaw, pitch, '_r%d' % roll if roll else '')
        Image.fromarray(img).save(os.path.join(outdir, name))
        print(name)


if __name__ == '__main__':
    main(sys.argv[1:])
