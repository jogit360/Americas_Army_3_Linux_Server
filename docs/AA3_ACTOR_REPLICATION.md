# AA3 Actor Replication — Phase 2 Research

Research notes for the post-JOIN phase of the AA3 protocol: the server
opens a stream of `ChType=2` (CHTYPE_Actor) channels and ships the world
state to the client. This is what the legit server (`GamePlayPCAP_Legit_Server.pcapng`
frames 72+) does after the package-manifest handshake completes.

This document cross-references three sources:

1. **UE3 engine reference** — `UnrealEngine3/Development/Src/Engine/Src/UnChan.cpp`
   and `UnrealEngine3/Development/Src/Engine/Src/UnNetDrv.cpp`. AA3 ships
   on UE3, so the engine-level wire format is the same.
2. **AA3-specific decomp** — `OnlineSubsystemAA3/uscript_decompiled/`
   gives us the actual property lists on `AA_PlayerController`,
   `AA_GameInfo`, `AA_GameReplicationInfo`, etc.
3. **Captured packets** — `GamePlayPCAP_Legit_Server.pcapng` frames 72-80
   (legit) and `LatestIterationTestComms5.pcap` frame 3289+ (our server
   sitting idle after JOIN, waiting for this phase).

## 1. When this phase begins

The client sends `"JOIN"` as the last FString in its HAVE-bunch storm
(see [`aa3_client_storm_format.md`](../../.claude/projects/-home-odeyin-Documents-AA3/memory/aa3_client_storm_format.md)).
After JOIN, the legit server emits frame 72 (`PacketSeq=49`) with the
first three actor channels (`Ch=2`, `Ch=2` update, `Ch=3`). In our pcap5
the client just pings empty 10-byte `PacketSeq`-only packets forever,
waiting for this stream.

## 2. UE3 actor-channel bunch format

From `UnChan.cpp::UActorChannel::ReceivedBunch` (line 1359):

```
On an OPEN bunch (bOpen=1, first bunch on a new channel):
   1. UObject*& Object      — read via PackageMap->SerializeObject
   2. If Object is an archetype (transient actor):
        SerializeCompressedInitial(Bunch, Location, Rotation, bInitialRotation)
   3. If Object is a PlayerController:
        Bunch << PC->NetPlayerIndex     // 1 byte

On EVERY bunch (open or update), property replication loop:
   loop:
      RepIndex = Bunch.ReadInt(ClassCache->GetMaxIndex())
      if RepIndex == MaxIndex: end-of-bunch (sentinel)
      FieldCache = ClassCache->GetFromIndex(RepIndex)
      if FieldCache is a UProperty:
         if ReplicatedProp->ArrayDim > 1: read 1-byte Element index
         ReplicatedProp->NetSerializeItem(Bunch, PackageMap, Data)
      if FieldCache is a UFunction:
         // RPC handling — read parameters via Function->FunctionMapPropertyList
         …
```

### 2.1 UObject reference encoding (`UPackageMapLevel::SerializeObject`)

Defined in `UnNetDrv.cpp:97`:

```
bDynamic : 1 bit
if bDynamic:
   ChannelIndex : RIM(MAX_CHANNELS=2048)        // 11-bit RIM, refers to an existing actor channel
else:
   ObjectIndex  : RIM(MAX_OBJECT_INDEX=1<<31)   // up to ~31-bit RIM, static IndexToObject() lookup
```

`UPackageMapLevel` is used for non-seekfree (i.e., PC) builds — see
`UnConn.cpp:281`. The `IndexToObject` table is built from all objects in
the loaded packages, indexed in load order. **This is why the package
manifest handshake matters so much**: both client and server must build
the same index table from the same package list in the same order so
that `ObjectIndex=172588` resolves to the same actor on both sides.

(Console builds with `GUseSeekFreePackageMap=1` use `UPackageMapSeekFree`
which serializes 32-bit `ChannelIndex` plus an FString path name for
static objects. Verbose, easy to read, but bigger packets. AA3's PC
client almost certainly uses `UPackageMapLevel`.)

### 2.2 `SerializeCompressedInitial` (Location + Rotation)

Used for transient actors (those spawned from a class default rather
than placed in the level). Encodes:

- `Location` : 3 × FLOAT32 with optional quantization (UE3 has a 24-bit
  packed format when bandwidth-saving but the default is full floats)
- `Rotation` : 1 bit `bRotationNonZero`; if set, 3 bytes (Pitch, Yaw, Roll
  as 8-bit packed angles)

Exact bit layout TBD — needs to be verified against the wire data once
we identify a transient actor in the legit pcap.

### 2.3 RepIndex encoding

`ClassCache->GetMaxIndex()` returns `RepProperties.Num() + NetFields.Num()`
— the count of all replicated properties plus all replicated functions
(RPCs) on the class. Both server and client build the same cache from
the class metadata in the loaded packages. `MaxIndex` is class-specific
and isn't on the wire — both sides must agree.

The sentinel "end of bunch" is `RepIndex == MaxIndex` (the read returns
the max value, no field is fetched, the loop exits).

## 3. Legit pcap frame 72 — first ChType=2 packet

Decoded from `GamePlayPCAP_Legit_Server.pcapng`:

```
PacketSeq=49
  [0] Bunch ch=2  OC=1 O=1 R=1 ChSeqRaw=1 ChType=2 BDB=2367  -> opens Ch=2 with a huge payload
        bDynamic=0, ObjectIndex=172588  (32-bit class reference)
        + 2335 bits of class spawn + property replication
  [1] ACK 20
  [2] ACK 21
  [3] Bunch ch=2  OC=0 O=0 R=1 ChSeqRaw=2 ChType=2 BDB=110   -> property update on Ch=2
  [4] Bunch ch=3  OC=1 O=1 R=1 ChSeqRaw=1 ChType=2 BDB=50    -> opens Ch=3 with tiny payload
        bDynamic=0, ObjectIndex=68101
        + 18 bits of "no replication, just spawn" sentinel tail
```

### 3.1 Channel-to-class hypothesis

Cross-referencing with what UE3 spawns on a fresh client connection
(see `UnConn.cpp` / `UnPenLev.cpp`) and the relative payload sizes:

| Ch | BDB    | ObjectIndex | Likely class                       | Why                                                                                 |
|----|--------|-------------|-------------------------------------|-------------------------------------------------------------------------------------|
| 2  | 2367   | 172588      | **WorldInfo** (`AA3WorldInfo`?)     | First actor replicated is always WorldInfo; biggest because it carries level config |
| 3  | 50     | 68101       | **GameReplicationInfo** (`AA_GRI`)  | Second; tiny because it's mostly empty until match starts                           |
| 4  | 3570   | 49290       | **PlayerController** (`AA_PC`)      | Biggest payload in frame 73; spawn+rotation+ownership                               |
| 5  | 50     | 56432       | **PlayerReplicationInfo** (own PRI) | Empty until PC writes name etc.                                                     |
| 6,7 | 95   | 70992 (same)| Same class — looks like TeamInfo    | Two slots, same class, low ObjectIndex repeated                                     |
| 8,9 | 95   | 70992 (same)| Same TeamInfo class                 | Frame 74 — bot/spectator teams maybe                                                |
| 10 | 594    | dyn         | LevelStreaming volume / GameMode?   |                                                                                     |
| 11-16, 18 | 50-159 | various | Match-info actors, Volumes      |                                                                                     |

This needs verification by computing the actual `IndexToObject` table
for our package set (see §5 below).

## 4. AA3-specific replicated property lists

From the `OnlineSubsystemAA3/uscript_decompiled/` tree. The decompiler
(UELib) trips on AA3's custom replication condition opcodes and prints
exception text in place of the `if(...)` conditions, but the property
*lists* themselves are preserved. These are the things that flow over
the wire on the spawn bunches:

### 4.1 `Engine.WorldInfo` (line 347 of WorldInfo.uc)

```
replication {
    if(...) Pauser, ReplicatedMusicTrack, TimeDilation, WorldGravityZ,
            bHighPriorityLoading;
}
```

5 properties. Plus the inherited `Actor` replication (Role, RemoteRole,
Location, Rotation, etc., conditioned on `bNetInitial`).

### 4.2 `Engine.GameReplicationInfo` (line 37 of GameReplicationInfo.uc)

```
replication {
    if(...) MatchID, Winner, bMatchHasBegun, bMatchIsOver, bStopCountDown;
    if(...) RemainingMinute;
    if(...) // more — file continues past read window
}
```

### 4.3 `AA3Baseline.AA_PlayerController` (line 463 of AA_PlayerController.uc)

A *much* longer list — needs separate read. The `repnotify`-marked
properties at the top of the class show what notifications the client
hooks into:

```
var repnotify AA_GameReplicationInfo.eSpectatorMode SpectatorMode;
var repnotify AA_GameReplicationInfo.eSpectatorModePrevNext_Type
              SpectatorModePrev_Type, SpectatorModeNext_Type;
var repnotify string LeftSpectateInfoString, RightSpectateInfoString;
var repnotify bool   bVOIPEnabled;
// + the full replication{} block at line 463
```

To extract: read `OnlineSubsystemAA3/uscript_decompiled/AA3Baseline/AA_PlayerController.uc`
around line 463 once the decompiler-exception noise is filtered.

## 5. ObjectIndex resolution

### 5.1 The algorithm (per `UnCoreNet.cpp` lines 210-540)

```
struct FPackageInfo {
    FName    PackageName;          // "Core", "Engine", "AA3Baseline", ...
    FGuid    Guid;                 // matches FULL_MANIFEST GUIDs
    DWORD    ObjectBase;           // running cumulative sum of ObjectCount
    DWORD    ObjectCount;          // number of net-serializable objects in this pkg
};
class UPackageMap { TArray<FPackageInfo> List; };  // package load order matters
```

`ObjectToIndex(Object)` = `Object->NetIndex + List[Object->Outermost].ObjectBase`.

`IndexToObject(InIndex)` iterates `List`: when `InIndex < ObjectCount` for
the current package, return `Parent->GetNetObjectAtIndex(InIndex)`;
otherwise `InIndex -= ObjectCount` and continue. (See `UnCoreNet.cpp:474-540`.)

So for every package we need:
1. `ObjectCount` — total count of net-serializable objects in the package.
2. For each net-serializable object: its per-package `NetIndex` and what
   it actually is (class, full name).

### 5.2 Where to get the data

`Object->NetIndex` assignment (per `UObject::SerializeNetIndex`,
`UnObj.cpp:1565`):
- **Cooked packages** (AA3 ships these): `NetIndex` is the **first INT32**
  of the object's serialized body. The export entry's `SerialOffset`
  points at the start of that body.
- **Uncooked packages**: `NetIndex = _LinkerIndex` (the export-table position).

`ObjectCount` per package: stored in `FObjectExport::GenerationNetObjectCount`
on the package's top-level export (`UnLinker.cpp:287` serializes it).

### 5.3 What we built: `aa3-tools/PackageMapDumper`

A small .NET console tool that uses `Eliot.UELib.dll` to walk each
package's export map and emit `(package_name, net_index, global_index,
class, full_name)` tuples to JSON. Driver:

```
PackageMapDumper <manifest.txt> <package-dir> <out.json>
```

`manifest.txt` is the package-name list in `FULL_MANIFEST` order (the
build already extracts this — see `aa3-tools/PackageMapDumper/manifest.txt`
in the tree). The tool tracks `ObjectBase` itself, so the JSON already
gives global indices.

### 5.4 Status of the tool — what works, what doesn't yet

**Works:**

- Loads `*.u`/`*.upk` packages via UELib without `InitializePackage` so we
  dodge the `System.Windows.Forms` dependency on Linux.
- Walks the export map, records `ClassName`, `ObjectName`/path, file
  `SerialOffset` and `SerialSize`.
- Computes cumulative `ObjectBase`, emits monotonic `global_index`.
- Correctly skips and reports compressed packages instead of producing
  garbage indices.
- End-to-end on uncompressed packages (e.g., `EngineFonts.upk`): exact
  match between table order and `net_index`.

**Doesn't yet:**

- **AA3 packages are LZO-compressed AND XOR-obfuscated**. UELib can read
  the *summary* of these but not the export table. Use the existing
  pre-staged pipeline:
  ```
  ~/lzoenv/bin/python3 aa3-tools/aa3_decompress.py  in.u  tmp.decomp.u
  ~/lzoenv/bin/python3 aa3-tools/aa3_deobfuscate.py tmp.decomp.u tmp.clean.u
  ```
  …then point the dumper at a directory of `*.clean.u` files. Verified
  working on `AA3Baseline.u` → 66,443 exports.
- **Per-object `NetIndex` accuracy is uncertain.** For cooked packages
  UE3 reads `NetIndex` as the first INT32 of each object's body. Our
  dumper does that, but the values come back with duplicates and many
  zeros, which means at least some AA3 objects don't follow the standard
  cooked layout (or the deobfuscation pass adjusted offsets in a way we
  haven't fully accounted for). A short investigation likely closes this
  — see §5.5.
- Six packages from the manifest aren't on disk under
  `AA3/AA3Game/CookedPC/` (`SoundNodeVOIP_Test`, `Editor_UI`, `TestSAI`,
  `Island_Assets`, `MCEditorResources`, `Editor_AISupportIcons`). Need
  to confirm where these live or whether they're optional.

### 5.5 Verification — six of eight legit ObjectIndexes resolve cleanly

After bulk-decompressing the 11 LZO+XOR packages into `clean_packages/`
and reading authoritative `ObjectCount` from each package summary's
`Generations[last].NetObjectCount`, lookups land in the right package:

| Channel / BDB | ObjectIndex | Resolved class (with `−19` offset) |
|---|---|---|
| Ch=3 / 50    | 68101 | `AA_SteamStatManager`        |
| Ch=4 / 3570  | 49290 | `AA_GameReplicationInfo`     |
| Ch=5 / 50    | 56432 | `AA_MOSQualificationBCT`     |
| Ch=6,7,8,9 / 95 | 70992 | `AA_TeamInfo` (×4 teams)  |
| Ch=10 / 594  | 60069 | `AA_PlayerReplicationInfo`   |
| Ch=18 / 159  | 65849 | `AA_PlayerStorageProxy`      |

These are **exactly** the actors you'd expect a UE3 server to spawn on
client world entry — clear confirmation that:

- Our load order matches the legit client's view (FULL_MANIFEST is right).
- Cumulative `ObjectBase` is correctly computed via `Generations[last].NetObjectCount`.
- The export-table position of a class equals its per-package NetIndex
  (for the cooked AA3 build).

The **constant `−19` offset** means there are 19 net-serializable objects
in the client's package map ahead of `Core`'s first object. Our current
working hypothesis: a built-in UE3 preamble (TransientPackage + a few
system classes/reflectors). To pin down the exact 19 objects, attach to
a running AA3 server and dump `UPackageMap::List[0].ObjectBase`, or
inspect `UPackage::GetNetPackages()` defaults.

### 5.6 Bridge_night_clear.aa3 confirmed to hold the right content

Direct inspection of `Bridge_night_clear.aa3` (after the LZO/XOR clean
pipeline) shows it absolutely contains the world spawn data we need:

- `TheWorld.PersistentLevel.WorldInfo_2` (`WorldInfo` class) at export
  **#24588** — the level's WorldInfo instance.
- `TheWorld.PersistentLevel` (`Level` class) at export #4060.
- `TheWorld` (`World` class) at export #24587.
- 6,700+ `TheWorld.PersistentLevel.*` placed actors (AA_BreakableWindow,
  StaticMeshActor, PointLight, Brush, …).
- 9 `LevelStreamingKismet` instances for the streaming sub-levels.

So Ch=2 BDB=2367 (the first ChType=2 channel the legit server opens)
almost certainly replicates `WorldInfo_2` from this package. **The class
data is in the right place — our cumulative-index math is what's wrong**.

### 5.7 The 29,417-object cumulative offset

Our predicted wire index for `WorldInfo_2` is:

```
Bridge_night_clear.object_base (= 177398)  +  export #24588  +  19  =  202005
```

The legit pcap has **172588** — we're 29,417 too high. The +19 offset
that nails the six AA3Baseline classes (§5.5) does NOT extend cleanly
into Bridge_night_clear. Trying:

- `min(GenerationNetObjectCount, export_table_count)` as ObjectCount
  drops the discrepancy from 29,417 to ~25,200 — not the root cause.
- All packages have `Generations.Count == 1`, so no generation-history
  trick.
- Manifest order matches the legit pcap's USES order exactly
  (verified by decoding the legit storm).

Working hypotheses for the remaining 25k–29k offset:

1. **Lazy package map**: the client may not enumerate every USES-acked
   package into its `UPackageMap` immediately — possibly only those
   referenced by replicated actors so far. Earlier packages might not
   "count" for global index purposes until something in them gets
   replicated. This would mean the index space changes over time, and
   the legit pcap's Ch=2 fires before some packages contribute.
2. **AA3 has a custom PackageMap implementation** that doesn't follow
   `UPackageMapLevel::Compute` linearly. The deobfuscation pass (and the
   PKG_NoExportAllowed flag clearing in `aa3_deobfuscate.py`) hints at
   AA3-specific tooling that may have its own index conventions.
3. **`Object->NetIndex` is genuinely stored per-object in cooked data**
   and is NOT identical to export-table position + 19. The +19 we saw
   for six AA3Baseline classes may have been coincidence — they happen
   to be in a sequential block where that offset holds, but other
   classes diverge. To prove or refute this, we need to reliably read
   the on-disk NetIndex per object.

### 5.8 Pragmatic paths forward

**Path A — Byte-replay sidesteps the math.** Since both client and
server resolve `ObjectIndex` the same way against the same package set,
copying the legit pcap's Ch=2 bunch bytes verbatim and shipping them
from our server (with our own PacketSeq + ack list wrapping) should
work — the client doesn't care HOW the server computed 172588, only
that the index resolves to the same `WorldInfo_2` on its side. This is
fragile across map changes but works today for Bridge_night_clear.

**Path B — Solve the indexing properly.** Recommended for a multi-map
robust server. The cleanest options:
1. **Attach gdb to a live AA3 server** and dump `UPackageMap::List` at
   the moment Ch=2 opens. That gives us the authoritative `ObjectBase`
   per package as the AA3 build computes it.
2. **Read per-object NetIndex from disk correctly.** The cooked-package
   serialization places `NetIndex` as the first INT32 of the object body
   (per `UnObj.cpp:1565`), but our reads come back with too many zeros
   and duplicates. Possible causes: AA3's deobfuscation pass may have
   shifted body offsets in a way we haven't fully tracked; or the
   serialized data starts with `RF_DebugSerialize` prefix bytes before
   the NetIndex. A short investigation against a known class (e.g.,
   `WorldInfo_2`'s actual on-disk body) will tell us which.
3. **Run a tiny port of `UPackage::ComputeNetObjectList`** offline
   against the cleaned packages. UE3 source for this is in
   `UnObjBas.cpp` (search for `SetNetIndex` callers during package
   load).

### 5.9 Six manifest packages confirmed optional

Confirmed by user 2026-05-15 — safe to ignore for gameplay server:

- `SoundNodeVOIP_Test` — TeamSpeak3 VOIP engine stub
- `Editor_UI`, `Editor_AISupportIcons`, `MCEditorResources`,
  `TestSAI`, `Island_Assets` — editor-only / test packages

### 5.10 Decisive net-index diagnosis (2026-06-14)

Direct measurement against the seven legit ObjectIndexes, the PackageMapDumper
output (`out_full_v2.json`), and the cleaned packages settles the open questions
from §5.7/§5.8. The "25k–29k cumulative mystery" is resolved — it was two
separate, smaller bugs plus a wrong assumption.

**(a) Script-package net indices are correct: `net_index` IS the first INT32 of
the object body.** Reading `clean_packages/AA3Baseline.u` at each export's
`serial_offset`:

| Object | export array pos | body[0] | dumper net_index |
|---|---|---|---|
| `AA_GameReplicationInfo` | 10697 | **10928** | 10928 |
| `AA_UnlockableAbility` | — | 3 | 3 |
| `actInventoryItem` | — | 1 | 1 |
| `AA_Ability3XMagnifierOptic` | — | 5 | 5 |

So `net_index ≠ export-table position` (GRI: pos 10697 vs net 10928); the body
INT32 is authoritative, and the dumper reads it correctly. All six legit
AA3Baseline classes resolve as `legit = object_base[AA3Baseline] + net_index + 19`
— a **constant +19** preamble. The cumulative base is therefore correct through
AA3Baseline (38343), and the +19 preamble is real and stable.

**(b) The map package's net indices are entirely unread.** All 24,589
`Bridge_night_clear` exports have `net_index = -1`. Reading `WorldInfo_2` at its
recorded `serial_offset` (134026746 — the last 724 bytes of the 134 MB file)
returns `(-244, -244, -1, -1, …)`, not a net index. The body[0] method that
works for script packages does **not** apply to the cooked map package.

**(c) The "29,417 offset" was an artifact** of assuming `net_index == export
position (#24588)`. WorldInfo is the level's first net actor, so its real
net_index is small; the actual base inflation is only **~4,830**:
`object_base[Bridge]` is computed as 177398 but must be `≈ 172569 −
net_index[WorldInfo_2]` (from `172588 = base + net_index + 19`).

**Root cause:** cooked **level/map** packages do not serialize per-actor NetIndex
into the object body. Level-placed actors (`WorldInfo_2`, StaticMeshActors,
lights, brushes…) are net-indexed by their **order in the persistent level's
actor list at load time**, not by a stored INT32. The dumper's body[0] read
therefore fails wholesale (→ −1) on the map.

**Next concrete steps (in order):**

1. **Assign map-package net indices by level actor order.** Walk the cleaned
   map's `TheWorld.PersistentLevel` and number only the net-relevant
   (`Actor`-subclass) exports, starting at `object_base[Bridge]`. Verify
   `WorldInfo_2` lands at `172588 − 19 = 172569` (i.e., net_index ≈
   `172569 − object_base[Bridge]`).
2. **Reconcile the ~4,830 base inflation.** Audit `object_count`
   (GenerationNetObjectCount) vs the true net-object count for the packages
   between AA3Baseline and the map — the cumulative over-counts by ~4.8k before
   the map even starts. Likely candidates: the packages where `object_count >
   export_table_count` (Sold_Anims +1373, fX +1284, NMEWeap_obran +217, …).
3. Once (1)+(2) land WorldInfo at 172588, re-validate the other map actors
   (Brushes/StaticMeshActors the legit server opens on Ch=2..N) the same way.

This is why **Path A (byte-replay) works today** (it never computes an index —
it copies the legit bytes, and the client resolves them against the same package
set) and why **Path B (synthesis) is blocked**: synthesis needs the map-actor
net-index numbering, which the dumper doesn't yet produce.

### 5.11 Why the dumper misses 154 packages — and the body[0] fix (2026-06-15)

Root cause of the −1 net indices, pinned exactly:

**Only 8 of 162 packages get net indices — exactly the script (`.u`) packages**
(Core, Engine, GameFramework, UnrealScriptTest, GFxUI, OnlineSubsystemAA3,
AA3Baseline, AA3Game). The dumper takes `net_index` from UELib's `obj.NetIndex`
(`Program.cs:98`), and **UELib only populates `NetIndex` for object types it
fully deserializes** (UClass/UStruct/UFunction/UState). Textures, meshes, sounds,
and level actors keep the default −1.

**The NetIndex is still on disk for those objects — it's the first INT32 of the
object body** (`SerialOffset`), exactly as for script objects. Verified by direct
read of the cleaned packages:

| Package | body[0] of first exports |
|---|---|
| EngineFonts | 0, 1, 2, 3, 4, 5, 6, 7 (dense) |
| EngineMaterials | 115, 0, 1, 2, 3, 4, 5, 6 |
| Sold_Tech | 595, 570, 0, 1, 2, 3, 4, 5 |
| RadialMenu | 20, 14, 0, 1, 2, 3, 4, 5 |

So the **dumper fix** is: when `obj.NetIndex < 0`, seek to
`obj.ExportTable.SerialOffset` and read the first INT32. (`Program.cs`, in the
per-object loop around line 98/113.)

**Caveat — body[0] is correct per-object but a poor *counter*.** Some exports
have non-standard body starts and yield garbage body[0]:
- `class == "Package"` forward-reference exports (the level's sub-package refs).
- Certain special level actors — e.g. **`WorldInfo_2` reads −244**, sits in the
  last 724 bytes of the file, and does not lead with its net index.

Taking `max(body[0])+1` as the per-package count therefore over-counts badly
(AA3Game: true 2039 vs garbage 20075; full-recompute pushed `object_base[Bridge]`
to 195316 — worse). **Per-package counts must stay sourced from
`GenerationNetObjectCount`** (UE3-authoritative; matches the dense count on clean
packages: EngineFonts 9=9, EngineMaterials 139 with 6 non-net exports).

### 5.12 The ~4,830 base inflation is a *count*, not a *read*, problem

Coordinate-independent constraint from the six AA3Baseline ground-truths + the
WorldInfo ground-truth (dense indexing + the stable +19/+20 preamble):

```
sum( net_count[AA3Baseline .. package-before-Bridge] ) + net_index[WorldInfo] = 134226
```

- `object_base[AA3Baseline] = 38343` is **validated** (the +19 holds for all six
  classes), so `GenerationNetObjectCount` is correct for Core..OnlineSubsystemAA3.
- Summing `GenerationNetObjectCount` across AA3Baseline..pre-Bridge gives 139055,
  which needs `net_index[WorldInfo] = −4829` — impossible. So a **small set of
  packages between AA3Baseline and Bridge are over-counted (or not in the
  client's package map at all) by ~4,830**.
- **Prime suspect: `Sold_Anims` (GenerationNetObjectCount = 4970 ≈ the gap).** If
  the client's net package map omits it or counts it differently, removing it
  yields `net_index[WorldInfo] ≈ 141` (small, sane). Needs checking against the
  legit storm's USES order (do client and server agree Sold_Anims is in the net
  map, and where?).

### Next concrete steps (updated)

1. **Add the body[0] fallback to the dumper** (verified correct for assets) so
   asset/object references resolve — keep `object_count` from
   `GenerationNetObjectCount`.
2. **Localize the ~4,830.** Re-decode the legit storm's USES list and diff its
   net-map package set/order against our 162-package manifest; check whether
   `Sold_Anims` (or a small set) is excluded or reordered. That single fact likely
   lands WorldInfo at 172588.
3. **WorldInfo_2 special read.** It doesn't lead with NetIndex (body0 = −244,
   near-EOF). Either read it serialization-aware (StateFrame/flags) or derive
   the persistent-level actors' net indices by load order off `TheWorld.PersistentLevel`.

### 5.13 USES diff + dumper fix results (2026-06-15)

Two concrete outcomes:

**Dumper fix shipped + verified.** `PackageMapDumper/Program.cs` now reads NetIndex
from body[0] when UELib leaves it −1 (guarded: skips `class=="Package"`, rejects
out-of-range). Rebuilt with `dotnet build` (clean) and confirmed on assets:
EngineFonts → net indices 0–8, EngineMaterials → 0–138 (139 valid = its
`GenerationNetObjectCount`). Previously both were all −1.

**USES diff refutes the Sold_Anims hypothesis (§5.12).** The decoded legit USES
list (`legit_manifest.txt`, 168 packages) and our dumper manifest contain the
**same package set**, and `Sold_Anims` is present in **both**. So the ~4,830 is
**not** a missing/extra package — the client and server net-map package sets agree.

**body[0] is not a universal counter — negative result.** Computing each
package's contiguous dense net count (`{0..N−1}` present) across AA3Baseline→Bridge
gives 27454 vs `GenerationNetObjectCount`'s 139055 — wildly low. body[0] is the
NetIndex only for objects whose body *leads* with it (EngineFonts, EngineMaterials,
Sold_Tech, RadialMenu, the six AA3Baseline classes — all confirmed); for fX,
Sold_Anims, and most `S_*` sound packages, body[0] is unrelated data. So body[0]
fixes per-object references where it's valid, but **cannot** be used to recount
packages.

**Where that leaves the 4,830.** `GenerationNetObjectCount` is ~96% correct
(139055 vs the required ≤134226); the overage is small and concentrated in a few
packages whose cooked net-object count differs from what the loaded client
computes. File-only methods can't isolate it further without a reliable per-object
NetIndex for the unreadable packages. **Decisive next step is Path B.1** (§5.8):
attach to a live AA3 server and dump `UPackageMap::List[*].ObjectBase` at the
moment Ch=2 opens — that yields the authoritative per-package base directly and
ends the guessing.

### 5.7 What this unlocks for the server

When `aa3-linux-server` needs to OPEN an actor channel with a specific
class (say `AA_PlayerController`), the encoder writes:

```
b_dynamic   : 1 bit  = 0       (static object)
ObjectIndex : RIM(2^31)        = 19 + ObjectBase[AA3Baseline] +
                                 export_table_index(AA_PlayerController)
```

…and then the property-replication payload defined in
`OnlineSubsystemAA3/uscript_decompiled/AA3Baseline/AA_PlayerController.uc`
line 463+ (the `replication{}` block).

`out_full.json` produced by `aa3-tools/PackageMapDumper` already contains
the `object_base` per package and the export-table-indexed list of every
class, so the server can do this lookup in O(1) at startup.

## 6. Sentinel-tail pattern in tiny bunches

The 50-bit bunches on Ch=3, 5, 11, 13-16 all end with the same 18-bit
tail `40 15 00` (LSB-first per byte):

```
bits 32..49:  0 0 0 0 0 0 1 0 1 0 1 0 1 0 0 0 0 0
```

Hypothesis: this is `RepIndex == MaxIndex` (end-of-bunch sentinel) plus
a few padding bits before the bunch's BDB boundary. The value of
`MaxIndex` depends on the class — that's why different classes will
emit different bit patterns here, but all classes with `MaxIndex` in
the same RIM-bit zone (e.g., 6 or 7 bits) and no replicated state will
look very similar.

To verify: build `FClassNetCache` for the suspected class (e.g.,
`GameReplicationInfo`), count its `RepProperties.Num() + NetFields.Num()`,
and confirm the RIM-encoded value matches.

## 7. What we have right now and what's next

**Have:**

- `decode_actor_channel.py` decodes the UObject reference (first 32 bits)
  cleanly and prints the rest as hex for inspection. Works on legit
  frames 72-74; ObjectIndices look sane (32-bit, monotonically related,
  in the same order of magnitude across small bunches).
- A roadmap (this doc) that maps every bit on the wire to a concrete UE3
  code path.

**Next milestones, in order:**

1. **Reproduce the ObjectIndex table.** Without this we can't name the
   first three classes. The fastest way is UE-Explorer + a script that
   simulates `UPackageMap::Build()` against our manifest.
2. **Decode `SerializeCompressedInitial`** in the WorldInfo-sized bunch
   (Ch=2, 2367 bits) so we can pull Location/Rotation out and confirm
   the "transient actor" path.
3. **Build `FClassNetCache` for at least WorldInfo + GameReplicationInfo
   + AA_PlayerController** so we can decode `RepIndex` and identify
   which property updates the legit server is shipping in the small
   tail bunches.
4. **Byte-replay frame 72.** Once we know the class is just "the WorldInfo
   for Bridge_night_clear", the bunch bytes for our server are identical
   to the legit server's — same package manifest, same level, same
   class. Send frames 72-80 verbatim (only adjusting our PacketSeq) and
   observe what the client does next.
5. **Hand-emit the PlayerController spawn.** Once we know the PC class
   and its replicated property list, we can synthesize this bunch from
   scratch with sensible defaults. After this fires the client should
   transition out of "loading screen" and start sending movement input
   over the channel back to us.

## 8. Cross-reference quick links

| Topic | UE3 ref | AA3 decomp | This repo |
|-------|---------|-----------|-----------|
| Actor channel receive | `UnChan.cpp:1359` `UActorChannel::ReceivedBunch` | — | `aa3-linux-server/src/network/protocol_handler.cpp` (TODO) |
| Actor channel send | `UnChan.cpp:1695` `UActorChannel::ReplicateActor` | — | — |
| PackageMap (PC build) | `UnNetDrv.cpp:97` `UPackageMapLevel::SerializeObject` | — | — |
| PackageMap (console) | `UnNetDrv.cpp:238` `UPackageMapSeekFree::SerializeObject` | — | (not needed for PC server) |
| WorldInfo replication | `UnrealEngine3/Development/Src/Engine/Classes/WorldInfo.uc` | `OnlineSubsystemAA3/uscript_decompiled/Engine/WorldInfo.uc:300-368` | — |
| GameReplicationInfo  | `UnrealEngine3/.../Classes/GameReplicationInfo.uc` | `OnlineSubsystemAA3/uscript_decompiled/Engine/GameReplicationInfo.uc` | — |
| AA_PlayerController  | (subclass; no UE3 ref) | `OnlineSubsystemAA3/uscript_decompiled/AA3Baseline/AA_PlayerController.uc` | — |
| Channel constants    | `UnConn.h:140` `MAX_NET_CHANNELS=2048` | — | — |
| MaxPacket=512        | `UnConn.cpp:305` | — | implicitly via `kMaxPayloadBits=4096` |
| Object index max     | `UnCoreNet.h:100` `MAX_OBJECT_INDEX=1<<31` | — | — |

## 9. RepIndex reconstruction from cooked metadata (2026-07-11)

§2.3 left the RepIndex encoding as "both sides must agree; MaxIndex isn't on
the wire." This section closes that: we can now compute the per-class replicated
property ordering directly from the shipped `.u` packages, natively in
`aa3_build_manifests`, with no .NET/UELib dependency. This is the property-side
counterpart to §5's ObjectIndex work.

### 9.1 The authoritative algorithm (`UnClass.cpp:2121-2153`)

`UClass::Link` assigns `RepIndex` at load time (both client and server run it,
so both agree):

```cpp
ClassReps = (SuperStruct != NULL) ? GetSuperClass()->ClassReps : TArray<FRepRecord>();
for( TFieldIterator<UField> It(this,FALSE); It; ++It ) {   // FALSE = this class only
    if( (P=Cast<UProperty>(*It)) && (P->PropertyFlags & CPF_Net) ) {
        NetFields.AddItem(*It);
        if( P->GetOuter()==this ) {
            P->RepIndex = ClassReps.Num();
            for(INT i=0;i<P->ArrayDim;i++) new(ClassReps)FRepRecord(P,i);
        }
    } else if( (F=Cast<UFunction>(*It)) && (F->FunctionFlags&FUNC_Net) && !F->GetSuperFunction() )
        NetFields.AddItem(*It);
}
Sort<...>( NetFields );   // net functions get indices after all properties
```

So absolute RepIndex order is: **inherited CPF_Net properties first** (seeded
from the super class, recursively), **then this class's own CPF_Net properties
in `TFieldIterator` order**, i.e. `Children → Next → Next …`. `MaxIndex` =
`ClassReps.Num()` + net-function count.

### 9.2 The on-disk UField layout for AA3 v568 (byte-verified)

Verified against `Engine.u` GameReplicationInfo (export #13417, 460 B) and its
child properties. v568 < `VER_MOVED_SUPERFIELD_TO_USTRUCT` (756), so `SuperField`
sits in the legacy UField slot.

The trap that cost the first parse pass: `UObject::Serialize` (UnObj.cpp:1756-1762)
does `SerializeNetIndex` then, **iff `Class != UClass::StaticClass()`**,
`SerializeScriptProperties`. For native meta-objects that emits just an
`FName("None")` terminator = **8 bytes**. So a *UClass* body has NO None tag,
but every *property / function / struct* body does:

```
UObject:   INT   NetIndex
           FName "None"          // 8 B — present for everything EXCEPT a UClass
UField:    INT   SuperField      // FPackageIndex → SuperStruct
           INT   Next            // FPackageIndex → next sibling  (Children→Next chain)
UStruct+:  INT   ScriptText, Children, CppText, Line, TextPos, ScriptBytecodeSize
           BYTE  Script[ScriptBytecodeSize]     // no separate ScriptStorageSize field
UProperty: INT   ArrayDim
           QWORD PropertyFlags   // CPF_Net = 0x20
           FName Category (8 B), FPackageIndex ArraySizeEnum (4 B)
           WORD  RepOffset       // present iff CPF_Net
```

MatchID (IntProperty, 46 B) decodes exactly with this layout; the missing 8-byte
None tag was why an earlier read had `Next=0` on every field.

### 9.3 Cooked packages DO keep the field chain — but strip nothing needed

Earlier note claimed the `Children→Next` list was "stripped" (only one function
survived the walk). That was the None-tag off-by-8 bug, not reality. With the tag
consumed, `GameReplicationInfo.Children → … → MatchID` walks all 47 fields
(21 functions, 26 properties) in reverse-declaration order — exactly
`TFieldIterator` order. Fields also remain reachable independently via
`export.outer_index == class`, in forward-declaration (export) order.

### 9.4 Tool: `aa3_build_manifests --dump-class <pkg.u> <ClassName>`

Prints the class header (NetIndex, SuperStruct, Children), walks `Children→Next`,
flags each CPF_Net property, and lists this class's replicated properties in
RepIndex order. Validated:

- **WorldInfo** → 5 CPF_Net props `{bHighPriorityLoading, TimeDilation, Pauser,
  WorldGravityZ, ReplicatedMusicTrack}` — exact match to §4.1's `replication{}`
  block. `SuperStruct=ZoneInfo` (correct).
- **GameReplicationInfo** → 20 own CPF_Net props; `SuperStruct=ReplicationInfo`.
  `RepOffset` clusters (many = 61) mark properties sharing one replication
  condition — a free signal for which props flush together.

### 9.5 Also fixed: reading already-decompressed "clean" packages

`clean_packages/*.u` (LZO+XOR stripped by `aa3_decompress.py`/`aa3_deobfuscate.py`)
keep the `PKG_StoreFullyCompressed` flag but have zero compressed chunks.
`PackageReader::ReadBuffer` used to force the decompress path on the flag alone
and fail. It now decompresses only when a chunk table is present and otherwise
treats the on-disk body as the (already-decompressed) body. `--dump-body-hex`
routes through `ReadFile` for the same reason.

### 9.6 Next step — superseded, see §10

Chain 9.1 across the inheritance line (WorldInfo ← ZoneInfo ← Info ← Actor …) to
get absolute RepIndex + MaxIndex per class, then emit `NetSerializeItem` for each
property to build a real Ch=2 open bunch — replacing the frame-72 byte replay the
server currently ships. Per-type `NetSerializeItem` bit layouts (Int/Bool/Float/
Str/Object) are the remaining unknowns; decode them from the legit pcap payloads.

**Done (2026-07-22):** §10 chains the inheritance line via a new tool
(`--rep-layout`), validated on WorldInfo/GameReplicationInfo/AA_PlayerController/
AA_GameReplicationInfo. §10 also pins the real `NetSerializeItem` bit layouts by
reading `UnrealEngine3/` source directly (ground truth, not guesswork) and
**refutes** §6's sentinel hypothesis. What's still open: reconciling those formats
against captured bytes — see §10.5.

## 10. RepIndex chaining tool + NetSerializeItem ground truth (2026-07-22)

### 10.1 New tool: `aa3_build_manifests --rep-layout <pkg.u> <ClassName> [--pkg-dir <dir>]`

Implements exactly the algorithm in §9.1/9.4: walks the class's inheritance chain
(following `SuperStruct`, crossing package boundaries via the import table and a
`PkgCache` that loads sibling packages from `--pkg-dir` on demand), collects each
class's own `CPF_Net` properties + `FUNC_Net` functions via `Children→Next`, sorts
each class's own fields by on-disk `NetIndex` ascending (mirrors `UClass::Link`'s
`Sort<>(NetFields)`), then assigns `FieldNetIndex = FieldsBase + position`, where
`FieldsBase` for each class is the running total from its super — an exact port of
`UPackageMap::GetClassNetCache` (`UnCoreNet.cpp:136-189`, ground-truth source now
available under `UnrealEngine3/Development/Src/Core/Src/`).

Validated on four classes (chain base→derived shown, MaxIndex = end-of-bunch bound):

| Class | Chain | MaxIndex | Notes |
|---|---|---|---|
| `Engine.WorldInfo` | Object→Actor→Info→ZoneInfo→WorldInfo | 28 | 23 inherited from Actor + exactly the 5 props from §4.1's `replication{}` block |
| `Engine.GameReplicationInfo` | …→ReplicationInfo→GameReplicationInfo | 43 | 20 own props incl. `MatchID`, `Winner`, `bMatchHasBegun` — matches §4.2 |
| `AA3Baseline.AA_PlayerController` | …→Controller→PlayerController→AA_PlayerController | 91 | 48 own fields (11 props, 37 RPCs) — `SpectatorMode`, `bVOIPEnabled` etc. match the `repnotify` list in §4.3 |
| `AA3Baseline.AA_GameReplicationInfo` | …→GameReplicationInfo→AA_GameReplicationInfo | 133 | 90 own props — `sBaseMapName`, `ServerName`, `sMissionType`, `sTOD` etc. map directly onto `ServerGameReplicationInfo` fields already in `world.cpp` |

`Actor`'s own 23 base props (RepIndex 0-22, shared by every class) are now known
precisely: `iOriginalOwningTeamIndex, RelativeRotation, RelativeLocation,
namePawnInventoryAssociation, Velocity, Instigator, bNetOwner, bProjTarget,
bBlockActors, bCollideWorld, bCollideActors, bHardAttach, bTearOff, bHidden, Base,
Owner, ReplicatedCollisionType, Role, RemoteRole, Physics, DrawScale, Rotation,
Location`. Note `iOriginalOwningTeamIndex` at index 0 — this is an AA3-specific
field bolted onto the licensed `Engine.Actor` base class (not stock UE3), confirming
AA3 ships a patched `Engine.u`, not the vanilla one.

### 10.2 §6 sentinel hypothesis REFUTED

§6 hypothesized the tail pattern on tiny bunches (`Ch=3,5,11,13-16`, all ending in
the identical 18-bit `40 15 00`) was `RepIndex == MaxIndex`, varying per class. Two
findings kill this:

1. **The tail bits are byte-for-byte identical across bunches whose resolved
   classes have different `MaxIndex`** (e.g. `AA_SteamStatManager` MaxIndex=25 vs
   whatever `AA_MOSQualificationBCT`/`AA_PlayerReplicationInfo` resolve to) — a
   per-class sentinel value could not produce identical bits across different
   `MaxIndex` bounds.
2. **The real `SerializeInt`/`ReadInt` algorithm cannot represent `value == Max`
   at all.** Read from actual source
   (`UnrealEngine3/Development/Src/Core/Src/UnBits.cpp:308-323`,
   `FBitReader::SerializeInt`): the loop condition is `Value+Mask < ValueMax`,
   strictly less-than, so the maximum representable value is always `< ValueMax`.
   This matches our own `BitReader::ReadIntMax`/`WriteIntMax` in
   `aa3-linux-server/src/util/bits.cpp` exactly (already correct — the header
   fields it decodes are fine) but rules out an explicit numeric sentinel.

### 10.3 The real end-of-bunch mechanism: BDB exhaustion, not a sentinel value

Read directly from `UnChan.cpp:1476-1578` (`UActorChannel::ReceivedBunch`) and the
matching send side `UnChan.cpp:1926-1985` (`UActorChannel::ReplicateActor`):

- **Send side never writes a terminator.** It iterates dirty properties in
  ascending `RepIndex` order, writes `FieldNetIndex` + value for each, and simply
  stops — `if (Bunch.GetNumBits()) SendBunch(...)`. No explicit "end" marker.
- **Read side loop condition is `while (FieldCache || bJustSpawned)`**, where
  `FieldCache = Bunch.IsError() ? NULL : ClassCache->GetFromIndex(RepIndex)`. Since
  `ReadInt(MaxIndex)` can only return valid in-range values (per §10.2), the ONLY
  way `FieldCache` becomes `NULL` in practice is `Bunch.IsError()` — i.e. **the
  read simply ran off the end of the bunch's declared `BunchDataBits`** (checked
  bit-by-bit inside `FBitReader::SerializeInt`/`SerializeBits`: `if (Pos>=Num)
  ArIsError=1`).
