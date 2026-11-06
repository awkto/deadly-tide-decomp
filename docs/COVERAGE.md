# DTPLAYER.EXE function coverage

How much of the engine binary we have mapped. Source data: the Ghidra function list (400 functions), our suggested-name table, and the address references in the other docs in this directory. No decompiled code is reproduced here; only addresses, names and one-line descriptions.

Buckets:

* **A** library / runtime: MSVC CRT, import thunks, startup, exception and math helpers.
* **B** game code documented: referenced by address in ENGINE_MAP, SCENE_GRAPH, GAMEPLAY_DATA, COMBAT_SPEC, FREELOOK_SPEC, LAUNCHER or L96_FORMAT (an address inside the function counts).
* **C** game code understood but not in the docs: named in our symbol table or readable at a glance; description and suggested name given.
* **D** game code not understood or not reachable.

## Summary

| Bucket | Functions | % of 400 | Bytes | % of bytes |
|---|---:|---:|---:|---:|
| A library / runtime | 168 | 42.0% | 27277 | 22.9% |
| B game, documented | 194 | 48.5% | 86155 | 72.3% |
| C game, understood, not in docs | 37 | 9.2% | 5619 | 4.7% |
| D game, not understood | 1 | 0.2% | 52 | 0.0% |
| **Total** | 400 | 100% | 119103 | 100% |

Game code only (B+C+D, 232 functions, 91826 bytes): **B documented = 83.6% by count, 93.8% by size**; B+C understood = 99.6% by count, 99.9% by size; D = 1 function(s).

How to read this: every game function has at least a working name and role. The 0x401000 to 0x418410 block is the game proper and the 0x43f000 block is the two rasteriser span loops; everything from 0x41842a to the end of the list is the statically linked C runtime plus import thunks. Bucket C is mostly small helpers, thunks, raw registry readers, unused-in-practice 16-bit paths, and platform glue that the engine replaces. Names with confidence "low" in the symbol table are structural guesses (for example `Tri_SpanA`); the one-line descriptions are from a quick look, not a full read.

Evidence for bucket A: the library block starts right after the last game function (0x418240 `Tga_WriteData`) with the import thunks (0x41842a to 0x418448), then `_strrchr`, `__fread_lk`, `__ftol`, `_rand`, `_sprintf`, `_memcpy`, stdio, `__global_unwind2`/`__local_unwind2`, `__handle_exc`, `entry` (0x418ed0, the CRT start), `__setargv`, locale and ctype, float-to-string, `__aulldiv`/`__aullrem`, `RtlUnwind`. About 58% (98 of 168) of these carry Ghidra library-signature names; the rest are unnamed but sit in the same contiguous block, are called only from CRT functions (or are leaf helpers called from many CRT sites), and match the usual MSVC 5/6 CRT layout. The unnamed CRT functions were not individually verified.

Note on `symbols.tsv`: all 215 rows are DTPLAYER.EXE addresses (0x4021a0 to 0x418240), none belong to DT.EXE.

## Recovery pass for the launcher (DT.EXE) and the player stubs

A headless Ghidra 12.2 pass that created functions at every code pointer in the data sections (vtables) and at every call target
without a function added 474 functions to DT.EXE and 23 to DTPLAYER.EXE. Almost all are 3-byte vtable stubs, thunks and
destructors; the valuable ones were the static initialisers 0x403f60 / 0x404754 / 0x4046ae (sprite tables and placements) and
the button and tick methods around 0x4056a5, 0x405547, 0x405e5a, 0x405f0d, 0x40b9ad, 0x40bb5a, 0x40bd5f and 0x41a8d2. They are
described in LAUNCHER section 7b. The DTPLAYER additions contain no game logic, so the player's coverage figures above stand.

## By subsystem

| Subsystem | Functions | B | C | D | A |
|---|---:|---:|---:|---:|---:|
| Window / platform | 13 | 12 | 1 | 0 | 0 |
| Video decode | 11 | 9 | 2 | 0 | 0 |
| Streaming / reader thread | 9 | 9 | 0 | 0 | 0 |
| Scene graph and playback loop | 17 | 17 | 0 | 0 | 0 |
| Gameplay / combat | 27 | 24 | 3 | 0 | 0 |
| HUD | 16 | 15 | 1 | 0 | 0 |
| Free-look renderer (3D) | 34 | 29 | 5 | 0 | 0 |
| Panorama, sprites and blitting | 26 | 19 | 7 | 0 | 0 |
| Input and view control | 19 | 18 | 1 | 0 | 0 |
| Sound | 31 | 20 | 10 | 1 | 0 |
| Registry / save | 25 | 18 | 7 | 0 | 0 |
| Misc | 4 | 4 | 0 | 0 | 0 |
| Library / runtime | 168 | 0 | 0 | 0 | 168 |

