# DT.EXE and DTLAUNCH.DLL: the "Pilot Control Center" launcher

Static analysis of the launcher front end of Deadly Tide (Microsoft / Rainbow Studios, 1996), written so the
menus can be rebuilt in the C11 + SDL3 engine. Addresses are virtual addresses in `DT.EXE` (image base
0x400000) or `DTLAUNCH.DLL` (0x10000000). Names are ours, not original. Confidence: **H** = read the code
and it is clear, **M** = strong inference, **L** = guess. Registry layout and the obfuscation are specified
in `COMBAT_SPEC.md` §11 and `SCENE_GRAPH.md` §3; this document says who writes what.

The window class and title are both `Deadly Tide PCC` ("PCC" = Pilot Control Center). The game window is
`Deadly Tide Player`. Both strings come from the DT.EXE string table (ids 11/12 and 21/22).

---

## 1. What the launcher is

* One fixed 640x480 picture (`bmp105`, see §6) with ten or so custom-drawn sprite buttons on top of it. There
  are **no** Win32 buttons, menus or fonts; labels are baked into the bitmaps. Only three tiny stock dialogs
  exist (name entry, delete confirm, "insert disc"), and the first two are repainted with bitmaps too.
* It is a single screen. There is no title screen, no level select, no new-game/continue choice, no credits
  button, no progress display. Everything else (the logo films, the LEVEL00 hub, saves) is DTPLAYER.EXE's job.
* The window is created without a border style of its own, sized to the desktop work area and the 640x480
  picture is centred in it (0x416749, 0x4171d4). F12 toggles a caption bar (0x4158b0; debugging aid, M).
* It stays open while the game runs. It is not minimised or hidden when `DTPLAYER.EXE` is started; the game is
  a separate full-screen DirectDraw window that covers it.

## 2. Screen flow

1. **Start-up checks** (WinMain 0x415e20, `Pcc_Init` 0x416179, 0x416422). Each failure shows a message box
   (DT.EXE string table, ids 5-10, 30-39) and exits:
   * module directory is a CD drive (`GetDriveTypeA` = 5): "cannot be started directly from CD".
   * Windows older than 4.0 (95 / NT4).
   * no Cinepak decompressor: `ICInfo('vidc','cvid')` fails.
   * display below 256 colours (`GetDeviceCaps(BITSPIXEL)` < 8).
   * a window of class `Deadly Tide PCC` already exists: the class is not registered, so a second copy
     exits silently (M).
   * `GamePath` (CD drive) missing from the registry and not given on the command line (an optional first
     argument is a drive letter): "re-install". If the drive's volume label does not start with `DEADLY`,
     the "Unable to find a Deadly Tide disc in drive $" dialog (template 210) appears with a drive list and a
     100 ms timer that polls for the disc; OK stores the letter back in `GamePath`.
   * no usable wave-out device: message 38 ("start without audio, choose OK"), the launcher still runs.
2. **Main screen** (the only screen). Controls, left to right and top to bottom (pixel positions in §6):
   * `PILOT:` row: a drop-down list of pilot names (arrow button), **ADD**, **DELETE**, then **HELP** and
     **ABOUT** at the right.
   * **CONTROLS** group: radio pair **MOUSE** / **JOYSTICK**, **CALIBRATE** (only meaningful with
     JOYSTICK), and a **FLIP DIRECTION** toggle.
   * **GRAPHIC QUALITY** radio group of three (I, II, III) and **DIFFICULTY** radio group of three (I, II, III).
   * **VOLUME** vertical slider with a level meter.
   * **EXIT** (a flap that opens into an "ARE YOU SURE?" YES/NO pair) and the big round **START** button.
3. **Pilot drop-down.** Clicking the arrow opens a list box (max 12 rows, alphabetically sorted). Choosing a
   name closes the list, makes it the current pilot and reloads the other controls from that pilot's registry
   values (`Pcc_SelectPilot` 0x4104c7, `Pcc_LoadControls` 0x401fbf).
