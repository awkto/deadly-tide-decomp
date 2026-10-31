#!/usr/bin/env python3
"""Bulk asset dumper for Deadly Tide .L96 files (index-driven, uses tools/l96.py).

    python3 tools/dump_assets.py FILE.L96 OUTDIR [--no-mp4] [--only-seg N]

Writes OUTDIR/<NAME>/{segments,sfx,sprites}, palette.png/.act/.txt, manifest.json.
segNNN.avi  = original Cinepak frames (no re-encode) + u8 stereo 22050 PCM as 2nd stream
segNNN.wav  = same audio (always written; the only output for no-video type-6 segments)
segNNN.mp4  = H.264 crf18, 2x nearest-neighbour, with audio (ffmpeg)
"""
import os, sys, struct, json, zlib, wave, subprocess, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l96 import L96, AUDIO_BYTES

FPS = 15; W, H = 320, 200; RATE = 22050


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def lst(tag, data):
    return chunk(b'LIST', tag + data)


def write_avi_av(path, groups, audio_total):
    """groups: list of (list_of_cinepak_frames, audio_bytes). Interleaved per group. Video 24bpp cvid, audio PCM u8 stereo."""
    nframes = sum(len(fr) for fr, _ in groups)
    mx = max(len(f) for fr, _ in groups for f in fr)
    avih = struct.pack('<14I', 1000000 // FPS, 0, 0, 0x110, nframes, 0, 2, mx, W, H, 0, 0, 0, 0)
    strh_v = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'vids', b'cvid', 0, 0, 0, 0, 1, FPS, 0, nframes, mx, 0xffffffff, 0, 0, 0, W, H)
    strf_v = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, int.from_bytes(b'cvid', 'little'), W * H * 3, 0, 0, 0, 0)
    nsamp = audio_total // 2
    strh_a = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'auds', b'\0\0\0\0', 0, 0, 0, 0, 1, RATE, 0, nsamp, 0, 0xffffffff, 2, 0, 0, 0, 0)
    strf_a = struct.pack('<HHIIHHH', 1, 2, RATE, RATE * 2, 2, 8, 0)
    hdrl = lst(b'hdrl', chunk(b'avih', avih)
               + lst(b'strl', chunk(b'strh', strh_v) + chunk(b'strf', strf_v))
               + lst(b'strl', chunk(b'strh', strh_a) + chunk(b'strf', strf_a)))
    movi = []; idx = []; off = 4
    for fr, au in groups:
        for f in fr:
            c = chunk(b'00dc', f); idx.append(struct.pack('<4sIII', b'00dc', 0x10, off, len(f))); movi.append(c); off += len(c)
        if au:
            c = chunk(b'01wb', au); idx.append(struct.pack('<4sIII', b'01wb', 0x10, off, len(au))); movi.append(c); off += len(c)
    body = b'AVI ' + hdrl + lst(b'movi', b''.join(movi)) + chunk(b'idx1', b''.join(idx))
    with open(path, 'wb') as fh:
        fh.write(b'RIFF' + struct.pack('<I', len(body)) + body)


def write_wav(path, data, ch, rate=RATE):
    with wave.open(path, 'wb') as w:
        w.setnchannels(ch); w.setsampwidth(1); w.setframerate(rate); w.writeframes(data)


def write_png(path, rows):
    h = len(rows); w = len(rows[0])
    raw = b''.join(b'\0' + bytes(c for px in r for c in px) for r in rows)
    def ck(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as fh:
        fh.write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                 + ck(b'IDAT', zlib.compress(raw, 9)) + ck(b'IEND', b''))


