# Deadly Tide `.L96` container format

Status: the container layout is complete and verified. Many record *contents* are still unknown.
Reader and checker: `tools/l96.py` (`info` / `check`).

All integers are little endian. A "sector" is 0x800 bytes. Function addresses refer to
`DTPLAYER.EXE` (the game engine, MSVC, Aug 1996).

## 1. Overall layout

```
0x0000  header (0x94 bytes)
        linked file names          8 x n_names
        RGB555 -> index table      0x8000
        RGB565 -> index table      0x10000
        palette                    256 x RGB (0x300)
        sound effects              n_sounds records
        sprite / animation sets    n_sprites records
        shade tables               2 x n_shade1 x 0x20000
        shade2 tables              2 x n_shade2 x 0x2000
        segment table              n_segments x 100
        table44                    n_tbl44 x 0x44
        table54                    n_tbl54 x 0x54
        table28                    n_tbl28 x 0x28
        stream index               n_index x 8
        zero pad to a sector boundary
stream  sector-aligned groups, up to EOF
```

The sections follow each other with no gaps, in exactly this order, in every shipped file.
Empty sections keep the running offset. The stream starts at the first sector boundary after the index.

The header is read in one 0x94-byte `fread` by the level loader **FUN_00413c40**. That
function also loads every table through the offset/count pairs below. The stream is opened
again with `CreateFileA` (unbuffered) in **FUN_00414420** and read by a separate thread
(**FUN_00401650** and helpers).

## 2. Header (0x94 bytes)

| off | type | name | meaning | evidence |
|---|---|---|---|---|
| 0x00 | char[4] | magic | `L96\0` | compared in FUN_00413c40 |
| 0x04 | char[8] | name | file name without extension, e.g. `LEVEL01` | uppercased; compared with "Level" in FUN_00414420 and "Level00" in FUN_00410a90 |
| 0x0c | u32 | version | must be 5 | FUN_00413c40 |
| 0x10 | u32 | unk10 | unknown (0, 0x10..0x17). Not referenced by the engine | — |
| 0x14 | u32 | unk14 | unknown, 0. Not referenced | — |
| 0x18 | u32 | n_names | number of linked file names | FUN_00413c40 (read count); FUN_00401650 (use) |
| 0x1c | u32 | n_sounds | number of sound-effect records | passed to FUN_00414e90 |
| 0x20 | u32 | n_sprites | number of sprite/animation sets | loop count in FUN_004022b0 |
| 0x24 | u32 | tbl28_flag | if 0 and n_tbl54≠0, each table28 record is rewritten at load (byte2=byte0, byte0-=n_tbl54/2, byte1=byte0) | FUN_00413c40 |
| 0x28 | u32 | unk28 | unknown, 0. Not referenced | — |
| 0x2c | u32 | n_segments | segment records (100 bytes) | FUN_00413c40 |
| 0x30 | u32 | n_tbl44 | 0x44-byte records | FUN_00413c40 |
| 0x34 | u32 | n_tbl54 | 0x54-byte records | FUN_00413c40 |
| 0x38 | u32 | n_tbl28 | 0x28-byte records (always 360 in shipped files) | FUN_00413c40 |
| 0x3c | u32 | n_index | 8-byte stream index entries | FUN_00413c40 |
| 0x40 | u32 | off_names | offset of linked names (always 0x94) | FUN_00413c40 |
| 0x44 | u32 | off_lut555 | 0x8000-byte table, used when the display is RGB555 | FUN_00412f50 |
| 0x48 | u32 | off_palette | 256×RGB palette (0x300 bytes) | FUN_0040c130 (reads 3×256 bytes; Ghidra lost the offset argument, the value is confirmed by the data) |
| 0x4c | u32 | off_sounds | sound-effect records | FUN_00414e90 |
| 0x50 | u32 | off_sprites | sprite/animation sets | FUN_004022b0 |
| 0x54 | u32 | unk54 | 0 in all files, not referenced | — |
| 0x58 | u32 | unk58 | 0 in all files, not referenced | — |
| 0x5c | u32 | off_segments | segment table | FUN_00413c40 |
| 0x60 | u32 | off_tbl44 | table44 | FUN_00413c40 |
| 0x64 | u32 | off_tbl54 | table54 | FUN_00413c40 |
| 0x68 | u32 | off_tbl28 | table28 | FUN_00413c40 |
| 0x6c | u32 | off_index | stream index | FUN_00413c40 |
| 0x70 | char[4] | fourcc1 | `cvid`: codec FourCC handed to the VfW decompressor setup | FUN_00417520 (called from FUN_00413c40) |
| 0x74 | char[4] | fourcc2 | `cvid` (second FourCC argument to the same call) | same |
| 0x78 | u32 | off_lut565 | 0x10000-byte table, used when the display is RGB565 | FUN_00412f50 |
| 0x7c | u8 | n_shade1 | number of 0x20000-byte shade tables (engine clamps to 4) | FUN_0040d960 |
| 0x7d | u8 | n_shade2 | number of 0x2000-byte shade tables (engine clamps to 4) | FUN_0040d960 |
| 0x7e | u16 | — | 0, unused | — |
| 0x80 | u32 | off_shade | shade tables: n_shade1 RGB555 tables, then n_shade1 RGB565 tables | FUN_0040d960 |
| 0x84 | u32 | off_shade2 | shade2 tables: n_shade2 RGB555, then n_shade2 RGB565 | FUN_0040d960 |
| 0x88 | u8 (u32) | special_count | starting value of a per-level counter that goes down by one each time a special action fires (0 in LEVEL00, 5 in levels). Probably a special weapon's ammo | read in FUN_00405f30/FUN_00406020, passed to FUN_004165d0; decremented in FUN_004155b0 |
| 0x8c | u32 | unk8c | 0, not referenced | — |
| 0x90 | u32 | unk90 | 0, not referenced | — |