4. **ADD.** If 12 pilots exist: "The Pilot name list is full" (msg 17). Otherwise the 273x115
   "PILOT REGISTRATION:" dialog (template 168) with one edit box (25 characters max, `EM_LIMITTEXT`), OK and
   CANCEL. Empty or cancel closes it. A name already in the list gives "Name already in use" (msg 28) and
   re-prompts. Otherwise the current pilot's settings are saved, the new name is added, becomes current, and the
   controls reload (a new pilot has no values, so the defaults of §4 show) and are written straight back, which
   creates the registry key. (ADD handler near 0x408300; name dialog proc 0x4085b9.)
5. **DELETE.** The 273x115 confirm dialog (template 180): "Are you sure you want to delete <name> ?" with
   YES / NO (msg 19 + name + msg 20). YES removes the entry from the list, selects the first remaining pilot
   if the deleted one was current, and deletes the pilot's registry key (`RegDeleteKeyA`, 0x410261).
6. **MOUSE / JOYSTICK / CALIBRATE.** At start-up and on every window activation `joyGetPos(0)` is polled
   (0x401f28); without a working joystick JOYSTICK and CALIBRATE are disabled (M). CALIBRATE runs
   `rundll32.exe shell32.dll,Control_RunDLL joy.cpl` from the Windows directory (0x405928; failure: msg 1).
7. **HELP** (and F1): `WinHelp` on `DT.HLP` (string 16) in the launcher directory (0x416808, HELP_FINDER).
   Closed with the launcher.
8. **ABOUT.** Shows the 386x346 about picture (`bmp104`: copyright, Rainbow Studios and TRG3 logos, "Product
   ID:" line, legal text) and scrolls the 386x1500 credits strip (`bmp110`) in it (0x4141xx, bitmap ids
   0x68 / 0x6e; M). The product id string template is `xxxxx-xxx-xxxxxxx`; where the real value is read from
   is the registry value `PID` (H, see §7).
9. **START / CONTINUE** (handler 0x405e5a, H):
   1. If the pilot name box is empty: "You need to enter a pilot name before playing the game. Would you like to
      enter one now?" (msg 25, Yes/No). Yes opens the ADD dialog, No returns.
   2. Save pilot and settings (`Pcc_SaveAll` 0x4020a9, §3).
   3. Re-check the disc (`Pcc_CheckGamePath` 0x415c03, may show the disc dialog).
   4. `FindWindow("Deadly Tide Player")`. If it exists the game is already running: restore it if iconic and
      `SetForegroundWindow` (this is what the button does when it reads **CONTINUE**). If it does not exist:
      `WinExec(<launcher dir>\DTPLAYER.EXE, SW_SHOW)`. If that returns below 32: msg 7 "Unable to start the
      game".
   The button shows **START** while no game window exists and **CONTINUE** while one does (frame sets
   `bmp161-166` and `bmp111-116`); the swap runs on every WM_ACTIVATE (0x401f73). F3 also brings the running
   game to the front (0x416894).
10. **EXIT.** The flap button opens the "ARE YOU SURE?" pair (bmp132-140). YES destroys the window. Alt+F4 does
    the same without a confirmation (M). WM_DESTROY (0x416894): `Pcc_SaveAll`, **send WM_CLOSE to the game
    window if it is running** (0x41590c), close WinHelp, quit; after the message loop the original wave-out
    volumes are restored (0x415bcf).

Changing the pilot while the game is running (including via ADD / DELETE) asks "The Pilot Name or Graphic
Quality setting you have changed will require the game to restart at the last save point. Do you want to
continue?" (msg 23, OK/Cancel). OK sends WM_CLOSE to the game (0x415923) and the button flips back to START;
Cancel restores the previous selection. The same prompt guards the radio-group change handler 0x405436 (M, the
text says Graphic Quality). Difficulty, joystick and flip changes while a game runs do not prompt and do not
affect it (the engine only reads them at start-up).

