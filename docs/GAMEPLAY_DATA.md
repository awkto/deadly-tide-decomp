# Deadly Tide per-frame gameplay data

This document covers the data that links the video to the gameplay: the 0x44-byte header in front of
every video frame, the `FG` object blocks (enemies and overlays, one block per video frame), and the
sprite sets in the `.L96` header section. Container layout: `docs/L96_FORMAT.md`. Engine overview:
`docs/ENGINE_MAP.md`. Tool: `tools/sprites.py` (it uses `tools/l96.py` and does not change it).

Function addresses are in `DTPLAYER.EXE`. Names follow `decomp/symbols.tsv`. "Checked" means the
claim was tested on LEVEL01 data with `tools/sprites.py`, unless a different file is named.

## 1. Coordinate conventions (read this first)

* The render buffer is 320×200, stored top-down. Every sprite draw call (`Obj_DrawLayer` 0x412e40,
  `Obj_DrawStatic` 0x40aaa0 and the HUD draws in `Hud_Draw` 0x409e20) passes the **last** buffer row
  as the base and **−width** as the stride to `Spr_Draw` 0x412d20. As a result:
  * the **y coordinates in records are counted from the bottom row** (y = 0 is the bottom of the screen);
  * **sprite pixel rows are stored bottom row first**.
  To draw a record on a normal top-down image, flip the decoded sprite vertically and place its top
  left corner at `(x, 200 − y − h)`. A hit bbox becomes `(x0, 199 − y1, x1, 199 − y0)` on screen.
  `Obj_ProjectAll` 0x403780 applies this same flip to the bbox in place, using scene dword 0x40
  (the video height, 200). Evidence from the data: the HUD message sprite "LASER OVERHEAT"
  (LEVEL01 sprite set 9, frame 1) only reads upright with this convention.
* World coordinates are plain integers, in the same units for the camera and the objects. The camera
  "look-at" point is always about 200 units in front of the camera. Fitting the data shows that
  world +Y appears as screen-down (see §2.2).

## 2. Per-video-frame header (0x44 bytes)

Every video frame in a group is `{0x44-byte header, Cinepak frame}`. `Play_Chunk` 0x4067b0 hands the
header pointer to `Frame_Process` 0x406e90. That function copies all 17 dwords to `g_frameHdr`
(0x4254f8) and gets the frame size back from `Vid_DecodeFrame` 0x412ba0 (size = dword 0 + 0x44).

| dword | off | type | field | used by | notes |
|---|---|---|---|---|---|
| 0 | 0x00 | u32 | Cinepak frame size | `Vid_DecodeFrame` 0x412ba0 | Cinepak data starts at +0x44 |
| 1–3 | 0x04 | i32 ×3 | camera position x, y, z | `Cam_UpdateFromHeader` 0x402750, `Enemy_Update` 0x4043f0, `Obj_ProjectAll` 0x403780 | world units; plain ints converted to double, **not fixed point** |
| 4–6 | 0x10 | i32 ×3 | look-at point x, y, z | same | ‖look − cam‖ = 198…201 in every type-1 frame checked |
| 7 | 0x1c | i32 | roll, **degrees** | `Cam_UpdateFromHeader` passes −roll to `Mat_LookAt` 0x40ab40 / 0x40ae40, which multiply by π/180 (constant at 0x4200f0) | LEVEL01 range about −30…+20 |
| 8 | 0x20 | u32 | frame flags | `Frame_Process`, `Player_Update` 0x410740 | bit 0: aim/combat on this frame. In type-1 scenes, `Enemy_Update` and `Hud_Draw` run only when it is set, and `Player_Update` reads input only when it is set **and** scene byte 0x44 ≠ 0. bit 1: the player may fire (also needs scene byte 0x45 ≠ 0); it is also passed to `Hud_Draw` as its 3rd argument. This is the dword that can differ between a group and its carry copy (L96_FORMAT §6). |
| 9–16 | 0x24 | — | unused | no code reads them | 0 in every LEVEL01 frame |

Only **type-1 scenes** use dwords 1–8. In type-0 scenes the flags are 0, and dwords 1–7 hold garbage
(huge values, probably leftover memory from the authoring tool). `Enemy_Update` copies them and then
returns early for types 0 and 5. Types 2 and 6 take the view from the player's yaw/pitch/roll
(`g_view` +0x28…+0x30) instead.

### 2.1 Field of view

