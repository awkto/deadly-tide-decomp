# deadly-tide-decomp

Reverse engineering and reimplementation of **Deadly Tide** (Microsoft / Rainbow Studios, 1996),
a rail shooter that plays Cinepak video backgrounds with gameplay layered on top.

Goal: understand the original engine (`DTPLAYER.EXE`) and the `.L96` data format, then write a clean
reimplementation (C + SDL3) that runs on modern PCs and in the browser (WASM).

**No game data is included or hosted.** You need your own copy of the original 4 CDs.

See [PLAN.md](PLAN.md) for findings and the roadmap.

## Tools
- `tools/l96_cvid.py <file.L96> <out.avi> [max_frames] [24|8]` — prototype: extracts Cinepak frames
  from an `.L96` into an AVI that ffmpeg can decode.
