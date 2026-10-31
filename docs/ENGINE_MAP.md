# DTPLAYER.EXE — engine architecture map

Static analysis of the Ghidra decompile of `DTPLAYER.EXE` (Deadly Tide, Microsoft / Rainbow Studios, 1996).
Addresses are virtual addresses in the original executable. Every name below is one we assigned,
not an original symbol. Confidence: **H** = read the code and it is clear, **M** = strong inference,
**L** = guess from shape or callers. The machine-readable list is in `decomp/symbols.tsv`, which is
private.

The `.L96` file reader is out of scope here: that is the reader thread `0x401650` and its helpers
`0x401000`, `0x4013c0`, `0x401da0`, `0x402070` and `0x40e380`, which another task covers. This
document only covers the interfaces that code exposes.

---

## 1. Startup

The CRT `entry` (0x418ed0) has WinMain **inlined**. It skips argv[0] in the command line and never
uses the rest, so **DTPLAYER takes no command-line arguments**. It calls `App_Run` (0x416bc0).

1. **`App_Run` 0x416bc0.** Uses `EnumWindows` to check for another instance, then
   `Wnd_Create` (0x416ca0) registers the class "Deadly Tide Player" and creates a full-screen popup
   window. It turns the screensaver off (`SystemParametersInfo` SPI 0x10/0x11) and calls
   `Game_Main`. Afterwards it restores the screensaver and redraws the launcher's window.
2. **`Game_Main` 0x416d80** does the initialisation and runs the session:
   - Hides the cursor. The video buffer is 320×200 (`g_vidW`/`g_vidH`).
   - Reads settings from the registry, all under
     `HKCU?\Software\Microsoft\Microsoft Games\Deadly Tide\1.00`. The open helper is 0x40d6b0, so
     the root key is not yet confirmed. Values read:
     - `GraphicQuality` (0x40cf00): selects the display mode.
     - `DifficultyLevel` (0x40cf90): the registry value maps to `g_difficulty`. 1 gives index 0
       (easy), 2 gives index 2 (hard), anything else gives index 1.
     - `FlipControl` (0x40d0b0): invert Y.
     - `UseJoystick` (0x40d020).
     - Volume is set through `Snd_SetMasterVolume(0xff)` (0x4153d0). The string `Volume` exists in
       the binary but nothing references it.
     - The per-player values `HighestLevel` (0x40d140), `SavePoint` (0x40d350) and `ACTNum`
       (0x40d500). They sit in a subkey named after `CurrentPlayer` (0x40ce20) and are
       **obfuscated**: each word is XORed with a checksum of the player name (`Reg_NameHash`
       0x40d240) and verified on read.
     - `AppPath` (0x40ce90).
   - **Choosing the level file.** If `CurrentPlayer` is `"Demo"`, the game sets demo mode
     (`g_demoMode`) and starts on `LOGOS.L96`. In demo mode, `LEVEL01.L96` loops back to `LOGOS`
     for the attract loop. Otherwise, if `HighestLevel` is 0 it starts on `LOGOS.L96`; if not, on
     `LEVEL00.L96`, the hub or level-select level. Inside a level, the scene to start from comes
     from `SavePoint` when the level number equals `HighestLevel`.
   - **`DD_Init` 0x40bc10.** Calls `DirectDrawCreate` and `SetCooperativeLevel(hwnd, 0x53)`
     (exclusive, fullscreen, allow reboot). It then calls `GetCaps` and `EnumDisplayModes`, and
     `DD_PickMode` (0x40ba90) picks a mode code from 0x3f6 to 0x3fc, based on available video
     memory and the GraphicQuality value:
     - **8-bit palettised:** 640×480 with a 40-pixel letterbox, or 320×200 / 320×240.
     - **16-bit (RGB555/565):** 640×480 or 320×240.

     `g_bpp` (0x421584) is 1 or 2 and is used everywhere. The function creates a primary surface,
     a flip chain or blit back buffer, and an offscreen 320×200 render surface. In 16-bit mode it
     adds a second offscreen surface for conversion and vertical flip. On failure it appends
     `"DirectDraw Init FAILED 0x%lx %d"` (HRESULT plus source line) to `c:\temp\error.log`.
   - `DD_Present(1, NULL)` clears the screen.
   - **`Stream_AllocBuffers` 0x4138e0** allocates the level tables, three stream ring buffers of
     0x25800 bytes each, and the frame buffers. It creates the manual-reset events
     `buffer{0,1,2}read`, `buffer{0,1,2}played`, `readSceneSet` and `playSceneSet`. On allocation
     failure it writes `"Insufficient Memory"` to `c:\temp\error.log`.
   - **`Session_Run` 0x414420.** See below.
   - Clean-up: clears the screen, releases DirectDraw (0x40cd40) and restores the cursor. It sets
     the window title to `"206 882 8080"`, an odd marker that the launcher may poll for. It then
     calls **`QuietlyActivateDeadlyTideUI`** from `dtlaunch.dll`. That function only runs
     `FindWindow` on the launcher's window (class and title come from the string table, ids
     0xb/0xc, title "Deadly Tide PCC") and brings it to the front.
