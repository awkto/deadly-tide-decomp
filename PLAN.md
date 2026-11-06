# Deadly Tide — decomp / port plan

Tracking: gitlab.dnsif.ca `github/deadly-tide-decomp` issues #1–#7.

## Status (2026-11-07)

| # | Step | State |
|---|------|-------|
| 1 | `.L96` container format | **done** — `docs/L96_FORMAT.md`, `tools/l96.py` |
| 2 | Asset dump | **done** — all 29 files, 413 segments, ~92 min video; private copy in `dump/`; CD4 music as FLAC |
| 3 | Decompile `DTPLAYER.EXE` | **done** — `docs/COVERAGE.md`: of the game's own code 93.8% (by size) is documented, 99.9% understood; the one unexplained function is dead code. Specs: `ENGINE_MAP`, `SCENE_GRAPH`, `GAMEPLAY_DATA`, `COMBAT_SPEC`, `FREELOOK_SPEC`, `LAUNCHER` |
| 4 | Original under Wine | **works** — LEVEL01 compared with the port: HUD, palette, sprites match; segment timing identical; idle-player death time matches (median ~42 s vs 40–51 s); enemy-fire model audited against the code |
| 5 | C + SDL3 engine | feature-complete: combat, free-look 3D, hub mission select, saves, demo mode; bot finishes the whole game; mouse, gamepad, fullscreen, pause/help/mute/screenshot keys; first-run data-folder picker; CI builds Linux/Windows/macOS |
| 5b | Launcher (DT.EXE) | documented (`docs/LAUNCHER.md`); the Pilot Control Center is rebuilt in the engine from the user's own DT.EXE bitmaps, native and web |
| 6 | WASM build | works in Chrome: lazy File.slice reads (Asyncify, 64MB heap for any level), launcher/level/full/demo start, folder drop, pilots in localStorage, mouse combat, 15 fps |

## Next
1. Real-user play testing (mouse feel, difficulty, audio sync on real speakers: a test rig measured ~0.9 s audio lag, likely its virtual sound device).
2. First release: tag `v0.1.0` (CI attaches Linux/Windows/macOS/web zips, no game data).
3. Optional: a hosted web page (code only; users pick their own files); 16-bit tint blend (`Px_Plot16Tinted`); START button animation details (sprite classes not decompiled).

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