`Cam_UpdateFromHeader` also computes `tan(scene_i16[0x4a] · π/92160)` (constant at 0x420020), i.e.
**scene short 0x4a = half the field of view in 1/512 degree**. LEVEL01 uses 19788 (38.6°) in segment 5
and 15360 (30°) in segments 15/17. `Proj_ScreenToWorld` 0x40b630 uses screen scale
`k = (W/2) / tan(half_fov)` and `sx = W/2 + X·k/Z`. Y uses the same scale.

### 2.2 Check: the camera agrees with the objects

`tools/sprites.py project()` rebuilds the view from (camera, look-at, roll, half_fov) as a yaw then
pitch look-at plus roll, which is the same order as `Mat_LookAt`. The axis signs were fitted to the
data. It projects each object's 3D position (record +0x20) and compares the result with the centre
of the object's screen bbox:

| segment | objects | median error | 75th percentile |
|---|---|---|---|
| LEVEL01 seg 5 | 229 | 1.8 px | 3.3 px |
| LEVEL01 seg 17 | 4180 | 3.1 px | 9.8 px |

With the roll negated, the median error grows to 25–60 px. The outliers are a few actors whose 3D
position is stale or (0,0,0). The engine itself uses the 3D position only for its distance to the
camera (see §3.2), so a coarse position does no harm. Camera motion is smooth: the largest
frame-to-frame camera step is 2 units in seg 5 and 12 units in seg 17
(`python3 tools/sprites.py hdr FILE SEG [STEP]`).

## 3. FG object blocks

### 3.1 Block layout

The FG part of a group (`b1` sectors, see L96_FORMAT §5) holds **one block per video frame**, so 5
blocks per group, followed by zero padding up to the sector boundary. Type-6 groups have no video
but still have 5 blocks per group. `Play_Chunk` 0x4067b0 starts at the FG part, and after each frame
it advances by the size that `Obj_BlockSize` 0x4038d0 returns.

| off | type | field |
|---|---|---|
| 0x00 | char[4] | `FG \0` |
| 0x04 | i32 | `n`: object count |
| 0x08 | i32 | body size = `n·0x58 + Σ record data sizes`. The engine recomputes this itself; it matches in every block checked |
| 0x0c | 0x58 × n | object records |
| … | | pixel data of the records (RLE, §4) |

Next block = `(0xc + body + 3) & ~3`. `Obj_BlockSize` computes the same thing as
`0xc + Σ(0x58 + rec.size)`, rounded up to 4.

### 3.2 Object record (0x58 bytes)

The same layout is used for the frame descriptors of the sprite sets (§5).

