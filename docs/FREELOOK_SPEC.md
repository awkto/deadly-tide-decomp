# Deadly Tide type-6 "free-look" renderer: implementation spec

Status: the renderer, texture source and camera are decoded and checked against the original.
The prototype `tools/freelook.py` reproduces frames of a Wine recording of the original LEVEL00 hub,
and the cut from LEVEL01 segment 5 into segment 6, at pixel level (§8).
Addresses refer to `cd/cd1/DTPLAYER.EXE`. Names follow `ENGINE_MAP.md` / `decomp/symbols.tsv`.
Confidence: **H** = read in code and confirmed by the prototype, **M** = read in code but not
exercised, **L** = inferred.

## 0. Summary

* A type-6 segment has no video. The background is a **skybox cube** around a camera at the origin.
  Each face is a 512×512 pre-rendered image split into **4×4 tiles of 128×128**. The camera looks
  out from inside the cube. LEVEL00's "globe" is a world map drawn onto the six faces of this cube.
  It is not a sphere. **H**
* The 96 tiles (6 faces × 16) are **128×128 Cinepak key frames**. They arrive in the carry sectors of
  the **last 20 groups of the segment before** the type-6 segment, and are decoded one per frame
  into a 128 × 12288 "strip" texture buffer. The tiles do **not** change while the type-6 segment
  plays. Only the FG sprites, drawn into the strip each frame, animate. **H**
* Rendering: each face is a 5×5 vertex grid. That gives 6×16 quads and 192 triangles, each mapped
  to one tile. They go through Chris Hecker's 1995/96 *perspective-correct texture mapper*
  ("sub-affine" version): 28.4 fixed-point vertices, float gradients of 1/z, u/z and v/z, a
  perspective divide every 8 pixels, and 16.16 affine steps in between. There is no lighting, no
  LUT, no Z-buffer and no wrapping. Texels are copied directly: 8-bit palette indices, or 16-bit
  words in 16-bit mode. **H**
* The camera has yaw, pitch and roll in units of **2496 per turn**, and half-FOV from segment
  i16 0x4a. The screen is 320×200. The mouse position works as a joystick: its offset from the screen
  centre sets the turn rate. Four control modes are selected by segment byte 0x21. **H** for mode 1
  (LEVEL00) and the mode-2 core (LEVEL01); **M/L** for the auto-turn planner and mode 3.

## 1. Data layout

### 1.1 Texture preload (carry sectors of the preceding segment)

`Play_SceneLoop` 0x406020: when a type 0/1 segment's `next` is type 6, the segment plays its first
`count−20` groups normally. It plays the last **20** (.data 0x4210ec) through
`Play_FreeLookEnter` 0x407a70. That function still shows the video, and also passes `Play_Chunk`
0x4067b0 a list of tile sources and destinations.

| item | value | evidence |
|---|---|---|
| groups carrying tiles | last 20 of the preceding segment, b2 = 15,15,…,15,3 sectors | 0x407a70, L96_FORMAT §6 |
| tiles per group | 5 (one per video frame); the 20th group carries 1 | 0x407a70 `param_2 - i == 1` branch |
| tile slot in carry | 0x1800 bytes (3 sectors), .data 0x4210dc | 0x407a70 |
| tile slot layout | u32 cinepak length, 0x40 bytes unused (zero), Cinepak frame (128×128, 1 strip, key) | 0x412c90 (`src+0x44`, length `*src`) |
| total | 19×5+1 = **96 tiles** | |
| destination | `g_texBuf` (DAT_0042a370) + i·0x4000·bpp | 0x407a70 (`+= bpp*0x4000`) |

`Play_Chunk` decodes the pending tile during its idle wait before each frame deadline, with the
128×128 decompressor (DAT_0042137c, `Vid_DecodeFrame` 0x417760 via 0x412c90). After each drawn frame
it moves to the next source/destination pair (0x4067b0, `local_74/local_78` += 1). Decode the
tiles in order with one decoder. Every shipped tile is a key frame (flags byte 0, strip id 0x10), so
order does not matter in practice.