## 3. Sections between the header and the stream

| section | size | contents / what the loader does |
|---|---|---|
| names | 8 × n_names | NUL-padded 8-char file names, without extension. A segment whose `next` is negative ends the file: the engine loads `names[-1-next]` + `.L96`. The special name `SUCCESS` ends the game run (FUN_00401650). LEVEL00 (the hub) lists 16 names. |
| lut555 | 0x8000 | One byte per 15-bit colour, so most likely an RGB555 → palette-index inverse colour map. Loaded by FUN_00412f50 when the green mask is 0x3e0. |
| lut565 | 0x10000 | Same for RGB565 (one byte per 16-bit colour). Loaded instead of lut555 on 565 displays. |
| palette | 0x300 | 256 × (R,G,B) bytes. The ends hold the Windows system colours. |
| sounds | variable | n_sounds × { 0x2c-byte header, data }. Header u32 @+8 = data size. The other header bytes are unknown and look like leftover memory from the authoring tool. The data is u8 mono PCM at 22050 Hz (FUN_004146c0 sets up the DirectSound buffers as 1 ch / 22050 / 8 bit). Loaded by FUN_00414e90. |
| sprites | variable | n_sprites × { 16-byte header: char[4] tag (`EX `…), u32 count, u32 size, i16 type, i16 unk; then `size` bytes }. The payload starts with `count` 0x58-byte frame descriptors (+4 = data offset relative to the payload, +0x14/+0x16 = i16 width/height), followed by pixel data. Types 0..4 sort the sets into five engine lists. Types −3/−4 hold compressed pixels, which FUN_00402600 expands at load through FUN_00410ae0. Loaded by FUN_004022b0. |
| shade | 2×n_shade1×0x20000 | Colour-transform tables (64K 16-bit entries each), probably for blending or tinting sprites. Selected per draw by FUN_0040db00 (index ≥ 0x80). Sprite blend tables, see GAMEPLAY_DATA §8. |
| shade2 | 2×n_shade2×0x2000 | Smaller colour tables, selected by FUN_0040db00 (index < 0x80). Gun flash and shot streak tints, see GAMEPLAY_DATA §8. |
| segments | 100 × n | See §4. |
| table44 | 0x44 × n | Unknown (1 record in LEVEL01). Read verbatim into a buffer by FUN_00413c40. |
| table54 | 0x54 × n | Unknown (15 in LEVEL01). The first record bytes look like small index lists. Read verbatim. |
| table28 | 0x28 × n | Unknown (always 360). FUN_004021a0 reads it, indexed by a short from per-object data. Bytes 0..3 are modified at load when tbl28_flag = 0. |
| index | 8 × n | Stream index, see §5. |