### Window / platform

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x40ba90 | DD_PickMode | 325 | chooses the DirectDraw display mode |
| C | 0x40bc00 | Nop | 1 | empty function (1 byte, ret) |
| B | 0x40bc10 | DD_Init | 1276 | DirectDraw init, surfaces |
| B | 0x40c130 | Pal_Load | 438 | loads the level palette to the DirectDraw palette |
| B | 0x40c2f0 | DD_ClearPrimary | 246 | clears the primary surface |
| B | 0x40c3f0 | DD_LockBack | 219 | locks the back surface |
| B | 0x40c4d0 | DD_BackPtrShaken | 70 | back buffer pointer with shake offset |
| B | 0x40c6c0 | DD_Present | 1652 | present: compose, flash, flip, screenshot |
| B | 0x40cd40 | DD_Shutdown | 219 | releases DirectDraw |
| B | 0x40d960 | Lut_Load | 276 | loads the 555 inverse colour map and shade tables from the L96 |
| B | 0x416bc0 | App_Run | 212 | application entry after CRT start: init, run session, shutdown |
| B | 0x416ca0 | Wnd_Create | 214 | registers class and creates the main window |
| B | 0x416d80 | Game_Main | 550 | WinMain-level setup: settings, DirectDraw/DirectSound init, level loop |

### Video decode

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x412840 | Frame_DecodeVideo | 353 | decodes a frame record to the surface |
| B | 0x412ba0 | Vid_DecodeFrame | 70 | main Cinepak frame decode |
| B | 0x412bf0 | Vid_DecodeAux256 | 148 | auxiliary 256-wide decode |
| B | 0x412c90 | Vid_DecodeAux128 | 137 | auxiliary 128-wide decode |
| B | 0x412f50 | Pal_LoadInverseMap (low) | 837 | loads the inverse colour map table |
| C | 0x4132a0 | Pal_BuildRemap (low) | 156 | builds a 256-entry palette remap table |
| B | 0x413340 | Px_ConvertLine16 | 1181 | converts a line to 16-bit |
| C | 0x4137e0 | Vid_PostDecode (low) | 256 | converts decoded 15-bit output to 8-bit palette indices via LUT |
| B | 0x417520 | Vid_OpenCodec | 565 | opens the VfW Cinepak codec |
| B | 0x417760 | Vid_Decompress | 876 | ICM decompress call |
| B | 0x417ad0 | Vid_CloseCodec | 98 | closes the codec |

### Streaming / reader thread

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x401000 | Reader_BranchCheckA | 950 | reader: per-group branch-request check (SCENE_GRAPH s.5) |
| B | 0x4013c0 | Reader_BranchCheckB | 647 | reader: per-group branch-request check, second copy |
| B | 0x401650 | Reader_Thread | 1834 | worker thread reading chunks into the ring, audio push, branching |
| B | 0x401da0 | Reader_ReadSegment | 710 | reader: reads a segment/chunk record at its stream offset |
| B | 0x402070 | Reader_Helper | 294 | reader helper listed in the stream-reader row of ENGINE_MAP |
| B | 0x4138e0 | Stream_AllocBuffers | 586 | allocates ring and decode buffers for the streaming reader |
| B | 0x413b30 | Stream_FreeBuffers | 205 | frees the streaming buffers |
| B | 0x413c00 | Stream_ResetEvents | 63 | resets the reader/player sync events |
| B | 0x413c40 | Level_Load | 1251 | opens the .L96, reads header and tables, allocates level memory |

### Scene graph and playback loop

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x402600 | Sprites_ExpandSet | 275 | expands a compressed (negative-type) sprite set |
| B | 0x405f30 | Game_ResetForScene | 239 | resets per-scene gameplay state |
| B | 0x406020 | Play_SceneLoop | 1647 | per-scene playback loop (timing, input, frame pump) |
| B | 0x4066b0 | Play_Chunks | 252 | steps a group of chunks for a video scene |
| B | 0x4067b0 | Play_Chunk | 1748 | plays one 333 ms chunk: audio sync, frames, drops |
| B | 0x406e90 | Frame_Process | 374 | per-frame update: logic, draw, present |
| B | 0x407040 | Play_PanoEnterFromVideo | 402 | enters panorama scene coming from a video scene |
| B | 0x4071e0 | Play_PanoEnter | 1213 | enters a panorama scene |
| B | 0x4076c0 | Play_PanoChunks | 264 | chunk stepping for panorama scenes |
| B | 0x4077d0 | Play_PanoHold | 671 | panorama hold loop (player looking around) |
| B | 0x407a70 | Play_FreeLookEnterFromVideo | 380 | enters 3D free-look scene from a video scene |
| B | 0x407bf0 | Play_FreeLookEnter | 741 | enters a 3D free-look scene |
| B | 0x40e380 | Branch_Resolve | 1130 | evaluates the hotspot/branch conditions of a segment (reader thread) |
| B | 0x414130 | Key_Skip | 476 | ESC key handler: skip intro / branch to esc_target |
| B | 0x414310 | Key_Restart | 131 | F5: restart the current level |
| B | 0x4143a0 | Key_Hub | 120 | F2: return to the hub level |
| B | 0x414420 | Session_Run | 608 | per-session loop: loads levels in sequence, handles exits |

