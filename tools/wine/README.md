# Running the original under Wine (reference rig)

Tested on Ubuntu 24.04, Wine 9, headless (Xvfb). What it takes:

1. Fake the install in the registry, `HKLM\Software\Microsoft\Microsoft Games\Deadly Tide\1.00`:
   `AppPath`=`C:\DT\`, `GamePath`=`D:\`, `PID`=any, `CurrentPlayer`=a pilot name, `Title`=`Deadly Tide`.
   Copy `DT.EXE`, `DTPLAYER.EXE`, `DTLAUNCH.DLL` (and `DT.HLP`) to `C:\DT`.
2. CD drive: symlink `dosdevices/d:` to the extracted CD folder and remove any auto-created `d::` device
   link (on DigitalOcean it points at the cloud-config disk). Put the volume label in `.windows-label`
   in the CD folder (`DEADLYTIDE1`; the launcher only checks the `DEADLY` prefix).
3. **Native Cinepak codec is required:** copy the `ICCVID.DLL` from CD1 to `system32`, set the DLL override
   `iccvid=native` and `Drivers32\vidc.cvid=iccvid.dll`. The engine pokes into the codec's instance
   data, so Wine's builtin `iccvid` crashes it (null read at 0x417780 in DTPLAYER).
   Copy it **after** `wineboot`/`regedit`: those can put the builtin back in `system32`.
4. An audio device must exist (timing runs off DirectSound): headless, a PulseAudio `module-null-sink` works.
5. Run `DT.EXE` inside `wine explorer /desktop=DT,640x480`; click START. The engine switches to 320x240.

`play.sh` automates launch + click + screenshots under `xvfb-run`.

Reaching LEVEL01 combat in the original: create a fresh pilot (a new `CurrentPlayer` name with no
`HighestLevel`). It starts at LOGOS and runs through the films into LEVEL01 by itself. Don't press Esc
during the film or loading screens; it can quit to the launcher.