3. **Launcher handoff.** DT.EXE (the "Pilot Control Center") checks for the Cinepak codec
   (`ICInfo('vidc','cvid')`) and an 8-bpp or better display, then starts `DTPLAYER.EXE`. The file
   name is in the string table. DT.EXE imports `WinExec`, but the call site near 0x405efb is not
   inside a decompiled function, so this is unresolved. **All state passes through the registry**:
   current player, difficulty, highest level, save point and ACT charges. DTPLAYER passes control
   back by activating the launcher window and exiting.

### Level loop: `Session_Run` 0x414420
The current file name lives in `g_levelFile` (0x42a3d0). While that string is not empty, the loop
does the following:
1. **`Level_Load` 0x413c40** (file-format territory). It reads the 0x94-byte header into
   0x42a450: magic `L96`, version 5, the level name at 0x42a454 such as `"Level07"`, counts and
   offsets. It then loads these tables:

   | Table | Record size | Global | Meaning (our interpretation) |
   |---|---|---|---|
   | scenes | 100 B | `g_scenes` 0x4213b4 | video segments: the branching graph |
   | hotspots | 0x44 B | `g_hotspots` 0x4213b8 | path-choice regions (yaw/pitch window → target scene) |
   | actor classes | 0x54 B | `g_actorClasses` 0x4213bc | enemy type stats (hit chance, damage, SFX, branch-on-death) |
   | actors | 0x28 B | `g_actors` 0x4213c0 | enemy instances; byte 0–2 = class per difficulty, byte 3 = state |
   | chunk index | 8 B | `g_chunkIndex` 0x4213b0 | per-5-frame chunk: sector counts and offset |
   | level links | 8 B | 0x42a040 | next-level file stems; `"SUCCESS"` = game won |

   It also loads the palette (0x40c130, 0x412f50), the SFX bank (`Snd_LoadBank` 0x414e90), the
   sprite and animation library (`Sprites_Load` 0x4022b0) and the colour lookup tables (0x40d960).
   It opens three Cinepak decompressors with `Vid_OpenCodec` (0x417520): 320×200 for the main
   video, 256×256 and 128×128 for the auxiliary panorama and texture streams.
2. If the level name is `LevelNN`, it stores NN in `g_curLevelNum` (0x4215b4).
3. It opens the file with `CreateFile` (`FILE_FLAG_NO_BUFFERING`) and starts the **reader
   thread** 0x401650 (other task), raising the priority of both threads.
4. **`Play_SceneLoop` 0x406020** runs on the main thread until the level ends.
5. It waits for the reader thread to finish. The reader thread has already written the next file
   name into `g_levelFile`, or cleared it to quit.

Level exit happens in the reader thread (0x401650, at the end). A negative next-scene value `-1-i`
selects level-link entry `i`:
- `"SUCCESS"` clears the file name, which ends the game.
- Any other entry replaces the file stem and loads that level.

When the player finishes the level equal to `HighestLevel`, the game raises `HighestLevel`
(0x40d2a0) and resets `SavePoint` (0x40d450) to 0.

---

## 2. The per-frame loop

There are **two threads** connected by a **triple-buffered ring** (`g_streamBuf[3]` at 0x42a020,
with the events above). The ring buffer index alternates through `g_bufSlot[]` (0x425540).

- **Reader thread** 0x401650 (other task). For each scene it raises `readSceneSet`, reads its
  chunks into buffer *k* and signals `buffer<k>read`. It also pushes each chunk's audio into the
  DirectSound stream buffer (`Snd_StreamPush` 0x415060). Scene branching is decided here from
  `g_branchReq` / `g_pendingScene` (0x421390). Gameplay code writes `g_branchReq`.
- **Main thread**, `Play_SceneLoop` 0x406020:
  - Each scene: wait for `readSceneSet`, set `g_curScene` (0x4210f8) and `g_sceneRec` (0x432078).
    Handle save points, reset health and ammo at checkpoints (`Game_ResetForScene` 0x405f30), then
    dispatch on the **scene type** (dword 0 of the scene record):

    | type | meaning (interpretation) | player | function chain |
    |---|---|---|---|
    | 0 | plain video (2D overlays, no shooting) | none | `Play_Chunks` 0x4066b0 |
    | 5 | cutscene / non-interactive | none | `Play_Chunks` |
    | 1 | **rail combat**: camera path in 3D from frame headers | crosshair + fire | `Play_Chunks` |
    | 2 | **panorama** (360° cylindrical look-around, yaw 0..0x9c0) with path-choice hotspots | look + fire + choose | `Play_PanoEnter` 0x407040 / 0x4071e0, `Play_PanoChunks` 0x4076c0, `Play_PanoHold` 0x4077d0 |
    | 6 | **3D free-look** (textured-triangle rasteriser; sub-mode in scene byte 0x21) | look + fire + choose | `Play_FreeLookEnter` 0x407a70 / 0x407bf0, then 0x4076c0 / 0x4077d0 |

  - `Play_Chunks` waits for `buffer<k>read`, calls `Play_Chunk` (0x4067b0) for each 8-byte
    chunk-index entry, then signals `buffer<k>played`.