### Gameplay / combat

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x4021a0 | Obj_IsTargetable | 250 | tests whether an actor can be targeted |
| C | 0x402720 | EnemyShots_Reset | 34 | clears the enemy shot list |
| B | 0x403780 | Obj_ProjectAll | 324 | projects all actors of the frame to screen |
| B | 0x4038d0 | Obj_BlockSize | 57 | size of an object block in the per-frame data |
| B | 0x403910 | Actor_PickExplosion | 354 | picks the explosion sprite set for a dying actor |
| B | 0x403a80 | Actor_UpdateStates | 1391 | actor state machine (appear, hit, explode, gone) |
| B | 0x403ff0 | Obj_HitTestPoint | 307 | bounding-box point hit test |
| B | 0x404140 | Weapon_HitScan | 350 | hit-scan of the crosshair against actors |
| B | 0x4042a0 | Weapon_NearestTarget | 334 | nearest target selection for lock-on |
| B | 0x4043f0 | Enemy_Update | 3865 | enemy fire and player damage logic |
| B | 0x409dd0 | Player_Regen | 77 | player health regeneration tick |
| B | 0x40bbe0 | Fx_Flash | 20 | starts hit flash / screen shake |
| B | 0x40c520 | Fx_GetShake | 401 | screen shake offset generator |
| B | 0x40e230 | Fx_GetOffset | 25 | returns the screen shake offset |
| B | 0x410740 | Player_Update | 525 | per-frame player update (input, weapons, damage) |
| B | 0x4155b0 | ACT_Fire | 614 | secondary weapon with chained lock-on |
| C | 0x415820 | ACT_FireThunk | 19 | thunk to ACT_Fire |
| B | 0x415840 | Shot_Spawn | 319 | spawns a player shot |
| B | 0x415980 | Shot_Update | 365 | advances player shots |
| B | 0x415af0 | Gun_Rapid | 719 | rapid-fire gun with heat |
| B | 0x415dc0 | Gun_Burst (low) | 532 | burst gun variant |
| B | 0x415fe0 | Gun_Pulse (low) | 553 | pulse gun variant |
| B | 0x416210 | Gun_Charge (low) | 579 | charge gun variant |
| B | 0x416460 | Weapon_Fire | 305 | fires the current weapon |
| B | 0x4165b0 | Weapon_Reset | 28 | resets weapon state |
| B | 0x4165d0 | Weapon_SetAmmo | 37 | sets ammo |
| C | 0x416600 | Weapon_GetHeatStub (low) | 6 | returns heat value (6-byte getter) |

### HUD

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x405d60 | Dbg_FormatStats | 431 | formats the debug statistics overlay text |
| B | 0x409b20 | Draw_Line | 568 | draws a line on the video buffer |
| B | 0x409d60 | Hud_SetAmmoBlink (low) | 28 | sets ammo blink timer |
| B | 0x409d80 | Hud_SetMessage | 70 | queues an on-screen HUD message |
| B | 0x409e20 | Hud_Draw | 736 | draws the whole HUD; death check |
| B | 0x40a110 | Hud_DrawBracket | 554 | draws target brackets |
| B | 0x40a340 | Hud_DrawCrosshair | 290 | draws the crosshair |
| B | 0x40a470 | Hud_DrawGauges | 34 | gauge drawing dispatcher |
| B | 0x40a4a0 | Hud_DrawGaugesStyle1 | 361 | gauge drawing, style 1 |
| B | 0x40a610 | Hud_DrawGaugesStyle0 | 951 | gauge drawing, style 0 |
| B | 0x40a9d0 | Hud_DrawBar (low) | 196 | draws a bar gauge |
| B | 0x412440 | Fx_DrawTracer | 398 | draws a shot tracer |
| B | 0x4125d0 | Fx_TracerSetup (low) | 428 | prepares tracer geometry |
| B | 0x416610 | Hud_DrawGunFlash | 961 | draws gun flash/heat overlay |
| B | 0x4169f0 | Hud_DrawACT | 295 | draws ACT charges |
| C | 0x416b20 | Hud_DrawACTThunk | 14 | thunk to Hud_DrawACT |

