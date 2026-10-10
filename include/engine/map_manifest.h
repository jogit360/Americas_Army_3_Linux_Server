#ifndef AA3_ENGINE_MAP_MANIFEST_H
#define AA3_ENGINE_MAP_MANIFEST_H

// Runtime model of an AA3 map manifest. Built either by reading a JSON
// manifest from `config/maps/` (fast path) or by parsing the cooked `.aa3`
// package directly via PackageReader (slower, used by the offline build tool
// and as a fallback).
//
// Schema reference: aa3-tools/map_manifest/SCHEMA.md (v1).

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aa3 {
namespace engine {

struct PackageUniverse;

struct UsesEntry {
    std::string guid;        // 32 hex chars, uppercase; empty if unknown
    std::string pkg;
    std::string ext;         // "aa3" | "upk" | "u"
    uint32_t    flags = 0;
    int32_t     gen   = 1;
    std::string basepkg = "None";
    std::string role;        // "map" | "script" | "content" | "shard"
    // The package's net object count for `gen` -- its FPackageInfo
    // ObjectCount on the client, which sums into every later package's
    // ObjectBase (UPackageMap::Compute). -1 = unknown. Doc sec 26.
    int32_t     net_objects = -1;

    // Format as the retail server emits, matching FUN_009e8200:
    //   USES GUID=<32hex> PKG=<name> EXT=<ext> FLAGS=<u32 dec> GEN=<int> BASEPKG=<base>
    std::string FormatUsesLine() const;
};

struct MapManifest {
    int manifest_version = 1;

    // source
    std::string source_path;
    int64_t     source_size = 0;
    std::string source_sha256;
    int         engine_version = 0;
    int         licensee_version = 0;

    // map
    std::string url;
    std::string game_class;        // empty -> omitted from WELCOME line
    std::string tod;
    bool        is_gametype_map = false;
    std::string default_options;

    std::vector<UsesEntry> uses;

    // DLMGR line emitted on the wire immediately before WELCOME. Retail
    // emits exactly one — see GamePlayPCAP_Legit_Server.pcapng frame 46.
    // Empty string suppresses it.
    std::string dlmgr = "DLMGR CLASS=Engine.ChannelDownload PARAMS=Enabled COMPRESSION=0";

    // Build a manifest from a `.aa3` file on disk. Fills source.path, hashes,
    // and the primary `uses[]` entry from the parsed package summary. Direct
    // imports become additional `uses[]` entries; their guid/flags/gen are
    // left blank unless `cooked_root` is non-empty, in which case the tool
    // walks `cooked_root` for matching `.upk`/`.u` files and fills them in.
    static std::optional<MapManifest>
    BuildFromPackage(const std::string& map_path,
                     const std::string& cooked_root = "",
                     std::string* err = nullptr);

    // Same as BuildFromPackage but uses a pre-built PackageUniverse. Resolves
    // forced-export children so the USES list approximates the retail wire
    // package set. Use this in batch mode where the universe is shared.
    static std::optional<MapManifest>
    BuildWithUniverse(const std::string& map_path,
                      const PackageUniverse& uni,
                      std::string* err = nullptr);

    // Format the WELCOME line the way FUN_0048a8d0 does.
    std::string FormatWelcomeLine() const;

    // ObjectBase of `pkg` in the PackageMap a client builds from this USES
    // list: the sum of net_objects over the entries before it, in wire
    // order (UPackageMap::Compute). An object's wire ObjectIndex is
    // ObjectBase + its NetIndex (doc sec 11.7, 26). nullopt if `pkg` isn't
    // listed or an earlier entry's count is unknown.
    std::optional<uint32_t> ObjectBase(const std::string& pkg) const;

    // Emit all wire-bound lines, in order: WELCOME first, then one USES per
    // uses[] entry, in array order.
    std::vector<std::string> FormatWireLines() const;

    // Serialize to JSON (the v1 schema). Returns a self-contained string.
    std::string ToJson() const;

    // Parse from a v1 JSON manifest string. Returns std::nullopt on schema
    // mismatch.
    static std::optional<MapManifest>
    FromJson(const std::string& json, std::string* err = nullptr);
};

// Convenience: build a manifest, write it to `<out_dir>/<MapStem>.aa3.manifest.json`.
// `cooked_root` is forwarded to BuildFromPackage (pass "" to skip dep resolution).
std::optional<std::string>
BuildAndWriteManifest(const std::string& map_path,
                      const std::string& out_dir,
                      const std::string& cooked_root = "",
                      std::string* err = nullptr);

// Build a {PackageName -> filesystem path} index by walking `cooked_root`
// recursively and indexing every `.upk` / `.u` / `.aa3` file by stem. The
// first match wins on duplicate stems.
std::unordered_map<std::string, std::string>
IndexCookedPackages(const std::string& cooked_root);

// Fill every UsesEntry::net_objects from the cooked packages (`by_name` from
// IndexCookedPackages). A package cooked into a BASEPKG takes the count from
// that base package's forced-export entry (GenerationNetObjectCount);
// anything else from its own file's generation table. Returns the number of
// entries left unknown.
size_t FillNetObjectCounts(MapManifest& m,
                           const std::unordered_map<std::string, std::string>& by_name);

// Pre-built view of the cooked package universe, used to resolve dep info
// (guid/flags/gen), forced-export children, and BASEPKG attribution.
struct PackageUniverse {
    // package name -> on-disk path (for .u/.upk/.aa3)
    std::unordered_map<std::string, std::string> by_name;
    // package name -> raw FPackageFileSummary fields the manifest needs
    struct DepInfo {
        std::string guid;
        uint32_t    flags = 0;
        int32_t     gen   = 1;
        std::string ext;
    };
    std::unordered_map<std::string, DepInfo> info;
    // forced-export child -> canonical parent package (after priority resolution)
    std::unordered_map<std::string, std::string> forced_parent_of;
    // parent -> list of forced-export children (canonical, deduped)
    std::unordered_map<std::string, std::vector<std::string>> forced_children_of;
    // Per-child GUID embedded in the parent's FObjectExport entry — used when
    // the child has no on-disk file (retail still ships it as a USES with a
    // real GUID rather than zero).
    std::unordered_map<std::string, std::string> forced_child_guid;

    // Build a universe by scanning `cooked_root`. Slow (parses every cooked
    // package summary + name+import+export tables), so the CLI does it once
    // and reuses it across all maps in a batch.
    static PackageUniverse Build(const std::string& cooked_root);
};

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_MAP_MANIFEST_H
