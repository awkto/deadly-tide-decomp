# Deadly Tide scene graph: segment records, hotspots and branching

Status: the branching logic is decoded completely. Most fields of the 100-byte segment record are
known; the remaining unknown bytes are listed in §6.
Tool: `python3 tools/l96.py graph FILE [OUT.dot]` prints the decoded segment table, the hotspot
table and graph sanity checks, and optionally writes a Graphviz file. The rendered example graphs are in
`out/scene_graph/` (LEVEL00, LEVEL01, LEVEL04, LEVEL05). Graphviz is not installed on the build
host, so only `.dot` files are provided: render them with `dot -Tsvg X.dot -o X.svg`.

Addresses refer to `DTPLAYER.EXE`. Names follow `docs/ENGINE_MAP.md`. "Segment" in
`L96_FORMAT.md` and "scene" in `ENGINE_MAP.md` mean the same thing: one 100-byte record. Offsets were
checked against the disassembly, because Ghidra sometimes indexes the record as an `int[]` (for example
its `rec[0x15]` is byte 0x54 and `rec[0x17]` is the short at 0x5c).

## 1. Segment record (100 bytes, table at header `off_segments`)

| off | type | name | meaning | evidence |
|---|---|---|---|---|
| 0x00 | i32 | type | 0 video, 1 rail combat, 2 panorama, 5 cutscene, 6 free-look 3D. The shipped files use only 0, 1 and 6 | reader 0x401650, `Play_SceneLoop` 0x406020, `Player_Update` 0x410740 |
| 0x04 | i32 | flag | 2 = stored normally, 1 = head prefetched in the carry sectors (L96_FORMAT §6) | 0x401da0, 0x401650 |
| 0x08 | u32 | offset | stream file offset | 0x401da0 |
| 0x0c | i16 | first | first stream-index entry | 0x401650, 0x406020 |
| 0x0e | i8 | **next** | default successor. Negative means leave the level and load linked name `names[-1-next]` | 0x401650, `Branch_Resolve` 0x40e380 (its fallback) |
| 0x0f | u8 | count | number of stream-index entries (groups of 5 frames, 1/3 s each) | 0x401650 |
| 0x10, 0x11 | u8 | ? | 0xff/0xff on type-0 segments; 0/1 (0x11 sometimes 3) on gameplay segments. No reader found | — |
| 0x12 | u8 | weapon | gun routine 0..5 | `Weapon_Fire` 0x416460, 0x4043f0 |
| 0x14 | u8 | ? | 0xff, 2 or 5. No reader found | — |
| 0x16 | u8 | hs_first | first record in table44 (the hotspot / branch table) | 0x40df80, 0x40e380, 0x40e7f0 |
| 0x17 | u8 | **hs_count** | number of hotspot records; 0 = no alternatives | same |
| 0x18 | i16 | entry_yaw | read from the segment **before** a type-2/6 segment. It gives the starting yaw of the panorama or free-look (for type 2 it is also the yaw offset `g_panoYawOffset`) | 0x406020 → 0x4071e0 / 0x407bf0 |
| 0x1a | i16 | entry_pitch | same, starting pitch | 0x406020 → 0x4071e0 (via tan) / 0x407bf0 |
| 0x1c | i16 | exit_yaw | the yaw a pano/free-look camera auto-turns to just before cutting **into** this segment, so the cut matches | `Pano_LookControl` 0x40e8e0, free-look controls 0x40edd0 / 0x40f1c0 / 0x410420 |
| 0x1e | i16 | exit_pitch | same, pitch | same |
| 0x21 | u8 | look_mode | type-6 control sub-mode 1..4 (selects 0x40edd0 / 0x40fc10 / 0x4100f0 / 0x40ff60, else 0x410420) | 0x410740 |
| 0x22 | i8 | **esc_target** | ESC skip target, see §4 | `Key_Skip` 0x414130 |
| 0x23 | i8 | **death** | segment to branch to when health reaches 0 | `Hud_Draw` 0x409e20 |
| 0x2c | u32 | pano_limits | type 2: nonzero = take yaw/pitch limits from the panorama header | `Pano_SceneSetup` 0x412780 |
| 0x34 | u32 | — | always 0x115 in the files; the engine also writes 0x115 here when it enters a type-2 segment | 0x406020 |
| 0x3c / 0x40 | i32 | view_w / view_h | 320×200 (0x200×0x200 on free-look). The engine overwrites them with 0x80 / 0x3000 when it enters a free-look segment. Used for projection and aim | 0x406020, 0x403780, 0x40e250, 0x4129b0, 0x412ab0 |
| 0x44 | u8 | aim | crosshair / aiming enabled. Bit 0 of the frame flags is honoured only if this is set | 0x410740, 0x409e20, 0x40a4a0 |
| 0x45 | u8 | xhair | crosshair style; also gates firing on type-1 frames | 0x409e20, 0x410740 |
| 0x46 | i16 | entry_roll | read from the segment before a type-6 segment: its starting roll | 0x406020 → 0x407bf0 |
| 0x48 | i16 | ? | almost always 0, a few rail segments ±46, −10, 11. No reader found | — |
| 0x4a | i16 | half_fov | half field of view in 1/512° (0x3c00 = 30°, 0x4d4c ≈ 38.6°). The camera computes tan(value·π/92160) | `Cam_UpdateFromHeader` 0x402750 |
| 0x4c | u8 | sfx_hp50 | SFX id when health drops below 50 % (0xff = none) | 0x417b90 (called from `Enemy_Update`) |
| 0x4d | u8 | sfx_hp20 | SFX id when health drops below 20 % | 0x417b90 |
| 0x4e | u8 | sfx_hp80 | SFX id when health rises back above 80 % | 0x417b90 |
| 0x4f | u8 | sfx_overheat | SFX id when the gun overheats | 0x417b40 from `Gun_Rapid` 0x415af0 / 0x416210 |
| 0x50 | u8 | sfx_cooled | SFX id when the gun is usable again | 0x417b40 |
| 0x51..0x53 | u8[3] | regen | health regenerated per frame (of 10000) on easy / normal / hard | `Player_Regen` 0x409dd0 (disassembly: `[rec+difficulty+0x51]`) |
| 0x54 | u8 | **save** | save point (§3) | 0x406020 (`cmp byte [rec+0x54]` at 0x4060f4) |
| 0x55 | u8 | **reset** | checkpoint: reset health, gun and actors, and the hook for SavePoint restore (§3) | 0x401650, 0x406020 |
| 0x56 | i8 | gun_param | extra argument to the gun routine | 0x416460 |
| 0x58 | u8 | gauge | HUD gauge layout (1 → 0x40a4a0, else 0x40a610) | 0x40a470 |
| 0x59 | u8 | gun_sprite | gun / crosshair sprite set index | 0x416460, 0x416610 |
| 0x5b | u8 | choice_xhair | crosshair style while hotspots are open | 0x409e20 |
| 0x5c | i16 | choice_from | segment frame (`g_sceneFrame`) after which look-picks are accepted; when nonzero, the `choice_xhair` crosshair and highlighting are shown from this frame | 0x409e20, 0x410740 |
| 0x13, 0x15, 0x20, 0x24..0x2b, 0x30..0x33, 0x38..0x3b, 0x57, 0x5a, 0x5e..0x63 | | | 0 in every shipped segment | — |

