# AA3 Map Translation Layer — Design

Sister document to [`AA3_ACTOR_REPLICATION.md`](./AA3_ACTOR_REPLICATION.md). The
goal is a **generic** translation layer that takes any AA3 cooked map
(`.aa3` after `aa3_pipeline.sh` cleaning) and emits the data the server
needs to drive `ChType=2` actor replication when the client JOINs:
which actors to open channels for, in what order, with which class
references and replicated property values. The translation must work
for arbitrary AA3 maps, not just `Bridge_night_clear`, and must not
depend on captured pcap byte-replay.

## 1. What the server actually needs at JOIN time

Per `UnChan.cpp::UActorChannel::ReceivedBunch` (line 1359) every open
bunch the server emits looks like:

```
PackageMapLevel::SerializeObject(Object)     // 1 bit + 31-bit RIM
[if Object is an archetype/CDO]
   SerializeCompressedInitial(Loc, Rot, ...) // transient spawn data
[if Object is a PlayerController]
   NetPlayerIndex : 1 byte
PropertyReplicationLoop                       // RepIndex + value pairs
```

So per actor we need: (a) the on-wire `ObjectIndex`, (b) whether the
object is a placed instance vs. an archetype/CDO, (c) for transient
spawns: Location + Rotation, (d) the initial replicated property values
(only those that differ from class defaults).

## 2. What's in `.aa3` map files

Verified by inspecting cleaned `Bridge_night_clear.aa3` (see
`out_full.json` from `aa3-tools/PackageMapDumper`):

- `TheWorld` (`World` class) — singleton root.
- `TheWorld.PersistentLevel` (`Level` class) — holds all placed actors.
- `TheWorld.PersistentLevel.WorldInfo_2` (`WorldInfo` class) — the
  level's WorldInfo instance.
- ~9 `TheWorld.LevelStreamingKismet*` streaming controllers.
- 6,700+ `TheWorld.PersistentLevel.*` placed actors:
  `AA_BreakableWindow`, `StaticMeshActor`, `PointLight`, `Brush`,
  `Material`, etc. — every editor-placed thing in the map.
- All prefab archetypes for actors that were dropped from prefabs.

The export table gives class + full name for free; the body data
contains everything else.

## 3. Cooked-package body layout (the hard part)

For each export, `SerialOffset` points at the start of the object's
serialized body. The body is the output of `UObject::Serialize` (UnObj.cpp:1596).

For OBJECTS WITH `RF_HasStack` (most placed level actors in AA3):

```
FStateFrame:
  Node          : FPackageIndex      // 4 bytes
  StateNode     : FPackageIndex      // 4 bytes
  ProbeMask     : QWORD              // 8 bytes
  [if ver < REDUCED_STATEFRAME_LATENTACTION_SIZE]
    LatentAction : INT               // 4 bytes
  [else]
    LatentAction : WORD              // 2 bytes
  StateStack    : TArray<FStateFrameElement>  // 4-byte count + items
  Offset        : INT                // 4 bytes
NetIndex        : INT                // 4 bytes
SerializeScriptProperties()          // tagged props until "None" name
```

For OBJECTS WITHOUT `RF_HasStack`:

```
NetIndex        : INT
SerializeScriptProperties()
```

Verified examples from `Bridge_night_clear.u`:

| Export | Class | First 16 bytes | Interpretation |
|---|---|---|---|
| #3 | `AA_BreakableWindow` (placed) | `e4ffffff e4ffffff ffffffffffffffff` | Node=-28, StateNode=-28, ProbeMask=~0 — **has stack** |
| #24588 | `WorldInfo_2` (placed) | `0cffffff 0cffffff ffffffffffffffff` | Node=-244, StateNode=-244, ProbeMask=~0 — **has stack** |
| #4060 | `Level` (PersistentLevel) | `0d070000 85020000 0000000000000fdd` | Node=+1805, StateNode=+2693, ProbeMask=0x00000fdd00000000 — **has stack**, different state |
| #4061 | `LevelStreamingKismet` | `0e070000 b40a0000 0000000000000a74` | Node=+1806, StateNode=+2740, ProbeMask=0x00000a7400000000 — **has stack**, different state |

So virtually every placed level actor in AA3 has `RF_HasStack` set. We
cannot read NetIndex from `SerialOffset[0..4)` — we must skip FStateFrame
first.

