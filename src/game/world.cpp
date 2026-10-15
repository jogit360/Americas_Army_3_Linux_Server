#include <cmath>
#include "game/world.h"
#include "engine/map_manifest.h"
#include "engine/map_bundle.h"
#include "network/rep_layout_gen.h"
#include "util/json.h"
#include <fstream>
#include <sstream>
#include <ctime>
#include "util/logging.h"

#include <algorithm>

namespace aa3 {
namespace game {

World::World() : tod_("Day") {
}

World::~World() {
    if (nav_thread_.joinable()) nav_thread_.join();
}
//This should come the map information
void World::Initialize(const std::string& map_name) {
    map_name_ = map_name;

    if (map_name.find("_night") != std::string::npos) {
        tod_ = "Night";
    } else if (map_name.find("_dawn") != std::string::npos) {
        tod_ = "Dawn";
    } else if (map_name.find("_dusk") != std::string::npos) {
        tod_ = "Dusk";
    } else {
        tod_ = "Day";
    }

    InitializeActors();

    LOG_INFO("World initialized: Map=%s, TOD=%s, actors=(WorldInfo, GRI, "
             "%zu TeamInfos, GameInfo)",
             map_name_.c_str(), tod_.c_str(), teams_.size());
}

namespace {
// NetIndex of each class default object this server spawns from: in cooked
// AA3 script packages, an export's NetIndex is its export-table position
// (each checked with `aa3_build_manifests --dump-exports`). The wire
// ObjectIndex is ObjectBase(package) + NetIndex, and ObjectBase depends on
// the USES list sent to the client -- so these are package-relative and
// the absolute values are computed in BindPackageMap (doc sec 26).
namespace net_index {
// AA3Baseline.u
constexpr uint32_t kPawn            = 8103;   // Default__AA_Pawn
constexpr uint32_t kGRI             = 10929;  // Default__AA_GameReplicationInfo
constexpr uint32_t kMOSQualBCT      = 18071;  // Default__AA_MOSQualificationBCT
// The rest of retail's per-player MOS set (doc sec 39.7), in its open order.
constexpr uint32_t kMOSQualExtra[]  = {18079,   // Default__AA_MOSQualificationDRMAIT
                                       18091,   // Default__AA_MOSQualificationGrenadierAIT
                                       18032,   // Default__AA_MOSQualficationCombatMedic
                                       18044,   // Default__AA_MOSQualificationARAIT
                                       18099};  // Default__AA_MOSQualificationRiflemanAIT
constexpr uint32_t kPRI             = 21708;  // Default__AA_PlayerReplicationInfo
constexpr uint32_t kSteamStatMgr    = 29740;  // Default__AA_SteamStatManager
constexpr uint32_t kTeamInfo        = 32631;  // Default__AA_TeamInfo
constexpr uint32_t kPlayerStorageProxy = 27488;  // Default__AA_PlayerStorageProxy
// AA3Game.u
constexpr uint32_t kPlayerController = 1286;  // Default__AGP_PlayerController
constexpr uint32_t kHUDClass         = 1114;  // class AGP_HUD (ClientSetHUD argument)
constexpr uint32_t kGameInfoClass    = 682;   // class AGP_GameInfo (GRI.GameClass)
}  // namespace net_index
}  // namespace

uint32_t World::BaselineIndex(uint32_t n) const {
    return base_aa3baseline_ ? base_aa3baseline_ + n : 0;
}
uint32_t World::GameIndex(uint32_t n) const {
    return base_aa3game_ ? base_aa3game_ + n : 0;
}

std::string World::MapBaseName(const engine::MapManifest& manifest) {
    // The map's sublevels are its "shard" USES entries; the base name is
    // what precedes "_pathing" (Bridge_pathing -> Bridge).
    const std::string suffix = "_pathing";
    std::string base;
    for (const auto& u : manifest.uses)
        if (u.role == "shard" && u.pkg.size() > suffix.size() &&
            u.pkg.compare(u.pkg.size() - suffix.size(), suffix.size(), suffix) == 0)
            base = u.pkg.substr(0, u.pkg.size() - suffix.size());
    return base;
}

std::string World::ResolveGameModeLevel(const engine::MapManifest& manifest,
                                        const std::string& configured) {
    if (!configured.empty()) return configured;
    const std::string base = MapBaseName(manifest);
    return base.empty() ? "" : base + "_inf_classic_large_dev";
}

void World::ConfigureGameMode(const engine::MapManifest& manifest,
                              const std::string& configured) {
    const std::string base = MapBaseName(manifest);
    const std::string suffix = "_pathing";
    streaming_levels_.clear();
    pathing_level_ = base.empty() ? "" : base + suffix;
    game_mode_level_ = ResolveGameModeLevel(manifest, configured);
    for (const auto& u : manifest.uses)
        if (u.role == "shard" && u.ext == "aa3") streaming_levels_.push_back(u.pkg);

    // GRI mission fields from "<Base>_inf_<type>_<size>_<variant>", e.g.
    // Bridge_inf_classic_large_dev -> sMissionType "classic", sMissionVariant
    // "dev" (retail's values for that level).
    if (gri_) {
        gri_->mission_active_sublevel = game_mode_level_;
        std::vector<std::string> parts;
        std::string cur;
        for (char c : game_mode_level_.substr(base.empty() ? 0 : base.size()))
            if (c == '_') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur += c;
        if (!cur.empty()) parts.push_back(cur);
        // parts: "inf", type, size, variant
        if (parts.size() >= 4 && parts[0] == "inf") {
            gri_->s_mission_type = parts[1];
            gri_->s_mission_variant = parts[3];
        }
    }
    LOG_INFO("Game mode level: %s (mission type '%s', variant '%s')", game_mode_level_.c_str(),
             gri_ ? gri_->s_mission_type.c_str() : "", gri_ ? gri_->s_mission_variant.c_str() : "");
}

ServerTeamInfo* World::FindTeam(int32_t team_index) {
    for (auto& t : teams_) if (t.team_index == team_index) return &t;
    return nullptr;
}

bool World::JoinTeam(ServerPlayerController& pc, int32_t team_index) {
    // Black (0) and Gold (1) are the playable teams; GRI.aiEmptyTeamSlots
    // counts their free places (team menu shows FULL at 0). 255 is the
    // client's auto-assign: the team with more free slots, Black on a tie.
    if (!gri_ || !pc.pri) return false;
    if (team_index == 255) {
        // 255 = Spectator (the client's team menu "Spectator" button): leave
        // any playable team and sit on the Spectator TeamInfo (index 4).
        if (ServerTeamInfo* spec = FindTeam(4)) {
            if (pc.pri->team == spec) return true;
            LeaveTeam(*pc.pri);
            pc.pri->team = spec;
            pc.pri->b_is_spectator = true;
            UpdateTeamMembers();
            return true;
        }
        return false;
    }
    if (team_index < 0 || team_index > 1) return false;
    ServerTeamInfo* team = FindTeam(team_index);
    if (!team || pc.pri->team == team) return pc.pri->team == team;
    if (gri_->empty_team_slots[team_index] <= 0) return false;
    if (pc.pri->team && pc.pri->team->team_index >= 0 && pc.pri->team->team_index <= 1)
        gri_->empty_team_slots[pc.pri->team->team_index]++;
    gri_->empty_team_slots[team_index]--;
    pc.pri->team = team;
    pc.pri->team_index = team_index;
    pc.pri->b_is_spectator = false;
    UpdateTeamMembers();
    return true;
}

void World::UpdateTeamMembers() {
    // TeamInfo member arrays (retail frame 145/190): player IDs, their
    // PlayerStorageProxies, role-selection priority. Rebuilt from scratch so
    // a leaving player's slot clears.
    namespace TI = network::rep::TeamInfo;
    for (int32_t t = 0; t < 2; ++t) {
        ServerTeamInfo* team = FindTeam(t);
        if (!team) continue;
        int32_t n = 0;
        for (const auto& pc : controllers_) {
            if (!pc->pri || pc->pri->team != team) continue;
            team->SetRoundField(TI::aiTeamMemberPlayerIDs, n, DataValue::Int(pc->pri->player_id));
            team->SetRoundField(TI::ListOfProxies, n, DataValue::Actor(pc->proxy));
            team->SetRoundField(TI::aiTeamMemberRoleSelectionPriority, n, DataValue::Int(1));
            ++n;
        }
        for (auto& f : team->round_fields)
            if ((f.idx == TI::aiTeamMemberPlayerIDs || f.idx == TI::aiTeamMemberRoleSelectionPriority ||
                 f.idx == TI::ListOfProxies) && f.elem >= n)
                f.v = f.idx == TI::ListOfProxies ? DataValue::Actor(nullptr) : DataValue::Int(0);
    }
}

void World::LeaveTeam(ServerPlayerReplicationInfo& pri) {
    ReleaseRole(pri);
    struct Rebuild { World* w; ~Rebuild() { w->UpdateTeamMembers(); } } rebuild{this};
    if (gri_ && pri.team && pri.team->team_index >= 0 && pri.team->team_index <= 1)
        gri_->empty_team_slots[pri.team->team_index]++;
    pri.team = nullptr;
    pri.team_index = -1;
}

size_t World::PlayersOnTeams() const {
    size_t n = 0;
    for (const auto& pc : controllers_) if (pc->pri && pc->pri->OnPlayableTeam()) ++n;
    return n;
}

void World::RecountReadyVotes() {
    if (!gri_) return;
    int32_t votes = 0;
    for (const auto& pc : controllers_) if (pc->pri && pc->pri->OnPlayableTeam() && pc->ready) ++votes;
    gri_->ready_votes = votes;
    gri_->ready_votes_required = std::max<int32_t>(1, static_cast<int32_t>(PlayersOnTeams()));
}

void World::SetPlayerReady(ServerPlayerController& pc, bool ready) {
    if (phase_ != MatchPhase::kRoleSelection) return;   // see EnterPhase
    pc.ready = ready;
    if (pc.pri) pc.pri->vote_ready = ready;   // PRI.bVoteReady (retail frame 234)
    RecountReadyVotes();
}

void World::EnterPhase(MatchPhase phase) {
    static const char* kNames[] = {"PendingMatch", "MissionPrePlanning_RoleSelection",
                                   "MissionPrePlanning_DeploymentWarning", "MatchInProgress",
                                   "MatchInProgress", "ShowScoreboard"};
    phase_ = phase;
    ++phase_serial_;
    phase_clock_ = 0.0;
    second_clock_ = 0.0;
    if (gri_) {
        // MatchEnded isn't replicated: MPStateName stays 'MatchInProgress'
        // until the reset (retail t=227.37-229.37; doc sec 36).
        if (phase != MatchPhase::kRoundOver) gri_->mp_state_name = kNames[static_cast<int>(phase)];
        switch (phase) {
        case MatchPhase::kRoleSelection:
            // Only ready votes cast during role selection count: clients
            // also send ServerSetPlayerReady from the pre-join menu.
            for (auto& pc : controllers_) {
                pc->ready = false;
                if (pc->pri) pc->pri->vote_ready = false;
            }
            RecountReadyVotes();
            gri_->role_selection_time_remaining = gri_->role_selection_time;
            gri_->deployment_time_remaining = gri_->deployment_time;
            gri_->battle_planning_time_remaining = 0;
            gri_->b_allow_preplanning = true;
            break;
        case MatchPhase::kDeploymentWarning:
            gri_->role_selection_time_remaining = 0;
            break;
        case MatchPhase::kInProgress:
            ResetRoundStats();                 // doc sec 45
            for (auto& c : controllers_)       // retail t=21.19 / 89.23: iMatchRoundsPlayed 1, 2
                if (c->pri && c->pri->OnPlayableTeam()) ++c->pri->rounds_played;
            // Retail frames 368/438: bMatchHasBegun, MPStateName
            // 'MatchInProgress', the mission time limit (420s on Bridge
            // classic) and its countdown, respawn counts.
            gri_->deployment_time_remaining = 0;
            gri_->b_match_has_begun = true;
            if (time_limit_seconds_ <= 0)
                time_limit_seconds_ = gri_->mission_time_remaining > 0
                    ? gri_->mission_time_remaining : game_info_->default_time_limit_seconds;
            gri_->mission_time_limit_seconds = time_limit_seconds_;
            gri_->mission_time_remaining = time_limit_seconds_;
            // GRI's own clock (TimeLimit in minutes, RemainingTime) follows a
            // configured limit too, as retail's 7 / 420 follow Bridge's 420.
            gri_->time_limit = time_limit_seconds_ / 60;
            gri_->remaining_time = time_limit_seconds_;
            SpawnNPCs();
            gri_->respawn_players_needed = {1, 1};
            had_valid_players_ = {false, false};
            break;
        default:
            break;
        }
    }
    LOG_INFO("Match phase -> %s (round %d)", phase == MatchPhase::kRoundOver ? "MatchEnded"
             : kNames[static_cast<int>(phase)], gri_ ? gri_->i_current_round : 0);
}

void World::UpdateMatch(double dt) {
    if (!gri_) return;
    phase_clock_ += dt;
    second_clock_ += dt;
    bool tick = second_clock_ >= 1.0;
    if (tick) second_clock_ -= 1.0;
    switch (phase_) {
    case MatchPhase::kPendingMatch:
        // Retail started role selection ~0.8s after the first player joined a
        // team, even alone (iMinPlayerCount=2 only stops stats being saved).
        if (PlayersOnTeams() == 0) phase_clock_ = 0.0;
        else if (phase_clock_ >= 0.8) {
            gri_->role_selection_time = role_selection_seconds_;
            RecountReadyVotes();
            EnterPhase(MatchPhase::kRoleSelection);
        }
        break;
    case MatchPhase::kRoleSelection:
        if (tick && gri_->role_selection_time_remaining > 0) --gri_->role_selection_time_remaining;
        RecountReadyVotes();
        if (gri_->role_selection_time_remaining <= 0 ||
            (gri_->ready_votes > 0 && gri_->ready_votes >= gri_->ready_votes_required))
            EnterPhase(MatchPhase::kDeploymentWarning);
        break;
    case MatchPhase::kDeploymentWarning:
        if (tick && gri_->deployment_time_remaining > 0) --gri_->deployment_time_remaining;
        if (gri_->deployment_time_remaining <= 0) EnterPhase(MatchPhase::kInProgress);
        break;
    case MatchPhase::kInProgress:
        UpdateReinforcements();
        // Also covers players who join a team after the round started.
        SpawnPlayers();
        // Retail t=23.83: bStopCountDown False ~1s after MatchInProgress.
        if (gri_->b_stop_count_down && phase_clock_ >= 1.0) gri_->b_stop_count_down = false;
        if (tick && gri_->mission_time_remaining > 0) --gri_->mission_time_remaining;
        CheckRoundEnd(tick);
        break;
    case MatchPhase::kRoundOver:
        // AA_GameInfo.MatchEnded: SetTimer(2.0, false, 'ResetLevel').
        if (phase_clock_ >= 2.0) ResetLevel();
        break;
    case MatchPhase::kShowScoreboard:
        // iScoreboardDisplayTimeSeconds + iEndRoundAARDisplayTimeSeconds
        // (5 + 10), then AA_MCMissionManager.PrepStartMatch.
        if (phase_clock_ < 15.0) break;
        if (match_over_) {
            // Retail would travel to the next map in the rotation; this
            // server restarts the match on the same one.
            match_over_ = false;
            gri_->i_current_round = 1;
            gri_->team_scores = {};
            gri_->last_round_tied = false;
            swapped_ = false;
            LOG_INFO("Match over: starting a new match");
        }
        if (PlayersOnTeams() == 0) { EnterPhase(MatchPhase::kPendingMatch); break; }
        // Roles carry over (IsRoleSelectRound: iRoleSelectionTime 1), except
        // in a new match and on the swap round, where the sides' roles
        // change (retail round 4: 10s).
        if (gri_->i_current_round == swap_round_ && !swapped_) swapped_ = true;
        gri_->b_team_side_swapping_round = swapped_;
        gri_->role_selection_time =
            (gri_->i_current_round == 1 || gri_->i_current_round == swap_round_) ? role_selection_seconds_ : 1;
        RecountReadyVotes();
        EnterPhase(MatchPhase::kRoleSelection);
        break;
    }
}

int32_t World::TeamOf(const ServerPlayerController& pc) {
    return pc.pri && pc.pri->team ? pc.pri->team->team_index : -1;
}

// AA_MCMissionManager.IsTeamStillAlive: someone on the team has a pawn and
// isn't incapacitated (dead, waiting and downed players don't count). With
// RespawnDelay set, a player waiting to respawn keeps the team in it too.
bool World::TeamStillAlive(int32_t team) const {
    for (const auto& pc : controllers_) {
        if (TeamOf(*pc) != team) continue;
        if (pc->pawn && !pc->pawn->torn_off && !pc->pawn->incapacitated) return true;
        if (respawn_delay_ > 0 && pc->died && pc->respawn_at >= 0) return true;
    }
    return false;
}

void World::CheckRoundEnd(bool second_tick) {
    if (phase_clock_ < 2.0) return;   // the round's soldiers spawn first
    // Extraction (AA_ExtractionVolume.HandlePawnEntry): enough of the
    // assault team's soldiers inside the volume -- retail Bridge classic
    // needs one -- wins at once.
    for (const auto& rule : extraction_rules_) {
        const int32_t team = ExtractingTeam(rule.team);
        int32_t inside = 0;
        ServerPawn* first = nullptr;
        for (const auto& pc : controllers_) {
            ServerPawn* p = pc->pawn;
            if (TeamOf(*pc) != team || !p || p->torn_off || p->incapacitated) continue;
            if (rule.carry && p != carry_.carrier) continue;   // only the briefcase's carrier extracts
            for (const auto& b : rule.boxes) {
                if (p->location[0] >= b[0] && p->location[1] >= b[1] && p->location[2] >= b[2] &&
                    p->location[0] <= b[3] && p->location[1] <= b[4] && p->location[2] <= b[5]) {
                    if (!first) first = p;
                    ++inside;
                    break;
                }
            }
        }
        if (inside > 0 && inside >= std::max<int32_t>(1, rule.min_players)) {
            EndRound(team, "extraction", first, rule.objective);
            return;
        }
    }
    if (!second_tick) return;
    // Time limit (AA_MCTimeLimitEndOfMatchSeqEvent): the defending team wins.
    if (gri_->mission_time_remaining <= 0) {
        int32_t defenders = -1;
        if (!extraction_rules_.empty()) defenders = 1 - ExtractingTeam(extraction_rules_[0].team);
        else if (defending_team_ >= 0) defenders = swapped_ ? 1 - defending_team_ : defending_team_;
        EndRound(defenders, "time limit");
        return;
    }
    // Team death (AA_GameInfo MatchInProgress.Timer + CheckForWinningTeam):
    // a team that had soldiers this round and has none left loses; nobody
    // left on either side is a tie.
    // Co-op reinforcements don't change this: retail ends the round as soon
    // as a team has nobody up, even with respawn points (Bridge AI bot run,
    // 2026-10-12); F5 helps only while a teammate is alive. A player waiting
    // in the reinforcement queue doesn't count as alive.
    const bool alive0 = TeamStillAlive(0), alive1 = TeamStillAlive(1);
    if (alive0) had_valid_players_[0] = true;
    if (alive1) had_valid_players_[1] = true;
    // A side with no players at all (the enemy NPCs' side in an AI
    // mission) isn't "down": the players going down lose (retail Bridge AI).
    if (!alive0 && !alive1 && had_valid_players_[0] != had_valid_players_[1])
        EndRound(had_valid_players_[0] ? 1 : 0, "team down vs AI");
    else if (!alive0 && !alive1) EndRound(-1, "both teams down");
    else if (!alive0 && had_valid_players_[0]) EndRound(1, "team 0 down");
    else if (!alive1 && had_valid_players_[1]) EndRound(0, "team 1 down");
}

// Round end as retail (AA_MCWinConditionsSeqAct.Activated ->
// ProcessEndOfGame -> WriteEndOfGameStats -> AA_GameInfo.MatchEnded;
// GamePlayPCAP_Legit_Server t=81.89 / 367.91, live capture t=227.34):
// GRI bStopCountDown, bMatchIsOver, atsTeamScores, bLastRoundTied and the
// winning objective; every living soldier's controller lets go of it
// (GameHasEnded -> UnPossess) and spectates.
void World::EndRound(int32_t winner, const char* reason, ServerPawn* completed_by,
                     const std::string& objective) {
    gri_->b_stop_count_down = true;
    gri_->b_match_is_over = true;
    if (completed_by && completed_by->controller) {        // the extraction (doc sec 45)
        ServerPlayerController& cpc = *completed_by->controller;
        ++cpc.live_round.iObjectivesCompleted;
        ++cpc.live_round.iCarriedObjectiveExtraction;
        if (cpc.pri) ++cpc.pri->objectives_completed;
    }
    PublishRoundStats(winner);
    if (winner == 0 || winner == 1) {
        gri_->team_scores[static_cast<size_t>(winner)].won++;
        gri_->team_scores[static_cast<size_t>(1 - winner)].lost++;
    } else {
        for (auto& t : gri_->team_scores) t.tied++;
    }
    gri_->last_round_tied = winner < 0;
    gri_->winning_objective = objective;
    gri_->winning_action_player = completed_by && completed_by->pri ? completed_by->pri->player_name : "";
    if (!objective.empty()) {
        constexpr uint32_t kCompletedTeam = 29, kCompleted = 31;   // AA_MCObjectiveMarker
        for (auto& a : join_actors_)
            if (a->class_name == "AA_MCExtractionObjectiveMarker") {
                a->Set(kCompletedTeam, -1, DataValue::Int(winner));
                a->Set(kCompleted, -1, DataValue::Bool(true));
            }
    }
    for (auto& pc : controllers_) {
        if (!pc->pri || !pc->pri->OnPlayableTeam()) continue;
        if (ServerPawn* p = pc->pawn) {
            p->unpossessed = true;
            p->last_known_pri = p->pri;
            p->pri = nullptr;
            p->controller = nullptr;
            p->trigger_held = false;
            p->burst_left = 0;
            p->holding_breath = false;
            pc->pawn = nullptr;
        }
        pc->collide_world = true;
        pc->spectator_mode = 1;
        pc->right_spectate_info = "Next View Point";
        pc->left_spectate_info = "Previous Player";
        if (pc->proxy) {
            pc->proxy->is_dead = true;
            pc->proxy->is_spectating = true;
            pc->proxy->incapacitated = false;
        }
        pc->pri->b_incapacitated = false;
    }
    reloads_.clear();
    deliberate_reloads_.clear();
    round_winner_ = winner;
    finished_round_ = gri_->i_current_round;
    match_over_ = gri_->i_current_round >= gri_->i_max_rounds_per_match;
    // AA_MCMissionManager.IsNextRoundSwapping.
    next_round_swaps_ = gri_->i_current_round + 1 != gri_->i_max_rounds_per_match &&
                        gri_->i_current_round + 1 == swap_round_;
    LOG_INFO("Round %d over (%s): %s", finished_round_, reason,
             winner < 0 ? "tie" : (winner == 0 ? "team 0 wins" : "team 1 wins"));
    EnterPhase(MatchPhase::kRoundOver);
}

// AA_GameInfo.MatchEnded.ResetLevel, 2s after the round ended (retail
// t=229.31): every soldier goes, the level's actors go back to their
// round-start state (doors IncMatchReset + 'Latched', windows whole, the
// objective open), the round counter moves on and the scoreboard shows.
void World::ResetLevel() {
    for (auto& p : pawns_) p->destroyed = true;
    for (auto& pc : controllers_) {
        pc->pawn = nullptr;
        pc->died = false;
        pc->played_in_round = false;
        pc->late_joiner = false;
        pc->late_join_at = -1.0;
        pc->respawn_at = -1.0;
        pc->collide_world = false;
        if (pc->pri) pc->pri->b_dead = false;
    }
    reloads_.clear();
    deliberate_reloads_.clear();
    door_timers_.clear();
    for (auto& g : thrown_) {                  // grenades still out: gone with the level
        g.gone = true;
        g.drop_at = clock_ + 10.0;
    }
    ++match_resets_;
    constexpr uint32_t kIncMatchReset = 28, kStateName = 30;            // AA_Door
    constexpr uint32_t kCollideActors = 10, kShatteredFlags = 23, kCrackedFlags = 24;
    for (auto& a : map_actors_) {
        if (a->class_name == "AA_Door") {
            DataValue latched; latched.kind = DataValue::Kind::kName; latched.s = "Latched";
            a->Set(kIncMatchReset, -1, DataValue::Int(match_resets_));
            a->Set(kStateName, -1, latched);
        } else if (a->class_name == "AA_BreakableWindow" && a->Get(kCollideActors, -1)) {
            a->Set(kCollideActors, -1, DataValue::Bool(true));
            collision_.SetOwnerEnabled(a->cdo_net_index + 1, true);
        }
    }
    window_hits_.clear();
    ResetCarriedObjective();
    for (auto& a : world_items_) a->gone = true;      // dropped rifles go with the level
    item_timers_.clear();
    for (auto& q : respawn_queue_) q.clear();
    next_wave_at_ = -1.0;
    for (auto& a : join_actors_) {
        if (a->class_name == "AA_BreakableWindowManager") {
            for (auto& f : a->fields)
                if (f.idx == kShatteredFlags || f.idx == kCrackedFlags) f.v = DataValue::Int(0);
        } else if (a->class_name == "AA_MCExtractionObjectiveMarker" && a->Get(31, -1)) {
            a->Set(29, -1, DataValue::Int(-1));
            a->Set(31, -1, DataValue::Bool(false));
        }
    }
    // ES2 objects: retail closes them at the next match start and opens a
    // fresh set (t=255.34).
    for (size_t i = 0; i < match_actors_.size() && i < match_actor_start_.size(); ++i) {
        match_actors_[i]->fields = match_actor_start_[i];
        match_actors_[i]->gone = false;
    }
    ++match_actor_serial_;
    gri_->b_match_is_over = false;
    gri_->battle_planning_time_remaining = 30;
    if (!match_over_) gri_->i_current_round++;
    EnterPhase(MatchPhase::kShowScoreboard);
}

uint32_t World::StingerIndex(bool victory) const {
    // S_Music.Stingers.S_Music_Stingers_AA3_Victory/Defeat_WB_USoundCue
    // (AA_MCWinConditionsSeqAct.PlayVictorySound): NetIndex 17 / 16, retail
    // <obj 164222> / <obj 164221>.
    return ObjectIndex("S_Music", victory ? 17 : 16);
}

uint32_t World::RandomViewpointIndex() const {
    if (viewpoints_.empty()) return 0;
    return ObjectIndex(map_name_, viewpoints_[static_cast<size_t>(std::rand()) % viewpoints_.size()]);
}

bool World::LoadMissionRulesFromJson(const std::string& json) {
    std::string err;
    auto doc = util::ParseJson(json, &err);
    if (!doc) { LOG_ERROR("Bad mission rules JSON: %s", err.c_str()); return false; }
    extraction_rules_.clear();
    for (const auto& e : (*doc)["extraction"].arr) {
        ExtractionRule r;
        r.team = static_cast<int32_t>(e["extracting_team"].Int());
        r.carry = e["carry"].b;
        r.min_players = static_cast<int32_t>(e["min_players"].Int(1));
        r.objective = e["objective"].Str();
        for (const auto& b : e["boxes"].arr) {
            std::array<float, 6> box{};
            for (int k = 0; k < 3; ++k) {
                box[k] = static_cast<float>(b["min"].arr.size() > static_cast<size_t>(k) ? b["min"].arr[k].num : 0);
                box[k + 3] = static_cast<float>(b["max"].arr.size() > static_cast<size_t>(k) ? b["max"].arr[k].num : 0);
            }
            r.boxes.push_back(box);
        }
        extraction_rules_.push_back(r);
    }
    carry_ = CarriedObjective{};
    if ((*doc).Has("carry")) {
        const util::JsonValue& c = (*doc)["carry"];
        carry_.active = true;
        carry_.factory_pkg = c["factory"]["pkg"].Str();
        carry_.factory_net = static_cast<uint32_t>(c["factory"]["net"].Int());
        for (size_t k = 0; k < 3 && k < c["location"].arr.size(); ++k)
            carry_.home[k] = static_cast<float>(c["location"].arr[k].num);
    }
    npc_starts_.clear();
    npc_firing_positions_.clear();
    npc_max_ = 0;
    if ((*doc).Has("npc")) {
        const util::JsonValue& n = (*doc)["npc"];
        npc_max_ = static_cast<int32_t>(n["max"].Int(35));
        for (const auto& s : n["starts"].arr) {
            NpcStart st;
            for (size_t k = 0; k < 3 && k < s["location"].arr.size(); ++k)
                st.loc[k] = static_cast<float>(s["location"].arr[k].num);
            st.yaw = static_cast<int32_t>(s["yaw"].Int());
            st.weapon = s["weapon"].Str();
            for (const auto& i : s["fp"].arr) st.fp.push_back(static_cast<int>(i.num));
            st.only = s["only"].b;
            st.random_spawn = s["random_spawn"].b;
            st.frightened = s["frightened"].b;
            st.sight = static_cast<int32_t>(s["sight"].Int(19200));
            npc_starts_.push_back(st);
        }
        npc_fog_ = static_cast<float>(n["fog"].num);
        // [x, y, z] in older bundles; now [x, y, z, yaw, flags, cover0..15].
        for (const auto& f : n["firing_positions"].arr) {
            if (f.arr.size() < 3) continue;
            FiringPos fp;
            for (int k = 0; k < 3; ++k) fp.loc[k] = static_cast<float>(f.arr[static_cast<size_t>(k)].num);
            if (f.arr.size() >= 5) {
                fp.yaw = static_cast<int32_t>(f.arr[3].num);
                fp.flags = static_cast<int>(f.arr[4].num);
            }
            for (size_t d = 0; d < 16 && 5 + d < f.arr.size(); ++d) fp.cover[d] = static_cast<uint8_t>(f.arr[5 + d].num);
            npc_firing_positions_.push_back(fp);
        }
        LOG_INFO("Enemy NPCs: %zu starts, %zu firing positions, cap %d", npc_starts_.size(),
                 npc_firing_positions_.size(), npc_max_);
    }
    respawn_markers_.clear();
    for (const auto& r : (*doc)["respawn_markers"].arr) {
        RespawnMarker m;
        m.team = static_cast<int32_t>(r["team"].Int());
        m.enabled = !r.Has("enabled") || r["enabled"].b;
        for (size_t k = 0; k < 3 && k < r["location"].arr.size(); ++k)
            m.loc[k] = static_cast<float>(r["location"].arr[k].num);
        m.yaw = static_cast<int32_t>(r["yaw"].Int());
        respawn_markers_.push_back(m);
    }
    viewpoints_.clear();
    for (const auto& v : (*doc)["viewpoints"].arr) viewpoints_.push_back(static_cast<uint32_t>(v.Int()));

    // Mission metadata the client's menus show (AA_MCMissionInfoConnector /
    // AA_MCTeamInfo of the gametype level; aa3_mapbake).
    const util::JsonValue& m = (*doc)["mission"];
    if (gri_ && m.Has("name")) {
        gri_->s_mission_name = m["name"].Str();
        gri_->s_author = m["author"].Str();
        gri_->s_type = m["type"].Str();
        gri_->s_summary = m["summary"].Str();
        gri_->min_player_count = static_cast<int32_t>(m["min_players"].Int(gri_->min_player_count));
        gri_->max_player_count = static_cast<int32_t>(m["max_players"].Int(gri_->max_player_count));
        gri_->mission_time_seconds = static_cast<int32_t>(m["mission_time_seconds"].Int(gri_->mission_time_seconds));
    }
    defending_team_ = -1;
    for (const auto& t : (*doc)["teams"].arr) {
        int64_t i = t["index"].Int(-1);
        if (i < 0 || i > 1) continue;
        const std::string& type = t["mission_type"].Str();
        if (gri_) {
            if (!t["name"].Str().empty()) gri_->team_names[static_cast<size_t>(i)] = t["name"].Str();
            gri_->mission_briefings[static_cast<size_t>(i)] = t["briefing"].Str();
            if (!type.empty()) gri_->mission_types[static_cast<size_t>(i)] = type;
        }
        if (type == "Defend" || type == "Defending" || type == "Ambush") defending_team_ = static_cast<int32_t>(i);
    }
    spawn_markers_.clear();
    for (const auto& sm : (*doc)["spawn_markers"].arr) {
        MarkerSpawns ms;
        ms.net = static_cast<uint32_t>(sm["net"].Int());
        ms.team = static_cast<int32_t>(sm["team"].Int());
        ms.fire_team = static_cast<int32_t>(sm["fire_team"].Int());
        for (const auto& st : sm["starts"].arr) {
            if (st.arr.size() < 4) continue;
            SpawnPoint p{{static_cast<float>(st.arr[0].num), static_cast<float>(st.arr[1].num),
                          static_cast<float>(st.arr[2].num)}, static_cast<int32_t>(st.arr[3].num)};
            ms.starts.push_back(p);
        }
        if (!ms.starts.empty()) spawn_markers_.push_back(std::move(ms));
    }
    LOG_INFO("Mission rules: %zu extraction objective(s), %zu viewpoints, %zu fire team spawn markers%s%s",
             extraction_rules_.size(), viewpoints_.size(), spawn_markers_.size(),
             m.Has("name") ? ", mission " : "", m.Has("name") ? m["name"].Str().c_str() : "");
    return true;
}

bool World::LoadMissionRules(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return LoadMissionRulesFromJson(ss.str());
}

void ServerRoundActor::Set(uint32_t idx, int32_t elem, const DataValue& v) {
    for (auto& f : fields)
        if (f.idx == idx && f.elem == elem) { f.v = v; return; }
    fields.push_back({idx, elem, v});
}

void ServerRoundActor::Touch(uint32_t idx, int32_t elem) {
    for (auto& f : fields)
        if (f.idx == idx && f.elem == elem) { ++f.revision; return; }
}

const DataField* ServerRoundActor::Get(uint32_t idx, int32_t elem) const {
    for (const auto& f : fields)
        if (f.idx == idx && f.elem == elem) return &f;
    return nullptr;
}

uint32_t World::ObjectIndex(const std::string& package, uint32_t net_index) const {
    auto it = package_bases_.find(package);
    return it == package_bases_.end() ? 0 : it->second + net_index;
}

namespace {

// JSON value from extract_round_actors.py -> DataValue. Actor references
// are left as retail channels in `retail_refs` and resolved by the caller.
DataValue ParseDataValue(const util::JsonValue& j, std::vector<std::pair<DataValue*, int64_t>>* retail_refs,
                         DataValue* self = nullptr) {
    DataValue v;
    const std::string& k = j["kind"].Str();
    using K = DataValue::Kind;
    if (k == "int") { v.kind = K::kInt; v.i = j["v"].Int(); }
    else if (k == "float") { v.kind = K::kFloat; v.f = j["v"].num; }
    else if (k == "bool") { v.kind = K::kBool; v.i = j["v"].b; }
    else if (k == "str") { v.kind = K::kStr; v.s = j["v"].Str(); }
    else if (k == "name") { v.kind = K::kName; v.s = j["v"].Str(); v.i = j["num"].Int(); }
    else if (k == "obj") { v.kind = K::kObj; v.s = j["pkg"].Str(); v.net = static_cast<uint32_t>(j["net"].Int()); }
    else if (k == "byte") { v.kind = K::kByte; v.bits = static_cast<uint32_t>(j["bits"].Int()); v.i = j["v"].Int(); }
    else if (k == "vector" || k == "rotator") {
        v.kind = k == "vector" ? K::kVector : K::kRotator;
        for (size_t n = 0; n < 3 && n < j["v"].arr.size(); ++n) v.v3[n] = static_cast<int32_t>(j["v"].arr[n].num);
    } else if (k == "struct") {
        v.kind = K::kStruct;
        for (const auto& m : j["members"].arr) v.members.push_back(ParseDataValue(m, nullptr));
    } else if (k == "raw") {
        v.kind = K::kRaw;
        v.s = j["bits"].Str();
    } else if (k == "actor") {
        v.kind = K::kNone;   // until resolved
        (void)self;
    }
    return v;
}

uint32_t RoundClassMaxIndex(const std::string& cls) {
    namespace rep = network::rep;
    if (cls == "AA_ES2Manager") return rep::ES2Manager::kMaxIndex;
    if (cls == "AA_FireTeamInfo") return rep::FireTeamInfo::kMaxIndex;
    if (cls == "AA_MCClassRoleReplicationInfo") return rep::ClassRoleRI::kMaxIndex;
    return 0;
}

}  // namespace

bool World::ReadActorTemplatesFromJson(const std::string& json, const char* what,
                                       std::vector<ActorTemplate>& out) {
    out.clear();
    std::string err;
    auto doc = util::ParseJson(json, &err);
    if (!doc) {
        LOG_ERROR("Bad %s JSON: %s", what, err.c_str());
        return false;
    }
    for (const auto& ja : (*doc)["actors"].arr) {
        ActorTemplate t;
        ServerRoundActor& a = t.proto;
        a.class_name = ja["class"].Str();
        a.max_index = ja.Has("max_index") ? static_cast<uint32_t>(ja["max_index"].Int())
                                          : RoundClassMaxIndex(a.class_name);
        // Spawned actors name their class default object ("cdo"); placed
        // map actors name themselves ("obj") and open with no spawn data.
        const bool placed = ja.Has("obj");
        const util::JsonValue& ref = placed ? ja["obj"] : ja["cdo"];
        a.cdo_package = ref["pkg"].Str();
        a.cdo_net_index = static_cast<uint32_t>(ref["net"].Int());
        a.object_index = ObjectIndex(a.cdo_package, a.cdo_net_index);
        a.transient = !placed;
        if (ja.Has("location") && ja["location"].arr.size() == 3)   // spawn position (world objects)
            for (int k = 0; k < 3; ++k) a.location[k] = static_cast<float>(ja["location"].arr[k].num);
        a.label = a.class_name;
        if (a.max_index == 0 || a.object_index == 0) {
            LOG_WARN("%s: %s has an unknown class or package, skipped", what, a.class_name.c_str());
            continue;
        }
        for (const auto& jf : ja["fields"].arr) {
            DataField fld;
            fld.idx = static_cast<uint32_t>(jf["idx"].Int());
            fld.elem = jf.Has("elem") ? static_cast<int32_t>(jf["elem"].Int()) : -1;
            if (jf["kind"].Str() == "actor") {
                t.refs.push_back({fld.idx, fld.elem, jf["retail_ch"].Int()});
                continue;
            }
            fld.v = ParseDataValue(jf, nullptr);
            a.fields.push_back(std::move(fld));
        }
        t.retail_ch = ja["retail_ch"].Int();
        out.push_back(std::move(t));
    }
    return true;
}

bool World::ReadActorTemplates(const std::string& path, const char* what,
                               std::vector<ActorTemplate>& out) {
    std::ifstream f(path);
    if (!f) {
        LOG_WARN("No %s file %s", what, path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ReadActorTemplatesFromJson(ss.str(), what, out);
}

std::vector<std::unique_ptr<ServerRoundActor>> World::Instantiate(
        const std::vector<ActorTemplate>& templates,
        const std::map<int64_t, const ServerActor*>& outside) {
    // Copies of every template; references resolve to the copies (by the
    // retail channel each template was captured from) or to `outside`
    // actors (the player's PC/PRI/pawn...); anything else goes out None.
    std::vector<std::unique_ptr<ServerRoundActor>> made;
    std::map<int64_t, const ServerActor*> by_ch = outside;
    for (const auto& t : templates) {
        made.push_back(std::make_unique<ServerRoundActor>(t.proto));
        by_ch[t.retail_ch] = made.back().get();
    }
    for (size_t i = 0; i < templates.size(); ++i)
        for (const auto& r : templates[i].refs) {
            auto it = by_ch.find(r.retail_ch);
            made[i]->Set(r.idx, r.elem, DataValue::Actor(it == by_ch.end() ? nullptr : it->second));
        }
    return made;
}

bool World::LoadRoundActorsFromJson(const std::string& json) {
    round_actors_.clear();
    for (auto& t : role_slots_) t.clear();
    std::vector<ActorTemplate> templates;
    if (!ReadActorTemplatesFromJson(json, "round-actor", templates)) return false;
    // References between round actors (FireTeamInfo.aamcrriClassRoles);
    // anything else (retail's objective marker) goes out as None for now.
    round_actors_ = Instantiate(templates, {});

    // Role list per team: fire teams by iFireTeamID, roles by element. The
    // team comes from the role IDs ("T0F0R1" -> team 0).
    namespace FT = network::rep::FireTeamInfo;
    namespace CR = network::rep::ClassRoleRI;
    std::vector<ServerRoundActor*> fire_teams;
    for (auto& a : round_actors_)
        if (a->class_name == "AA_FireTeamInfo") fire_teams.push_back(a.get());
    auto ft_id = [](const ServerRoundActor* ft) {
        const DataField* f = ft->Get(FT::iFireTeamID);
        return f ? static_cast<int32_t>(f->v.i) : 0;
    };
    std::stable_sort(fire_teams.begin(), fire_teams.end(),
                     [&](const ServerRoundActor* x, const ServerRoundActor* y) { return ft_id(x) < ft_id(y); });
    for (ServerRoundActor* ft : fire_teams) {
        std::vector<std::pair<int32_t, ServerRoundActor*>> members;
        int32_t team = -1;
        for (const auto& fld : ft->fields) {
            if (fld.idx != FT::aamcrriClassRoles || fld.v.kind != DataValue::Kind::kActor) continue;
            auto* role = const_cast<ServerRoundActor*>(static_cast<const ServerRoundActor*>(fld.v.actor));
            members.push_back({fld.elem, role});
            if (const DataField* id = role->Get(CR::sClassRoleID))
                if (id->v.s.size() >= 2 && id->v.s[0] == 'T') team = id->v.s[1] - '0';
        }
        if (team < 0 || team > 1) continue;
        std::sort(members.begin(), members.end());
        for (auto& [elem, role] : members)
            role_slots_[static_cast<size_t>(team)].push_back({role, ft, elem, ft_id(ft), nullptr});
    }
    // Each playable team's role structure on its TeamInfo (retail frame 178/
    // 183): roles in role-list order, fire teams, spawn points = the fire
    // teams' default spawn markers, assault/defend from the mission types.
    namespace TI = network::rep::TeamInfo;
    for (int32_t t = 0; t < 2; ++t) {
        ServerTeamInfo* team = FindTeam(t);
        if (!team) continue;
        team->round_fields.clear();
        const auto& slots = role_slots_[static_cast<size_t>(t)];
        for (size_t r = 0; r < slots.size(); ++r)
            team->SetRoundField(TI::aamcrpiClassRoles, static_cast<int32_t>(r), DataValue::Actor(slots[r].role));
        team->SetRoundField(TI::iTotalClassRoles, -1, DataValue::Int(static_cast<int64_t>(slots.size())));
        std::vector<ServerRoundActor*> fts;
        for (const auto& slot : slots)
            if (std::find(fts.begin(), fts.end(), slot.fire_team) == fts.end()) fts.push_back(slot.fire_team);
        for (size_t f = 0; f < fts.size(); ++f) {
            team->SetRoundField(TI::aaftiFireTeams, static_cast<int32_t>(f), DataValue::Actor(fts[f]));
            if (const DataField* m = fts[f]->Get(FT::aamcfsmDefaultSpawnMarker))
                team->SetRoundField(TI::aamcfsmPlayerSelectable, static_cast<int32_t>(f), m->v);
        }
        const std::string type = gri_ ? gri_->mission_types[static_cast<size_t>(t)] : "";
        bool assault = !(type == "Defend" || type == "Defending" || type == "Ambush");
        team->SetRoundField(assault ? TI::bTeamAssault : TI::bTeamDefend, -1, DataValue::Bool(true));
        team->SetRoundField(assault ? TI::bTeamAssaultNextRound : TI::bTeamDefendNextRound, -1,
                            DataValue::Bool(true));
    }
    LOG_INFO("Round actors: %zu; role slots: team 0 = %zu, team 1 = %zu",
             round_actors_.size(), role_slots_[0].size(), role_slots_[1].size());
    ++round_serial_;
    return true;
}

bool World::LoadRoundActors(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return LoadRoundActorsFromJson(ss.str());
}

void World::ReleaseRole(ServerPlayerReplicationInfo& pri) {
    namespace FT = network::rep::FireTeamInfo;
    namespace CR = network::rep::ClassRoleRI;
    for (auto& team : role_slots_)
        for (auto& slot : team) {
            if (slot.owner != &pri) continue;
            slot.owner = nullptr;
            slot.role->Set(CR::iOwningPlayerID, -1, DataValue::Int(0));
            slot.role->Set(CR::sOwningPlayerName, -1, DataValue::Str(""));
            slot.role->Set(CR::priOwningRole, -1, DataValue::Actor(nullptr));
            slot.fire_team->Set(FT::aapriFireTeamMembers, slot.member_index, DataValue::Actor(nullptr));
            slot.fire_team->Set(FT::aapriPendingFireTeamMembers, slot.member_index, DataValue::Actor(nullptr));
            ++round_serial_;
        }
    pri.assigned_class_role_index = -1;
    pri.current_class_role_id.clear();
    pri.class_role_name.clear();
    pri.active_fire_team = nullptr;
    pri.is_fire_team_leader = false;
}

bool World::AssignRole(ServerPlayerController& pc, int32_t role_index) {
    namespace FT = network::rep::FireTeamInfo;
    namespace CR = network::rep::ClassRoleRI;
    if (!pc.pri || !pc.pri->OnPlayableTeam()) return false;
    // Retail (AA_GameInfo DeploymentWarning, GRI.bRoleSwappingDisabled):
    // swaps are refused only while the deployment countdown runs. Mid-round a
    // live soldier keeps his kit; the new role's applies at his next spawn.
    if (phase_ == MatchPhase::kDeploymentWarning) return false;
    int32_t team = pc.pri->team->team_index;
    if (team < 0 || team > 1 || role_index < 0 ||
        role_index >= static_cast<int32_t>(role_slots_[static_cast<size_t>(team)].size()))
        return false;
    RoleSlot& slot = role_slots_[static_cast<size_t>(team)][static_cast<size_t>(role_index)];
    if (slot.owner && slot.owner != pc.pri) return false;
    ReleaseRole(*pc.pri);
    ServerPlayerReplicationInfo& pri = *pc.pri;
    slot.owner = &pri;

    // What retail changes on a pick (frame 204 and the role/fire-team
    // channels): the slot names its owner, the fire team lists the member,
    // the PRI takes the role's identity and kit.
    slot.role->Set(CR::iOwningPlayerID, -1, DataValue::Int(pri.player_id));
    slot.role->Set(CR::sOwningPlayerName, -1, DataValue::Str(pri.player_name));
    slot.role->Set(CR::priOwningRole, -1, DataValue::Actor(&pri));
    slot.fire_team->Set(FT::aapriFireTeamMembers, slot.member_index, DataValue::Actor(&pri));
    slot.fire_team->Set(FT::aapriPendingFireTeamMembers, slot.member_index, DataValue::Actor(&pri));
    ++round_serial_;

    auto str = [&](uint32_t idx) {
        const DataField* f = slot.role->Get(idx);
        return f ? f->v.s : std::string();
    };
    auto flag = [&](uint32_t idx) {
        const DataField* f = slot.role->Get(idx);
        return f && f->v.i != 0;
    };
    bool squad_leader = flag(CR::bSquadLeader);
    pri.assigned_class_role_index = role_index;
    pri.current_class_role_id = str(CR::sClassRoleID);
    pri.is_fire_team_leader = flag(CR::bFireTeamLeader);
    // Retail's PRI.sClassRoleName for Alpha's Fireteam Leader is "FTL0".
    pri.class_role_name = squad_leader ? "SL"
                        : pri.is_fire_team_leader ? "FTL" + std::to_string(slot.fire_team_id)
                                                  : str(CR::sClassRoleName);
    pri.active_fire_team = slot.fire_team;
    pri.has_had_role_assigned = true;
    if (const DataField* w = slot.role->Get(CR::wpnPrimaryOne); w && w->v.kind == DataValue::Kind::kObj) {
        pri.primary_weapon_pkg = w->v.s;
        pri.primary_weapon_net = w->v.net;
    }
    if (const DataField* ammo = slot.role->Get(CR::iPrimaryWeaponOneAmmo))
        pri.primary_one_ammo = static_cast<int32_t>(ammo->v.i);
    // Default mod per rail = element 0 of the role's allowed list.
    const uint32_t rails[6] = {CR::aawmbPrimaryOneMainRail, CR::aawmbPrimaryOneTopRail,
                               CR::aawmbPrimaryOneLeftRail, CR::aawmbPrimaryOneRightRail,
                               CR::aawmbPrimaryOneBottomRail, CR::aawmbPrimaryOneRearSightRail};
    for (size_t r = 0; r < 6; ++r) {
        const DataField* m = slot.role->Get(rails[r], 0);
        pri.primary_mods[r] = m && m->v.kind == DataValue::Kind::kObj
                                  ? std::make_pair(m->v.s, m->v.net) : std::make_pair(std::string(), 0u);
    }
    LOG_INFO("Player %s takes role %d: %s (%s)", pri.player_name.c_str(), role_index,
             pri.current_class_role_id.c_str(), str(CR::sClassRoleName).c_str());
    return true;
}

void World::MaybeLateJoin(ServerPlayerController& pc) {
    // Retail ServerSelectClassRole_ByIndex: a player without a pawn who
    // hasn't played this round volunteers 5 s after picking a role.
    if (phase_ != MatchPhase::kInProgress || pc.pawn || !pc.pri || !SupportsRespawning() ||
        pc.played_in_round || pc.late_joiner || pc.late_join_at >= 0.0)
        return;
    pc.late_join_at = clock_ + 5.0;
    ++pc.late_join_serial;
    LOG_INFO("%s qualifies to join late", pc.pri->player_name.c_str());
}

void World::SetRoundTimes(int32_t role_selection_seconds, int32_t deployment_seconds) {
    if (!gri_) return;
    if (role_selection_seconds > 0) gri_->role_selection_time = role_selection_seconds;
    role_selection_seconds_ = gri_->role_selection_time;
    if (deployment_seconds > 0) gri_->deployment_time = deployment_seconds;
}

void World::SetSpawnPoint(int32_t team_index, float x, float y, float z, int32_t yaw) {
    if (team_index < 0 || team_index > 1) return;
    SpawnPoint& p = spawn_points_[static_cast<size_t>(team_index)];
    p.loc[0] = x; p.loc[1] = y; p.loc[2] = z; p.yaw = yaw;
}

// Per-role weapon kits, rail mods and grenades: src/game/weapons.cpp.

const World::SpawnPoint* World::FireTeamSpawn(const ServerPlayerController& pc) const {
    if (spawn_markers_.empty() || !pc.pri || !pc.pri->team) return nullptr;
    const int32_t team = pc.pri->team->team_index;
    if (team < 0 || team > 1) return nullptr;
    const int32_t side = swapped_ ? 1 - team : team;
    int32_t fire_team = 0, member = 0;
    const auto& slots = role_slots_[static_cast<size_t>(team)];
    const int32_t role = pc.pri->assigned_class_role_index;
    if (role >= 0 && static_cast<size_t>(role) < slots.size()) {
        fire_team = slots[static_cast<size_t>(role)].fire_team_id;
        member = slots[static_cast<size_t>(role)].member_index;
    }
    const MarkerSpawns* best = nullptr;
    for (const auto& m : spawn_markers_) {
        if (m.team != side) continue;
        if (m.fire_team == fire_team) { best = &m; break; }
        if (!best) best = &m;
    }
    if (!best) return nullptr;
    return &best->starts[static_cast<size_t>(member) % best->starts.size()];
}

ServerPawn* World::SpawnPawn(ServerPlayerController& pc) {
    if (pc.pri && pc.pri->OnPlayableTeam() && !pc.pawn) pc.played_in_round = true;
    if (!pc.pri || !pc.pri->OnPlayableTeam() || pc.pawn) return pc.pawn;
    const int32_t team = pc.pri->team->team_index;
    const SpawnPoint* fsp = FireTeamSpawn(pc);
    SpawnPoint reinforcement{};
    if (pc.has_respawn_spot) {                  // a co-op reinforcement point
        for (int k = 0; k < 3; ++k) reinforcement.loc[k] = pc.respawn_spot[k];
        reinforcement.yaw = pc.respawn_spot_yaw;
        fsp = &reinforcement;
        pc.has_respawn_spot = false;
        pc.reinforcement_spawn = true;
    } else {
        pc.reinforcement_spawn = false;
    }
    const SpawnPoint& sp = fsp ? *fsp : spawn_points_[static_cast<size_t>(swapped_ ? 1 - team : team)];
    auto pawn = std::make_unique<ServerPawn>();
    pawn->object_index = BaselineIndex(net_index::kPawn);
    pawn->transient     = true;
    pawn->health        = 100.0f;
    pawn->spawned_at    = clock_;
    pawn->pri           = pc.pri;
    pawn->controller    = &pc;
    pawn->label         = "Pawn:" + pc.pri->player_name;
    for (int i = 0; i < 3; ++i) pawn->location[i] = sp.loc[i];
    pawn->rotation_yaw  = sp.yaw;
    pawn->proxy         = pc.proxy;
    pawn->body_soldier_name = pc.pri->player_name;
    // Team marks for AA3's friend/enemy swap (doc sec 38): every client draws
    // a soldier whose iOriginalOwningTeamIndex isn't its own team as the
    // enemy force; iBodyTeamIndex is the same for the body after death
    // (AA_MCMissionManager.RestartPlayer).
    pawn->original_owning_team = pc.pri->team->team_index;
    pawn->body_team_index = pc.pri->team->team_index;
    pc.pawn = pawn.get();
    // Spawn inventory (doc sec 30): copies of retail's, references mapped
    // from retail's channels to this player's actors -- 2 PC, 10 PRI,
    // 18 PlayerStorageProxy, 159 Pawn (GamePlayPCAP_Legit_Server).
    if (!inventory_template_.empty()) {
        auto items = Instantiate(inventory_template_,
                                 {{2, &pc}, {10, pc.pri}, {18, pc.proxy}, {159, pc.pawn}});
        for (const auto& it : items) {
            if (it->class_name == "AA_InventoryManager") pc.pawn->inv_manager = it.get();
            if (it->class_name == "AA_InventoryContainer_Pouch_DumpPouch") {
                pc.pawn->dump_pouch = it.get();
                pc.pawn->next_mag_pouch = it.get();     // retail: both = the dump pouch (doc sec 31)
            }
        }
        ApplyRoleKit(pc, items);
        // The kit's own team mark (retail spawns the rifle with the owner's
        // TeamIndex; the template is retail's Black kit, team 0).
        for (auto& it : items)
            if (it->Get(0)) it->Set(0, -1, DataValue::Int(pc.pri->team->team_index));
        pawn_inventory_[pc.pawn] = std::move(items);
    }
    pawns_.push_back(std::move(pawn));
    if (pc.proxy) { pc.proxy->is_dead = false; pc.proxy->is_spectating = false; }
    pc.collide_world = false;
    LOG_INFO("Spawned %s (%s) at (%.0f, %.0f, %.0f)", pc.pri->player_name.c_str(),
             pc.pri->current_class_role_id.c_str(), sp.loc[0], sp.loc[1], sp.loc[2]);
    return pc.pawn;
}

void World::SpawnPlayers() {
    // Everyone on Black/Gold gets a soldier at match start; anyone who never
    // picked a role takes the first free slot of their team.
    for (auto& pc : controllers_) {
        if (!pc->pri || !pc->pri->OnPlayableTeam() || pc->pawn) continue;
        if (pc->died) {
            if (pc->respawn_at < 0 || clock_ < pc->respawn_at) continue;
            pc->died = false;
            pc->pri->b_dead = false;
        }
        if (pc->pri->assigned_class_role_index < 0) {
            const auto& slots = role_slots_[static_cast<size_t>(pc->pri->team->team_index)];
            for (size_t r = 0; r < slots.size(); ++r)
                if (!slots[r].owner && AssignRole(*pc, static_cast<int32_t>(r))) break;
        }
        SpawnPawn(*pc);
    }
}

// Vault/mantle as retail (live capture 2026-10-09, t=157.87): the client
// sends the target it computed; the server sets bAdvMvmt_MovingToTarget, and
// 0.27 s later VaultMantleHeight, MovingToTarget off,
// ClientFinishVaultMantleMoveToTarget(), body graph {106}, gravity off; at
// +1.47 s graph {7} and gravity back on.
//
// A ladder mounted from the top starts with the same move, its callback
// FinishLadderMoveToTarget (retail Bridge AI capture 2026-10-12, 13:37:53):
// 0.14 s later MovingToTarget off and ClientFinishLadderMoveToTarget(), no
// vault states; the client then sends ServerClimbLadder.
void World::StartMoveToTarget(ServerPawn* pawn, const float target[3], bool ladder) {
    if (!pawn || pawn->torn_off || pawn->incapacitated) return;
    pawn->moving_to_target = true;
    pawn->move_to_ladder = ladder;
    for (int k = 0; k < 3; ++k) pawn->vault_target[k] = target[k];
    pawn->vault_finish_at = clock_ + (ladder ? 0.14 : 0.27);
}

// AA_Pawn.TryVaultOrMantle / GetObstacleHeight / AdvMvmt_StepOverObstacle.
// Retail finds the obstacle with its AdvMvmt_VaultMantleVolumes; we have
// the map's geometry instead: a trace ahead at knee height (130 units, 390
// sprinting) finds its face, one down from above just inside it finds the
// top. Height over the feet (AA_Pawn defaults): 16-40.12 vault, 40.13-56
// mantle, 55.1-87.5 climb. No headroom on top (a trace up 2 x 44): the
// crouched vault/mantle. Sprinting into a vault or mantle keeps the sprint
// (AdvMvmt_bSprintedIntoVaultMantle): the combat step / hood slide.
// Animation lengths (Sold_Anims): vault 0.63 s (crouched 0.77), mantle
// 0.63, climb 1.2 (retail t=157.87: {106} .. {7} 1.2 s on), hood slide
// 0.5 + 0.47 (retail t=347.65: {165}, {164} +0.48), combat step 0.37.
bool World::TryVaultOrMantle(ServerPawn* pawn) {
    if (!pawn || pawn->torn_off || pawn->incapacitated) return false;
    ServerPawn& p = *pawn;
    if (p.crouched || p.prone || p.sights || p.on_ladder) return false;
    if (p.vault_busy_until >= 0.0 && clock_ < p.vault_busy_until) return false;
    const float yaw_u = static_cast<float>((p.controller ? p.controller->rotation_yaw : p.rotation_yaw) & 0xFFFF);
    const float yaw = yaw_u * 3.14159265f / 32768.0f;
    const Vec3 dir{std::cos(yaw), std::sin(yaw), 0.0f};
    const float foot = p.location[2] - 44.0f;
    const float reach = p.sprinting ? 390.0f : 130.0f;
    TraceHit face;
    for (float up : {10.0f, 25.0f, 45.0f}) {
        const Vec3 a{p.location[0], p.location[1], foot + up};
        face = collision_.LineTrace(a, Vec3{a.x + dir.x * reach, a.y + dir.y * reach, a.z},
                                    CollisionWorld::kBlockTrace);
        if (face.hit) break;
    }
    float height = -1.0f;
    float top = 0.0f;
    if (face.hit) {
        Vec3 n{face.normal.x, face.normal.y, 0.0f};
        const float nl = std::hypot(n.x, n.y);
        n = nl > 0.01f ? Vec3{n.x / nl, n.y / nl, 0.0f} : Vec3{-dir.x, -dir.y, 0.0f};
        for (float in : {10.0f, 5.0f, 3.0f}) {
            const Vec3 from{face.location.x - n.x * in, face.location.y - n.y * in, foot + 97.5f};
            const TraceHit down = collision_.LineTrace(from, Vec3{from.x, from.y, foot - 10.0f}, CollisionWorld::kBlockTrace);
            if (!down.hit) continue;
            if (down.location.z >= from.z - 0.5f) break;          // a wall: taller than any climb
            top = down.location.z;
            height = top - (foot - 2.0f);
            break;
        }
    }
    enum { kNone, kVault, kMantle, kClimb } kind = kNone;
    if (height >= 40.13f && height <= 56.0f) kind = kMantle;
    else if (height >= 16.0f && height <= 40.12f) kind = kVault;
    else if (height >= 55.1f && height <= 87.5f) kind = kClimb;
    if (kind == kNone) {
        // The client found a vault volume we can't see in the geometry:
        // get over it the plain way (no slide).
        if (!face.hit && height < 0.0f) {
            LOG_INFO("%s: vault/mantle with nothing found ahead -- mantling", SoldierName(&p).c_str());
            height = 48.0f;
            kind = kMantle;
            top = foot - 2.0f + height;
        } else {
            LOG_INFO("%s: obstacle %.1f high -- nothing to vault", SoldierName(&p).c_str(), height);
            return false;
        }
    }
    // Headroom on top, just past the edge.
    const Vec3 edge = face.hit ? face.location : Vec3{p.location[0], p.location[1], 0.0f};
    const Vec3 probe{edge.x + dir.x * 12.0f, edge.y + dir.y * 12.0f, top + 5.0f};
    const bool crouch = kind != kClimb &&
        collision_.LineTrace(probe, Vec3{probe.x, probe.y, probe.z + 88.0f}, CollisionWorld::kBlockTrace).hit;
    const bool sprinted = p.sprinting && !crouch && kind != kClimb;
    if (p.sprinting && !sprinted) {                                // ServerStopSprint
        p.sprinting = false;
        SetWeaponROF(p, p.selected_rof);
    }
    const uint8_t walk = p.tactical ? 10 : 7, crh = p.tactical ? 14 : 1;
    double length = 0.633;
    p.graph_steps.clear();
    p.vault_mantle_height = height;
    p.b_simulate_gravity = false;
    const char* what = "";
    if (kind == kClimb) {
        what = "climbs";
        length = 1.2;
        p.SetGraph(2, 106, 106);
        p.graph_steps.push_back({clock_ + length, 2, walk, walk});
    } else if (sprinted) {
        if (kind == kMantle) {
            what = "hood-slides over";
            p.SetGraph(2, 165, 165);
            p.graph_steps.push_back({clock_ + 0.48, 2, 164, 164});
            length = 0.97;
        } else {
            what = "steps over";
            p.SetGraph(2, 163, 163);
            length = 0.37;
        }
        p.graph_steps.push_back({clock_ + length, 2, 9, 9});
    } else {
        const uint8_t state = kind == kMantle ? (crouch ? 97 : 66) : (crouch ? 96 : 65);
        what = kind == kMantle ? (crouch ? "mantles (crouched) onto" : "mantles onto")
                               : (crouch ? "vaults (crouched) over" : "vaults over");
        if (kind == kVault && crouch) length = 0.767;
        p.SetGraph(2, state, state);
        const uint8_t after = crouch ? crh : walk;
        p.graph_steps.push_back({clock_ + length, 2, after, after});
        if (crouch) { p.crouched = true; p.ground_speed = 100.0f; }
    }
    p.vault_gravity_at = clock_ + length;
    p.vault_busy_until = clock_ + 2.0;
    LOG_INFO("%s %s an obstacle %.1f high", SoldierName(&p).c_str(), what, height);
    return true;
}

// Ladders as retail (t=227.72 / 232.92): mount -> ClientSlingWeapon(),
// graph 2 {174}, gravity off, weapon graph {117} 0.5 s later; dismount ->
// graph 2 {7}, graph 3 {11 -> 0}, gravity on. Climbing down (mounted from
// the top through FinishLadderMoveToTarget; Bridge AI capture 13:37:53):
// weapon graph {23} at +0.47 s, dismount graph 3 {22 -> 0}.
void World::ClimbLadder(ServerPawn* pawn, bool on) {
    if (!pawn || pawn->torn_off || pawn->incapacitated || pawn->on_ladder == on) return;
    pawn->on_ladder = on;
    pawn->graph_steps.clear();
    if (on) {
        pawn->ladder_down = pawn->move_to_ladder;
        pawn->move_to_ladder = false;
        ++pawn->sling_serial;
        pawn->SetGraph(2, 174, 174);
        pawn->b_simulate_gravity = false;
        if (pawn->ladder_down) pawn->graph_steps.push_back({clock_ + 0.47, 3, 23, 23});
        else pawn->graph_steps.push_back({clock_ + 0.5, 3, 117, 117});
    } else {
        pawn->SetGraph(2, 7, 7);
        pawn->SetGraph(3, pawn->ladder_down ? 22 : 11, 0);
        pawn->b_simulate_gravity = true;
        pawn->ladder_down = false;
    }
}

bool World::BeginHoldBreath(ServerPawn* pawn) {
    if (pawn && pawn->incapacitated) return false;
    if (!pawn || !pawn->sights || pawn->asphyxiating || pawn->sprinting) return false;
    pawn->holding_breath = true;
    return true;
}

void World::EndHoldBreath(ServerPawn* pawn) {
    if (pawn) pawn->holding_breath = false;
}

// Rifle rate of fire (eRateOfFire); the protocol sends ClientNotifyRateOfFire
// whenever CurrentROF changes.
void World::SetWeaponROF(ServerPawn& pawn, uint8_t rof) {
    ServerRoundActor* w = FindInventory(&pawn, pawn.loaded_weapon);
    if (!w) return;
    DataValue v; v.kind = DataValue::Kind::kByte; v.bits = 2; v.i = rof;
    w->Set(58, -1, v);                           // AA_BulletWeapon.CurrentROF
}

// Posture (doc sec 31). Retail GamePlayPCAP_Legit_Server t=183-357 answers
// each request within ~30ms with RepGraphStates {current -> destination}
// for graph 2 (body) and graph 3 (weapon carry: 11 low ready, 22 high
// ready, 21 sights, 23 sprint) plus GroundSpeed: 200 relaxed, 130
// tactical, 316 sprinting, 100 crouched. Stamina drains 10/s sprinting and
// recovers 20/s; at 0 the server ends the sprint itself.
// A transition {cur -> dst} on graph 2 always ends with the settled
// {dst -> dst} (retail: {26->10} .. {10->10}, {115->7} .. {7->7}, crouch
// {118->1} .. {76->58} .. {58->58}). Until it does, the client's pawn is
// "transitioning" and refuses use/vault/ladder/reload (doc sec 31).
static void Transition(ServerPawn& p, double now, uint8_t cur, uint8_t dst, double settle_after) {
    p.graph_steps.clear();
    p.SetGraph(2, cur, dst);
    if (cur != dst && dst != 0) p.graph_steps.push_back({now + settle_after, 2, dst, dst});
}

void World::SettleStanding(ServerPawn& p) {
    if (p.tactical) { Transition(p, clock_, 26, 10, 0.35);  p.SetGraph(3, 22, 22); p.ground_speed = 130.0f; }
    else            { Transition(p, clock_, 115, 7, 0.35);  p.SetGraph(3, 11, 11); p.ground_speed = 200.0f; }
}

void World::ApplyPawnAction(ServerPawn* pawn, PawnAction a, bool arg) {
    if (!pawn || pawn->incapacitated) return;
    ServerPawn& p = *pawn;
    switch (a) {
    case PawnAction::kSetTactical:
        p.tactical = arg;
        if (p.sprinting) break;                     // retail ignores toggles mid-sprint
        if (p.crouched) { p.SetGraph(3, p.sights ? 21 : 22, 0); break; }
        p.sights = false;
        SettleStanding(p);
        break;
    case PawnAction::kRaiseSights:
        if (p.sprinting) break;
        p.sights = true;
        // Sighting puts the soldier in the tactical (combat-ready) stance,
        // which outlasts the sights: lowering them goes to {22}, not the
        // relaxed {11} (retail Bridge AI capture 13:24:15-17, the DMR's
        // ACOG; with {11} the client kept the scope's focus blur).
        p.tactical = true;
        if (p.prone) {                                   // retail t=581.54: {114->17} .. {64->64}, 45 -> 23
            Transition(p, clock_, 114, 17, 0);
            p.graph_steps.push_back({clock_ + 0.17, 2, 64, 64});
            p.ground_speed = 23.0f;
        } else if (p.crouched) Transition(p, clock_, 60, 58, 0.5);
        else Transition(p, clock_, 10, 0, 0);
        p.SetGraph(3, 21, 21);
        break;
    case PawnAction::kLowerSights:
        if (!p.sights) break;
        p.sights = false;
        if (p.holding_breath) { p.holding_breath = false; ++p.breath_end_serial; }
        if (p.prone) {                                   // retail t=582.84: {25->2} .. {75->75}, 45
            Transition(p, clock_, 25, 2, 0);
            p.graph_steps.push_back({clock_ + 0.17, 2, 75, 75});
            p.SetGraph(3, 11, 11);
            p.ground_speed = 45.0f;
            break;
        }
        p.SetGraph(3, (p.tactical || p.crouched) ? 22 : 11, (p.tactical || p.crouched) ? 22 : 11);
        break;
    case PawnAction::kSprint:
        if (p.sprinting || p.stamina <= 0.0f) break;
        p.sprinting = true;
        p.sights = false;
        if (p.crouched) { Transition(p, clock_, 63, 9, 0.3); p.SetGraph(3, 23, 23); }
        else            { Transition(p, clock_, 9, 9, 0);    p.SetGraph(3, 23, 0); }
        p.crouched = false;
        p.prone = false;
        p.speed_at = -1.0;
        p.ground_speed = 316.0f;
        SetWeaponROF(p, 0);                         // Safe while sprinting (retail t=31.05)
        break;
    case PawnAction::kStopSprint:
        if (!p.sprinting) break;
        p.sprinting = false;
        SetWeaponROF(p, p.selected_rof);            // back on (retail t=32.79)
        SettleStanding(p);
        break;
    case PawnAction::kCrouch:
        if (p.crouched) break;
        if (p.prone) {                                   // retail t=586.54: {3->71} at 0 .. {71->71}
            p.prone = false;
            p.crouched = true;
            p.sights = false;
            Transition(p, clock_, 3, 71, 0);
            p.graph_steps.push_back({clock_ + 2.5, 2, 71, 71});
            p.ground_speed = 0.0f;
            p.speed_at = clock_ + 1.23;
            p.speed_after = 100.0f;
            break;
        }
        {
            // Only a running soldier slides into the crouch (SprintToCrouch
            // 118, CbtSlide; retail's sample: {118->1} .. {76->58} ..
            // {58->58}). Walking: CbtWalkToCrh 68 / TacWalkToCrh 62; standing
            // still: CbtStdToCrh 6 / TacStdToCrh 15 to the knee
            // (Sold_Tech state graph).
            const float speed = std::hypot(p.velocity[0], p.velocity[1]);
            const bool running = p.sprinting || speed > 250.0f;
            p.crouched = true;
            p.sprinting = false;
            if (running) {
                Transition(p, clock_, 118, 1, 0);
                p.graph_steps.push_back({clock_ + 1.29, 2, 76, 58});   // retail: 1.29s later
                p.graph_steps.push_back({clock_ + 1.60, 2, 58, 58});   // settled crouch
            } else if (speed > 20.0f) {
                if (p.tactical) Transition(p, clock_, 62, 14, 0.5);
                else Transition(p, clock_, 68, 1, 0.5);
            } else {
                if (p.tactical) Transition(p, clock_, 15, 58, 0.6);
                else Transition(p, clock_, 6, 71, 0.6);
            }
        }
        p.SetGraph(3, p.sights ? 21 : 22, 0);
        p.ground_speed = 100.0f;
        break;
    case PawnAction::kVault:
        // AA_PlayerController.ServerTryVault -> TryVaultOrMantle(True).
        TryVaultOrMantle(&p);
        break;
    case PawnAction::kProne:
        if (p.prone) break;                              // retail t=579.44: {4->75} at 0 .. 45
        p.prone = true;
        p.crouched = false;
        p.sprinting = false;
        p.sights = false;
        Transition(p, clock_, 4, 75, 0);
        p.graph_steps.push_back({clock_ + 1.6, 2, 75, 75});
        p.ground_speed = 0.0f;
        p.speed_at = clock_ + 1.6;
        p.speed_after = 45.0f;
        break;
    case PawnAction::kStand:
        if (!p.crouched && !p.prone) break;
        p.crouched = false;                         // (no retail sample: back to the stance)
        p.prone = false;
        p.speed_at = -1.0;
        p.sights = false;
        SettleStanding(p);
        break;
    }
}

void World::UpdatePostures(double dt) {
    for (auto& pc : controllers_) {
        ServerPawn* p = pc->pawn;
        if (!p) continue;
        while ((p->trigger_held || p->burst_left > 0) && clock_ >= p->next_shot_at) {
            if (p->burst_left > 0) --p->burst_left;
            FireRound(*p);
            p->next_shot_at += 0.075;
        }
        if (p->vault_finish_at >= 0.0 && clock_ >= p->vault_finish_at && p->move_to_ladder) {
            p->vault_finish_at = -1.0;
            p->moving_to_target = false;
            ++p->ladder_finish_serial;              // ClientFinishLadderMoveToTarget()
        }
        if (p->vault_finish_at >= 0.0 && clock_ >= p->vault_finish_at) {
            // AA_Pawn.FinishVaultMantleMoveToTarget: ClientFinishVaultMantle-
            // MoveToTarget(), then the server's own TryVaultOrMantle(True)
            // (retail t=157.87 was a climb: {106}, {7} 1.2 s on).
            p->vault_finish_at = -1.0;
            p->moving_to_target = false;
            ++p->vault_finish_serial;
            TryVaultOrMantle(p);
        }
        UpdateEncumbrance(*p);
        if (p->vault_gravity_at >= 0.0 && clock_ >= p->vault_gravity_at) {
            p->b_simulate_gravity = true;
            p->vault_gravity_at = -1.0;
        }
        if (p->speed_at >= 0.0 && clock_ >= p->speed_at) {
            p->ground_speed = p->speed_after;
            p->speed_at = -1.0;
        }
        while (!p->graph_steps.empty() && clock_ >= p->graph_steps.front().at) {
            const ServerPawn::GraphStep st = p->graph_steps.front();
            p->graph_steps.erase(p->graph_steps.begin());
            p->SetGraph(st.graph, st.cur, st.dst);
        }
        // Hold breath: ~6 s of air, recovered at the same rate.
        if (p->holding_breath) {
            p->asphyx = std::min(100.0f, p->asphyx + static_cast<float>(100.0 / 6.0 * dt));
            if (p->asphyx >= 100.0f) {
                p->holding_breath = false;
                p->asphyxiating = true;
                ++p->breath_end_serial;
            }
        } else if (p->asphyx > 0.0f) {
            p->asphyx = std::max(0.0f, p->asphyx - static_cast<float>(100.0 / 6.0 * dt));
            if (p->asphyxiating && p->asphyx < 50.0f) p->asphyxiating = false;
        }
        if (p->sprinting) {
            p->stamina -= static_cast<float>(10.0 * dt);
            if (p->stamina <= 0.0f) {
                p->stamina = 0.0f;
                p->sprinting = false;
                p->tactical = true;                  // retail t=278.6: {26 -> 10}, 130
                SetWeaponROF(*p, p->selected_rof);
                pc->tactical_combat = true;
                SettleStanding(*p);
            }
        } else if (p->stamina < 100.0f) {
            p->stamina = std::min(100.0f, p->stamina + static_cast<float>(20.0 * dt));
        }
    }
}

// Doors (doc sec 31). Retail's AA_Door (GamePlayPCAP_Legit_Server ch 30):
// ServerOpenDoor -> ReplicatedStateName 'Opening', VolumeModifier 1.0,
// PostureModifier 1.0, bUsedFromFront True, then 'FreeSwinging' 1.21s
// later; ServerCloseDoor -> 'Closing', bUsedFromFront False, then
// 'Latched' 1.36s later.
bool World::UseDoor(uint32_t door_object_index, bool open) {
    constexpr uint32_t kStateName = 30, kVolumeModifier = 31, kPostureModifier = 32,
                       kUsedFromFront = 34;
    for (auto& a : map_actors_) {
        if (a->class_name != "AA_Door" || a->object_index != door_object_index) continue;
        ServerRoundActor* door = a.get();
        DataValue state; state.kind = DataValue::Kind::kName;
        state.s = open ? "Opening" : "Closing";
        door->Set(kStateName, -1, state);
        if (open) {
            DataValue one; one.kind = DataValue::Kind::kFloat; one.f = 1.0;
            door->Set(kVolumeModifier, -1, one);
            door->Set(kPostureModifier, -1, one);
        }
        // (Live retail capture: no bUsedFromFront on a first open.)
        (void)kUsedFromFront;
        door_timers_.erase(std::remove_if(door_timers_.begin(), door_timers_.end(),
                           [&](const DoorTimer& t) { return t.door == door; }), door_timers_.end());
        door_timers_.push_back({door, clock_ + (open ? 1.21 : 1.36), open ? "FreeSwinging" : "Latched"});
        return true;
    }
    return false;
}

void World::UpdateDoors() {
    for (auto it = door_timers_.begin(); it != door_timers_.end();) {
        if (clock_ < it->at) { ++it; continue; }
        DataValue state; state.kind = DataValue::Kind::kName; state.s = it->state;
        it->door->Set(30, -1, state);
        it = door_timers_.erase(it);
    }
}

// Ammo (doc sec 31). Live retail capture, 2026-10-09: every ServerStartFire
// on semi is answered with the magazine's RoundIndex + 1 (t=34.47 ->
// 34.49 ...); chambering the first round sets it to 1. RoundIndex counts
// rounds drawn from the magazine, so a 30-round magazine is spent at 30
// (the M249's 200-round box at 200);
// the next shot fires the chambered round and leaves the chamber empty and
// the bolt locked back until a reload. Rates of fire (eRateOfFire): 0
// safe, 1 semi, 2 burst of 3, 3 automatic (~800 rpm).
void World::FireRound(ServerPawn& pawn) {
    constexpr uint32_t kRoundIndex = 29, kChamberedRound = 57, kBoltLockedToRear = 62;
    ServerRoundActor* weapon = FindInventory(&pawn, pawn.loaded_weapon);
    ServerRoundActor* mag = FindInventory(&pawn, pawn.loaded_mag);
    if (!weapon) return;
    const DataField* chamber = weapon->Get(kChamberedRound, 0);
    if (!chamber || chamber->v.kind != DataValue::Kind::kObj) {   // dry
        pawn.trigger_held = false;
        pawn.burst_left = 0;
        return;
    }
    TraceShot(pawn, *weapon);
    StatShot(pawn);
    // Pawn.FlashCount: the other players' clients show and play the shot.
    pawn.flash_count = static_cast<uint8_t>(pawn.flash_count + 1);
    if (pawn.flash_count == 0) pawn.flash_count = 1;
    const DataField* ri = mag ? mag->Get(kRoundIndex) : nullptr;
    int64_t drawn = ri ? ri->v.i : 0;
    if (mag && drawn < pawn.mag_rounds) {
        mag->Set(kRoundIndex, -1, DataValue::Int(drawn + 1));      // next round chambered
    } else {
        weapon->Set(kChamberedRound, 0, DataValue::Actor(nullptr)); // last round gone
        weapon->Set(kBoltLockedToRear, -1, DataValue::Bool(true));
        pawn.trigger_held = false;
        pawn.burst_left = 0;
    }
}

// Bullet trace (doc sec 32). Retail (live capture 2026-10-09, 144 impacts):
// the shot starts 31 units above the pawn's location along its aim, and the
// first hit goes out as AA_BulletWeapon.ImpactRepList[0] = {RepIdx (shot
// counter), ImpactLocation, TraceDirection = unit direction * 32768}. Our
// collision world reproduces retail's first impacts to within a unit.
// Later list entries (penetration exits) and spread aren't modelled yet.
void World::TraceShot(ServerPawn& pawn, ServerRoundActor& weapon) {
    constexpr uint32_t kImpactRepList = 56;
    // Where retail's trace starts above the pawn's location (fitted to the
    // ImpactRepList rays of the Bridge AI captures: standing +33, crouched
    // +20, prone about +3); a fixed 31 put crouched and prone shots high.
    const float kEyeOffset = (pawn.prone ? 3.0f : pawn.crouched ? 20.0f : 33.0f) - pawn.body_drop;
    constexpr float kRange = 32768.0f;
    const ServerPlayerController* shooter = nullptr;
    for (const auto& pc : controllers_) if (pc->pawn == &pawn) shooter = pc.get();
    if (!shooter) return;
    // AA_BulletWeapon.GetAdjustedAim: the view plus the weapon's sway
    // (SwayAimOffset, from ServerOffsetAim) -- where the sight points.
    // Sighted: a Gaussian minutes-of-angle variance (fMinutesOfAngleRating,
    // 1 MoA = 8.34). From the hip: a weighted random cone of
    // CombatWeaponAccuracy{,Crh,Prn} (the DMR and MSP use the M16A4's).
    struct Accuracy { const char* cls; int stand, crouch, prone; float moa; };
    static const Accuracy kAccuracy[] = {
        {"AA_BulletWeapon_M16DMR", 275, 225, 175, 1}, {"AA_BulletWeapon_ObranMSP", 275, 225, 175, 1},
        {"AA_BulletWeapon_M16A4", 275, 225, 175, 2},  {"AA_BulletWeapon_ObranNP", 275, 225, 175, 2},
        {"AA_BulletWeapon_M4A1", 350, 300, 250, 3},   {"AA_BulletWeapon_ObranKNP", 350, 300, 250, 3},
        {"AA_BulletWeapon_M249", 425, 375, 225, 3},   {"AA_BulletWeapon_ObranLMK", 425, 375, 225, 3}};
    const Accuracy* acc = &kAccuracy[4];
    for (const auto& a : kAccuracy) if (weapon.class_name == a.cls) acc = &a;
    auto frand = [] { return std::rand() / static_cast<double>(RAND_MAX); };
    // The sway only moves the sighted rifle; from the hip retail aims along
    // the view itself (AA_Pawn.GetAdjustedAimFor: GetViewRotation()).
    const int sway_on = pawn.sights ? 1 : 0;
    double aim_pitch = static_cast<int16_t>(shooter->rotation_pitch & 0xFFFF) +
                       sway_on * static_cast<int16_t>(pawn.aim_offset & 0xFFFF);
    double aim_yaw = (shooter->rotation_yaw & 0xFFFF) + sway_on * static_cast<int16_t>((pawn.aim_offset >> 16) & 0xFFFF);
    if (pawn.sights) {
        auto moa = [&] { return (frand() > 0.5 ? 8.34 : -8.34) * acc->moa * std::exp(-(std::pow(frand(), 2) / 0.18)); };
        aim_pitch += moa();
        aim_yaw += moa();
    } else {
        const int base = pawn.prone ? acc->prone : pawn.crouched ? acc->crouch : acc->stand;
        // GetWeightedBaseAim: most shots well inside the cone.
        static const double kBand[10][2] = {{0, 0}, {0, .25}, {.25, .5}, {.5, .65}, {.65, .7},
                                            {.7, .75}, {.75, .8}, {.8, .85}, {.85, .9}, {.9, 1}};
        const double r = frand();
        const int band = std::min(9, static_cast<int>(r * 10));
        const double delta = r - band * 0.1;
        const double frac = kBand[band][0] + (kBand[band][1] - kBand[band][0]) * 10 * delta;
        const double extent = (frand() > 0.5 ? 1 : -1) * std::floor(frac * base);
        const double theta = frand() * 2 * 3.14159265358979;
        aim_pitch += extent * std::sin(theta);
        aim_yaw += extent * std::cos(theta);
    }
    const double k = 3.14159265358979 * 2.0 / 65536.0;
    {   // per-shot record: how far the shot left the view, and why
        const double vp = static_cast<int16_t>(shooter->rotation_pitch & 0xFFFF);
        const double vy = shooter->rotation_yaw & 0xFFFF;
        const double dyaw = std::remainder(aim_yaw - vy, 65536.0);
        LOG_INFO("Shot %s: %s %s sway (%d, %d) off view %.2f deg (pitch %+.0f yaw %+.0f)", pawn.label.c_str(),
                 pawn.sights ? "SIGHTS" : "hip", pawn.prone ? "prone" : pawn.crouched ? "crouch" : "stand",
                 static_cast<int16_t>((pawn.aim_offset >> 16) & 0xFFFF), static_cast<int16_t>(pawn.aim_offset & 0xFFFF),
                 std::hypot(aim_pitch - vp, dyaw) * 360.0 / 65536.0, aim_pitch - vp, dyaw);
    }
    const double pitch = aim_pitch * k;
    const double yaw = aim_yaw * k;
    const Vec3 dir{static_cast<float>(std::cos(pitch) * std::cos(yaw)),
                   static_cast<float>(std::cos(pitch) * std::sin(yaw)),
                   static_cast<float>(std::sin(pitch))};
    // A player's shot leaves from their camera: read from the live client
    // (2026-10-13, standing, scope up) 4.6 right of the pawn, 2 back and
    // 31.4 up; from the hip 1.1 right, 8 back. From the pawn's centre at
    // +33, sighted shots hit left of and above the red dot.
    float fwd = 0.0f, right = 0.0f, up = kEyeOffset;
    if (!pawn.is_npc) {
        fwd = pawn.sights ? -2.0f : -8.0f;
        right = pawn.sights ? 4.6f : 1.1f;
        if (!pawn.crouched && !pawn.prone) up = pawn.sights ? 31.4f : 32.0f;
    }
    const float vyaw = static_cast<float>((shooter ? shooter->rotation_yaw : pawn.rotation_yaw) & 0xFFFF) *
                       3.14159265f / 32768.0f;
    const Vec3 start{pawn.location[0] + std::cos(vyaw) * fwd - std::sin(vyaw) * right,
                     pawn.location[1] + std::sin(vyaw) * fwd + std::cos(vyaw) * right,
                     pawn.location[2] + up};
    const Vec3 end{start.x + dir.x * kRange, start.y + dir.y * kRange, start.z + dir.z * kRange};
    TraceHit hit = collision_.LineTrace(start, end, CollisionWorld::kBlockTrace);
    // Soldiers: vertical cylinders (radius 22, half-height 48 standing, 34
    // crouched, 14 prone) around each live pawn's location -- players and
    // enemy NPCs; the nearest hit in front of the map's wins.
    float best_t = hit.hit ? hit.time : 1.0f;
    const Vec3 seg{end.x - start.x, end.y - start.y, end.z - start.z};
    ServerPawn* victim = TracePawns(start, seg, &pawn, /*skip_npcs=*/false, &best_t);
    const Vec3 at = victim ? Vec3{start.x + seg.x * best_t, start.y + seg.y * best_t, start.z + seg.z * best_t}
                           : (hit.hit ? hit.location : end);
    {   // the sway's miss at that range, for comparing with what the shooter saw
        const float range = std::sqrt((at.x - start.x) * (at.x - start.x) + (at.y - start.y) * (at.y - start.y) +
                                      (at.z - start.z) * (at.z - start.z));
        LOG_INFO("  from (%.1f, %.1f, %.1f) view yaw %d pitch %d", start.x, start.y, start.z,
                 shooter->rotation_yaw & 0xFFFF, static_cast<int16_t>(shooter->rotation_pitch & 0xFFFF));
        LOG_INFO("  impact (%.0f, %.0f, %.0f) range %.0f -> %s; sway moves it %.0f units", at.x, at.y, at.z, range,
                 victim ? SoldierName(victim).c_str() : "world",
                 sway_on * range * std::hypot(static_cast<int16_t>(pawn.aim_offset & 0xFFFF),
                                              static_cast<int16_t>((pawn.aim_offset >> 16) & 0xFFFF)) * k);
    }
    if (victim) DamagePawn(*victim, &pawn, at, dir);
    else if (hit.hit && hit.owner) HitMapActor(hit.owner - 1);
    NpcBulletNear(pawn, start, at, victim);      // suppression and hearing (npc.cpp)
    pawn.shots_fired = static_cast<uint8_t>(pawn.shots_fired + 1);
    if (pawn.shots_fired == 0) pawn.shots_fired = 1;

    DataValue info;
    info.kind = DataValue::Kind::kStruct;
    DataValue rep; rep.kind = DataValue::Kind::kByte; rep.bits = 8; rep.i = pawn.shots_fired;
    DataValue loc; loc.kind = DataValue::Kind::kVector;
    loc.v3[0] = static_cast<int32_t>(std::lround(at.x));
    loc.v3[1] = static_cast<int32_t>(std::lround(at.y));
    loc.v3[2] = static_cast<int32_t>(std::lround(at.z));
    DataValue d; d.kind = DataValue::Kind::kVector;
    d.v3[0] = static_cast<int32_t>(std::lround(dir.x * 32768.0f));
    d.v3[1] = static_cast<int32_t>(std::lround(dir.y * 32768.0f));
    d.v3[2] = static_cast<int32_t>(std::lround(dir.z * 32768.0f));
    info.members = {rep, loc, d};
    weapon.Set(kImpactRepList, 0, info);
}

// Damage (doc sec 33). No retail sample of a bullet hit yet: damage by
// zone along the 96-unit cylinder (head = top 14 units 100, torso 40,
// legs 25), the impact replicated through PawnShotEffectInfo so clients
// draw the hit, Health to the owner.
void World::DamagePawn(ServerPawn& victim, ServerPawn* shooter, const Vec3& at, const Vec3& dir) {
    if (!victim.is_npc && !victim.first_reload_done) return;   // spawn protection (retail)
    const float hh = victim.prone ? 14.0f : (victim.crouched ? 34.0f : 48.0f);
    const float rel = at.z - (victim.location[2] - victim.body_drop);
    // Zone -> eHitLocation and the hit's total: head 100 (no wound, down at
    // once), torso 40, legs 35 -- retail's leg hits (capture 2026-10-11, doc
    // sec 39): ~15 at once and ~20 bled out (100 -> 84.88, HealthTarget 64.96).
    const bool legs = rel <= -hh * 0.2f;
    if (shooter && !shooter->is_npc)                          // round statistics (doc sec 45)
        StatHit(*shooter, victim,
                rel > hh - 14.0f ? 0 : legs ? (rel > -hh * 0.6f ? 3 : 4) : rel > hh * 0.2f ? 1 : 2,
                RifleStat(*shooter));
    if (rel > hh - 14.0f) {
        ApplyDamage(victim, shooter, 100.0f, 6464, at, dir);   // AA3Baseline AA_DamageType_Bullet
        return;
    }
    if (victim.is_npc) {
        // Enemies take a hit at once, nothing bled out (retail Bridge AI
        // capture 2026-10-12, user session, ~30 NPC hits): upper torso (3)
        // drops it to 25, down, in one; lower torso (4) 34; arms and legs
        // 15 each. Our player shots needed 4-5 chest hits (2026-10-13 live).
        const bool upper = rel > hh * 0.2f;
        const int location = legs ? (rel > -hh * 0.6f ? 12 : 13) : upper ? 3 : 4;
        const float dmg = legs ? 15.0f : upper ? std::max(0.0f, victim.health - kIncapacitationHealth) : 34.0f;
        AddWound(victim, location, 0.0f, SoldierName(shooter));
        ApplyDamage(victim, shooter, std::max(dmg, 1.0f), 6464, at, dir);
        return;
    }
    const int location = legs ? (rel > -hh * 0.6f ? 12 : 13) : 3;   // HL_UpperLegRight / LowerLegRight / UpperTorso
    const float total = legs ? 35.0f : 40.0f;
    const float now = total * 0.43f;
    ApplyDamage(victim, shooter, now, 6464, at, dir);
    if (!victim.torn_off && !victim.incapacitated)
        AddWound(victim, location, total - now, SoldierName(shooter));
}

void World::ApplyDamage(ServerPawn& victim, ServerPawn* shooter, float dmg, uint32_t damage_class,
                        const Vec3& at, const Vec3& dir) {
    if (victim.torn_off || dmg <= 0.0f) return;
    if (!victim.is_npc && !victim.first_reload_done) return;   // spawn protection (retail)
    // A soldier already down dies to the next hit (doc sec 35).
    if (victim.incapacitated) {
        LOG_INFO("%s finished %s", SoldierName(shooter).c_str(), SoldierName(&victim).c_str());
        KillPawn(victim, shooter);
        return;
    }
    victim.health = std::max(0.0f, victim.health - dmg);
    victim.health_target = victim.health;
    victim.movement_speed_modifier = InjuredSpeedModifier(victim.health);
    victim.hit_location[0] = at.x; victim.hit_location[1] = at.y; victim.hit_location[2] = at.z;
    victim.hit_damage_class = damage_class;
    ++victim.hit_serial;
    victim.shot_effect_hit_location[0] = at.x;
    victim.shot_effect_hit_location[1] = at.y;
    victim.shot_effect_hit_location[2] = at.z;
    victim.shot_effect_hit_rotation_yaw = static_cast<int32_t>(std::atan2(dir.y, dir.x) * 32768.0 / 3.14159265358979) & 0xFFFF;
    victim.shot_effect_hit_rotation_pitch = static_cast<int32_t>(std::asin(std::max(-1.0f, std::min(1.0f, dir.z))) * 32768.0 / 3.14159265358979) & 0xFFFF;
    LOG_INFO("%s hit %s for %.0f (health %.0f)", SoldierName(shooter).c_str(), SoldierName(&victim).c_str(), dmg,
             victim.health);
    if (shooter && shooter->controller) victim.last_hit_by_client = shooter->controller->owning_client_id;
    if (shooter) {
        victim.npc_last_hit_by = shooter;      // (players too: iIncapacitatedByEnemy)
        victim.npc_last_hit_at = clock_;
    }
    if (victim.health <= kIncapacitationHealth) {
        // An NPC hit in the head dies outright (retail frame 12295: bTearOff,
        // Health -15); otherwise it goes down like a soldier.
        if (victim.is_npc && dmg >= 100.0f) KillPawn(victim, shooter);
        else Incapacitate(victim, damage_class, SoldierName(shooter));
    }
    // AA_NPCBaseSoldierController: NotifyIncapacitated -> ascIncapacitated
    // (Medic); a wound it walks off -> ascInitiallyWounded (Medic,
    // RequestingSupportAtMyLocation).
    if (victim.is_npc && !victim.torn_off) {
        if (victim.incapacitated) NpcVoice(victim, {961});
        else NpcVoice(victim, {961, 975});
    }
}

void World::PlaySound(const char* pkg, uint32_t net, const ServerActor* source, const Vec3& loc,
                      bool stop_when_destroyed, bool delay, float priority, float radius) {
    SoundEvent e;
    e.seq = ++sound_seq_;
    e.at = clock_;
    e.pkg = pkg;
    e.net = net;
    e.source = source;
    e.loc = loc;
    e.stop_when_destroyed = stop_when_destroyed;
    e.delay = delay;
    e.priority = priority;
    e.radius = radius;
    sound_events_.push_back(e);
}

bool World::Occluded(const Vec3& from, const Vec3& to) const {
    return collision_.LineTrace(from, to, CollisionWorld::kBlockTrace).hit;
}

uint32_t World::BriefingSoundIndex(int32_t team) const {
    // S_AIMissionAudio: <Map>.GoldCOMissionBriefingCue for team 0 (retail
    // Bridge AI: the Black-team player heard Bridge.GoldCO, NetIndex 352),
    // BlackCO for team 1. AI co-op levels only.
    if (game_mode_level_.find("_ai") == std::string::npos) return 0;
    static const struct { const char* map; uint32_t gold, black; } kBriefings[] = {
        {"Bridge", 352, 349}, {"Airfield", 345, 342}, {"Ranch", 364, 361}};
    for (const auto& b : kBriefings)
        if (game_mode_level_.rfind(b.map, 0) == 0 || map_name_.rfind(b.map, 0) == 0)
            return ObjectIndex("S_AIMissionAudio", team == 1 ? b.black : b.gold);
    return 0;
}

// AA_Pawn.MovementSpeedModifier as retail sends it while hurt: 0.8852 at
// full health, 0.6602 at 67.3, 0.5 at 34.7 (capture 2026-10-09); linear
// between the samples, 0.5 below.
float World::InjuredSpeedModifier(float health) {
    if (health >= 100.0f) return 0.8852f;
    if (health >= 67.3443f) return 0.6602f + (health - 67.3443f) * (0.8852f - 0.6602f) / (100.0f - 67.3443f);
    if (health >= 34.6885f) return 0.5f + (health - 34.6885f) * (0.6602f - 0.5f) / (67.3443f - 34.6885f);
    return 0.5f;
}

// Incapacitation as retail (capture 2026-10-09 t=227.04, a fall): the
// owner gets ClientIncapacitated() on the pawn and, on the controller,
// the hit notice, ClientNotifyIncapacitated(True, instigator),
// NotifyPostureChange('EndWalk') and ClientSetCameraMode('FreeCam'); the
// pawn Health/HealthTarget 25, RepGraphStates[0] {41}, [2] {68 -> 1},
// IncapacitationType = the damage class; the storage proxy bIncapacitated.
// 0.23 s later GroundSpeed 130, graph 2 {71}, PRI bIsIncapacitated; at
// 1.2 s graph 0 {42 -> 13} + bRagdollTick/bRagdollStuck, which settles the
// ragdoll into the incapacitated pose (UpdateIncapacitated).
void World::Incapacitate(ServerPawn& victim, uint32_t damage_class, const std::string& instigator) {
    if (!victim.is_npc && victim.npc_last_hit_by && victim.npc_last_hit_by->is_npc)
        if (PlayerRoundStats* s = LiveStats(&victim)) ++s->iIncapacitatedByEnemy;
    if (victim.is_npc) {
        // Retail (Bridge AI capture, ch 262, t=526.26): the killer is told
        // at once (ClientNotifyNeutralizedNME); the NPC lies at Health 25,
        // IncapacitationType set, graph 0 {41}, graph 2 {5 -> 67}, ragdoll
        // ~0.9 s on, until secured or finished.
        CreditNpc(victim, nullptr);
        victim.incapacitated = true;
        victim.incap_at = clock_;
        victim.incap_stage = 0;
        victim.health = kIncapacitationHealth;
        victim.health_target = kIncapacitationHealth;
        victim.incap_type_index = ObjectIndex("AA3Baseline", damage_class);
        victim.incap_instigator = instigator;
        victim.bleeding = false;
        victim.movement_speed_modifier = 0.5f;
        victim.SetGraph(0, 41, 41);
        victim.SetGraph(2, 5, 67);
        victim.graph_steps.clear();
        victim.velocity[0] = victim.velocity[1] = victim.velocity[2] = 0.0f;
        if (auto it = npc_brains_.find(&victim); it != npc_brains_.end()) { it->second.path.clear(); it->second.path_i = 0; }
        LOG_INFO("NPC %s incapacitated by %s", victim.npc_name.c_str(), instigator.c_str());
        return;
    }
    // Down with the case: the HUD's "drop it for a squad member" notice
    // (AA_Pawn: ClientNotifyIncapacitatedWithObjective).
    if (carry_.active && carry_.carrier == &victim && victim.controller) ++victim.controller->objective_incap_serial;
    // The bleed still owed, for a revive (doc sec 39): retail keeps
    // HealthTarget while down; ours shows 25.
    victim.target_before_incap = std::min(victim.health_target, victim.health);
    victim.incapacitated = true;
    victim.incap_at = clock_;
    victim.incap_stage = 0;
    victim.health = kIncapacitationHealth;
    victim.health_target = kIncapacitationHealth;
    victim.incap_type_index = ObjectIndex("AA3Baseline", damage_class);
    victim.incap_instigator = instigator;
    // Gunshot and blast wounds bleed; falls (broken legs) don't --
    // assumption, no retail sample of a bullet incapacitation yet.
    victim.bleeding = incap_bleeding_ && damage_class != 6486;
    victim.SetGraph(0, 41, 41);
    victim.SetGraph(2, 68, 1);
    victim.graph_steps.clear();
    victim.speed_at = -1.0;
    victim.trigger_held = false;
    victim.burst_left = 0;
    victim.holding_breath = false;
    victim.moving_to_target = false;
    victim.vault_finish_at = -1.0;
    victim.sprinting = victim.sights = false;
    ++victim.incap_serial;
    for (auto& c : controllers_)
        if (c->pawn == &victim && c->proxy) c->proxy->incapacitated = true;
    LOG_INFO("%s incapacitated by %s", victim.pri ? victim.pri->player_name.c_str() : "?", instigator.c_str());
}

bool World::Withdraw(ServerPawn* pawn) {
    if (!pawn || !pawn->incapacitated || pawn->torn_off) return false;
    KillPawn(*pawn, nullptr);
    return true;
}

void World::UpdateIncapacitated(double dt) {
    for (auto& up : pawns_) {                   // downed enemies: the ragdoll settles (retail +0.9 s)
        ServerPawn& n = *up;
        if (!n.is_npc || !n.incapacitated || n.torn_off || n.zip_tie || n.incap_stage >= 2) continue;
        if (clock_ - n.incap_at >= 0.9) {
            n.incap_stage = 2;
            n.SetGraph(0, 42, 13);
            n.ragdoll = true;
        }
    }
    for (auto& c : controllers_) {
        ServerPawn* p = c->pawn;
        if (!p || !p->incapacitated || p->torn_off) continue;
        const double t = clock_ - p->incap_at;
        if (p->incap_stage == 0 && t >= 0.23) {
            p->incap_stage = 1;
            p->ground_speed = 130.0f;
            p->SetGraph(2, 71, 71);
            if (c->pri) c->pri->b_incapacitated = true;
        }
        // The client drops the body as a ragdoll; after its
        // InitialRagdollTimeCheck (0.85 s) retail settles it -- graph 0
        // {42 -> 13}, bRagdollTick + bRagdollStuck at +1.2 s -- and the
        // client blends into the curled incapacitated pose. Without this the
        // body stays a loose ragdoll (seen 2026-10-09).
        if (p->incap_stage == 1 && t >= 1.2) {
            p->incap_stage = 2;
            p->SetGraph(0, 42, 13);
            p->ragdoll = true;
        }
        // A downed soldier holds that position with Health steady until the
        // round ends, an enemy finishes them or a medic treats them (user,
        // retail). Only a bleeding soldier loses Health -- at
        // IncapacitatedBloodlossRate (0.25/s), in whole points so Health
        // isn't resent every tick -- and dies at 0.
        if (!p->bleeding) continue;
        const float left = kIncapacitationHealth - static_cast<float>(t) * kIncapBloodlossRate;
        const float shown = std::ceil(std::max(0.0f, left));
        if (shown < p->health) { p->health = shown; p->health_target = shown; }
        if (left <= 0.0f) {
            LOG_INFO("%s bled out", p->pri ? p->pri->player_name.c_str() : "?");
            KillPawn(*p, nullptr);
        }
    }
    (void)dt;
}

// Death as retail replicates it (GamePlayPCAP_Legit_Server t=81.83): pawn
// Health 0, bTearOff, Controller/PRI/Owner cleared, LastKnownPRI kept, death
// animation states; PRI Deaths+1, iDied, bIsDead; PlayerStorageProxy

void World::CreditNpc(ServerPawn& victim, ServerPawn* killer) {
    if (victim.npc_credited) return;
    victim.npc_credited = true;
    ServerPlayerController* kpc = killer && killer->controller ? killer->controller
                                                                : FindPlayerController(victim.last_hit_by_client);
    if (!kpc) return;
    kpc->last_npc_kill = victim.npc_name;
    ++kpc->npc_kill_serial;
    StatNeutralized(*kpc, victim);
}

// Retail (Bridge AI capture frame 12295): pouches None; its weapon slots and
// dump pouch close, its rifle drops.
void World::StripNpc(ServerPawn& npc) {
    npc_brains_.erase(&npc);
    npc.dump_pouch = nullptr;
    npc.next_mag_pouch = nullptr;
    if (auto it = pawn_inventory_.find(&npc); it != pawn_inventory_.end())
        for (auto& g : it->second)
            if (g->class_name.find("WeaponSlot") != std::string::npos || g->class_name.find("Pouch") != std::string::npos)
                g->gone = true;
    npc.velocity[0] = npc.velocity[1] = npc.velocity[2] = 0.0f;
    DropNpcWeapon(npc);
}

// ServerTryToIdentifyCorpse for an enemy NPC (retail 13:31:39 / 13:28:00):
// one still down is secured -- "X secured Y (Z : weapon)" to the team,
// EnemySecured (commo 47), off the scoreboard, AD_SecuredCount and
// GA_ZipCuffLabel, +30 (rule 10, Duty+Respect); its pawn: graph 4 {150 ->
// 83}, bBodyAlreadyIdentified, Health 0, IncapacitationType None,
// bPlayZipTie; channel closed ~18 s on. A body is confirmed: EnemyDown
// (commo 46), the secured and confirmed counts, +30 (10) and +15 (rule 9).
bool World::SecureEnemy(ServerPlayerController& pc, const std::string& name) {
    ServerPawn* me = pc.pawn;
    if (!me || me->torn_off || me->incapacitated) return false;
    ServerPawn* npc = nullptr;
    for (auto& up : pawns_)
        if (up->is_npc && up->npc_name == name && !up->destroyed && !up->npc_body_gone && !up->body_identified) npc = up.get();
    if (!npc) return false;
    if (std::hypot(npc->location[0] - me->location[0], npc->location[1] - me->location[1]) > 300.0f) return false;
    SecureEvent ev;
    ev.id = next_secure_id_++;
    ev.by = &pc;
    ev.team = TeamOf(pc);
    ev.npc = name;
    npc->body_identified = true;
    if (npc->incapacitated && !npc->torn_off && npc->health > 0.0f) {
        ev.alive = true;
        std::string wounder = "Unknown", weapon = "Unknown";
        if (ServerPlayerController* w = FindPlayerController(npc->last_hit_by_client)) {
            if (w->pri) wounder = w->pri->player_name;
            if (w->pawn)                        // sLastWoundedByWepType: its rifle, e.g. "M16DMR"
                if (auto it = pawn_inventory_.find(w->pawn); it != pawn_inventory_.end())
                    for (const auto& g : it->second)
                        if (g->class_name.rfind("AA_BulletWeapon_", 0) == 0) { weapon = g->class_name.substr(16); break; }
        }
        ev.message = (pc.pri ? pc.pri->player_name : std::string("?")) + " secured " + name + " (" + wounder + " : " +
                     weapon + ")";
        npc->zip_tie = true;
        npc->health = npc->health_target = 0.0f;
        npc->incap_type_index = 0;
        npc->SetGraph(4, 150, 83);
        npc->npc_body_until = clock_ + 18.0;
        StripNpc(*npc);
        ++pc.secured_count;
        // Retail t=544.48: iSecuredEnemy and iRoundConfirmedNeutralizedEnemy
        // +1, Duty +30 and Respect +30 (a confirm: +15 each).
        ++pc.live_round.iSecuredEnemy;
        if (pc.pri) {
            ++pc.pri->secured_enemy; ++pc.pri->round_confirmed;
            pc.pri->xp_duty += 30; pc.pri->xp_respect += 30;
            pc.pri->xp_round_honor += 60; pc.pri->match_total_score += 60;
        }
    } else if (npc->torn_off) {
        ++pc.secured_count;
        ++pc.confirmed_count;
        ++pc.live_round.iConfirmedEnemy;
        if (pc.pri) {
            ++pc.pri->secured_enemy; ++pc.pri->round_confirmed;
            pc.pri->xp_duty += 15; pc.pri->xp_respect += 15;
            pc.pri->xp_round_honor += 30; pc.pri->match_total_score += 30;
        }
    } else {
        npc->body_identified = false;
        return false;
    }
    ev.secured = pc.secured_count;
    ev.confirmed = pc.confirmed_count;
    // The player's "Enemy secure!" (retail t=543.69: US_Auto_NMESecure from
    // the secured enemy, with the EnemyConfirm message).
    PlaySound("S_Char_VO_Soldier", 931, npc, Vec3{npc->location[0], npc->location[1], npc->location[2]}, false, false,
              0.0f, 5000.0f);
    LOG_INFO("%s %s %s", pc.pri ? pc.pri->player_name.c_str() : "?", ev.alive ? "secured" : "confirmed", name.c_str());
    secure_events_.push_back(ev);
    if (secure_events_.size() > 64) secure_events_.erase(secure_events_.begin());
    return true;
}

// bIsDead; the controller loses its pawn (the client goes to spectating).
void World::KillPawn(ServerPawn& victim, ServerPawn* killer) {
    DropObjective(victim);                      // a carried briefcase falls with the soldier
    // Treatment links end with the soldier.
    StopTreatment(&victim);
    for (auto& p : pawns_)
        if (p->patient == &victim) StopTreatment(p.get());
    victim.treater = nullptr;
    ServerPlayerController* pc = nullptr;
    for (auto& c : controllers_) if (c->pawn == &victim) pc = c.get();
    victim.health = 0.0f;
    victim.torn_off = true;
    victim.last_known_pri = victim.pri;
    victim.SetGraph(2, 7, 7);
    victim.SetGraph(4, 77, 77);
    victim.trigger_held = false;
    victim.holding_breath = false;
    if (pc && pc->pri) {
        pc->pri->b_incapacitated = false;
        pc->pri->deaths += 1;
        pc->pri->i_died += 1;
        pc->pri->b_dead = true;
    }
    // PRI.Score: one per enemy that dies of this player's hits, at once or
    // later (retail: +1 at a headshot neutralization, nothing for one
    // downed, +1 when a downed one died; t=121.35 ... 754.58).
    ServerPlayerController* credit = killer && killer->controller ? killer->controller
        : victim.is_npc ? FindPlayerController(victim.last_hit_by_client) : nullptr;
    if (killer && killer->pri && killer != &victim) killer->pri->kills += 1;
    if (victim.is_npc && credit && credit->pri) credit->pri->score += 1;
    else if (!victim.is_npc && killer && killer->pri && killer != &victim) killer->pri->score += 1;
    // An enemy NPC: whoever last hurt it gets the neutralization notice
    // (unless it went down first); the body stays ~60 s.
    if (victim.is_npc) {
        CreditNpc(victim, killer);
        if (!victim.zip_tie) StripNpc(victim);
        victim.npc_body_until = clock_ + 60.0;
    }
    if (pc) {
        if (pc->proxy) {
            pc->proxy->is_dead = true;
            pc->proxy->incapacitated = false;
            pc->proxy->is_spectating = true;
        }
        // Spectating until the next spawn (retail t=81.83).
        pc->collide_world = true;
        pc->spectator_mode = 1;
        pc->pawn = nullptr;
        pc->died = true;
        pc->respawn_at = respawn_delay_ > 0 ? clock_ + respawn_delay_ : -1.0;
    }
    LOG_INFO("%s killed %s", SoldierName(killer).c_str(), SoldierName(&victim).c_str());
}

// A bullet hit a dynamic map actor (doc sec 32). Breakable windows as on a
// live retail capture (2026-10-09): the first hit sets the window's bit in
// AA_BreakableWindowManager.CrackedWindowFlags[] (the client draws cracked
// glass), the next hit its bit in ShatteredWindowFlags[] (the client
// shatters it) and Actor.bCollideActors = False on the window itself. The
// bit is the window's position among the level's breakable windows
// (config/maps/<map>.windows.json). A shattered window stops blocking traces.
void World::HitMapActor(uint32_t net_index) {
    constexpr uint32_t kCollideActors = 10, kShatteredFlags = 23, kCrackedFlags = 24;
    auto idx_it = window_index_.find(net_index);
    if (idx_it == window_index_.end()) return;
    ServerRoundActor* window = nullptr;
    for (auto& a : map_actors_)
        if (a->cdo_net_index == net_index && a->class_name == "AA_BreakableWindow") window = a.get();
    ServerRoundActor* manager = nullptr;
    for (auto& a : join_actors_)
        if (a->class_name == "AA_BreakableWindowManager") manager = a.get();
    if (!window || !manager) return;
    const int hits = ++window_hits_[net_index];
    const int bit = idx_it->second;
    auto set_bit = [&](uint32_t field) {
        const DataField* f = manager->Get(field, bit / 32);
        const uint32_t v = (f ? static_cast<uint32_t>(f->v.i) : 0u) | (1u << (bit % 32));
        manager->Set(field, bit / 32, DataValue::Int(static_cast<int32_t>(v)));
    };
    if (hits == 1) {
        set_bit(kCrackedFlags);
    } else if (hits == 2) {
        set_bit(kShatteredFlags);
        window->Set(kCollideActors, -1, DataValue::Bool(false));
        collision_.SetOwnerEnabled(net_index + 1, false);
        LOG_INFO("Window %u (manager bit %d) shattered", net_index, bit);
    }
}

void World::SetTrigger(ServerPawn* pawn, bool down) {
    if (!pawn) return;
    if (pawn->incapacitated) { pawn->trigger_held = false; return; }
    if (!down) { pawn->trigger_held = false; return; }
    if (pawn->fire_mode != 0 || pawn->fire_mode_target >= 0) return;      // the XM320 is up (or coming up)
    for (const auto& j : reloads_) if (j.pawn == pawn) return;            // initial reload
    for (const auto& j : deliberate_reloads_) if (j.pawn == pawn) return;
    ServerRoundActor* weapon = FindInventory(pawn, pawn->loaded_weapon);
    const DataField* rof_f = weapon ? weapon->Get(58) : nullptr;
    const int64_t rof = rof_f ? rof_f->v.i : 1;
    if (rof == 0) return;                                                   // safe
    FireRound(*pawn);
    pawn->trigger_held = rof == 3;
    pawn->burst_left = rof == 2 ? 2 : 0;
    pawn->next_shot_at = clock_ + 0.075;
}

// Reload (doc sec 31). Retail's one ServerTryToReload(eReloadType 2) at
// t=426.81 (GamePlayPCAP_Legit_Server, third life): weapon graph 4 {33->33}
// at once; CurrentROF 0 (+ClientNotifyRateOfFire) at +0.3s; the loaded
// magazine's RoundIndex = 0 at +1.9s; CurrentROF back at +3.8s; graph 4
// {79->79} (ready) at +4.0s. No magazine swap in that sample.
void World::BeginReload(ServerPawn* pawn) {
    if (pawn && pawn->launcher && (pawn->fire_mode == 1 || pawn->fire_mode_target >= 0)) {
        // AA_Weapon.Reload: in the weapon mod's fire mode it's RT_WeaponMod.
        if (!BeginLauncherReload(pawn)) ++pawn->confirm_reload_serial;
        return;
    }
    if (!pawn || !pawn->loaded_weapon || pawn->incapacitated) return;
    if (!pawn->pri) return;
    bool any = false;                            // a magazine left to load (AdvancedHasAmmoMag)
    for (const auto& m : pawn->pri->primary_mags) any = any || (!m.in_weapon && m.rounds > 0);
    if (!any) return;
    for (const auto& j : deliberate_reloads_) if (j.pawn == pawn) return;
    for (const auto& j : reloads_) if (j.pawn == pawn) return;   // initial reload still running
    uint8_t rof = 1;
    if (ServerRoundActor* w = FindInventory(pawn, pawn->loaded_weapon))
        if (const DataField* f = w->Get(58)) rof = static_cast<uint8_t>(f->v.i);
    // Live retail capture (2026-10-09, empty magazine): graph 4 {32->32} and
    // ConfirmReload() at once; RoundIndex 0 + magazine counts at +1.8s;
    // RoundIndex 1, ChamberedRound[0], bBoltLockedToRear False at +3.7s;
    // ready {79->79} at +4.9s.
    pawn->SetGraph(4, 32, 32);
    ++pawn->confirm_reload_serial;
    bool empty = true;                           // nothing chambered: the bolt is back
    if (ServerRoundActor* w = FindInventory(pawn, pawn->loaded_weapon))
        if (const DataField* c = w->Get(57, 0)) empty = c->v.kind != DataValue::Kind::kObj;
    deliberate_reloads_.push_back({pawn, clock_, 0, rof, empty});
}

ServerRoundActor* World::FindInventory(const ServerPawn* pawn, const ServerActor* a) {
    auto it = pawn_inventory_.find(pawn);
    if (it == pawn_inventory_.end() || !a) return nullptr;
    for (auto& item : it->second) if (item.get() == a) return item.get();
    return nullptr;
}

void World::BeginInitialReload(ServerPawn* pawn, const ServerActor* weapon) {
    if (!pawn || !weapon || pawn->loaded_weapon == weapon) return;
    for (const auto& j : reloads_) if (j.pawn == pawn && j.weapon == weapon) return;
    reloads_.push_back({pawn, weapon, clock_, 0});
}

// The magazine change of a reload: the pouch's first magazine goes into the
// weapon the way the initial reload loads one (shown, Inventory None,
// AttachedActor the weapon, LoadedMagazine[0], ClientLoadAmmoMag via
// loaded_mag_serial); the old one goes hidden to the back of the pouch.
// The client counts each magazine's rounds itself, so a swap is what gives
// it ammo (doc sec 31).
bool World::SwapMagazine(ServerPawn* pawn, ServerRoundActor* weapon, ServerRoundActor* old_mag) {
    constexpr uint32_t kHidden = 13, kHiddenByInvManager = 23, kInventory = 25,
                       kAttachedActor = 28, kInventoryChain = 29, kLoadedMagazine = 34;
    ServerRoundActor* pouch = FindInventory(pawn, pawn->dump_pouch);
    if (!pouch || !weapon) return false;
    const DataField* head = pouch->Get(kInventoryChain);
    ServerRoundActor* next = head && head->v.kind == DataValue::Kind::kActor
        ? FindInventory(pawn, head->v.actor) : nullptr;
    if (!next || next->class_name.rfind("AA_AmmoContainer", 0) != 0) return false;
    const DataField* after = next->Get(kInventory);
    pouch->Set(kInventoryChain, -1, after ? after->v : DataValue::Actor(nullptr));
    next->Set(kHidden, -1, DataValue::Bool(false));
    next->Set(kHiddenByInvManager, -1, DataValue::Bool(false));
    next->Set(kInventory, -1, DataValue::Actor(nullptr));
    next->Set(kAttachedActor, -1, DataValue::Actor(weapon));
    if (old_mag && old_mag != next) {
        old_mag->Set(kInventory, -1, DataValue::Actor(nullptr));
        old_mag->Set(kAttachedActor, -1, DataValue::Actor(pouch));
        old_mag->Set(kHidden, -1, DataValue::Bool(true));
        old_mag->Set(kHiddenByInvManager, -1, DataValue::Bool(true));
        // Append to the end of the pouch's chain.
        ServerRoundActor* last = nullptr;
        const DataField* link = pouch->Get(kInventoryChain);
        for (int guard = 0; guard < 64 && link && link->v.kind == DataValue::Kind::kActor; ++guard) {
            last = FindInventory(pawn, link->v.actor);
            if (!last) break;
            link = last->Get(kInventory);
        }
        if (last) last->Set(kInventory, -1, DataValue::Actor(old_mag));
        else pouch->Set(kInventoryChain, -1, DataValue::Actor(old_mag));
    }
    weapon->Set(kLoadedMagazine, 0, DataValue::Actor(next));
    pawn->loaded_mag = next;
    ++pawn->loaded_mag_serial;
    return true;
}

void World::UpdateReloads() {
    for (auto it = deliberate_reloads_.begin(); it != deliberate_reloads_.end();) {
        ServerPawn* pawn = it->pawn;
        ServerRoundActor* weapon = FindInventory(pawn, pawn->loaded_weapon);
        ServerRoundActor* mag = FindInventory(pawn, pawn->loaded_mag);
        double t = clock_ - it->start;
        auto set_rof = [&](uint8_t v) {
            if (!weapon) return;
            DataValue rof; rof.kind = DataValue::Kind::kByte; rof.bits = 2; rof.i = v;
            weapon->Set(58, -1, rof);                    // AA_BulletWeapon.CurrentROF
        };
        // The client tracks rounds, chamber and bolt itself; the server's
        // copies never moved, so everything is re-sent (Touch) even when
        // unchanged.
        auto touch = [](ServerRoundActor* a, uint32_t idx, int32_t elem, const DataValue& v) {
            if (!a) return;
            a->Set(idx, elem, v);
            a->Touch(idx, elem);
        };
        // Advanced inventory (AA_Weapon.AdvancedReplaceAmmoMag; retail Bridge
        // AI capture 2026-10-12, user t=350.26-351.29): the loaded magazine
        // goes back to the PRI's list with what it has left (index -1);
        // 0.4 s on, the fullest one's rounds go into the same magazine actor
        // (RoundIndex = RoundCount - rounds) with the magazine count; 1 s
        // after the dump it's flagged in the weapon. Nothing is thrown away.
        auto* pri = pawn->pri;
        if (it->stage == 0 && t >= 0.4) { set_rof(0); it->stage = 1; }
        if (it->stage == 1 && t >= 1.4) {
            if (pri && pri->primary_mag_index >= 0 && pri->primary_mag_index < ServerPlayerReplicationInfo::kMaxMagazines) {
                auto& cur = pri->primary_mags[static_cast<size_t>(pri->primary_mag_index)];
                const DataField* ri = mag ? mag->Get(29) : nullptr;
                cur.rounds = std::max<int32_t>(0, pawn->mag_rounds - static_cast<int32_t>(ri ? ri->v.i : pawn->mag_rounds));
                cur.in_weapon = false;
                pri->primary_mag_index = -1;
            }
            it->stage = 2;
        }
        if (it->stage == 2 && t >= 1.8) {
            int best = -1, count = 0;
            if (pri)
                for (int i = 0; i < ServerPlayerReplicationInfo::kMaxMagazines; ++i) {
                    const auto& m = pri->primary_mags[static_cast<size_t>(i)];
                    if (m.in_weapon || m.rounds <= 0) continue;
                    ++count;
                    if (best < 0 || m.rounds > pri->primary_mags[static_cast<size_t>(best)].rounds) best = i;
                }
            it->chosen = best;
            if (best >= 0)                                // AA_AmmoContainerBase.RoundIndex
                touch(mag, 29, -1, DataValue::Int(pawn->mag_rounds - pri->primary_mags[static_cast<size_t>(best)].rounds));
            if (pri) {
                int total = 0;
                for (const auto& m : pri->primary_mags) total += m.rounds;
                LOG_INFO("Reload %s: loads magazine %d (%d rounds); %d magazines, %d rounds in all",
                         pri->player_name.c_str(), best, best >= 0 ? pri->primary_mags[static_cast<size_t>(best)].rounds : 0,
                         count, total);
            }
            pawn->spare_mags = count;                     // ClientAdvancedInventoryInitializeAmmoMagazineCount
            ++pawn->mag_count_serial;
            it->stage = 3;
        }
        if (it->stage == 3 && t >= 2.43) {
            if (pri && it->chosen >= 0) {
                pri->primary_mags[static_cast<size_t>(it->chosen)].in_weapon = true;
                pri->primary_mag_index = it->chosen;
            }
            it->stage = 4;
        }
        if (it->stage == 4 && t >= 3.7) {
            // A round goes into an empty chamber only (live retail capture
            // 2026-10-09, empty magazine); with one chambered, nothing moves.
            if (it->empty && it->chosen >= 0) {
                const DataField* ri = mag ? mag->Get(29) : nullptr;
                touch(mag, 29, -1, DataValue::Int(static_cast<int32_t>(ri ? ri->v.i : 0) + 1));
                DataValue round; round.kind = DataValue::Kind::kObj; round.s = "AA3Baseline";
                round.net = 1044;                         // AA_AmmoType_556mmBall
                touch(weapon, 57, 0, round);              // ChamberedRound[0]
                touch(weapon, 62, -1, DataValue::Bool(false)); // bBoltLockedToRear
            }
            it->stage = 5;
        }
        if (it->stage == 5 && t >= 4.8) { set_rof(pawn->sprinting ? 0 : pawn->selected_rof); it->stage = 6; }
        if (it->stage == 6 && t >= 4.9) { pawn->SetGraph(4, 79, 79); it = deliberate_reloads_.erase(it); continue; }
        ++it;
    }

    // Retail GamePlayPCAP_Legit_Server, equip at t=22.05 (doc sec 30):
    //   t=23.49 magazine: bHidden=False, bHiddenByInvManager=False,
    //           AttachedActor=None (out of the dump pouch)
    //   t=24.63 rifle: ClientLoadAmmoMag(mag), LoadedMagazine[0]=mag,
    //           CurrentAmmoType=AA_AmmoType_556mmBall; magazine:
    //           Inventory=None, AttachedActor=rifle; dump pouch:
    //           InventoryChain = the magazine's old next item
    //   t=27.53 rifle: ChamberedRound[0] = AA_AmmoType_556mmBall
    //   t=28.83 pawn: weapon graph 4 -> 79 (ready)
    constexpr uint32_t kHidden = 13, kHiddenByInvManager = 23, kInventory = 25,
                       kAttachedActor = 28, kInventoryChain = 29,
                       kLoadedMagazine = 34, kCurrentAmmoType = 35,
                       kChamberedRound = 57;                 // AA_BulletWeapon
    constexpr uint32_t kAmmoType556Ball = 1044;   // class AA_AmmoType_556mmBall (AA3Baseline)
    for (auto it = reloads_.begin(); it != reloads_.end();) {
        ServerPawn* pawn = it->pawn;
        ServerRoundActor* pouch = FindInventory(pawn, pawn->dump_pouch);
        ServerRoundActor* weapon = FindInventory(pawn, it->weapon);
        const DataField* chain = pouch ? pouch->Get(kInventoryChain) : nullptr;
        ServerRoundActor* mag = chain && chain->v.kind == DataValue::Kind::kActor
            ? FindInventory(pawn, chain->v.actor) : nullptr;
        bool have_mag = mag && mag->class_name.rfind("AA_AmmoContainer", 0) == 0;
        if (!weapon || (it->stage < 4 && (!pouch || !have_mag))) {
            it = reloads_.erase(it);    // nothing to load
            continue;
        }
        double t = clock_ - it->start;
        // Equip (retail frames 376/378/395): ActiveInventory at once, the
        // rifle shown at +0.04s, moved from the chest slot to the pawn at +0.24s.
        if (it->stage == 0) {
            pawn->active_inventory = weapon;
            weapon->Set(kHidden, -1, DataValue::Bool(false));
            pawn->SetGraph(4, 150, 79);                    // weapon graph: draw
            it->stage = 1;
        }
        if (it->stage == 1 && t >= 0.24) {
            weapon->Set(kAttachedActor, -1, DataValue::Actor(pawn));
            it->stage = 2;
        }
        if (!it->reloading && t >= 0.48) {                 // initial reload begins
            pawn->SetGraph(3, 11, 0);
            pawn->SetGraph(4, 47, 47);
            it->reloading = true;
        }
        if (it->stage == 2 && t >= 1.44) {
            mag->Set(kHidden, -1, DataValue::Bool(false));
            mag->Set(kHiddenByInvManager, -1, DataValue::Bool(false));
            mag->Set(kAttachedActor, -1, DataValue::Actor(nullptr));
            it->stage = 3;
        }
        if (it->stage == 3 && t >= 2.58) {
            const DataField* next = mag->Get(kInventory);
            pouch->Set(kInventoryChain, -1, next ? next->v : DataValue::Actor(nullptr));
            mag->Set(kInventory, -1, DataValue::Actor(nullptr));
            mag->Set(kAttachedActor, -1, DataValue::Actor(weapon));
            weapon->Set(kLoadedMagazine, 0, DataValue::Actor(mag));
            DataValue ammo;
            ammo.kind = DataValue::Kind::kObj;
            ammo.s = "AA3Baseline";
            ammo.net = kAmmoType556Ball;
            weapon->Set(kCurrentAmmoType, -1, ammo);
            pawn->loaded_weapon = weapon;
            pawn->loaded_mag = mag;
            ++pawn->loaded_mag_serial;
            if (pawn->pri) {                               // retail frame 522: entry 0 in the weapon
                pawn->pri->mag_actor = mag;
                pawn->pri->primary_mags[0].in_weapon = true;
                pawn->pri->primary_mag_index = 0;
            }
            it->stage = 4;
        }
        if (it->stage == 4 && t >= 5.48) {                 // first round chambered (retail frame 774)
            DataValue round;
            round.kind = DataValue::Kind::kObj;
            round.s = "AA3Baseline";
            round.net = kAmmoType556Ball;
            weapon->Set(kChamberedRound, 0, round);
            if (ServerRoundActor* m = FindInventory(pawn, pawn->loaded_mag))
                m->Set(29, -1, DataValue::Int(1));        // RoundIndex 1 (retail t=28.75)
            it->stage = 5;
        }
        if (it->stage == 5 && t >= 6.78) {                 // initial reload done: weapon ready
            pawn->first_reload_done = true;
            SetWeaponROF(*pawn, pawn->selected_rof);      // off Safe (retail t=29.75)
            pawn->SetGraph(4, 79, 79);
            it = reloads_.erase(it);
            continue;
        }
        ++it;
    }
}

uint32_t World::HUDClassIndex() const {
    return GameIndex(net_index::kHUDClass);
}

bool World::BindPackageMap(const engine::MapManifest& manifest,
                           const std::string& game_mode_level,
                           const engine::MapBundle* bundle) {
    auto baseline = manifest.ObjectBase("AA3Baseline");
    auto game = manifest.ObjectBase("AA3Game");
    if (!baseline || !game) {
        LOG_ERROR("Map manifest has no net object counts for the packages before "
                  "AA3Baseline/AA3Game; actor ObjectIndex values can't be resolved. "
                  "Regenerate it with aa3_build_manifests --cooked-root.");
        return false;
    }
    base_aa3baseline_ = *baseline;
    base_aa3game_ = *game;
    package_bases_.clear();
    for (const auto& u : manifest.uses)
        if (auto b = manifest.ObjectBase(u.pkg)) package_bases_[u.pkg] = *b;
    if (gri_) {
        gri_->object_index = BaselineIndex(net_index::kGRI);
        gri_->game_class_object_index = GameIndex(net_index::kGameInfoClass);
    }
    std::string effective_level = game_mode_level;
    if (effective_level.empty() && bundle && !bundle->GameModeLevel().empty()) {
        effective_level = bundle->GameModeLevel();
    }
    ConfigureGameMode(manifest, effective_level);
    if (!game_mode_level_.empty()) {
        // Rules first: their team mission types decide assault/defend on
        // the TeamInfos the round actors fill in.
        std::string rules_json;
        if (bundle && bundle->GetMissionRulesJson(rules_json)) {
            LoadMissionRulesFromJson(rules_json);
        } else if (bundle) {
            // No mission in the bundle (training levels): don't show Bridge
            // classic's mission text (the GRI defaults).
            if (gri_) {
                gri_->s_mission_name = bundle->MapName();
                gri_->s_summary = bundle->MapName();
                gri_->s_author.clear();
                gri_->s_type.clear();
                gri_->mission_briefings = {"", ""};
            }
            LOG_INFO("No mission rules in the bundle for %s", game_mode_level_.c_str());
        } else {
            LoadMissionRules("config/maps/" + game_mode_level_ + ".rules.json");
        }
        std::string round_json;
        if (bundle && bundle->GetActorJson(engine::SectionType::kRoundActors, round_json)) {
            LoadRoundActorsFromJson(round_json);
        } else {
            LoadRoundActors("config/maps/" + game_mode_level_ + ".round.json");
        }
    }
    {
        std::vector<ActorTemplate> placed;
        std::string map_act_json;
        bool have_actors = false;
        if (bundle && bundle->GetActorJson(engine::SectionType::kMapActors, map_act_json)) {
            have_actors = ReadActorTemplatesFromJson(map_act_json, "map-actor", placed);
        } else {
            have_actors = ReadActorTemplates("config/maps/" + manifest.url + ".map_actors.json", "map-actor", placed);
        }
        if (have_actors) {
            map_actors_ = Instantiate(placed, {});
            // The level's WorldInfo (retail sends its WorldGravityZ, e.g.
            // Bridge -750): grenades and 40 mm rounds fall by it on every
            // client, so the server's flights must too.
            for (const auto& a : map_actors_)
                if (a->class_name == "WorldInfo")
                    if (const DataField* gz = a->Get(24))
                        if (gz->v.kind == DataValue::Kind::kFloat && world_info_) {
                            world_info_->world_gravity_z = gz->v.f;
                            LOG_INFO("World gravity %.0f (the level's WorldInfo)", gz->v.f);
                        }
            LOG_INFO("Map actors: %zu from %s", map_actors_.size(),
                     bundle ? bundle->MapName().c_str() : manifest.url.c_str());
        }
    }
    if (!game_mode_level_.empty()) {
        std::vector<ActorTemplate> t;
        std::string act_json;
        if (bundle && bundle->GetActorJson(engine::SectionType::kJoinActors, act_json)) {
            if (ReadActorTemplatesFromJson(act_json, "join-actor", t))
                join_actors_ = Instantiate(t, {});
        } else if (ReadActorTemplates("config/maps/" + game_mode_level_ + ".join.json", "join-actor", t)) {
            join_actors_ = Instantiate(t, {});
        }
        SetupCarriedObjective();

        t.clear();
        if (bundle && bundle->GetActorJson(engine::SectionType::kMatchActors, act_json)) {
            if (ReadActorTemplatesFromJson(act_json, "match-actor", t))
                match_actors_ = Instantiate(t, {});
        } else if (ReadActorTemplates("config/maps/" + game_mode_level_ + ".match.json", "match-actor", t)) {
            match_actors_ = Instantiate(t, {});
        }
        LOG_INFO("Mission actors: %zu at join, %zu at match start", join_actors_.size(),
                 match_actors_.size());
        match_actor_start_.clear();
        for (const auto& a : match_actors_) match_actor_start_.push_back(a->fields);

    }
    // Breakable windows: map NetIndex -> manager bit (export_windows.py).
    window_index_.clear();
    if (bundle && bundle->GetWindows(window_index_)) {
        LOG_INFO("Breakable windows: %zu from bundle", window_index_.size());
    } else {
        std::ifstream wf("config/maps/" + manifest.url + ".windows.json");
        std::stringstream ws;
        ws << wf.rdbuf();
        if (auto doc = util::ParseJson(ws.str()))
            for (const auto& [k, v] : (*doc)["windows"].obj)
                window_index_[static_cast<uint32_t>(std::stoul(k))] = static_cast<int>(v.Int());
        LOG_INFO("Breakable windows: %zu", window_index_.size());
    }
    // Map collision (doc sec 32):
    const uint8_t* col_ptr = nullptr;
    size_t col_size = 0;
    if (bundle && bundle->GetCollisionData(col_ptr, col_size)) {
        collision_.LoadFromMemory(col_ptr, col_size, bundle->MapName());
    } else {
        collision_.Load("data/collision/" + manifest.url + ".col");
    }
    StartNavBuild();                            // NPC routes (AI missions), in the background
    // Every spawn gets retail's Fireteam Leader kit for now (doc sec 30).
    std::string inv_json;
    if (bundle && bundle->GetActorJson(engine::SectionType::kSpawnInventory, inv_json)) {
        if (ReadActorTemplatesFromJson(inv_json, "spawn-inventory", inventory_template_))
            LOG_INFO("Spawn inventory: %zu actors per player from bundle", inventory_template_.size());
    } else if (ReadActorTemplates("config/spawn_inventory.json", "spawn-inventory", inventory_template_)) {
        LOG_INFO("Spawn inventory: %zu actors per player", inventory_template_.size());
    }
    for (auto& t : teams_) t.object_index = BaselineIndex(net_index::kTeamInfo);
    LOG_INFO("PackageMap: ObjectBase AA3Baseline=%u AA3Game=%u -> GRI=%u TeamInfo=%u "
             "PlayerController=%u", base_aa3baseline_, base_aa3game_,
             BaselineIndex(net_index::kGRI), BaselineIndex(net_index::kTeamInfo),
             GameIndex(net_index::kPlayerController));
    return true;
}

// Build the dynamic actor set the server spawns at boot, mirroring what
// retail AA_GameInfo does during InitGame() / PostBeginPlay(). ObjectIndex
// values come from the legit-server pcap so the client resolves them to
// the same classes via the shared PackageMap.
void World::InitializeActors() {
    // WorldInfo is placed in the level package; its real ObjectIndex is
    // unknown and it isn't opened as a channel (doc sec 26). 172588, which
    // this used to be, is Default__AGP_PlayerController in retail's map.
    // GRI/TeamInfo indices are set by BindPackageMap.
    constexpr uint32_t kObjIdxWorldInfo = 0;
    const uint32_t kObjIdxGRI      = BaselineIndex(net_index::kGRI);
    const uint32_t kObjIdxTeamInfo = BaselineIndex(net_index::kTeamInfo);

    // WorldInfo — placed in the level package, not server-spawned. We hold
    // a server-side mirror so the network layer can read its replicated
    // fields and ship them on Ch=2.
    world_info_ = std::make_unique<ServerWorldInfo>();
    world_info_->object_index  = kObjIdxWorldInfo;
    world_info_->transient     = false;
    world_info_->label         = "WorldInfo (" + map_name_ + ")";
    world_info_->time_dilation = 1.0f;
    world_info_->world_gravity_z = -520.0f;

    // GameInfo — server-only, no client replica. Holds rules + spawn logic.
    game_info_ = std::make_unique<ServerGameInfo>();
    if      (map_name_.find("_inf_classic_") != std::string::npos)
        game_info_->class_name = "AGP_Game_INF_Classic";
    else if (map_name_.find("_inf_co_") != std::string::npos)
        game_info_->class_name = "AGP_Game_INF_CO";
    else if (map_name_.find("_inf_tah_") != std::string::npos)
        game_info_->class_name = "AGP_Game_INF_TAH";
    else if (map_name_.find("_inf_vip_") != std::string::npos)
        game_info_->class_name = "AGP_Game_INF_VIP";
    else if (map_name_.find("_inf_ao_") != std::string::npos)
        game_info_->class_name = "AGP_Game_INF_AO";
    else if (map_name_.find("_inf_ad_") != std::string::npos)
        game_info_->class_name = "AGP_Game_INF_AD";
    else if (map_name_.find("_bct_") != std::string::npos)
        game_info_->class_name = "AGP_Game_BCT";
    // base-map default keeps the configured value above.

    // GRI — server-spawned at boot, replicates to clients on Ch=4.
    gri_ = std::make_unique<ServerGameReplicationInfo>();
    gri_->object_index = kObjIdxGRI;
    gri_->transient    = true;   // server-spawned from class default
    gri_->label        = "AA_GameReplicationInfo"; 
    gri_->server_name  = "AA3 Server"; // This should be variable based on the config
    gri_->s_base_map_name  = map_name_;
    gri_->s_mission_type   = game_info_->class_name;
    gri_->s_tod            = tod_;
    gri_->b_match_has_begun = false;
    // GameReplicationInfo.TimeLimit is in minutes (retail: 7).
    gri_->time_limit        = game_info_->default_time_limit_seconds / 60;
    gri_->remaining_time    = game_info_->default_time_limit_seconds;
    gri_->remaining_minute  = gri_->time_limit;
    gri_->mission_time_remaining = game_info_->default_time_limit_seconds;

    // 4 TeamInfos — Allies, Enemy, Neutral, Spectator. Retail emits four //DevNote: 3 are used, normally Spectator is automatically assigned to newly joined players, 
    // until they choose a side. 
    // even when only two are used so the client's UI knows about all slots.
    teams_.reserve(4);
    // Retail's TeamInfos carry TeamIndex 0, 1, 3, 4 (frame 73-74; doc sec 27).
    static const char* kTeamNames[4] = {"Black Team", "Gold Team", "Neutral", "Spectator"};
    static const int32_t kTeamIndices[4] = {0, 1, 3, 4};
    for (int i = 0; i < 4; ++i) {
        ServerTeamInfo t;
        t.object_index = kObjIdxTeamInfo;  // same class for all 4
        t.transient    = true;
        t.team_index   = kTeamIndices[i];
        t.team_name    = kTeamNames[i];
        t.label        = std::string("TeamInfo[") + std::to_string(i) + "]";
        teams_.push_back(t);
    }
    for (size_t i = 0; i < teams_.size() && i < 4; ++i) {
        gri_->teams[i] = &teams_[i];
    }
}

void World::Update(double delta_time) {
    player_manager_.Update(delta_time);
    clock_ += delta_time;
    UpdateMatch(delta_time);
    UpdateReloads();
    UpdateGrenades(delta_time);
    UpdateLauncher();
    while (!sound_events_.empty() && clock_ - sound_events_.front().at > 2.0) sound_events_.pop_front();
    UpdateInjuries(delta_time);
    UpdatePostures(delta_time);
    UpdateIncapacitated(delta_time);
    UpdateDoors();
    UpdateObjective();
    UpdateWorldItems();
    if (phase_ == MatchPhase::kInProgress) UpdateNPCs(delta_time);

    if (world_info_) {
        world_info_->real_time_seconds += static_cast<float>(delta_time);
    }
    if (gri_ && gri_->b_match_has_begun && !gri_->b_match_is_over) {
        gri_->second_count += static_cast<float>(delta_time);
        while (gri_->second_count >= 1.0f) {
            gri_->second_count   -= 1.0f;
            gri_->elapsed_time   += 1;
            gri_->remaining_time = std::max(0, gri_->remaining_time - 1);
            // RemainingMinute stays: retail never updates it (no samples).
        }
    }
}

ServerPlayerController* World::CreatePlayerController(uint32_t owning_client_id,
                                                     const std::string& player_name) {
    auto pri = std::make_unique<ServerPlayerReplicationInfo>();
    pri->object_index   = BaselineIndex(net_index::kPRI);
    pri->transient      = true;
    pri->player_name    = player_name.empty() ? "Player" + std::to_string(owning_client_id)
                                              : player_name;
    pri->player_id      = static_cast<int32_t>(next_player_id_++);
    // New players start on the Spectator team (TeamInfo index 4), as on a
    // retail server. With PRI.Team None the real client's team menu showed
    // both teams FULL until the player clicked Spectator (doc sec 28).
    pri->team           = FindTeam(4);
    pri->b_is_spectator = pri->team != nullptr;
    pri->team_index     = -1;            // unassigned until select-team
    pri->label          = "PRI:" + pri->player_name;
    ServerPlayerReplicationInfo* pri_ptr = pri.get();
    pris_.push_back(std::move(pri));

    auto pc = std::make_unique<ServerPlayerController>();
    // AGP_PlayerController (AA3Game.u) -- the class retail's AGP_GameInfo
    // spawns, on retail Ch=2. Was AA_PlayerController, the base class the
    // AA3Game script RPCs aren't on (doc sec 26).
    pc->object_index     = GameIndex(net_index::kPlayerController);

    pc->transient        = true;
    pc->owning_client_id = owning_client_id;
    pc->net_player_index = 0;            // single-screen client
    pc->observed.connected_at_unix = static_cast<uint64_t>(std::time(nullptr));
    pc->pri              = pri_ptr;
    pc->label            = "PC:" + pri_ptr->player_name;
    ServerPlayerController* pc_ptr = pc.get();
    controllers_.push_back(std::move(pc));

    // No Pawn here: it spawns at match start (World::SpawnPawn; doc sec 29),
    // as in retail, where the player sits in the menus until then.

    // One AA_SteamStatManager per player (Ch=3). Its CDO, not the class
    // export (docs/AA3_ACTOR_REPLICATION.md sec 12.8).
    auto stat_mgr = std::make_unique<ServerSteamStatManager>();
    stat_mgr->object_index = BaselineIndex(net_index::kSteamStatMgr);
    stat_mgr->transient     = true;
    stat_mgr->label         = "SteamStatManager:" + pri_ptr->player_name;
    ServerSteamStatManager* stat_mgr_ptr = stat_mgr.get();
    stat_managers_.push_back(std::move(stat_mgr));
    pc_ptr->stat_manager = stat_mgr_ptr;

    // One AA_MOSQualificationBCT per player (Ch=5; doc sec 14).
    auto mos = std::make_unique<ServerMOSQualificationBCT>();
    mos->object_index = BaselineIndex(net_index::kMOSQualBCT);
    mos->transient      = true;
    mos->label          = "MOSQualBCT:" + pri_ptr->player_name;
    ServerMOSQualificationBCT* mos_ptr = mos.get();
    mos_quals_.push_back(std::move(mos));
    pc_ptr->mos_qual = mos_ptr;
    for (uint32_t cdo : net_index::kMOSQualExtra) {
        auto q = std::make_unique<ServerMOSQualificationBCT>();
        q->object_index = BaselineIndex(cdo);
        q->transient    = true;
        q->label        = "MOSQual:" + pri_ptr->player_name;
        pc_ptr->extra_mos_quals.push_back(q.get());
        mos_quals_.push_back(std::move(q));
    }

    auto proxy = std::make_unique<ServerPlayerStorageProxy>();
    proxy->object_index = BaselineIndex(net_index::kPlayerStorageProxy);
    proxy->transient    = true;
    proxy->controller   = pc_ptr;
    proxy->pri          = pri_ptr;
    proxy->label        = "PlayerStorageProxy:" + pri_ptr->player_name;
    pc_ptr->proxy = proxy.get();
    proxies_.push_back(std::move(proxy));

    return pc_ptr;
}

void World::RemovePlayerController(uint32_t owning_client_id) {
    if (ServerPlayerController* pc = FindPlayerController(owning_client_id)) {
        if (pc->pri) LeaveTeam(*pc->pri);
        // The player's soldier leaves with them: torn off (clients keep the
        // body briefly, channels close) and unhooked from the controller/PRI
        // being destroyed, so traces and the round never see a ghost.
        if (pc->pawn) {
            ServerPawn* pawn = pc->pawn;
            pawn->torn_off = true;
            pawn->health = 0.0f;
            pawn->controller = nullptr;
            pawn->pri = nullptr;
            pawn->last_known_pri = nullptr;
            pawn->proxy = nullptr;
            pc->pawn = nullptr;
        }
    }
    controllers_.erase(std::remove_if(
        controllers_.begin(), controllers_.end(),
        [&](const std::unique_ptr<ServerPlayerController>& p) {
            return p->owning_client_id == owning_client_id;
        }), controllers_.end());
}

ServerPlayerController* World::FindPlayerController(uint32_t owning_client_id) {
    for (auto& p : controllers_) {
        if (p->owning_client_id == owning_client_id) return p.get();
    }
    return nullptr;
}

} // namespace game
} // namespace aa3
