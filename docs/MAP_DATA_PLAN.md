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

## 5. Phases

| # | Step | Result |
|---|---|---|
| 1 | Instanced collision in the current `.col` (unique meshes + instances + heightfield terrain), same API | ~95 MB -> ~10 MB; validation unchanged |
| 2 | Bundle container (header, section table, mmap) holding collision + manifest | one file per map, zero-copy load |
| 3 | Actors / windows / spawn points into the bundle, baked from packages | map_actors/windows JSON and spawn `.ini` keys retired |
| 4 | Mission data baked from the game-mode level | round/join/match/spawn_inventory JSON retired; any map playable without a capture |
| 5 | Bake all 91 maps; CI runs the validators | multi-map rotation |

Phase 1 alone delivers the memory win and touches only the collision
module, so it can go first without disturbing anything else.

## 6. Open questions

- Mission data from packages: role/fire-team definitions are in the
  game-mode level (`Bridge_inf_classic_large_dev`); needs the same
  archetype/property reading the collision work built, but for
  `AA_MCClassRole*` objects. To verify against `round.json`.
- Dynamic map objects that move (doors, InterpActors): collision for a
  door in its open pose needs the mesh's pivot and swing; today doors are
  traced in their closed pose only.
- Fractured meshes / destructibles beyond windows (none found on Bridge).
- Whether any map uses BSP heavily enough to merit convex-polygon BSP
  traces instead of triangles.
