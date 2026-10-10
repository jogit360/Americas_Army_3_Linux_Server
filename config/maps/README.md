# Map manifests

One `*.aa3.manifest.json` per cooked AA3 map package. Generated offline by:

```
python3 aa3-tools/map_manifest/build_manifest.py \
    --maps-root  AA3/AA3Game/CookedPC/Maps \
    --out-dir    aa3-linux-server/config/maps \
    --repo-root  .
```

Schema reference: `aa3-tools/map_manifest/SCHEMA.md` (v1).

## What the server does with these

At level-load time the retail server emits two text RPCs on the control
channel (decompiled from `FUN_0048a8d0` + `FUN_009e8200`, parsed client-side
by `FUN_009f6940`):

```
WELCOME LEVEL=<map.url> [<map.game_class>]
USES    GUID=<uses[i].guid> PKG=<uses[i].pkg> EXT=<uses[i].ext> \
        FLAGS=<uses[i].flags> GEN=<uses[i].gen> BASEPKG=<uses[i].basepkg>
... (one USES per entry)
```

The linux server's level-load handler walks `uses[]` top-to-bottom and emits
those lines verbatim. The manifest order is already deterministic
(scripts → content → shards → map).

## Asset mirror (TODO)

The bytes themselves live alongside the manifests under
`aa3-linux-server/assets/Maps/<MapDir>/<file>.aa3`. To mirror the source
tree:

```
rsync -av --include='*/' --include='*.aa3' --exclude='*' \
    AA3/AA3Game/CookedPC/Maps/  aa3-linux-server/assets/Maps/
```

The manifest's `source.path` and per-USES `file.path` are repo-relative; the
server normalizes them to its own asset root at startup.

## Known gaps (v1)

- `uses[i].guid` is null for non-primary packages. Fill from `GuidCache.upk`
  at server startup (TODO: write a small GuidCache reader; the cache is a
  plain UPackage with one FGuidCache export = `TMap<FName,FGuid>`).
- `uses[i].gen` defaults to 1 for non-primary packages. Read the real value
  from each package's `FPackageFileSummary.Generations.Num()` once we land
  the script/content sweep.
- Direct imports list is empty for any map whose body is
  `PKG_StoreFullyCompressed` (almost all gametype maps). The static
  script-base list in `build_manifest.py` covers the always-loaded set;
  per-map content imports need an LZO-decompressed pass before they can be
  enumerated.