### Free-look renderer (3D)

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x402750 | Cam_UpdateFromHeader | 723 | builds the 3D camera from the scene header |
| B | 0x402d30 | FreeLook_FixupObjects (low) | 2628 | fixes up object placement for 3D scenes |
| B | 0x405310 | FreeLook_InitGrid (low) | 1023 | builds the free-look ground/geometry grid |
| B | 0x405710 | FreeLook_Render (low) | 1042 | renders the 3D scene (triangles) |
| B | 0x405b30 | FreeLook_ProjectPoint (low) | 485 | projects a point for the free-look view |
| B | 0x407ee0 | Tri_EdgeSetupA (low) | 511 | triangle edge setup, variant A |
| B | 0x4080e0 | Tri_EdgeSetupB (low) | 735 | triangle edge setup, variant B |
| B | 0x4083c0 | Tri_EdgeSetupC (low) | 309 | triangle edge setup, variant C |
| B | 0x408500 | Tri_RasterA (low) | 2670 | textured triangle rasteriser (8-bit path) |
| B | 0x408f70 | Tri_RasterB (low) | 2359 | textured triangle rasteriser (16-bit path) |
| C | 0x4098b0 | Tri_SpanA (low) | 239 | advances/clips a triangle edge (per-scanline step) |
| C | 0x4099a0 | Tri_SpanB (low) | 383 | clips a scanline to 320 px and dispatches the span drawer |
| B | 0x40ab40 | Mat_LookAtA (low) | 418 | look-at matrix, variant A |
| B | 0x40acf0 | Mat_FromEulerA (low) | 335 | rotation matrix from Euler angles, variant A |
| B | 0x40ae40 | Mat_LookAtB (low) | 459 | look-at matrix, variant B |
| B | 0x40b010 | Mat_FromEulerB (low) | 328 | rotation matrix from Euler angles, variant B |
| B | 0x40b160 | Proj_Segment (low) | 514 | projects and clips a 3D segment |
| B | 0x40b370 | Proj_WorldToScreen | 344 | world to screen projection |
| B | 0x40b4d0 | Proj_WorldToScreenClip (low) | 344 | world to screen projection with clipping |
| B | 0x40b630 | Proj_ScreenToWorld | 123 | screen to world ray conversion |
| B | 0x40b6b0 | Clip_Line | 647 | 2D line clipping |
| C | 0x411050 | Vec_Normalize (low) | 107 | normalises a 3-float vector |
| B | 0x4110c0 | Mat_RotX (low) | 133 | X rotation matrix |
| B | 0x411150 | Mat_RotY (low) | 136 | Y rotation matrix |
| B | 0x4111e0 | Mat_RotZ (low) | 139 | Z rotation matrix |
| B | 0x411270 | Mat_Translate (low) | 134 | translation matrix |
| B | 0x411300 | Mat_Mul (low) | 433 | matrix multiply |
| B | 0x4114c0 | Mat_Copy (low) | 92 | matrix copy |
| B | 0x411520 | Vec_Length (low) | 47 | vector length |
| C | 0x411550 | Vec_AddScaled | 67 | double-precision vector a*s1 + b*s2 |
| C | 0x4115a0 | Vec_Sub | 37 | double-precision vector subtract |
| B | 0x4115d0 | Isqrt (low) | 86 | integer square root |
| B | 0x43f000 | Tri_Span8 | 713 | 8-bit sub-affine span loop of the rasteriser (_TEXT segment) |
| B | 0x43f2c9 | Tri_Span16 | 821 | 16-bit sub-affine span loop of the rasteriser |

### Panorama, sprites and blitting

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x4022b0 | Sprites_Load | 810 | loads sprite sets |
| B | 0x402a30 | Yaw_Wrap | 30 | wraps a panorama yaw angle |
| B | 0x402a50 | Pano_FixupObjects | 726 | fixes up object positions for panorama scenes |
| C | 0x407030 | thunk_Spr_RestoreBackground | 5 | thunk to 0x40daa0 |
| B | 0x40aaa0 | Obj_DrawStatic | 149 | draws a static (non-streamed) object |
| C | 0x40da80 | Spr_ResetSaveUnder (low) | 23 | resets the sprite save-under state |
| B | 0x40daa0 | Spr_RestoreBackground | 84 | restores background under a sprite |
| B | 0x40db00 | Spr_SetTint (low) | 94 | selects blend/shade tables for a sprite tint |
| C | 0x40dd00 | Px_Plot16Tinted (low) | 70 | tinted 16-bit span blend through a shade table (function-pointer target) |
| B | 0x40dec0 | Px_Plot8Tinted (low) | 63 | tinted 8-bit span blend through a shade table (function-pointer target) |
| B | 0x410ae0 | Spr_BlitRLE8 | 596 | 8-bit RLE sprite blit |
| B | 0x410d40 | Spr_BlitRLE16 | 596 | 16-bit RLE sprite blit |
| B | 0x410fa0 | Spr_HitTestPixel | 161 | per-pixel sprite hit test |
| B | 0x411630 | Pano_ScreenToPano (low) | 261 | screen to panorama coordinates |
| B | 0x411740 | Pano_PanoToScreen (low) | 514 | panorama to screen coordinates |
| B | 0x411950 | Pano_InitTables | 256 | builds panorama lookup tables |
| B | 0x411a50 | Pano_Blit8 | 203 | 8-bit panorama blit driver |
| C | 0x411b20 | Pano_Blit8Column (low) | 377 | per-column worker of the 8-bit panorama blit |
| C | 0x411ca0 | Pano_Blit8Finish (low) | 536 | edge/finish pass of the 8-bit panorama blit |
| B | 0x411ec0 | Pano_Blit16 | 203 | 16-bit panorama blit driver |
| C | 0x411f90 | Pano_Blit16Column (low) | 501 | per-column worker of the 16-bit panorama blit |
| C | 0x412190 | Pano_Blit16Finish (low) | 685 | edge/finish pass of the 16-bit panorama blit |
| B | 0x412780 | Pano_SceneSetup | 178 | panorama scene setup |
| B | 0x4129b0 | Pano_Frame | 256 | panorama frame handler |
| B | 0x412d20 | Spr_Draw | 273 | draws a sprite record |
| B | 0x412e40 | Obj_DrawLayer | 268 | draws an object layer |