| off | type | field | evidence |
|---|---|---|---|
| 0x00 | i16 | actor id: index into the 0x28-byte actor table (`table28`, `g_actors` 0x4213c0) | all actor code indexes `id·0x28` |
| 0x02 | u8 | kind (a bit mask). **1** = rail enemy (only live in type-1 scenes), **2** = enemy / target, **4** = effect (set at run time when an explosion frame replaces the sprite), **8** = static overlay (HUD widget). Every other value is skipped | `Obj_DrawLayer` draws mask 3 and then mask 4; `Obj_DrawStatic` draws kind 8 (called from `Hud_Draw`); `Actor_UpdateStates` / `Enemy_Update` / `Weapon_HitScan` skip kind 8, and skip kind 1 outside type 1 |
| 0x03 | u8 | 0 | — |
| 0x04 | i32 | pixel data offset, **relative to the block start** | `Actor_UpdateStates` 0x403a80 rebases it to a pointer (+= block); it is zeroed when the actor is gone |
| 0x08 | i32 | pixel data size in bytes (padded to 4) | `Obj_BlockSize` |
| 0x0c | u32 | flags (below) | |
| 0x10 | i16 ×4 | draw x, y (y from the bottom), w, h | `Obj_DrawLayer` → `Spr_Draw` |
| 0x18 | i16 ×4 | hit bbox x0, y0, x1, y1, inclusive, y from the bottom. Same as the draw rect in the stream (x1 = x + w − 1) | `Obj_ProjectAll` flips y; hit test in `Obj_HitTestPoint` 0x403ff0 (grown or shrunk by actor-class byte 1 & 0x3f) |
| 0x20 | i32 ×3 | 3D position x, y, z, world units | `Obj_ProjectAll`: stores ‖cam − pos‖² in the actor (+0x10, float), then **overwrites** pos with the un-projection of the bbox centre at that distance |
| 0x2c | i32 | type 6 only: cube-face mask. bits 0–5 select faces (+Z, −X, −Z, +X, −Y, +Y order in `FreeLook_FixupObjects` 0x402d30). `Enemy_Update` uses `>>6` of it in type 6 | 0 in type 1 |
| 0x30 | i16 ×4 ×n | type 6 only: one rect (x0, y0, x1, y1) per set face bit, in 512×512 face coordinates (centre 256, cube half-size 256.0) | `FreeLook_FixupObjects` |
| 0x48 | u8 | tint: 0 = plain copy; ≥ 0x80 = blend table (shade1 #tint−0x80); 1…0x7f = shade2 table | `Spr_Draw` → `Spr_SetTint` 0x40db00 |
| 0x49…0x57 | | 0 in all data seen | — |

Flags (+0x0c):

| bits | meaning | evidence |
|---|---|---|
| 31 (0x80000000) | targetable | `Obj_IsTargetable` 0x4021a0 tests byte +0xf & 0x80 |
| 30 (0x40000000) | **fires now** (damage random, scaled by actor-class byte 2 and hit % +0x1d) | `Enemy_Update` |
| 7 (0x80) | **fires now** (sure hit, full class damage byte 2) | `Enemy_Update` |
| 18–29 | muzzle x, screen pixels | `Enemy_Update`: `(f & 0x3ffc0000) >> 18` |
| 9–17 | muzzle y (`Enemy_Update` uses `199 − ((f & 0x3fe00) >> 9)` in type 1) | see below |
| 8 (0x100) | spawn roll: on this frame the actor's spawn is re-rolled with class probability +0x1b | `Actor_UpdateStates` first loop (byte +0xd & 1) |
| 6 (0x40) | remove the actor now (state = 0x7f, gone) | `Actor_UpdateStates` |

If neither fire bit is set, `Enemy_Update` still lets a targetable live enemy fire at random, using
class fire % +0x1c. The fire bits are scripted shots.
Seen in the data: LEVEL01 and LEVEL05 have no scripted shots. LEVEL10 seg 4 has 16, all bit 7, with
muzzle x inside the enemy bbox. The stored muzzle-y field is about **bbox-y0 + 320**
(e.g. 449 for bbox y 128…145), so the engine's `199 − y` falls off-screen and its tracer draw is
skipped while the damage still applies. Whether this is an encoding we have not understood or a real
quirk is **open**.

The actor state (byte +3 of the 0x28-byte actor entry) works as follows. Below −0x40 the actor is
alive (each hit adds 1). −0x40…−1 is the countdown after death. 0…n indexes the frames of the
explosion animation picked by `Actor_PickExplosion` 0x403910, which also sets kind 4 and swaps in
that frame's pixels, size and tint. 0x7f means gone. This is in `Actor_UpdateStates` and
`Weapon_HitScan` 0x404140.

### 3.3 Draw order per frame (type 0/1/5)

`Frame_DecodeVideo` 0x412840 runs `Obj_ProjectAll`, then `Actor_UpdateStates`, then decodes the
video, then calls `Obj_DrawLayer` with mask 3 (kinds 1, 2) and then mask 4 (kind 4). After that,
`Hud_Draw` calls `Obj_DrawStatic` (kind 8). In LEVEL00, `Obj_DrawLayer` also hides kind-1 records
whose actor id is above `2·highestLevel − 1` (the hub's locked doors).

LEVEL01 contents: type-1 segments hold kinds 2 and 8 only. Most frames carry one kind-8 record
(actor 0, a radar/gauge widget of about 44×26 to 51×43 at the top left; 120 of 120 frames in seg 5,
645 of 965 in seg 17). Type-6 segments hold kind 2 only, with cube-face rects.

**Types 2 and 6 differ.** `Pano_Frame` 0x4129b0 and `FreeLook_Frame` 0x412ab0 do not draw the records
on the screen. They draw them into the panorama/texture buffer (0x428a98), using scene dwords
0x3c/0x40 (512×512 in LEVEL01 type 6) as stride and height, with the wrap flag set. Masks 1, 2 and 4
are drawn into that buffer, and then the buffer is projected. In type-6 data the draw rect looks
like a 128-wide strip with y in the thousands (e.g. x 0, y 5235, 128×2854), so the
draw-coordinate mapping there is **not decoded**; the cube-face rects at +0x30 carry the hit areas.

## 4. Sprite RLE

Decoder: `Spr_BlitRLE8` 0x410ae0 (`Spr_BlitRLE16` 0x410d40 is the 16-bit twin). Read one signed
byte `c`:

* `c < 0`: copy the next `−c` bytes (palette indices) to the current position;
* `c > 0`: skip `c` transparent pixels;
* `c == 0`: read a **big-endian** u16 `n`; move down `n` rows and return to x = 0. Stop when the row
  reaches `h`.

The first row is the bottom row (§1). Rows end with `00 00 01`. An empty sprite is `00 00 C8`
(skip 200 rows). Stream sprites often begin with `00 00 00`. The decoder uses exactly the record's
size minus 0–3 padding bytes on every set frame checked in LEVEL01 and LEVEL08. Pixel values are
indices into the level palette (L96 header 0x48). The 8-bit plain-copy span routine is 0x40de10. Tinted
spans (0x40dec0) look up `shade[(src<<8)|dst]` to get an RGB555 colour, then map it back through
`lut555`. So the shade tables are **256×256 blend tables** (translucent / additive explosions).

## 5. Sprite sets (`EX ` records in the header section)

`Sprites_Load` 0x4022b0 reads each set as a 16-byte header plus `size` bytes:

| off | type | field |
|---|---|---|
| 0x00 | char[4] | `EX ` |
| 0x04 | u32 | frame count |
| 0x08 | u32 | payload size |
| 0x0c | i16 | list / type: 0 explosions (`g_anims` 0x437fb0), 1 HUD (0x437b70), 2 crosshair (0x43aa40), 3 gun flash / ACT (0x43af80), 4 enemy shot streaks (0x439500). Negative values (−3, −4) mean "pre-expand at load" |
| 0x0e | i16 | size hint. For explosions it is the nominal size: `Actor_PickExplosion` picks a set whose hint is between max(w, h) of the target and twice that. Other lists use it differently (200 for HUD / full screen) |

The payload is `count` × 0x58 frame descriptors (the record layout of §3.2: +4 data offset
**relative to the payload start**, +8 size, +0x10 x, y, w, h, +0x18 bbox, +0x48 tint), followed by
the RLE data. The engine keeps a 0x110-byte set entry: +4 size hint, +0xc count,
+0x10 pre-expanded flag, +0x14 frame-descriptor pointers. For type 3 a small table of power-of-two
steps at +0x90 is filled from the count; its use is not traced.

**"Compressed" (negative type) sets** are stored with the same RLE. `Sprites_ExpandSet` 0x402600
decodes each frame at load into a raw `w·h` bitmap (zero = transparent). It does this by calling the
RLE blitter with stride = w and the plain-copy span. It then rewrites +4 to point into the
expanded buffer and +8 to `w·h`. Only LEVEL08 has such a set (type −3, 3 frames). There is no
second compression algorithm.

LEVEL01 sets: 9 explosion animations (16 frames each, 320×200 canvases centred on the screen,
tint 0x80, hints 15…59); a HUD set (gauge frame, messages such as "LASER OVERHEAT"); a crosshair set
(27×27); two type-3 sets (gun flash with 7 frames, a 20-frame full-screen set probably used for the
ACT weapon); and 3 enemy-shot streak sprites (picked by on-screen streak length versus frame height
in `Enemy_Update`).

## 6. Tool and proof

```
python3 tools/sprites.py hdr     cd/cd1/LEVEL01.L96 17 25        # camera/flags over a segment
python3 tools/sprites.py fg      cd/cd1/LEVEL01.L96 5 21         # object records of one group
python3 tools/sprites.py sets    cd/cd1/LEVEL01.L96
python3 tools/sprites.py setpng  cd/cd1/LEVEL01.L96 9 out/x 0,1   # sprite-set frames to PNG
python3 tools/sprites.py overlay cd/cd1/LEVEL01.L96 17 130 3 out/x.png
```

`overlay` decodes the video frame with ffmpeg, starting from the last Cinepak key frame. It pastes
every streamed sprite where its record puts it, and draws the hit bbox (green), the draw rect
(yellow) and the projected 3D position (red cross). Results are in `out/gameplay_check/`. Tints are
not applied; tinted sprites are drawn with plain palette colours.

## 7. Open points

* Muzzle-y encoding of scripted shots (§3.2).
* Type 6: the meaning of `+0x2c >> 6`, the full face numbering, and how the draw rect maps into the texture buffer.
* The type-3 step table in the set entry. The exact role of shade2 (< 0x80) tints.
* Frame-header dwords 9–16 are never read, and are 0 in every frame checked.
