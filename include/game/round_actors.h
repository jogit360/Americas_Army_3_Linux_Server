#ifndef AA3_GAME_ROUND_ACTORS_H
#define AA3_GAME_ROUND_ACTORS_H

// Round actors (doc sec 28): the AA_ES2Manager, AA_FireTeamInfo and
// AA_MCClassRoleReplicationInfo actors retail opens when role selection
// starts. Their contents are mission data (role names/IDs, weapons, rail
// mods, loadouts, fire teams, spawn markers), loaded from the
// config/maps/<GameModeLevel>.round.json files that
// aa3-tools/map_manifest/extract_round_actors.py writes, and replicated as
// generic typed fields.

#include <cstdint>
#include <string>
#include <vector>
#include "game/server_actors.h"

namespace aa3 {
namespace game {

// DataValue / DataField live in game/server_actors.h.

struct ServerRoundActor : ServerActor {
    std::string class_name;      // e.g. "AA_MCClassRoleReplicationInfo"
    uint32_t max_index = 0;      // the class's RepIndex MaxIndex
    std::string cdo_package;     // where its class default object lives
    uint32_t cdo_net_index = 0;
    std::vector<DataField> fields;

    // Insert or replace the field at (idx, elem).
    void Set(uint32_t idx, int32_t elem, const DataValue& v);
    const DataField* Get(uint32_t idx, int32_t elem = -1) const;
    // Send the field again even if its value hasn't changed (the server
    // doesn't track it the way the client does, e.g. a magazine's RoundIndex).
    void Touch(uint32_t idx, int32_t elem = -1);
};

// A selectable role: its slot in a team's role list, which is the fire
// teams in iFireTeamID order and each fire team's aamcrriClassRoles in
// element order (retail: RoleIndex 1 = T0F0R1, Alpha's Fireteam Leader).
struct RoleSlot {
    ServerRoundActor* role = nullptr;
    ServerRoundActor* fire_team = nullptr;
    int32_t member_index = 0;    // element in the fire team's arrays
    int32_t fire_team_id = 0;
    const ServerPlayerReplicationInfo* owner = nullptr;
};

} // namespace game
} // namespace aa3

#endif // AA3_GAME_ROUND_ACTORS_H