### Input and view control

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x40df80 | Hotspot_Pick | 391 | picks the hotspot under the cursor |
| B | 0x40e110 | Joy_Init | 85 | joystick init |
| B | 0x40e170 | View_SetPano | 88 | selects panorama view mode |
| B | 0x40e1d0 | View_SetFreeLook | 77 | selects free-look view mode |
| C | 0x40e220 | View_SetLookSpeed (low) | 11 | resets look speed to 0x50 |
| B | 0x40e250 | Aim_GetRay | 289 | crosshair to aim ray |
| B | 0x40e7f0 | Branch_AutoPick | 225 | automatic branch pick |
| B | 0x40e8e0 | Pano_LookControl | 811 | panorama look control from mouse/joystick |
| B | 0x40ec10 | Input_Poll | 448 | polls mouse, keys, joystick |
| B | 0x40edd0 | FreeLook_Control1 (low) | 1003 | free-look control, sub-mode 1 |
| B | 0x40f1c0 | FreeLook_ControlCore (low) | 2635 | free-look control core |
| B | 0x40fc10 | FreeLook_Control2 (low) | 834 | free-look control, sub-mode 2 |
| B | 0x40ff60 | FreeLook_Control4 (low) | 400 | free-look control, sub-mode 4 |
| B | 0x4100f0 | FreeLook_Control3 (low) | 815 | free-look control, sub-mode 3 |
| B | 0x410420 | FreeLook_ControlDefault (low) | 785 | free-look default control |
| B | 0x410970 | View_ClampPano | 148 | clamps the panorama view |
| B | 0x410a10 | View_ClampFreeLook | 119 | clamps the free-look view |
| B | 0x410a90 | Branch_PickWithCheat | 62 | branch pick honouring the force-branch debug flag |
| B | 0x410ad0 | Input_SetButtons | 10 | WndProc callback storing mouse button state |

### Sound

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| D | 0x414680 | Snd_ResetState | 52 | stops audio and clears sound state flags; no callers or pointer references |
| B | 0x4146c0 | DS_Init | 584 | DirectSound init |
| C | 0x414910 | Sfx_StopAll | 88 | stops and releases every SFX buffer and the stream buffer |
| C | 0x414970 | DS_Shutdown | 250 | releases DirectSound |
| C | 0x414a70 | DS_CreateBuffer | 322 | creates a secondary sound buffer |
| C | 0x414bc0 | DS_BufferStopAll (low) | 60 | stops a buffer and resets position |
| C | 0x414c00 | DS_BufferPlay | 141 | plays a buffer |
| C | 0x414c90 | DS_BufferStop | 125 | stops a buffer |
| B | 0x414d10 | Sfx_Play | 229 | plays a sound effect (id, pan, volume) |
| C | 0x414e00 | Sfx_Stop | 41 | stops one sound effect |
| C | 0x414e30 | Sfx_IsPlaying | 88 | tests whether a sound effect is playing |
| B | 0x414e90 | Snd_LoadBank | 457 | loads the sound bank from the L96 |
| B | 0x415060 | Snd_StreamPush | 667 | pushes audio chunks to the stream buffer |
| B | 0x415300 | Snd_StreamStart | 120 | starts the stream buffer |
| B | 0x415380 | Snd_StopAll | 72 | stops all audio |
| B | 0x4153d0 | Snd_SetMasterVolume | 271 | sets master volume |
| B | 0x4154e0 | Snd_GetPlayPos | 112 | stream play cursor (clock source) |
| C | 0x415550 | thunk_Sfx_StopAll | 5 | thunk to 0x414910 |
| C | 0x415560 | Snd_Shutdown | 74 | sound shutdown |
| B | 0x417b40 | Sfx_Overheat | 73 | overheat warning sound |
| B | 0x417b90 | Sfx_HealthWarning | 265 | low-health warning sound |
| B | 0x417ca0 | Sfx_WeaponFire | 132 | weapon fire sound |
| B | 0x417d40 | Sfx_PlayerDeath | 97 | player death sound |
| B | 0x417db0 | Sfx_PickRandom | 61 | picks a random sound variant |
| B | 0x417df0 | Sfx_PlayerHurtLight (low) | 50 | light hurt sound |
| B | 0x417e30 | Sfx_PlayerHurtHeavy (low) | 63 | heavy hurt sound |
| B | 0x417e70 | Sfx_EnemyFire (low) | 133 | enemy fire sound |
| B | 0x417f00 | Sfx_EnemyHit (low) | 134 | enemy hit sound |
| B | 0x417f90 | Sfx_EnemyExplode | 170 | enemy explosion sound |
| B | 0x418040 | Sfx_ResetLoops | 62 | resets looping sounds |
| B | 0x418080 | Sfx_EnemyLoop | 251 | enemy loop sound |