def dump(path, outdir, mp4=True, only=None):
    f = L96(path)
    name = os.path.splitext(os.path.basename(path))[0].upper()
    root = os.path.join(outdir, name)
    for d in ('segments', 'sfx', 'sprites'):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    files = []; errors = []
    def rel(p): return os.path.relpath(p, root)

    # palette
    pal = f.palette
    write_png(os.path.join(root, 'palette.png'), [[tuple(pal[y * 16 + x]) for x in range(16) for _ in range(16)]
                                                  for y in range(16) for _ in range(16)])
    open(os.path.join(root, 'palette.act'), 'wb').write(bytes(c for p in pal for c in p))
    open(os.path.join(root, 'palette.txt'), 'w').write(''.join('%3d %3d %3d  #%02x%02x%02x\n' % (r, g, b, r, g, b) for r, g, b in pal))
    files += ['palette.png', 'palette.act', 'palette.txt']

    # sfx
    sounds, _ = f.sounds()
    for i, (o, hdr, size) in enumerate(sounds):
        p = os.path.join(root, 'sfx', '%03d.wav' % i)
        write_wav(p, bytes(f.d[o + 0x2c:o + 0x2c + size]), 1)
        files.append(rel(p))

    # sprites (raw blob = 16-byte set header + payload)
    sprites, _ = f.sprites()
    for i, s in enumerate(sprites):
        p = os.path.join(root, 'sprites', '%03d.bin' % i)
        open(p, 'wb').write(bytes(f.d[s['offset']:s['offset'] + 16 + s['size']]))
        files.append(rel(p))
    sprite_info = [dict(index=i, tag=s['tag'], count=s['count'], size=s['size'], type=s['type']) for i, s in enumerate(sprites)]

    # segments
    segs = []
    for s, sg in enumerate(f.segments):
        if only is not None and s != only: continue
        info = dict(index=s, type=sg['type'], flag=sg['flag'], next=sg['next'], entries=sg['count'])
        try:
            groups = []; nfr = 0; atot = 0; nvirt = 0
            for k in range(sg['count']):
                g = f.group(s, k); nvirt += g.virtual
                fr = [c for _, c in g.frames] if g.b0 else []
                au = bytes(g.audio[:AUDIO_BYTES]) if g.b3 else b''
                nfr += len(fr); atot += len(au); groups.append((fr, au))
            info.update(frames=nfr, virtual_entries=nvirt, audio_bytes=atot,
                        video_seconds=round(nfr / FPS, 3), audio_seconds=round(atot / (RATE * 2), 3))
            base = os.path.join(root, 'segments', 'seg%03d' % s)
            if atot:
                write_wav(base + '.wav', b''.join(a for _, a in groups), 2); files.append(rel(base + '.wav'))
            if nfr and sg['type'] != 6:
                write_avi_av(base + '.avi', groups, atot); files.append(rel(base + '.avi'))
                info['avi'] = rel(base + '.avi')
                if mp4:
                    r = subprocess.run(['ffmpeg', '-v', 'error', '-y', '-threads', '2', '-i', base + '.avi',
                                        '-vf', 'scale=640:400:flags=neighbor,format=yuv420p', '-c:v', 'libx264',
                                        '-crf', '18', '-preset', 'medium', '-c:a', 'aac', '-b:a', '128k',
                                        '-movflags', '+faststart', base + '.mp4'], capture_output=True, text=True)
                    if r.returncode: raise RuntimeError('ffmpeg: ' + r.stderr[-300:])
                    files.append(rel(base + '.mp4')); info['mp4'] = rel(base + '.mp4')
            elif sg['type'] != 6 and not nfr:
                info['note'] = 'no video frames'
        except Exception as e:
            info['error'] = repr(e); errors.append('seg %d: %r' % (s, e))
        segs.append(info)

    tot = sum(x.get('frames', 0) for x in segs)
    man = dict(name=name, source=os.path.basename(path), linked_names=f.names,
               counts=dict(segments=len(f.segments), sounds=len(sounds), sprite_sets=len(sprites),
                           video_frames=tot, video_minutes=round(tot / FPS / 60, 2)),
               segments=segs, sprites=sprite_info, errors=errors, files=sorted(files))
    json.dump(man, open(os.path.join(root, 'manifest.json'), 'w'), indent=1)
    print('%s: %d segs, %d frames (%.1f min), %d sfx, %d sprite sets, %d errors' %
          (name, len(f.segments), tot, tot / FPS / 60, len(sounds), len(sprites), len(errors)), flush=True)
    return 1 if errors else 0


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('file'); ap.add_argument('outdir')
    ap.add_argument('--no-mp4', action='store_true'); ap.add_argument('--only-seg', type=int)
    a = ap.parse_args()
    sys.exit(dump(a.file, a.outdir, not a.no_mp4, a.only_seg))
