# Skate 3 audio: reverse-engineering notes

How Skate 3 (Xbox 360, title update 3.0.3.0) decides which skateboard sound to
play, recovered for the TF2 mod. All addresses are TU3 and match the
skate-engine annotations and the recompiled `skate3` binary's `sub_XXXXXXXX`
symbols.

## Getting a readable executable

The analysis needs `default.xex` (with title update 3's `default.xexp` applied) as a
decrypted, decompressed PowerPC image. Get it with any standard XEX tool, for example the
image that Xenia or XexTool produces. The tools here start from that image.

Other tools:
- `tools/re/callgraph.py`: `.pdata` function bounds and the direct-call graph.
- `tools/re/match_tu.py`: maps functions between builds.
- `tools/re/ghidra_scripts/DecompileFunctions.java`: decompiles chosen functions headless, without creating all 34k functions first.

AttribSys field hashes never appear as 8 contiguous bytes in the code. PowerPC builds them from four 16-bit immediates, so search for those within a short window of each other.

## Splicer banks (`.bnk`, magic `SPLC`, version 3)

| Offset | Contents |
|---|---|
| 0x08 | Size of the index after the 0x3C-byte header |
| 0x0C | `nSampleRefs` |
| 0x10 | `nClusters` |
| 0x18 | `nSamples` (audio streams, SNR/SNS EA-XMA, stored in index order) |
| 0x1C | Bank name |
| 0x3C | Sample refs, 36 bytes each: u16 index @4, u8 layer count @7, f32 length ms @0x18. The layer pointer @0x20 is filled at load; layers are stored back to back in ref order. |
| then | Clusters, 72 bytes each: u16 refs[32] @4 (0xFFFF = empty), u8 mode @0x44, u8 state @0x45 |
| then | Layers: 12-byte header (u8 sample count @8, u8 mode @9) followed by that many 72-byte sample descriptors |

Sample descriptor fields:

| Offset | Field |
|---|---|
| +0x00 | u16 sample index |
| +0x04 | gain (linear) |
| +0x08 | pitch ratio |
| +0x10 | pan/position offset |
| +0x14 | start delay (s) |
| +0x1C | duration (s) |
| +0x40 | play probability |

A **splice ID** below `nSampleRefs` is a sample ref; otherwise it is cluster `ID - nSampleRefs`. A cluster picks one of its refs (`0x82976DD8`, by its mode). A ref plays *all* its layers together, each layer picking one sample.

Runtime functions:

| Function | Role |
|---|---|
| `0x82975700` | Construct splice (bank, ID) |
| `0x829757D0` | Instantiate layers |
| `0x82975A60` | Play |
| `0x824958F0` | Queued request processor |

## Which splice the game plays

Splice IDs come from AttribSys collections (`skatercollections.vlt`; class layouts from `skaterschema.vlt` via `tools/asset_pipeline/vlt.py`). The skater audio component's update is `0x824B8218`. Its edge dispatcher `0x824B90D8` watches skater-state bytes +0x14C (airborne) and +0x155.

| Event | Code | Fields (collection class `C26949FCB638A2CA`) |
|---|---|---|
| Pop | `0x824B9CC8` → `0x824B9AD8` | `3C1E3B965A93594A` (hard ground) / `84DFABF76D821DEC` (wood); a 3-entry array indexed by pop strength (state +0x1D4 vs 0.25 / 0.42) |
| Takeoff rattle | `0x824B9CC8` | `9C4C…`/`537C…`/`7A74…` by speed (state +0xD0) over 4 / 8 / 12; nothing below 4 |
| Landing | `0x824BA630` | `F262…` / `3A2F…` (hard, airtime < / ≥ 0.75 s), `1E86…` / `BF22…` (wood) |
| Board impacts | `0x824BA3F0` | `5A93…`, `A0F8…`, `797E…`, `AB0D…`: 13-entry arrays [row × 3 + intensity] |
| Cloth foley | `0x824BBB28` | `sk82_cloth_foley` fields |

**Surface classes.** `Sk8::AudioSurfaceMap` (collection key `C489459A0C07D154`, array `4CA607558B1CF440`) has 95 records of 72 bytes, indexed by the 94 audio surface IDs (names in the engine's Blender add-on).
- +8: "hollow / wood ramp" class, used by `0x82494D78`.
- +0x18: grind category, used by `0x82494F58`.

**Grinds.** Grind start `0x824E9FD8` → `0x82493E60` (categories 5 and 7 have direct fields) or → `0x824975D8`. The latter uses a per-surface table at `0x8302D6E8` (16-byte entries: u32 bank selector, u64 collection key) into class `D40CB4C0FFE45676`:

| Bank | Start | Loop | Board slide |
|---|---|---|---|
| Skate_Collisions | +0x40 | +0x3C | +0x68 |
| Skate_Metal | +0x50 | +0x4C | +0x54 |

Byte +0x7D gates whether the surface has grind sounds. `0x82493690` adds cloth-foley layers per category.

## AEMS modules (`.csi` + `.abk`), partly mapped

The skateboard's continuous sounds run on EA's AEMS. Code sends messages and parameters to a
module instance, and the module's bytecode picks patches and samples.

**Interfaces (`MOIR` `.csi` files in `audiofiles.big`).** These name each module's
classes, messages and parameters. `SK8_AEMS_skateboard.csi` declares:
- Classes: `Class_grind`, `Class_wheels_skid`, `Class_foot_drag`, `Class_Flips`,
  `Class_wheels_flip`, `Class_Squeaks`, `Class_Seams`, `Class_Treatment`,
  `Class_pre_lands_whsh`, `Rolling_Rattle_Class`.
- Messages: `play_grind_start/stop`, `play_wheelskid_start/stop`, `play_FT_drag_start/stop`,
  `play_rolling_start/stop`.
- Sample selectors: `taildrag_cnc`, `taildrag_wood`, `body_slide_cnc`, `body_slide_wood`,
  `wheelskid_*`, `FootDrag_*`, `soft_wheelskid_*`, `seams_swt_*`, `SenseOfSpeed_*`.

`SK8_AEMS_rolling.csi` declares the rolling class and its parameters (`Metal_type`,
`Metal_wheel`, `GrassDirtVolume`, `Low/Med/High` ranges).

**Banks (`ABKC`, big endian).** GRINDS.abk is the example:

| Offset | Contents |
|---|---|
| 0x14 | File size |
| 0x18 | Header size (0x5C) |
| 0x20 | Size of the module section that starts at 0x5C (0x5C00) |
| 0x24 | Offset of the trailer (0x1F0E84) |
| 0x30 / 0x34 / 0x38 | Trailer pointers: patch table, count, module name |
| 0x80.. | Module bytecode: records `op u8, argc u8, 0000, ffffffff, args…` (ops 0x0F…0x24), then patch definitions (gains as 0x7FFF, times in ms) |
| 0x5C04 | Sample offset table: one u32 per sample, relative to the audio data, 0xFFFFFFFF padded |
| after | EA-XMA streams (vgmstream decodes them as subsongs: 123 in GRINDS) |
| 0x1F6A88 | Trailer: u32 patch count (18), patch offsets, then a hash and the class name (`Class_grind`) |

So each `.abk` holds one AEMS class: its bytecode, its patches and its samples.
Still to do: decode the bytecode ops (the AEMS VM in TU3; start from the "Aems Modules Int"
string references) to learn which parameter values (surface, speed) select which patch, and
which samples each patch uses.

## Not mapped yet

- Concrete-ledge grind and slide scrapes, board scrapes, wheel skids and tail drags. These are the AEMS modules above; their bytecode is not decoded yet.
- Body bails (class `923CCB46EF5BF5BA`, emitters `0x824C0350`, `0x824EBB58`, …).
- Flip whooshes (`Sk82_Whsh_Bys`, `0x824D2B50`).

The mod's sound picker (`tools/skate_sound_picker.py`) covers these until they are mapped.