## 2. Table44: branch / hotspot records (0x44 bytes, `g_hotspots` 0x4213b8)

A segment owns `hs_count` consecutive records starting at `hs_first`. Each record is one possible
alternative to `next`.

| off | type | meaning | evidence |
|---|---|---|---|
| 0x00 | i32 | target segment (negative = level exit, like `next`) | 0x40df80, 0x40e380 |
| 0x04 | u32 | flags (below) | 0x40e380 |
| 0x08 | i32 | health threshold, compared with health/100 | 0x40e380 |
| 0x0c | i32 | kill threshold, compared with `player+0x200` (targets hit) after scaling by (frames shown − frames dropped) / frames shown | 0x40e380 |
| 0x10 | i16[6] | actor list A (table28 indices, −1 = unused) | 0x40e380 |
| 0x1c, 0x20 | i16, i16 | x0 / x1: yaw window for look-picks (type 2 wrapped by `Yaw_Wrap` 0x402a30, wraps around when x0 > x1), or crosshair x range for aim conditions | 0x40df80, 0x40e380 |
| 0x1e, 0x22 | i16, i16 | y0 / y1: pitch window, or crosshair y range | same |
| 0x24 | i16[6] | actor list B | 0x40e380 |
| 0x30 | u8 | chance in % for random branches | 0x40e380, 0x40e7f0 |
| 0x31..0x43 | | 0 in the files | — |

