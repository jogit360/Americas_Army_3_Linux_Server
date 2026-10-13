// Co-op reinforcements (respawning with F5), from the retail Bridge AI
// capture of 2026-10-12 (13:26:14, 13:29:14, 13:42:37) and
// AA_MCMissionManager.JoinPlayMidRound / PlayerReinforcementSpawnMonitor /
// SpawnRespawnPlayer:
//
//   request   AA_PlayerController.ServerRequestRespawn() while dead or
//             spectating: the player joins their team's queue ("You are now
//             waiting for your reserve fireteam ...", "Stand by for
//             deployment ...", "from readily available stores.").
//   wave      every 10 s of the round each team's queue deploys -- at least
//             DetermineMinPlayersToRespawn players (1 for small teams), up
//             to 4 -- each at an enabled AA_MCPlayerRespawnMarker of the
//             team not used in the last 15 s ("You have just hit the LZ
//             with your reserve team"), then the round-start spawn sequence.
#include <algorithm>
#include <cmath>
#include "game/world.h"
#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {
constexpr double kWaveSeconds = 10.0;
constexpr double kMarkerReuseSeconds = 15.0;
constexpr size_t kMaxPerWave = 4;
}  // namespace

void World::RequestRespawn(ServerPlayerController& pc) {
    if (!SupportsRespawning()) {
        pc.pending_messages.push_back({"You may only volunteer on Cooperative Missions that support reinforcements.", ""});
        return;
    }
    const int32_t team = TeamOf(pc);
    if (phase_ != MatchPhase::kInProgress || (team != 0 && team != 1) || pc.pawn) return;
    auto& q = respawn_queue_[static_cast<size_t>(team)];
    if (std::find(q.begin(), q.end(), pc.owning_client_id) != q.end()) {
        pc.pending_messages.push_back({"You are still waiting while your reserve fire team prepares to deploy.", ""});
        return;
    }
    q.push_back(pc.owning_client_id);
    pc.pending_messages.push_back({"You are now waiting for your reserve fireteam to complete before deploying (" +
                                       std::to_string(q.size()) + " players waiting.", ""});
    pc.pending_messages.push_back({"Stand by for deployment.  You will be assigned weapons and ammo", "Notification"});
    pc.pending_messages.push_back({"from readily available stores.", "Notification"});
    LOG_INFO("%s volunteers for reinforcement (team %d, %zu waiting)",
             pc.pri ? pc.pri->player_name.c_str() : "?", team, q.size());
}

void World::UpdateReinforcements() {
    if (!SupportsRespawning()) return;
    if (next_wave_at_ < 0.0) next_wave_at_ = clock_ + kWaveSeconds;
    // The HUD countdown (GRI.fpRespawnTimeSeconds, retail counts it down by
    // the second) and players still needed per team.
    gri_->respawn_time_seconds = static_cast<float>(std::ceil(std::max(0.0, next_wave_at_ - clock_)));
    for (int32_t t = 0; t < 2; ++t) {
        int32_t size = 0;
        for (const auto& c : controllers_) if (TeamOf(*c) == t) ++size;
        const int32_t need = size >= 8 ? 4 : size >= 4 ? 2 : 1;      // DetermineMinPlayersToRespawn
        const int32_t queued = static_cast<int32_t>(respawn_queue_[static_cast<size_t>(t)].size());
        gri_->respawn_players_needed[static_cast<size_t>(t)] = need > queued ? need - queued : 0;
    }
    if (clock_ < next_wave_at_) return;
    next_wave_at_ = clock_ + kWaveSeconds;
    for (int32_t team = 0; team < 2; ++team) {
        auto& q = respawn_queue_[static_cast<size_t>(team)];
        // Leavers and players already back in drop out of the queue.
        q.erase(std::remove_if(q.begin(), q.end(), [&](uint32_t id) {
                    ServerPlayerController* pc = FindPlayerController(id);
                    return !pc || pc->pawn || TeamOf(*pc) != team;
                }),
                q.end());
        // DetermineMinPlayersToRespawn: 4 for big teams, 2 for mid-size, else 1.
        int32_t size = 0;
        for (const auto& c : controllers_) if (TeamOf(*c) == team) ++size;
        const size_t min_players = size >= 8 ? 4 : size >= 4 ? 2 : 1;
        if (q.empty() || q.size() < min_players) continue;
        const int32_t marker_team = swapped_ ? 1 - team : team;     // GetPlayerMCTeamIndex
        size_t spawned = 0;
        for (auto it = q.begin(); it != q.end() && spawned < kMaxPerWave;) {
            RespawnMarker* spot = nullptr;
            for (auto& m : respawn_markers_)
                if (m.enabled && m.team == marker_team && clock_ - m.last_used > kMarkerReuseSeconds) {
                    spot = &m;
                    break;
                }
            if (!spot) break;
            ServerPlayerController* pc = FindPlayerController(*it);
            spot->last_used = clock_;
            pc->pending_messages.push_back({"You have just hit the LZ with your reserve team", ""});
            pc->has_respawn_spot = true;
            for (int k = 0; k < 3; ++k) pc->respawn_spot[k] = spot->loc[k];
            pc->respawn_spot_yaw = spot->yaw;
            pc->respawn_at = clock_;                 // SpawnPlayers deploys it now
            pc->died = true;
            pc->pending_messages.push_back({"Your equipment has been pulled from available ready stores.  You'll get "
                                            "your assigned gear at the start of the next mission.", "Notifications"});
            pc->pending_messages.push_back({"Organize your fire team and move out to support the squad.", "Notifications"});
            LOG_INFO("Reinforcement: %s deploys at (%.0f, %.0f, %.0f)", pc->pri ? pc->pri->player_name.c_str() : "?",
                     spot->loc[0], spot->loc[1], spot->loc[2]);
            it = q.erase(it);
            ++spawned;
        }
    }
}

}  // namespace game
}  // namespace aa3