The body bytes don't directly tell us whether an object has `RF_HasStack`
(that's a runtime flag), but the FStateFrame pattern is recognizable:
two FPackageIndex values followed by an 8-byte ProbeMask. Practically
all placed actors in cooked AA3 levels have the pattern.

## 4. The wire-index puzzle (recap)

Six AA3Baseline classes from `GamePlayPCAP_Legit_Server.pcapng` frame
72-74 resolve to the expected class with a constant `+19` offset
(`NetIndex = export_table_index + 19`). Two indices (`172588`, `193640`)
point at WorldInfo / a level actor but our cumulative ObjectBase math
is off by 29,417 for Bridge_night_clear. Likely root causes (untested):

1. The cooked-package per-object NetIndex is stored on disk
   (`SerializeNetIndex` in `UnObj.cpp:1565`) and is **not** identical to
   export-table-index + 19 — the +19 we observed for six classes may
   be a happy-path coincidence on a sequential block. To prove or
   disprove this we must parse past FStateFrame and read real NetIndex.
2. The client's `UPackageMap` doesn't enumerate every USES-acked
   package linearly — some packages may contribute conditionally.
3. AA3 has a customized PackageMap implementation.

The translation-layer design below sidesteps the per-object NetIndex
question for placed-level actors by anchoring on **the level package's
own internal numbering**: every placed actor has a stable position
inside `TheWorld.PersistentLevel` and that position is what UE3 uses
to disambiguate them. Whatever exact constant translates that to a
wire `ObjectIndex` can be discovered per-map at server startup by
parsing one captured open-bunch (a "calibration" step) without baking
in the byte stream.

## 5. Design

### 5.1 Stage 1 — Map decoder (offline, build-time)

A tool (Python or C#, leveraging `aa3-tools/PackageMapDumper` for the
export-table walk) that for a cleaned `<map>.u` emits a manifest JSON:

```jsonc
{
  "map_name": "Bridge_night_clear",
  "package_object_base_hint": 177398,    // from PackageMapDumper, may need calibration
  "world": {
    "export_index": 24587,
    "class": "World",
    "full_name": "TheWorld"
  },
  "persistent_level": {
    "export_index": 4060,
    "class": "Level"
  },
  "world_info": {
    "export_index": 24588,
    "class": "WorldInfo",
    "full_name": "TheWorld.PersistentLevel.WorldInfo_2",
    "has_state_frame": true,
    "body_offset_after_state_frame": <N>,  // skip distance to NetIndex
    "net_index_on_disk": <M>,              // if we can read it
    "tagged_properties": { ... }           // Location, Rotation, GameInfo class, ...
  },
  "level_streaming": [ /* LevelStreamingKismet entries */ ],
  "placed_actors": [
    {
      "export_index": 3,
      "class": "AA_BreakableWindow",
      "full_name": "TheWorld.PersistentLevel.AA_BreakableWindow",
      "has_state_frame": true,
      "tagged_properties": { /* Location, ... */ }
    },
    /* 6700+ more */
  ]
}
```

Open-source UE3 forks have implementations of cooked-package parsing
we can crib from (umodel, UELib's FullPackage path). For the
translation layer we re-use UELib's export table walk and write our
own FStateFrame skipper + tagged-property reader on top.

### 5.2 Stage 2 — Index calibration (per server start, optionally)

If we can't reliably compute on-wire `ObjectIndex` from the manifest
alone (the +19 / 29k puzzle), we have a small calibration pass:

1. The server emits its USES storm normally.
2. The client acks and JOINs.
3. The server picks ONE well-known class (e.g., the level's WorldInfo)
   and consults a small **observation database** that records "for this
   exact package manifest + map, the WorldInfo ObjectIndex is X". The
   database starts populated from `GamePlayPCAP_Legit_Server.pcapng`
   captures.
4. Anchored on that one observation, the server computes the per-map
   wire-index offset constant. All other actors in the map get
   `manifest.export_index + constant`.

If we eventually crack the math fully (option from `AA3_ACTOR_REPLICATION.md`
§5.8 path B), this calibration step becomes a no-op fallback for
unknown manifests.

### 5.3 Stage 3 — Runtime replication driver

The server reads the manifest at JOIN time and:

1. Opens `Ch=2..N` in manifest order, mirroring the legit server's frame
   72-80 channel-allocation pattern.
2. For each channel, emits a bunch:
   - PackageMapLevel::SerializeObject with `ObjectIndex = manifest.export_index + calibrated_offset`.
   - If the manifest entry is an archetype/CDO (rarely for placed-level
     actors): emit `SerializeCompressedInitial(Location, Rotation, ...)`.
   - Property replication loop emitting only properties whose value
     differs from the class default (per `manifest.tagged_properties`).
3. Listens for the client's acks and watches for the client's first
   `Ch=N` MOVEMENT bunch (input replication) to confirm the world spawn
   worked.

## 6. Implementation plan

| # | Task | Output |
|---|---|---|
| 1 | Add `state_frame_size`, `net_index_on_disk`, and `tagged_props_offset` per export to `PackageMapDumper` JSON. | Updated `out_full.json` schema. |
| 2 | Implement FStateFrame skipper in C# (matches `UObject::Serialize` 1684-1747 verbatim, gated on the FStateFrame signature). | Function in `PackageMapDumper` that returns the byte offset just past FStateFrame for any export body. |
| 3 | Use #2 to read NetIndex correctly for every export. Validate against legit pcap — the six AA3Baseline classes should now resolve cleanly without the +19 mystery. | New evidence on whether NetIndex==export-index-plus-constant or some other relationship. |
| 4 | Tagged-property reader — walk `[FName name][FName type][INT size][value-bytes][...]` until name == "None". For `WorldInfo`'s known replicated set this gives us `TimeDilation`, `WorldGravityZ`, etc. | Property value extraction. |
| 5 | Map manifest writer — combine #1-#4 into a per-map JSON conforming to §5.1 schema. | `aa3-tools/MapTranslator`. |
| 6 | Server-side manifest loader and replication driver — `aa3-linux-server/src/network/actor_replication.cpp`. | `ChType=2` channel opens that don't hard-code any bytes. |

Tasks #1-#4 unblock step #3 (the wire-index math discovery). After
that, tasks #5-#6 are mostly schema and integration work.

## 7. Status — 2026-05-15

- Tasks #1-#2 in progress. Status of NetIndex extraction: confirmed
  that `SerialOffset[0..4)` is FStateFrame, not NetIndex. We need to
  skip the variable-length FStateFrame first.
- `aa3-tools/PackageMapDumper/out_full.json` already has 162/168
  packages with `object_base`, export tables, and `serial_offset`
  per export — that's the foundation for the manifest.
- Next focused work item: implement FStateFrame skipper, then re-read
  NetIndex for the six legit-pcap-verified AA3Baseline classes and
  see if the `+19` constant disappears.