- **`Play_Chunk` 0x4067b0 is the real frame loop.** One chunk holds **5 video frames**. It returns
  after 5 frames are presented.
  - **Chunk layout** in the ring buffer, from chunk-index entry `e` (sectors of 0x800 bytes):
    - `e[3]` is the start offset.
    - `e[0]` sectors of **video frames** come first.
    - Then `e[1]` sectors of **object/sprite data** (the third block type).
    - Then `e[2]` sectors of **audio**.

    Audio per chunk is 0x396c = 14 700 bytes, i.e. 44 100 bytes/s over 1/3 s. The silence fill is
    0x80, so this is probably 8-bit unsigned at 44.1 kHz mono or 22.05 kHz stereo (M).
  - **Timing:**
    - The chunk period is `g_chunkMs = 0x14d` (333 ms), corrected each chunk by the DirectSound
      play cursor (`Snd_GetPlayPos` 0x4154e0, position modulo 0x396c). Audio is the master clock.
    - The frame period is `g_chunkMs/5` (about 66.7 ms, so **15 fps**).
    - The deadline is `local_a0`. If the frame is early, `Sleep(remaining)`.
    - If the frame is late by more than half a period, the next frame is **decoded but not drawn**
      (`Frame_Process(...,draw=0)`) and counted in `g_framesDropped` (0x421108).
    - The loop pumps messages with `PeekMessage` / `GetMessage`. `WM_QUIT` sets the quit flags
      0x421034 and 0x421038.
  - **Order of operations for each drawn frame:**
    1. **`Frame_Process(frame, objs, &vlen, &olen, draw=1)` 0x406e90**:
       - `Obj_BlockSize` 0x4038d0 measures this frame's object block.
       - `Cam_UpdateFromHeader` 0x402750 builds the view and projection matrices. Type 1 uses the
         frame header's camera position, look-at and roll; type 2 uses panorama yaw and pitch;
         type 6 uses yaw, pitch and roll.
       - It copies the 68-byte frame header to `g_frameHdr` (0x4254f8).
       - Decode and draw the background. **Type 0/1/5:** `Frame_DecodeVideo` 0x412840, which
         calls `Obj_ProjectAll` 0x403780, `Actor_UpdateStates` 0x403a80, `Vid_DecodeFrame`
         0x412ba0 / 0x417760 (`ICDecompress` straight into the locked surface, bottom-up, so the
         stride is negative) and `Obj_DrawLayer` 0x412e40 for sprite layers 3 and 4.
         **Type 2:** `Pano_Frame` 0x4129b0 (object fixups, cylindrical blit 0x411a50 or 0x411ec0).
         **Type 6:** `FreeLook_Frame` 0x412ab0 (triangle renderer 0x405710, 0x408500, 0x408f70).
       - **`Enemy_Update`** 0x4043f0 runs the enemy AI and enemy fire.
       - **`Hud_Draw`** 0x409e20 draws target brackets, health, ammo and messages, and runs the
         death check.
       - `Player_Regen` 0x409dd0 runs on type 0/5 frames.
    2. `Dbg_FormatStats` 0x405d60: if the debug overlay is on, builds the stats line using the
       format `"%c %c %d:%4d%5d:%s ..."`.
    3. **`DD_Present(0, dbgText)` 0x40c6c0**: applies screen shake and colour flash
       (`Fx_GetShake` 0x40c520). In 16-bit mode it converts and flips the 320×200 buffer to the
       display, `TextOut`s the debug line, and then flips or blits. It also writes the
       **screenshot** `dump%04d.tga` when requested (16-bit mode only).
    4. `Spr_RestoreBackground` 0x40daa0: undoes the save-under rectangles for sprites drawn
       directly into the video buffer.
    5. **`Player_Update(frame, objs, framesLeft)` 0x410740**: reads input, moves the view, handles
       the weapon and path choice. Despite its position after Present, this is the gameplay input
       step; its effects show up on the next frame.
    6. The death timer runs 9 frames after health reaches 0, then plays the death SFX and sets
       flash 3.
    7. Frame counters: `g_frameNo` 0x421140, `g_sceneFrame` 0x421144, `g_framesShown` 0x42110c.