- **Conclusion:** `BunchDataBits` (BDB) is the sole authority on where a bunch
  ends. There is no in-band terminator to decode. A correct decoder must simply
  stop consuming (gracefully, not throw) the instant a read would exceed `BDB`.

### 10.4 NetSerializeItem ground truth per type (source-cited)

All read directly from `UnrealEngine3/Development/Src/Core/Src/UnProp.cpp` and
`UnMath.cpp` — no more guessing:

| Type | Function | Wire format |
|---|---|---|
| `BoolProperty` | `UnProp.cpp:1432` | 1 bit |
| `IntProperty` | `UnProp.cpp:818` | `Ar << *(INT*)Data` → 32 raw bits (byte-serialize, not RIM) |
| `FloatProperty` | `UnProp.cpp:1532` | `Ar << *(FLOAT*)Data` → 32 raw bits |
| `ByteProperty` | `UnProp.cpp:656` | `Ar.SerializeBits(Data, Enum ? CeilLogTwo(NumEnums-1) : 8)` — **8 raw bits only if non-enum**; enum-typed bytes (likely `Role`, `RemoteRole`, `Physics`, `SpectatorMode`, …) use a compact bit-width derived from the enum's cardinality, which the dumper does not yet read (needs the trailing `Enum` `FPackageIndex` that `UByteProperty::Serialize` appends after the base `UProperty` tail) |
| `ObjectProperty`/`ClassProperty` | `UnProp.cpp:1922` | `Map->SerializeObject(...)` — same `bDynamic:1 + RIM` encoding as the channel-open object reference (§2.1) |
| `StrProperty` | `UnMisc.cpp:835` (`operator<<(FArchive&,FString&)`) | `SaveNum:INT32` (raw, sign = ANSI(+)/UNICODE(−), magnitude includes the null terminator), then `\|SaveNum\|` raw chars (8 bits ANSI / 16 bits UNICODE) |
| `NameProperty` | `UnProp.cpp:3041` | `Ar << *(FName*)Value` — **not yet decoded**; likely routes through a net-specific name-table archive, format TBD |
| `StructProperty` (`Vector`) | `UnProp.cpp:4194`, `UnMath.cpp:51` | `FVector::SerializeCompressed`: `Bits = RIM(20)`; `Bias = 1<<(Bits+1)`, `Max = 1<<(Bits+2)`; `DX,DY,DZ = RIM(Max)` each; real coord = `D - Bias` |
| `StructProperty` (`Rotator`) | `UnProp.cpp:4199`, `UnMath.cpp:84` | Per axis (Pitch, Yaw, Roll independently): 1 bit "nonzero", if set 1 byte (`raw>>8`, i.e. only the high byte of the 16-bit angle is sent) |
| `StructProperty` (other, e.g. `Quat`) | `UnProp.cpp:4204+` | Custom per-struct-type; not yet extracted for AA3-custom structs (`atsTeamScores`, `saiServerAuthorizationStatus`) |
| Fixed C-array element (`ArrayDim>1`) | `UnChan.cpp:1513-1523`, `1953-1957` | 1 raw byte `Element` index precedes the value, both read (`Bunch << Element`) and write (`Bunch << Element`) side |

**`SerializeCompressedInitial`** (the transient-actor spawn transform, §2.2, called
from `UnChan.cpp:15` when the resolved object `HasAnyFlags(RF_ClassDefaultObject \|
RF_ArchetypeObject)` — i.e. it's a class-default-object reference, meaning "spawn a
new instance of this class"): unconditionally `Location.SerializeCompressed(Bunch)`
(same Vector format as above), then **only if `Actor->bNetInitialRotation`**,
`Rotation.SerializeCompressed(Bunch)` (same Rotator format). This replaces §2.2's
"TBD" full-float guess.

### 10.5 Validation attempt against real pcap bytes — inconclusive, open problem

Built a Python decoder (scratch, not yet committed) applying §10.3/§10.4 to
frame 73's `Ch=4` bunch (`AA_GameReplicationInfo`, `ObjectIndex=49290`, BDB=3570,
legit pcap). Findings:

- With the transient-spawn `SerializeCompressedInitial` path applied, `Location`
  decodes to `(0,0,0)` (5+2+2+2=11 bits) — plausible for a non-physical actor like
  GRI — and the very next `RepIndex` read lands on `2` (`Actor.RelativeLocation`),
  i.e. small/near-zero as expected for a fresh `bNetInitial` spawn. But the decode
  desyncs a few properties later (falls into repeated implausible
  `RepIndex=0`/huge-int reads), so this is **not yet a clean end-to-end decode**.
- Cross-checked against the tiny `Ch=3` bunch (`AA_SteamStatManager`, `MaxIndex=25`,
  only 18 bits of payload after the 32-bit object ref — a fully hand-traceable
  case). Manually walked the exact bits
  (`raw: 0a 14 02 00 40 15 00`, bunch bits 2642-2691 of frame 72): the transient
  `Location` decode consumes exactly 11 bits → `(0,0,0)` again (self-consistent
  with the math), leaving only 7 bits. A `RepIndex=RIM(25)` read against those 7
  bits yields `2` (`RelativeLocation` again) using 5 bits, leaving only 2 bits —
  nowhere near enough to encode a `Vector`. This is either (a) a real overflow that
  a correct decoder should treat as graceful termination (matching §10.3 exactly —
  entirely plausible, since UE3's own reader would do the same and just stop), or
  (b) evidence that the transient-spawn assumption itself is wrong for these
  objects (e.g. they resolve to a placed/existing instance rather than a `Default__`
  CDO, so `SerializeCompressedInitial` never fires and the 18 raw bits are pure
  property loop instead — tried this too, also overflows partway through
  `Actor.iOriginalOwningTeamIndex`'s 32-bit int).
- Neither hypothesis (transient vs non-transient) currently produces a bunch that
  exactly exhausts its declared BDB with all-plausible field values — the real
  discriminator per §10.3 is "did we run out of bits at a sane boundary," and
  right now both hypotheses run out mid-property, which is the signature of a
  still-missing wire element (candidates: the enum-compact `ByteProperty` width
  from §10.4 affecting an earlier field than expected in a longer bunch; a
  RepNotify-condition/array-index detail; or `RF_ArchetypeObject` not actually
  applying to these specific objects).

**Next concrete step:** either (a) get a debug/dev AA3 build or gdb-attach to a
live legit server (§5.8 Path B.1, still the decisive option) to directly observe
`ClassCache->GetFromIndex` resolution and `bNetInitialRotation`/CDO-flag state for
one of these actors, or (b) brute-force the small `Ch=3`-style bunches: since they
have only ~18 bits of freedom, exhaustively try both hypotheses (transient/not)
against all plausible small dirty-property combinations and keep whichever
reconstruction exactly consumes 18 bits with no overflow — tractable precisely
because these bunches are so small, unlike the 3570-bit `Ch=4` case.

**Superseded by §11** (2026-07-22): (b) was tried and failed — see §11.4/§11.5.
The remaining blocker is narrower than either hypothesis above suggested.

## 11. Wire-format ground truth completed; the real remaining blocker (2026-07-22)

Follow-up session. Goal: close §10.5's open decode problem. Result: **every
`NetSerializeItem` wire format is now fully resolved with zero guessing**, sourced
directly from `UnrealEngine3/Development/Src/` (this tree is present in the repo
root and is ground truth — stop guessing when it can just be read). The original
decode still doesn't close end-to-end on the one legit pcap we have; that failure
is now isolated to something *outside* wire-format (see §11.5).

### 11.1 New tool: `aa3_build_manifests --struct-layout <pkg.u> <StructName>`

`UStructProperty::NetSerializeItem` (`UnProp.cpp:4192-4283`) hand-codes five
native struct types and falls through to a generic recursive path for everything
else:

```cpp
if      (Struct->GetFName()==NAME_Vector)     V.SerializeCompressed(Ar);
else if (Struct->GetFName()==NAME_Rotator)    R.SerializeCompressed(Ar);
else if (Struct->GetFName()==NAME_Quat)       { /* 3 floats, W derived */ }
else if (Struct->GetFName()==NAME_Plane)      { /* 4 SWORDs, rounded */ }
else if (Struct->GetFName()==NAME_UniqueNetId){ Ar << *(QWORD*)Data; }
else {
    for (TFieldIterator<UProperty> It(Struct); It; ++It)
        if (Map->SupportsObject(*It))
            for (INT i=0; i<It->ArrayDim; i++)
                It->NetSerializeItem(Ar, Map, Data+It->Offset+i*It->ElementSize);
}
```

Two things this settles that earlier sections only guessed at:

- **The "else" branch has no CPF_Net filter and no per-field RepIndex header** —
  unlike class-level replication, EVERY `UProperty` member of a custom struct gets
  serialized, unconditionally, in `TFieldIterator` (`Children→Next`) order. Both
  ends already agree on the struct's exact layout, so there's nothing to index.
- **`UArrayProperty::NetSerializeItem` is a hard no-op** — `return 1;`, no bits
  read or written (`UnProp.cpp:3365-3368`). A dynamic `TArray` member (as opposed
  to a fixed `ArrayDim>1` C-array, which *does* replicate via the existing
  1-byte-Element-index mechanism) contributes zero bits.

`--struct-layout` walks a `ScriptStruct` export the same way `--rep-layout` walks
a `UClass` (reusing `UStructReader`), recursing into nested non-native
`StructProperty` members, resolving each field's `ByteProperty`'s `Enum` /
`StructProperty`'s `Struct` trailing `FPackageIndex` (both append one 4-byte
index right after the base `UProperty` tail — `UByteProperty::Serialize`
`UnProp.cpp:665`, `UStructProperty::Serialize` `UnProp.cpp:4289` — at offset 44 or
46 depending on whether `CPF_Net` is set, since `RepOffset` is only present when
it is). Validated end-to-end on `AA_SteamStatManager`'s two custom stat structs:
`AA_MP_PlayerMatchStatistics` (12 fields: 2 bool, 1 float, 9 int) and
`AA_MP_PlayerStatistics` (116 fields, almost entirely raw `Int`/`Byte` counters) —
both fully flat, no further unknowns, no enums, no nesting.

### 11.2 Every ByteProperty enum width across all 6 known classes, resolved exactly

`UByteProperty::NetSerializeItem` (`UnProp.cpp:656-660`):
```cpp
Ar.SerializeBits(Data, Enum ? appCeilLogTwo(Enum->NumEnums()-1) : 8);
```
`NumEnums()` is `UEnum::Names.Num()`, and `UEnum::Serialize` (`UnCoreNative.cpp:438`)
writes it as a plain `TArray` count — a raw INT32 sitting right after the `UField`
header (offset 20: `NetIndex(4)+None(8)+SuperField(4)+Next(4)`). Read directly
from the cleaned `.u` files (no UELib, no guessing):

| Property (shared or class) | Enum | Names.Count | Bits (`CeilLog2(N-1)`) |
|---|---|---|---|
| `Actor.ReplicatedCollisionType` | `ECollisionType` | 10 | 4 |
| `Actor.Role`, `Actor.RemoteRole` | `ENetRole` | 5 | 2 |
| `Actor.Physics` | `EPhysics` | 14 | 4 |
| `AA_PlayerReplicationInfo.SelectedLoadout` | `eDefaultLoadoutType` | 5 | 2 |
| `AA_PlayerReplicationInfo.MedicQualification`, `AA_PlayerStorageProxy.RequiredMedicTreatmentLevel` | `eTreatmentLevel` | 4 | 2 |
| `AA_GameReplicationInfo.TS3ServerMode` | `eTS3ServerMode` | 4 | 2 |
| `AA_GameReplicationInfo.SpectatorMode` | `eSpectatorMode` | 5 | 2 |
| `PlayerReplicationInfo.PacketLoss`, `.Ping`, `AA_GameReplicationInfo.SpectatorTeamIndex` | *(none — plain byte)* | — | 8 |

Every `ByteProperty` field across all 6 previously-resolved classes
(`AA_SteamStatManager`, `AA_GameReplicationInfo`, `AA_MOSQualificationBCT`,
`AA_TeamInfo`, `AA_PlayerReplicationInfo`, `AA_PlayerStorageProxy`) is now on this
list. §6/§10.4's "enum widths unknown" caveat is closed.

### 11.3 `NameProperty` wire format resolved

`UNameProperty::NetSerializeItem` doesn't override `NetSerializeItem` — it falls
to `UProperty::NetSerializeItem` → `SerializeItem` → `Ar << *(FName*)Value`. For
`FInBunch`/`FOutBunch` this `operator<<(FName&)` override
(`UnBunch.cpp:41`) forwards to `Connection->PackageMap->SerializeName(...)`, base
implementation at `UnCoreNet.cpp:79` (AA3's PC client uses `UPackageMapLevel`,
which doesn't override `SerializeName`, so the base applies):

```
bHardcoded : 1 bit
if bHardcoded:
    NameIndex : RIM(MAX_NETWORKED_HARDCODED_NAME + 1)   // = RIM(1251), UnNames.h:16
else:
    FString (as §10.4)  +  Number : INT32 raw
```

> **CORRECTED by sec 22 (2026-10-01):** the above is stock UE3 source and is
> **wrong for AA3**. On the wire AA3 sends an FName as a plain FString +
> INT32 Number with **no `bHardcoded` bit** — verified both directions on
> retail traffic (server `NotifyPostureChange("EndWalk")`, client
> `ServerUpdateLevelVisibility("Bridge_pathing")`).

### 11.4 Cross-validated the packet/bunch header against source — confirmed correct

Read `UNetConnection::ReceivedPacket` (`UnConn.cpp:907-1065`) directly. Every
constant our existing decoders use (`bits.cpp`, `decode_pcap4.py`,
`protocol_handler.cpp`) matches exactly:

| Field | Bound used | Source constant |
|---|---|---|
| PacketSeq | `RIM(0x4000)` | `MAX_PACKETID=16384` (`UnNet.h:33`) |
| ChIndex | `RIM(0x800)` | `MAX_CHANNELS=MAX_NET_CHANNELS=2048` (`UnConn.h:143`) |
| ChSeq | `RIM(0x400)` | `MAX_CHSEQUENCE=1024` (`UnNet.h:34`) |
| ChType | `RIM(8)` | `CHTYPE_MAX=8` (`UnNet.h:63`) |
| BunchDataBits | `RIM(4096)` | `MaxPacket*8`, `MaxPacket=512` (`UnConn.cpp:305`) |
| Static ObjectIndex | `RIM(2^31)` | `MAX_OBJECT_INDEX=(DWORD(1)<<31)` (`UnCoreNet.h:100`) |

This rules out header misparsing as the cause of the §10.5 decode failure — the
bunch boundaries our tools compute are provably correct.

### 11.5 The actual remaining blocker: NOT wire format, isolated but unsolved

With every format above locked in, re-ran the full decoder
(`decode_rep_layout2.py`) against the same `Ch=3` bunch from §10.5
(`AA_SteamStatManager`, frame 72, legit pcap, 50-bit bunch). **Identical failure
point** — proves the earlier overrun was never a format problem:

- Transient hypothesis: overruns 2 bits short attempting `RelativeLocation`'s
  Vector `Bits` field (RepIndex=2, the very next field after `Location=(0,0,0)`).
- Non-transient hypothesis: overruns immediately on `iOriginalOwningTeamIndex`'s
  32-bit int (RepIndex=0), 19 bits short.

Also ran a full small combined search — 3 structural modes (persistent /
transient-no-rotation / transient-with-rotation) × `MaxIndex` perturbed ±8 (to
account for possible cooked-package/pcap-capture version skew) = 51 combinations
— against the same bunch. **Zero clean matches** (none consume exactly to
`bunch_end` with no overflow). This is a genuinely useful negative result: it
rules out both "wrong prefix mode" and "slightly wrong property count" as
standalone explanations.

**A real anomaly surfaced along the way, possibly related:** `ObjectToIndex`
(`UnCoreNet.cpp:445-460`) uses the specific object's OWN `NetIndex`
(`Info.ObjectBase + Object->NetIndex`), and the send side (`UnChan.cpp:1788`,
confirmed by reading source directly) writes `Actor->GetArchetype()` — normally
the class's CDO, NOT the class object itself. But reading
`AA3Baseline.u`'s `Default__AA_SteamStatManager` export directly (body[0], same
convention validated in §5.11) gives `NetIndex=29740`, which computes to wire
value **68102** — one more than the captured **68101**, which instead matches the
*class's own* `NetIndex=29739` exactly. This ±1 offset between "class own index"
and "its CDO's index" is suspiciously clean (not a wild divergence) and recurred
in spirit across all 6 classes in §10 (the reason the original `+19` preamble
constant was ever found to work at all was because it was calibrated against
class-export entries, since CDU entries showed `NetIndex=-1` under plain UELib
parsing at the time §5.5/§5.10 were written). **This wasn't chased further this
session** — it doesn't explain the bit-level overrun directly (RIM(2^31) consumes
~31 bits regardless of the encoded value, so a ±1 value error doesn't shift
anything downstream), but it's a loose thread: if the true reference convention
differs from what §5's "+19 preamble" methodology assumed, other things
downstream of package/object indexing may be subtly miscalibrated too.