Flags:

| value | meaning |
|---|---|
| exactly 0x200 | **random**: taken with probability `chance` % |
| exactly 0x80 | **look-only**: never taken automatically, only by looking at the window and pressing fire (type 2/6) |
| 0x80 bit | selectable by `Hotspot_Pick` 0x40df80 |
| 0x1 | crosshair must be inside the (x0..x1, y0..y1) rectangle when the segment ends. On type 2 the meaning is inverted: the record only applies if bit 0 is clear |
| 0x2 / 0x4 | health > / < threshold |
| 0x8 / 0x10 | kills > / < threshold |
| 0x20 | any listed actor is still alive (actor state byte < −0x40, the same test `Obj_IsTargetable` 0x4021a0 uses) |
| 0x40 | none of the listed actors is alive |
| 0x100 | if the first actor of list A is alive, another listed actor must be alive too; true if the first actor is not alive |
| 0x80000000 | OR mode: the tests above that are present are ORed instead of ANDed. Absent tests count as false in OR mode and true in AND mode |

Uses found in the files: hub level-select windows (LEVEL00, flags 0x80), 50 % random forks
(LEVEL01/05/09, 0x200), "crosshair on the left or right half when the segment ends" forks (LEVEL05,
0x1), and "punishment" branches when a listed enemy survives (LEVEL04/05/10, 0x20 / 0x40).

### Related fields in the actor tables (gameplay; only the branch-relevant bits)
* table54 (actor class, 0x54 bytes) **+0x34 i8: branch target when an actor of this class is
  destroyed** (−1 = none). It writes `g_branchReq` in `Actor_UpdateStates` 0x403a80. It is used only in
  LEVEL04 (classes 2/9/16 → 37, 4/11/18 → 10). +0x1b is the spawn chance %, +0x1f the spawn delay
  (`Game_ResetForScene` 0x405f30).
* table28 (actor instance, 0x28 bytes): bytes 0..2 hold the class per difficulty, byte 3 the runtime state
  (0x7f gone, < −0x40 alive). Hotspot actor lists index this table.

## 3. Save points, checkpoints and death

Registry values (per player, obfuscated): `HighestLevel` (get 0x40d140 / set 0x40d2a0),
`SavePoint` (0x40d350 / 0x40d450), `ACTNum` (0x40d500 / 0x40d600). Save handling only applies
while you play your **frontier** level, that is when `g_curLevelNum` (NN of `LevelNN`) equals
`HighestLevel`.

* **Restore (reader thread 0x401650).** Before it streams segment 0, or any segment with
  `reset` (0x55) set, the reader reads SavePoint. On the frontier level with SavePoint ≠ 0 it streams
  segment `SavePoint` instead.
* **Save (main thread, `Play_SceneLoop` 0x406020).** On entering a segment with `save` (0x54) set,
  on the frontier level: if its index is greater than SavePoint, it writes SavePoint = index and ACTNum =
  current ACT charges. If its index equals SavePoint (we just resumed there), it resets the gun,
  health = 10000 and kills = 0, reloads ACT charges from the registry and re-rolls actor spawns
  (`Game_ResetForScene(0)`).
