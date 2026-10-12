#ifndef AA3_ENGINE_MISSION_EXTRACTOR_H
#define AA3_ENGINE_MISSION_EXTRACTOR_H

// Mission data of a gametype level (e.g. Pipeline_inf_ad_large_classic),
// read from its Kismet the way retail builds a round:
//
//   AA_MCMissionInfoConnector  miMissionInfo (GRI mission name/author/...),
//                              Team[0]/Team[1] -> AA_MCTeamInfo
//   AA_MCTeamBuilderSeqAct     "Team Info" -> AA_MCTeamInfo (iTeamID, name,
//                              briefing, mission type); input link k = the
//                              fire team in slot k (Alpha, Bravo, ...;
//                              AA_MCFireteamInfoBuilderSeqAct.iFireteamID is
//                              set from that link, doc comment in its .uc)
//   AA_MCFireteamInfoBuilderSeqAct  "Position 1..5" -> AA_MCClassRole_* objects
//   TeamBuilder -> SpawnGroupManager -> AA_MCSpawnSelectorSeqAct
//                              "Alpha"/"Bravo"/... -> AA_MCSeqVar_Object ->
//                              AA_MCFireteamSpawnMarker (anpPositions = its
//                              AA_PlayerStarts)
//
// Role replication info comes from per-class templates (retail's values,
// config/class_role_templates.json) with the level's loadout overrides on
// top. Output is the bundle's round / join / match / rules JSON.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "engine/package_reader.h"

namespace aa3 {
namespace engine {

// config/class_maxindex_table.tsv: class name -> MaxIndex (empty on error).
std::map<std::string, uint32_t> LoadClassMaxIndexTable(const std::string& path);

struct MissionSpawn {
    float x = 0, y = 0, z = 0;
    int32_t yaw = 0;
};

struct MissionData {
    std::string round_json;
    std::string join_json;
    std::string match_json;
    std::string rules_json;
    bool has_team_spawn[2] = {false, false};
    MissionSpawn team_spawn[2];
    std::vector<std::string> warnings;
};

class MissionExtractor {
public:
    // `level` is the gametype level package, `map` the base map (for its
    // AA_Viewpoint NetIndex values); both need their bodies loaded.
    MissionExtractor(const Package& level, const std::string& level_name,
                     const Package& map, const std::string& map_name);

    // config/class_role_templates.json text.
    bool LoadRoleTemplates(const std::string& json, std::string* err);

    // False when the level has no team builders (training levels).
    bool Build(MissionData& out);

private:
    const Package& level_;
    std::string level_name_;
    const Package& map_;
    std::string map_name_;
    std::string role_templates_json_;
};

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_MISSION_EXTRACTOR_H