0x412c90 copies the decoder's DIB output into the tile with the **row order reversed**. Result:
**memory row 0 of a tile holds the bottom image row**, as ffmpeg shows it. The whole strip buffer
is therefore "bottom-up per tile". **H**: only this orientation reproduces the reference (§8).

### 1.2 Strip texture buffer

```
strip[128 cols × 12288 rows] (bpp = 1 or 2 bytes)
tile t (0..95) occupies rows t*128 .. t*128+127
t = face*16 + qrow*4 + qcol        face 0..5, qrow/qcol 0..3 (grid quad, §2.1)
```

When the engine enters the type-6 segment (0x406020, just before 0x407bf0), it overwrites the
segment record's `view_w/view_h` (0x3c/0x40) with **0x80 / 0x3000**. The values in the file are
512×512. `FreeLook_Frame` passes these to `Obj_DrawLayer` as the stride and height of the strip,
so FG sprites are drawn **in strip coordinates** (§4). **H**

### 1.3 Type-6 segment groups

Each group is `[audio 8 sectors][video 0][FG b1][carry b2]`. The FG part holds 5 blocks, one per
frame (GAMEPLAY_DATA §3). Entry 2 and the last entry are **virtual** groups. They have the same
audio+FG layout, assembled from the carry sectors of the stored entries (L96_FORMAT §6). They
carry no texture data. **H** (verified for LEVEL01 seg 6: entry 2 = 0x16 FG + 8 audio = 30 = 15+15
carry sectors of entries 0, 1).

### 1.4 Relevant segment fields

| field | use in type 6 |
|---|---|
| 0x21 `look_mode` | control sub-mode (§5). Shipped: mode 1 = LEVEL00 hub only; mode 2 = LEVEL01/05/10/11/12; mode 3 = LEVEL02/03/04/07/14/15. Modes 4 and 0 are unused |
| 0x4a `half_fov` | half FOV in 1/512° (`tan(v·π/92160)`, const 0x420020). LEVEL00: 19200 = 37.5°; LEVEL01: 19788 / 15360 |
| prev seg 0x18/0x1a/0x46 | entry yaw / pitch / roll (from the segment **before**) → `View_Set` 0x40e1d0 in 0x407bf0 |
| target seg 0x1c/0x1e/0x48 | exit yaw / pitch / **roll** that the auto-turn steers to. 0x48 is read by the mode-2/3 planner 0x40f1c0 (it is 0 in every shipped type-6 target) |
| 0x16/0x17, 0x5c | hotspot records / choice start frame (SCENE_GRAPH §2, §5) |

## 2. Geometry and projection

### 2.1 Cube grid (`FreeLook_InitGrid` 0x405310)

There are 150 vertices, stored as doubles (x, y, z) at 0x4246d0: face f at 0x4246d0 + f·0x258, vertex
(row r, col c) at +r·0x78 + c·0x18. The cube half-size is **2048** and the grid step is 1024.
With `a = -2048 + 1024*c`, `b = 2048 - 1024*r`:

| face | name | vertex (r, c) | content (LEVEL01 / LEVEL00) |
|---|---|---|---|
| 0 | +Z | ( a, b, +2048) | forward / Pacific |
| 1 | −X | (−2048, b, a) | |
| 2 | −Z | (−a, b, −2048) | |
| 3 | +X | (+2048, b, −a) | |
| 4 | −Y | ( a, −2048, b) | **ceiling** / North pole |
| 5 | +Y | ( a, +2048, −b) | **floor** / Antarctica |

**World +Y points down** (screen convention). The face order matches the cube-face mask bits of
FG records (+0x2c bit f) and `FreeLook_FixupObjects` 0x402d30. That function uses the same
layout at half-size 256: face point (x, y) in 0..511 → e.g. face 0 `(x−256, 256−y, 256)`.

