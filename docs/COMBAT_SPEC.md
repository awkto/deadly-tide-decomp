# Deadly Tide combat and player logic: reimplementation spec

This is a specification for reimplementing the gameplay layer of `DTPLAYER.EXE` (input, crosshair,
weapons, hit testing, actor states, enemy fire, health, HUD, SFX triggers, branch inputs, save data)
in C. It is written against the static decompile. Addresses are virtual addresses in `DTPLAYER.EXE`.
Names follow `decomp/symbols.tsv` and `docs/ENGINE_MAP.md`. File formats, segment records, FG
records and table44 are in `docs/GAMEPLAY_DATA.md` and `docs/SCENE_GRAPH.md`; this document only
repeats what is needed to follow the logic. Rendering of type-6 scenes and panorama projection are
out of scope.

Confidence tags: **H** = read the code (and the disassembly where Ghidra's struct indexing was in
doubt), **M** = strong inference, **L** = guess. "Checked in data" means the claim was tested on the
shipped `.L96` files (LEVEL01, 05, 10 and 14 class tables, and segment fields of every level).

## 0. Corrections to the existing docs

| Topic | Existing claim | What the code says |
|---|---|---|
| Difficulty labels | ENGINE_MAP §1: registry 1 → index 0 = easy | The mapping is right (1 → 0, 2 → 2, anything else → 1; the default registry value is 1), but **index 1 is the easiest**: it has the highest regen, the lowest hit % and spawn %, and the only index without the extra gun heat (§9). So index 0 = normal, 1 = easy, 2 = hard (M). |
| table54 +0x1f | SCENE_GRAPH: "spawn delay" | It is **extra hit points**. Spawn sets state = −0x40 − hp, and each hit adds 1 (H). |
| Segment 0x10, 0x11 | "no reader found" | SFX ids of the primary gun shot (0x10) and the ACT launch (0x11), read by `Sfx_WeaponFire` 0x417ca0 (H). |
| Segment 0x14 | "no reader found" | SFX id played on player death, by `Sfx_PlayerDeath` 0x417d40 (H). |
| Segment 0x44 | "aim" flag | Also the **index of the HUD sprite set** that the gauges use (`Hud_DrawGauges*`), H. |
| Actor state range | "−0x40…−1 countdown after death" | Right, but the countdown length is set by the **gun's** segment byte 0x56: a kill sets state = −gun_param. So the countdown is 0x56 frames (5 or 7 in the data), not 0x40 frames. −0x40 itself only occurs from the linked-actor rule (§4.3). |
| `Sfx_Play` 0x414d10 | (id, pan, vol) | Six arguments: (id, pan, volume, frequency, loop, play). Ghidra truncated it (H, from callers' pushes). |
| `Fx_Flash(type, shake)` | "type" | The first argument is a **countdown**. Each present takes one tint level and decrements it, so `Fx_Flash(2, …)` is 2 frames of tint and `Fx_Flash(3, …)` is 3 (§5.4). |

## 1. Data structures

The structures are written as C, with field offsets. `rec` is a 100-byte segment record and `fg` is
a 0x58-byte FG object record.

```c
/* Player / view state, g_view at 0x42a150 (only the gameplay part). */
typedef struct {
    int32_t w, h;            /* +0x00/+0x04  320, 200 */
    /* +0x18..+0x30 pano limits / yaw / pitch / roll: renderer spec */
    /* +0x34 JOYCAPS: wXmin +0x58, wXmax +0x5c, wYmin +0x60, wYmax +0x64 */
    /* +0x1c8 JOYINFOEX overlays the next fields: dwXpos=+0x1d0, dwYpos=+0x1d4, dwButtons=+0x1e8 */
    int32_t xhair_x;         /* +0x1d0  0..319, top-down screen space */
    int32_t xhair_y;         /* +0x1d4  0..199, top-down */
    uint32_t buttons;        /* +0x1e8  bit0 fire, bit1 alt (path pick), bit2 ACT */
    int32_t health;          /* +0x1fc  0..10000, can go negative */
    int32_t kills;           /* +0x200  targets destroyed by the player */
    int32_t aim_x, aim_y;    /* +0x204/+0x208  crosshair latched at the last shot */
} Player;

/* Actor instance, table28 (g_actors 0x4213c0), persistent for the whole level. */
typedef struct {
    uint8_t cls[3];          /* +0  class index per difficulty index */
    int8_t  state;           /* +3  see §4.1 */
    void   *expl_set;        /* +4  runtime: explosion sprite set picked at death */
    int32_t link_id;         /* +8  linked actor (0 = none) */
    int32_t link_mode;       /* +0xc 0 = child needs a live parent, 1 = child damages parent */
    float   dist2;           /* +0x10 runtime: |cam - pos|^2, written by Obj_ProjectAll */
    /* +0x14..+0x27: not read by the combat code we traced */
} Actor;

/* Actor class, table54 (g_actorClasses 0x4213bc). Bytes unless noted. */
typedef struct {
    uint8_t b0;              /* +0x00 not read by combat code */
    uint8_t box;             /* +0x01 bits0-5 bbox grow code, 0x40 draw bracket, 0x80 pixel-exact hit */
    uint8_t dmg;             /* +0x02 max damage per enemy shot (units of 1/100 health) */
    uint8_t heavy;           /* +0x03 damage above this = "heavy hit" (flash + heavy hurt SFX) */
    uint8_t _4;
    uint8_t n_expl;          /* +0x05 number of explosion set candidates */
    uint8_t expl[?];         /* +0x06.. explosion sprite-set indices (list 0); up to 0x14 bytes */
    uint8_t min_size;        /* +0x1a min on-screen size in px to fire/bracket; 0xfe = cannot be hit
                                by the gun; 0xff = never targetable, never fires */
    uint8_t spawn_pct;       /* +0x1b spawn chance % */
    uint8_t fire_pct;        /* +0x1c random fire chance % per frame */
    uint8_t hit_pct;         /* +0x1d hit chance % of a random shot */
    uint8_t dmg_min;         /* +0x1e damage must exceed this to count (also the "light hurt" level) */
    uint8_t hp;              /* +0x1f extra hit points (hits to kill = hp) */
    uint8_t sfx_explode[5];  /* +0x20 */
    uint8_t sfx_hurt_lt[3];  /* +0x25 player hurt, light */
    uint8_t sfx_hurt_hv[3];  /* +0x28 player hurt, heavy (falls back to light) */
    uint8_t sfx_hit[2];      /* +0x2b enemy hit but not killed */
    uint8_t sfx_near[3];     /* +0x2d approach sound, growing enemy */
    uint8_t sfx_big[2];      /* +0x30 approach sound, big enemy */
    uint8_t sfx_fire[2];     /* +0x32 enemy shot */
    int8_t  kill_branch;     /* +0x34 segment to branch to when this actor is removed, -1 none */
} ActorClass;                /* 0x54 bytes; 0xff in an SFX slot = empty */

/* Per-actor history, g_actorPrev 0x423018, 16 bytes per actor id. */
typedef struct {
    int16_t x0, y0;          /* +0  bbox x0,y0 last frame (written by Enemy_Update) */
    int16_t w, h;            /* +4  draw w,h last frame */
    int32_t frame_seen;      /* +8  g_frameNo when a record for this actor was last processed */
    float   dist2;           /* +0xc last frame's dist2 */
} ActorPrev;

/* Player shot slot, g_shots 0x429660, 8 x 48 bytes; g_actShot 0x429620 has the same layout. */
typedef struct {
    int32_t x, y;            /* +0  screen point (top-down) of the shot's end */
    int32_t hit;             /* +8  1 = hit an actor */
    int32_t active;          /* +0xc 1 = fired this slot */
    int32_t actor;           /* +0x10 actor hit, -1 for a miss */
    int32_t _14;
    double  world[3];        /* +0x18 world point of a miss (depth 10000), reprojected per frame */
} Shot;

/* Weapon globals (initial .data values in brackets). */
int g_heat     /*0x4214bc [240]*/, g_heatMax /*0x4214c0 [240]*/, g_overheated /*0x4214c4 [0]*/;
int g_shotIdx  /*0x4214cc [-1]*/;  uint32_t g_shotHist /*0x4214b8: bit (n-1) = shot this frame*/;
int g_shotsFired /*0x4214c8*/, g_shotLimit /*0x429650, set to 0x7fffffff*/;
int g_rapidArmed /*0x4214e4 [1], never written*/, g_semiArmed /*0x4214e8 [1]*/;
int g_chargeArmed /*0x4214fc [1]*/, g_chargeGap /*0x421500 [1]*/, g_chargeCount /*0x421504 [0]*/;
uint32_t g_actAnim /*0x4214d0*/; int g_actAmmo /*0x4214d4*/, g_actReq /*0x421574*/;
int g_actChainN /*0x4214d8*/, g_actChainHit /*0x4214dc*/, g_actChainTimer /*0x4214e0*/;
void *g_actChain[] /*0x429100*/;
```

Sprite-set entries (0x110 bytes each, `Sprites_Load` 0x4022b0) are used as: +0xc frame count,
+0x10 flag (nonzero: draw the gun flash as a textured line, not a sprite), +0x14 frame-descriptor
pointers, and, for list 3 only, a step table at +0x90. Lists: HUD 0x437a60 + i·0x110, crosshair
0x43a930 + i·0x110, gun flash / ACT 0x43af80 + i·0x110 (entry 0 = gun flash for `seg[0x59]`=0, entry
1 = ACT), enemy streaks 0x439500 (one entry), explosions 0x437fb0 + i·0x110. (The HUD/crosshair
bases here are one entry lower than the list bases in GAMEPLAY_DATA §5, because the code indexes them
by a 1-based segment byte: L.)

## 2. Per-tick order (one drawn video frame)

`Play_Chunk` 0x4067b0 runs this for each of the 5 frames in a group (15 fps). Dropped frames call
`Frame_Process(..., draw=0)` only, which decodes but skips **all** gameplay (no enemy fire, regen,
HUD, input or weapons). A reimplementation that never drops frames can ignore that.

```
Frame_Process(hdr, objs, draw=1)                                   0x406e90
  Obj_BlockSize; Cam_UpdateFromHeader(hdr)
  switch seg.type:
   0,5: copy hdr; Frame_DecodeVideo -> [Obj_ProjectAll, Actor_UpdateStates, decode, draw kinds 1|2, draw kind 4]
        Enemy_Update(objs, hdr)        (returns at once for 0/5, only latches the camera)
        Player_Regen()                 (no health > 0 check here)
   1:   copy hdr; Frame_DecodeVideo (as above)
        if (hdr.flags & 1):  Enemy_Update(objs, hdr); Hud_Draw(objs, 0, hdr.flags & 2)
   2,6: Pano_Frame / FreeLook_Frame (renderer; they also run the actor fixups)
        Enemy_Update(objs, copy of last type-1 header); Hud_Draw(objs, 0, 1)
DD_Present                    (consumes the flash and shake counters, §5.4)
Spr_RestoreBackground
Player_Update(hdr, objs, framesLeftInSegment)                      0x410740
death timer (§5.3); g_frameNo++, g_sceneFrame++
```

Two consequences matter for a reimplementation:
* Hits from `Player_Update` change actor states **after** this frame was drawn. The explosion first
  appears in the next frame's `Actor_UpdateStates`.
* `Hud_Draw` holds the **death check and the regen** on combat frames, so both run only on type-1
  frames with flag bit 0 and on every type-2/6 frame.

## 3. Input and crosshair

### 3.1 `Input_Poll` 0x40ec10 (H)

It is called only from `Player_Update` on type-1 aim frames, and from the type-2/6 look controls
(renderer spec). Between calls the crosshair stays where it was.

**Mouse** (`g_useJoystick` = 0). Absolute cursor, no sensitivity setting, no acceleration:
1. `GetCursorPos`, clamp to [0, screenW−1] × [0, screenH−1].
2. If `FlipControl`: y = screenH − 1 − y.
3. y = y·5/6 (integer).
4. If screenW = 640: x /= 2, y /= 2.
5. xhair = (x, y); buttons = `g_mouseButtons` (0x4287d8, written by the WndProc through
   `Input_SetButtons` 0x410ad0).

In 640×480 and 320×240 modes this maps the screen onto 0..319 × 0..199. In 320×200 mode,
y·5/6 tops out at 165, so the bottom 34 rows cannot be reached. That is a quirk of the original. A
port should map the window onto 320×200 directly. The WndProc (0x416fb0, disassembled) calls
`Input_SetButtons` with 1 on left-button-down, 2 on right-button-down, and 0 on **either** button-up, so left = bit 0
(fire) and right = bit 1 (alt / path pick), exactly as ours, but the value is assigned, not OR-ed: releasing one
button while holding the other clears both (H). There is no middle-button or double-click handling (double-clicks go to
`DefWindowProc`). Space sets `g_actReq` directly (SCENE_GRAPH §4).

**Joystick.** `joyGetPosEx(0)` with `dwFlags = 0x83` (X, Y, buttons) writes the raw X and Y straight
into xhair_x/xhair_y and the buttons into +0x1e8 (the JOYINFOEX overlays the player struct). If the
X and Y ranges are non-empty:
* x = (raw_x − Xmin)·320 / (Xmax − Xmin + 1)
* y = (raw_y − Ymin)·200 / (Ymax − Ymin + 1)
* if **not** FlipControl: y = 199 − y

So the joystick's default is flight-style (push forward → crosshair goes down), and FlipControl
makes it direct. For the mouse it is the other way round. Joystick button 1 is fire, 2 is alt and 3
is ACT (the dwButtons bit order).

**ACT request.** If buttons & 4 and not buttons & 3: `g_actReq` = 1.

### 3.2 Where the crosshair is live (H)

| Scene type | Input read | Fire allowed | Crosshair drawn |
|---|---|---|---|
| 0, 5 | no; `Weapon_Reset` every frame | no | no |
| 1 | only if hdr.flags bit 0 **and** seg[0x44] ≠ 0; otherwise falls through to the type-0 path (weapon reset) | hdr.flags bit 1 **and** seg[0x45] ≠ 0 | if style ≠ 0 and hdr.flags bit 1 |
| 2, 6 | every frame (look control) | every frame unless a path pick is pending | if style ≠ 0 |

Crosshair style = seg[0x45]. From segment frame seg[0x5c] onwards (when 0x5c ≠ 0 and seg[0x5b] ≠ 0)
it is seg[0x5b], and "on target" means "a look window is under the view" (`Hotspot_Pick` ≠ −1).

Because `Weapon_Reset` runs on every non-aim type-1 frame, **the gun heat refills to full whenever
the aim flag drops** (H).

### 3.3 Crosshair sprite, `Hud_DrawCrosshair` 0x40a340 (H)

* The set is crosshair list entry `style` (n frames). The first n/2 frames are the normal animation;
  the second n/2 are the "on target" animation.
* A global counter (0x4211d0) advances 1 per call and wraps at n/2.
* frame = counter + (onTarget ? n/2 : 0), drawn centred on (xhair_x, xhair_y) in top-down
  coordinates.
* onTarget (type 1/2/6 outside the choice window) = the crosshair lies inside the bbox of any record
  that passes `Obj_IsTargetable(rec, 0)` and has a nonzero bbox.

## 4. Actors

### 4.1 State byte (table28 +3), H

| value | meaning | targetable / fires | drawn |
|---|---|---|---|
| < −0x40 | alive; remaining hits = −0x40 − state | yes (if the other tests pass) | stream sprite |
| −0x40 … −1 | killed, waiting to explode (+1 per frame) | no | stream sprite |
| 0 … 0x7e | exploding; value = explosion frame index | no | explosion frame (kind 4) |
| 0x7f | gone | no | not drawn (pixel ptr zeroed) |

This answers the "state < −0x40" question in SCENE_GRAPH.md: "alive" means **strictly below −0x40**.
Everything (`Obj_IsTargetable` 0x4021a0, `Branch_Resolve` 0x40e380) uses `< -0x40`. The data has
hp ≥ 1 for every class that can be hit (checked in data), so a fresh actor starts at ≤ −0x41.

**Reset (`Game_ResetForScene` 0x405f30, at checkpoints).** For every actor, with
c = class[actor.cls[d]]: if rand()·100/0x7fff > c.spawn_pct, state = 0x7f; otherwise
state = −0x40 − c.hp. It also: clears the SFX loop slots and the health-warning state
(`Sfx_ResetLoops`), clears enemy shots, `Weapon_SetAmmo(ammo, 0x7fffffff)`, health = 10000,
kills = 0, HUD message 0, the death latch = 0 and the "dead" flag = 0.

Records map to actors only through `fg.id`. An actor exists for the whole level; streamed records
are just "this actor is here this frame". The engine never creates actors from records.

### 4.2 Targetability, `Obj_IsTargetable(rec, mode)` 0x4021a0 (H)

It is always true that state < −0x40, `fg.flags` bit 31 is set, and the kind is 2 (or 1, in type-1
scenes only). Then, with s = c.min_size and the size test `(fg.w ≥ s || fg.h ≥ s)`:

| mode | used by | extra test |
|---|---|---|
| 0 | brackets, on-target crosshair | s < 0xfe and size test |
| 1 | gun hit scan | s < 0xfe |
| 2 | ACT nearest target | s < 0xfe and size test |
| 3 | enemy fire | s ≠ 0xff and size test |

All the per-record loops (`Actor_UpdateStates`, `Enemy_Update`, `Weapon_HitScan`, `Hud_Draw`) skip
kind 8, and they skip kind 1 outside type 1.

### 4.3 `Actor_UpdateStates(cam, objs)` 0x403a80, called before the video is drawn (H)

Let `d` = difficulty index, `c` = class[actor.cls[d]], `seen` = (prev[id].frame_seen == g_frameNo − 1).

**Pass 1, over the records (skipping the kinds above):**
1. **Respawn roll.** If state ≥ −0x40 (killed, exploding or gone) and `fg.flags & 0x100`: if
   rand()·100/0x7fff ≤ c.spawn_pct, state = −0x40 − c.hp.
2. **If state == 0** (the first explosion frame is about to play) and link_id ≠ 0 and link_mode == 1:
   the parent takes one hit. If the parent's state is < 0, it is incremented. If that brings the
   parent to exactly −0x40: if link_id == 250 (0xfa), set g_branchReq = class[parent].kill_branch
   (if ≠ −1) and leave the parent at −0x40; otherwise parent.state = 0 (it explodes at once). This
   kill is **not** counted in `kills`.
3. **Else, if alive** and not `seen` (the actor's first frame on screen), link_id ≠ 0, link_mode == 0
   and the parent is not alive: state = 0x7f (orphans vanish).

**Pass 2, over all records:**
* Kind 8, or kind 1 outside type 1: just rebase the pixel pointer.
* If `fg.flags & 0x40`: state = 0x7f (scripted removal).
* state == 0x7f: pixel pointer = NULL (not drawn).
* state < −0x40: rebase. −0x40 ≤ state < 0: state++, rebase (the sprite stays visible).
* state ≥ 0 (exploding):
  * If c.min_size == 0xff, or (`seen` and the bbox is nonzero): frame = expl_set.frames[state].
    * If state == 0: `Sfx_EnemyExplode` and `Fx_Flash(2, 10)`.
    * If frame == NULL (past the end of the animation): **remove** (below).
    * Otherwise, replace the record's sprite. The draw rect is re-centred on the old draw rect
      (type 6: on the bbox centre). Pixel pointer, size, w, h and tint (+0x48) come from the frame,
      kind = 4, and state++.
  * Otherwise (the actor left the stream or the screen mid-explosion): **remove**.
  * **Remove** = pixel pointer NULL, state = 0x7f, g_branchReq = c.kill_branch if ≠ −1, and zero
    prev[id].
* For −0x41 < state < 0x7f (dying or exploding): every player shot slot with `actor == id` gets
  (x, y) = the bbox centre. The tracer follows the falling target. In type 2 this is run through
  `Pano_PanoToScreen`.
* prev[id].frame_seen = g_frameNo.

Kill-triggered branches therefore fire when the explosion **ends** (or is cut short), not at the
moment of the hit.

### 4.4 Explosion choice, `Actor_PickExplosion` 0x403910 (H)

With size = max(fg.w, fg.h) of the draw rect and candidates `c.expl[0..n_expl-1]` (indices into
explosion list 0, whose +4 field is the size hint):
1. Collect up to 8 sets with size < hint < 2·size. If there is more than one, pick one with
   `rand() % count`.
2. If there are none: take the set with the smallest hint that is > size.
3. If there are still none: take the largest hint.

The result is stored in actor.expl_set.

## 5. Player weapons

### 5.1 `Player_Update(hdr, objs, framesLeft)` 0x410740 (H)

```
pickTimer stuff: if framesLeft > lastFramesLeft (new segment) or pick == curSeg: clear look-pick state
view offset (0x42128c/0x421290) = 0
switch seg.type
 1: if (hdr.flags&1) && seg[0x44]: Input_Poll; if (hdr.flags&2) && seg[0x45]: Weapon_Fire(hdr,objs); break
    else fall through
 0,5: Weapon_Reset()
 2: Pano_LookControl; View_ClampPano; goto pick
 6: if !picked: Branch_AutoPick; look control by seg[0x21]; View_ClampFreeLook
 pick: if !picked && (buttons&3) && g_sceneFrame > seg[0x5c]:
          p = Hotspot_Pick (via 0x410a90; Ctrl held in Level00 sets 0x4213ac)
          if p >= 0: remember it, picked = 1, return        (this press fires nothing)
       if no pick pending: Weapon_Fire(hdr, objs)
```

### 5.2 `Weapon_Fire` 0x416460 (H)

1. `Shot_Update` on the 8 shot slots and on the ACT shot. For active slots that missed, the world
   point is reprojected to the screen each frame: type 1 through the current view, type 2 through
   the panorama mapping, other types with y flipped (199 − y).
2. `ACT_Fire` (§5.6).
3. n = frame count of gun-flash set `seg[0x59]`. If n == 0, there is no primary gun.
4. Dispatch on seg[0x12], always passing gp = (int8) seg[0x56] as the **kill delay**:

| seg[0x12] | routine | behaviour | used in |
|---|---|---|---|
| 0 | `Gun_Rapid(objs, 7, n, gp)` 0x415af0 | full-auto every frame, heat | L01, 05, 06, 09–13 |
| 1, 2, 4 | `Gun_Burst(objs, 1, n, gp)` 0x415dc0 | semi-auto: one shot per press | 2 in L08 only |
| 3 | `Gun_Charge(objs, n, gp)` 0x416210 | every 2nd frame, 7-shot burst cap | L02–04, 07, 14, 15 |
| 5 | `Gun_Pulse(objs, n, gp)` 0x415fe0 | alternate frames, 7 shots per press | unused |

Common shot step (all routines): `g_shotIdx = (g_shotIdx + 1) % n`; `g_shotHist >>= 1`. On a shot:
* `Sfx_WeaponFire(seg, 1)`;
* g_shotsFired++;
* aim_x/aim_y = xhair;
* `Aim_GetRay` and then `Weapon_HitScan(…, gp)`;
* `g_shotHist |= 1 << (n−1)`;
* `shots[idx].active` = 1;
* in demo mode, firing clears god mode (`g_attract` = 0) and sets `g_demoInterrupted`.

Without a shot: `shots[idx].active = shots[idx].hit = 0`.

**Sticky tracer (all routines, cosmetic).** If n ≠ 1, this shot missed, the previous slot hit, and
that actor is still alive, or exploding with more than n frames of animation left
(`frames/2 − state > n`), the previous slot's whole record is copied into this one. Rapid fire then
keeps streaming at a target that was just hit.

### 5.3 Heat model, `Gun_Rapid` (weapon 0), H

The constants are g_heatMax = 240 and the "LASER OVERHEAT" lockout value −120.

```
heat += 1; if heat == 0: heat = heatMax;  heat = min(heat, heatMax)        // passive cooling
if overheated && heat >= 0: Hud_SetMessage(2, 30, 30, 0); Sfx_Overheat(seg, 1) /*seg[0x50]*/; overheated = 0
advance slot
canFire = g_rapidArmed(=1) && (buttons & 1) && heat >= 2
if !canFire: heat += 1 (clamp)                                             // extra cooling when idle
else:
   shoot (common step)
   if (hist & (7 << (n-3))) == (7 << (n-3)): heat -= 4                    // 3 consecutive frames firing
   if difficulty index != 1: heat -= 2
   if heat < 1: heat = -120; Hud_SetMessage(1, -1000, 5, 5); Sfx_Overheat(seg, 0) /*seg[0x4f]*/; overheated = 1
```

Resulting rates at 15 fps: idle cooling +2/frame. Sustained fire costs a net −3/frame on easy
(index 1) and −5/frame on the other two, but the first two shots of a burst cost only the difficulty
part. From full heat, continuous fire overheats after about 80 frames (5.3 s) on easy and about 48
frames (3.2 s) otherwise. The lockout runs from −120 up to 0 at +2/frame, so 60 frames (4 s), and
then **jumps straight to full** (240). Message 1 has duration −1000, so it effectively stays up until
message 2 replaces it.

**`Gun_Charge` (weapon 3).** It fires when charge armed && gap ≥ 1 && fire is held; then gap = 0 and
count++. Otherwise gap++. While held, that gives one shot every 2 frames (7.5/s). At count == 7:
`Hud_SetMessage(1, 20, 2, 3)`, `Sfx_Overheat(seg, 0)`, armed = 0 and gap = −30. When fire is
released: armed = 1, count = 0 and gap++ (so the gap recovers at 2/frame while released). Tapping
avoids the lockout. There is no heat bar, because the gauge only draws heat for weapon 0. H.

**`Gun_Burst` (weapons 1, 2, 4).** It fires only if armed && fire && `g_shotHist == 0` (no shot in
the last n frames); then armed = 0. It re-arms when fire is released **and** the history is empty.
So the minimum shot interval is n frames. H.

`Weapon_Reset` 0x4165b0: hist = 0, actAnim = 0, heat = heatMax = 240. `Weapon_SetAmmo(a, lim)`
0x4165d0: Weapon_Reset; actAmmo = a; shotLimit = lim; actReq = 0; shotsFired = 0.

### 5.4 Gun flash and tracer drawing, `Hud_DrawGunFlash` 0x416610 (M)

This is drawn by `Hud_Draw` when g_shotHist ≠ 0. Let set = gun-flash entry seg[0x59] (n frames,
step table S[] at +0x90). Origins, in top-down screen coordinates:

| weapon | origins | thick line |
|---|---|---|
| 0, 1, default | two guns: (0, 180) and a mirrored one at (320, 180) | no |
| 2 | two guns, as above | yes |
| 3, 4, 5 | one gun at x = 160 + (aim_x − 160)·8/10, y = 199 + frame0.h/2 | yes |

For each age k = 0..n−1 with bit (n−1−k) of g_shotHist set, take slot (g_shotIdx − k) mod n and its
target point T = shots[slot].(x, y):
* **Sprite sets** (+0x10 == 0): draw frames[k] centred at O + (T − O)·S[k]/S[n−1]. The second gun
  is mirrored about x = 320.
* **Line sets**: `Fx_DrawTracer` from O + (T − O)·S[k]/S[n] to O + (T − O)·S[k+1]/S[n], textured
  with frames[k].

The step table is S[0] = 0 and S[i] = S[i−1] + 2^(c+1−i) (c = frame count). That is a geometric
approach: the bolt starts as a muzzle flash at the gun and gets smaller and slower as it nears the
target. With 7 frames the bolt takes 7 frames to arrive, which matches the kill delay gp = 7 in
LEVEL01. A shot's hit is already decided on the frame it is fired; only the visual travels.

### 5.5 Hit testing (H)

**`Aim_GetRay` 0x40e250** returns (sx, sy) for the bbox test and (px, py) for the pixel test:
* type 1 (and 0/5): sx = aim_x + offX, sy = aim_y + offY, px = sx, py = seg[0x40] − aim_y − offY
  (bottom-up). off = the view offset (0x42128c/0x421290), which `Player_Update` sets to 0 every
  frame, so it is 0 here.
* type 2: panorama coordinates via `Pano_ScreenToPano`.
* type 6: px = py = 0 (no pixel test).

**`Weapon_HitScan(objs, sx, sy, px, py, gp)` 0x404140.** It walks the records **from last to first**
(topmost drawn first). For each record that passes `Obj_IsTargetable(rec, 1)` and
`Obj_HitTestPoint`:
* state++.
* If still < −0x40: `Sfx_EnemyHit` (a non-lethal hit).
* Otherwise: state = −gp, kills++ and `Actor_PickExplosion`.
* shots[idx] = {x = aim_x, y = aim_y, hit = 1, actor = id}. Return: **one target per shot**.

If nothing is hit, `Shot_Spawn` puts the miss into shots[idx]:
* (x, y) = (sx, sy) and actor = −1.
* The world point is the screen ray unprojected at depth 10000.0 (type 1 view; other types use the
  identity view with y flipped). Type 2 stores pano coordinates.

**`Obj_HitTestPoint(rec, sx, sy, px, py)` 0x403ff0.**
1. Grow the bbox (already flipped to top-down by `Obj_ProjectAll`) by g on each side, with
   g = {1: +1, 2: +2, 3: −1, 4: −2, else 0}[c.box & 0x3f]. If shrinking inverts an axis, collapse it
   to the midpoint.
2. Inclusive test: x0 ≤ sx ≤ x1 and y0 ≤ sy ≤ y1.
3. If c.box & 0x80 and the scene is not type 6, also require `Spr_HitTestPixel(px, py, rle, …,
   fg.x, fg.y, fg.w, fg.h)`.

**`Spr_HitTestPixel` 0x410fa0.** It walks the sprite's RLE (GAMEPLAY_DATA §4) from row fg.y
(bottom-up) to row py, then scans that row. It returns 1 if px falls inside a literal (opaque) run
[x, x + len]. Note the inclusive end: one pixel to the right of a run also counts. Skips are
transparent; a row-advance code ends the row with a miss.

### 5.6 Secondary weapon (ACT), `ACT_Fire` 0x4155b0 (H, chain timing M)

This runs every Weapon_Fire frame, before the gun. Let m = frame count of ACT set (gun-flash list
entry 1, 20 frames in LEVEL01).

```
actAnim >>= 1
aim = xhair; Aim_GetRay
if actReq && actAnim == 0 && actAmmo > 0:
    actAnim = 1 << (m-1); demo: clear god mode
    Sfx_WeaponFire(seg, 2)            /* seg[0x11] */
    Shot_Spawn into actShot (ray at depth 10000); actShot.hit = 0; actShot.active = 1
    actChainN = 0; actAmmo--
actReq = 0
if actAnim == 0x2000: Fx_Flash(0, 20)                                 /* shake only */
if ((actAnim == 0 || actAnim > 0x1ff) && actChainTimer == 0) || actChainHit: actChainHit = 0
else:
    if actChainTimer == 0: actChainTimer = 6
    t = Weapon_NearestTarget(objs, r2 = 0x1900, actShot.x, actShot.y, gp = 1)
    if !t && actChainN > 0: t = Weapon_NearestTarget(..., centre of bbox of actChain[0], ...)
    if t: actChainHit = 1; actChain[actChainN++] = t
if actChainTimer > 0: actChainTimer--
Hud_SetAmmoBlink(actAmmo)
```

**`Weapon_NearestTarget` 0x4042a0.** It looks at records passing `Obj_IsTargetable(rec, 2)` and
takes the one whose bbox centre is nearest to the point; it must be strictly closer than
r² = 6400 (80 px). It applies one hit: state++. If the target is still alive it gets `Sfx_EnemyHit`
and the call returns NULL. Otherwise state = −1 (it explodes on the next frame), kills++, the
explosion is picked, and the record is returned.

In effect, the missile flies for about m − 9 frames. Then, during the last 9 animation frames (and
until the 6-frame timer runs out) it destroys the nearest target within 80 px of the impact point,
one every second frame, re-centring on the first victim when the impact area is empty. Each victim
takes **one** hit.

**`Hud_DrawACT` 0x4169f0.** For each bit k set in actAnim, frame k of the ACT set is drawn centred
with its left edge at x = 160 + (actShot.x − 160)·S[k]/(2^(m+1) − 1) − w/2 and its bottom edge at
y = (100 − actShot.y)·S[k]/(2^(m+1) − 1) (H, read from the disassembly of 0x4169f0 and checked against the
engine's `draw_act`). `S[k]` is the step table at +0x90 of the set (GAMEPLAY_DATA §5), `m` the frame count, and
the y argument goes straight to `Spr_Draw` as a bottom-up row (GAMEPLAY_DATA §1) with **no** h/2 term, unlike x.
The asymmetry is real: the original does not centre the sprite vertically here. The earlier doubt about the y term
is closed.

**Ammo.** It comes from the L96 header +0x88 at level start (0, 3 or 5 per level in the data), or
from the registry `ACTNum` after a save-point restore. It is saved on save-point entry. The registry
caps it at 7.

## 6. Enemy update and enemy fire, `Enemy_Update(objs, hdr)` 0x4043f0

### 6.1 Camera bookkeeping (H; only the streak geometry depends on it)

For type 0/1/5 it latches the camera and look-at from the header (0x423000…), and returns for types
0 and 5. In type 1:
* camDist = |cam − look| is computed once (≈ 200).
* A two-frame state machine estimates the camera velocity as (cam_t − cam_{t−1})·2.0 and the roll
  rate as Δroll·2.
* A **predicted view matrix** is built from cam + v and look + v (`Mat_LookAtB` 0x40ae40). It is used
  only to place enemy streaks.

Other types use the current view matrix (0x437af0).

### 6.2 Per record (skipping kind 8, and kind 1 outside type 1), H

```
dmg = 0; fires = 0
if state < 0:                                       // alive or dying: approach sound
    pan = ((fg.x0 + fg.w/2)*5 - 800)*2              // type 2: yaw-relative instead
    if fg.w >= 21 || fg.h >= 21:                      Sfx_EnemyLoop(c, rec, pan, big=1)
    else if fg.w >= 16 && fg.h >= 16 && fg.w > prev.w && fg.h > prev.h: Sfx_EnemyLoop(c, rec, pan, big=0)
if Obj_IsTargetable(rec, 3):
    if !(fg.flags & 0x80) && !(fg.flags & 0x40000000):          // AI fire
        if rand()*100/0x7fff < c.fire_pct:
            r = rand()*100/0x7fff
            if r < c.hit_pct: hit = 1; fires = 1; dmg = c.dmg * r / c.hit_pct
            if rand() < 0x2aaa: fires = 1                        // 1/3: a visible shot even if it misses
            muzzle = bbox centre
    else:                                                        // scripted shot
        fires = hit = 1
        dmg = (flags & 0x80) ? c.dmg : (rand()*100/0x7fff) * c.dmg / c.hit_pct
        muzzle = ((flags>>18)&0xfff, 199 - ((flags>>9)&0x1ff))   // type 1; other types via pano/face projection
        muzzle world point = unproject(muzzle) at sqrt(actor.dist2)
    if fires:
        if muzzle on screen (0 < x < 320, 0 < y < 200) and a streak slot is free: spawn streak (6.3)
        Sfx_EnemyFire(c, rec)
        if hit && dmg > c.dmg_min: health -= dmg*100; frameDmg += dmg
heavy |= dmg > c.heavy || frameDmg > c.heavy   (remember c as heavyClass)
light |= dmg > c.dmg_min || frameDmg > c.dmg_min (remember c as lightClass)
prev[id] = {bbox x0, y0, w, h, dist2}
```

After the loop: if heavy, `Fx_Flash(2, 20)` and `Sfx_PlayerHurtHeavy(heavyClass)`; else if light,
`Sfx_PlayerHurtLight(lightClass)`. Then `Sfx_HealthWarning(seg, health/100)`.

**RNG (H).** `rand()` is the MSVC CRT linear congruential generator (state * 214013 + 2531011, bits
16..30). The program never calls `srand` (no writer of the state exists), and the state starts at 1,
so the original's random stream is a fixed sequence from process start: runs with the same input
timing are nearly identical, and a handful of original runs is not a sample of the death-time
distribution. Our `--seed` mixes the seed first, so different seeds sample different streams. The
fire roll is once per targetable record per drawn frame (15 fps), exactly as ours; an audit of the
fire, hit, damage-threshold, regen and difficulty-index logic found no deviation. The spread of
death times between seeds (idle player, LEVEL01 from segment 5) comes from which actors pass the
per-actor spawn roll at the checkpoint and from bursts when several are on screen.

**Effective damage.** A random shot that hits does dmg ∈ [0, c.dmg), uniform over the
hit-percentage roll, and only counts if it is > dmg_min. For LEVEL01 (dmg 7, dmg_min 2) a counted
hit is 3–6, i.e. 300–600 health. Difficulty changes damage only by picking a different class
(`actor.cls[d]`): fire %, hit %, dmg, spawn % and hp all come from that class. In the data most
classes differ by hit % and spawn % (for example LEVEL01 hit % 20/16/20, spawn % 75/75/85 for index
0/1/2), with a few dmg and hp changes (LEVEL10, LEVEL14).

### 6.3 Enemy shot streaks (H for the formulas below; cosmetic)

There are 8 slots (0x422b40, each a life counter set to 3) and a 3-entry ring (0x421098) of per-slot
3D segments (0x422b60/0x422b78, 48 bytes = 2 points). On a spawn:
1. Take the on-screen motion of the actor since the last frame: the record's current position
   (bbox corner pair, fields +8/+9 on type 1, +0xc/+0xd otherwise; swapped on type 2) minus the stored previous one
   (table 0x423018), ×512, with dy halved until |dy| ≤ |dx|, and everything /8 if a component leaves ±0x7fff.
   Reverse it if the actor is receding (prev.dist2 ≤ dist2).
2. Clip the ray that **starts at the actor's current screen position** (not the muzzle, H from the call at
   0x404c9x: `FUN_0040b6b0` is a plain line-to-rectangle clip against 0..w−1 × 0..h−1) and runs along that direction
   to the screen edge, and unproject the endpoint (`FUN_0040b630`) at camDist (double at 0x4210a0) through the
   predicted view. That gives E. The start S is the muzzle unprojected through the current view at sqrt(dist2).
   d = (E − S)·0.1 (constant 0x420040 = 0.1, and 0x420048 = 3.0 gives the S+4d point; H, both read from the data).
3. Store [S, S+d] for this ring frame, [S+4d, S+5d] for the next, and [S+6d, E] for the one after.

Each frame, for each live slot:
* Project its ring-current segment (`Proj_Segment` 0x40b160).
* Pick the streak sprite: the first frame of the streak set whose height (+0x16) exceeds the
  on-screen length, else the last.
* Clip and `Fx_DrawTracer` it, thick if seg[0x12] == 3. Then life-- and ring++ (mod 3).

The streak is drawn 3 times over 3 frames. It is visual only: the damage was applied at spawn.

Difference in our engine (`streak_spawn`, game.c): it clips from the muzzle in 2D and interpolates the endpoint in
screen space, instead of starting from the actor's position and unprojecting through the predicted view. The
streak therefore differs a little in direction and length from the original. It touches no gameplay state, so it was
left as a cosmetic follow-up.

## 7. Health, death and screen effects

### 7.1 Health

* Range 0..10000 (shown as /100). It is reset to 10000 at checkpoints, segment 0 and save-point
  resume.
* **Damage**: `health -= dmg*100` in `Enemy_Update`. There is no lower clamp.
* **Regen**, `Player_Regen` 0x409dd0: health += seg[0x51 + d] per frame, capped at 10000. In demo
  attract mode (`g_attract`) it is +0x639c (25500) instead, which is god mode. It is called from
  `Hud_Draw` (when health > 0) on combat frames and from `Frame_Process` on type-0/5 frames
  (unconditionally).
* Regen values in the data range from 10 to 80 per frame. LEVEL01 is 45/55/35 for index 0/1/2, i.e.
  675/825/525 health per second (6.75–8.25 % per second).

### 7.2 Low-health warnings, `Sfx_HealthWarning(seg, hp)` 0x417b90 (H)

State: tick counter T (+1 per call, i.e. per combat frame), the last band B (reset to 100), and the
last-play times t20, t50 and t80 (reset to −0x8000). With hp = health/100:
* hp < 21 and B > 20 and T − t20 > 44: play seg[0x4d]; B = hp; t20 = T.
* else hp < 51 and B > 50 and T − t50 > 44: play seg[0x4c]; B = hp; t50 = T.
* else hp > 79 and B < 80 and T − t80 > 44: play seg[0x4e] ("recovered"); B = hp; t80 = T.

The HUD (gauge style 0) also shows message 3 (60 frames, blinking 5 on / 5 off) every frame while
hp ≤ 20.

### 7.3 Death (H)

* In `Hud_Draw`: if not latched (0x437adc), not already dead (0x436718) and health < 1: latch and
  set g_branchReq = (int8) seg[0x23].
* The reader aborts the segment at its next group boundary and plays the death clip (type 0); see
  SCENE_GRAPH §3.
* In `Play_Chunk`, after each drawn frame: on the first frame after the latch, the timer = 9; it is
  decremented each frame. At 0: `Sfx_PlayerDeath(seg)`, `Fx_Flash(3, 20)`, dead = 1 and latch = 0.
* `Sfx_PlayerDeath` plays a random sound from class **0**'s sfx_fire pool (+0x32), then class 0's
  heavy-hurt pool, then seg[0x14]. Because of the 9-frame delay, the death sound and flash usually
  land in the first frames of the death clip.
* Game_ResetForScene clears both flags.
* **God mode** exists only in demo attract (`Demo` player): `g_attract` regen. Any player shot or ACT
  launch clears it and sets `g_demoInterrupted`.

### 7.4 Flash and shake (`Fx_Flash` 0x40bbe0, `Fx_GetShake` 0x40c520), H except the colours

`Fx_Flash(f, s)` sets flashCount = f and shake = s. At each present:
* tint level = flashCount, then flashCount-- if > 0.
* If shake ≠ 0: dx = rand·s/0x7fff − s/2, rounded down to a multiple of 4; dy = rand·s/0x7fff − s/2;
  then s--. The image is offset by (dx, dy) and the uncovered edge is left black. Both are doubled
  in 640-wide modes.

Tint level 3, 2 and 1 select three different colour-conversion tables in 16-bit mode
(`Px_ConvertLine16` 0x413340: 3 → 0x42a700/0x42ab20, 2 → 0x42a900/0x42bb20, 1 → 0x42a500/0x42cb20);
in 8-bit mode they switch the palette. The colours (red, white?) are **unknown**: the tables are
built somewhere we did not trace (L). Uses:

| event | call |
|---|---|
| enemy starts exploding | `Fx_Flash(2, 10)` |
| player heavy hit | `Fx_Flash(2, 20)` |
| player death (9 frames after) | `Fx_Flash(3, 20)` |
| ACT, 6 frames after launch (actAnim == 0x2000) | `Fx_Flash(0, 20)`, shake only |

## 8. HUD, `Hud_Draw(objs, mode, fireAllowed)` 0x409e20 (H)

The draw order on combat frames:
1. `Hud_DrawACT`, then `Hud_DrawGunFlash`.
2. `Obj_DrawStatic(objs)`: kind-8 records (the per-frame radar/gauge widget streamed with the
   video).
3. Outside the choice window (seg[0x5c] == 0 or g_sceneFrame < seg[0x5c]): for each record passing
   `Obj_IsTargetable(rec, 0)` with a nonzero bbox:
   * if c.box & 0x40 (or the debug "always brackets" key is on), draw corner brackets
     (`Hud_DrawBracket` 0x40a110): the bbox grown by the same box code as the hit test, palette
     colour 0xfb, 3-pixel L-shaped corners;
   * set onTarget if the crosshair is inside the bbox.
4. The death check and regen (§7).
5. In the choice window with seg[0x5b] ≠ 0: style = seg[0x5b] and onTarget = (`Hotspot_Pick` ≠ −1).
6. `Level11.l96` special: in segments 3 and 4 it shows HUD message 5 (duration 960, 5/5 blink) and
   clears it elsewhere.
7. Crosshair if style ≠ 0 and fireAllowed.
8. Gauges if seg[0x44] ≠ 0: style 1 if seg[0x58] == 1 (LEVEL08 only), else style 0. The HUD set is
   entry seg[0x44].

**Gauge style 0** (`Hud_DrawGauges0` 0x40a610):
* **Health bar.** `Hud_DrawBar(hp/5, colour, x = 303, y = 12)` with hp = health/100 (so up to 20
  pips). The colour is 0xfc if hp > 80, 0xfa if 51–80, 0xfb if 21–50, and 0xf9 if ≤ 20 (which also
  sets message 3).
* **Heat bar** (weapon 0 only), at x = 8:
  * h = heat. If h < 0, n = h/−12 (10 → 0 during the lockout); otherwise n = 10 − h/24.
  * Draw n pips in colour 0xf9 from y = 12, then 10 − n pips in colour 4 above them.
  * While h < 1 the whole bar blinks (drawn only when g_frameNo & 4).
* **HUD sprites** (set frames, full-screen canvases drawn at 0,0): frame 0 = the gauge frame;
  frame 5 + actAmmo if actAmmo ≥ 1 (the ammo counter, `Hud_SetAmmoBlink` 0x409d60); then the
  current message frame `msg.id` with blinking.
* **Message state** (`Hud_SetMessage(id, dur, on, off)` 0x409d80; a new id restarts the blink).
  Each drawn frame: show for `on` frames, hide for `off` frames, and repeat. dur-- every frame;
  at dur == 0 the message stops (a negative dur therefore lasts ~forever).
* **Message ids** used:
  * 1 = "LASER OVERHEAT" (confirmed in data: set 9 frame 1);
  * 2 = gun ready (M);
  * 3 = low health (M);
  * 5 = Level11 text (L).

`Hud_DrawBar(n, colour, x, y)` 0x40a9d0 draws n pips upward from top-down row 199 − y, at x. Each
pip is 4 rows of width 6, 8, 8, 6 (the 6-wide rows are offset 1 px right), and the pips are 8 rows
apart. In 16-bit mode the colour index goes through the palette→RGB table at 0x431e50.

**Gauge style 1** (`Hud_DrawGauges1` 0x40a4a0): frame 0 at 0,0. Then frames 0xb/0xd (0xc/0xe when
hp < 30) are slid horizontally by ∓(100 − hp)·120/100 px, two halves closing in as health drops. No
messages, heat or ammo (M).

## 9. SFX triggers (H)

All go through `Sfx_Play(id, pan, vol, freq=0, loop=0, play=1)`:
* pan is in DirectSound units (−10000..10000);
* vol is in hundredths of a dB, scaled by the master volume (255, so effectively unchanged; values
  ≤ −2000 pass through).

`PickRandom(pool, n)` drops trailing 0xff entries and returns `pool[rand() / (0x7fff / n)]`. The
original can index one past the end when rand() = 0x7fff; clamp it.
`screenPan(rec) = ((bbox.x0 + fg.w/2)·5 − 800)·2` (bbox x0 = +0x18, w = +0x14), which is ±1600 at the screen edges.
`sizeVol = (max(w,h) < 30 && c.min_size != 0xff) ? −300/max(w,h) : 0` (quieter for small, distant
targets).

| event | sound id | pan | volume | where |
|---|---|---|---|---|
| primary shot | seg[0x10] | 0 | −80 (from `byte[0x421568]>>6`, which is never written, so always case 0) | `Sfx_WeaponFire(seg,1)` 0x417ca0 |
| ACT launch | seg[0x11] | 0 | −80 | `Sfx_WeaponFire(seg,2)` |
| gun overheated | seg[0x4f] | 0 | 0 | `Sfx_Overheat(seg,0)` 0x417b40 |
| gun usable again | seg[0x50] | 0 | 0 | `Sfx_Overheat(seg,1)` |
| hp < 51 / < 21 / > 79 | seg[0x4c] / [0x4d] / [0x4e] | 0 | 0 | §7.2 |
| player death | class0.sfx_fire pick, class0 heavy pick, seg[0x14] | 0 | 0 | `Sfx_PlayerDeath` 0x417d40 |
| enemy hit, not killed | pick(c.sfx_hit, 2) | screenPan | sizeVol | `Sfx_EnemyHit` 0x417f00 |
| enemy starts exploding | pick(c.sfx_explode, 5) | screenPan | sizeVol | `Sfx_EnemyExplode` 0x417f90 (first stops this actor's loop sound) |
| enemy fires | pick(c.sfx_fire, 2) | screenPan | −300/size if size < 30 | `Sfx_EnemyFire` 0x417e70 |
| player hurt, light | pick(c.sfx_hurt_lt, 3) | 0 | 0 | `Sfx_PlayerHurtLight` 0x417df0 |
| player hurt, heavy | pick(c.sfx_hurt_hv, 3), else the light pool | 0 | 0 | `Sfx_PlayerHurtHeavy` 0x417e30 |
| enemy approach | pick(c.sfx_near, 3) or pick(c.sfx_big, 2) | 3 × screenPan | 0 | `Sfx_EnemyLoop` 0x418080 |

**Approach sounds.** There are 8 slots (actor id, sfx id, pan). If the actor already owns a slot that
is still playing, only its pan is updated (`play=0`). Otherwise, if a slot is free, a sound is picked
and started, provided that same sound id is not already playing. Slots are freed when their sound
stops, and when the actor explodes.

## 10. Branch-condition inputs (what the engine must supply)

`Branch_Resolve` 0x40e380 runs in the **reader thread**, once per segment, right after the reader has
queued the segment's last group (and only when no branch request is pending). With the original
3-buffer ring this is about 1–2 groups (0.3–0.7 s) before the segment ends on screen; the exact lead
depends on disk speed (M). It reads the live globals at that moment:

| input | source | notes |
|---|---|---|
| crosshair x, y (flag 0x1) | player +0x1d0/+0x1d4 | the last polled position; type 2 inverts the test (SCENE_GRAPH §2) |
| health (0x2/0x4) | health/100 vs threshold | |
| kills (0x8/0x10) | player +0x200 vs threshold·(shown − dropped)/shown | shown/dropped = `g_framesShown`/`g_framesDropped`, reset at the start of each level's scene loop; without drops the factor is 1 |
| actor alive (0x20/0x40/0x100) | table28 state < −0x40 | persists across segments; reset only at checkpoints |
| random (0x200) | rand()·100/0x7fff < chance | rolled at the same moment (type 6: at segment start, `Branch_AutoPick`) |
| look-pick | `g_lookPick` 0x4212a4 ≥ 0 | set by `Player_Update` on fire/alt inside a window after seg[0x5c] |

Asynchronous requests write `g_branchReq` 0x42a36c at any frame. The reader checks it before every
group, aborts the current segment and jumps:
* death: seg[0x23], from `Hud_Draw`;
* kill branch: class +0x34, when the explosion ends;
* linked-parent branch: actor 250.

**Mapping onto `engine/src`.**
* `scene_successor()` and `hs_holds()` already implement the table44 logic over `GameState`. The
  combat layer only has to fill in `GameState`:
  * `health`, `kills`, `xhair_x/y`;
  * `actor_state` (point it at a level-lifetime `int8_t[n_actors]`);
  * `frames_shown = frames_dropped = 0` (no drops).
* `chain_fill()` resolves the successor when it **reads** the last group, and its ring can run up to
  16 groups ahead. To get the original's timing, either cap the read-ahead at 2 groups, or defer the
  `scene_successor` call until the display side reaches group `count−2` of the segment.
* `g_branchReq` has to be added: a display-side request that drops the buffered groups of the
  current segment, the same as `chain_skip` does for ESC.
* Combat hooks, per tick, in this order:
  1. `actors_update(blk)` before compositing (it changes kinds and sprites);
  2. `enemy_update`;
  3. `hud_draw` after compositing;
  4. `player_update` after the frame is handed out.

  They need the per-frame header flags (bit 0/1) and the segment record bytes listed here.

## 11. Difficulty and saved state

**Difficulty index d** (`g_difficulty` 0x429468): registry `DifficultyLevel` 1 → 0, 2 → 2, else → 1;
the default registry value is 1, the maximum 2. Its effects are:
* the class per actor (`actor.cls[d]`), which sets fire %, hit %, damage, hp, spawn %, SFX and kill
  branch;
* regen seg[0x51 + d];
* +2 heat per rapid-fire shot when d ≠ 1.

The data puts d = 1 as easiest and d = 2 as hardest (§0).

**Registry obfuscation** (`Reg_NameHash` 0x40d240, H). It works per player, under the `CurrentPlayer`
subkey; each value is a DWORD:

```c
uint16_t name_fold(const char *name) {            /* XOR of the name as little-endian u16 words */
    size_t n = strlen(name); uint16_t h = 0;
    for (size_t i = 0; i + 1 < n; i += 2) h ^= (uint8_t)name[i] | (uint8_t)name[i+1] << 8;
    if (n & 1) h ^= (uint16_t)(uint8_t)name[n-1] << 8;
    return h;
}
uint16_t nhash(uint16_t v, const char *name, uint16_t key) { return v ^ key ^ name_fold(name); }
/* write: enc = nhash(value, name, kA); chk = nhash(enc, name, kB); dword = enc << 16 | chk
   read:  valid iff nhash(lo16, name, kB) == hi16; value = min(nhash(hi16, name, kA), max);
          missing/invalid -> default; getter also returns default if value > max            */
```

| value | kA | kB | default | max | getter / setter |
|---|---|---|---|---|---|
| HighestLevel | 0xbeef | 0xcafe | 0 | 20 | 0x40d140 / 0x40d2a0 |
| SavePoint | 0x9021 | 0x2001 | 0 | 1000 | 0x40d350 / 0x40d450 |
| ACTNum | 0x3772 | 0x1169 | 5 | 7 | 0x40d500 / 0x40d600 |

(The constants are in .rdata 0x4201c4…0x420207.) The `CurrentPlayer` name is read with a 0x101-byte
buffer. Plain values (no hash): `DifficultyLevel`, `FlipControl`, `UseJoystick`, `GraphicQuality`.

## 12. Constants (quick reference)

| constant | value | where |
|---|---|---|
| frame rate | 15 fps (333 ms per 5-frame group) | `Play_Chunk` |
| health max / regen cap | 10000 | 0x409dd0, 0x405f30 |
| attract regen | 0x639c / frame | 0x409dd0 |
| damage scale | ×100 | 0x4043f0 |
| heat max / lockout | 240 / −120 | 0x4165b0, 0x415af0 |
| heat per shot | −4 when the last 3 frames all fired, −2 more if d ≠ 1; +1 passive, +1 more when not firing | 0x415af0 |
| charge gun | 1 shot / 2 frames, 7 per burst, −30 gap | 0x416210 |
| shot slots | min(gun-flash frames, 8); miss depth 10000.0 | 0x415840 |
| ACT radius² | 0x1900 (80 px); chain timer 6; ACT shake 20 | 0x4155b0 |
| enemy random visible-miss chance | rand < 0x2aaa (1/3) | 0x4043f0 |
| approach sound sizes | ≥ 21 px (big) or ≥ 16 px and growing | 0x4043f0 |
| health-warning bands / repeat | < 21, < 51, > 79 / 44 ticks | 0x417b90 |
| death delay | 9 frames | `Play_Chunk` |
| enemy streak slots / life | 8 / 3 frames | 0x4043f0 |
| fire SFX volume | −80 | 0x417ca0 |
| crosshair y scale (mouse) | ×5/6, then /2 at 640 wide | 0x40ec10 |

## 13. Open points

* (closed) WndProc 0x416fb0: see SCENE_GRAPH §4 for the keys, and §5 above for the buttons. `FlipControl` is only
  re-read on window activation (0x41717c writes `g_flipY`); there are no toggle keys and no debug keys.
* The flash tint colours (tables 0x42a500…0x42cb20 are filled by code we did not trace).
* The contents of HUD frames 2, 3 and 5 (assumed "ready", "low health" and Level11 text); gauge style
  1 frames 0xb–0xe.
* The enemy-streak endpoint is only approximated by the engine (cosmetic, §6.3).
* table28 bytes +0x14…+0x27, and class bytes 0, 4 and the unused tail of the explosion list.
* The `Branch_Resolve` sampling lead, which depends on the reader thread's progress (it is not
  deterministic in the original).
* The scripted-shot muzzle-y encoding (GAMEPLAY_DATA §3.2) is still open. Damage from scripted shots
  applies regardless, because only the tracer needs the muzzle point.