**What happens when the game exits.** DTPLAYER restores the screensaver, then calls `QuietlyActivateDeadlyTideUI`
from DTLAUNCH.DLL (`FindWindow` on the PCC class/title, restore if iconic, `SetForegroundWindow`; no error box).
The launcher is not told anything else. It does not wait on the process, does not read an exit code, shows no
mission results and does not re-read the registry; the activation just swaps CONTINUE back to START. Progress
(HighestLevel, SavePoint, ACTNum) is written by the engine and read by the engine on the next START.

## 3. Registry: who writes what

Root is **HKEY_LOCAL_MACHINE** (open helper 0x4118fb, create helper 0x41191e; the engine uses `RegOpenKeyEx`
on `0x80000002` too: ENGINE_MAP §1 left the root open, this settles it).
Base key `Software\Microsoft\Microsoft Games\Deadly Tide\1.00` (call it `BASE`). All DWORD values are REG_DWORD
and all strings REG_SZ.

**A pilot is a registry subkey `BASE\<name>`.** The launcher builds its pilot list by enumerating the subkeys of
`BASE` (`RegEnumKeyEx`, 0x41045e), so there is no separate pilot list. The ADD dialog, DELETE and the
drop-down are just a view of those subkeys.

| value | where | type | writer | reader | meaning, default, range |
|---|---|---|---|---|---|
| `CurrentPlayer` | BASE | SZ | Setup (first pilot), launcher (0x411405) on every select/add/delete and save | launcher 0x4113a7, engine 0x40ce20 | name of the active pilot. A 0x101-byte buffer is used everywhere. |
| `AppPath` | BASE | SZ | Setup | engine 0x40ce90 | install dir (`C:\DT\` in the Wine rig). The launcher never reads it. |
| `GamePath` | BASE | SZ | Setup, launcher (0x415c03, after the disc check) | launcher only | CD drive root, e.g. `D:\`. The launcher sets its current directory to it before starting the game, so the engine inherits it as the working directory. |
| `Title`, `PID` | BASE | SZ | Setup | not seen read in DT.EXE (PID: see §7) | product name / product id. |
| `GraphicQuality` | BASE\name | DWORD | launcher (0x4114d2) | launcher 0x411488, engine 0x40cf00 | radio index 0..2 (button I..III). Default **2**, anything above 2 reads as 2. |
| `DifficultyLevel` | BASE\name | DWORD | launcher (0x411596) | launcher 0x41154c, engine 0x40cf90 | radio index 0..2. Default **1**. Engine: 0 -> easiest, 1 -> normal, 2 -> hardest (COMBAT_SPEC §11). The launcher's own out-of-range fallback is 2 (original quirk, it reuses the GraphicQuality default); irrelevant since it always writes 0..2. |
| `Volume` | BASE\name | DWORD | launcher (0x411657) | launcher 0x41160d | 0..255, default **0x80**. Slider top = 255. Value `(H*256 - y*255 - 1) / H` for slider height H and knob offset y (0x402148). **Not read by the engine.** |
| `UseJoystick` | BASE\name | DWORD | launcher (0x41170c) | launcher 0x4116be, engine 0x40d020 | 1 if the JOYSTICK radio is selected, else 0. Launcher default 1, engine default 0 (never matters, the launcher always writes it). |
| `FlipControl` | BASE\name | DWORD | launcher (0x4117c1) | launcher 0x411773, engine 0x40d0b0 | 0/1, default 0. Written from the FLIP DIRECTION toggle. |
| `HighestLevel` | BASE\name | DWORD, obfuscated | engine; launcher only for the Poseidon pilot | engine 0x40d140 | 0..20, default 0. |
| `SavePoint`, `ACTNum` | BASE\name | DWORD, obfuscated | engine only | engine | never touched by the launcher. |

Plain values have no checksum. The three obfuscated values use the scheme in COMBAT_SPEC §11 (the launcher
carries a copy of the same hash routine at 0x411807 with the same `0xbeef` / `0xcafe` keys for `HighestLevel`).

**`Pcc_SaveAll` 0x4020a9** runs on START, EXIT and the ADD flow. It reads the name from the pilot field, makes
sure it is in the list (0x410166), writes `CurrentPlayer`, then writes the five plain values of the selected
pilot (0x402148): GraphicQuality, DifficultyLevel, Volume, UseJoystick, FlipControl. Writing creates the
subkey if needed. **Surprise: if the name equals `Poseidon` (case-insensitive, string at 0x42b1a0) it also
writes `HighestLevel` = 20 (0x14) every time**, i.e. a built-in "everything unlocked" pilot. Any other pilot
keeps whatever the engine stored.

**Reset / new game.** The launcher has no reset. Deleting a pilot deletes its whole key, so SavePoint, ACTNum and
HighestLevel go with it, and re-adding the same name starts from nothing (the engine then plays the logo films
and LEVEL01). `CurrentPlayer` is also set to the empty string at the end of DELETE (0x410261) until the next
select or save.

**Settings are per pilot.** ENGINE_MAP §1 lists the five plain values as global. They are not: the engine
helpers (0x40d6e0) and the launcher helpers (0x411954 / 0x4119df) both open `BASE\<CurrentPlayer>`. Setup writes
the same five values under `$PLAYERNAME` for the first pilot.

**Launching the engine.** `WinExec("<dir of DT.EXE>\DTPLAYER.EXE", SW_SHOW)`: no arguments, no waiting, no
process handle. The engine ignores its command line (ENGINE_MAP §1). The single instance rule is the
launcher's `FindWindow` check; the engine also enumerates windows for another instance.

## 4. Pilot rules (all H unless noted)

* **Maximum 12 pilots** (constant 12 passed to the list-box constructor 0x40f994 from 0x41594c, also the number
  of visible rows). The check is `count < 12` before ADD; the registry itself has no limit.
* **Name length 25 characters** in the ADD edit box. Other readers use 0x100/0x101 byte buffers. Characters are not
  filtered by anything we found; a name containing `\` would create a nested key (not tested, L).
* Names are unique, case-insensitively in the list box (`LB_FINDSTRINGEXACT`); the registry is case-insensitive
  too. The list is sorted alphabetically (`LBS_SORT`).
* **"Demo" is not special in the launcher** and Setup does not create it. The launcher has no string "Demo".
  It is just a pilot name; the engine reacts to it (attract mode, ENGINE_MAP §1). The Wine README's trick of making a
  fresh pilot is exactly ADD.
* The name shown in the field is the pilot that START will use; `CurrentPlayer` is rewritten from it on START
  and EXIT, so the launcher always overrides an externally edited value on exit.
* DELETE of the last pilot leaves an empty name; START then asks for a name (msg 25).

## 5. Which options does the engine honour?

| option | launcher | engine behaviour |
|---|---|---|
| Pilot (`CurrentPlayer`) | name + subkey | **yes**: selects the save slot; `Demo` -> demo mode |
| `GraphicQuality` 0..2 | radio I-III | **yes**: index into the mode choice of `DD_PickMode` 0x40ba90 (the list of usable DirectDraw modes ordered by memory and capability; 8-bit 320x200/320x240/640x480+letterbox or 16-bit 320x240/640x480; ENGINE_MAP §1). The mapping is decoded in §7a (H). |
| `DifficultyLevel` | radio I-III | **yes**: actor classes, regen, gun heat (COMBAT_SPEC §11) |
| `UseJoystick` | MOUSE/JOYSTICK | **yes**: mouse or `joyGetPosEx` input (COMBAT_SPEC §5) |
| `FlipControl` | FLIP DIRECTION | **yes**: inverts Y (mouse flips from top, joystick default is flight style) |
| `Volume` | slider | **no registry use in the engine** (its master volume is a constant 255). The launcher applies the slider live to the Windows wave-out device volume with `waveOutSetVolume` (0x415b80), scaled from each device's saved left/right level, and restores the saved levels when it exits. So volume only takes effect system-wide while the launcher is open. |
| CALIBRATE, HELP, ABOUT, EXIT | launcher-only | n/a |
| `HighestLevel`, `SavePoint`, `ACTNum` | not shown anywhere | engine-owned progress; the Poseidon pilot gets HighestLevel 20 |

For an SDL3 port: implement the five per-pilot settings as the engine's config (the engine's reader should take
the same defaults: quality 2, difficulty 1, volume 128 (apply as the real mixer volume, since there is no
system mixer to rely on), joystick 0/1, flip 0), and keep progress values separate.

## 6. Assets

All extracted from the DT.EXE resources (RT_BITMAP, 8-bit palettised DIBs; the credits and sprite
bitmaps use blue `(0,0,128)` as the transparent key, M). PNG copies are in `dump/launcher/bmpNNN.png`
(gitignored, **do not commit**; 74 bitmaps plus the icon `IDI_TIDEPOOL` as `icon_0..4.png`, 16, 16, 32, 32 and
48 px). Resource ids below are decimal. Audio: 27 `WAVE` resources (167, 184-209, 2.5 KB to 47 KB each,
UI click/hover/exit sounds, M) were listed but not extracted.

| id | size | content |
|---|---|---|
| 105 | 640x480 | the whole main panel: metal frame, "MICROSOFT DEADLY TIDE", PILOT/ADD/DELETE/HELP/ABOUT, CONTROLS, GRAPHIC QUALITY, DIFFICULTY, VOLUME, EXIT, START in their idle state, black pilot field, flip-direction label |
| 104 | 386x346 | About picture (copyright, Rainbow / TRG3 logos, "Product ID:" line, legal text) |
| 110 | 386x1500 | scrolling credits strip: Microsoft, TRG3, Rainbow Studios, special thanks |
| 117, 129, 118, 120 | 103x23, 103x23, 93x23, 94x23 | ABOUT, HELP, ADD, DELETE (hover/lit state) |
| 152, 154 | 93x23, 94x23 | ADD, DELETE disabled/dim |
| 131, 151, 157 | 123x37-39 | MOUSE selected (blue), hover (green), bright variant |
| 130, 150, 182 | 123x37 | JOYSTICK selected, hover, dim (no joystick) |
| 119, 141, 153 | 113x31-32 | CALIBRATE normal, hover, disabled |
| 121-123, 126-128, 142-144, 147-149 | 70x70 | the I / II / III square buttons in four state sets (normal, dim, lit, hover); mapping decoded in §7b (H): GRAPHIC QUALITY row uses 126-128 (mid) and 147-149 (lit), DIFFICULTY row 121-123 (mid) and 142-144 (lit) |
| 125, 145, 146 | 34x36 | FLIP DIRECTION toggle states (red square, yellow-red W, black-red M) |
| 124, 155 | 19x22 | pilot drop-down arrow, normal and pressed |
| 132, 133, 134 | ~153x160 | EXIT flap frames; with 135-138 they are the seven sprites of the flap sequence (§7b) |
| 135-138 | ~156x255 | "ARE YOU SURE?" flap frames with the red YES button and small NO |
| 139, 140 | 74x23, 149x123 | NO button, YES cap |
| 161-164, 166 | 151x142 | START: 161-164 are the four frames of the post-click flip-book, 166 is the pressed look of the button (165 is not referenced by DT.EXE) (§7b) |
| 112-115, 111, 116 | 151x142 | CONTINUE: 112-115 flip-book, 116 pressed look, 111 its idle frame (§7b) |
| 158, 159, 160 | 25x65, 30x14, 33x18 | volume meter fill, slider knob, knob shadow/mask |
| 215 | 81x118 | VOLUME panel overlay |
| 172, 179 | 273x115 | "PILOT REGISTRATION:" dialog background; delete-confirm background |
| 170, 171, 173, 174, 175, 176, 177, 178 | 102x22 | dialog buttons: CANCEL, OK, NO, YES, each in two states |
| 106-109 | 5x5 to 15x15 | tiny indicator bitmaps (L, probably LED or cursor masks) |

**Layout** (pixels in the 640x480 picture; read off the bitmap, +-5 px except where marked H, exact sprite offsets are
in the layout code at 0x402c90 / 0x4027b8 / 0x403273; the 12-byte sprite-table entries and the placement coordinates are filled in by static initialisers (0x403f60, 0x404754, 0x4046ae) that Ghidra left undecompiled, now read, see §7b):

| element | x, y | size |
|---|---|---|
| title text | 122, 22 | |
| pilot field + arrow | 126, 65 (code, H) | 245 wide (field about 228 + arrow 19x22) |
| ADD, DELETE | 185, 42 / 280, 42 | 93x23, 94x23 |
| HELP, ABOUT | 413, 41 / 413, 65 | 103x23 |
| MOUSE, JOYSTICK, CALIBRATE | 134, 139 / 134, 178 / 139, 214 | 123x37, 123x37, 113x31 |
| FLIP toggle | 134, 247 | 34x36 |
| GRAPHIC QUALITY buttons | x 291, 363, 435; y 137 | 70x70 each |
| DIFFICULTY buttons | x 291, 363, 435; y 227 | 70x70 each |
| volume panel | 282, 338 | 81x118 |
| START / CONTINUE | 387, 318 | 151x142 |
| EXIT | 128, 300 | ~153x160 |
| dialogs | centred on the launcher | 273x115 (edit box at 25,38 about 225x21, buttons at 24,75 / 149,75) |

**Colours and fonts.** There is no font resource and no `CreateFont`: every label is part of a bitmap
(a squared sans like "Bank Gothic"; headings green `#52ffce`, labels white `#f7f7f7`, panel teal `#18394a`).
The pilot list box and name field use the Windows default GUI font with the colours `SetBkColor` = black
(0x42b25c, M) and `SetTextColor` = RGB(0, 128, 192) (0x42b260 = COLORREF 0x00C08000, M), and the list box takes
the pilot rows at the font's item height. Credits strip text uses yellow `#ffff00`, red `#ff0000`, green `#00ff00`
and light blue `#a6caf0` on the keyed navy. A recreation should use any similar sans font only for pilot names;
everything else can be the original bitmaps if rights allow, otherwise redrawn.

## 7. Unknowns

* (closed, see §7b) START/CONTINUE animation, I/II/III state sets, EXIT flap timing.
* Sprite coordinates: the I/II/III squares, MOUSE/JOYSTICK/FLIP/ABOUT are now exact (§7b); the rest are still read off the bitmap.
* (closed) The "Product ID" string: the ABOUT builder (0x4142f5) calls 0x411349, which reads the REG_SZ value `PID`
  (up to 100 chars) from the launcher's product registry key (the same key that holds `CurrentPlayer`, opened by 0x4118fb). Only if the value
  is absent or not a string does it fall back to the template `xxxxx-xxx-xxxxxxx`. Nothing in DT.EXE writes `PID`; it is presumably
  written by SETUP.EXE (M, SETUP not checked).
* (closed, see §7b) Which group triggers the restart prompt: GRAPHIC QUALITY only (H).
* Which of the 27 WAVE resources plays on which event.
* Behaviour of `RegDeleteKey` on names with `\`, and any name filtering in the edit box subclass at 0x408bcd.
* (closed, see §7a) `GraphicQuality` index versus the video mode.
* Not run under Wine: everything here is from the static decompile, the resource tables and the SETUP.EXE strings.

## 7a. GraphicQuality to video mode (`DD_PickMode` 0x40ba90, H, from the disassembly)

The registry value is read by 0x40cf00 (default 2, and any value above the limit 2 falls back to 2). `DD_Init` passes it to
`DD_PickMode`; values of 1000 or more are used directly as a mode code (a debug override). `EnumDisplayModes` (callback 0x40b940)
first sets one availability flag per mode: 320x200, 320x240, 640x400 and 640x480, each in 8 and 16 bits. `DD_PickMode` then fills a
three-entry list (index 2 = best) from the top, using the free video memory plus the current mode's size:

| mode code | size, depth | condition |
|---|---|---|
| 0x3fc | 320x240, 16-bit | available; takes slot 2 |
| 0x3fb | 640x480, 16-bit (flag 0x421588 = 1) | 640x480x16 available and memory at least 0x1c2000 bytes |
| 0x3fa | 640x480, 16-bit (flag 0x421588 = 0) | 640x480x16 available but memory below 0x1c2000 |
| 0x3f9 | 320x240, 8-bit | available (fills the remaining lower slots) |
| 0x3f8 | 640x480, 8-bit (flag = 1) | 640x480x8 available and memory at least 0xe1000 bytes |
| 0x3f6 | 640x480, 8-bit (flag = 0) | fallback fill for the lowest slots |
| 0x3f7 | 320x200, 8-bit | defined in `DD_Init` but never chosen by the picker (the 320x200 flags are set and then ignored) |

Typical result on a card with everything available: quality 0 (I) = 0x3f9 (320x240 8-bit), quality 1 (II) = 0x3fb (640x480
16-bit), quality 2 (III, the default) = 0x3fc (320x240 16-bit). With only 8-bit modes, all three indexes collapse to 0x3f9 (or to
0x3f8 / 0x3f6 when 320x240x8 is missing); if no usable mode exists the picker returns 0 and the game fails to start. The
render buffer is always 320x200; the mode only changes the presentation size and the pixel depth (`g_bpp`). All 640x480 modes carry a 40-row border (field 0x421558 = 0x28, 0x14 for 320x240), and the flag at 0x421588 only selects which back-buffer creation path `DD_Init` takes (1 = system-memory surface plus a lock pointer, 0 = plain surface); its exact role in presentation was not traced (L). Our engine ignores
`GraphicQuality` and always renders to a window, which is a deliberate simplification.

## 7b. Sprite button classes (recovered from the undecompiled constructors and static initialisers, DT.EXE)

Ghidra had not made functions of the static initialisers 0x403f60 / 0x404754 / 0x4046ae and of many vtable slots; they were
created with a headless script (tools/ghidra/ExportDecomp.java style) and read. All of this is H unless marked.

**Widget model.** Every button is a "flip-book": a list of frames added with the sprite-add virtual call (slot +0x20), a current
frame, a target frame and a tick sink that moves the current frame one step toward the target whenever 200 ms have passed
(constructor default 200 at +0x44 of the 0xb0-byte toggle class, +0x58 of the 0x8c-byte container class). Setting a frame (slot
0x40bef1 / 0x41a8d2) sets the target and the direction. The visibility virtual (+0x14) takes **1 = hide, 0 = show**.
Each sprite table entry is 12 bytes (bitmap id, x, y); an id of 0xffff in slot 0 means "capture the panel background (bmp105)
at the button rectangle as frame 0", so frame 0 is the idle look and is the panel itself.

**I/II/III squares** (toggle class, ctor 0x40ae50, tables at 0x42d2b8, placement 0x42cf38): three frames, 0 = idle (panel),
1 = mid sprite, 2 = lit sprite. Quality row (y 138): 126+i then 147+i; difficulty row (y 227): 121+i then 142+i; x = 291, 363,
435; sprites drawn at exactly that origin. Selecting animates 0 -> 1 -> 2 (400 ms), deselecting 2 -> 1 -> 0. Mouse-down on a
button immediately sets the target to the opposite of its current state (frame 2 if idle, 0 if lit) and the mouse-up commits it
(0x40b9ad / 0x40bb5a). There is no separate hover frame; the former "hover/dim" sets are just the mid and lit frames. The other
toggles use the same class: JOYSTICK 130 (idle) / 150, MOUSE 131 / 151 with idle 157 as frame 0 and JOYSTICK frame 0 captured,
FLIP 125 / 146 with frame 0 = 145 at (133,246), ABOUT 117 at (412,65), all pairs (mid, lit). Placement (H): MOUSE (133,138),
JOYSTICK (133,177), FLIP (133,246), ABOUT (412,65).

**Radio groups** (0x40225c): group 1 = the three quality squares (listener object 0x42cfb8, vtable 0x427108), group 2 = the
three difficulty squares (no listener), group 3 = MOUSE and JOYSTICK (listener 0x42cf2c, vtable 0x427100, slot 0x4053eb: enables
CALIBRATE when the selection is 1 = JOYSTICK). FLIP is a plain checkbox.

**Restart prompt.** The handler 0x405436 belongs to the listener of group 1, so it is **GRAPHIC QUALITY only**; the selection at
launcher start is stored in 0x42cfbc, and a change while the player window exists (0x415900) asks the restart question (0x415923);
Cancel restores the old selection. DIFFICULTY, controls and volume never prompt (they are re-read on WM_ACTIVATEAPP, SCENE_GRAPH
section 8).

**START and CONTINUE.** The text-button class (0xa8 bytes, ctor 0x409f42) has two frames: START 0 = panel, 1 = bmp166 pressed;
CONTINUE 0 = bmp111, 1 = bmp116 (both at (387,318)). Visibility (0x401f73): when the player window exists CONTINUE is shown
and START hidden, otherwise the reverse. On the click the launcher hides the button, starts the game
(0x405e5a: WinExec of the player, or SetForegroundWindow on an existing player window) and a full-window timer object (0x54 bytes,
ctor 0x40da8f, 3000 ms) runs; when it expires (0x405f0d) the matching 0x8c-byte container (START: frames 161-164, CONTINUE:
112-115, 200 ms per frame, ctor 0x419c67) is shown and set to its last frame, so it plays 161 -> 162 -> 163 -> 164 (600 ms,
M for the exact trigger) and rests on the bright frame 164 / 115. There is no hover animation; hover and glow cycling in the
engine were guesses. bmp165 is not used.

**EXIT flap** (0x405547 button handler, 0x4056a5 tick every >= 100 ms, six steps, then a final step). The seven sprites are
bmp132..138 in that order (index 0..6); a step shows one sprite and hides the previous one. *EXIT pressed* (mode 1): all seven
are hidden, then 134, 135, 136, 138, 137, 138 at 100 ms each, then the ARE YOU SURE content (panel 0x42d000 with YES and NO)
is shown. *NO pressed* (mode 0): the content is hidden and the flap goes back 136, 135, 134, 133, 133, 132 at 100 ms each,
ending on the idle (closed) panel. YES calls DestroyWindow on the launcher. Whether the hold frame is 138 or the panel itself
is M.

**Unresolved / L.** The exact z-order and rectangle of the flap sprites (the engine bottom-aligns them, M), and which event
resets the START/CONTINUE flip-book to frame 0 (the container is created at frame 0, M).

**DTPLAYER.EXE.** The same headless pass over DTPLAYER.EXE created 23 more functions, all CRT stubs, thunks and destructors;
no restart-prompt sprite group exists in the player (the prompt is the launcher's, above), so that item is closed.