### Registry / save

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x40ce20 | Reg_GetCurrentPlayer | 105 | reads CurrentPlayer from the registry |
| B | 0x40ce90 | Reg_GetAppPath | 105 | reads AppPath |
| B | 0x40cf00 | Reg_GetGraphicQuality | 51 | GraphicQuality getter |
| C | 0x40cf40 | Reg_ReadGraphicQuality | 80 | raw GraphicQuality registry read |
| B | 0x40cf90 | Reg_GetDifficulty | 54 | difficulty getter |
| C | 0x40cfd0 | Reg_ReadDifficulty | 80 | raw difficulty registry read |
| B | 0x40d020 | Reg_GetUseJoystick | 33 | UseJoystick getter |
| C | 0x40d050 | Reg_ReadUseJoystick | 84 | raw UseJoystick read |
| B | 0x40d0b0 | Reg_GetFlipControl | 33 | FlipControl getter |
| C | 0x40d0e0 | Reg_ReadFlipControl | 84 | raw FlipControl read |
| B | 0x40d140 | Reg_GetHighestLevel | 51 | HighestLevel getter |
| C | 0x40d180 | Reg_ReadHighestLevel | 190 | raw obfuscated HighestLevel read |
| B | 0x40d240 | Reg_NameHash | 86 | hash used to obfuscate progress values |
| B | 0x40d2a0 | Reg_SetHighestLevel | 176 | writes HighestLevel |
| B | 0x40d350 | Reg_GetSavePoint | 51 | SavePoint getter |
| C | 0x40d390 | Reg_ReadSavePoint | 190 | raw obfuscated SavePoint read |
| B | 0x40d450 | Reg_SetSavePoint | 176 | writes SavePoint |
| B | 0x40d500 | Reg_GetACTNum | 51 | ACTNum getter |
| C | 0x40d540 | Reg_ReadACTNum | 190 | raw obfuscated ACTNum read |
| B | 0x40d600 | Reg_SetACTNum | 176 | writes ACTNum |
| B | 0x40d6b0 | Reg_OpenBaseKey | 36 | opens the base registry key |
| B | 0x40d6e0 | Reg_OpenPlayerKey | 224 | opens the pilot subkey |
| B | 0x40d7c0 | Reg_CreatePlayerKey | 243 | creates the pilot subkey |
| B | 0x40d8c0 | Reg_QueryDword | 82 | reads a DWORD value |
| B | 0x40d920 | Reg_SetDword | 51 | writes a DWORD value |

### Misc

| B | Address | Name | Size | Description |
|---|---|---|---:|---|
| B | 0x412ab0 | FreeLook_Frame | 234 | free-look frame handler |
| B | 0x418180 | Tga_Header (low) | 65 | writes the TGA header |
| B | 0x4181d0 | Tga_Write | 98 | writes a screenshot TGA |
| B | 0x418240 | Tga_WriteData (low) | 489 | writes TGA pixel data |

Name confidence: "low" is marked next to names taken from the symbol table with low confidence; unmarked names are medium/high or come from the docs.

### Library / runtime (bucket A)