## 4. Segment record (100 bytes)

A segment is a run of consecutive index entries that plays as one piece of video. The graph of
segments (`next`, plus alternative branches somewhere in the unknown bytes) is the level's path.

| off | type | meaning | evidence |
|---|---|---|---|
| 0x00 | i32 | type: 0 = plain video, 1 = video with gameplay data, 6 = sequence with no video (FG data + audio only). The code also handles 2 and 5, which no shipped file uses. | switch in FUN_00401650 |
| 0x04 | i32 | flag: 2 = stored normally; 1 = the head of the segment is prefetched elsewhere (see §6) | FUN_00401da0, FUN_00401650 |
| 0x08 | u32 | file offset of the segment's stream data (see §6 for flag 1) | SetFilePointer in FUN_00401da0 / FUN_00401650 |
| 0x0c | i16 | first index entry | FUN_00401650 |
| 0x0e | i8 | next segment; negative = go to linked file `names[-1-next]` | FUN_00401650 |
| 0x0f | u8 | number of index entries | FUN_00401650 |
| 0x10..0x54 | | unknown. Gameplay and branch parameters; varies by type | — |
| 0x55 | u8 | flag tested when a segment starts (affects checkpoint / progress handling; exact meaning unknown) | FUN_00401650, FUN_00406020 |
| 0x56..0x63 | | unknown | — |

## 5. Stream index entry (8 bytes)

One entry describes one **group**. A group is normally 5 video frames plus 1/3 s of audio.

| byte | meaning |
|---|---|
| 0 | `nv`: video sectors |
| 1 | `nf`: FG (gameplay) sectors |
| 2 | `nc`: carry sectors (prefetch for another group, §6) |
| 3 | `na`: audio sectors (8 in every shipped group) |
| 4..7 | always 0 |

Group size = nv+nf+nc+na sectors. Evidence: FUN_00401000 and FUN_004013c0 read
`(b0+b1+b2+b3)` sectors per entry. FUN_00401000 sends the last `b2` sectors to a second
buffer. The consumer FUN_004067b0 finds video at `b3*0x800` and FG data at
`(b0+b3)*0x800`, and FUN_004071e0 / the routine after it copy carry data from `(b0+b1+b3)*0x800`.

### Layout inside a group

```
[ audio : na sectors ][ video : nv sectors ][ FG : nf sectors ][ carry : nc sectors ]
```

* **audio**: 14700 bytes of u8 stereo PCM at 22050 Hz (= 1/3 s), zero padded to `na` sectors.
  The engine's streaming buffer is 2 ch / 22050 / 8 bit (FUN_004146c0); FUN_00415060 queues it.
* **video**: exactly 5 records `{ 0x44-byte frame header, Cinepak frame }`. Header u32 @0 =
  Cinepak frame length, which equals the 24-bit big-endian length inside the Cinepak frame.
  Bytes 4..0x43 are per-frame data (contents not covered here; u32 @+0x20 is a marker that differs
  between a group and its carry copy, see §6). All frames are 320×200. **Bytes after the 5th
  frame up to the end of the video part are leftover junk from the mastering tool, not zero**.
  Do not scan past 5 frames. Sometimes nv is one sector larger than needed.