- **Audio** runs separately as a looping 6×0x396c DirectSound streaming buffer that the reader
  thread fills, plus static secondary buffers for SFX. No audio work happens per frame on the main
  thread apart from triggering SFX.

---

## 3. Subsystems

| Subsystem | Key functions (address — suggested name) | Purpose |
|---|---|---|
| App / window | 0x418ed0 `entry` (WinMain inlined), 0x416bc0 `App_Run`, 0x416ca0 `Wnd_Create`, 0x416d80 `Game_Main`, **WndProc at 0x416fb0 (not decompiled)** | process start, window, init / teardown, handoff to the launcher |
| Session / level | 0x414420 `Session_Run`, 0x413c40 `Level_Load`, 0x4138e0 `Stream_AllocBuffers`, 0x413b30 `Stream_FreeBuffers`, 0x413c00 `Stream_ResetEvents` | level-file loop, table loading |
| Stream reader (other task) | 0x401650 `Reader_Thread`, 0x401000, 0x4013c0, 0x401da0, 0x402070, 0x40e380 | reads chunks into the ring, pushes audio, branching and level exit |
| Scene sequencing / path branching | 0x406020 `Play_SceneLoop`, 0x4066b0 `Play_Chunks`, 0x407040 / 0x4071e0 `Play_PanoEnter*`, 0x4076c0 `Play_PanoChunks`, 0x4077d0 `Play_PanoHold`, 0x407a70 / 0x407bf0 `Play_FreeLookEnter*`, 0x40df80 `Hotspot_Pick`, 0x40e7f0 `Branch_AutoPick`, 0x410a90 `Branch_PickWithCheat` | scene graph walk; choose the next scene from hotspots or random |
| Frame loop / timing | 0x4067b0 `Play_Chunk`, 0x406e90 `Frame_Process`, 0x405d60 `Dbg_FormatStats` | 15 fps loop synced to the audio clock, frame dropping |
| Video decode / blit | 0x417520 `Vid_OpenCodec`, 0x417760 `Vid_Decompress`, 0x417ad0 `Vid_CloseCodec`, 0x412840 `Frame_DecodeVideo`, 0x412ba0 `Vid_DecodeFrame`, 0x412bf0 / 0x412c90 `Vid_DecodeAux256/128`, 0x413340 `Px_ConvertLine16` | Cinepak via VfW ICM into the surface |
| DirectDraw / present | 0x40bc10 `DD_Init`, 0x40ba90 `DD_PickMode`, 0x40c2f0 `DD_ClearPrimary`, 0x40c3f0 `DD_LockBack`, 0x40c4d0 `DD_BackPtrShaken`, 0x40c6c0 `DD_Present`, 0x40cd40 `DD_Shutdown`, 0x40c130 `Pal_Load` | surfaces, mode, palette, flip |
| Screen FX | 0x40bbe0 `Fx_Flash(type, shake)`, 0x40c520 `Fx_GetShake`, 0x40e230 `Fx_GetOffset` | hit flash / palette tint, screen shake |
| Panorama (type 2) | 0x412780 `Pano_SceneSetup`, 0x411950 `Pano_InitTables`, 0x4129b0 `Pano_Frame`, 0x411a50 / 0x411ec0 `Pano_Blit8/16`, 0x411630 `Pano_ScreenToPano`, 0x411740 `Pano_PanoToScreen`, 0x402a30 `Yaw_Wrap`, 0x402a50 `Pano_FixupObjects` | cylindrical 360° view |
| Free-look 3D (type 6) | 0x412ab0 `FreeLook_Frame`, 0x402d30 `FreeLook_FixupObjects`, 0x405710 `FreeLook_Render`, 0x408500 / 0x408f70 `Tri_Raster*`, 0x405310 `FreeLook_InitGrid`, 0x405b30 | perspective textured view |
| 3D math | 0x402750 `Cam_UpdateFromHeader`, 0x40ab40 / 0x40ae40 `Mat_LookAt*`, 0x40acf0 / 0x40b010 `Mat_FromEuler*`, 0x4110c0..0x411300 `Mat_*`, 0x40b370 `Proj_WorldToScreen`, 0x40b630 `Proj_ScreenToWorld`, 0x40b160 `Proj_Segment`, 0x40b6b0 `Clip_Line` | camera, projection |
| Sprites / objects | 0x4038d0 `Obj_BlockSize`, 0x403780 `Obj_ProjectAll`, 0x412e40 `Obj_DrawLayer`, 0x412d20 `Spr_Draw`, 0x410ae0 / 0x410d40 `Spr_BlitRLE8/16`, 0x40aaa0 `Obj_DrawStatic`, 0x4022b0 `Sprites_Load`, 0x40daa0 `Spr_RestoreBackground`, 0x40db00 `Spr_SetTint` | per-frame streamed RLE sprites |
| Enemies / targets & hit testing | 0x403a80 `Actor_UpdateStates`, 0x4021a0 `Obj_IsTargetable`, 0x403ff0 `Obj_HitTestPoint`, 0x410fa0 `Spr_HitTestPixel`, 0x404140 `Weapon_HitScan`, 0x4042a0 `Weapon_NearestTarget`, 0x403910 `Actor_PickExplosion`, 0x4043f0 `Enemy_Update` | actor state machine, enemy fire, bbox and pixel hit tests |
| Player weapons | 0x416460 `Weapon_Fire`, 0x415af0 `Gun_Rapid` (heat), 0x415dc0 / 0x415fe0 / 0x416210 `Gun_*Variants`, 0x4155b0 `ACT_Fire` (secondary, chained lock-on), 0x415840 `Shot_Spawn`, 0x415980 `Shot_Update`, 0x4165b0 `Weapon_Reset`, 0x4165d0 `Weapon_SetAmmo`, 0x40e250 `Aim_GetRay` | firing, tracers, ammo, heat |
| Input / crosshair / camera | 0x40ec10 `Input_Poll`, 0x40e110 `Joy_Init`, 0x410ad0 `Input_SetButtons` (called from WndProc), 0x40e8e0 `Pano_LookControl`, 0x40edd0 / 0x40fc10 / 0x40ff60 / 0x4100f0 / 0x410420 `FreeLook_Control*`, 0x40f1c0, 0x410970 / 0x410a10 `View_Clamp*`, 0x40e170 / 0x40e1d0 `View_Set*` | mouse/joystick to crosshair, look, buttons |
| Player health | 0x409dd0 `Player_Regen`, damage inside 0x4043f0, death check in 0x409e20 | health 0..10000 at `player+0x1fc` |
| HUD / text | 0x409e20 `Hud_Draw`, 0x40a110 `Hud_DrawBracket`, 0x40a340 `Hud_DrawCrosshair`, 0x40a470 / 0x40a4a0 / 0x40a610 `Hud_DrawGauges*`, 0x40a9d0 `Hud_DrawBar`, 0x416610 `Hud_DrawGunFlash`, 0x4169f0 `Hud_DrawACT`, 0x409d80 `Hud_SetMessage`, 0x409d60 `Hud_SetAmmoBlink`, 0x412440 / 0x4125d0 `Fx_DrawTracer`, 0x409b20 `Draw_Line` | overlay on the video buffer; debug text via GDI `TextOut` |
| Audio | 0x4146c0 `DS_Init`, 0x414e90 `Snd_LoadBank`, 0x415060 `Snd_StreamPush`, 0x4154e0 `Snd_GetPlayPos`, 0x415300 `Snd_StreamStart`, 0x415380 `Snd_StopAll`, 0x414d10 `Sfx_Play(id,pan,vol)`, 0x417b40..0x418080 `Sfx_*` (event wrappers), 0x418040 `Sfx_ResetLoops`, 0x417db0 `Sfx_PickRandom`, 0x4153d0 `Snd_SetMasterVolume` | DirectSound streaming plus SFX |
| Save / registry | 0x40ce20 `Reg_GetCurrentPlayer`, 0x40ce90 `Reg_GetAppPath`, 0x40cf00 / 0x40cf90 / 0x40d020 / 0x40d0b0 getters, 0x40d140 / 0x40d2a0 `HighestLevel` get/set, 0x40d350 / 0x40d450 `SavePoint` get/set, 0x40d500 / 0x40d600 `ACTNum` get/set, 0x40d240 `Reg_NameHash`, 0x40d6b0 / 0x40d6e0 / 0x40d7c0 / 0x40d8c0 / 0x40d920 raw helpers | settings and progress |
| Error log | inside 0x40bc10 and 0x4138e0 (`fopen("c:\temp\error.log", "a"?)` via 0x418aa0) | logs DirectDraw init failure and out-of-memory only |
| Screenshot | inside 0x40c6c0: `dump%04d.tga`, `Tga_Write` 0x4181d0 (0x418180 header, 0x418240 write) | 16-bit mode only, flag `g_screenshotReq` 0x421590 (set by WndProc, presumably a hotkey) |
| Scoring / end of level | no score counter found. `player+0x200` counts enemies hit or killed and `g_shotsFired` 0x4214c8 counts shots, but neither is displayed or saved. Level completion is the negative-scene path in the reader thread. | see §1 |