* **Checkpoint.** Segment 0 and every `reset` segment reset the gun, health and kills and re-roll the actors.
  ACT charges come from the registry if a save point exists, else from header 0x88.
* **Death.** `Hud_Draw` 0x409e20 latches once when health ≤ 0 and sets `g_branchReq = death`
  (0x23). Every combat level points `death` at short 10–15-group (3–5 s) type-0 clips whose `next` is 0.
  Segment 0 then triggers the restore above, so **death = death clip → restart at the last save
  point**, or at the level start if there is none. A branch request of 0 means "none" to the engine.
* **Level complete.** The reader handles a negative successor after its loop. `SUCCESS` clears the
  file name, so the game is won and the player exits. Otherwise the linked stem replaces the file name. If the
  finished level was the frontier, HighestLevel goes up by 1 and SavePoint is reset to 0.

## 4. Keys that change the flow (WndProc 0x416fb0, `WM_KEYDOWN` jump table at 0x4174ac)

| key | handler | effect |
|---|---|---|
| ESC | 0x414130 `Key_Skip` | LEVEL00 → quit; LOGOS → INTRO → INTROB → LEVEL01 (skips the intro films). In a level: if `esc_target` is greater than the current segment, or the current `next` is 0, branch to `esc_target`. If `esc_target` equals the current segment, ESC is ignored. Otherwise leave to LEVEL00 (LOGOS in demo mode). Rate-limited to once per second |
| F5 | 0x414310 | restart the current level from segment 0 (SavePoint is reset on the frontier level) |
| F2 | 0x4143a0 | back to the hub LEVEL00 (not in demo mode) |
| Space | — | sets 0x421574 (ACT fire request) |

Example: in LEVEL01, segments 0–4 (intro) have `esc_target` 5, so ESC jumps to the first combat
segment. LEVEL04 segments 13–17 skip to 18.

## 5. Control flow

### `Session_Run` 0x414420
It copies the start file into `g_levelFile`. The start file is chosen by `Game_Main`: LOGOS for the
`Demo` player or when HighestLevel = 0, else LEVEL00. While the name is not empty, the loop:
1. Loads the level (`Level_Load` 0x413c40) and starts at segment 0.
2. Sets `g_curLevelNum` = NN for `LevelNN` files (0 otherwise). If the level was entered from the hub with
   **Ctrl held during the pick** (flag 0x4213ac, set in 0x410a90) and it is the frontier level,
   SavePoint is cleared, so the level starts from the beginning.
3. Starts the reader thread 0x401650, runs `Play_SceneLoop` on the main thread, then joins the reader.
   By then the reader has written the next file name or cleared it.

### Reader thread 0x401650: choosing what to stream
For each segment the reader: applies the save-point redirect (§3); publishes the segment (`readSceneSet`);
then streams its groups. Before every group, `g_branchReq` is checked (0x401000 / 0x4013c0). A
pending request aborts the segment, stops the sounds, and becomes the next segment. A negative request ends the level.
Without a request, the successor is chosen by segment type:

* **type 0/1 followed by a type 2/6 segment**: it streams the last 5 (type 2) or 20 (type 6) groups
  separately, together with their carry sectors (the texture/panorama preload), then the first 5 or 2
  groups of the pano or free-look segment. The successor is `next`; hotspots are not evaluated.
* **type 0/1 otherwise, and type 2/6 not followed by type 5**: after the last group, the successor is
  `Branch_Resolve` 0x40e380.
* **type 5, and type 2/6 followed by type 5**: the successor is `next`.

`Branch_Resolve` 0x40e380, evaluated once per segment and cached in 0x4212a8:
1. A look-pick made by the player (0x4212a4 ≥ 0) wins.
2. Debug key state 'N' disables alternatives; 'Y' always takes the first record.
3. Otherwise it walks the segment's records in order. The first record whose condition holds (§2)
   gives the target. Records that are exactly 0x80 (look-only) are skipped.
4. Otherwise it returns `next`.

