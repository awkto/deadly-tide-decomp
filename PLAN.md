# Deadly Tide — decomp / port plan

## What we have (findings so far, 2026-11-01)

- 4 CDs, extracted to `cd/cd1..cd4` (CD4 was a bin/cue; data track converted to ISO, tracks 2–10 are Red Book audio = music).
- `DT.EXE` (2.6MB, 2.4MB of it resources) — launcher / menus / options UI. GDI only.
- **`DTPLAYER.EXE` (160KB) — the actual game engine.** MSVC, built Aug 1996. DirectDraw + DirectSound,
  VfW `ICOpen/ICSendMessage` for video, joystick + keyboard, registry for settings. Small enough to fully decompile.
- `DTLAUNCH.DLL` — glue between the two (`QuietlyActivateDeadlyTideUI`).
- Game data: `LOGOS.L96`, `INTRO*.L96`, `LEVEL00–15.L96`, `EPILOG.L96`, `CDn_NEXT.L96` (~1.9GB total).

### `.L96` container (partially mapped)
- Header: magic `L96\0`, name, version 5, counts, offset table, `cvid` FourCC, list of linked level names.
- `0x9c`: 0x18000-byte table (unknown — lookup/collision/path data?).
- `0x1809c`: 256×RGB palette (768 bytes, Windows system colours in the ends).
- `~0x183a0`: stream descriptor (320×200, rates).
- Body: data in 0x6000 (24KB) blocks; each block holds one **Cinepak 320×200 frame** preceded by a
  **68-byte per-frame header**, plus other interleaved data (audio and probably sprite/hit data).
- LEVEL01 ≈ 4,360 frames ≈ 4.8 min at 15fps.

### Video: solved at prototype level
`tools/l96_cvid.py` scans for Cinepak frames, wraps them in an AVI, ffmpeg decodes them correctly as 24-bit
(see `out/grid.png`, `out/l01grid.png`). ~4% of frames fail to decode — almost certainly frames split across
a 24KB block boundary or false-positive matches; will go away once the container is parsed properly
instead of pattern-scanned.

## The real work (not the video)
The backgrounds are just a movie. The game is everything layered on top: enemies/sprites, aiming, hit
detection, damage, the path branching between video segments, timing, HUD, sound. That logic lives in
`DTPLAYER.EXE` and is driven by the per-frame headers and the side tables in the `.L96`. That's where
most of the effort goes.

## Approach (recommended)
**Decompile for understanding, then reimplement cleanly** — not a byte-matching decomp. Matching MSVC 4.x
output gives little for a 160KB binary and costs a lot. Plan:

1. **Container + assets** — full `.L96` parser (Python first): block layout, per-frame header fields,
   audio format, sprites/overlays, the 0x18000 table. Dump everything to open formats.
2. **Static RE of `DTPLAYER.EXE`** in Ghidra (needs installing): label the file reader, the frame-header
   consumer, the enemy/sprite system, input, scoring. Cross-reference with dumped data.
3. **Dynamic check** — run the original under Wine (or 86Box/Win95 VM if Wine fails on DirectDraw 8-bit)
   to record reference behaviour/screens for comparison.
4. **Reimplementation** in C (or C++) + **SDL3**: own Cinepak decoder (~500 lines), DirectSound → SDL audio,
   DirectDraw → software 320×200 surface scaled up. CD audio tracks → OGG/FLAC files.
5. **WASM build** via Emscripten. Problem: ~1.9GB data. Plan is to stream per-level `.L96` via HTTP
   range requests (the 24KB block layout suits this) and have the user supply their own ISOs/files —
   don't host the game data.

## Open questions
- What the 68-byte frame header and the 0x18000 table contain (likely the key to gameplay).
- Audio stream format inside blocks.
- Whether the game runs 8-bit palettized (palette in file suggests yes; sprites likely 8-bit).
- Does it run under Wine at all, for reference recordings?