Quad (r, c) for r, c in 0..3 uses vertices v = r·5+c, v+1, v+5 and v+6. It is drawn as two
triangles with these texture coordinates (`FreeLook_Render` 0x405710, constant 0x42fe0000 = 127.0):

```
T1: v(0,0)  v+6(127,127)  v+1(127,0)
T2: v(0,0)  v+6(127,127)  v+5(0,127)
```

UV runs from 0 to **127**, not 128, so the tile's last row and column fall exactly on the edge.
v = 0 is memory row 0 of the tile (the bottom image row, §1.1).

### 2.2 Camera matrix (`Cam_UpdateFromHeader` 0x402750, type ≠ 1, 2)

Angles are taken from `g_view` +0x28/+0x2c/+0x30 (yaw, pitch, roll, ints). They are converted to
degrees with ×360/2496 (const 0x420008) and then to radians.
`Mat_FromEuler` 0x40acf0 builds 4×4 double matrices for **row vectors** (`p' = p·M`, translation
in row 3, `Mat_Mul` 0x411300 `out = a·b`, `Mat_XformPoint` 0x4114c0):

```
M_view (0x43af00) = T(-camPos=0) · Ry(-yaw) · Rx(-pitch) · Rz(-roll)
Ry(a) = [[ c,0,-s],[0,1,0],[ s,0,c]]   (0x4111e0)
Rx(a) = [[1,0,0],[0, c, s],[0,-s, c]]  (0x411150)
Rz(a) = [[ c, s,0],[-s, c,0],[0,0,1]]  (0x411270)      c=cos a, s=sin a
```

Effect: positive yaw turns right (from +Z toward +X), and positive pitch looks **up** (toward −Y). The roll
sign is exactly as in the matrices above; the seg 5 → 6 match in §8 confirms it. (0x40b010 builds the
matching inverse at 0x43c4e0 for un-projection; M.)

### 2.3 Projection (`Proj_WorldToScreen` 0x40b4d0)

Arguments: `(vertex, out, M, W, H)` plus a hidden double `tanHalfFov`. FreeLook passes
**W = 0x1400, H = 0xc80**, the screen in **28.4 fixed point**. FixupObjects passes 320/200.

```
v = p·M ;  if v.z < 1.0 → status 0 (out = -1,-1,-1)
k = (W/2) / tanHalfFov                       // same k for x and y (square pixels in 320x200)
sx = W/2 + round(v.x/v.z * k)                // round = +/-0.5 then truncate (0x4200d8, 0x4200d0)
sy = H/2 + round(v.y/v.z * k)                // +y down; screen buffer row 0 is the top
out = (sx, sy, v.z)                          // z kept as the view depth (not 1/z)
status = 2 if |v.x| <= v.z and |v.y| <= v.z (inside a 90° frustum), else 1
```

### 2.4 Triangle submission (`FreeLook_Render` 0x405710, called by `FreeLook_Frame` 0x412ab0)

```
for each of 150 vertices: project; vertex = {x28.4:int, y28.4:int, z:float, u:float, v:float}
for face in 0..5, quad r,c in 0..3:
    tex = strip + (face*16 + r*4 + c) * 0x4000 * bpp
    for T in (T1, T2):
        if any vertex of T has status 2
           and all 3 x in [-0x2800, 0x3c00] and all 3 y in [-0x2800, 0x2800]:   // guard band
            Tri_Raster 0x408500(screenPitch=320, screen, verts, texPitch=128, tex)
```

There is no back-face culling and no near-plane clipping. A vertex adjacent to one inside the 90°
frustum is always far in front of the camera (quads are 1/4 face), so the −1 placeholder for
z < 1 never reaches the rasteriser in practice. All triangles are seen from inside a convex box, so
they never overlap and need no depth sort. Draw order: face 0..5, quad row-major, T1 then T2.