* **FG**: gameplay / foreground block. Starts with the magic `FG \0`, then u32 count, u32 …
  (not decoded yet). Present in type-1 and type-6 segments.
* **carry**: raw sectors that belong to another group (§6).

## 6. Physical placement of groups (prefetch / branching)

To branch without seeking on a 2x CD, the authoring tool copies the start of a branch target
into the `carry` tails of the groups played just before it. Three cases cover every shipped file.

1. **flag 2** segments: entries are stored back to back from `offset`.
2. **flag 1, type 0/1** segments: also stored back to back from `offset`. In addition,
   entry 0 is duplicated in the carry sectors of the segment that branches into it, collected in
   entry order. When the engine enters such a segment from the prefetch buffer it seeks to
   `offset + (b0+b1+b3 of entry 0)*0x800` and continues from entry 1 (FUN_00401da0). Copies are
   byte-identical except, in one LEVEL02 case, the u32 at +0x20 of the frame headers.
3. **flag 1, type 6** segments (constant 3 at DTPLAYER .data 0x4210f0; type 2 would use 6 from
   0x4210e4):
   * entries 0 and 1 are stored directly in front of `offset`, after the previous segment;
   * entry 2 is **virtual**: its bytes are the carry sectors of entries 0 and 1, concatenated;
   * entries 3 … n−2 are stored back to back from `offset`;
   * the last entry is **virtual**: its bytes are the carry sectors of the stored entries before it
     (normally the last two).
   FUN_00401650 skips the first 3 entries and seeks to `offset`. FUN_00402070 / FUN_004013c0
   read entries 0..1 at the end of the previous segment.
4. The 20 groups before each type-6 segment (constant 20 at 0x4210ec) carry 15+…+15+3 = 288
   sectors (0x90000 bytes). The engine collects these into a separate buffer (FUN_004071e0,
   switch cases 3/4). The data starts with a 0x44-byte header and a **128×128** Cinepak key frame (dark
   water texture). What it is for is not determined yet.

With these rules, the stored groups cover the stream exactly once from `stream_start` to EOF in
28 of the 29 shipped `.L96` files (layout-only check over all four CDs; full content walk on the files in §7). The exception is cd1 `LEVEL02.L96`: 118 sectors at
0xc1f6800..0xc231800 that no entry references (they look like audio + FG data). This is probably
an orphan left by the authoring tool.

## 7. Verified counts (`tools/l96.py check`)

| file | segments | groups (virtual) | video frames | audio blocks | FG blocks | carry blocks | inconsistencies |
|---|---|---|---|---|---|---|---|
| cd1/LOGOS | 1 | 88 (0) | 440 | 88 | 0 | 0 | 0 |
| cd1/LEVEL00 | 18 | 213 (2) | 845 | 213 | 5 | 24 | 0 |
| cd1/LEVEL01 | 28 | 1221 (8) | 4305 | 1221 | 856 | 105 | 0 |
| cd4/LEVEL14 | 27 | 1044 (8) | 4290 | 1044 | 759 | 96 | 0 |
| cd1/LEVEL02 | 33 | 2071 (12) | 8855 | 2071 | 1306 | 220 | 0 (2 warnings: orphan sectors, header-only copy diff) |

Every group that has video holds exactly 5 frames. All sampled frames decode with ffmpeg.

## 8. Open questions

* Segment record bytes 0x10..0x54 and 0x56..0x63 (branch targets, gameplay parameters).
* table44 / table54 / table28 contents; FG block structure; sprite descriptor fields beyond offset/size.
* The 0x44 per-frame header fields (only the size and the +0x20 copy marker are known).
* What the 288-sector type-6 preload (128×128 Cinepak) is used for.
* Header 0x10/0x14; exact role of the shade tables and of lut555/565.
