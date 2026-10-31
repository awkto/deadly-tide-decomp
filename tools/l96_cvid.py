"""Prototype: pull Cinepak frames out of a Deadly Tide .L96 file and wrap them in an AVI for ffmpeg."""
import sys, re, struct

def find_frames(d):
    pat = re.compile(rb'[\x00\x01][\x00-\x03]..\x01\x40\x00[\x80-\xf0]\x00[\x01-\x10][\x10\x11]\x00', re.S)
    out = []
    for m in pat.finditer(d):
        o = m.start(); ln = int.from_bytes(d[o+1:o+4], 'big')
        if out and o < out[-1][0] + out[-1][1]: continue
        out.append((o, ln))
    return out

def write_avi(path, frames, w, h, fps=15, bpp=24, pal=None):
    def chunk(tag, data): return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')
    def lst(tag, data): return chunk(b'LIST', tag + data)
    n = len(frames); mx = max(len(f) for f in frames)
    avih = struct.pack('<14I', 1000000 // fps, 0, 0, 0x10, n, 0, 1, mx, w, h, 0, 0, 0, 0)
    strh = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'vids', b'cvid', 0, 0, 0, 0, 1, fps, 0, n, mx, 0xffffffff, 0, 0, 0, w, h)
    ncol = 256 if pal else 0
    strf = struct.pack('<IiiHHIIiiII', 40, w, h, 1, bpp, int.from_bytes(b'cvid', 'little'), w*h*bpp//8, 0, 0, ncol, 0)
    if pal: strf += b''.join(bytes((b, g, r, 0)) for r, g, b in pal)
    hdrl = lst(b'hdrl', chunk(b'avih', avih) + lst(b'strl', chunk(b'strh', strh) + chunk(b'strf', strf)))
    movi = b''; idx = b''; off = 4
    for f in frames:
        c = chunk(b'00dc', f); idx += struct.pack('<4sIII', b'00dc', 0x10, off, len(f)); off += len(c); movi += c
    body = b'AVI ' + hdrl + lst(b'movi', movi) + chunk(b'idx1', idx)
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
    d = open(src, 'rb').read()
    pal = [tuple(d[0x1809c+3*i:0x1809c+3*i+3]) for i in range(256)]
    fr = find_frames(d)[:limit]
    print(len(fr), 'frames; sizes', [l for _, l in fr[:8]])
    mode = sys.argv[4] if len(sys.argv) > 4 else '24'
    write_avi(dst, [d[o:o+l] for o, l in fr], 320, 200, bpp=8 if mode == '8' else 24, pal=pal if mode == '8' else None)
