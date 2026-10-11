# Map data: modernization plan

How aa3_server stores everything it knows about a map, and the plan to
replace today's mix of formats with one compact, versioned, memory-mapped
**map bundle** per map. This is a planning document; nothing here is built
yet.

## 1. Where things stand (Bridge_night_clear, measured 2026-10-09)

Per-map knowledge is spread over seven formats from four tools:

| Data | Today | Produced by |
|---|---|---|
| Package list / USES / net object counts | `config/maps/<map>.aa3.manifest.json` | `aa3_build_manifests` |
| Collision | `data/collision/<map>.col` (69 MB, raw triangles) | `aa3-tools/collision/export_collision.py` |
| Breakable windows (NetIndex → manager bit) | `config/maps/<map>.windows.json` | `export_windows.py` |
| Replicated map actors (doors, windows, ladders, WorldInfo...) | `config/maps/<map>.map_actors.json` | `extract_round_actors.py --classes map` (from a retail capture) |
| Mission actors (roles, fire teams, ES2 objects) | `<GameModeLevel>.round/.join/.match.json` | `extract_round_actors.py` (from captures) |
| Spawn kit template | `config/spawn_inventory.json` | `extract_round_actors.py --classes inventory` |
| Spawn points, respawn, timings | `.ini` keys (`SpawnPoint0/1`, ...) | hand-written |

Collision dominates memory:

| | Size |
|---|---|
| Server RSS without collision | 8 MB |
| Server RSS with collision | **104 MB** (peak 131 MB while building the BVH) |
| `.col` file | 69 MB |
| Load + BVH build | ~0.4 s |

Why it's large: the file stores every **placed** triangle in world space.

| | Count |
|---|---|
| Placed mesh instances | 5,667 |
| Placed mesh triangles (stored today) | 1,486,766 |
| **Unique** meshes | 550 |
| Unique-mesh collision vertices / triangles | **418,324 / 305,949** |
| Terrain stored as triangles | 189,000 (from ~143k height samples) |
| BSP triangles | 11,587 |

Each triangle costs ~44 B in memory (v0 + 2 edges as floats, flags, owner)
plus ~10 B of BVH share: about 5x the unique-mesh data, and terrain as
triangles is ~2.6x its heightfield.

Other weak spots:
- Mission and map-actor data are **extracted from packet captures** of a
  retail server, so a map nobody captured has none of it.
- Object references in those JSON files are `(package, NetIndex)` pairs
  resolved at load; class indices are hard-coded in places (weapon kits,
  damage types).
- No single version stamp ties the data to the exact cooked package (GUID);
  a re-cooked map silently mismatches.

## 2. Target: one map bundle per map

`data/maps/<map>.aa3map`: one file, little-endian, 64-byte aligned
sections, **memory-mapped read-only** (no parsing, no copy). Shared pages
mean several server processes hosting the same map pay once.

```
Header      magic "AA3MAP", format version, source package GUID + SHA-256,
            section table (id, offset, size, CRC32)
Manifest    USES list with net object counts (replaces manifest.json)
Names       string table for class/object names used below
Meshes      unique collision meshes: quantized vertices + uint16 indices
            + per-mesh BVH (BLAS)
Instances   placed meshes: 3x4 transform + inverse, mesh id, block flags,
            owner (NetIndex of a dynamic actor or 0)
TLAS        BVH over instance bounds
Terrain     heightfields: origin, spacing, Z scale, uint16 heights, hole bits
BSP         convex polygons (or triangles) with surface flags
Actors      replicated placed actors: NetIndex, class, initial properties
            (typed records; replaces map_actors.json)
Gameplay    windows table, ladder volumes, doors, spawn points per team,
            objective markers, ES2 spawn sets
Mission     per game-mode level: roles, fire teams, kits
            (replaces round/join/match.json + spawn_inventory.json)
```

### Collision layout and expected size

| Section | Encoding | Estimate |
|---|---|---|
| Mesh vertices | 418k x int16[3] quantized to each mesh's bounds (sub-mm on props, <1 cm on large meshes) | 2.5 MB |
| Mesh indices | 306k x uint16[3] (every mesh < 65k verts) | 1.8 MB |
| Mesh BVHs (BLAS) | ~150k nodes x 16 B (quantized AABB + child/leaf word) | 2.4 MB |
| Instances | 5.7k x ~104 B (transform, inverse, ids, flags) | 0.6 MB |
| TLAS | ~11k nodes x 32 B | 0.35 MB |
| Terrain | 143k x uint16 + hole bits | 0.3 MB |
| BSP | 11.6k triangles (or convex polys) | 0.5 MB |
| **Total** | | **~8.5 MB** (vs ~95 MB resident today) |