| Family | Count | Bytes | Addresses (Ghidra names where known) |
|---|---:|---:|---|
| Other CRT (unnamed, in the CRT address block) | 69 | 15695 | 4185d0, 418670, 418690, 4186d0, 418720, 4187b0, 4187d0, 418aa0, 418aec, 418bd0, 418c40, 418c70, 418cb0, 418d30, 418d50, 419180, 4193f0, 419580, 4195f0, 4197a0, 4197d0, 419860, 419d00, 419eb0, 41a000, 41aab0, 41aac0, 41aae0, 41afbc, 41afec, 41b0de, 41b0e5, 41b410, 41b920, 41bc80, 41be20, 41c0e0, 41c0f0, 41c389, 41c3f0, 41c630, 41c6a0, 41c810, 41c8a0, 41c8f0, 41cb00, 41cde0, 41d220, 41d2e0, 41d4a0, 41da66, 41da83, 41dbb0, 41dc10, 41de60, 41de90, 41e130, 41e250, 41e340, 41ea90, 41efa0, 41f600, 41f610, 41f630, 41f650, 41f690, 41f6f0, 41f9a0, 41fa20 |
| Floating-point / math / 64-bit helpers | 42 | 4038 | 4186f0 fpmath, 418758 ftol, 418ae7 cintrindisp1, 4197f0 setdefaultprecision, 419810 ms_p5_test_fdiv, 419ac0 cftoe, 419c00 cftof, 419db0 cftoe_g, 419de0 cftof_g, 419e80 shift, 41b030 cintrindisp2, 41b06e cintrindisp1, 41b0a4 ctrandisp2, 41b215 ctrandisp1, 41b241 fload, 41c960 control87, 41c9a0 controlfp, 41c9c0 abstract_cw, 41ca70 hw_cw, 41cc00 ZeroTail, 41cc70 IncMan, 41cce0 RoundMan, 41cd90 CopyMan, 41cdb0 FillZeroMan, 41cdc0 IsZeroMan, 41ce90 ld12cvt, 41d060 FID_conflict:__ld12tod, 41d080 FID_conflict:__ld12tod, 41d0a0 FID_conflict:__atodbl, 41d0e0 FID_conflict:__atodbl, 41d120 fptostr, 41d1b0 fltout, 41d3b0 aulldiv, 41d420 aullrem, 41d8c0 trandisp1, 41d927 trandisp2, 41da90 87except, 41e160 add_12, 41e1d0 shl_12, 41e210 shr_12, 41faa0 set_exp, 41fae0 decomp |
| stdio / low-level I/O | 20 | 2827 | 418480 fread_lk, 418800 sprintf, 4189c0 fclose, 418a30 FID_conflict:_fwprintf, 418a70 fsopen, 419090 filbuf, 4194b0 fflush, 419500 flush, 41a9e0 write_char, 41aa30 write_multi_char, 41aa70 write_string, 41aaf0 close, 41abc0 freebuf, 41ac00 stbuf, 41aca0 ftbuf, 41acf0 openfile, 41af00 getstream, 41c5e0 getbuf, 41c760 set_osfhnd, 41ee30 chsize |
| String / locale / ctype | 17 | 2492 | 418450 strrchr, 418870 FID_conflict:_memcpy, 418b10 atol, 418bc0 atoi, 41b370 isctype, 41b8c0 ismbblead, 41b8e0 x_ismbbtype, 41c000 getSystemCP, 41c050 CPtoLCID, 41c0b0 setSBCS, 41cb10 tolower, 41d310 wctomb, 41dc90 crtGetStringTypeA, 41ded0 crtLCMapStringA, 41e100 strncnt, 41fbe0 strcmpi, 41fc70 strupr |
| Startup / exit / error | 7 | 926 | 418c90 exit, 419060 amsg_exit, 41ba00 setargv, 41baa0 parse_cmdline, 41c3b0 FF_MSGBANNER, 41ddc0 crtMessageBoxA, 41f5d0 set_errno |
| Import thunks (VfW/ACM/DirectX) | 6 | 36 | 41842a ICSendMessage, 418430 ICOpen, 418436 ICClose, 41843c acmMetrics, 418442 DirectDrawCreate, 418448 DirectSoundCreate |
| Exception / SEH | 5 | 870 | 41b63c global_unwind2, 41b67e local_unwind2, 41b712, 41f310 handle_exc, 41fbd0 RtlUnwind |
| rand | 1 | 42 | 418780 rand |
| Startup (entry) | 1 | 351 | 418ed0 entry |

## Bucket D: not understood

| Address | Name | Size | Why |
|---|---|---:|---|
| 0x414680 | Snd_ResetState | 52 | Purpose is clear (stops audio, clears flags) but nothing calls it and no function-pointer reference exists: dead code. Engine: not needed. |

No game function is left with an unknown purpose. The only D item is dead code. The data globals that these functions use still contain unknowns (for example the 0x44 bytes of the sound-bank header and the exact role of shade2 tints, see GAMEPLAY_DATA); those are data-format gaps, not function gaps.

## Bucket C: understood but not in the docs

Engine column: "yes" means engine/src implements the behaviour, "n/a" means platform glue or 16-bit/DirectX path the engine replaces.