---

## 4. Key globals

| Address | Name | Meaning |
|---|---|---|
| 0x429594 | `g_hwnd` | main window |
| 0x421584 | `g_bpp` | bytes per pixel (1 = 8-bit, 2 = 16-bit) |
| 0x421588 | `g_blitMode` | 0 = page flip, 1 = blit to primary |
| 0x421540 / 0x421544 | `g_screenW/H` | display mode (640×480 or 320×200/240) |
| 0x421548 / 0x42154c | `g_vidW/H` | 320×200 render buffer |
| 0x421558 | `g_letterboxY` | vertical offset on the display |
| 0x421384 | `g_pixFmtGreenMask` | 0x3e0 means RGB555 (otherwise 565) (M) |
| 0x431b38, 0x431cfc, 0x431b40, 0x431b3c, 0x431b34 | `g_dd`, `g_ddsPrimary`, `g_ddsBack`, `g_ddsRender`, `g_ddsConvert` | DirectDraw objects (M) |
| 0x421204 | `g_lockedBits` | pointer to the locked render surface |
| 0x421374 / 0x421378 / 0x42137c | `g_codecMain/256/128` | Cinepak decompressor contexts |
| 0x42a150 (via `PTR_DAT_00421398`) | `g_view` | **player and view state**, see below |
| 0x4213b4 / 0x4213b8 / 0x4213bc / 0x4213c0 / 0x4213b0 | `g_scenes`, `g_hotspots`, `g_actorClasses`, `g_actors`, `g_chunkIndex` | level tables (§1) |
| 0x42a450 / 0x42a454 | `g_levelHdr`, `g_levelName` | level file header |
| 0x42a3d0 | `g_levelFile` | current or next `.L96` file name; empty = quit |
| 0x4210f8 | `g_curScene` | current scene index |
| 0x432078 | `g_sceneRec` | pointer to the current 100-byte scene record |
| 0x4213cc / 0x4213d0 | `g_nextScene` / `g_queuedScene` | reader-side scene sequencing |
| 0x42a36c | `g_branchReq` | branch target requested by gameplay (death, hotspot, kill-triggered) |
| 0x421390 | `g_pendingBranch` | reader's copy of the above; negative = level exit |
| 0x421034 / 0x421038 | `g_abortScene` / `g_quit` | stop flags |
| 0x42a020[3] | `g_streamBuf` | ring buffers, 0x25800 bytes each |
| 0x425540 | `g_bufSlot[]` | ring slot sequence |
| 0x4254f8 | `g_frameHdr` | copy of the current 68-byte frame header |
| 0x421140 / 0x421144 / 0x42110c / 0x421108 | `g_frameNo`, `g_sceneFrame`, `g_framesShown`, `g_framesDropped` | counters |
| 0x43207c | `g_chunkMs` | 333 ms ± audio-drift correction |
| 0x429468 | `g_difficulty` | 0/1/2; indexes actor bytes 0–2 and scene byte 0x51+d |
| 0x429464 / 0x421564 | `g_useJoystick` / `g_flipY` | input settings |
| 0x4215ac / 0x4215b0 / 0x429598 | `g_highestLevel`, `g_savePoint`, `g_actCharges` | progress (registry-backed) |
| 0x4215b4 | `g_curLevelNum` | NN from `LevelNN` |
| 0x421578 / 0x421598 / 0x42157c | `g_demoMode`, `g_attract` (godmode regen), `g_demoInterrupted` | demo / attract loop |
| 0x4214bc / 0x4214c0 | `g_gunHeat` / `g_gunHeatMax` (0xf0) | rapid-fire energy; ≤0 means overheat message |
| 0x4214d4 | `g_actAmmo` | secondary weapon ("ACT") charges, saved as `ACTNum` |
| 0x4214c8 / 0x429650 | `g_shotsFired` / `g_shotLimit` | shots fired this scene vs. cap |
| 0x4214cc, 0x429660[8×48 B], 0x429620 | `g_shotIdx`, `g_shots`, `g_actShot` | player shots and tracers |
| 0x422b40[8], 0x422b60.. | `g_enemyShotSlots`, `g_enemyShotPaths` | enemy projectile streaks (3-frame history) |
| 0x423018[n×16 B] | `g_actorPrev` | previous bbox and distance per actor (approach detection) |
| 0x437fb0[×0x110 B] | `g_anims` | explosion and effect animation library |
| 0x437a6c, 0x43a93c, 0x43af8c, 0x43b09c | HUD, crosshair, gun-flash and ACT sprite sets | from `Sprites_Load` |
| 0x429fe0 / 0x429c60[×36 B] | `g_dsStream` / `g_sfx[]` | DirectSound stream buffer and SFX table |
| 0x421208 / 0x42120c | `g_flashType` / `g_shakeAmt` | screen effects |
| 0x4211d8.. | `g_hudMsg` | current HUD message id and parameters |
| 0x4213d4 | `g_panoYawOffset` | yaw offset of the current panorama scene |
| 0x421570, 0x421590, 0x421368, 0x4215a8, 0x42159c, 0x4215a4, 0x42158c | debug: stats overlay, screenshot request, CPU-burn slowdown, force branch 'Y'/'N', draw 3D markers, always draw brackets, run/pause | presumably toggled by WndProc keys (WndProc not decompiled) |