**Where this leaves things:** every `NetSerializeItem` format, the packet/bunch
header, and the RepIndex/`FClassNetCache` reconstruction algorithm are now
individually verified against source line-by-line. The single legit pcap capture
we have still doesn't decode cleanly under any tried hypothesis. Combinatorial
brute-forcing of small nearby hypotheses (§ above) came up empty, which argues
against "one more missing flag" and for either (a) a genuine live-server
ground-truth capture (§5.8 Path B.1 — gdb-attach or a debug build, still
unclaimed after multiple sessions flagging it as decisive), or (b) a second,
independent legit gameplay capture to cross-validate against (rules out
capture-specific artifacts / version skew in the one pcap we've been using).
`GamePlayPCAP_Legit_Server2.pcapng` exists in the repo and has 110 `ChType=2`
bunches but hasn't been used for this cross-check yet — worth trying before
investing in (a).

### 11.6 §11.5 followed through to its conclusion: object-index resolution is the blocker, not format

Continuation, same session. Three more decisive checks, all confirming the same
diagnosis:

1. **Cross-capture determinism.** Pulled the equivalent `AA_SteamStatManager`
   open bunch from `GamePlayPCAP_Legit_Server2.pcapng` (frame 73, Ch=7) — an
   independent capture session. Raw bytes: `0a140200401500`, **byte-for-byte
   identical** to `GamePlayPCAP_Legit_Server.pcapng` frame 72 Ch=3. This rules
   out capture noise/corruption/version-skew-between-sessions: whatever's
   encoded here is fully deterministic, and our decode fails identically on
   both, so the bug is in our model, not the data.

2. **On-disk `RF_ClassDefaultObject` flag check** (`aa3_build_manifests
   --dump-exports`, flags field = `FObjectExport::object_flags`,
   `RF_ClassDefaultObject=0x200` / `RF_ArchetypeObject=0x400` per
   `UnObjBas.h:324-325`):
   ```
   [29739] class=Class name=AA_SteamStatManager             flags=0x040f000400000000  (no CDO/Archetype bit)
   [29740] class=AA_SteamStatManager name=Default__AA_SteamStatManager flags=0x0007000400000200  (RF_ClassDefaultObject SET)
   ```
   The class export (29739) has neither flag — and structurally *can't* be
   what's on the wire anyway, since `Cast<AActor>` on a raw `UClass` object
   fails on the receive side. Net_index 29740 (`Default__...`, the true CDO)
   does carry `RF_ClassDefaultObject`. This is independent, on-disk
   confirmation that **the transient/`SerializeCompressedInitial` path is the
   only one that's even logically consistent** here — not just a
   hypothesis to try, but the only one that could receive successfully at all.

3. **Exact `MaxIndex` requirement, computed from the real bits, not guessed.**
   The transient path leaves exactly 7 bits after `Location=(0,0,0)` (11 bits,
   itself forced by the actual captured bits — `Bits=0` field is a hard
   5-bit minimum read regardless of value, consumed exactly as such). Swept
   every `MaxIndex` from 2 to 400 and checked which ones make `RIM(MaxIndex)`
   on those exact 7 bits legitimately run out (`Bunch.IsError()`, §10.3's real
   termination mechanism) rather than resolve to an in-range `RepIndex` that
   then can't fit its value in what's left. **Minimum passing value: 131.**
   `AA_SteamStatManager`'s actual replicated-property count, independently
   derived from the decompiled `.uc` and cross-checked via `--rep-layout`
   against the cooked `.u`, is 25 — nowhere close. (169 total values in
   [131,400) pass, all similarly implausible for this simple 2-struct class.)

**Conclusion:** every wire-format primitive is now proven correct via source and
via two independent byte-identical captures. What remains wrong is upstream of
formats entirely — the `ObjectIndex→(class, RepIndex table)` resolution chain
from §5 (the `+19`/`+18` preamble ambiguity noted in §11.5's CDO-vs-class
net_index anomaly is the same open thread). §5 already tried exhaustively to
nail this from static package analysis alone (§5.7-§5.13) and explicitly
concluded static analysis wasn't sufficient — this session's independent,
much-more-rigorous attempt (with every other variable now eliminated) reaches
the identical conclusion from a completely different angle. **§5.8 Path B.1
(gdb-attach or debug build against a live legit server, to read
`UPackageMap::List[*].ObjectBase` and `FClassNetCache` directly out of process
memory) is no longer just the recommended next step — it's the only remaining
one that isn't more static guessing.**

## 11.7 Path B.1 executed: the `+19` preamble mystery is SOLVED (2026-07-23)

Same overall thread, follow-up session. Ran `AA3Game_console.exe` (the real retail
dedicated server) under Wine locally, got a **real client to connect and play**
(player "DrAxEl", live on `Bridge_night_clear`), and used `gdb` breakpoints —
not blind memory scanning — to read `UPackageMap::List` directly out of the live
process. This closes the `ObjectBase` question that §5.7-§5.13 and §11.5/§11.6
left open, across many sessions.

### 11.7.1 Infrastructure: running + attaching to the real server

`AA3Game_console.exe server "Bridge_night_clear?MissionType=co?..."` under plain
`wine` (no Docker needed) boots fully — engine init, map + all sub-levels loaded,
`AA_GameReplicationInfo`/teams/etc spawned, PunkBuster up — in about 1s once
engine init finishes. It only fails Steam CM auth (no real Steam session), then
runs in stable "LAN mode stand-alone authorization." A real AA3 client can then
connect over UDP 8777 and play normally against it.

`winedbg --gdb` (the Wine-aware gdb-proxy) **crashes internally** on this binary —
unusable. Plain `gdb -p <pid>` works fine once `kernel.yama.ptrace_scope` is
temporarily set to 0 (default 1 blocks non-parent ptrace; needs `sudo sysctl`,
which Claude cannot supply a password for interactively — the user ran it
directly via `!`).

### 11.7.2 Finding `UPackageMap`'s exact layout via Ghidra, without symbols

Two pre-existing Ghidra projects for `AA3Game.exe` were already in the repo
(`AA3_Decomp/AA3_Reversed.gpr`, `AA3Project/AA3ServerProject.gpr`), already
auto-analyzed (3013 functions, 4144 symbols) — no need to redo that. Used
`analyzeHeadless` (`/snap/ghidra/37/.../support/analyzeHeadless`) with small
Java `GhidraScript`s (Ghidra 12.1 dropped built-in Jython for `.py` scripts —
`PyGhidra` isn't configured here, so Java scripts are required) run with
`-readOnly` so nothing gets written back to the shared project.

Method: `strings -a -e l AA3Game_console.exe` (UTF-16LE, since UE3 TCHAR is wide
on PC) to find debug/log strings that exist verbatim in `UnrealEngine3/` source,
then locate their **file offset**, convert to VA via `VA = file_offset + 0x400000`
(confirmed via `objdump -h`: every section's RVA equals its raw file offset for
this binary, no alignment padding), then raw-scan `.text` for that 4-byte address
immediate (a `PUSH`/relocatable reference) to find the single unambiguous call
site, then widen the Ghidra disassembly window around it and scan backward for
the nearest `RET`/`INT3` padding + SEH prologue (`PUSH -1; PUSH <handler>; MOV
EAX,FS:[0]; ...`) to find the true function entry.

Three strings/functions located this way:

| String | Address (VA) | Function found |
|---|---|---|
| `"Forged object: got %s, expecting %s"` | `0x1ef7ec0` | `UPackageMapLevel::SerializeObject`, entry `0x9dfe20` |
| `"Received invalid actor class on channel %i"` | `0x1efb7d8` | (inside `UActorChannel::ReceivedBunch`) |
| `"Received index %i for removed/unloaded package %s"` | `0x1f3b228` | `UPackageMap::IndexToObject`, entry `0xc7d8d0` |

A **fourth, unplanned find**: the function immediately preceding `IndexToObject`
(entry `0xc7d7d0`) turned out to be a full package-list dump/diagnostic routine,
identified via its own format string at VA `0x1f3b0d8`:

```
      Package %i: Name - %s, LocalGeneration - %i, RemoteGeneration - %i, BaseIndex - %i, ObjectCount - %i
```

This one string is what actually cracked the struct layout — it names every
field with its exact position in the argument list, cross-validated against the
loop's own pointer arithmetic:

```
sizeof(FPackageInfo) = 0x44 (68 bytes)     -- confirmed independently via BOTH
                                               functions' loop stride (ADD ESI,0x44 /
                                               ADD ECX,0x44)
FPackageInfo (all offsets confirmed 2 independent ways):
  0x00  PackageName   FName (8 bytes: Index, Number)
  0x08  Parent        UPackage*  (can legitimately be NULL for a manifest-listed
                                   but not-shipped package)
  0x1c  ObjectBase    INT   ("BaseIndex" in the debug string)
  0x20  ObjectCount   INT   (independently confirmed via IndexToObject's own
                              `InIndex < Info.ObjectCount` loop test at the same
                              offset)
  0x24  LocalGeneration   INT
  0x28  RemoteGeneration  INT

UPackageMap object itself: List.Data ptr @ offset 0x40, List.ArrayNum @ 0x44
  (read directly off ECX/this in IndexToObject/SerializeObject, both thiscall)
```

`UArrayProperty::NetSerializeItem` finding from §11.1 and the enum-width/FName
findings from §11.2/§11.3 are all still correct and untouched by this section —
this section is purely about the `ObjectIndex↔object` resolution layer, a
different part of the protocol.

### 11.7.3 Getting a live, populated instance: blind memory scanning doesn't work, breakpoints do

First attempt: scan the *idle* server's writable memory for the `FPackageInfo`
array signature (structural match: stride 0x44, monotonic `ObjectBase`, plausible
`ObjectCount`/`Parent`). Burned significant time on this and it **never found
the real array** — every "hit" was a false positive (regular unrelated tables:
an animation-curve-like array with a smooth +30/+31 arithmetic progression;
another with a narrow-banded repeating `ObjectCount`). Root cause, confirmed via
source: `UNetConnection::InitConnection` constructs each connection's
`PackageMap` fresh (`new(this)UPackageMapLevel(this)`), and even the
connection-independent `UNetDriver::MasterMap` only gets *populated* via
`UWorld::BuildServerMasterMap() → NetDriver->MasterMap->AddNetPackages()`. With
**zero real client connections** (only failed Steam CM auth attempts happened),
there was no guarantee a fully-built, plausible-looking table existed anywhere
in memory yet to find.

Once a real client connected, the fix wasn't a better scan — it was a **gdb
breakpoint at `SerializeObject`'s real entry (`0x9dfe20`)**, which is thiscall
(`this` in `ECX`, cached to `EBP` on entry) and fires on every object-reference
(de)serialization in both directions. It hit immediately and repeatedly once
gameplay was active:

```
HIT SerializeObject: this(ECX)=0x3583bd00
List.Data=0x27f89000 List.Num=171
```

(An earlier attempt breakpointing `IndexToObject` — receive-direction-only —
never hit, because the server mostly *sends* object references, i.e. calls the
send-direction internals of the same dispatcher function, not the client-only
resolve path. `SerializeObject` handles both directions internally via
`Ar.IsLoading()`, so it was the right, universal target.)

### 11.7.4 The real table — and the `+19` mystery resolved exactly

Dumped `0x27f89000` for `171 × 0x44` bytes and decoded every `FPackageInfo`.
Full table: `aa3-linux-server/config/live_packagemap_ground_truth.json`. Sanity
checks against independent static analysis:

- Package 0: `ObjectBase=0, ObjectCount=1328` — **exact match** to Core's known
  `object_count=1328` from the static `.u` analysis (§5.5/§9).
- Package 18: `ObjectBase=38361, ObjectCount=39408` — this is `AA3Baseline`
  (`ObjectCount` **exact match** to the static count; `ObjectBase` is **18
  higher** than the static computation of 38343).

That +18 discrepancy, combined with the CDO-vs-class `NetIndex` anomaly already
found in §11.5 (`Default__AA_SteamStatManager`'s real `NetIndex` is 29740, one
higher than the class object's own 29739), resolves the entire "+19 preamble"
saga in one step:

```
real formula (no fudge constant at all):
    wire_ObjectIndex = ObjectBase[package] + Object->NetIndex

verification against the known-good AA_SteamStatManager wire value (68101):
    38361 (real ObjectBase[AA3Baseline]) + 29740 (CDO's real NetIndex) = 68101  ✓ exact

what the old "+19" was actually doing (two errors canceling):
    38343 (static ObjectBase, 18 LOW)  + 29739 (class NetIndex, 1 LOW, wrong object)
        + 19 (empirical fudge that happened to equal 18+1) = 68101  (worked by accident)
```

§5.5's "six validated classes" were never wrong about *which class* — they were
computing the *right answer* via a formula with two compensating bugs, which is
exactly why nobody could find the bugs by staring at those six matches: the
matches were real, the derivation was coincidentally doubly-wrong.

**Practical upshot:** `aa3-linux-server` should use the real per-package
`ObjectBase` table (`live_packagemap_ground_truth.json`) directly, or (better,
for map-independence) re-derive `ObjectBase` for any package as "cumulative sum
of `GenerationNetObjectCount` over all packages loaded before it, in manifest
order" — the static formula itself was fine; only one input value was off by 18
for reasons not yet root-caused (§11.7.6). And always resolve to the object's
**own** `NetIndex` (which for a transient/class-spawned actor is the **CDO's**
`NetIndex`, `Default__ClassName`, not the class object's).

### 11.7.5 This also overturns the years-old WorldInfo hypothesis for Ch=2

§3.1 hypothesized `Ch=2` (`ObjectIndex=172588`, the biggest first-opened channel
in the legit pcap) was `WorldInfo`, on the theory that it's part of
`Bridge_night_clear`. **The real table refutes this directly:**

```
Bridge_night_clear (package #123): ObjectBase=178576, ObjectCount=15065
    → covers wire indices [178576, 193641)

172588 does NOT fall in that range at all.
```

`172588` actually falls in **package #114** (`ObjectBase=171302,
ObjectCount=2132`, range `[171302, 173434)`), giving a local `NetIndex` of
`172588 − 171302 = 1286` within that package. Package #114's `name_idx` is 5737
— not yet resolved to an actual package name (would need the live `GNames`
table, not captured this session). Whatever Ch=2 actually is, it is **not** the
map's `WorldInfo` — the entire hypothesis chain built on that assumption
(§3.1's channel-to-class table) needs re-deriving against this real table before
being trusted again.

### 11.7.6 Open threads for next time

1. ~~**Resolve package #114's name.**~~ **DONE, same session (2026-07-24).**
   Found the FName resolution function too: `Names = *(FNameEntry**)0x25d2418`
   (global array of `FNameEntry*`, indexed directly by the FName index — no
   hashing/wraparound at this layer), `FNameEntry+0x8` low bit = wide-string
   flag, string data (ANSI or UTF-16LE per that flag) starts at
   `FNameEntry+0x10`. Found by disassembling the `PackageName.ToString()`-style
   call site (`0xc09d10` → delegates to the real lookup at `0xc09a70`) the same
   string-anchored way as everything else in §11.7.2. Resolved all 171 real
   package names in one gdb pass; merged into
   `live_packagemap_ground_truth.json`. Confirms package #123 really is
   `Bridge_night_clear` (`ObjectBase=178576`, matches §11.7.5), and **package
   #114 (where `Ch=2`'s `ObjectIndex=172588` actually lands) is `AA3Game`** —
   local `NetIndex = 172588 − 171302 = 1286` within that package. **Also
   resolved, same session:** scanned every export in `AA3Game.u` for
   `body[0]==1286` (same technique as §5.11) — exactly one hit:
   `Default__AGP_PlayerController` (export #1286, `body[0]` coincidentally
   equal to its own export position, same alignment-early-in-file pattern seen
   for AA3Baseline classes in §5.5). **`Ch=2` is the connecting player's own
   `AGP_PlayerController` CDO** — makes far more sense than `WorldInfo` ever
   did as the first, biggest channel a server opens for a new connection (a
   client needs its own controller immediately; `WorldInfo` has only 28
   properties, nowhere near enough to explain a 2367-bit bunch).
   `AGP_PlayerController` extends `AA_PlayerController` (already fully
   RepIndex-mapped in §10, `MaxIndex=91`) with 2 more own RPCs:
   `--rep-layout AA3Game.u AGP_PlayerController` gives chain
   `Object→Actor→Controller→PlayerController→AA_PlayerController→AGP_PlayerController`,
   **`MaxIndex=93`**. Being a `PlayerController`, its open bunch also needs the
   extra `NetPlayerIndex:1 byte` after `SerializeCompressedInitial` per
   `UnChan.cpp:1413-1417` (§2) — not yet incorporated into `decode_rep_layout2.py`.
2. **Root-cause the remaining 18-object static-analysis gap** (why
   `GenerationNetObjectCount`-based cumulative summing landed 18 short for
   AA3Baseline specifically) — now a small, bounded, well-defined discrepancy
   instead of the earlier "~4,830 mystery," and probably fast to find by
   re-running `--rep-layout`-style counting against the packages between Core
   and AA3Baseline with this exact target number in hand.
3. **Re-decode Ch=2 and the actor-replication bunches from §10/§11.5/§11.6**
   using the *correct* class (`AA3Game.u`, local NetIndex 1286 — needs
   identifying which export that is) instead of the presumed-wrong `WorldInfo`
   — this may turn out to be the actual root cause of the never-closing
   `AA_SteamStatManager` bit-decode puzzle too, if a similar package/class
   misidentification exists elsewhere in the six "validated" classes. Worth
   re-verifying all six against this real table before trusting them further
   (quick check: recompute each of the six wire ObjectIndexes as
   `real_object_base[AA3Baseline] (38361) + CDO_NetIndex`, using the
   `--dump-class`/body[0] technique from §5.11 on `Default__ClassName` instead
   of the class itself, and confirm all six still land exactly on their known
   wire values with the corrected 38361 base).
4. **`FClassNetCache`** (the RepIndex/MaxIndex side, separate from PackageMap)
   was not located this session — same breakpoint-based technique
   (`GetClassNetCache`, called from `UActorChannel::ReceivedBunch`/
   `ReplicateActor`) should work the same way if pursued.

### 11.7.7 First decode attempt on the corrected Ch=2 class — plausible prefix, still desyncs in the property loop

With the class now genuinely correct (`AGP_PlayerController`, `MaxIndex=93`),
ran `decode_rep_layout2.py` (now handles the `PlayerController`
`NetPlayerIndex:1 byte` from §2/`UnChan.cpp:1413-1417`) against `Ch=2` in the
legit pcap:

```
ObjectIndex=172588
SerializeCompressedInitial Location=(4114, -330, 342)   -- plausible map coordinates
NetPlayerIndex=2                                         -- plausible small int
RepIndex=0  iOriginalOwningTeamIndex = -789564399         -- implausible, desync
RepIndex=22 Location = (-8,-2,0)
RepIndex=10 bCollideActors = 0
...
!! RepIndex 0/22/10 is not ascending -- confirms desync, same signature as §11.5/11.6
```

This is a **real, meaningful improvement** — `Location` and `NetPlayerIndex`
are both exactly the kind of values a real spawn should produce (versus the
previous class's immediate hard failures), so the object identity and the
`SerializeCompressedInitial` + `NetPlayerIndex` prefix are very likely correct
now. The property loop itself still desyncs almost immediately after, with the
same non-ascending-RepIndex signature documented in §11.5/§11.6. Candidates for
next session, roughly in order of suspicion:

1. **Rotation may be present and wasn't tried here.** `bNetInitialRotation` is
   very plausibly `TRUE` for a `PlayerController` (the client needs facing
   direction immediately) — try `--rot` (now wired into the transient block).
2. **`RepIndex=22` (`Location`) appearing in the normal property loop right
   after `SerializeCompressedInitial` already sent a location is suspicious**
   — worth checking against source whether `Location`/`Rotation` (RepIndex
   21/22) and/or `RelativeLocation`/`RelativeRotation` (1/2) are excluded from
   the ordinary replication pass for transient actors specifically (i.e.
   whether `ReplicateActor`'s dirty-property scan skips whatever
   `SerializeCompressedInitial` already covered), which the source read so far
   hasn't settled either way.
3. ~~Now that a live retail server is available and controllable (§11.7.1), the
   most decisive next step is likely **gdb-breakpointing
   `UActorChannel::ReplicateActor`'s send-side dirty-property loop itself**~~
   **DONE, same session — see §11.7.8. Result: the RepIndex/MaxIndex
   reconstruction itself is now proven correct against live data.** The
   remaining desync is isolated to the actor-channel-*open* prefix
   (`SerializeCompressedInitial`/`NetPlayerIndex`), not the property loop.

### 11.7.8 CAPSTONE: live `WriteIntWrapped` capture directly confirms the RepIndex/MaxIndex reconstruction is correct

Followed the plan from item 3 above. Found `Bunch.WriteIntWrapped(FieldNetIndex,
MaxIndex)`'s actual call site (`UActorChannel::ReplicateActor`'s property loop,
`UnChan.cpp:1952`) the same string/return-address-chasing way as everything
else this session: breakpointed `UPackageMapLevel::SerializeObject`
(`0x9dfe20`), read its caller's return address off the live process
(`0xc5c182`) while a real client played — that turned out to be
`UObjectProperty::NetSerializeItem` (a small thiscall wrapper reading
`this+0x88` = `PropertyClass` then vtable-dispatching to `SerializeObject`, one
layer too deep). Repeated the same trick one level up: breakpointed *that*
wrapper's entry (`0xc5c160`) and read *its* caller — dominant hit
`0x9f3cff` (many different `this` pointers = many different actors' `Object`
properties, i.e. the real per-property replication loop). Disassembling
backward from there found `CALL 0x00c80fa0` with exactly the argument shape of
`WriteIntWrapped(Value, ValueMax)` (`EAX = [FieldNetCache+4]` i.e.
`FieldNetCache::FieldNetIndex`; `EDX` = a computed `FieldsBase + Fields.Num()`
sum, i.e. `ClassNetCache::GetMaxIndex()`).

Breakpointing `0xc80fa0` (`WriteIntWrapped`'s real entry, reading `Value` at
`[ESP+4]` and `ValueMax` at `[ESP+8]`) while the client played captured **8069
hits in 20 seconds** — turns out `WriteIntWrapped`'s underlying algorithm is
the same one used for *every* RIM-encoded field in the whole protocol (not just
property replication), so most hits are the already-known header constants
(`16384`=`MAX_PACKETID`, `4096`=`BunchDataBits` bound, `2048`=`MAX_CHANNELS`,
`1024`=`MAX_CHSEQUENCE`, `8`=`CHTYPE_MAX` — all exactly matching §11.7.4's
table, another free cross-check). Filtering those out leaves the real
per-property-replication `MaxIndex` values actually observed live:

| `MaxIndex` seen live | count | Matches |
|---|---|---|
| **28** | 47 | **Exact match to our computed `WorldInfo`** (§9.4) |
| **133** | 40 | **Exact match to our computed `AA_GameReplicationInfo`** (§10.1) |
| 139, 131, 491, 47, 71, 30, 32, 31, 212 | various | Not yet mapped to a class (likely the player's `Pawn`/character class — `139` alone had 1222 hits, by far the most active, consistent with continuous movement/animation state — plus weapons, teams, etc.) |

**The two matches are decisive, and the actual `RepIndex` values sent are exactly
right**, not just the `MaxIndex` bound:

- `MaxIndex=28` (`WorldInfo`): every single one of the 47 live hits has
  `Value=26` — our reconstructed field table has RepIndex 26 =
  `WorldInfo.TimeDilation` (a `FloatProperty`). A continuously-fluctuating time-
  dilation float replicating every tick during normal gameplay is exactly the
  right real-world behavior.
- `MaxIndex=133` (`AA_GameReplicationInfo`): the live sequence alternates
  `78, 96, 78, 96, ...` — our table has RepIndex 78 =
  `AA_GameReplicationInfo.iBattlePlanningTimeRemaining` and RepIndex 96 =
  `AA_GameReplicationInfo.iMissionTimeSeconds`, both `IntProperty` countdown
  timers. Two match-timer fields alternately ticking down during an active
  match is exactly the right real-world behavior.

**Conclusion:** the `UClass::Link`-reconstruction algorithm (§9.1), the
`FieldsBase`/`GetMaxIndex()` chaining (§9.4/§10.1), and the resulting
`RepIndex`/`FieldNetIndex` tables this project has built up over many sessions
are now proven correct against live retail-server ground truth — not just
self-consistent with decompiled `.uc` source, but **the actual bytes a real
server sends for real gameplay match our tool's output exactly**. This flips
the remaining §11.7.7 puzzle (`Ch=2`/`AGP_PlayerController` not decoding
cleanly) from "is our RepIndex model even right?" to a much narrower question:
something specific to the actor-channel-*open* prefix
(`SerializeCompressedInitial` and/or `NetPlayerIndex`) — or a transient-actor-
specific difference in which properties enter the ordinary replication pass —
is still wrong, not the property loop itself. `93` (`AGP_PlayerController`'s
`MaxIndex`) did not appear in this particular 20-second capture window, so a
longer/targeted capture (or breakpointing specifically during a fresh
connection's *open* bunch rather than mid-session *update* traffic) is the
natural next step to close that out.

**Follow-up, same session:** tried this twice more (self-terminating captures,
20000 and 30000 hits, each timed to start *before* asking the user to
reconnect so the window covered the actual reconnect). `MaxIndex=93` never
appeared in either. Along the way, caught a live `MaxIndex=94` in the first
attempt and spent real effort chasing it as a possible near-miss
(`AGP_PlayerController` +1 field) — ruled out conclusively: manually audited
every function in `AGP_PlayerController`'s own `Children→Next` chain against
its raw `FunctionFlags` bytes and `SuperField`, confirmed exactly 2 own net
functions with `SuperField=0` (genuinely new, not overrides) — `MaxIndex=93`
is correct, the tool has no bug here. The live `94` (and other unmapped values
like `139`, `131`, `491` — `139` is extremely hot, 1000+ hits per capture,
almost certainly the player's `Pawn`/character class) belong to classes not
yet identified, unrelated to this specific puzzle.

**Conclusion: catching the exact channel-open moment via a generic
"start capturing, then ask for a reconnect" window is not reliably landing.**
The `bNetInitial` burst is evidently narrow enough (relative to
authentication/handshake time before it, which varies) that two honest
attempts with generous hit budgets (20-30k breakpoint hits each) never caught
it. **Next session, don't repeat this approach as-is.** Better angles, roughly
in order of promise:
1. Breakpoint further upstream, at the actual **connection-accept /
   `bNetInitial=1` assignment** (`Actor->bNetInitial = 1;` in
   `UActorChannel::ReplicateActor`, `UnChan.cpp:1734`, gated on
   `OpenPacketId==INDEX_NONE`) rather than the generic property-write call —
   this fires exactly once per fresh channel, so a breakpoint there (even a
   short, bounded one) can't miss the moment the way a generic high-volume
   capture can.
2. Or: since we now have a **known-working recipe for running the real
   server** (§11.7.1), launch a **fresh, throwaway second server instance**
   for this specific test instead of coordinating live with the user's actual
   play session — start the capture, connect a *disposable* client (or even
   just watch what happens on the existing connection's *next* natural
   reconnect/relog, e.g. after a map change or death), removing the
   back-and-forth cost entirely.
3. Or skip live capture for this specific question and go back to a **wireshark
   capture of a fresh connection** (loopback, `tcpdump`, no gdb needed) —
   simpler and 100% guaranteed to catch the open bunch (it's just the first
   `ChType=2` bunch on a new channel), then decode those bytes with
   `decode_rep_layout2.py` using the now-fully-correct `MaxIndex=93` field
   table. This avoids the live-gdb timing problem entirely for the *content*
   question, even though it won't independently verify the live `RepIndex`
   sequence the way the successful `WorldInfo`/`GameReplicationInfo` capture
   did.

### 11.7.8b Chasing the next unmapped live class (`MaxIndex=139`) — strong evidence, name not yet found

Aggregated `MaxIndex` counts across every `WriteIntWrapped` capture log from
this session (`cat scratchpad/writeintwrapped*.log scratchpad/caller*.log |
grep -oP 'MaxIndex=\K\d+' | sort -n | uniq -c | sort -rn`). `139` dominates by
a wide margin — 7248 hits, more than 4× the next-most-common non-header value
— strongly suggesting it's the player's own `Pawn`/character class (constantly
ticking movement/animation, unlike everything else which updates
occasionally).

**Strong supporting evidence:** the actual `RepIndex` values observed for
`MaxIndex=139` are dominated by `22` (1856 hits) and `21` (1353 hits) — in
every class's shared `Actor` base (§10.1), RepIndex 21/22 are
`Rotation`/`Location`, and RepIndex 4 (820 hits) is `Velocity`. A class
constantly replicating its own Location, Rotation, and Velocity is exactly
what a moving character does every tick. Values also reach as high as `131`,
confirming genuine own-fields well beyond any of the base classes checked.

**Class not yet identified, despite a real search:**
- `AA_Pawn` (the obvious first guess, `Object→Actor→Pawn→AA_Pawn`) is
  `MaxIndex=95` — not it.
- Scanned `AA3Baseline.u` directly for every `Class` export whose `SuperStruct`
  field points straight at `AA_Pawn` (reading the raw on-disk `SuperField`,
  same technique as everywhere else this session) — only three:
  `AA_EnemyPawn` (95), `AA_NPCPawn` (99), `AA_VIPPlayer` (95). None match, and
  none sound like a real player's pawn.
- Checked one level deeper down the `AA_NPCPawn` branch too (`AA_NPCMultiplayerPawn`=102,
  `AA_NPCEnemyPawn`=102, `AA_NPCFriendlyPawn`=102, `AA_NPCDeadPawn`=23,
  `AA_NPCPawn_MAT`=99) — all NPC/bot-branded classes anyway, unlikely to be
  what a real connected human player controls, and none match `139`.
- `AA3Game.u` has no class importing `AA_Pawn` at all (checked via
  `--dump-imports`), so the real player pawn class isn't declared there either
  — or inherits through some other chain not yet traced.

**Next-session approach, in order of promise:**
1. Don't keep guessing names — get the actual export directly. The exact same
   `body[0]==<local NetIndex>` scan that pinned down `Ch=2`
   (`Default__AGP_PlayerController`, §11.7.6 item 1) would work here too, **if**
   we had a live wire `ObjectIndex` for this actor to convert to a local
   `NetIndex`. The `WriteIntWrapped` breakpoint only reports `RepIndex`, not
   which actor/`ObjectIndex` it belongs to — extend the breakpoint script to
   also read the *actor* pointer (available in the same stack frame the
   `ReplicateActor` disassembly in §11.7.8 walked through — `ESI` held the
   channel/actor context there) and resolve it back to a wire `ObjectIndex`
   via `ObjectToIndex`-equivalent math, or simply capture a fresh pcap
   alongside the breakpoint session and cross-reference by timing.
2. ~~Alternative: just brute-force every `Class` export in `AA3Baseline.u`
   through `--rep-layout`~~ **DONE, same session — negative result.** Batch-ran
   `--rep-layout` against every single `Class` export: all 1563 in
   `AA3Baseline.u` (background job, ~a few minutes), all 46 in `AA3Game.u`, and
   all remaining classes in the other script packages (`GameFramework.u` 9,
   `OnlineSubsystemAA3.u` 7, `UnrealScriptTest.u` 72 — the full set of `.u`
   script packages this project has ever loaded, per §9.4/§10.1's manifest).
   **Zero matches for `MaxIndex=139` anywhere.** This is a real, useful
   negative result, not a dead end from giving up early — it rules out the
   entire known AA3/Engine class universe as currently understood. Remaining
   possibilities: (a) a resolution edge case in the batch script silently
   producing empty output for the one class that matters (each invocation
   re-resolves the whole inheritance chain fresh; a cross-package hiccup could
   swallow the real answer without erroring loudly — worth re-checking by hand
   for any class whose batch output was blank rather than a number); (b) `Core.u`
   or `Engine.u` themselves weren't exhaustively swept class-by-class this
   session (only specific named lookups like `Pawn`, `WorldInfo`, `Actor`) —
   worth doing the same brute-force sweep there too; (c) the "must be a Pawn"
   assumption itself could be wrong — a very active `Vehicle` (`GameFramework.u`
   has `GameVehicle`=49 as a base; an AA3-specific vehicle subclass with ~90
   more fields is equally consistent with constant Location/Rotation/Velocity
   replication and wasn't specifically searched for by name).

### 11.7.8c `MaxIndex=139` exhaustive sweep completed — likely NOT a class RepIndex bound at all

Finished the sweep §11.7.8b left open, then found a much bigger structural
reason to doubt the "it's an unmapped class" premise entirely.

**Exhaustive class sweep, now genuinely complete and verified clean.** Every
non-editor `.u`/map package this server can ever load was checked, name-agnostic
(every single `Class` export tested via `--rep-layout`, not name-guessing):
`Core.u`, `Engine.u` (1234 real classes after fixing the extraction bug below),
`GameFramework.u`, `OnlineSubsystemAA3.u`, `UnrealScriptTest.u`, `AA3Baseline.u`,
`AA3Game.u`, `GFxUI.u`, `IpDrv.u`, plus the three currently-loaded map packages
(`Bridge_night_clear.u`, `Bridge_inf_classic_large_dev.u`, `Bridge_pathing.u` —
confirmed 0 `Class` exports each, as expected: maps hold actor *instances*, not
class definitions). **Zero matches for `MaxIndex=139` anywhere.**

The Engine.u/AA3Baseline.u batch scripts' class-list extraction had a latent bug
(`grep "class=Class"` substring-matches `class=ClassProperty` too — a *property*
whose value type is a class reference, not a class definition itself — e.g.
`NewActorClass`, `MessageClass` are property names, not real classes). Confirmed
this only ever **added** harmless bogus entries (which just return blank) and
never dropped a real one: the properly-anchored re-extraction (`class=Class `
with a trailing space) produced a class list that is a strict subset of the old
contaminated one (`comm -23` diff empty). So the negative result was already
trustworthy; this just closes the doubt raised in §11.7.8b's caveat (a).
Directly confirmed `Pawn`=45, `Vehicle`=47, `SVehicle`=49, `AA_Pawn`=95 are all
correctly resolved and present in their scans — none is 139, and no AA3Baseline/
AA3Game subclass of any of them is either (the sweep is class-name-agnostic, so
it would have caught a `Vehicle`-branch subclass regardless of naming).

**Bigger finding: `WriteIntWrapped` (entry `0xc80fa0`) has 9 distinct static
call sites, not 1.** Found via a raw PE scan (no live process needed — parsed
`AA3Game_console.exe`'s section table by hand, converted the known entry VA to a
file offset, then scanned `.text` for every `E8` (`CALL rel32`) instruction whose
target resolves to `0xc80fa0`):

```
0x6ec8b7                                                     (lone, far away)
0x9e6e79 0x9e74a4 0x9e75bc 0x9e75da 0x9e75fe 0x9e761b         (tight cluster, ~0x1.9K span)
0x9f3641 0x9f3cb2                                             (pair; 0x9f3cb2 already
                                                                suspected inside ReplicateActor,
                                                                see §11.7.7's rejected-guess note)
```

This means the existing `WriteIntWrapped Value=/MaxIndex=` capture logs
(`writeintwrapped*.log`) conflate **at least three unrelated subsystems** behind
one shared utility function — confirmed by the round power-of-two `MaxIndex`
values already visible in those logs (`4096`, `2048`, `16384`, `1024`) that
obviously aren't class property counts; these are almost certainly
sequence/ack-style bounded counters from the tight 6-call cluster (packet/bunch
header fields), not `ClassCache->GetMaxIndex()`. **`139` was never confirmed to
come from the `ReplicateActor` call site specifically — every capture so far
recorded `Value`/`MaxIndex` only, not the return address, so hits from all 9
call sites are indistinguishable in the existing logs.**

**A concrete non-class explanation for `139` already exists in this doc:**
`live_packagemap_ground_truth.json` package index 3, `EngineMaterials`, has
`object_count=139` (§5.13 independently confirmed this via `GenerationNetObjectCount`
and the body[0] dumper fix — `EngineMaterials → net indices 0–138`, matching
exactly). If the lone distant caller `0x6ec8b7` (or one of the other non-
`ReplicateActor` sites) is a static-object-reference serialization
(`UPackageMap`-style, bounding an on-wire object index by its *package's*
`ObjectCount` rather than a class's property `MaxIndex`), then `139` hits would
mean "a reference into the `EngineMaterials` package," not "a property write for
an unidentified Pawn/Vehicle class" — fully consistent with high frequency (every
actor with a material reference touches this) and with small `Value`s like
21/22/4 (valid material indices, not suspiciously-coincidentally
Rotation/Location/Velocity `RepIndex`es).

**Decisive test in progress:** a new breakpoint script
(`bp_writeintwrapped_retaddr.gdb`) captures `RET=0x%x Value=%d MaxIndex=%d` per
hit (return address = `*(int*)$esp` at the function's very first instruction,
calling-convention-independent). Grouping real captured hits by `RET` and
checking which address produces the `139` hits — and whether that address is the
*same* one that produces the already-confirmed-correct class values (`28`
WorldInfo, `93` AGP_PlayerController, `133` GRI) or a different one — will settle
this conclusively. **Blocked as of this write-up on an actively connected,
playing client**: the live server (PID 636165) is healthy but idle (CPU barely
ticking over background load), so zero breakpoint hits have occurred yet at any
of the 9 sites. Not a technique failure, just needs traffic.

### 11.7.9 §11.7.8c settled WITHOUT a live client — static call-graph tracing matches the source formula exactly (2026-09-20)

Rather than wait on live traffic, resolved the ambiguity statically: read-only
Ghidra headless (`analyzeHeadless AA3Project AA3ServerProject -process
AA3Game.exe -readOnly -noanalysis`, project confirmed unlocked — the only
running Ghidra process on this machine had an unrelated project open) to
decompile the functions actually containing the 3 non-cluster
`WriteIntWrapped` (`FUN_00c80fa0`) call sites from §11.7.8c.

**Result: the "pair" (`0x9f3641`, `0x9f3cb2`) are both inside one function,
`FUN_009f3030` (0x9f3030, 4379 bytes), called from `FUN_006ec680` (0x6ec680,
the function containing the "lone distant" `0x6ec8b7` site).** `FUN_006ec680`
is the actor-replication dispatch function itself: besides calling
`FUN_009f3030`, it directly contains the RPC-call bunch path, with literal
error strings `"Error: Can't send function '%s' on '%s': RPC bunch
overflowed (too much data in parameters?)"` and `"...Reliable buffer
overflow"` — this is `UActorChannel::ReplicateActor`'s RPC dispatch, not a
guess. Its own `WriteIntWrapped` call (the "lone distant" `0x6ec8b7`) sits in
that RPC path, bounded by `*(int*)(iVar3+0xc) + *(int*)(iVar3+0x20)` — a
second instance of the same base+count summing pattern, but for RPC/function
net-indexing, not property `RepIndex`. This **refutes** the §11.7.8c
hypothesis that it's `UPackageMap`-style package-object-count bounding
(`EngineMaterials`=139); it's a third, unrelated subsystem, just like the
tight 6-call cluster turned out to be packet/bunch-header fields (confirmed
separately below).

`FUN_009f3030` (called per-actor from the dispatch function above) has
exactly two `WriteIntWrapped` calls:

```c
// ~0x9f3641 — fixed bound, unrelated to RepIndex (role/relevance byte path)
FUN_00c80fa0(uVar15, 0x20);

// ~0x9f3cb2 — dynamic bound, inside the actual per-property replication loop
local_13c = (int *)FUN_00c7ff00(*(undefined4 *)(*(int *)(param_1 + 0x70) + 0x34));
...
for (; piVar18 < piStack_150; piVar18 = piVar18 + 1) {
    // piVar18 walks a dirty/changed RepIndex set; param_1+0x74 -> +0x100 is
    // indexed by RepIndex to fetch {UProperty*, ArrayIndex} — this is
    // UClass::ClassReps, an FRepRecord table, read exactly as UE3 source
    // describes it.
    piVar7 = *(int **)(iVar8 + *piVar18 * 8);        // FRepRecord.Property
    iVar8  = *(int *)(iVar8 + 4 + *piVar18 * 8);      // FRepRecord.Index (array index)
    iVar9  = piVar7[0x13] * iVar8 + piVar7[0x1a];     // Property->ElementSize*Index + Property->Offset
    // ... Role/RemoteRole substitution by FName check (0x2e6/0x2e7) ...
    iVar16 = FUN_006ec060(piVar23);                   // resolve {property -> net-cache record}
    FUN_00c80fa0(*(undefined4 *)(iVar16 + 4),         // the record's RepIndex field
                 local_13c[3] + local_13c[8]);        // MaxIndex bound
}
```

`local_13c` is the return of `FUN_00c7ff00(Class)` — a class-net-cache
lookup — and `local_13c[3] + local_13c[8]` is a **base offset + a count**,
summed, exactly matching `FClassNetCache::GetMaxIndex() { return FieldsBase +
Fields.Num(); }` (the §9.1-cited `UnClass.cpp` formula). The property lookup
immediately above it (`UClass::ClassReps` walk, `ElementSize*Index+Offset`
offset math, Role/RemoteRole FName-based substitution) is unmistakably UE3's
real `ReplicateActor` per-property loop — there is no other candidate
interpretation for that shape of code.

**Conclusion: the `aa3_build_manifests --dump-class`/`--rep-layout`
`MaxIndex = FieldsBase + Fields.Num()` computation (§9.1, already
implemented) is independently confirmed correct at the disassembly level.**
No live client, no gdb, no traffic needed for this half of the question — it
fell out of tracing 3 static call sites to their containing functions and
matching the resulting shape against the known UE3 source formula. What
remains genuinely live-only is narrower than §11.7.8c implied: not "which
call site is RepIndex" (now known: `FUN_009f3030`'s second call), but
confirming the *numeric* class-to-`ClassCache` mapping (which concrete class
owns a given live `local_13c` pointer) for classes not yet independently
verified via `--rep-layout` against pcap bytes — that part does still need
either live memory (a populated `FClassNetCache*` per class) or working
through `FUN_00c7ff00`'s own logic statically (unexplored; next candidate for
a pure-static pass before reaching for gdb again).

### 11.7.10 `FUN_00c7ff00` traced statically — the class↔`MaxIndex` mapping needs NO live memory at all; `MaxIndex=139` mystery closed by exhaustive sweep (2026-09-20)

Followed the "next candidate for a pure-static pass" from §11.7.9 through
immediately, same session. `local_13c = FUN_00c7ff00(Class)` (the pointer the
`FUN_009f3030` RepIndex call reads `[3]`/`[8]` from) decompiles to
`UPackageMap::GetClassNetCache` almost line-for-line:

```c
int * __thiscall FUN_00c7ff00(int *param_1, int param_2 /* UClass* */)
{
    // memoized lookup (hashtable keyed by Class*) — not live/runtime state,
    // just a cache: same Class* always yields the same result.
    ... if already cached, return cached FClassNetCache* ...

    piVar5[3] = 0; piVar5[4] = 0; piVar5[5] = 0;      // FieldsBase=0, pSuper=0, RPCsBase=0
    if (*(int *)(iVar7 + 0x40) != 0) {                // Class->SuperField/SuperStruct
        iVar3 = FUN_00c7ff00(*(int *)(iVar7 + 0x40)); // recurse: GetClassNetCache(SuperClass)
        piVar5[4] = iVar3;                            // pSuper
        piVar5[5] = *(int *)(iVar3 + 0x14);           // inherited RPCsBase
        piVar5[3] = *(int *)(iVar3 + 0x20) + *(int *)(iVar3 + 0xc);  // FieldsBase = pSuper->GetMaxIndex()
    }
    // walk Class->Children (iVar7+0x110 = count, +0x10c = array) — OWN
    // fields only, filtered by the bNet flag (0x8000) — build this class's
    // own Fields[] and Fields.Num() (piVar5[8]).
    ...
    return piVar5;
}
```

Every input (`Class->SuperField`, `Class->Children`, each field's net flags)
is static `.u`-file class metadata — the same data `aa3_build_manifests`
already parses. The hashtable lookup at the top is pure memoization (no
non-determinism); recursion bottoms out at `UObject` (no super). So
**`FieldsBase + Fields.Num()` (i.e. `MaxIndex`) is a pure function of the
class hierarchy, computable identically offline — which is exactly what
`--rep-layout` already does.** `FUN_006ec060` (the property→`FieldNetCache`
resolver used at the RepIndex call site) just walks this same `pSuper` chain
(`*(int*)(param_1+0x10)`) looking for a hashtable hit — again memoization
over static data, not live game state. Nothing in this chain touches runtime
object instances.

**This means the §11.7.8c `MaxIndex=139` question is answerable by brute-force
enumeration, no gdb needed.** Ran `--rep-layout` across every real `Class`
export (`class=Class `, space-anchored per the §11.7.8c fix) in all 9 script
packages — 2448 classes total (`Core.u`=8, `Engine.u`=1234, `GameFramework.u`=9,
`OnlineSubsystemAA3.u`=7, `UnrealScriptTest.u`=72, `AA3Baseline.u`=1049,
`AA3Game.u`=43, `GFxUI.u`=16, `IpDrv.u`=10 — `Engine.u`'s 1234 matches
§11.7.8c's count exactly, confirming the sweep is complete). Spot-checked the
tool against the two other already-known-correct values along the way:
`Engine.GameReplicationInfo` alone → `MaxIndex=43`; the actual runtime class
`AA3Baseline.AA_GameReplicationInfo` → **`MaxIndex=133`**, matching §11.7.8's
citation exactly and confirming `World::InitializeActors()` is right to use
the `AA_`-prefixed subclass, not bare Engine classes, when this gets wired
into real property replication.

Full results saved as `aa3-linux-server/config/class_maxindex_table.tsv`
(2446 classes; a couple of the 2448 attempted produced no output and were
dropped, not investigated further — negligible).

**Result: zero classes produce `MaxIndex=139`.** Combined with §11.7.9 (the
`0x9f3641` fixed-bound-0x20 site can't produce 139 either — 139 > 32 — and the
6-cluster sites are fixed round powers of two, never 139), this rules out
*every* property-RepIndex-adjacent interpretation of the old "139" logs. The
only structurally-similar-but-unexplored candidate left is the RPC path found
in §11.7.9 (`0x6ec8b7`, also a `FieldsBase+Count`-shaped dynamic bound, but
for `UFunction` RPC net-indices via `piVar5[5]`/`RPCsBase`, a table this
tooling doesn't build yet) — plausible, but a different and lower-priority
question than property replication, and not blocking the actual server work.
**Practical takeaway: the property-replication `MaxIndex` model is now
validated by two independent methods (disassembly shape-matching + exhaustive
static enumeration against real values) with zero live-client dependency.**
The original §11.7.8/11.7.8c framing — "decisive test... blocked on an
actively connected, playing client" — was true for disambiguating *which*
`WriteIntWrapped` call site matters, but that question turned out to be
answerable from the static call graph alone; live gdb was never actually
required for it.

- `aa3-linux-server/config/live_packagemap_ground_truth.json` — the real
  171-package table.
- `/tmp/.../scratchpad/bp_serializeobject.gdb` (not committed — scratch) — the
  working breakpoint script; reproduce via: `break *0x9dfe20` in `AA3Game.exe`,
  read `List.Data=*(int*)($ecx+0x40)`, `List.Num=*(int*)($ecx+0x44)`.
- Ghidra Java scripts used (`FindXrefs.java`, `FuncAt.java`, `DisasmAndFind.java`,
  `WideDisasm.java`/`WideDisasm2.java`, `ListProgram.java`, `DisasmFName.java`) —
  scratch, not committed, but the *technique* (string→raw-address-scan→Ghidra
  disassemble-in-place→RET-boundary function-start scan, all via `-readOnly`
  headless so the shared Ghidra project is never mutated) is the reusable part;
  redo them fresh next time rather than hunting for these exact files.
- **FName resolution** (`name_idx` → real string): global
  `Names = *(FNameEntry**)0x25d2418` (array of `FNameEntry*`, indexed directly,
  no hashing at this layer); `FNameEntry+0x8` low bit = wide-string flag;
  string data at `FNameEntry+0x10` (ANSI or UTF-16LE per that flag). Found via
  the same string-anchored technique, disassembling the
  `PackageName.ToString()` call site (`0xc09d10` → `0xc09a70`). This resolves
  *any* `name_idx` seen anywhere in this project, not just packages — reusable
  for resolving object/class names generally if further live-memory work is
  done.
- `decode_rep_layout2.py` now also handles `--rot` (Rotation in
  `SerializeCompressedInitial`) and auto-detects `PlayerController` in the
  class name to read the extra `NetPlayerIndex:1 byte`.

## 12. Property encoder implemented for WorldInfo, GRI, TeamInfo (2026-09-21)

`aa3-linux-server/network/rep_encode.{h,cpp}` now encodes real
`RepIndex:RIM(MaxIndex)` + value bunches for the three placed/persistent
actor types the world model holds (`ServerWorldInfo`, `ServerGameReplicationInfo`,
`ServerTeamInfo`), replacing the empty BDB=32 channel-open shells and the
ack-only Tick() placeholder. Verified via a standalone encode→decode
round-trip (independent `BitReader`, not the encoder's own logic) for all
three: every field lands at its documented RepIndex with the correct value,
and each full-state encode consumes **exactly** its declared bit count with
no overrun — see the class docstrings in `rep_encode.h` for the specific
fields skipped (type mismatches, non-existent properties found along the
way) per class.

**Bonus closure of a 2026-05-xx open item:** §3.1's channel-to-class
hypothesis table left `ObjectIndex=70992` (the TeamInfo channels, Ch=6-9) as
an unverified guess ("looks like TeamInfo"). Resolved it properly this
session: looked up 70992 against the *corrected* package-base table
(`config/live_packagemap_ground_truth.json`, the one §11.7.4 fixed via live
gdb) — package `AA3Baseline`, `NetIndex=32631` — then cross-referenced that
`NetIndex` against `aa3-tools/PackageMapDumper/out_full.json`'s per-export
dump, which lands exactly on `Default__AA_TeamInfo`. Confirms `AA_TeamInfo`
(`MaxIndex=66`) is correct, not just plausible-by-naming-convention.

### 12.1 PlayerReplicationInfo — first transient actor, adds SerializeCompressedInitial (2026-09-22)

Extended the encoder to `AA_PlayerReplicationInfo` (`MaxIndex=207`; 10 real
fields matched against `ServerPlayerReplicationInfo`: `bBot`/`bOutOfLives`/
`bOnlySpectator`/`bIsSpectator`/`bAdmin`, `TeamID`, `PlayerName`, `Ping`
(plain byte), `Deaths`/`Score` (`FloatProperty` on the wire, cast from the
`int32_t` this server models them as)). `UniqueId` (RepIndex 26, a
`UniqueNetId` struct = raw QWORD per sec 11.1) is left unsent —
`unique_id_hex` is never populated anywhere in `World`, so there's nothing
real to encode yet.

PRI is the first actor this server spawns with `transient=true` (server-
spawned, not placed), which per `UnChan.cpp:1768-1797` means its channel
open needs `SerializeCompressedInitial` (Location, then Rotation only if
`bNetInitialRotation`) between the `Archetype` reference and the property
loop — a preamble none of WorldInfo/GRI/TeamInfo (all placed) needed. Read
`FVector`/`FRotator::SerializeCompressed` directly from
`UnMath.cpp:51-129` rather than relying on sec 2.2/10.4's summary, and
implemented both exactly (`WriteCompressedVector`'s `Bits` selection —
`clamp(CeilLogTwo(1+max(|round(x)|,|round(y)|,|round(z)|)),1,20)-1` —
matches source line-for-line). `OpenActorChannel` now takes an optional
`transient_actor` param that builds this preamble into its own sub-writer
first (same measure-then-splice pattern as the property payload) so
`BunchDataBits` stays exact regardless of the preamble's value-dependent
bit cost.

Verified: PRI's 10-field encode (309 bits) round-trips exactly through an
independent decoder. The zero-vector `SerializeCompressedInitial` case
matches sec 10.5's own hand-derived worked example bit-for-bit (11 bits:
5 for the `Bits` field + 2×3 for `DX`/`DY`/`DZ` at `Bits=0`). A non-trivial
vector `(100,-50,12345)` also round-trips exactly, correctly selecting
`Bits=13`, closing the loop on the one primitive sec 10.5 flagged as
"exact bit layout TBD" back in the original research pass.

### 12.2 PlayerController — the fourth ObjectIndex found by static resolution alone, and the first real (non-NULL) object reference (2026-09-22)

`World::CreatePlayerController` never had a real `ObjectIndex` for the PC
actor itself (`object_index = 0; // fill when known` — no channel was ever
opened for it, only its PRI). Resolved it the same way as sec 12's
`AA_TeamInfo`: `Default__AA_PlayerController`'s `NetIndex=25382` in
`AA3Baseline.u` + the corrected package base (38361) = **`ObjectIndex=63743`**,
cross-validated in both directions (package-base lookup ↔ per-export dump).
Unlike `AA_TeamInfo`, there was no existing pcap-captured value to compare
against for this one specific actor — but the same method exactly reproduces
the PRI's already-known, pcap-captured `60069`
(`Default__AA_PlayerReplicationInfo` → `NetIndex=21708` + `38361`), a third
independent match (after GRI and TeamInfo), which is enough confidence to
trust 63743 without its own capture to check against. `World` now sets it
for real; Ch=11 opens for it (transient, with `SerializeCompressedInitial` +
`NetPlayerIndex`, per `UnChan.cpp:1792-1797`).

`AA_PlayerController` itself contributes no data fields to
`ServerPlayerController` — but `Controller.PlayerReplicationInfo`
(RepIndex 23, `ObjectProperty`) maps exactly onto the `pri` pointer the
struct already holds, and unlike every other `ObjectProperty` gap in this
file (`Winner`/`Pauser`/`Pawn`), this one is a **real, non-NULL object
reference** — `cur.pri->object_index`, not a stub. First time this encoder
has sent anything other than 0 through `WriteObjectRefProp` (renamed from
`WriteNullObjectProp` now that it does both). Verified: encodes to
`RepIndex=23, idx=60069` exactly, decodes bit-exact, and the diff correctly
fires only when the referenced PRI's `object_index` actually changes.

### 12.3 Per-tick diffing for TeamInfo/PRI/PC, and a real Pawn actor (2026-09-23)

**Per-tick diffing.** `ClientSession` now carries `last_sent_teams[4]`/
`last_sent_pri`/`last_sent_pc` (alongside the existing GRI/WorldInfo
snapshots), seeded in `OpenInitialActorChannels` and diffed every 200ms
tick exactly like GRI already was. Each dirty actor is its own packet — no
multi-bunch batching (a packet can legally carry more than one bunch per
the sec 2 grammar, but nothing here builds one yet; worth doing if a real
client session shows this producing an unreasonable packet rate). WorldInfo
still isn't diffed — nothing mutates it after spawn in this model, so it
would always be a no-op.

**A real Pawn.** `AA_Pawn` (`MaxIndex=95`) resolved the same way as
`AA_PlayerController`: `Default__AA_Pawn` `NetIndex=8103` + AA3Baseline's
corrected base = **`ObjectIndex=46464`**, computed and trusted on the
strength of the method's 3-for-3 track record against real captured values
(GRI, TeamInfo, PRI) plus one prior computed-only value (PC) that had no
capture to check against either — this is the second in that latter
category.

Deliberately scoped down to 4 real fields, not the full ~95: `Velocity`
(RepIndex 4, Actor-base — zero today, nothing moves a Pawn yet, but real
storage, free to wire up), `PlayerReplicationInfo` (29) and `Controller`
(44) (both real object references, same non-stub treatment as
`EncodePCRep`'s), and `Health` (30). `Location`/`Rotation` (21/22) are
deliberately kept OUT of the property loop even on the initial send: they're
already covered by `SerializeCompressedInitial` on the channel-open bunch
(sec 12.1), and duplicating them in the normal RepIndex loop would send the
same data twice. They'll need to join the loop once something actually
moves a Pawn after spawn — the open-only preamble can't carry position
updates, only the property loop can. `Controller.Pawn` (RepIndex 24 on the
PC side) — previously left unsent because no Pawn existed — is now a real
reference too, following the exact pattern
`Controller.PlayerReplicationInfo` already established.

Spawned alongside the PC/PRI on connect (`World::CreatePlayerController`),
not gated behind a team-select/deploy flow — this server doesn't have one
yet, so there's nowhere later to defer spawning to. Opens on Ch=12. Same
pre-existing lifecycle gap as PRI: `World::RemovePlayerController` doesn't
clean up the PRI or the new Pawn on disconnect (only the PC itself) — not
new to this change, worth fixing together with PRI's cleanup at some point.

Verified: Pawn's 4-field encode (134 bits) round-trips exactly; PC's encode
now correctly carries both real references (RepIndex 23→60069, 24→46464);
diffs correctly no-op when unchanged and fire only on real changes.

### 12.4 Pawn field set expanded: movement tuning, stance, body identity (2026-09-25)

Added 15 more real `AA_Pawn` fields on top of sec 12.3's 4: the 6
movement-tuning floats + `bSimulateGravity` (RepIndex 31-36, 40), 3
plain-byte fields `FiringMode`/`FlashCount`/`RemoteViewPitch` (37-39, each
confirmed non-Enum via the same empty-Enum-column check already used for
`Ping`/`PacketLoss` in sec 11.2), 3 stance bools `bIsProne`/`bIsCrouched`/
`bIsWalking` (41-43), and body identity `sBodySoldierName`/`iBodyTeamIndex`
(47-48). 19 real fields total now.

The movement-tuning values aren't guesses: found `Engine.Pawn`'s actual
`defaultproperties` block
(`UnrealEngine3/Development/Src/Engine/Classes/Pawn.uc:3645-3668` —
`AccelRate=2048`, `AirSpeed=600`, `GroundSpeed=600`, `JumpZ=420`,
`WaterSpeed=300`, `AirControl=0.05`, `bSimulateGravity=true`), then checked
both AA3's `uscript` and `uscript_decompiled` `AA_Pawn.uc` for an override —
neither has one, so these are exactly what a real `AA_Pawn` instance
carries, not placeholders.

Deliberately still excluded: the ~70 remaining fields, mainly because
they'd require either an unresolved Enum width (`IncapacitationLoc`'s
`eHitLocation` — nobody's run `--struct-layout` on it yet), a TArray (all 3
injury-state fields — wire no-ops anyway per sec 11.1, so no value in
wiring them up), or a real inventory/pouch subsystem this server doesn't
have (`ActiveInventory`, `DumpPouch`, etc.).

Verified: 19-field full encode (620 bits) round-trips exactly, values match
the sourced defaults precisely (`AirControl=0.05`, `JumpZ=420`, etc.), and
a diff touching 2 fields (`GroundSpeed`, `bIsCrouched`) fires exactly those
2 and nothing else.

### 12.5 `eHitLocation` enum width resolved; 7 more Pawn fields; 2 deliberately excluded (2026-09-25)

**Resolved `IncapacitationLoc`'s blocker.** Found `eHitLocation`'s export
(`AA3Baseline.u[6402]`, class=Enum) and read `NumEnums` at the byte offset
sec 11.2 already derived (offset 20 into the `UField` body — `NetIndex(4) +
None-tag(8) + SuperField(4) + Next(4)`, then a raw `INT32`), via
`--dump-body-hex`: **19**. Didn't trust that on a new enum blind — first
reproduced the *already-known* `ENetRole`=5 and `ECollisionType`=10 with the
exact same technique (both matched exactly) before applying it to
`eHitLocation`. `CeilLog2(19-1)=5` bits.

**Added 7 more real fields**: `fDesiredLeanDirection`(52), `LastKnownPRI`(53,
another real object reference, same treatment as `PlayerReplicationInfo`/
`Controller`), `Stamina`(59), `Asphyx`(60), `AimOffset`(62),
`IncapacitationLoc`(63, the newly-resolved 5-bit enum), and
`AdvMvmt_VaultMantleHeight`(64) — 26 real fields total now.

**Deliberately excluded `MovementSpeedModifier`(51) and `HealthTarget`(61)**,
even though they're simple floats like the rest of this batch: both read as
multiplier/lerp-target fields (a 0 *speed multiplier* means "frozen in
place," not "unset"), `AA_Pawn.uc` has no `defaultproperties` override for
either (same check as everything else), and there's no way to confirm from
static script analysis alone whether native `PostBeginPlay` code initializes
them to something sane (1.0, `=Health`) before a real client would ever see
them. Sending the raw script-default 0 for either would be a guess dressed
up as a sourced value — the one thing this file's methodology has
consistently avoided — so they're left out rather than encoded wrong.

Verified: 26-field full encode (756 bits) round-trips exactly;
`IncapacitationLoc` correctly survives the 5-bit width (encoded 7 → decoded
7); `AimOffset`'s negative-value handling checked (-12 round-trips exactly);
diff correctly isolates a single changed field.

### 12.6 First custom (non-native) struct: `PawnShotEffectInfo` (2026-09-25)

Every `StructProperty` encoded so far (`Vector`/`Rotator`, including inside
`SerializeCompressedInitial`) used one of UE3's 5 hardcoded native-type fast
paths (doc sec 11.1). `AA_Pawn.PawnShotEffectInfo` (RepIndex 50) is the
first one that doesn't — its type, `AA_PawnHitEffectInfo`, isn't `Vector`/
`Rotator`/`Quat`/`Plane`/`UniqueNetId`, so it falls into the generic
recursive path: `--struct-layout` resolved it to exactly 2 fields
(`HitVector`, `HitRotation`), and sec 11.1's own already-established rule
("no CPF_Net filter, no per-field RepIndex — just `TFieldIterator` order")
means encoding it is: write the outer RepIndex 50 once, then
`WriteCompressedVector` + `WriteCompressedRotator` back to back,
unconditionally, with no inner framing at all. No new wire primitive
needed — this only required combining two already-verified ones in the
struct's declared field order.

Zero vector/rotation is the correct default: this server has no combat/
damage system, so there's never a real hit to report yet.

Verified: a non-zero `HitVector`/`HitRotation` round-trips exactly through
an independent decoder, including `HitRotation`'s lossy high-byte-only
compression behaving correctly (`0x1234` → `0x1200`, i.e. only the top 8
bits of the 16-bit angle survive, exactly as `FRotator::SerializeCompressed`
specifies); the struct-level diff correctly fires only when at least one of
its 6 underlying floats/ints actually changes. 27 real Pawn fields total
now.

### 12.7 Remaining Pawn gaps triaged; 3 closed, 3 left deliberately open with reasons (2026-09-25)

Went through every field still unmodeled and sorted them by *why* they were
open, since "not modeled yet" was starting to blur two very different
situations:

**Closed — definitionally NULL, not a guess.** `DrivenVehicle`(25),
`CurrentTreatmentPatient`(55), `IncapacitationType`(58) all send NULL,
initial-send-only, same wire treatment as `Winner`/`Pauser`. But unlike
those two, there's no server_actors.h field behind any of these — nothing
to track, because this server has no vehicle, medical-treatment, or
incapacitation system *at all*. "Not driving/treating/incapacitated" isn't
an assumption about real AA3 behavior here, it's simply true of this
server's state. 30 real fields total now.

**Investigated, left open — for three different reasons, not one:**
- `InvManager`(24)/`PlayerProxy`(49): the mirror-image problem from the
  three just closed — these are plausibly *always* non-null on a real
  spawned pawn (an inventory manager is normally auto-spawned with its
  owner). Sending NULL here would misrepresent real behavior, not just omit
  unknown state, so left unsent rather than asserted.
- `aaController`(70): checked whether it's just a same-object typed-cast
  alias of `Controller`(44) — the standard UE3 idiom for this exact
  situation (base class holds a generic reference, subclass caches a
  strongly-typed one). Found the one place that would confirm it,
  `AA_Pawn::PossessedBy`, and it's one of the functions AA3's custom
  bytecode breaks UELib's decompiler on ("Statement decompilation error:
  Index was out of range") — so this is a plausible pattern match, not a
  confirmed fact, and left out on that basis. The one gap in this file
  that's closer to "probably right" than "structurally certain."
- Injury state (3 `TArray` fields) + `CurrentTreatmentTreater[]` (also
  `TArray`): confirmed these would carry zero payload bits even if
  "correctly" sent (`UArrayProperty::NetSerializeItem` no-ops both
  directions, sec 11.1) — functionally indistinguishable from never
  sending them. Not worth building array-dirty-tracking for a wire event
  with no observable client-side effect either way.

This closes out the Pawn field-coverage work for now — every remaining gap
has a specific, load-bearing reason rather than just being unaddressed.

## 13. Next actor: `AA_SteamStatManager` (Ch=3), and a resolved historical anomaly (2026-09-26)

Moved to the next channel still using the old raw/stub treatment (Ch=3,
hardcoded `ObjectIndex=68101`, zero property data). `--rep-layout` gives
`MaxIndex=25`: 23 Actor-base + exactly 2 own properties, both
`StructProperty` over custom stat-block types
(`AA_MP_PlayerMatchStatistics`, `AA_MP_PlayerStatistics`) —
`--struct-layout` resolves them to 12 and 116 fields respectively, all flat
`Bool`/`Float`/`Int`/`Byte` (no enums, no nesting), matching sec 11.1's
original validation of these exact two structs exactly.

**Resolved a standing anomaly from sec 11.5.** That section flagged: reading
`Default__AA_SteamStatManager`'s `NetIndex` gave a computed `ObjectIndex` one
higher than the retail-captured `68101`, with the captured value instead
matching the *class* export's own `NetIndex` — seemingly contradicting the
"send the Archetype/CDO" theory from source. Recomputed with the corrected
package base (38361, from sec 11.7.4's live-gdb fix, which postdates sec
11.5) instead of the old static base that section used: `Default__AA_Steam-
StatManager`'s `NetIndex` (29740) + 38361 = **68101** — an exact match. The
CDO theory was right all along; sec 11.5's anomaly was the identical
base-offset bug sec 11.7.4 later fixed for everything else, just not yet
recognized as the same bug at the time. One more data point (5th, after
GRI/TeamInfo/PRI/PC) confirming the corrected-base method.

**Modeled as `ServerSteamStatManager` — deliberately zero data members.**
Both stat structs are pure counters (shots fired, kills, hit-location
tallies, etc.) that this server has no combat system to ever increment.
Rather than hand-transcribe 128 field names into C++ struct members that
would sit permanently unmutated, the two structs are generated as a
kind-sequence string (`B`=Bool/`F`=Float/`I`=Int/`Y`=Byte) pulled directly
from `--struct-layout`'s output via a script, cross-checked byte-for-byte
against the tool's raw output rather than hand-typed — 128 fields is enough
that a manual transcription slip wouldn't surface until a real client
desynced. `EncodeSteamStatManagerRep` fires exactly once (hardcoded
`if (prev != nullptr) return 0`, not just currently-zero data) and sends
both structs as their honest all-zero/false "no stats recorded" state.

One per connected player (spawned in `World::CreatePlayerController`
alongside PRI/PC/Pawn), transient (confirmed by sec 11.6's CDO-flag check
on this exact class), so its Ch=3 open also carries the
`SerializeCompressedInitial` preamble like PRI/PC/Pawn.

Verified: 2-field encode is 2518 bits, decodes bit-exact through both
structs; a subsequent call with any `prev` always returns 0 fields, as
designed.

## 14. Last raw stub closed: `AA_MOSQualificationBCT` (Ch=5) — every retail-opened channel now modeled (2026-09-28)

Same treatment as sec 13, but simpler: `--rep-layout` gives `MaxIndex=23` —
**zero** own properties, just the inherited Actor-base 23 this project has
consistently left unencoded everywhere. Nothing to build an encoder for;
`ServerMOSQualificationBCT` has no data members and there's no
`EncodeMOSQualificationBCTRep` function, because there's nothing a
property-loop could ever carry for this class.

Resolved `ObjectIndex` the same way as every actor since sec 12.3:
`Default__AA_MOSQualificationBCT`'s `NetIndex` (18071) + the corrected
package base (38361) = **56432** — exact match against the existing
retail-captured value, a 6th confirmation of the resolution method.
Confirmed `RF_ClassDefaultObject` set on that export (`flags=0x...200`),
same check as sec 11.6/12.8, so it's transient and gets the
`SerializeCompressedInitial` preamble too. One per connected player,
spawned alongside PRI/PC/Pawn/StatManager.

**This closes out the "raw stub" channels entirely.** Every channel the
client's initial actor burst opens (Ch=2 WorldInfo, Ch=3 SteamStatManager,
Ch=4 GRI, Ch=5 MOSQualificationBCT, Ch=6-9 TeamInfo×4, Ch=10 PRI, Ch=11 PC,
Ch=12 Pawn) now carries a real, RE-grounded server-side actor — either with
verified property data, or a verified-empty payload where the real class
genuinely has none to send. None of them are placeholder byte templates or
unverified guesses anymore.

## 15. RPC / function calls — the wire format §2 left as "…" (2026-09-28)

Every section so far encoded properties. `RepIndex` can also point at a
`UFunction` (§2's grammar: `if FieldCache is a UFunction: // RPC handling —
read parameters via Function->FunctionMapPropertyList …`), and that
branch was never resolved — genuinely open ground, not a gap in an
already-mapped area.

Read the real send-side implementation directly, `UnScript.cpp:2842-3036`
(`InternalProcessRemoteFunction`, called via `AActor::ProcessRemoteFunction`
/ `CallRemoteFunction`). Ground truth:

```
Bunch.WriteIntWrapped(FieldCache->FieldNetIndex, ClassCache->GetMaxIndex())
for each parameter (declaration order, excluding the return param):
    if BoolProperty:
        value : 1 bit                    // always sent, no flag
    else:
        Send : 1 bit                     // 1 = differs from zero/default
        if Send: value : NetSerializeItem format (per ArrayDim element)
```

Two things worth being precise about:
- **The RepIndex table is the same one properties use.** Functions and
  properties share one `FieldNetIndex` sequence per class — already visible
  in every `--rep-layout` dump as interleaved `Function`/`Property` rows
  (e.g. `AA_PlayerController`'s `29 Function ClientDrawCoordinateSystem()`
  sitting between `Property` rows). No new RepIndex/MaxIndex machinery
  needed — `network::kPCMaxIndex` (91) is reused as-is.
- **The per-parameter Send-flag rule is NOT the same as a struct's inner
  fields.** `PawnShotEffectInfo` (sec 12.6) always serializes both its
  members unconditionally, no flag. RPC parameters get an explicit 1-bit
  "did the caller pass something non-default" flag per non-bool parameter.
  Easy to conflate — both are "things inside something one outer RepIndex
  triggers" — so `network/rpc_encode.h` documents the distinction
  explicitly rather than leaving it implicit.

**The resulting bunch is shape-identical to a property-update bunch**
(bOpen=false, ChType=2, RepIndex-prefixed payload) — so
`ProtocolHandler::SendActorUpdateBunch`, already built for property diffs,
sends RPC payloads completely unchanged. Only the payload-building logic
is new (`network/rpc_encode.{h,cpp}`); no new bunch-header or
packet-sending code was needed.

**First concrete RPC implemented**: `PlayerController.ClientMessage(coerce
string S, optional Name Type, optional float MsgLifeTime)` — RepIndex 34
in `AA_PlayerController`'s table. Sends a real "Welcome to the server,
`<name>`!" message on the PC's own channel (Ch=11) right after that
channel opens — RPCs target a specific actor instance and must ride that
actor's channel, and per source the channel must already be open before an
RPC can be sent on it (`UnScript.cpp:2890`), which the channel-open bunch
just above satisfies. `Type`/`MsgLifeTime` are left at their UE3 defaults
(`Send=0` for both) — no message-type categorization or custom message
durations exist on this server yet, same "send only what's genuinely
backed" rule as everywhere else in this doc.

Verified: full RPC encode is 297 bits, decodes bit-exact including the
message text round-tripping exactly; the empty-string and nonzero-float
Send-flag edge cases both behave correctly (empty string → `Send=0`,
`5.5` → `Send=1` with the value following).

This is infrastructure for RPCs generally, not just one message — adding
the next one is a new `Build<Name>RPC` function in `rpc_encode.cpp` plus
its RepIndex from an existing `--rep-layout` dump; no new wire-format
research needed unless a future RPC's parameters include a type this file
hasn't encoded yet (Name, Object, Struct, arrays).

## 16. Five more RPCs — every parameter type category covered except Name (2026-09-28)

Went through `AA_PlayerController`'s full function list looking for the
next tractable batch. Implemented, all from clean (non-decompiled) base
UE3 source:

- **`Controller.ClientSetRotation(rotator NewRotation, optional bool
  bResetCamera)`** (RepIndex 25) — first `Rotator` *RPC parameter*.
  Different from a struct's inner Rotator (sec 12.6): the parameter itself
  gets the outer Send-flag test, and only if `Send=1` does
  `WriteCompressedRotator` (already-verified primitive) run.
- **`Controller.ClientSwitchToBestWeapon(optional bool bForceNewWeapon)`**
  (RepIndex 26) — trivial, and needs no server-side inventory data since
  the client does its own weapon selection.
- **`PlayerController.TeamMessage(PlayerReplicationInfo PRI, coerce string
  S, name Type, optional float MsgLifeTime)`** (RepIndex 35) — first RPC
  with a real (non-NULL) object-reference *parameter*: the speaking
  player's own PRI.
- **`PlayerController.ClientSetProgressMessage(EProgressMessageType
  MessageType, string Message, optional string Title, optional bool
  bIgnoreFutureNetworkMessages)`** (RepIndex 36) — first RPC with an
  Enum-typed parameter. Resolving `EProgressMessageType`'s `NumEnums` the
  same way as sec 12.5 turned up something worth flagging: the `.u` file
  says **7**, but counting the named values in
  `UnrealEngine3/.../PlayerController.uc`'s script source gives **8**
  (`PMT_Clear` through `PMT_SocketFailure`). The cooked package is the
  actual wire ground truth regardless of why they disagree (version skew
  between this source tree and AA3's exact fork is the likely cause) —
  concrete proof this file's "always read Enums from the `.u` data, never
  hand-count script source" rule isn't just caution for its own sake.
  Live trigger uses value `1` for the message type without independently
  confirming that index is still `PMT_Information` specifically in this
  cooked build (only the *count*, hence the bit-width, was verified) —
  flagged in the code comment; any valid value encodes correctly regardless,
  and the message text is what actually informs the player either way.
- **`PlayerController.ClientSetViewTarget(Actor A, optional
  ViewTargetTransitionParams TransitionParams)`** (RepIndex 37) — the
  `TransitionParams` struct parameter is always `Send=0`: struct
  *parameters* get the same "differs from zero" test as everything else
  (unlike a struct's *inner* fields), so passing an implicit all-zero one
  means its field layout never needs resolving at all. Live trigger points
  the client's camera at its own just-spawned Pawn.

All 6 RPCs now sent as a real post-spawn batch (view target → orient →
best-weapon, then a self-addressed `TeamMessage` + connection-status
`ClientSetProgressMessage`), not synthetic one-offs. Verified: all 30
round-trip checks pass (RepIndex, every Send-flag in both the "sent" and
"skipped" state, every value, exact bit-count match) across all 5 new RPCs.

**Checked two more candidates, both dead ends, not silently skipped:**
`PlayerController.ClientDrawCoordinateSystem` (RepIndex 29) doesn't exist
anywhere in the available UE3 source tree despite being listed in
`--rep-layout`'s output (possibly a symbol resolved from a header/macro
this text search can't find, or a version mismatch) — no source, no
implementation. `AA_Pawn.ClientEndHoldBreath` (RepIndex 46) *is* in the
decompiled source, but its signature decompiled to `native static final
singular operator(26) ClientEndHoldBreath(optional bool bForceCancel)` —
not valid UnrealScript syntax, the same AA3-custom-bytecode decompiler
corruption noted elsewhere in this doc (sec 12.7's `aaController` gap hit
the identical issue). Not implemented on that basis, not guessed.

**What's left, by why it's excluded, not just "not done":** `Name`-typed
parameters (`ClientGotoState`) — never needed yet since every RPC with an
optional Name parameter so far could legitimately stay at `Send=0`, so
`doc sec 11.3`'s NameProperty format remains unimplemented in code, only
documented; real object/asset references this server doesn't have
(`ReceiveLocalizedMessage`'s `LocalMessage` class, `ClientHearSound`'s
`SoundCue`, `ClientStopForceFeedbackWaveform`'s waveform); no meaningful
trigger without a real subsystem behind it (`ClientCommitMapChange` needs
an active level-transition flow; `AddDebugText`'s 9 parameters are mostly
debug-only and not worth the RE cost); and the ~15 remaining AA3-specific
`Client*`/notification functions (scoreboard, votes, admin, inventory
HUD), most of which share `ClientEndHoldBreath`'s decompiler-corruption
risk and would need individual signature verification before any of them
could be trusted the way this section's batch is.

## 17. The other direction: decoding incoming client RPCs (2026-09-29)

Every prior RPC section was server→client. This is genuinely the reverse
direction — reading, not writing — and it turned out the hard part was
already done.

**`ProcessPostStormPacket` already walks the general ack/bunch grammar
correctly for any channel/type** — it just treated anything besides Ch=0/
ChType=1 as opaque: log at DEBUG, `reader.Skip(bdb)`. That skip-via-BDB
behavior is exactly the safety property real decoding needs too (doc sec
10.3: BDB is the sole authority on where a bunch ends, never an in-band
marker), so the fix was narrow: give `ChType=2` (Actor) its own branch that
calls a new `DecodeActorBunch`, then **unconditionally** `reader.SetPos(
bunch_start + bdb)` afterward — regardless of how far the decoder actually
got. `DecodeActorBunch` itself is designed to fail safe: the moment it
hits a RepIndex it can't interpret (unmapped property, or a function with
unresolved parameters), it stops trying to read further entries in that
one bunch and returns immediately. It never guesses a bit width. Under-
decoding one bunch costs nothing (the caller resyncs regardless); guessing
wrong would silently corrupt every element after it in the same bunch.

**Decoding requires knowing which class table a channel's RepIndex belongs
to** — a bug source with real consequences (wrong table = wrong RIM bit-
width = garbage for everything after). Added `ClientSession::
channel_max_index`, populated at the exact same call sites that already
open each channel (`open_for_actor` now takes the class's `MaxIndex`
directly, reusing the same constants `network::EncodePCRep`/etc. already
use — one source of truth, not a second table that could drift from the
first).

**Implemented exactly one client→server RPC**: `PlayerController.
ServerViewSelf(optional ViewTargetTransitionParams TransitionParams)`,
RepIndex 38 on the PC channel (Ch=11). This is the *only* `Server*`-
prefixed function on `AA_PlayerController` confirmed from clean,
non-decompiled UE3 source — inherited unchanged from base
`Engine.PlayerController`. Every other `Server*` candidate checked
(`ServerReturnToParty`, `ServerVoteKick`, `ServerMapVote`) is AA3-specific
and hit the identical decompiler corruption pattern already seen twice
before (sec 12.7's `aaController`, sec 16's `ClientEndHoldBreath`) —
`ServerMapVote` even carries an explicit `/* Statement decompilation
error: Index was out of range */` marker in its decompiled body, and
`ServerVoteKick`'s signature line itself is garbled
(`unreliable server static final iterator singular exec event
operator(116)` — not valid syntax). Also caught a real version-skew trap
before it became a bug: `ServerSay(string Msg)`, from the same reference
UE3 source tree as `ServerViewSelf`, looked equally trustworthy — but
checking AA3's own cooked `Engine.u` directly shows it's simply **not**
`FUNC_Net`-flagged in this build, so it doesn't exist on the wire at all
here regardless of what the (evidently newer/different-patch) reference
source shows. Exactly the discipline this whole project has run on:
the cooked package is ground truth, a reference source tree is a lead to
verify, not a fact to use directly.

`ServerViewSelf`'s parameter (`TransitionParams`) is the same struct
`BuildClientSetViewTargetRPC` (sec 16) never had to resolve, for the same
reason — but on the *decode* side, a real client sending a non-default
one can't just be skipped. If `Send=1`, `DecodeActorBunch` logs that it
saw a non-default call and bails out of the bunch rather than guess the
struct's layout — the fail-safe design handles this cleanly rather than
needing it solved before this could ship at all.

**Verification**: the private method itself can't be driven in isolation
without the full handshake state machine, so — matching this whole
project's established pattern of verifying wire-format logic standalone
rather than full live integration — replicated `DecodeActorBunch`'s exact
algorithm in a standalone harness against 5 cases: a single call, two
calls back-to-back in one bunch (proves the loop correctly continues after
success), the `Send=1` bail path, an unrecognized-RepIndex bail path, and
— the one that actually matters for safety — a recognized call followed by
trailing bits it can't interpret, confirming it never reads past what it
understands even when bits remain in the declared BDB. All 5 behave
exactly as designed. The simple integration around it (session/channel-map
lookups) was checked by code review and the standard clean-build/clean-
boot pass, consistent with how every other piece of protocol glue in this
project has been checked.

## 18. `--struct-layout` reads Function parameter lists too — bypassing decompiler corruption where it matters, and an unresolved dead end (2026-09-29)

Went looking for a second incoming RPC. Every remaining `Server*` candidate
on `AA_PlayerController` is AA3-specific, and every one checked so far hit
decompiler corruption on its *body* (sec 17). But the corruption is
specifically about bytecode — `UFunction::Serialize`'s parameter list comes
from walking `Children`, exactly the same `UProperty` chain
`CollectStructFields` already walks for custom structs (the 128-field stat
blocks, `AA_PawnHitEffectInfo`, etc.), completely independent of the
bytecode blob that fails to decompile. `DoStructLayout` just hardcoded
`FindExport(name, "ScriptStruct")` as its entry point; since a `UFunction`
*is* a `UStruct` (and `CollectStructFields` already treats `Function` as a
valid recursion target for nested struct fields — the `is_struct_sub`
check was already there), letting it also accept `"Function"` as the
starting point took one `FindExport` fallback line.

Ran it on `ServerReturnToParty` and `ClientDrawCoordinateSystem` (both hit
corrupted bodies: `/* Statement decompilation error: Index was out of
range */` and garbled modifier soup like `static final iterator singular
exec event operator(116)` respectively) — and got clean, complete parameter
lists for both: `ServerReturnToParty(bPartyMembersOnly: BoolProperty)`,
`ClientDrawCoordinateSystem(AxisLoc: Vector, AxisRot: Rotator, Scale:
Float, bPersistentLines: Bool)`. Both match exactly what the corrupted
decompiled *signature line* showed (the body is what's garbled, not the
declaration) — an independent cross-check that this metadata-only
technique is sound, not just "different from a source that also might be
wrong."

**Added `ServerReturnToParty` as the second decoded incoming RPC.** RepIndex
42 on the PC channel. Its single `BoolProperty` parameter has no "Send"
flag (always exactly 1 bit, per doc sec 15's rule) — simpler than
`ServerViewSelf`'s struct parameter, and unlike that one, never has a case
it can't fully decode. Verified via the same standalone-algorithm-
replication approach as sec 17: a lone call, and two different recognized
functions decoded back-to-back in one bunch (`ServerReturnToParty` then
`ServerViewSelf`), both consuming exactly the declared bits.

**A genuine dead end, reported rather than shipped:** tried to also verify
`Client*`/`Server*` directionality independent of the naming convention, by
reading each function's raw `FunctionFlags` word (`UnStack.h`'s
`FUNC_NetServer`/`FUNC_NetClient`/`FUNC_NetReliable` bits) directly from
the tail of its `Function` export (`UFunction::Serialize`:
`FunctionFlags(4) [+RepOffset(2) iff FUNC_Net] FriendlyName(8)`, anchored
from the end since script bytecode is token-serialized and its on-disk
length isn't independently knowable). The byte offset checks out —
`RepOffset` decodes to `0xFFFF` (`MAXWORD`), the same sentinel
`UFunction::Serialize` uses elsewhere in this exact function
(`ReturnValueOffset = MAXWORD`) for "not applicable," which is a strong
structural sanity check that the right bytes are being read. But the
*value* is still wrong: `ClientMessage` (confirmed `reliable client event`
from clean source) decodes to `FUNC_NetServer` set and `FUNC_NetReliable`
clear — backwards from what the declaration says. `FUNC_NetServer`'s own
doc comment in `UnStack.h` — "set by replication code if passes check" —
reads as a *dynamically*-assigned flag, which shouldn't appear in static
cooked data at all, so either AA3's engine build numbers these specific
bits differently than this reference header, or there's a subtler byte-
layout issue the `MAXWORD` coincidence is masking. Removed the
direction-labeling output before it shipped (it produced confident-looking
but wrong labels — worse than not having the feature) and reverted to the
`Server*`/`Client*` naming convention as the only trusted direction signal,
same as every prior RPC in this doc. `NetField::func_flags` stays in the
tool, unused for labeling, as raw data for whoever picks this back up.

## 19. AA3 shuffles the `FUNC_*` bits — `FUNC_Net` is `0x100`, not `0x40`; every class with RPCs had the wrong RepIndex table (2026-09-30)

**The bug.** `--rep-layout` decided "is this function networked" by testing
stock UE3's `FUNC_Net = 0x40` (`UnStack.h`). AA3's build assigns the
`FUNC_*` bits differently, so the tool silently dropped ~88% of net
functions — including `ServerMove`, `ServerSay`, `ClientAdjustPosition`,
and every other movement RPC. Because functions and properties share one
`FieldNetIndex` sequence (sec 15), each missed function shifted every later
RepIndex *and* shrank `MaxIndex`, which changes the RIM bit-width of every
RepIndex on that channel. Every class with net functions was mis-encoded:

| class | old MaxIndex | correct |
|---|---|---|
| AA_PlayerController | 91 | **473** |
| AGP_PlayerController | 93 | **491** |
| AA_Pawn | 95 | **131** (AA_Pawn fields from `sBodySoldierName` on shifted +6) |
| AA_PlayerReplicationInfo | 207 | **212** |
| AA_TeamInfo | 66 | **70** |
| AA_SteamStatManager | 25 | **28** |
| WorldInfo / AA_GameReplicationInfo / AA_MOSQualificationBCT | 28 / 133 / 23 | unchanged (no net functions ahead of their fields) |

This is also why the live-verified classes in sec 11.7.8 (WorldInfo, GRI)
never exposed it: neither has net functions.

**How the real bits were derived — from structure, not semantics.**
`UFunction::Serialize` writes `RepOffset(2)` iff `FUNC_Net`. That field's
presence is observable in the export tail independent of what any flag bit
*means*: with it, the word at `size-12` reads `0xFFFFxxxx` (RepOffset =
`MAXWORD` + FriendlyName's first bytes) and the real flags sit at
`size-14`. Over all 3172 functions reachable from the 6 main replicated
classes (AA3Baseline + Engine):

- bit `0x100` predicts RepOffset presence **perfectly** (523/523, 0 false
  positives) → AA3's `FUNC_Net`.
- bits `0x200` (266) and `0x4000` (257) partition those 523 exactly and match
  the `Server*`/`Client*` naming → server-bound / client-bound.
- bit `0x1` is clear on exactly the functions stock UE3 declares
  `unreliable` (`ServerMove`, `DualServerMove`, `ClientAdjustPosition`,
  `ClientHearSound`, `ServerViewSelf`, `ServerSay`, `ServerUse`) → reliable.

Detection rule now in the tool: net iff `ffB & 0x100` and `ffB & (0x200|0x4000)`
and `!(ffA & 0x100)` (the direction-bit term rejects the one false positive,
`Object.SubtractEqual_StrStr`, whose script tail bytes happened to set 0x100).
This closes sec 18's "dead end": the labels were wrong because the bit
positions were stock UE3's, not because the offset was wrong. It also
invalidates sec 17's conclusion that `ServerSay` "isn't FUNC_Net in this
build" — it is (`rep::PC::ServerSay` = 55, unreliable).

**Independent confirmation against live retail data.** Re-sweeping all 2366
classes with the fixed tool, *every* unexplained `MaxIndex` from the sec
11.7.8 live `WriteIntWrapped` capture now maps to a concrete, plausible
class — overturning sec 11.7.8c's "139 is likely not a class bound":

| live MaxIndex | class |
|---|---|
| 28 | WorldInfo |
| 133 | AA_GameReplicationInfo |
| 212 | AA_PlayerReplicationInfo |
| 491 | AGP_PlayerController (the "93 never appears" mystery: it was 491) |
| 131 | AA_Pawn |
| 139 | AA_NPC*Pawn (NPC soldiers — the hottest channel) |
| 71 | AA_BulletWeapon (M4A1 / M16A4 / ...) |
| 47 | AA_PlayerStorageProxy |
| 94 | AA_MCClassRoleReplicationInfo |
| 30 / 31 / 32 | ammo containers / inventory pouches / XM320 mod |

**Server changes.**
- `tools/gen_rep_layout.py` generates `include/network/rep_layout_gen.h`: per
  class, `kMaxIndex` plus a named constant per net field (with direction +
  reliability for RPCs). The encoders (`rep_encode.cpp`, `rpc_encode.cpp`)
  and the incoming-RPC decoder now reference fields **by name** — no
  hand-typed RepIndex numbers remain. A future tool fix propagates by
  regenerating the header. 27/27 GRI names mapped to the identical indices
  they had, an independent check of the name mapping.
- `tests/rep_roundtrip_test.cpp` (CMake target `rep_roundtrip_test`) encodes
  a full Pawn state and the 6 PC RPCs, then decodes them using types parsed
  from the tool's `--rep-layout` output (not the encoders' constants): 30
  Pawn fields / 1037 bits bit-exact, all 6 RPCs resolve to the right
  function. Feeding it the wrong class's layout fails loudly (checked).
- Server builds and boots clean (`--standalone`).

**Open / next.**
1. The server announces the PC as `Default__AA_PlayerController` (MaxIndex
   473); retail uses `AGP_PlayerController` (491). Consistent either way
   (the client builds its cache from whatever class we name), but switching
   to AGP would match retail and pick up its 18 extra fields.
2. **Movement.** `ServerMove` = `rep::PC::ServerMove` (61, unreliable),
   `DualServerMove`/`OldServerMove` nearby, and `ClientAdjustPosition` /
   `ClientAckGoodMove` are the server's reply. **Trap:** `--struct-layout`
   on a Function lists locals as well as parameters (ServerMove shows 16
   fields; stock UE3 has 6 params: `TimeStamp, InAccel, ClientLoc,
   MoveFlags, ClientRoll, View`). Needs a `CPF_Parm` filter — and given
   this section, verify AA3 hasn't also shuffled `CPF_*` bits before
   trusting stock values (`CPF_Net=0x20` for properties *is* live-verified).
3. **Stats.** `AA_SteamStatManager` has `ServerStatsTransmittedByClient` /
   `ServerMatchStatsTransmittedByClient` (server-bound, reliable) — the
   client pushes its own stat blocks to the server. Decoding these is the
   natural source for the stats→web pipeline, and the 128-field struct
   layouts are already resolved (sec 13).

## 20. Client movement: `ServerMove` family decoded from real retail traffic, implemented server-side (2026-09-30)

**Parameters vs locals.** A `UFunction`'s `Children` chain holds its
parameters *and* its locals, so `--struct-layout ServerMove` used to list 16
fields. `PropertyFlags` are **not** shuffled in AA3 (unlike `FUNC_*`, sec 19):
`CPF_Parm=0x80`, `CPF_OptionalParm=0x10`, `CPF_OutParm=0x100`,
`CPF_ReturnParm=0x400`, `CPF_CoerceParm=0x800` all verified (ServerMove's 6
params = `PlayerController.uc:2910` exactly with the 10 locals at `pf=0`;
`ClientMessage`'s `coerce string S` + two `optional`s; `GetAdjustedAimFor`'s
`ReturnValue`). `--struct-layout` on a Function now prints only real
parameters (marking optional ones) plus raw `pf=` for every row.

**Retail validation.** `aa3-tools/map_manifest/decode_servermove.py` walks
every client→server bunch in `GamePlayPCAP_Legit_Server.pcapng` (14 min of
real play, client :47026 → server :8777) and decodes against
`AGP_PlayerController` (MaxIndex 491). The retail PC rides **Ch=2**. Of
54,138 Ch=2 bunches, **99.78% decode to exactly their BunchDataBits**:

| RPC (rep::PC) | count | notes |
|---|---|---|
| ServerMove (61) | 28,987 | TimeStamp +~23ms/move; ClientLoc e.g. (11385,−3308,45→30) = spawn + land |
| ServerSetSpectatorLocation (88) | 20,159 | pre-spawn / dead spectating |
| ServerOffsetAim (155) | 7,474 | AA3 weapon-sway aim offset (int) |
| DualServerMove (59) | 4,420 | |
| OldServerMove (60) | 518 | |
| ForceServerOffsetAimUpdate (154), ServerUpdatePing (70), ServerHoldSprint (182), ServerStopSprint (187), ServerRaiseSights (170) | 521 | |

The 121 non-exact bunches stop at rare, not-yet-tabled RPCs (177
ServerToggleStance, 179/180 ServerSetProne/Crouch, 200, 321, 76, 171, ...).
This is the strongest confirmation of the sec 19 table: hand-picked indices
from the corrected tool landed on semantically-right functions with
bit-exact parameter widths across tens of thousands of real messages.

Wire details confirmed: RPC Vector params use `FVector::SerializeCompressed`
(integer components); `View = (Yaw << 16) | (Pitch & 0xFFFF)`; `InAccel` is
acceleration ×10; `ClientRoll = Roll >> 8`.

**Server reply cadence (retail, server→client Ch=2).** `ClientAckGoodMove`
(66): 21,137 in 783s = **~27/s, unreliable**, BDB **42 bits** (RIM(491)=9 +
Send 1 + float 32), one per server tick acking only the newest TimeStamp.
Corrections are rare: `ClientAdjustPosition` (67) 214, `VeryShort` (63) 44,
`Short` (64) 36.

**Implemented.**
- `network/rpc_decode.{h,cpp}`: table-driven `DecodePcRpc` (one row per
  function: RepIndex from `rep_layout_gen.h` + param kinds). Replaces the
  hand-rolled ServerViewSelf/ServerReturnToParty branches. Same fail-safe
  contract as sec 17: any unknown index / unresolved struct / overrun stops
  decoding that bunch; the caller resyncs via BDB.
- `ProtocolHandler::ApplyPcRpc`: ServerMove/DualServerMove → drop stale
  TimeStamps (`CurrentTimeStamp` rule), set pawn location from `ClientLoc`
  (ignoring UE3's `(1,2,3)` "no location" sentinel), PC/pawn rotation from
  `View`/`ClientRoll`; ServerSetSpectatorLocation → PC location;
  ServerUpdatePing → `PRI.Ping = clamp(NewPing/4, 0, 250)` (stock rule).
- `BuildClientAckGoodMoveRPC` + unreliable bunch support in
  `SendActorUpdateBunch` (no ChSequence consumed). `Tick` sends at most one
  ack per 33ms acking the newest move — matches retail's ~27/s, 42 bits.
- `tests/rpc_decode_pcap_test` runs the C++ decoder over the exported
  retail bunches (99.78%, threshold 99%) and round-trips the ack.

**Deliberate limits.** No server-side physics: `ClientLoc` is trusted and
every move is acked good (never `ClientAdjustPosition`) — fine for a
first playable server, no protection against speed/teleport hacks. Pawn
`Location` isn't replicated yet, so *other* players won't see this
player move; that needs Actor.Location/Rotation (RepIndex 22/21) on the
pawn channel for non-owning connections (owner is autonomous and must not
receive it). Our server's PC is on Ch=11, retail's on Ch=2 — consistent
either way since the decoder keys on the channel's recorded MaxIndex.

**Next.** (1) Replicate pawn Location/Rotation to non-owners → visible
multiplayer movement. (2) Table the stance RPCs (177/179/180) and drive
`bIsCrouched`/`bIsProne`. (3) SteamStatManager's
`Server*StatsTransmittedByClient` for the stats pipeline.

## 21. Multiplayer: other players' pawns, per-connection actor references, roles (2026-10-01)

**Retail census first.** `aa3-tools/map_manifest/channel_census.py` resolves
every server→client channel OPEN in the retail gameplay pcap to its object
(package base + export-table position, which *is* the NetIndex for cooked AA3
packages) and tallies the RepIndex fields each class later receives. That
capture has one human player (1 PRI, 3 `AA_Pawn` opens = respawns), so it
can't show a non-owned pawn, but it confirms the owner side: the player's
own pawn gets Health (30), Stamina (65), AimOffset (68), RepGraphStates (71)
etc. and **never Location (22)** — stock UE3 behavior for an
AutonomousProxy.

**Bug 1 — actor references were wrong everywhere.** `UPackageMapLevel::
SerializeObject` (UnNetDrv.cpp) writes a spawned actor as **bDynamic=1 +
RIM(channel index on that connection, MAX_CHANNELS=0x800)**, and NULL as
`1 + RIM(0, 0x800)`. Only package/static objects use `0 + RIM(ObjectIndex,
2^31)`. Retail confirms: `PC.PlayerReplicationInfo` = dynamic ch 10,
`PC.Pawn` = dynamic ch 159/161/163, `ClientSetViewTarget(pawn)` = dynamic
ch. The server was sending `bDynamic=0` + the *class default object's*
index, so on the client PC.Pawn, PC.PlayerReplicationInfo, Pawn.Controller
and the view target all pointed at `Default__*` objects. Fixed:
`WriteActorRef`, `RepContext::channel_of` (per-connection actor→channel
map) and `RepContext::sent_refs` (per-channel RepIndex→last wire value), so
a reference sent as NULL before its target's channel opened is re-sent
once it does — UE3's `Mapped=0` retry (PC.Pawn now goes out NULL in the
Ch=11 open and is corrected on the next tick once Ch=12 is open).

**Bug 2 — roles never sent.** `UnChan.cpp:1942` writes the server's `Role`
under `RemoteRole`'s index and vice versa; the client spawns channel actors
with swapped class defaults (`UnLevAct.cpp:554`). Without explicit roles
the client sees *its own* pawn as a SimulatedProxy. The pawn's initial
bunch now carries Role (= AutonomousProxy for the owner, SimulatedProxy
for others — `UnChan.cpp:1803` downgrade), RemoteRole = Authority, and
`bNetOwner`. ENetRole is 2 bits (sec 11.2).

**Bug 3 — incoming movement was never decoded.** Unreliable non-open
bunches carry no ChType on the wire, and the RX dispatcher only recognized
actor bunches by ChType — but every retail `ServerMove`/`DualServerMove`/
`ServerSetSpectatorLocation` bunch is unreliable. Now any unreliable bunch on
a channel we opened as an actor channel goes to `DecodeActorBunch`.

**Bug 4 — clients never disconnected.** The socket layer never fired
`connected=false`, so departed players' actors and channels lived forever.
Added an idle timeout = AA3Engine.ini `[IpDrv.TcpNetDriver]
ConnectionTimeout=60.0` (override: `AA3_CONNECTION_TIMEOUT_S`).

**Owner-aware Pawn encoding** (Actor.uc/Pawn.uc replication blocks):

| who | fields |
|---|---|
| initial, all | Role/RemoteRole (swapped), bNetOwner, DrivenVehicle (NULL) |
| owner (AutonomousProxy) | Location/Rotation on init only; Health; Controller; Air/Ground/Water speed, AccelRate, JumpZ, AirControl |
| non-owners (SimulatedProxy) | Location, Rotation, Velocity, Physics (4 bits), RemoteViewPitch; bIsCrouched, FlashCount, FiringMode |
| everyone | PlayerReplicationInfo, bSimulateGravity, bIsWalking, bIsProne, all AA_Pawn fields (AA3's own conditions unrecoverable, sec 12.7) |

**Remote players.** Each connection opens every *other* player's PRI and
Pawn (never their PC — owner-only) on channels allocated from 13 up, PRI
first so the pawn's PRI reference resolves immediately. Non-movement diffs
go reliable on the 200ms tick; movement (Location/Rotation/Velocity/
RemoteViewPitch) goes **unreliable at 20Hz** with a forced 1s keyframe
(unreliable updates can be lost, and diffing would otherwise leave a pawn
frozen where its last-lost update would have put it). When a player leaves
(or their pawn/PRI changes identity), the channels get a reliable
`bClose` bunch. Velocity is derived from consecutive `ServerMove`
positions (no server physics); `RemoteViewPitch` = view pitch >> 8.

**Verified.**
- `rep_roundtrip_test`: owner pawn decodes with Role=AutonomousProxy, refs
  = channels 10/11, owner-only fields present and no non-owner ones;
  non-owner pawn decodes with Role=SimulatedProxy, Location/Rotation/
  Velocity/Physics/bIsCrouched/RemoteViewPitch and no owner-only fields;
  movement-only diff after a move = exactly Location; PC.Pawn sent NULL
  then re-sent as ch 12 once open.
- `tests/fake_client.py` (scripted 2-client run against the real server):
  B sees A's PRI/Pawn open on ch 13/14 and gets ~18 Location updates/s
  tracking A's walk (11399→12579 over 4s at 300uu/s, Y fixed); both get
  ~25 acks/s; with `--leave-test` A goes silent, times out, and B gets
  close bunches for ch 13/14.

**Not verified with a real AA3 client** (scripted clients only). Remaining
known gaps: no reliable retransmission (a lost reliable bunch stalls that
channel on the client — pre-existing, matters off-LAN); channels are never
reused; departed players' PRI/Pawn objects aren't freed (kept so pointers
held elsewhere stay valid); no relevancy culling (everyone sees everyone).

## 22. Stance/posture, and the rest of the client's PC traffic (2026-10-01)

**Every client RPC in the retail capture is now tabled.** Resolving each
RepIndex the decoder stopped on (names from `rep_layout_gen.h`, parameter
lists from `--struct-layout`) and re-measuring: **54,137 of 54,138** retail
PC-channel bunches decode to exactly their BunchDataBits in the C++
decoder (one stray index, 233). New parameter kinds: FString, object
reference, FName. The decoded transcript is a complete retail join flow:
`ServerUpdateLevelVisibility` per streamed level → `ServerSetTS3ClientID` →
11× `ServerSetLoadoutPreference` + 7× `ServerSetWeaponPreference`
("AA_BulletWeapon_M4A1", "...M249", ...) → `ServerJoinTeam(0)` →
`ServerSelectClassRole_ByIndex(1)` → `ServerSetPrimaryWeaponOne(class)` /
`...AmmoTypeOne(class)` / `ServerSetRailMod("PrimaryOne","Main",1)` →
`ServerSetPlayerReady(true)` → `AskForPawn()` →
`ServerAcknowledgePossession(pawn = dynamic ch 159)`.

**AA3 FName wire format differs from stock** (sec 11.3 corrected):
FString + INT32 Number, no `bHardcoded` bit. Found decoding the server's
`NotifyPostureChange` replies — the stock form gave a garbage 0x80000004
string length; dropping the bit gives `"EndWalk"` exactly.

**Posture model, read off the retail server's replies.** Each stance input
gets an immediate reliable reply on the PC channel:

| client | server reply |
|---|---|
| `ServerToggleStance()` | `NotifyPostureChange("EndWalk", true)`, `NotifyTacticalCombatPostureChange(b)`, `NotifyWeaponReadyChange(true)`, `NotifyTacticalCombatPostureChange(b)` — `b` alternates 1,0,1,0: toggle = **tactical-combat posture** |
| `ServerSetCrouch(true)` while sprinting | `NotifyPostureChange("EndSprint", false)` |
| `ServerHoldSprint()` while crouched | `NotifyPostureChange("EndCrouch", true)` |
| `ServerStopSprint()` | `NotifyPostureChange("EndSprint", false)` |
| `ServerHoldSprint()` from walking | `NotifyPostureChange("EndWalk", true)` |

i.e. the reply names the posture being **left** and whether the weapon was
at high-ready in it (false only for Sprint). Implemented as a
Walk/Crouch/Prone/Sprint state machine on `ServerPlayerController`,
mirrored into the pawn's `bIsCrouched`/`bIsProne` (so other players see
it). The toggle reply is **bit-identical to retail frame 16526** (169 bits,
`rpc_decode_pcap_test`). Also recorded for later game logic:
`requested_team`, `class_role_index`, `ready`.

**Verified.** `rpc_decode_pcap_test`: 54,137/54,138 retail bunches,
threshold raised to 99.9%; toggle reply bit-exact. `fake_client.py`: A's
`ServerSetCrouch(true/false)` → A receives `EndWalk`, `EndCrouch`; B sees
A's pawn `bIsCrouched` 1 then 0.

**Not modeled:** sprint speed changes, prone/crouch collision, posture
rules (e.g. can't sprint while prone) — the server accepts whatever the
client requests. `AskForPawn`/`ServerAcknowledgePossession` are logged
but the server still spawns the pawn unconditionally at join rather than
following the retail team→class→ready→AskForPawn flow.

## 23. Stats: the server is the authority; model, retail-exact encoding, JSON export + web viewer (2026-10-01)

**Correction to sec 19's "the client pushes its stats to the server".**
`ServerStatsTransmittedByClient` / `ServerMatchStatsTransmittedByClient` /
`ClientTransmitLastRoundStats` all have **zero parameters**. The stat data
flows the other way: the server replicates `AA_MP_PlayerMatchStatistics` /
`AA_MP_PlayerStatistics` down on the StatManager channel, then calls
`ClientTransmitLastRoundStats` (RepIndex 25) so the client commits them
(to Steam); the `Server*Transmitted*` calls are just its acknowledgements.
So the server must compute stats itself — which is also what a stats
website needs (a single trusted source).

**What retail actually tracks** (`aa3-tools/map_manifest/stat_census.py`
on the 14-minute capture): pushes happen **once per round, at round end**,
each followed ~15s later by `ClientTransmitLastRoundStats`:
- t=370s (round 1 end): `iRoundsWon=1`, `iHighestMatchObjectiveScore=1`;
  round: `iM4A1ShotsFired=29`, `iObjectivesCompleted=1`,
  `iClassicExtraction=1`, `iES2ObjectsInRound=8`.
- t=806s (round 2 end): `iM4A1ShotsFired=180`, `iM67Thrown=2`,
  `iM83Thrown=1`, `bM106Thrown=2`, `iM84Thrown=1`, `iROEInstances=1`,
  `iROEReceivedInstances=1`, `iES2ObjectsReported=1`.
`fpStampLastUpdate` is a Unix time stored as a float.

**Implemented.**
- `tools/gen_player_stats.py` → `include/game/player_stats_gen.h`: both
  structs as typed C++ members named exactly as in UnrealScript (128
  fields) plus a `{name, kind, offset}` table. Its type sequence is
  identical to the hand-verified kind strings from sec 13 (checked).
- `ServerSteamStatManager` now holds the real `match`/`round` structs; the
  encoder walks the generated table and re-sends a struct whenever any of
  its fields change; the StatManager channel (Ch=3) is diffed on the
  replication tick.
- **Bit-exact vs retail:** loading retail's t=370s values into the model
  reproduces that 2191-bit round push exactly (`rep_roundtrip_test`).
- Server-observed stats on each PC (`observed`): time played, distance
  (sum of accepted ServerMove deltas, uu), moves, sprints, posture changes,
  stance toggles; plus team / class role / ready choices (sec 22).
- `game::StatsExporter`: writes `StatsFile` (default
  `stats/players.json`, relative to the working dir; `""` disables) every
  `StatsIntervalSeconds` (default 5) atomically (tmp + rename). Online
  players plus the last 200 departed ones (snapshotted on disconnect),
  each with identity, `observed`, and the full `match`/`round` blocks by
  field name.
- `tools/stats_web.py`: stdlib-only HTTP server — `/api/players.json`
  (the export) and an auto-refreshing table (time, team, shots/hits/kills/
  headshots summed across weapons, objectives, rounds won, distance,
  sprints). Read-only, so it can run on another host fed by a synced file.

**Verified end to end:** two scripted clients against the live server;
the export showed A's 3s walk at 300uu/s as `distance_uu: 897`, its 2
posture changes, both players `online:false` after timing out, all 128
AA3 fields present; the web server served the JSON and page.

**What is NOT there yet — and why the AA3 counters stay zero.** Every
AA3 counter (shots, hits, kills, headshots, grenades, objectives, ROE,
medic, hit locations) is driven by combat and game-mode events this server
doesn't simulate: weapons/inventory aren't replicated (the retail census
shows weapon, ammo and inventory actors on their own channels), there's no
hit detection, no objectives, and no round lifecycle — so there is also no
round end to trigger `ClientTransmitLastRoundStats`. The pipeline is
built so that work only has to increment named fields
(`stat_manager->round.iM4A1ShotsFired++`) for them to reach both the client
and the website.

## 24. In-game LAN browser: the server now lists in the real client (2026-10-01)

Verified end to end with the real AA3 3.3 client (Wine, see memory
`aa3-real-client-test-setup`): our server appears in Multiplayer > Local as
"AA3 Server · Bridge (night, clear) · 0/32 · co", and Join from the list
connects, completes the handshake and loads the map.

**This is the in-game browser, not AA3Loader.** Earlier notes
(aa3-backend/results/05) concluded LAN was a dead end — that was about
AA3Loader's 3.2-era lister. The 3.3 game client's own browser works, with the
ports the other way round (client log: "MBS LAN Server Listener port: 9001,
Client Listener port: 9000", version "3.3").

**Protocol, read off the running retail server** (no capture privileges, so
by acting as the client against it):
1. Client broadcasts `34983~1` to UDP 9001.
2. Server answers `34983~0~<ip>~<detailsPort>~3.3` to UDP 9000 (retail:
   details port 9002).
3. Client sends ACPU `get_server_state` (`playerName`) to the details port.
4. Server replies ACPU {`attributeNames`[36], `attributeValues`[36],
   `resultCode`=1}, zlib level 9 + SSC (body key). Retail fixture:
   `tests/fixtures/retail_lan_state_reply.bin`.

**ACPU header, from libaa3 acpu_decompile/construct_fragment_header:**
`u16 0x354a | u16 checksum | u8 2 | u8 flags (bit0 fragmented, bit1
compressed) | u16 message | u16 field count`. The second word is **not a
transaction id**: it's a per-fragment checksum (acpu_compile @0x630d113b) over
the encrypted payload — `h = 0x14fe` as two bytes, `h[i%2] ^= key30[i%30] ^
data[i] ^ data[i-1]` (key table @0x631d9c05 `"l26aquiwy814m05kpt37vxc9osrnf\0"`,
max 1010 bytes/fragment). Matches two real client requests and the retail
reply exactly. The client **silently drops replies with a bad checksum and
re-requests** (~13×) — which is how our first version (echoing the request's
word) failed. Found by A/B against retail's exact bytes served from Python:
verbatim → listed; any byte change in content or that word → rejected, while
identical content always carried an identical word.

Also observed (harmless): retail stores a trailing NUL on only the first 5
array elements; we NUL-terminate all, which the client accepts.

**Implemented:** `network/lan_beacon.{h,cpp}` (UDP 9001 queries → announce to
9000 as broadcast + unicast; UDP 9002 details with the 36 attributes in
retail's order; ACPU parse/build, SSC with an embedded keystream from
`tools/gen_ssc_keystream.py`, checksum). Engine config: `LanBeacon` (default
1), `LanIP` (default: primary IPv4), `MissionType/Size/TourID/Variant`;
player count is live. `tests/lan_beacon_test`: parses the retail reply, our
names/order identical to retail's, checksum vs all 3 real samples, our reply
round-trips with a valid checksum, announce format.

## 25. Reliability: retransmission, ack-every-packet, in-order delivery (2026-10-07)

Sec 21 listed "no reliable retransmission" as a known gap: one lost reliable
bunch stalled its channel on the client for good (the client holds every
later bunch waiting for the missing ChSequence, UChannel::ReceivedRawBunch).
On loopback that never showed; on a real network it would. The server now
implements the UE3 reliability model from `UnConn.cpp`/`UnChan.cpp`
(`UNetConnection::ReceivedPacket`, `SendAck`/`PurgeAcks`,
`UChannel::SendBunch`/`ReceivedNak`/`ReceivedRawBunch`).

**What retail does on the wire** (re-measured on GamePlayPCAP_Legit_Server):
- The client's Ch=0 reliable sequence is continuous from the handshake:
  HELLO = ChSeq 1 (bOpen), NETSPEED/login bundle = 2, the first HAVE = 3,
  and so on. Same against our server (LiveClientConnect.pcap).
- Every packet is acked, and every ack goes out twice: in the next packet
  and once more in the one after (`QueuedAcks` → `ResendAcks`). The retail
  server packs all owed acks into each ~30Hz packet, alongside bunches.
- The capture has real retransmissions: a duplicate client reliable bunch
  (frame 11252, ch 160 ChSeq 2 twice).

**Why acking only some packets was wrong.** The server used to ack only
packets that carried a reliable bunch, plus "the latest" on the 200ms
keepalive. UE3 treats every packet below the newest acked one that is still
unacked as lost ("virtual NAK" + the "missed NAK" sweep) and resends its
reliable bunches. Combined with no duplicate detection on our side, that
could replay a client RPC.

**Implemented** (`protocol_handler.{h,cpp}`):
- Every send goes through `TransmitPacket`/`SendBunch`. Each reliable bunch
  is kept as complete bits (header with ChSequence, plus payload) in
  `ClientSession::out_rec` until acked. A retransmission puts the same bits
  in a new packet.
- Acks are parsed from every incoming packet in every state. The handshake
  stages used to return early without reading them. Wrapped ids are
  unwrapped with UE3's `MakeRelative`.
- Resend rule: an unacked record whose packet is older than the newest
  acked packet. Fallback: past a deadline (1s, doubling to 8s), once the
  client has sent 3 more packets without acking it. This covers a lost
  CHALLENGE, where only the client's HELLO keeps coming. Silence never
  triggers a resend (the client may be loading the map). Requiring 3 packets
  avoids spurious resends from a client that sends before it has read its
  backlog.
- Every client packet in AUTHENTICATING/ESTABLISHED is queued for acking.
  Packets with a reliable bunch are acked immediately (as before). The rest
  are acked by the next outgoing packet or by Tick's ≤33ms ack flush. Each
  ack is sent twice, capped at 64 per packet.
- Incoming reliable bunches are deduped and ordered per channel. A
  ChSequence ≤ last processed is dropped (retransmission); one ahead of a
  gap is held (up to 128) and released once the gap fills. The Ch=0
  baseline is taken from the HELLO/bundle packets the handshake stages
  consume. Bunch handling moved into `DispatchBunch`; actor-bunch decode
  now runs on a reader bounded to the bunch's own bits.
- Backpressure: `ChannelReady` skips the reliable diff for a channel with
  ≥120 unacked bunches (RELIABLE_BUFFER 128). Without it, a long stall could
  push ChSequence past the client's ±512 window. The diff baseline isn't
  advanced, so the change goes out once the channel drains.
- Handshake packet shapes are unchanged: challenge without an ack, storm
  packets acking the client's last packet. Disconnect logs per-client counts
  (resent / unacked / duplicate / out-of-order).

**Test client** (`tests/fake_client.py`) now behaves like a UE3 client:
real HELLO/bundle packets on Ch=0 ChSeq 1/2, per-channel ChSequence (it
used to send every reliable bunch as ChSeq 1), acks every packet twice, and
processes reliable bunches in order with dedup. New modes: `--loss P` drops
incoming packets (unread, unacked), and PASS then also requires every
reliable gap to be filled. `--reorder` sends A's crouch RPCs as k+1, k, k.

**Verified** (two scripted clients, server on a spare port):

| scenario | result |
|---|---|
| no loss, ×3 | PASS, 0 resends; server acks ~99% of client packets (all but the last before exit) |
| 20% incoming loss | PASS: postures + remote crouch delivered, 0 unfilled gaps; 33 resends, 0 unacked at disconnect |
| 40% incoming loss | PASS: same; 96 resends |
| `--reorder` | PASS: server queued 1, dropped 1 duplicate; replies `EndWalk`, `EndCrouch` exactly once, in order |
| previous build, 20% loss | FAIL: Ch=0 stalls at the first lost storm packet (40+ unfilled gaps), PC channel stalls, no posture replies |
| previous build, `--reorder` | FAIL: RPCs applied in arrival order, one lost |

**Real client (2026-10-08), loopback, capture on `lo`.**
- First run: the client **resent its HELLO** (Ch=0 ChSeq 1) in its packet 2.
  The CHALLENGE carries no ack (same as retail), and our storm packets acked
  only client packet 1, so the client read packet 0 as lost (virtual NAK).
  The new dedup dropped the duplicate. Retail acks packet 0 in its first
  storm packet, so the handshake-stage packets now go into the normal ack
  queue: the first storm packet acks 0 and 1, the CHALLENGE still has no ack,
  and the `kLastPacket` ack mode is gone. Rerun: 0 duplicates on Ch=0.
- 3.3 client under Proton with the real GPU (on `:0`): handshake, 166 HAVE,
  JOIN, Ch 2–14 opened (including the second client's PRI/Pawn). Over 79s:
  the client acked all 911 server packets, the server acked all its packets
  except the one in flight; 0 gaps, 0 resends, 0 duplicates; clean timeout
  disconnect with 0 unacked. But the client only ever sent acks (5/s, one per
  server tick) and never the post-load RPCs. Its log stops after terrain
  creation during map load and no game window was mapped, so it never
  finished loading. Cause not established; nothing at the link level is
  wrong.
- Wine client in Xephyr (llvmpipe): handshake + map load, then the same
  render-thread GPF as on 2026-10-01 (`0x411903/0x4b159a/0xbf2f3f`) about 5s
  after JOIN. Environmental.

**Not done:** multiple bunches per packet (each bunch is still its own
packet, as before); incoming-packet loss stats; no test yet against the real
client under induced loss (the real client was only tested on loopback,
sec 24).

## 26. The real client gets in: PackageMap, PlayerController, AA3's spawn preamble (2026-10-08)

The real 3.3 client (Steam/Proton, GPU) loaded the map but sat on the
"Connecting" screen. It sends JOIN only after `LoadMap` succeeds
(UnGame.cpp:3442), so the load was fine. "Connecting" stays up until the
client accepts a PlayerController (UnGame.cpp:3581, via `HandleClientPlayer`
with NetPlayerIndex 0). Three server bugs stood in the way, found in order:

**1. Every ObjectIndex was wrong.** The client logged `Received invalid actor
class on channel 2..11`. `UPackageMap::Compute` (UnCoreNet.cpp:210) builds
each package's ObjectBase as the running sum of ObjectCount over the USES
list *as sent*. Our indices were copied from retail captures, but our USES
list differs from retail's in order (from entry 3) and in set. Fix:
- `aa3_build_manifests` writes each USES entry's `net_objects`. A package
  cooked into a BASEPKG uses the base package's forced-export entry
  (`FObjectExport::GenerationNetObjectCount`, now parsed instead of
  skipped); anything else uses its own file's generation table.
  `--dump-summary` and `--dump-exports` print both.
- `MapManifest::ObjectBase(pkg)`; `World::BindPackageMap` sets each actor's
  index as ObjectBase + the CDO's NetIndex. NetIndex is the export position
  in the cooked script package, verified for all 7 CDOs we use.
- Validated against retail: over retail's real 170-package USES order, the
  rule reproduces the live ground-truth table exactly (all 168 shared
  counts; AA3Baseline base 38361, AA3Game base 171302, so
  `Default__AGP_PlayerController` NetIndex = 1286). The old `wire_truth`
  extraction had found only 164 of retail's 170 USES. The builder's
  exclusion list dropped 5 packages retail does send (Editor_UI,
  Editor_AISupportIcons, MCEditorResources, TestSAI, SoundNodeVOIP_Test, all
  forced exports of AA3Baseline); removed. All 91 manifests regenerated.

**2. Two PlayerControllers, neither usable.** Ch=2 carried retail's
index 172588, labeled "WorldInfo" in world.cpp (left over from the old Ch=2
hypothesis) but actually `Default__AGP_PlayerController`, followed by
WorldInfo property bits. The client read those bits as Location and
NetPlayerIndex=206, which made it create a split-screen child connection.
Ch=11 then sent an `AA_PlayerController`, the base class without the AA3Game
script RPCs. Now: one `AGP_PlayerController` on Ch=2 (kPCMaxIndex =
`rep::AGP_PC::kMaxIndex` = 491; RepIndex values 0..472 are unchanged), then
StatManager 3, GRI 4, MOS 5, TeamInfo 6-9, PRI 10, own Pawn 11; remote
players from 12. No WorldInfo channel (placed level actor; retail doesn't
open one here).

**3. AA3 appends 7 bits to the spawn preamble.** With indices fixed, the
client spawned our PC but read NetPlayerIndex=46 and appErrorf'd ("Failed
to find LocalPlayer for received PlayerController"). AA3's
`UActorChannel::ReceivedBunch` (AA3Game.exe, string xref 0x9f4adf) reads,
after `Location.SerializeCompressed` (0x9eeaf0) and optional rotation
(0x9ee9f0): ReadBit (0xc80a90) ×2, then SerializeInt(32) (0xc80a60). That's
7 bits. The first bit selects an alternate SpawnActor call, the second is
an argument to it, and the int is stored at actor+0x4b8. Retail sends
`0,1,0` for nearly every transient actor, and `0,0,0` for
Pawn/InventoryManager/weapon/tracer/sight. We now send `0,1,0`. Every
transient actor we opened before this was misaligned by 7 bits.

**Result (real client, GPU):** `AGP_PlayerController_1 setplayer
LocalPlayer_0`. The client is in the map, can look around (spectator
camera, sending `ServerSetSpectatorLocation`), stayed connected 3 minutes,
exited cleanly (0 resends, 0 unacked). It cannot move and shows no menus.
Its log shows what's missing: `PlayerReplicationInfo` None at PC
PostBeginPlay, `Team` None (pawn), `PlayerProxy` None (retail opens an
`AA_PlayerStorageProxy`), `myHUD` None (no HUD, so no in-game menus).

**Retail, for comparison** (server→client on the PC channel): the
2367-bit PC open (ours is about 140); `ClientUpdateScoreboard`;
`ClientGotoState` ×8 at t≈6.9s; then about 15 seconds of client-side menu
flow (team/class/loadout) before inventory, `ClientAdjustPosition` and
`ClientAckGoodMove`, i.e. possession at t≈21.8s. Retail sends no
separate `ClientSetHUD`. UE3 packs RPCs called before a channel opens into
its open bunch, so the HUD setup and initial state likely ride in that
2367-bit bunch.

**Next:** decode retail's full PC open bunch (properties + RPCs); add the
PlayerStorageProxy; make sure PC.PlayerReplicationInfo/Team resolve; stop
spawning the Pawn at join and follow retail's
menu → ServerJoinTeam → … → AskForPawn → possession flow.

## 27. HUD, post-auth menu, team join; decode_rep.py (2026-10-08)

**`aa3-tools/map_manifest/decode_rep.py`** decodes any actor-channel bunch
against a class's replication layout, the way the client reads it:
properties (NetSerializeItem; static arrays carry a BYTE element index after
the RepIndex, UnChan.cpp:1512), RPC parameters (`--struct-layout`, sec 15),
enum widths (Names.Num() at Enum export body +20, sec 11.2), the channel-open
preamble including AA3's 7 bits (sec 26), and NetPlayerIndex. It decodes
retail's PC open (2367 bits), GRI open (3570) and PRI open (594) to the last
bit. `--cdo` handles captures whose PackageMap isn't retail's.

**Retail's PC open is mostly RPCs** (UE3 queues RPCs called before a
channel opens into the open bunch): Role, bNetOwner, bUseFlashSAI, then
`ClientSetHUD(AGP_HUD)`, `ClientCapBandwidth(10000)`,
`ClientUpdateLevelStreamingStatus` per sublevel (gametype level + pathing
loaded/visible, the rest off), `ClientFlushLevelStreaming`,
`ClientCapBandwidth`, `OpenPostAuthMenu()`, `ClientEnableTS3Voice(name)`. We
sent none of these: no HUD (`myHUD` None), so no menus. Now sent in the same
order, except ClientEnableTS3Voice (no TeamSpeak server). The gametype
level is `GameModeLevel` (ini), default `<Base>_inf_classic_large_dev`.

**`AA_PlayerStorageProxy`** (retail Ch=18; ours Ch=12, remote players from
13): Owner/RepController → PC, RepPRI → PRI, bNetOwner, bIsDead, bActive. It
is PC.PlayerProxy, which the client's use/weapon code reads. The PC also
references it and `aassmSteamStatManager`.

**Teams showed FULL** because GRI `aiEmptyTeamSlots` (free slots per team,
retail [6,6]) was never sent. Retail's GRI open also carries GameClass
(AGP_GameInfo), MPStateName 'PendingMatch', asTeamNames
['Black Team','Gold Team'], asMissionTypes/asMissionBriefings,
sMissionActiveSubLevel, sMissionType 'classic' / sMissionVariant 'dev'
(we sent 'AGP_Game_INF_Classic'), SpectatorMode, and the server option
bools. All added (retail values in server_actors.h). TeamInfo indices are
0, 1, 3, 4 with bRestrictFTRolesByTeamSize. The PRI gets PlayerID, the
soldier name from the client's LOGIN URL (`?Name=`), and retail's
qualification record (iAccessRights 5, tier levels 4, training flags).

**ServerJoinTeam(i)** now does what retail does about 20ms after the
request: PRI.Team → the TeamInfo's channel (+TeamID), a free slot taken (and
freed on leave/disconnect), then `TeamMessage(PRI, "joined Black Team")` +
`EnterWaitingForPlayers(false)` on the PC channel. Removed: the join-time
"You have joined the game." TeamMessage and "Connected to server"
ClientSetProgressMessage (retail sends neither).

**Real client (GPU):** HUD and post-auth menu shown; loadout, rail-mod and
ready calls arrive; both teams joinable; after Join it shows "Team
Selected / waiting for others" and stops. That's the next step.

**Retail's round start**, after the join, is the next work item:
t=6.9s ClientSetViewTarget(viewpoint), ClientEnterPrePlanningTool,
ClientGotoState('WaitingForOtherPlayers'), opens AA_ES2Manager + ~26
AA_MCClassRoleReplicationInfo + 6 AA_FireTeamInfo; t=7.15s GRI mission
metadata + MPStateName 'MissionPrePlanning_RoleSelection', role-selection
and deployment timers 10; t=10.9s 'MissionPrePlanning_DeploymentWarning' +
ClientDeploymentWarning(10); t=20.85s ClientClosePrePlanningTool; t=21.8s
bMatchHasBegun, inventory, ClientRestart, GivePawn(pawn),
ClientAdjustPosition, then move acks.

## 36. Round end, scoreboard, next round; the original UnrealScript source (2026-10-10)

> Sections 28-35 are cited by code comments (round phases, spawn, inventory,
> doors/ES2, collision, bullets, vault/ladder, incapacitation) but were never
> written up here; the commits e602c04bd / fa7a0c74c / 10a28c153 describe them.

### 36.1 The scripts' source is in the packages

The cooked script packages still carry every class's original `.uc` source as
`TextBuffer` exports (`ScriptText`, outer = the class): AA3Baseline 1049,
Engine 1234, AA3Game 43, GameFramework 9. Body after the properties and
NetIndex: `Pos:i32, Top:i32, FString Text`. Extracted to
`script_source/<Package>/<Class>.uc` (AA_GameInfo.uc is 8313 lines, with
comments). Unlike the UELib decompiles (corrupt function bodies) this is the
real game logic; only natives are missing (some carry their former script
body in a comment, e.g. `AA_MCMissionManager.CheckForWinningTeam`).

### 36.2 Retail ground truth

Three round ends in `GamePlayPCAP_Legit_Server` (t=81.83 a withdraw, 367.91
and 804.79 extractions) and one in the 2026-10-09 live capture (t=227.34, a
fall incapacitating the only soldier). Decoded with a per-connection
timeline (every bunch, classes from the channel opens).

**Rules (source):**
- `AA_GameInfo.MatchInProgress.Timer` (1 Hz) -> `CheckForWinningTeam` ->
  `AA_MCMissionManager.IsTeamStillAlive(t)`: a team is alive while one of
  its players has a pawn, isn't dead, waiting or incapacitated. A team that
  had living players this round (`bHadValidPlayersDuringRound`) and has
  none loses; neither team alive = tie (`AA_MCBothTeamsDiedSeqEvent`). A team
  empty from the start doesn't count -- so one player alone ends the round
  as a tie when downed (both retail single-player samples).
- `AA_ExtractionVolume.HandlePawnEntry`: an assault-team (`IsTeamAssault`,
  side-swap aware) pawn entering; with `iMinimumPlayerCount` > 0 that many
  team members inside -> `bObjectiveCompleted`, `iObjectiveCompletedTeam`,
  GRI `sWinningObjective = string(PreplanSymbol)` ('EPOC_Zulu') and
  `sWinningActionPlayer`. Immediate (not on the timer).
- Time limit: `AA_MCTimeLimitEndOfMatchSeqEvent` sets the win condition's
  `iTimeExpiredWinner = GetDefendingTeam()`.
- `AA_MCWinConditionsSeqAct`: `PlayVictorySound`/`PlayDefeatSound` =
  `ClientPlaySound(S_Music...Victory/Defeat)` per team (a tie: defeat for
  both), `bLastRoundTied`, `atsTeamScores[]` won/lost/tied.
- `MatchEnded.BeginState`: `GameHasEnded` per player, `SetTimer(2.0,
  'ResetLevel')`. `ResetLevel`: `iCurrentRound++`, `ClientDisplayScoreboard(
  iEndRoundAARDisplayTimeSeconds=10, round)`, then `ShowScoreboard` for
  `iScoreboardDisplayTimeSeconds + iEndRoundAARDisplayTimeSeconds` = 15 s.
  After `iMaxRoundsPerMatch` (6) the match ends; sides swap at
  `iAutoSwitchSidesRound` (4), announced at the end of round 3 ("Teams will
  swap sides the next round.").

**Wire (owner's connection):**

| t | what |
|---|---|
| 0 | if alive: `ClientPlaySound(stinger)`, `ClientSetViewTarget(own PC)`, `ClientUnpossess`, `TS3ClientChangeChannel`, `ClientSetCameraMode('FreeCam')`, `ClientSetSpectatorMode('FreeCam')`, `HideScoreboard(True)`, `ClientUpdateSpectatorViewType`, `ClientGameEnded(False)` (False even for the winners). Already dead: stinger + `ClientGameEnded` only. Pawn: PRI/Owner/Controller None, Role SimulatedProxy, bNetOwner False, LastKnownPRI; inventory bNetOwner False. PC: bCollideWorld True, Pawn None, Right/LeftSpectateInfoString, SpectatorMode 1 |
| +0.03 | GRI `bStopCountDown`, `bMatchIsOver`, `atsTeamScores[0..1]`, `bLastRoundTied` (+ `sWinningObjective`, `sWinningActionPlayer`); proxy `bIsDead`, `bIsSpectating`; `ClientUpdateScoreboard(False)`, `ClientGotoState('Spectating')` |
| +2.0 | `ClientReset`, `ClientSetCameraMode('FirstPerson')`, `ClientSetSpectatorMode('FirstPerson')`, `ClientSetViewTarget(<an AA_Viewpoint>)`; every pawn + inventory channel closes; doors `IncMatchReset`+1, `ReplicatedStateName 'Latched'`; objective marker reset; `ClientDisplayScoreboard(10, round)`; GRI `bMatchIsOver False`, `MPStateName 'ShowScoreboard'`, `iBattlePlanningTimeRemaining 30`, `iCurrentRound+1`; PC bCollideWorld False |
| +2.1 ... | `ClientGotoState('WaitingForMatchStart')` every 2 s until match start |
| +17 | role selection: `TS3ClientChangeChannel`, `ClientEnterPrePlanningTool(True, False, False)`; GRI `iRoleSelectionTime 1` (roles carry over; 10 again on the swap round) |
| +18 | `AA_SteamStatManager.ClientTransmitLastRoundStats()`, `ClientDeploymentWarning(10)` |
| +28 | `ClientClosePrePlanningTool`, `ClientSteamStatUpdateMissionStart(False)`; ES2 objects closed and a new set opened; spawn as in round 1 |

A death mid-round (t=81.83): inventory channels close at once (weapon,
magazine and sight drop as an `AA_DroppedPickup_*`, not modelled yet), then
`ClientPawnDied`, `ClientSetCameraMode('FirstPerson')`,
`TS3ClientChangeChannel`, `ClientSetCameraMode('FreeCam')`,
`ClientSetSpectatorMode('FreeCam')`, `HideScoreboard(True)`,
`ClientUpdateSpectatorViewType`; later `ClientGotoState('Spectating')`.

Object references: stingers = S_Music NetIndex 16 (defeat) / 17 (victory)
(retail <obj 164221/164222>), viewpoints = Bridge_night_clear NetIndex 85-94
(`AA_Viewpoint`). Retail's ObjectBases must be rebuilt from the same
capture's USES (`extract_round_actors.retail_bases`); the old
`retail_bases.json` in a scratchpad was from a different list.

### 36.3 USES: the gametype level's own packages

Retail's USES also list the running gametype level's dependencies with
`BASEPKG=<level>` -- on Bridge classic `ES2_Objects` and `S_Music` -- and
leave out the other gametype levels. Ours had neither, so nothing in those
packages had an ObjectIndex (the stingers; also every ES2 object's
`ES2ObjectMesh`, which went out as None). The server now appends the
level's `BASEPKG=<level>` entries from `config/maps/<level>.aa3.manifest.json`
at load; appended, so no other package's ObjectBase moves.

### 36.4 Implementation

- `aa3-tools/map_manifest/extract_mission_rules.py` -> `config/maps/<level>.rules.json`:
  extraction objectives (team, min players, PreplanSymbol, the volume's
  world box = brush Model bounds + Location; Bridge: x -7008..-6208, y
  2864..3216, z -32..224 -- retail's winning extractor stood at y=2866) and
  the base map's viewpoints.
- `World`: phases `kRoundOver` (MatchEnded, 2 s, MPStateName unchanged) and
  `kShowScoreboard` (15 s); `CheckRoundEnd` (extraction every tick; time
  limit and team death on the 1 Hz tick, after a 2 s spawn grace);
  `EndRound` (scores, objective, unpossess, spectate); `ResetLevel` (pawns
  `destroyed`, doors/windows/objective/ES2 back to round start). Side swap
  from round 4 flips the extracting team (so the defenders for the time
  limit) and sets GRI `bTeamSideSwappingRound`; roles and spawn points don't
  swap yet. After round 6 the match restarts on the same map (retail would
  travel to the next map). `MissionTimeLimit` (ini) overrides the 420 s.
- Protocol: per-phase RPCs above (`SendPhaseRPCs`), the owner death bundle
  (`SendDeathRPCs`), `RetireOwnPawn` (inventory + pawn channels),
  `CloseDataActor`. Other players' PRI channels now stay open while they
  have no soldier (retail keeps them for the scoreboard); their pawn
  channel opens/closes with the body.
- GRI per-second `ElapsedTime`/`RemainingTime` removed: they are
  `bNetInitial` only in `GameReplicationInfo` (retail never sends them after
  the open); the old "RemainingTime 0 -> bMatchIsOver" rule is gone.
- Encoders: GRI `atsTeamScores` (3 ints), `bLastRoundTied`,
  `sWinningObjective`, `sWinningActionPlayer`, `iBattlePlanningTimeRemaining`
  (on the open only when set -- the GRI open is near the 4096-bit limit);
  PC `bCollideWorld`, spectate strings, `SpectatorMode`; proxy
  `bIsSpectating`; owner pawn Role/bNetOwner drop on unpossess.

## 37. Weapons: every role's rifle, rail mods (sights), hand grenades (2026-10-11)

### 37.1 What there is to build

Every `wpnPrimaryOne` of the 286 role slots in the 18 baked retail maps is
one of four rifles -- M4A1, M16A4, M16DMR, M249 -- all already kitted
(sec 34). The rest of a soldier's weapons are the rail mods the role offers
and the five grenade types:

| rail (PRI `mriPrimaryWeaponOneMods`) | classes offered (AA3Baseline class export) |
|---|---|
| Main | `Sights_Iron_US_A3Handle` 35979 (always element 0), `CCO_US_M68comp4` 35972, `2x_ACOG_RCO` 35943, `35x_ACOG_RCO` 35952, `ACOG_RCO` 35933 |
| Right | `Lights_Incan_M95` 35843 |
| Bottom | `Weapon_XM320` 36153 (grenadiers) |

Grenades: `AA_ThrowWeapon_M67` (frag, EInventoryType 5), `M83` (smoke, 3),
`M84` (stun, 6), `M106` (smoke, 4), `AN_M14_TH3` (incendiary, 1).

### 37.2 Rail mods, retail ground truth (GamePlayPCAP_Legit_Server)

- Frame 204: role pick -> PRI `mriPrimaryWeaponOneMods` = element 0 of each
  rail list (Iron sights + M95 light).
- Client `ServerSetRailMod("PrimaryOne","Main",1)` -> frame 218 Main = CCO;
  `ServerSetRailMod("PrimaryOne","Right",-1)` -> frame 234 Right = None.
- Spawn (frame 366): one `AA_WeaponModSights_CCO_US_M68comp4` actor (Base
  and ParentWeapon = the rifle); the rifle's `sWeaponSightType` names it and
  `ModInventory` points at it. Mods chain through `AA_WeaponModBase.NextModPtr`
  (`AA_Weapon.AttachWeaponModToRail`); `ParentRail` is
  `eAttachmentRailType` (Main 0, Top 1, Bottom 2, Left 3, Right 4, Stock 5,
  RearSight 6: 3 bits). `AA_WeaponModBase` fields: Health 23, Offset 24,
  ParentRail 25, ParentWeapon 26, NextModPtr 27 (sights MaxIndex 30, M95 29,
  XM320 32).

### 37.3 Grenades, retail ground truth (third life, t=436-680)

Six throws (M83, M67, M84, M106, M106, M67). The M67 at t=444.92:

| t | client | server |
|---|---|---|
| 444.92 | IM `ServerSetCurrentWeapon(M67)` | pawn graph 4 {151->151}; PC `ClientSendHUDNotifyChangeInventoryCount(5, -1)` + `ClientAdvancedInventoryUpdateGrenadeCounts(5, False)` |
| +0.23 | | M67 `bHidden`/`bHiddenByInvManager` False; `ClientNotifyChangedWeapon(M67, rifle)`; graph {151->80}; `ActiveInventory` = M67 |
| +0.65 / +0.73 | | M67 `AttachedActor` = pawn; PRI `iFragGrenades` 2 -> 1 |
| 446.01 | M67 `ServerStartFire` | `bPulledPin`; graph {89->90} (underhand after `SwitchThrowType`: {154->133}) |
| 447.82 | `CookIt` | `bCooking`, `ClientStartCookingDelayedRestoreTimer()` |
| 449.25 | `ServerStopFire` | graph {91->91} (underhand {93->92}) |
| 449.74 | | graph {91->77}; projectile open: Location, Rotation, Velocity (Speed 1020.5 along the view), Instigator, `fExplodeTime` 3.1 (5 - 1.92 s cooked; left out when uncooked); a new M67 opens into `aatwPreloadedGrenades[0]`; the thrown one loses Owner/bNetOwner/AttachedActor; `ClientSwitchToBestWeapon(True)` |
| 450.68 | IM `ServerSetCurrentWeapon(rifle)` | graph {150->79}, `ActiveInventory` = rifle, `ClientAdvancedInventoryUpdateGrenadeCounts(5, True)`, `ClientNotifyChangedWeapon(rifle, M67)` |
| 452.80 | | projectile `Base`, `bCollideActors` False, `ClientHearSound` (the blast) |
| 455.70 | | projectile channel closed |

Every projectile closes exactly at its class `LifeSpan` (M67 6 s, M83 20,
M84 16, M106 17). `ClientSendHUDNotifyChangeInventoryCount` has a third,
optional `ClipIndex` (Send=0): 49 bits with NumClips -1. Class defaults read
from AA3Baseline.u: `fExplodeTime` 5 (base; M67, AN-M14), 1 (M18 -> M83,
M106), 2 (M84); M67 Damage 100 with `DamageLookupTable` {0: 100%, 262.5:
100%, 350: 66.7%, 437.5: 33.7%, 525: 0} and damage classes
`Explosion_Grenade_Death/Red/Yellow/Green`; AN-M14 Damage 10 within 175,
BurnDuration 20 s, `AA_DamageType_Fire`; smoke and the M84 do no damage
(their effects are the clients', from the replicated fuse).

### 37.4 Implementation

- `src/game/weapons.cpp`: role kits (moved from world.cpp), `BuildRailMods`
  (one mod actor per filled PRI rail, cloned from retail's sight actor;
  the role's own default sight only if the PRI has no rails at all),
  `SetRailMod` (index into the role's list for that rail, -1 = None),
  PRI grenade counts from the role at spawn, `TakeGrenade` /
  `PutAwayGrenade` / `GrenadeAction` / `ThrowGrenade` with the timeline
  above, `ReplaceGrenade` (a fresh copy of the spawn template's grenade),
  projectile flight (world gravity; `AA_ThrowProjectile.Bounce`: velocity x
  restitution then reflected; rests under `MinRollVelocity` 10), the M67
  blast (lookup-table damage to soldiers in line of sight) and the AN-M14
  burn (once a second). Bullets and blasts share `World::ApplyDamage`.
- Protocol: IM `ServerSetCurrentWeapon` routes grenades in/out of the hand;
  AA_ThrowWeapon RPCs 31/32/59/62 (60/65 ignored); PC `ServerSetRailMod`;
  the tick opens new inventory to its owner and thrown grenades to every
  player in the round, and closes them when gone.
- PRI: `iFragGrenades`, `iSmokeGrenades`, `iM106SmokeGrenades`,
  `iStunGrenades`, `iIncendiaryGrenades` (sent when changed; not on the open
  when 0, the class default).
- `tests/grenade_client.py`: two scripted players; rail pick -> 2x ACOG,
  default iron + M95 for the other; M67 out, pin, cook, throw, back to the
  rifle; projectile seen by both and closed at its LifeSpan. PASS.

**Not modelled / guesses:** the XM320 is attached (visible) but has no
40 mm rounds, pouches or firing -- no retail grenadier sample; the
restitution (0.4) and the throw origin (31 units up, 25 forward) are
guesses; the blast's `Base`/`ClientHearSound` aren't sent; M67 shrapnel
traces and the prone "ultimate sacrifice" rule aren't modelled; retail's
second replacement grenade ~1.47 s after each throw isn't repeated;
`CurrentWeight` isn't updated; the "Frag out!" TeamMessage isn't sent.

### 37.5 Fixes after the first real-client test (2026-10-11)

- **M249 shots didn't hit.** The server put every rifle on semi after the
  initial reload and counted every magazine as 30 rounds. The M249's
  class defaults are `CurrentROF = ROF_Auto`, `bOpenBoltFire`, and its box
  holds 200 (`AA_AmmoContainer_200x556mmBall_Box.RoundCount`): a held
  trigger got one server trace, and after 30 rounds the server emptied the
  gun (`ChamberedRound` None, bolt locked) and stopped tracing. Kits now
  carry the rate they come off Safe to and the magazine's RoundCount
  (`ServerPawn::mag_rounds`).
- **Cooking (dropping the spoon) broke weapon switching.** Retail's server
  runs the cook to its end even after the throw: `bCooking` False on the
  cooked grenade at cook + fuse (t=452.80), then CookingDelayedRestoreTimer
  (fuse + 0.25) -> RequestWeaponRestoreTimer (+0.9) -> PrevWeapon:
  `InventoryManager.SetCurrentWeapon(rifle)` (client RPC 24) at cook +
  6.15 s (t=453.94). Without it the client's own restore timer picks its
  "previous weapon" -- which can be the grenade. Now modelled; `CookIt` is
  accepted whenever a grenade is in hand (it's a global function in
  `AA_ThrowWeapon`); a grenade held past its fuse sends
  `ClientHolderDied()` (64) and goes off in the hand.

## 38. Friend/enemy look: each side sees the other as the enemy force (2026-10-11)

AA3 draws every soldier of the viewer's own team as a US soldier and every
other soldier as the enemy force (OPFOR body, Obran weapons, U-series
grenades) -- the `AA_SwapInfo_*` classes pair each US item with its enemy
look. The choice is the client's, from team marks the server replicates:

- `Actor.iOriginalOwningTeamIndex` (Engine Actor.uc: `if (bNetInitial &&
  Role == ROLE_Authority) iOriginalOwningTeamIndex,
  namePawnInventoryAssociation` -- *every* connection; Epic's comment:
  "team index of original owner at spawn time and other swapping
  support"). Default -1 (not owned).
- `AA_Pawn.iBodyTeamIndex` (the body's team after death, when the pawn has
  no PRI).

Retail sets both on spawn (`AA_MCMissionManager`: `Pawn.iOriginalOwningTeamIndex
= PRI.Team.TeamIndex`, `aap.iBodyTeamIndex = aapri.Team.TeamIndex`), and
spawns the rifle with the owner's TeamIndex.

The server sent `iOriginalOwningTeamIndex` only to the owner, `iBodyTeamIndex`
as 0 for everyone, and the kit's items with retail's Black value 0: the
other side reached a Black client as unowned/team 0 and was drawn as US
soldiers (live client 2026-10-11). Now the pawn's team goes to every
connection on the open, `iBodyTeamIndex` is the team, and the kit's
`iOriginalOwningTeamIndex` is the owner's team. Verified on the wire with
one bot per team: each sees the other's pawn with the other team's index.

**Not done yet:** other players' weapons -- their inventory actors are
opened only to their owner, so the third-person rifle (and its Obran look
for enemies) isn't replicated to other players.

## 39. IFAK treatment: retail ground truth (2026-10-11)

Captured on the retail AA3Game server (LAN, PunkBuster off) with the real
client treating a scripted teammate bot. Capture and decodes:
`../captures/retail_ifak_2026-10-11.pcapng`,
`../captures/ifak_decoded_2026-10-11/` (medic session 21:37-21:42; relative
times below are from 21:37:00). Times t= are the decoder's.

### 39.1 Medic -> server (PC channel)

Each treatment attempt is one bunch:
`ServerStartTreatment(sVictimSoldierName)` (213), `ServerStartPassiveTreatment()`
(216), `SendCommoRequest(CommoType 42)` (AGP 485, the "Stop..." medic call),
`ServerUse()` (76). It ends with `ServerStopTreatment()` (214) +
`ServerWeaponRaise(bRaise=True)` (168). A symptom-based ("choice")
treatment: `ServerStartTreatment` + `ServerUse`, then
`ServerGiveActiveTreatment(TreatmentType='Saline', sVictimSoldierName)` (219),
then `ServerStopTreatment` (t=199.63-204.78).

### 39.2 Server -> medic (PC channel)

Start: `ClientPlayShoutedVoice(sndVoice, pawnShouter = medic)`,
`SetTreatmentOnRequestingClient(newPatient = patient's pawn)` (215),
`NotifyWeaponReadyChange(False)`, `NotifyTacticalCombatPostureChange(True)`,
`ClientReceiveRequestFriendly(42, bWhisper, PlayerIndex, Speaker = medic)`,
`TeamMessage(PRI, 'Stop...', 'TeamSay')`. Stop:
`ClientPlayShoutedVoice(..., pawnShouter = patient)`,
`SetTreatmentOnRequestingClient()` (no patient), `NotifyWeaponReadyChange(True)`.
A completed treatment scores `ClientNotifyScoringEventDetails(16, 8, +20)`.

### 39.3 Patient pawn (as every client sees it)

Wounded (t=7.87): `HealthTarget` 64.96, `bHasLowerBodyInjury`,
`ReplicatedInjuryVariables[n]` {BleedRate 1.0, WoundTreatmentProgress[0..] 0},
`RepNotifyInjuryVariables[n]` {WoundStatus[0] = {AA_Wound_Gunshot, step 0,
eTreatmentLevel[0]}}, `bIsBleeding`; Health then drains ~1.2/s toward
HealthTarget.

Passive treatment (t=14.83-16.87, 2.04 s): at once `CurrentTreatmentTreater[0]`
= medic, BleedRate 0, `bIsBleeding` False, `HealthTarget` 64.96 -> 76.6; then
`WoundTreatmentProgress[0]` +1/60 every ~33 ms to 1.0; then
`bHasLowerBodyInjury` False, WoundStatus[0] level 0 -> 1, treater None.
Stopping early (t=57.97-58.10, 63.84-65.14) leaves the progress where it got
to (0.0333, 0.6834) and the wound untreated.

Incapacitation from bleeding (t=178.3-180.6): HealthTarget 6.56,
`bPlayingInjuredAnims`, `IncapacitationType = AA_DamageType_Bleeding`,
`bProxyInCrouchState`, then `bRagdollTick` + `bRagdollStuck`.

Not yet worked through: the Saline active treatment's effect on the
patient (t=199-205), the injury-array index n (12, 13, 16 over the
session), and the patient's own-channel view.

### 39.4 Revive, patient side (bot session, t=204.78)

Patient's own pawn: `ClientRecapacitate()` (110), Health 25, RepGraphStates[0]
{134}, [2] {74 -> 1}, IncapacitationType None, HealthTarget 12.875 (25 minus
the bleed still owed, 18.73 - 6.59), bRecapacitated, bResetWeaponIK;
GroundSpeed 124 at +0.26 s. Patient's PC: ClientSwitchToBestWeapon x5,
`ClientNotifyDiscardInventory(7)` (the patient's own IFAK is used first),
`ClientNotifyIncapacitated(False, instigator)`, ClientSetCameraMode
('FirstPerson'), NotifyPostureChange('EndCrouch'), a +12 assist score.

### 39.5 Implementation

- `src/game/medic.cpp`: wounds per eHitLocation (head 1 = no wound, torso 3,
  upper/lower right leg 12/13), a bullet hit's total (torso 40, legs 35)
  split ~43% at once / ~57% bled out at 1.2 Health/s to HealthTarget;
  bleeding to 25 incapacitates (AA_DamageType_Bleeding). Treatment:
  StartTreatment (same team, 250 units, by soldier name), passive treatment
  (bleeding off, HealthTarget = Health, most critical wound +0.5/s, treated
  at 1.0), StopTreatment, GiveActiveTreatment (downed patient + an IFAK,
  the patient's first -> Recapacitate as 39.4). Treatment links end on death.
- Protocol: ServerStart/StopTreatment, ServerStartPassiveTreatment,
  ServerGiveActiveTreatment, ServerWeaponRaise, SendCommoRequest decoded;
  SetTreatmentOnRequestingClient + weapon ready/tactical notices to the
  medic; the revive's ClientRecapacitate + PC notices to the patient; a used
  IFAK's channel closes.
- Pawn encoder: CurrentTreatmentTreater[0], ReplicatedInjuryVariables[l],
  RepNotifyInjuryVariables[l] (None WoundType as the 12-bit dynamic None,
  as retail), bHasLower/UpperBodyInjury, bRecapacitated, bResetWeaponIK to
  every connection; Health/HealthTarget to non-owners after the open.
- `tests/medic_client.py`: leg hit -> treat -> head shot -> Saline revive.
  PASS; the patient's fields decode cleanly with aa3-tools' decoder.

Not modelled: active treatments on a conscious patient (Saline etc. only
revive here), multiple simultaneous treaters, scoring events, medic voice
(ClientPlayShoutedVoice, the "Stop..." commo reply), the retail 33 ms
progress cadence (ours follows the replication tick), arm/upper-torso
locations beyond the three zones.

### 39.6 Test bots on a retail server

`tests/bot_client.py --retail NAME` logs in like the real client (HELLO ->
CHALLENGE; NETSPEED + LOGIN RESPONSE=0 URL=...?Name=; HAVE per USES; JOIN)
and plays round after round: ServerUpdateLevelVisibility for every
ClientUpdateLevelStreamingStatus (without it the server drops the client at
the next round's re-stream), role + ready at each ClientEnterPrePlanningTool,
AskForPawn until Controller.Pawn names one, ServerAcknowledgePossession, the
rifle drawn (retail's TakeDamage ignores a pawn whose first reload hasn't
finished), and `--wander` to avoid the idle kill. Retail LAN server notes:
`pb/pbsvgame.cfg` sv_punkbuster 0 disables PunkBuster; AA3Game.ini
fpMaxROEAutoKick=0, bKickIdlePlayers=false, bAutoBalanceTeamsSizes=false --
edited only while the server is down (it rewrites the ini on exit); the
early-round ROE kick (15-25 s, same-team damage) is hard-coded; logins from
one address under 10 s apart are banned as a DoS for 120 s.

### 39.7 The client's treat prompt (2026-10-12)

The "Treat Soldier" use prompt is client-side: the native context trace
(CAT_TREAT_SOLDIER) and `CanTreat_Pawn` decide it from replicated state.
On our server the patient's wound fields reached the medic intact (the
gunshot WoundType decodes as `FieldName` with the retail resolver only
because two `FieldName` objects sit 6 apart), yet no prompt appeared. Two
medic-side differences from retail (`retail_ifak_2026-10-11.pcapng`, the
medic's connection):

- Retail opens the player's whole MOS set together, each with the same
  empty 50-bit payload as BCT: DRMAIT (18079), GrenadierAIT (18091),
  BCT (18071), **CombatMedic** (18032), ARAIT (18044), RiflemanAIT (18099)
  -- LAN servers authorize every qualification. We opened BCT only.
  `PRI.MedicQualification` isn't replicated ("set in native code").
- Retail's PRI open carries `bHasReviveKit = True` (186), which
  `AA_InventoryContainer_Pouch_IFAK` keeps in step with the IFAK pouch.

Both are now sent: `ServerPlayerController::extra_mos_quals` (opened on
free channels after Ch=5) and `PRI::has_revive_kit` (set from the kit,
cleared when the last IFAK is used).

## 40. Co-op AI missions: Bridge AI (2026-10-12)

Retail server run as `Bridge_night_clear?MissionType=co?MissionVariant=ai`
(sub-level `Bridge_inf_co_large_ai`), one real client plus bots. Capture,
decodes and notes: `../captures/retail_bridge_ai_2026-10-12.pcapng`,
`../captures/bridge_ai_2026-10-12/` (`FINDINGS.md`, `timeline.txt`).
Our server runs it with `-m Bridge_inf_co_large_ai` (a bundle named after
the mission, header map `Bridge_night_clear`).

### 40.1 Ladders from the top

Mounting a ladder from above starts with the same move as a vault:
`AA_Pawn.ServerStartMoveToTarget(TargetLocation, TargetRotation,
FinishMoveCallback, fMinDistanceToTarget)` (pawn 97). Wire layout: Send +
compressed vector; Send + rotator (per component a bit + byte); Send +
delegate = object (bDynamic + channel, here the pawn) + the function's FName
(FString + INT number); Send=0 for the float. The callback names the move:
`FinishLadderMoveToTarget` (a ladder) or a vault's. Retail, 13:37:53:
`bAdvMvmt_MovingToTarget` True, 0.14 s later `ClientFinishLadderMoveToTarget()`
(pawn 102) and False; the client then sends `ServerClimbLadder(L)` ->
ClientSlingWeapon, gravity off, graph 2 {174}, graph 3 **{23}** at +0.47 s;
at the bottom `ServerEndClimbLadder` -> graph 2 {7}, graph 3 **{22 -> 0}**
(from below: {117}, top {11 -> 0}). Our server answered every move with
`ClientFinishVaultMantleMoveToTarget` (100) and the vault states, which the
client took for a vault: the "can't climb down" bug.

### 40.2 The carried objective (briefcase)

- Join: `AA_Objective_Briefcase_US` (CDO 26202, MaxIndex 29) at the factory
  (AttachInfo {-1, 'SpineBSocket'}); the placed `AA_PickupFactory_Objective`
  (Location, bBlockActors, Inv = the briefcase); `AA_MCCarryableObjectiveMarker`
  (CDO 15660, MaxIndex 35: MyLocation, PreplanSymbol (2 bits),
  aLinkedObjectiveItems[0] = MyFactory = the factory).
- Pickup: `ServerRepExec('Pickup', ContextActor = factory)` (PC 189) +
  `ServerUse`. The briefcase channel closes and a fresh one opens. To the
  carrier: Owner, Instigator, bNetOwner, iOriginalOwningTeamIndex,
  InvManager, AttachInfo, `ClientGivenTo(pawn)`, then AttachedActor. To
  others: Instigator, namePawnInventoryAssociation, team, AttachInfo,
  AttachedActor. Factory bCollideActors/bBlockActors False, Inv None; marker
  bHideSAIIcon. Carrier: `SetNotificationMessage('Your team has picked up the
  objective!')` + `ReceiveLocalizedMessage(Engine.LocalMessage, , , ,
  class AA_Objective_Briefcase_US)`.
- Carrier killed: briefcase Owner/Instigator/AttachedActor None, bNetOwner
  False; `AA_DroppedObjective` (CDO 7016, MaxIndex 34) opens at the body:
  Rotation, Velocity (0,0,-70), Physics Falling (4 bits), then None,
  ReplicatedCollisionType 3 (3 bits), bBlockActors, InventoryClass, Mesh
  (retail's bits `1` + 2047), MyObjectiveMarker, aapfParentFactory. Carrier:
  `ClientSendObjectiveDroppedNotification` + `ClientNotifyDiscardInventory(22)`.
  It can't be dropped by choice. Players report that a downed carrier can
  hand it to a teammate; that isn't in the capture.
- Win: the carrier in its team's extraction zone. Which team extracts where
  comes from the Kismet objective variables
  (`AA_MCObjectiveLocationSeqVar_Object.oObjectiveData.EXT_iRequiredTeamOverride`),
  not the marker's iExtractingTeam: Black -> Zulu, Gold -> X-ray. Retail
  replicates iExtractingTeam as is, plus iRequiredTeamOverride when nonzero.
- Time limit: the human team lost (`ClientGameEnded(bIsWinner False)`, the
  defeat stinger).

### 40.3 Reinforcements (F5)

`ServerRequestRespawn()` (PC 252) while dead or spectating, only where the
level has `AA_MCPlayerRespawnMarker`s (12 per team on Bridge AI). The player
is queued ("You are now waiting for your reserve fireteam ...", "Stand by
for deployment ...", "from readily available stores."). Every 10 s each
team's queue deploys, up to 4, at an enabled marker of the team unused for
15 s ("You have just hit the LZ with your reserve team"). Then the usual
spawn sequence (ClientRestart, HUD inventory, DisplayMissionBriefing,
ClientMatchStarting, Pawn = the new pawn) and two more messages. The round
doesn't end when a team is all down: players can still come back.

### 40.4 Enemy NPCs

35 spawn at match start: up to the level's cap, and no more than 46 minus
the players. They spawn at the pathing level's 40
`AA_NPCEnemyPlayerStart`s (aabwWeaponClass: ObranKNP by default, else
ObranMSP / ObranLMK). On the wire:
- **Pawn only.** Only the `AA_NPCEnemyPawn` (CDO 21100, MaxIndex 139 =
  AA_Pawn's 131 + AA_NPCPawn's) is sent, with no controller and no PRI. Its
  open has namePawnInventoryAssociation ('AA_NPCEnemyPawn_38'_96),
  iBodyTeamIndex 4, sBodySoldierName = `sNPCName` (131), and no
  iOriginalOwningTeamIndex.
- **Gear opened with it.** Its gear opens with it: ObranKNP (bHidden False,
  Instigator, AttachedActor, CurrentAmmoType = ChamberedRound[0] =
  AA_AmmoType_556mmBall, LastInventoryContainer = chest slot,
  LoadedMagazine[0]), a magazine (`AA_AmmoContainer_NME_30x556mmBall`,
  RoundIndex 1, attached to the rifle), a dump pouch ('DumpPouchSocket'), and
  the chest and right-shoulder weapon slots. The pawn's
  DumpPouch / NextMagPouch / NextStorageMagPouch / ActiveInventory go to
  everyone.
- **Updates.** Movement, posture graphs and FlashCount (shots) update the
  NPC. Damage replicates exactly as a player's does (Health, HealthTarget,
  injuries, PawnShotEffectInfo).
- **Neutralizing one.** The killer gets
  `ClientNotifyNeutralizedNME(name, False)` +
  `ClientNotifyScoringEventDetails(7, , , 32, +10)`. Players report bodies
  with `SendEnemyDownRequest(name, interactor)` + ServerUse.
- **Relevancy.** Retail opens and closes NPCs by relevancy (249 opens in
  22 minutes); ours opens every live one.

Our AI (`src/game/npc.cpp`) is a first cut. It holds the start, turns to the
nearest player soldier in line of sight within 5000 units, and fires
3-round bursts after a 0.8-1.6 s reaction, with distance-scaled spread. When
hit, it turns toward the shooter. It doesn't yet move between the 119 firing
positions (baked), patrol, take cover or use grenades.

### 40.5 Channel reuse and reliable sequences

UE3 keeps `InReliable`/`OutReliable` per channel index for the whole
connection, so a reopened channel continues its numbering. Retail closed the
briefcase's ch 15 at R2 and reopened it at R3. Our server had restarted a
reopened channel at sequence 1, which a real client drops as an old bunch
(this affected the own pawn's ch 11 after a respawn). It now continues the
sequence, and reuses a closed channel index 5 s after the close. Before
this, every round's NPC channels would have run out the 2048 indexes in
about nine rounds.

### 40.6 A bot through the whole mission, retail and ours (2026-10-12)

`tests/mission_bot.py` plays Bridge AI start to finish on either server
(`--retail NAME` for a retail AA3Game server). It joins Black, draws its
rifle, walks to Yankee, picks up the case, sprints it to its team's zone,
fights NPCs on the way, and after a death withdraws and presses F5. It walks
on routes from `aa3_navgrid`, a walkable grid built from the bundle's
collision (`src/game/navgrid.cpp`). Retail's server log confirmed
"Extraction Entered by MissionBotB" twice; on ours, "Round 2 over
(extraction): team 0 wins". What the runs showed about retail:

- **Movement input.** The client sends `ServerMove.InAccel` as the
  acceleration times 10, magnitude 7680 while walking; a scripted move with
  a small InAccel barely moves the pawn. Walking speed is 200, sprinting 316
  (`ServerHoldSprint`).
- **Rounds end on team death.** The round ends as soon as Black has nobody
  up, even with respawn points; F5 helps only while a teammate lives.
  `ClientGameEnded(bIsWinner)` was False for the extractor on a win.
- **Spawn protection.** A soldier can't be hurt until his drawn rifle's
  initial reload ends (`bFirstReloadFinished`). Ours now does the same.
- **Sights and the tactical stance.** Raising the sights puts the soldier in
  the tactical stance, and it outlasts the sights: lowering them goes to
  weapon graph {22}, with NotifyWeaponReadyChange(True) and
  NotifyTacticalCombatPostureChange(True). Ours went to the relaxed {11}, and
  the real client then kept the ACOG's focus blur.
- **Flood protection.** Logging in twice from one IP within 10 s gets
  banned ("Invalid JOIN DoS").

Map collision bugs found because the bot got stuck where retail let it pass,
or passed where retail stopped it:

- **Prefab templates.** They were taken as placed actors at the origin; a
  sun lens-flare template made a 14,400-unit wall through Bridge. Only
  actors whose outer is a Level are collision now (16 of the 19 bundles
  changed).
- **Blocking volumes.** `BlockingVolume` and `AA_PlayerBlockingVolume`
  brushes are now soldier-only collision (kBlockPawn): they stop soldiers,
  not bullets.
- **Grid details.** Buildings have more than 4 floors (the grid keeps 12).
  Crouch-height passages exist. A soldier needs about 26 units of clearance
  around him.

NPC tuning against the retail run: retail's first NPC fired 23 rounds in
160 s. Ours now react in 1.5-3 s, see 3000 units within ~75 degrees of where
they face (all round within 600), fire 3-round bursts with 2-4 s pauses and
about 6 degrees of spread, and spawn on the floor under their marker.

### 40.7 NPC AI, relevancy, dropped rifles, hand-off, countdown, movement checks (2026-10-12)

- **NPC movement** (`npc.cpp`). The server builds `NavGrid` (48-unit cells,
  over the NPCs' area plus 1500) in a background thread from a copy of the
  collision, about 25 s on Bridge; NPCs hold position until it's ready.
  - Patrols: an idle NPC walks (170) every 25-50 s to a firing position
    within 1200 of its start.
  - Contact: on spotting a soldier it runs (300) to the nearest firing
    position within 700 that sees him, and doesn't fire on the move.
  - Fighting: from a stand it crouches, and the pawn drops 14 so it matches
    the crouched hit cylinder.
  - Grenades: 4-15 s after losing a soldier 400-1500 away, it lobs a U23A1
    frag (`AA_ThrowProjectile_U23A1`, retail's NPC grenade) on the arc that
    reaches his last position, at most every 25-45 s.
- **NPC relevancy.** An NPC opens to a player within 5000 units of his
  soldier and closes past 6000; a player with no soldier uses his last
  location.
- **Dropped rifles.** A killed NPC drops `AA_DroppedPickup_Obran{KNP,MSP,LMK}`
  (CDO 7130/7134/7132, MaxIndex 30), as retail's frame 12295 shows. It opens
  falling, with Rotation, Velocity, ReplicatedCollisionType 3 (4 bits; see 40.8), bBlockActors,
  InventoryClass, Mesh (retail's bits), InventoryTeam (TeamInfo 4) and
  `LoadedMagazines[0]` = {ammo class, 30 (LMK: 200), round index}. After
  0.12 s it rests on the floor (Physics None), and it closes after 120 s
  (`fpMaxNPCDroppedWeaponLifeTimeSeconds`). Picking a dropped rifle up isn't
  modelled.
- **Hand-off** (AA_PlayerController.SafeDropObjective). A carrier who goes
  down gets `ClientNotifyIncapacitatedWithObjective()` (PC 240); his
  `ServerDropObjective(bDebugDropping)` (PC 239) drops the case 50 units in
  front of him, and a teammate picks it up from the ground.
- **Reinforcement countdown.** GRI `fpRespawnTimeSeconds` (78) counts down
  to the next wave by the second; `iRespawnPlayersNeededTeam0/1` is the
  minimum players to deploy less those queued (retail's formula).
- **Movement checks** (INI `MovementChecks`, default on). A ServerMove's
  ClientLoc is refused when it is:
  - from a downed or dead soldier;
  - faster than 650 units/s plus 60;
  - through static `kBlockPawn` geometry at both knee and chest height,
    except during a vault or move-to-target or on a ladder.

  A refused move gets an unreliable `ClientAdjustPosition` back to the last
  good spot, at most every 0.2 s. Scripted protocol tests that place their
  soldiers by teleporting set `MovementChecks = 0`.

### 40.8 Retail's enemy AI, downed enemies, mid-round roles (2026-10-12)

**Client crash on killing an NPC.** The dropped rifle (40.7) and the dropped
briefcase sent `Actor.ReplicatedCollisionType` in 3 bits; `ECollisionType`
is 4 (retail frame 12295: 146 -> 155 is a 5-bit header plus 4). Every field
after it was shifted one bit, so the client read `bBlockActors` as
`DrawScale` and filled `LoadedMagazines` with huge values. The client logs
showed a General Protection Fault (20:57) and "Ran out of virtual memory"
(21:02), each right after the kill was scored. Both drops now decode bit
for bit like retail's. A kill's notice now matches retail's order too:
`ClientNotifyNeutralizedNME` first, `ClientNotifyScoringEventDetails` in a
bunch of its own 0.2 s later. On death, the NPC's weapon slots and dump
pouch close, its body stays 60 s and its rifle drops.

**Retail's enemy AI** (`AA_NPCBaseSoldierController` /
`AA_NPCBaseSoldierBaseThinking`; values from the package defaults).

What the map gives each NPC:
- *Start.* Every Bridge AI start has `bManFiringPositionsOnly` and its own
  `aanpcfpFiringPositions` (NavReference array: Actor + Guid, 20 bytes
  each). Retail then allows only these states:
  - ScanForTargets;
  - ChangeFiringPosition, among its own positions;
  - TargetingNewEnemy and HidingInCover;
  - ReloadingWeapon and WeaponJam;
  - the grenade throws, Flashbanged, incapacitated, dead.

  Patrols, running to cover, chasing and securing are refused. Starts
  without it (Airfield) choose the nearest firing position near home.
- *Firing positions.* Each `AA_NPCFiringLocationPathNode` has its yaw,
  `bCanShootStanding/Crouched/Prone`, `bCanThrowGrenades` and 16 cover
  directions (one per 4096 of yaw). The baker now extracts these, with the
  starts' `iMaxSightDistance`, `bSpawnRandomPatrolLocation`, temperament and
  the map's `WorldInfo.fpAIFogDistance` (Bridge night 2500, RanchAI 3200).

Senses:
- *Sight.* Range is 19200; 20% of NPCs see 60% of that and 40% see 80%.
  Nothing past the fog. Each look uses one of four peripheral cones
  (cos 0.78 / 0.698 / 0.5235 / 0.349). Retail's posture and range rules hide
  crouched or prone soldiers who keep still far away. Smoke clouds (M83,
  M106, U72; radius 300 for 45 s) block sight.
- *Hearing.* A player's rifle is heard within 18000 (`iMaxHearDistance` 450
  × 40). An NPC that isn't fighting turns toward the shot and scans.

Targets, contact and fire:
- *Choosing a target.* `EvaluateNewPawn`, including retail's distance term,
  which grows with distance.
- *Contact.* `AA_NPCCombatManager.NotifyEnemyContact` gives an order to each
  NPC within 12000 with health ≥ 80 and no order in 30 s. It attacks if it
  sees the enemy; otherwise it heads there, which positions-only NPCs
  refuse.
- *Firing.* Rifles fire semi-auto bursts of 1-3 rounds (the MSP fires 1),
  0.1 s apart, with 0.25-0.5 s after each burst. Then 50% crouch, and if the
  enemy is out of sight they wait 3-13 s; otherwise 0.25-1.25 s. The LMK
  fires auto for 0.15-1.15 s, then pauses 1-1.75 s. Retail's capture
  matches: bursts 0.1 s apart, 0.8-2.4 s between bursts.
- *Aim.* The target's centre ± 15 across and ± 25 up and down (MSP 10/15),
  + 5 by `IsAimingAffectedBySuppression`. Then our dispersion, standing in
  for the rifle's spread: about 5° (wider far off and against moving
  targets). It's tuned so the mission bot lives about as long per spawn as
  on retail, ~110 s.
- *Losing sight.* A frag at the last position once, if past shrapnel range
  and the position allows grenades. Then `NotifyLostSightOfEnemy`: after
  2.5 s, brave NPCs would head there (refused); the rest scan. If the enemy
  is gone at aim time, one more burst at the last position, then scan.
- *Every 8 s and after 10 s fighting.* `ConsiderChangingFiringPosition`.

Reactions:
- *Suppression.* A player's round passing within 192 suppresses, at most
  once per 15 s. Bravery goes to 35 (`iFixedSuppressionBravery`). The NPC
  goes prone if it wasn't reacting in the last 25 s, else one posture
  lower, then hides 2.5-4.5 s.
- *Grenades.*
  - Frags: 30 s per NPC, then `FRand() >= 0.5` (0.25 just after losing
    sight), never with a friend in the line. Ours also caps NPC frags at
    one per 120 s map-wide; retail threw 6 in 22 minutes.
  - Smoke: U72, from ChangeFiringPosition: 30 s, then `FRand() >= 0.8`.
  - A player's frag within 650 makes a fighting NPC run about 750 away
    (`TargetingNewEnemy.RunForCover` bypasses the state rules), then lie
    prone for 3.5 s.
  - An M84 in sight within 1000: Flashbanged for 7.5 s.
- *Reloads.* Prone, or crouched if already crouching; about 3 s (LMK 4 s);
  magazines are unlimited, as retail's `ForceWeaponLoading`.
- *Bravery.* Starts at 80 (frightened temperament 60). It rises out of
  danger and falls in danger (`AdjustMentalState`, every 0.25 s). It only
  changes what NPCs without the positions-only rule do.
- *Speed.* Top speed 200, reached over 0.5 s; retail's capture never shows
  faster.

Our own fix in passing: frag and incendiary damage skipped every pawn
without a controller, so NPCs never took grenade damage. They do now.

**Downed enemies** (retail ch 262, t=526.26-543.69).
- *Down.* A headshot still kills outright (bTearOff, Health -15). Other
  damage to 25 or below incapacitates:
  - `ClientNotifyNeutralizedNME` to the shooter at once (+10);
  - Health 25 steady, `IncapacitationType` set, `MovementSpeedModifier` 0.5;
  - graph 0 {41} and graph 2 {5 -> 67}, then ragdoll at +0.9 s with
    graph 0 {42 -> 13}.

  The next hit kills it.
- *Secured.* `AGP_PlayerController.SendEnemyDownRequest(name, interactor)`
  plus `ServerUse`, within 300, for a downed NPC:
  - teammates get `TeamMessage(PRI, "X secured Y (wounder : rifle)",
    'EnemyConfirm')` and `ClientReceiveRequestFriendly(47)`;
  - everyone gets `ClientRemovePlayerFromScoreboard(name)`;
  - the securer gets AD_SecuredCount and GA_ZipCuffLabel ('Secured NME',
    of 50), and `ClientNotifyScoringEventDetails(10, 0, 0, 6, +30)`;
  - the pawn gets graph 4 {150 -> 83}, `bBodyAlreadyIdentified`, Health 0,
    `IncapacitationType` None and `bPlayZipTie` (137); it closes 18 s on.
- *Confirmed.* The same request on a dead body gives commo 46, the secured
  counts and +30, then AD_ConfirmCount and GA_TNBLabel ('Confirmed NME') and
  `(9, 0, 0, 6, +15)` (retail 13:28:00 and 13:31:44).

**Changing roles mid-round** (`ServerSelectClassRole_ByIndex`).
- *When allowed.* Retail refuses a pick only while the deployment countdown
  runs (`bRoleSwappingDisabled`).
- *Alive.* A living soldier keeps his kit; the new role's applies at his
  next spawn.
- *No pawn yet, not played this round.* He gets
  `StartCountDownToLateJoin(5)` ("You will automatically volunteer as a
  reinforcement in 5 seconds") and joins the reinforcement queue 5 s later
  (`DelayedLateJoinTeamWithClassTimer`).
- *AskForPawn.* It now re-sends `GivePawn` to a client that has a pawn, as
  `PlayerController.AskForPawn`.

**Fixes from the first real-client test of the new AI (2026-10-13).**
- *ACOG blur stayed after lowering the sights.* Retail also tells the
  sight attachment: `AA_WeaponModSights.ClientNotifySightsUp()` (28) on
  raise and `ClientNotifySightsDown()` (29) on lower, on its own channel
  (Bridge AI capture, ACOG ch 154, frames 10602 / 10751). We never sent
  them, so the client kept the scope's blur.
- *NPCs vanishing and snapping back.* An NPC's crouch or prone moved its
  location down but never told the clients its posture. Their copy kept a
  standing body below the floor, which their physics dropped through the
  floor until the next update. NPCs now send `bIsCrouched` / `bIsProne` and
  retail's `bProxyInStandState` (78) / `bProxyInCrouchState` (79) with each
  change.
- *Bullet landing points.* The shot's trace started a fixed 31 above the
  pawn. Fitted to retail's `ImpactRepList` rays (entry point and
  direction, run back to the shooter's ClientLoc), retail's start is
  +33 standing and +20 crouched (6 of 6 crouched shots in the user's
  capture, 2-4 units across). Prone is about +3, from a smaller cluster in
  the bot capture. Player and NPC shots, and NPC eyes, now use these.