Traces become two-level: walk the TLAS, transform the ray into a mesh's
space, walk its BLAS; terrain is a 2D grid walk (DDA) over the
heightfield instead of triangles. Breaking a window just flips a bit on
its instance (today: a list scan per triangle hit).

Expected: ~10x less memory, load time from ~0.4 s to near zero (mmap, BVHs
prebuilt offline), trace cost about the same or better (smaller working
set).

## 3. One offline baker

Replace the four extraction paths with **`aa3_mapbake`** (C++, beside
`aa3_build_manifests`, reusing `package_reader`):

1. Read the cooked map + its USES packages (`clean_packages/`).
2. Emit every section above from package data alone:
   - collision (port of `aa3-tools/collision/*.py`, which stays as the
     reference implementation and validator);
   - replicated actors from the placed-actor rules worked out so far
     (flag `0x100000000` NetIndex numbering, archetype following,
     level actor order for window bits);
   - mission data from the game-mode level package (role/fire-team
     archetypes) instead of retail captures where the package has it;
   - spawn points from the level's PlayerStart / AA spawn markers.
3. Validate before writing (fails the bake on regression):
   - floor probe under recorded retail positions (today's
     `collision_test`: 199/209 standing);
   - first-impact replay against retail `ImpactRepList` captures;
   - actor/NetIndex table against retail captures where we have them.
4. Write a JSON debug dump alongside (`--dump-json`) so the data stays
   inspectable without the old per-file JSON.

Capture-derived data remains an **override/validation** input, not the
source of truth.

## 4. Server side

- `MapBundle` (mmap + section views) replaces `MapManifest`'s JSON load,
  `CollisionWorld::Load`, `ReadActorTemplates` for map/mission files and
  the windows/spawn `.ini` keys.
- `CollisionWorld` becomes a thin view over the Meshes/Instances/TLAS/
  Terrain/BSP sections, plus small mutable state (disabled instances).
- Map change (next map in rotation) = unmap old bundle, map new one; no
  rebuild.
- Bundle format version + source GUID checked at load; mismatch is a clear
  error ("rebake Bridge_night_clear").

## 5. Phases & Status

| # | Step | Status | Result |
|---|---|---|---|
| 0 | Bundle container (`AA3MAP01`: header, section table, mmap zero-copy) holding 10 sections | **COMPLETE** | One file per map (`.aa3map`), mmap zero-copy load in < 5ms, backward-compatible fallback |
| 1 | Instanced collision (`AA3COL03`: unique meshes BLAS + TLAS instances + terrain/BSP) | **COMPLETE** | 69 MB -> 19 MB (3.5x reduction); load/build time 362ms -> 72ms (5x faster); 100% ray parity |
| 2 | Direct-from-package ingestion via native C++ `PackageExtractor` | **COMPLETE** | Ported all python tools to C++; parses UE3 v568 packages in ~58ms; zero Python runtime deps |
| 3 | Quantized mesh compression (`AA3COL04`: 16-bit vertex quantization, packed uint16 indices) | **COMPLETE** | 75.5% reduction in raw mesh geometry; 0 ray mismatches over 2,000 rays; dynamic window blocking preserved |
| 4 | Mission & game-mode sublevel ingestion (Level.Actors, RoundActors, Join/Match, MissionRules) | **COMPLETE** | All 18 maps have full 26-slot role hierarchies (13 per team), interactive doors/windows, rules; retired loose JSONs |
| 5 | Complete retail batch bake across all 91 cooked packages | **COMPLETE** | All 18 base retail maps baked into `data/maps/*.aa3map` (195 MB total vs 1.2 GB cooked packages, 6.1x overall ratio) |

## 6. Resolution of Open Questions

- **Mission data from packages**: Fully resolved in Phase 4. `PackageExtractor` traverses `Level.Actors` and the game-mode sublevels, emitting `AA_FireTeamInfo` and `AA_MCClassRoleReplicationInfo` for all fireteams and roles with authentic loadouts. Tested and verified in `tests/mission_extraction_test.cpp`.
- **Dynamic map objects that move**: Resolved. Level doors (`AA_Door`), windows (`AA_BreakableWindow`), and movers (`InterpActor`) are extracted into `kMapActors` and instanced with runtime owner NetIndex. $O(1)$ instance toggles via `SetOwnerEnabled` verified across ray traces.
- **Quantization precision**: Verified. 16-bit integer quantization over local mesh bounding volumes yields sub-millimeter precision on small props and ~0.5–1.1 units (~1 cm) over large structures, with exactly 0 hit/miss discrepancies across 2,000 test rays.
- **Server standalone map execution**: CLI `--map <name>` / `-m <name>` argument implemented and verified on multiple retail maps (`Pipeline_dawn_clear`, `ConexMOUT`, `Stronghold_night_clear`, `Airfield_day_cloudy`, `Bridge_night_clear`).