**`g_view` struct (0x42a150).** Fields:
- +0 / +4: buffer width and height (320, 200).
- +0x18 to +0x24: panorama yaw and pitch limits.
- **+0x28 yaw** (0..0x9c0 = 2496 units per turn), **+0x2c pitch**, +0x30 roll.
- +0x34: `JOYCAPS`, with joystick ranges at +0x58 to +0x64.
- +0x1c8: `JOYINFOEX`.
- **+0x1d0 / +0x1d4**: crosshair x,y in 320×200 space.
- **+0x1e8**: buttons. Bit 0 = fire, bit 2 = ACT secondary, bits 0–1 also confirm a path choice.
- **+0x1fc health** (0..10000, shown /100).
- **+0x200**: targets hit or killed.
- +0x204 / +0x208: aim point latched at the moment of firing.

---

## 5. Priority functions for a reimplementation

Ordered by importance for gameplay. One line each; each is a candidate for a separate follow-up decompile.

1. **0x4067b0 `Play_Chunk`**: frame timing, audio sync, frame dropping, order of operations.
2. **0x406e90 `Frame_Process`**: per-frame dispatch by scene type; the bridge to the stream data.
3. **0x406020 `Play_SceneLoop`**: scene graph walk, save points, transitions between scene types.
4. **0x403a80 `Actor_UpdateStates`**: actor state byte. 0x7f = gone, 0 = alive, −1..−0x40 = dying
   or exploding, < −0x40 = waiting to spawn. Handles spawn probability (class +0x1b), linked
   actors, kill-triggered branches (class +0x34) and explosion sprite substitution.
