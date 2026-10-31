# Deadly Tide — decomp / port plan

Tracking: gitlab.dnsif.ca `github/deadly-tide-decomp` issues #1–#6.

## Status (2026-11-01)

| # | Step | State |
|---|------|-------|
| 1 | `.L96` container format | **done** — `docs/L96_FORMAT.md`, `tools/l96.py` |
| 2 | Asset dump | in progress (bulk dump on a DO droplet); CD4 music ripped (9 tracks, 33 min) |
| 3 | Decompile `DTPLAYER.EXE` | Ghidra export done; architecture in `docs/ENGINE_MAP.md`; gameplay data + scene graph being decoded |
| 4 | Original under Wine | **works** — see `tools/wine/README.md`; first reference recording taken |
| 5 | C + SDL3 engine | started: `.L96` reader + own Cinepak decoder + A/V playback |
| 6 | WASM build | not started |

## What the game actually is

- `DT.EXE` = launcher/options UI (pilot names, controls, graphics quality, difficulty). `DTPLAYER.EXE`
  (160KB) = the engine; it ignores its command line, everything is passed through the registry.
- The engine walks a **scene graph** of 100-byte segment records. Segment types:
  0 plain video, 5 cutscene, **1 on-rails combat**, **2 360° panorama with path choice**, **6 real-time
  free-look 3D** (e.g. LEVEL00's rotating globe mission select, textured with 128×128 Cinepak frames).
  So it's mostly video, but not only video — there is a small real-time renderer too.
- Video: Cinepak 320×200 at 15 fps (also 256×256 and 128×128 variants), played in a 320×240 mode.
  Audio: 8-bit stereo 22050 Hz streamed alongside; SFX 8-bit mono 22050 Hz. Timing is clocked off the
  audio play position.
- Each video frame carries a 0x44-byte header with the **camera** (position, look-at, roll) and
  aim/fire flags, so sprites can be placed in 3D over the pre-rendered video.
- Enemies are streamed RLE sprites (`FG ` gameplay blocks, 0x58-byte records: bbox, 3D position,
  "fires now"). No score; health 0–10000 regenerating by difficulty; death branches to a death scene.
- Branching is seek-free: the start of each branch target is duplicated into "carry" sectors of the
  groups before the branch point.
- CD4 audio tracks: neither EXE uses CD audio, so they may be a bonus soundtrack.

## Approach
Decompile for understanding, then reimplement cleanly (C11 + SDL3, own Cinepak decoder), not byte-matching.
WASM via Emscripten: stream per-level `.L96` from user-supplied files (never host game data); the
group/index layout suits HTTP range requests. The free-look 3D scenes need a small software renderer
(original is software too, so this stays portable).

## Infrastructure
- Heavy work (bulk dump, Ghidra, Wine, builds) runs on a DigitalOcean droplet (`claude-test` tag),
  not on pro. Raw decompiled C (`decomp/`), game data and dumps stay private and out of git.
