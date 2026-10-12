#ifndef AA3_ENGINE_PACKAGE_EXTRACTOR_H
#define AA3_ENGINE_PACKAGE_EXTRACTOR_H

// PackageExtractor: extracts collision geometry (AA3COL03), breakable windows,
// and gameplay markers directly from cooked UE3 v568 packages (.aa3 / .u)
// without Python dependencies.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/package_reader.h"
#include "game/collision.h"

namespace aa3 {
namespace engine {

struct SpawnPointDef {
    int team = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int32_t yaw = 0;
};

class PackageExtractor {
public:
    explicit PackageExtractor(const Package& pkg);

    // 1. Breakable windows table: map NetIndex -> manager bit index.
    // Matches export_windows.py (order of AA_BreakableWindow in Level::Actors).
    bool ExtractWindows(std::map<uint32_t, int>& out_windows);

    // 2. Spawn points: extracts PlayerStart / AA_PlayerStart actors.
    bool ExtractSpawns(std::vector<SpawnPointDef>& out_spawns);

    // 3. Collision geometry: extracts unique meshes, placed instances,
    // BSP, and terrain, packing into AA3COL03 (lossless, default: the shipped
    // bundles' 1:1 form) or AA3COL04 (16-bit quantized).
    // `extra_packages` (loaded sublevels) add their static geometry.
    bool ExtractCollision(std::vector<uint8_t>& out_col_bytes,
                          uint32_t& out_num_meshes,
                          uint32_t& out_num_instances,
                          uint32_t& out_num_direct_tris,
                          uint32_t& out_total_tris,
                          bool quantize = false,
                          const std::vector<const Package*>& extra_packages = {});

    // 4. Placed map actors (Section 3: kMapActors): WorldInfo, AA_Door,
    // AA_BreakableWindow, AdvMvmt_LadderVolume,
    // AA_DynamicStaticMeshActor from Level.Actors, by on-disk NetIndex, with
    // MaxIndex from `max_index` (config/class_maxindex_table.tsv). Mission
    // data (roles, objectives, rules) comes from MissionExtractor.
    bool ExtractMapActors(std::string& out_json, const std::string& map_name,
                          const std::map<std::string, uint32_t>& max_index);

private:
    std::vector<int32_t> LevelActorOrder() const;

    const Package& pkg_;
};

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_PACKAGE_EXTRACTOR_H