5. **0x4043f0 `Enemy_Update`**: enemy fire decision. Uses class +0x1c fire %, +0x1d hit %,
   +2 damage and +0x1e / +3 thresholds. Damage is health −= dmg×100. Also enemy projectile streaks,
   hurt sounds and flash.
6. **0x404140 `Weapon_HitScan`** with **0x403ff0 `Obj_HitTestPoint`**, **0x410fa0 `Spr_HitTestPixel`**
   and **0x4021a0 `Obj_IsTargetable`**: primary-weapon hit tests. The bbox is adjusted by class
   +1 & 0x3f, and the pixel-exact test is used when class +1 & 0x80.
7. **0x410740 `Player_Update`**: per-scene-type input, look, fire and path-choice handling.
8. **0x416460 `Weapon_Fire`** and **0x415af0 `Gun_Rapid`**: weapon type comes from scene byte 0x12
   (0–5); covers heat, overheat and difficulty modifiers.
9. **0x4155b0 `ACT_Fire`** with **0x4042a0 `Weapon_NearestTarget`**: secondary weapon that chains
   to the nearest targets within radius² 0x1900.
10. **0x40df80 `Hotspot_Pick`**, **0x40e7f0 `Branch_AutoPick`** and **0x40e8e0 `Pano_LookControl`**:
    path choice by yaw/pitch window, random branches (flag 0x200, probability at +0x30), and the
    camera auto-turn toward the chosen exit.
11. **0x409e20 `Hud_Draw`**: target brackets, death trigger (health ≤ 0 → branch to scene byte
    0x23), path-choice window: hotspots become active once the scene frame reaches scene short 0x5c; byte 0x5b is the crosshair style used then, Level11 special case.
12. **0x409dd0 `Player_Regen`**: per-frame health regeneration by difficulty (scene bytes
    0x51..0x53).
13. **0x405f30 `Game_ResetForScene`**: rolls actor spawns, resets shots and SFX, health 10000,
    ammo.
14. **0x402750 `Cam_UpdateFromHeader`**: how the frame header's camera fields become a view matrix.
15. **0x403780 `Obj_ProjectAll`**: per-object 3D distance and screen projection, fixes up the
    bbox to bottom-up coordinates.
16. **0x412e40 `Obj_DrawLayer`**, **0x412d20 `Spr_Draw`** and **0x410ae0 / 0x410d40
    `Spr_BlitRLE8/16`**: the sprite RLE format and layer flags.
17. **0x4038d0 `Obj_BlockSize`**: object-block layout (header 0xc + n×0x58 + pixel data).
18. **0x403910 `Actor_PickExplosion`**: picks an explosion animation by the target's on-screen size.
19. **0x415840 `Shot_Spawn`** / **0x415980 `Shot_Update`** / **0x40e250 `Aim_GetRay`**: tracer
    projection per scene type.
20. **0x40ec10 `Input_Poll`**: mouse/joystick to crosshair mapping (y×5/6, 640→320 halving,
    FlipControl).