### Main thread `Play_SceneLoop` 0x406020
For each segment it waits for `readSceneSet`, sets `g_curScene` / `g_sceneRec`, applies save and checkpoint
(§3), then plays it:
* **type 0/1**: all groups go through `Play_Chunks` 0x4066b0. If `next` is type 2 or 6, the last 5 or 20
  groups go through `Play_PanoEnter` 0x407040 or `Play_FreeLookEnter` 0x407a70. These collect the
  preload; then the loop switches straight to the next segment. It takes entry yaw, pitch and roll from this record's
  0x18 / 0x1a / 0x46, and plays the first groups with 0x4071e0 or 0x407bf0.
* **type 2/6**: the remaining groups go through `Play_PanoChunks` 0x4076c0. The final 3 go through `Play_PanoHold`
  0x4077d0, which splices the carried data so the cut to the successor is seamless. If the
  successor is type 5, everything goes through `Play_PanoChunks`.
* **type 5**: `Play_Chunks`.
* The loop ends when the quit flag is set or the current segment is negative.

### Path choice in panorama / free-look segments
* From segment frame `choice_from` (0x5c), `Hud_Draw` switches to the `choice_xhair` crosshair and calls
  `Hotspot_Pick` 0x40df80 every frame to highlight. `Hotspot_Pick` returns the target of the first
  record with the 0x80 bit whose yaw window (x0..x1, wrapping) and pitch window (y0..y1) contain the
  view's yaw and pitch (`g_view+0x28/+0x2c`; yaw is 0..0x9c0 per turn).
  **In LEVEL00 (the hub) record i is only enabled when i = 0, i = 15, or i < HighestLevel**: this is
  the level-select lock.
* Fire (buttons & 3) while a window is under the view confirms it (`Player_Update` 0x410740 via
  0x410a90). `Pano_LookControl` 0x40e8e0 (or the free-look controls) then turns the camera over
  5–19 frames to the target segment's `exit_yaw` / `exit_pitch` (0x1c / 0x1e) and only then sets
  `g_branchReq`.
* Without a pick: on type 6, `Branch_AutoPick` 0x40e7f0 rolls the random (0x200) records once at the
  start. In both type 2 and type 6 the camera then auto-turns to the chosen successor's `exit_yaw` during the
  remaining frames. Otherwise the crosshair offset from the screen centre pans the view.

### Demo attract mode
When the player name is `Demo` (`g_demoMode` 0x421578): the run starts at LOGOS and follows the
linked names LOGOS → INTRO → INTROB → LEVEL01. `g_attract` 0x421598 gives god-mode regeneration
(+0x639c per frame). When LEVEL01 ends, or streams segment 0 a second time, the reader replaces the file with
LOGOS, so the attract loop repeats. ESC in a level also returns to LOGOS. Firing the ACT weapon clears
god mode (`ACT_Fire` 0x4155b0).

## 6. Data checks (`l96.py graph`, every `.L96` on the 4 CDs)

* Every `next`, hotspot, death, ESC and kill target lands on a valid segment or linked name: 0 problems.
* Every level is connected from segment 0 over next/hotspot/kill edges, and reaches its exit name.
  Segments not reached that way are death clips (10–15 groups, `next` 0), except LEVEL03 17/18
  (exit clips) and LEVEL04 36 (a 12-group clip). Nothing references these, so they are probably unused alternatives.
* LEVEL01: 0–4 intro (ESC → 5), 5 first combat (`reset`), 6 free-look with a 50 % random fork
  (→ 7 or → 11). The two branches 7–10 and 11–14 rejoin at 15. The level ends at 22 → LEVEL02. Death
  clips are 23–27. There is no save point.
* LEVEL00: 0 intro → 1 free-look hub (its `next` is itself) with 16 look windows → 2..17 launch
  clips → LEVEL01..LEVEL15, INTRO.
* `CDn_NEXT` files are one-segment "change CD" clips whose `next` picks the right linked name.

Unknown: segment bytes 0x10, 0x11, 0x14, 0x48 (no reader found yet), and the exact use of the 0x34 value 0x115.