## 3. Rasteriser (Hecker sub-affine, 0x407ee0 / 0x4080e0 / 0x4083c0 / 0x408500 / _TEXT 0x43f000)

The code is a near-literal copy of Chris Hecker's "Perspective Texture Mapping" (Game Developer,
1995–96), `TextureMapTriangle` with `gradients`/`edge` and the sub-dividing assembly inner loop.
An existing implementation of that article can be used directly. Details for a C port:

* **Vertex**: 5 dwords, `x, y` int 28.4; `z, u, v` float. `Tri_Raster` 0x408500 first rejects
  the triangle if all three vertices are left of 0, right of 0x1400, above 0 or below 0xc80. It then
  sorts by y into top/middle/bottom.
* **Gradients** (`Grad_Init` 0x4080e0): per vertex 1/z, u/z, v/z (`1.0/z`, const 0x4200b8).
  `dX = 1/(((x1−x2)(y0−y2) − (x0−x2)(y1−y2))/16)` with ints in 28.4 (>>4 rounds toward zero),
  then the d/dx and d/dy of the three quantities. The ×8 steps are precomputed (const 0x4200bc
  = 8.0). The per-axis rounding bias for u and v (+0x48/+0x4c) is **0x8000**, or **0x7fff** when
  the gradient sign test says the coordinate decreases (Hecker's `dUdXModifier`).
* **Edges** (`Edge_Init` 0x407ee0): `Y = ceil(y/16)` (`(y+15)>>4`), height = `ceil(yEnd) − Y`.
  X uses a floor-div-mod DDA: x step, numerator and denominator in 1/16 units (Hecker
  `FloorDivMod`). The float 1/z, u/z, v/z values at the edge start get prestep corrections
  (const 0x4200a8 = 1/16). The left edge steps with an extra-step correction when the error
  term wraps.
* **Scan conversion**: the long edge against the top-middle edge, then the long edge against the
  middle-bottom edge. Left and right sides come from the y-sorted vertex order. **Vertical clip**:
  edges entering above row 0 are advanced; spans are clipped to rows 0..199. **Horizontal clip**
  (`Span_ClipX` 0x4083c0): X is clamped to 0..320. The start/end 1/z, u/z, v/z are advanced by
  gradient·Δx, then restored after the span (globals 0x42563c…0x425658).
* **Span** (8-bit 0x43f000, 16-bit 0x43f2c9; same algorithm with word pixels and pointers):
  * FPU set to truncation (control word &= ~0x300).
  * `width = xRight − xLeft`. There are `width>>3` affine sub-spans of 8 pixels. When `width%8 == 0`
    the last full sub-span is handled as the remainder.
  * At each sub-span end: `z = 1/(1/z)`, `u = u/z·z`, `v = v/z·z`. The next divide is overlapped
    with the inner loop.
  * u, v are 16.16 fixed (×65536.0, const 0x42131c). Steps are `(uEnd−uStart)·8192.0` (=65536/8,
    const 0x421320).
  * Inner loop: Hecker's carry trick. `esi` = texel pointer, `ebx`/`ecx` = u/v fractions in the
    high bits. The step is chosen from a two-entry table {`vInt*128 + uInt`, same + 128}
    (0x421330/0x42132c) by the v-fraction carry. Pixel = `tex[vInt*128 + uInt]`, with the u/v
    bias added.
  * Remainder (`width%8` pixels or the final sub-span): one more perspective divide at the span
    end, then plain per-pixel 16.16 stepping with `Δ/count`.
* **Output**: `dst[y*320 + x] = texel`. 8-bit: palette index, no remap. 16-bit: the 16-bit word.
  No shading, LUTs, transparency, filtering or texture wrap. u, v stay inside 0..127 because the
  vertex UVs are 0/127.
* An equivalent, simpler implementation is a per-pixel ray cast (what `tools/freelook.py` does). It
  differs only by sub-texel amounts, from the 8-pixel affine segments and rounding.

## 4. FG sprites in type-6 scenes

### 4.1 Drawing (`FreeLook_Frame` 0x412ab0)

```
FreeLook_FixupObjects(…)          // 0x402d30: cube rects → screen bboxes (hit tests)
Actor_UpdateStates(…)             // 0x403a80
screen = DD_Lock()                // 0x40c3f0
Obj_DrawLayer(objs, saveUnder=0, mask=1, strip, w=0x80, h=0x3000, …, draw=1)   // permanent paint
Obj_DrawLayer(objs, saveUnder=1, mask=2, strip, 0x80, 0x3000, …, draw=drawFlag) // save-under
if drawFlag:
    FreeLook_Render(strip → screen)
    Obj_DrawLayer(objs, 0, mask=4, screen, 320, 200, …)   // explosion/effect frames, screen space
```

* The record draw rect (+0x10 x, y, w, h) is **in strip coordinates**: x in 0..127, and
  `y = tile*128 + row`. The blitter (`Spr_BlitRLE8/16` 0x410ae0 / 0x410d40) writes RLE row i
  to strip row y+i, which is image-bottom-up like the tiles. Sprite data is authored that way.
  One sprite that crosses tile boundaries is stored as a single tall RLE (h up to ~9900) whose row
  skips (`0, u16 n`) jump between the tiles. Example: LEVEL01 seg 6 actor 7, xywh (41, 6089, 55, 55),
  face 2 rect (425,457)-(479,511) = tile 2·16+15, row 73. **H** (prototype reproduces the
  LEVEL00 badges in the reference recording.)
* **Kind 1** records are drawn without save-under, so they **stay painted into the texture**. The LEVEL00 hub uses
  this: each of the first ~20 frames carries one badge or label record (actors 0..18), and they
  build up into the level markers. **Kind 2** records (LEVEL01's enemies) are drawn with save-under
  (pixel op `0x40dc60`: copies the destination run to the save buffer at 0x428598 first). After
  Present, `Spr_RestoreBackground` 0x40daa0 restores them, so the strip is clean again for the next
  frame. **H** for the code path; the restore order is **M**.
* In 16-bit mode, sprite palette indices go through the 256-entry palette→16-bit table at 0x431e50.
  Tint ≠ 0 selects the shade-table ops (`Spr_SetTint` 0x40db00), as on screen.
* **Hub lock** (`Obj_DrawLayer`, 0x412e40): if the level file is `LEVEL00.L96` (string 0x4212c4), a
  kind-1 record with actor id > 1 and id > 2·HighestLevel − 1 **ends the layer pass** (`break`).
  There is one record per frame in the hub, so this works like a skip.

### 4.2 Cube-face mask and rects (+0x2c, +0x30)

Bit f of +0x2c selects face f (§2.1). For each set bit, in face order, one rect `(x0,y0,x1,y1)`
in 512×512 face pixels follows at +0x30. `FreeLook_FixupObjects` turns the four corners into
3D points on the half-size-256 cube (face 0: `(x−256, 256−y, 256)`, face 1: `(−256, 256−y, x−256)`,
face 2: `(256−x, 256−y, −256)`, face 3: `(256, 256−y, 256−x)`, face 4: `(x−256, −256, 256−y)`,
face 5: `(x−256, 256, y−256)`). It projects them with 0x40b4d0 (320×200), and the screen bbox of
the visible corners becomes the record's hit box. A sprite that crosses a cube edge has two
rects (e.g. mask 0x6, 0x9, 0x18). `Enemy_Update` uses `+0x2c >> 6` in type 6 (meaning open,
combat-side). 0x405b30 maps a face point to the screen the same way, at half-size 25600 (callers in
the enemy-fire code).

### 4.3 Hotspots and path choice

The pick does not use sprites. It compares the **view centre** yaw/pitch with table44 look windows
(`Hotspot_Pick` 0x40df80; yaw window may wrap, both ends inclusive; pitch window y0..y1). The
first matching record with flag 0x80 wins. In LEVEL00, record i is enabled only if i = 0, i = 15
or i < HighestLevel (0x4215ac). From segment frame `choice_from` (0x5c), fire (buttons & 3) confirms
the pick through `Branch_PickWithCheat` 0x410a90 (`Player_Update` 0x410740, case 6). The pick is
stored in 0x4212a4, and the camera then turns to the target's exit yaw/pitch (§5.3). Without a pick,
`Branch_AutoPick` 0x40e7f0 runs once (while 0x4212ac = 0). It rolls the 0x200 random records
(`rand()*100/0x7fff < chance`, the last success wins) or falls back to `next`, and stores the result
in 0x4212a8 as the auto-turn target. Debug key 'N' disables this and 'Y' forces the first record.
See SCENE_GRAPH §5.

## 5. Camera control (`Player_Update` 0x410740 case 6)

Per frame: `Branch_AutoPick` (first time only) → control mode by byte 0x21 → `View_Clamp6`
0x410a10 (pitch clamped to **±0x270 = ±90°**, yaw wrapped to 0..0x9bf) → pick check → weapon.
`Input_Poll` 0x40ec10 places the crosshair at the **absolute** cursor position: clamped to the
desktop, `y·5/6`, halved on a 640-wide desktop, Y optionally inverted (0x421564). This gives
a 320×200 position `(cx, cy)` at `g_view+0x1d0/0x1d4`. Joystick input is mapped linearly instead.
`View_Set` 0x40e1d0 resets the rate state and sets the max yaw rate 0x421288 = 80.

### 5.1 Mode 1 (LEVEL00 hub), `FreeLook_Control1` 0x40edd0 — H

```
if pick < 0:
    if autoTarget > 0 and seg[autoTarget].type != 6:  → linear auto-turn (below), using framesLeft
    else:
        yaw   += clamp((cx - 160) / 2, ±80)        // integer division (sar), units/frame
        pitch += (100 - cy) / 2
        // the code then divides yawRate by max(cos(...),1.0); the condition is inverted, so the
        // divisor is always 1.0 (no pole correction)
        // over-the-pole code: pitch 693..832 → roll = 9*pitch − 6237, ≥ 832 → flip. This is dead
        // because View_Clamp keeps pitch ≤ 624 and one step is ≤ 50
```

The hub's `next` is itself (type 6), so the hub never auto-turns. It loops until a pick is made.

### 5.2 Mode 2 (LEVEL01 …), `FreeLook_Control2` 0x40fc10 — core H, constants from disassembly

The function first calls the planner `0x40f1c0(4.0)` (§5.3). If the planner returns nonzero, it is
driving the camera and this frame's input is ignored. Otherwise it works in **degrees** (×360/2496
in, ×6.9333 = 2496/360 out, truncated):

```
r      = (cx - 160) / 27.0                       // yaw rate, deg/frame (const 0x420220)
yaw   += r ; wrap to [0,360)
pitch += (100 - cy) / 27   (integer division)   ; clamp to [-90, 90]
roll  (bank / bob), state: n=0x4212a0 frames in state, ph=0x421298 phase, k=0x42129c
  if |r| > 0.5 (turning):  if ph > 15 then k = 15 ; ph = 0
      if k: roll += (4r - roll) * (15 - k) / 15 ; k--
      else: (first frame: save roll0) n++; roll = n < 16 ? roll + (4r - roll0)/16 : 4r
  else (centred):          if ph == 0: ph = trunc(asin?/acos?(roll)·57.3)   (0x418aec; L)
      ph += 3 ; (first frame: save roll0) n++
      roll = n < 16 ? roll + (4r - roll0)/16
                    : cos(ph°) * (ph >= 30 ? 5.0 : ph/6)     // idle rocking, ±5°, period 120 frames
```

So: max turn 160/27 ≈ 5.9°/frame (89°/s). The roll banks to 4 × the yaw rate, and when the
mouse is centred the camera rocks gently (a boat). **M** for the exact easing; the shape is **H**.

### 5.3 Auto-turn and pick turn

* **Modes 1 / 0 (0x40edd0, 0x410420), linear. H:** when a pick is confirmed: target =
  `seg[pick].exit_yaw/exit_pitch` (0x1c/0x1e). The yaw delta is wrapped to ±1248. `N = framesLeft % 15`,
  plus 15 if N < 5 (so 5..19). Each frame, `yaw = tgt − dYaw·cnt/N` and `pitch = tgt − dPitch·cnt/N`, where
  `cnt` counts down from N by the frames elapsed. Once `cnt < 15`, `g_branchReq` 0x42a36c = pick
  is set (once). With no pick but an auto-target of a different type, the lerp uses the frames left
  in the segment as N (arrives exactly at the end). Mode 0 also clamps pitch to ±0x1c2 (±65°).
* **Modes 2/3 planner `0x40f1c0(gain)`. M/L:** in degrees, with target exit yaw/pitch/**roll**
  (0x1c/0x1e/0x48). With a pick: `n = framesLeft % 5 + 5` frames of constant per-frame deltas, and
  `g_branchReq` is set immediately. Without one, and if the target (`autoTarget`, else `next`) is not
  type 6: it plans a smooth turn with an acceleration ramp from the table {3,6,10,15,21,28,36,45,
  55,66,78} (cumulative degrees after 2..12 frames, i.e. 1°/frame² steps). It keeps returning 0
  (free control) until the frames left are just enough to reach the target by the segment's end,
  and then takes over. The exit roll is scaled by 0.25 (const 0x420248). The use of the double
  argument (4.0 in mode 2, 0.5 in mode 3) and the exact staging (0x4285c0 per-frame table,
  0x4287b4 flags) were not traced.
* **Mode 3, 0x4100f0. M/L:** like mode 2 with rate `(cx−160)/16`° and pitch `(100−cy)/16`°,
  clamped to [−50°, 60°]. Yaw is incremented by `trunc(r / max(cos pitch, 0.5))` (whole degrees,
  with pole compensation), and the roll banks with r (sign depends on pitch).
* Mode 4 (0x40ff60) writes yaw/pitch/roll from stored doubles (scripted). It is unused.

## 6. Timing and the segment lifecycle

* A type-6 segment lasts `count` groups × 1/3 s. Like every segment it is clocked by the streamed
  audio (8 sectors = 14 700 bytes per group). `Play_Chunk` runs 5 frames per group at 15 fps, with
  the DirectSound play-cursor correction and frame dropping from ENGINE_MAP §2. A dropped frame still
  runs `FreeLook_Frame` with draw = 0: kind-1 paint happens, render and kind-2 do not. **H**
* Sequence: preceding type 0/1 segment → last 20 groups via 0x407a70 (video plus one tile decode per
  frame) → `Play_SceneLoop` sets view 0x80/0x3000, then 0x407bf0 plays type-6 entries 0..1 from the
  prefetch. On its first group it calls `Pano_SceneSetup` 0x412780 (sets `g_texBuf` 0x428a98 =
  0x42a370) and `View_Set(entryYaw, entryPitch, entryRoll)` from the previous record's
  0x18/0x1a/0x46. Then `Play_PanoChunks` 0x4076c0 plays the rest. The final 3 groups go through
  `Play_PanoHold` 0x4077d0, which splices the successor's prefetched head, unless the successor is
  type 5. **H** (sequence), **M** (Hold splice details).
* Entry continuity: LEVEL01 seg 5 ends at yaw 1164, pitch 0, roll 104, and the first free-look frame
  matches the last video frame pixel for pixel (§8). The authoring tool rendered the cube from the
  rail camera's final pose.
* What virtual groups carry: ordinary type-6 group data (audio + 5 FG blocks), assembled from carry
  sectors. They contain no textures. **H**
* Hub (LEVEL00 seg 1, 44 groups, `next` = 1): it plays to its end and restarts itself. The texture
  and the painted badges persist, because the strip is never cleared. How a type-6 → same type-6
  restart re-reads entries 0..2 was not traced (**L**).

## 7. Constants

| const | value | where |
|---|---|---|
| angle unit | 2496 (0x9c0) per turn; ×0.144231 → deg (0x420008); deg → units ×6.93333 (0x420250) | |
| pitch clamp | ±0x270 (±90°) | 0x410a10 |
| half-FOV | i16 seg 0x4a · π/92160 rad | 0x420020 |
| screen | 320×200, rasteriser coords 28.4 (0x1400 × 0xc80) | 0x405710 |
| guard band | x ∈ [−0x2800, 0x3c00], y ∈ [−0x2800, 0x2800] (28.4) | 0x405710 |
| near reject | view z < 1.0 | 0x40b4d0 |
| cube | half-size 2048, step 1024, 5×5 vertices/face | 0x405310 |
| tile | 128×128, UV 0..127.0, 0x4000·bpp bytes | 0x405710 |
| strip | 128 × 0x3000 | 0x406020 |
| preload | 20 groups (0x4210ec), 0x1800 bytes/tile (0x4210dc), 96 tiles | 0x407a70 |
| sub-span | 8 px, 16.16 u/v (65536.0 / 8192.0 at 0x42131c / 0x421320) | 0x43f000 |
| mode-1 rates | yaw (cx−160)/2 (clamp ±80), pitch (100−cy)/2, units/frame | 0x40edd0 |
| mode-2 rates | yaw (cx−160)/27 °, pitch (100−cy)/27 ° (int), bank 4·rate, bob ±5° / 120 frames | 0x40fc10 |

## 8. Prototype and checks (`tools/freelook.py`)

`python3 tools/freelook.py FILE SEG OUTDIR [yaw,pitch[,roll] ...] [--fg K [--highest N]] [--tiles]`
collects and decodes the 96 tiles (ffmpeg), optionally paints the FG records, and ray-casts 320×200
views. Images are in `out/freelook_check/`:

* `LEVEL00_s1_faces.png`, `LEVEL01_s6_faces.png`: the six faces, flipped upright for viewing
  (top row: faces 0, 1, 2; bottom row: 3, 4, 5). The globe faces form a correct world map, so the
  tile order, face orientation and Y-down convention all agree.
* `ref_vs_render_f010/071/110/053/098.png`: frames from `out/ref/ref_run1.mp4` (top) against
  renders at yaw/pitch/roll found by brute-force search (bottom). Shapes, scale (FOV) and the hub
  badges ("01" and the EOA badge, HighestLevel = 1 lock) match. f053 → (544, 368), f098 → (1596, 88).
* `LEVEL01_s5_last_vs_s6_entry.png`: the last video frame of seg 5 (top) against the render at the
  entry pose (1164, 0, 104) with group-0 FG enemies (bottom). The background is identical; only the
  sprites are added. This confirms the roll sign, the FOV and the projection.

## 9. Unknowns

* Exact mode-2/3 planner behaviour (0x40f1c0) and the function at 0x418aec (asin/acos?) used to seed
  the bob phase. Mode 3 roll-sign details.
* `Enemy_Update`'s use of `+0x2c >> 6` and how type-6 enemy fire and effects (kind 4, drawn in screen
  space after rendering) get screen coordinates. Left to the combat spec.
* The exact restore order of kind-2 save-under runs in the strip relative to the next frame's tile
  decode (irrelevant in practice, because tiles are not decoded during type 6).
* The hub self-loop restart path (§6), and whether 16-bit mode decodes the tiles as 555 or 565
  (follows the display format; not checked).
* GAMEPLAY_DATA §3.3 still calls the type-6 draw rect "not decoded". §4.1 above resolves it.
