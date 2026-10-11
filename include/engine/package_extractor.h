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
    // BSP, and terrain, packing into AA3COL04 (quantized, default) or AA3COL03.
    bool ExtractCollision(std::vector<uint8_t>& out_col_bytes,
                          uint32_t& out_num_meshes,
                          uint32_t& out_num_instances,
                          uint32_t& out_num_direct_tris,
                          uint32_t& out_total_tris,
                          bool quantize = true);

    // 4. Placed map actors (Section 3: kMapActors):
    // Extracts Level.Actors (WorldInfo, AA_Door, InterpActor, AA_BreakableWindow,
    // AdvMvmt_LadderVolume, AA_DynamicStaticMeshActor) into standard JSON.
    bool ExtractMapActors(std::string& out_json, const std::string& map_name);

    // 5. Mission round actors (Section 4: kRoundActors):
    // Extracts or synthesizes complete role hierarchy (Squad Leader + FT Alpha/Bravo/Charlie)
    // with FireTeamInfo and MCClassRoleReplicationInfo into standard JSON.
    bool ExtractRoundActors(std::string& out_json, const std::string& sublevel_name);

    // 6. Join & Match actors (Section 5: kJoinActors, Section 6: kMatchActors):
    // Extracts/synthesizes window manager, extraction objective markers, and ES2 match items.
    bool ExtractJoinActors(std::string& out_json, const std::string& sublevel_name);
    bool ExtractMatchActors(std::string& out_json, const std::string& sublevel_name);

    // 7. Mission rules (Section 7: kMissionRules):
    // Extracts extraction bounding boxes, time limits, and spectator viewpoints.
    bool ExtractMissionRules(std::string& out_json, const std::string& map_name, const std::string& sublevel_name);

private:
    const Package& pkg_;
};

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_PACKAGE_EXTRACTOR_H
