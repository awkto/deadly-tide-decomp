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
  Verified headless (a bot completes the game) and with scripted mouse/keyboard input in a window and the browser.
- Browser build: play it at **https://awkto.github.io/deadly-tide-decomp/** with your own game files (pick the CD folders and DT.EXE). Files are read lazily from your disk, never uploaded or copied into memory.

See [PLAN.md](PLAN.md) for details and next steps.

## Game files
Copy the `.L96` files from each CD into one folder (or keep them as `cd1/ … cd4/` subfolders):
`LOGOS`, `INTRO`, `INTROB`, `LEVEL00`–`LEVEL15`, `EPILOG`, `CDn_NEXT`, plus `DT.EXE` (for the launcher
screen). File and folder names are matched case-insensitively on every platform (`logos.l96` is fine).
The easiest layout is to put the executable next to the `cd1/ … cd4/` folders, or next to the loose `.L96` files.

### First run
Double-click the executable; no flags are needed. Without `--data` it looks for the game files in the
folder remembered from last time, then in the executable's folder, then in the current directory. If none
has `LOGOS.L96` or `LEVEL01.L96` (also inside `cd1`–`cd4`), it explains what is needed and opens a folder
picker; the folder you choose is remembered (`datadir.txt` in the per-user data folder). `--choose-data`
shows the picker again to change it; `--data DIR` always wins.

## Build (native)
```sh
cmake -S engine -B build/native -DCMAKE_BUILD_TYPE=Release   # fetches and builds SDL3 statically
cmake --build build/native -j
build/native/deadlytide --data /path/to/game --level LOGOS   # full game from the start
build/native/deadlytide --data /path/to/game --level LEVEL00 # mission select globe
```
With the original `DT.EXE` in the data folder (or its `cd1/`), the game opens with the original
launcher screen (pilots, controls, difficulty, volume), drawn from your own `DT.EXE`; `--no-menu` skips it.
Mouse aims and fires, Space fires the ACT secondary weapon, Esc skips, F2 = hub, F5 = restart level,
Pause or F3 pauses (also automatic when the window loses focus; any key or click resumes), F1 = help overlay
while paused, F4 = mute / unmute, F12 = screenshot (`dumpNNNN.bmp` in the user data folder, next to the save INI),
F11 or Alt+Enter toggles borderless fullscreen (`--fullscreen` starts in it). A gamepad works too (hot-plug,
first one used; any stick motion or button takes over from the mouse until it moves): left stick aims
(absolute, like the original joystick), A or right trigger fires, B/X/left trigger = ACT, Start = Esc, Back = F2;
in the launcher the stick moves the cursor and A clicks, D-pad up/down selects.
Q quits. Options: `--difficulty 0|1|2`, `--pilot NAME`, `--scale N`, `--filter nearest|linear|smooth`; `--help` lists everything.

The picture is two layers: the scene (video, enemies, explosions, tracers) and the HUD/crosshair overlay,
which is always drawn crisp on top and redrawn at the display rate, so the crosshair follows the mouse
smoothly even though the 320x200 video runs at 15 fps. `--filter` picks how the scene is scaled:
`nearest` (hard pixels), `linear` (bilinear), or `smooth` (the default: a Catmull-Rom 2x upscale, then
bilinear to the window size). Started from the launcher, GRAPHIC QUALITY I / II / III selects nearest /
linear / smooth unless `--filter` is given. The browser build has the same choice on the setup panel.

Headless build without SDL (for testing): `cmake -S engine -B build/headless -DDT_HEADLESS=ON`, then e.g.
`build/headless/deadlytide --data DIR --level LEVEL01 --headless --no-pace --bot` (prints the path taken
and combat stats) or `--dump-frames OUT --every 15`.

## Build (browser)
Playable build: https://tide.pro.dnsif.ca (bring your own CD files; nothing is uploaded).
With emsdk: `emcmake cmake -S engine -B build/wasm && cmake --build build/wasm`, serve `build/wasm/`
over HTTP, open `index.html`, pick (or drag in a whole folder of) your `.L96` files and `DT.EXE`, choose a start
mode (the original launcher screen, full game, a single level, or the demo) and difficulty, press Play.
Files are read lazily from your disk and never uploaded; pilots are kept in the browser's localStorage.

## Prebuilt binaries / CI
GitHub Actions (`.github/workflows/build.yml`) builds Linux, Windows (MSVC), macOS and the browser (WebAssembly)
version on every push to `main` and every pull request; each run uploads one zip per platform as a workflow artifact.
Pushing a `v*` tag publishes those zips as a GitHub release. The binaries contain the engine only: you still need
your own game files (see "Game files").

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