| Address | Name | Size | Description | Engine |
|---|---|---:|---|---|
| 0x402720 | EnemyShots_Reset | 34 | clears the enemy shot list | yes (game.c enemy shots) |
| 0x407030 | thunk_Spr_RestoreBackground | 5 | thunk to 0x40daa0 | n/a |
| 0x4098b0 | Tri_SpanA | 239 | advances/clips a triangle edge (per-scanline step) | yes (freelook.c rasteriser, own implementation) |
| 0x4099a0 | Tri_SpanB | 383 | clips a scanline to 320 px and dispatches the span drawer | yes (freelook.c) |
| 0x40bc00 | Nop | 1 | empty function (1 byte, ret) | n/a |
| 0x40cf40 | Reg_ReadGraphicQuality | 80 | raw GraphicQuality registry read | yes (save.c; file-based, no registry) |
| 0x40cfd0 | Reg_ReadDifficulty | 80 | raw difficulty registry read | yes (save.c; file-based, no registry) |
| 0x40d050 | Reg_ReadUseJoystick | 84 | raw UseJoystick read | yes (save.c; file-based, no registry) |
| 0x40d0e0 | Reg_ReadFlipControl | 84 | raw FlipControl read | yes (save.c; file-based, no registry) |
| 0x40d180 | Reg_ReadHighestLevel | 190 | raw obfuscated HighestLevel read | yes (save.c; file-based, no registry) |
| 0x40d390 | Reg_ReadSavePoint | 190 | raw obfuscated SavePoint read | yes (save.c; file-based, no registry) |
| 0x40d540 | Reg_ReadACTNum | 190 | raw obfuscated ACTNum read | yes (save.c; file-based, no registry) |
| 0x40da80 | Spr_ResetSaveUnder | 23 | resets the sprite save-under state | no (engine redraws whole frame) |
| 0x40dd00 | Px_Plot16Tinted | 70 | tinted 16-bit span blend through a shade table (function-pointer target) | done (sprites.c `draw_rle` applies the shade lookup; GAMEPLAY_DATA §8) |
| 0x40e220 | View_SetLookSpeed | 11 | resets look speed to 0x50 | yes (main.c input) |
| 0x411050 | Vec_Normalize | 107 | normalises a 3-float vector | yes (freelook.c) |
| 0x411550 | Vec_AddScaled | 67 | double-precision vector a*s1 + b*s2 | yes (freelook.c own vector math) |
| 0x4115a0 | Vec_Sub | 37 | double-precision vector subtract | yes (freelook.c) |
| 0x411b20 | Pano_Blit8Column | 377 | per-column worker of the 8-bit panorama blit | yes (scene.c panorama, own implementation) |
| 0x411ca0 | Pano_Blit8Finish | 536 | edge/finish pass of the 8-bit panorama blit | yes (scene.c) |
| 0x411f90 | Pano_Blit16Column | 501 | per-column worker of the 16-bit panorama blit | n/a (16-bit path unused by engine) |
| 0x412190 | Pano_Blit16Finish | 685 | edge/finish pass of the 16-bit panorama blit | n/a (16-bit path) |
| 0x4132a0 | Pal_BuildRemap | 156 | builds a 256-entry palette remap table | n/a (engine is 32-bit RGB) |
| 0x4137e0 | Vid_PostDecode | 256 | converts decoded 15-bit output to 8-bit palette indices via LUT | n/a (cinepak.c outputs RGB) |
| 0x414910 | Sfx_StopAll | 88 | stops and releases every SFX buffer and the stream buffer | yes (platform audio stop) |
| 0x414970 | DS_Shutdown | 250 | releases DirectSound | n/a (platform audio replaces DirectSound) |
| 0x414a70 | DS_CreateBuffer | 322 | creates a secondary sound buffer | n/a (platform audio replaces DirectSound) |
| 0x414bc0 | DS_BufferStopAll | 60 | stops a buffer and resets position | n/a (platform audio replaces DirectSound) |
| 0x414c00 | DS_BufferPlay | 141 | plays a buffer | n/a (platform audio replaces DirectSound) |
| 0x414c90 | DS_BufferStop | 125 | stops a buffer | n/a (platform audio replaces DirectSound) |
| 0x414e00 | Sfx_Stop | 41 | stops one sound effect | n/a (platform audio replaces DirectSound) |
| 0x414e30 | Sfx_IsPlaying | 88 | tests whether a sound effect is playing | n/a (platform audio replaces DirectSound) |
| 0x415550 | thunk_Sfx_StopAll | 5 | thunk to 0x414910 | n/a |
| 0x415560 | Snd_Shutdown | 74 | sound shutdown | n/a (platform audio replaces DirectSound) |
| 0x415820 | ACT_FireThunk | 19 | thunk to ACT_Fire | yes |
| 0x416600 | Weapon_GetHeatStub | 6 | returns heat value (6-byte getter) | yes (game.c heat) |
| 0x416b20 | Hud_DrawACTThunk | 14 | thunk to Hud_DrawACT | yes |

### C items that matter for gameplay (follow-up candidates)

| Address | Name | Why it matters | Engine |
|---|---|---|---|
| 0x402720 | EnemyShots_Reset | clears the enemy shot list at scene start; wrong reset timing would leak shots between scenes | yes |
| 0x40da80 | Spr_ResetSaveUnder | sprite save-under reset; only matters for the original dirty-rect drawing | no, not needed |
| 0x40dd00 | Px_Plot16Tinted | the 16-bit tinted span: `table[(src<<8)|dst_index]` stored directly as a 16-bit colour, used for explosion, gun flash and streak tints; documented in GAMEPLAY_DATA §8, engine matches | done |
| 0x40e220 | View_SetLookSpeed | resets the look speed to 0x50 at scene entry; affects look feel | yes |
| 0x416600 | Weapon_GetHeatStub | returns the heat value used by the HUD gun-flash path | yes |
| 0x4098b0 / 0x4099a0 | Tri_SpanA / Tri_SpanB | edge stepping and 320 px clipping of the free-look rasteriser; the doc covers the algorithm but not these addresses | yes |
| 0x411550 / 0x4115a0 | Vec_AddScaled / Vec_Sub | double-precision helpers inside the projection code | yes |
| 0x40d180 / 0x40d390 / 0x40d540 | Reg_Read* (progress) | raw obfuscated reads of HighestLevel, SavePoint, ACTNum; the hash and keys are documented in LAUNCHER | yes (save.c) |

Nothing in bucket C changes the documented combat or scene-graph rules. Shade/tint blending (the two tinted pixel routines and `Spr_SetTint`) is documented in GAMEPLAY_DATA §8 and implemented. The 16-bit blit and decode paths (`Pano_Blit16*`, `Px_ConvertLine16`) matter only for the original 16-bit display mode.