21. **0x411950 / 0x411a50 / 0x411630 / 0x411740**: panorama projection maths (needed for type-2
    scenes).
22. **0x412ab0 / 0x405710 / 0x408500 / 0x408f70**: type-6 renderer; check first how many scenes
    actually use type 6.
23. **0x417b40..0x418080 `Sfx_*`**: map from game events to scene and class SFX bytes.
24. **0x415060 `Snd_StreamPush`** / **0x4154e0 `Snd_GetPlayPos`**: the audio clock that drives
    timing.
25. **0x40c6c0 `DD_Present`** / **0x40c520 `Fx_GetShake`** / **0x413340 `Px_ConvertLine16`**:
    flash tints and the final compose.
26. **0x4022b0 `Sprites_Load`** and **0x40d960 `Lut_Load`**: global sprite, animation and
    colour-table loading (overlaps the format task).
27. **0x40d240 `Reg_NameHash`** and the 0x40d140..0x40d600 getters and setters: needed to stay
    save-compatible.
28. **0x416fb0 WndProc (not yet a function in Ghidra)**: keyboard handling, debug keys, pause,
    help (`dt.hlp`). **Create the function in Ghidra first.**

---

## 6. Where per-frame stream data is consumed

- **Chunk-index entry (8 bytes, `g_chunkIndex`)** is consumed by `Play_Chunk` 0x4067b0, which
  computes the video and object pointers (bytes 0, 1, 3), and by `Play_PanoHold` 0x4077d0 /
  0x407bf0, which copy the audio part (byte 2). The reader thread consumes the same entries.
- **Video frame (68-byte header plus Cinepak data).** `Play_Chunk` steps through frames using the
  sizes returned by `Frame_Process`, where size = hdr[0] + 0x44 (`Vid_DecodeFrame` 0x412ba0).
  - Header dword 0 is the compressed size. The Cinepak data starts at hdr+0x44.
  - **Dwords 1–3 are the camera position and dwords 4–6 the look-at point** (ints), and
    **dword 7 is roll**. They are read in `Cam_UpdateFromHeader` 0x402750 and `Enemy_Update`
    0x4043f0, scene type 1 only.
  - **Dword 8 holds flags.** Bit 0 means the player can aim this frame (gated by scene byte
    0x44), bit 1 means weapon fire is allowed (gated by scene byte 0x45). They are read in `Player_Update` 0x410740 and `Frame_Process`
    0x406e90.
  - The header is copied whole to `g_frameHdr` (0x4254f8) and reused on type-2/6 frames
    (`Frame_Process`, the second `Enemy_Update` call). Dwords 9–16 are **not referenced by the
    code we read**.
- **Object/sprite block (the third, non-video, non-audio type).** Layout:
  - Header: dword +4 = count.
  - Then `count` × **0x58-byte records** at +0xc, followed by RLE pixel data. The record's +4 is
    an offset that gets rebased to a pointer.
  - Record fields (our reading):
    - +0 actor id (index into `g_actors`).
    - +2 kind: 1 = rail-only enemy, 2 = enemy, 4 = explosion, 8 = static overlay.
    - +4 pixel pointer, +8 data size.
    - +0xc flags. 0x80 and 0x40000000 mean "fires now". The muzzle x is packed in bits 18–29 and
      y in bits 9–17.
    - +0xf bit 0x80: targetable.
    - +0x10..+0x16: draw x, y, w, h.
    - +0x18..+0x1e: hit bbox x0, y0, x1, y1.
    - +0x20..+0x28: 3D position (ints).
    - +0x48: tint / palette byte.

  Consumers: `Obj_BlockSize` 0x4038d0, `Obj_ProjectAll` 0x403780, `Actor_UpdateStates` 0x403a80,
  `Pano_FixupObjects` 0x402a50, `FreeLook_FixupObjects` 0x402d30, `Obj_DrawLayer` 0x412e40,
  `Obj_DrawStatic` 0x40aaa0, `Enemy_Update` 0x4043f0, `Hud_Draw` 0x409e20,
  `Weapon_HitScan` 0x404140, `Weapon_NearestTarget` 0x4042a0, and `Player_Update` 0x410740 (via
  the weapon functions).
- **Audio block.** The reader thread hands it to `Snd_StreamPush` 0x415060. The main thread only
  copies it in `Play_PanoHold` 0x4077d0 / 0x407bf0, to loop audio while a panorama is held.

### Open points
- The WndProc at 0x416fb0 is not decompiled, so keyboard controls and debug keys are unknown.
- Which display mode a typical machine got, and whether 8-bit mode was the norm.
- The meaning of type-6 sub-modes (scene byte 0x21: 1..4) and what the 256×256 / 128×128 aux
  decoders hold (panorama strips or textures: M/L).
- The exact WinExec / CreateProcess call in DT.EXE.
