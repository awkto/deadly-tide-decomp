"""Verify the C Cinepak decoder against ffmpeg.

    python3 tools/verify_cinepak.py DATA_DIR FILE.L96 SEG COUNT ENGINE_BIN WORKDIR

Decodes the first COUNT frames of segment SEG with the engine (--dump-frames) and with ffmpeg
(frames pulled out via tools/l96.py, wrapped by tools/l96_cvid.py's write_avi), then prints
mean/min PSNR. Needs numpy and ffmpeg.
"""
import os, subprocess, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l96, l96_cvid

data, name, seg, count, engine, work = sys.argv[1:7]
seg, count = int(seg), int(count)
os.makedirs(work + '/ppm', exist_ok=True)
f = l96.L96(os.path.join(data, name))
frames = []
for k in range(f.segments[seg]['count']):
    frames += [c for _, c in f.group(seg, k).frames]
    if len(frames) >= count: break
frames = frames[:count]
l96_cvid.write_avi(work + '/ref.avi', frames, 320, 200, bpp=24)
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', work + '/ref.avi', '-pix_fmt', 'rgb24', '-f', 'rawvideo',
                work + '/ref.rgb'], check=True)
subprocess.run([engine, '--data', data, '--file', name, '--segment', str(seg), '--dump-frames', work + '/ppm',
                '--count', str(count)], check=True)
ref = np.fromfile(work + '/ref.rgb', np.uint8).reshape(-1, 200, 320, 3)
psnr = []
for i in range(min(count, len(ref))):
    raw = open('%s/ppm/frame_%05d.ppm' % (work, i), 'rb').read()
    ours = np.frombuffer(raw[-320 * 200 * 3:], np.uint8).reshape(200, 320, 3)
    mse = np.mean((ours.astype(float) - ref[i].astype(float)) ** 2)
    psnr.append(99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse))
psnr = np.array(psnr)
print('frames compared %d (ffmpeg produced %d)  mean PSNR %.2f dB  min %.2f dB  identical %d'
      % (len(psnr), len(ref), psnr.mean(), psnr.min(), int((psnr == 99.0).sum())))
