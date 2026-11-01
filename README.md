# deadly-tide-decomp

Reverse engineering and clean reimplementation of **Deadly Tide** (Microsoft / Rainbow Studios, 1996),
a rail shooter that plays pre-rendered Cinepak video with enemies, aiming and a small real-time
3D "free-look" renderer layered on top.

The original engine (`DTPLAYER.EXE`) was decompiled for understanding; this repo contains the
documented formats and mechanics, plus a new engine written from those docs in C11 + SDL3 that
runs natively and in the browser (WebAssembly).

**No game data is included or hosted.** You need your own copy of the original 4 CDs.

## Status
- File formats, scene graph, gameplay data, combat and free-look renderer: documented in `docs/`.
- Engine: plays the whole game — video + audio, branching scene graph, enemies, weapons with heat,
  health/death, HUD, SFX, free-look 3D scenes, the mission-select globe, save points.
  Verified headless (a bot completes the game); needs real-mouse play testing.
- Browser build: works with files you select locally; large levels still load fully into memory.

See [PLAN.md](PLAN.md) for details and next steps.

## Game files
Copy the `.L96` files from each CD into one folder (or keep them as `cd1/ … cd4/` subfolders):
`LOGOS`, `INTRO`, `INTROB`, `LEVEL00`–`LEVEL15`, `EPILOG`, `CDn_NEXT`.

## Build (native)
```sh
cmake -S engine -B build/native -DCMAKE_BUILD_TYPE=Release   # fetches and builds SDL3 statically
cmake --build build/native -j
build/native/deadlytide --data /path/to/game --level LOGOS   # full game from the start
build/native/deadlytide --data /path/to/game --level LEVEL00 # mission select globe
```
Mouse aims and fires, Space fires the ACT secondary weapon, Esc skips, F2 = hub, F5 = restart level,
Q quits. Options: `--difficulty 0|1|2`, `--pilot NAME`, `--scale N`; `--help` lists everything.

Headless build without SDL (for testing): `cmake -S engine -B build/headless -DDT_HEADLESS=ON`, then e.g.
`build/headless/deadlytide --data DIR --level LEVEL01 --headless --no-pace --bot` (prints the path taken
and combat stats) or `--dump-frames OUT --every 15`.

## Build (browser)
With emsdk: `emcmake cmake -S engine -B build/wasm && cmake --build build/wasm`, serve `build/wasm/`
over HTTP, open `index.html`, pick your `.L96` files, press Play.

## Docs
| File | Contents |
|---|---|
| `docs/L96_FORMAT.md` | `.L96` container: header, index, groups, carry sectors |
| `docs/ENGINE_MAP.md` | original engine architecture, threads, subsystems |
| `docs/SCENE_GRAPH.md` | segment records, branch/hotspot table, death/save/hub logic |
| `docs/GAMEPLAY_DATA.md` | per-frame camera header, enemy/sprite blocks, RLE sprites |
| `docs/COMBAT_SPEC.md` | crosshair, weapons + heat, hit-scan, health, HUD, SFX |
| `docs/FREELOOK_SPEC.md` | skybox renderer, tile textures, camera modes |
| `docs/graphs/` | rendered scene graphs |

## Tools
`tools/l96.py` (reader: `info`, `check`, `graph`), `tools/sprites.py` (sprite decode + overlays),
`tools/freelook.py` (renderer prototype), `tools/dump_assets.py` (bulk export), `tools/ghidra/`
(headless decompile export), `tools/wine/` (running the original under Wine for reference).
