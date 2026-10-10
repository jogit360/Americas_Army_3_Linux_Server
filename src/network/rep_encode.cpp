#include "network/rep_encode.h"
#include <algorithm>
#include <cstring>

namespace aa3 {
namespace network {

using util::BitWriter;
using game::ServerActor;
using game::ServerWorldInfo;
using game::ServerGameReplicationInfo;
using game::ServerTeamInfo;
using game::ServerPlayerReplicationInfo;
using game::ServerPlayerController;
using game::ServerPawn;
using game::ServerSteamStatManager;

// FloatProperty (doc sec 10.4): `Ar << *(FLOAT*)Data` — 32 raw bits, IEEE-754
// bit pattern, same LSB-first raw serialization BitWriter::WriteInt already
// uses for IntProperty (`Ar << *(INT*)Data`). Not RIM-encoded.
static void WriteFloatProp(BitWriter& w, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    w.WriteInt(bits);
}

// StrProperty (doc sec 10.4, UnMisc.cpp operator<<(FArchive&,FString&)):
// SaveNum:INT32 (sign = ANSI(+)/UNICODE(-), magnitude = char count incl.
// null), then |SaveNum| raw chars. We only ever write ANSI. This is the same
// wire shape as the handshake/storm FString records (protocol_handler.cpp's
// WriteFString) — duplicated here rather than shared to keep this file
// decoupled from the handshake code.
static void WriteStrProp(BitWriter& w, const std::string& s) {
    w.WriteInt(static_cast<uint32_t>(s.length() + 1));
    for (char c : s) w.WriteByte(static_cast<uint8_t>(c));
    w.WriteByte(0);
}

// ObjectProperty/ClassProperty reference. UPackageMapLevel::SerializeObject
// (UnNetDrv.cpp) writes NULL and every spawned actor as bDynamic=1 +
// RIM(channel, MAX_CHANNELS); only static/package objects use bDynamic=0 +
// RIM(ObjectIndex, 2^31). Every reference this server sends is NULL or a
// spawned actor, so this is the only form needed (doc sec 21 -- the old
// bDynamic=0 + CDO ObjectIndex form pointed clients at class defaults).
void WriteActorRef(BitWriter& w, uint32_t channel) {
    w.WriteBit(1);
    w.WriteIntMax(channel, 0x800);
}

namespace {

// Shared reference-field logic: resolve the actor to this connection's
// channel, and send if this is the initial send or the wire value differs
// from what was last sent for this RepIndex (so a NULL sent before the
// target's channel opened gets corrected once it does).
struct RefWriter {
    BitWriter& w;
    const RepContext& ctx;
    uint32_t max_index;
    bool initial;
    uint32_t& n;
    void operator()(uint32_t rep_index, const ServerActor* a) {
        uint32_t wire = (a && ctx.channel_of) ? ctx.channel_of(a) : 0;
        bool changed = initial;
        if (!changed && ctx.sent_refs) {
            auto it = ctx.sent_refs->find(rep_index);
            changed = it == ctx.sent_refs->end() || it->second != wire;
        }
        if (!changed) return;
        w.WriteIntMax(rep_index, max_index);
        WriteActorRef(w, wire);
        if (ctx.sent_refs) (*ctx.sent_refs)[rep_index] = wire;
        ++n;
    }
};

}  // namespace

uint32_t EncodeWorldInfoRep(BitWriter& w, const ServerWorldInfo& cur,
                            const ServerWorldInfo* prev) {
    uint32_t n = 0;

    // RepIndex 23 ReplicatedMusicTrack (MusicTrackStruct) — SKIPPED, see
    // rep_encode.h header comment (struct type mismatch, not modeled).

    // RepIndex 24 WorldGravityZ : FloatProperty
    if (prev == nullptr || cur.world_gravity_z != prev->world_gravity_z) {
        w.WriteIntMax(rep::WorldInfo::WorldGravityZ, kWorldInfoMaxIndex);
        WriteFloatProp(w, cur.world_gravity_z);
        ++n;
    }

    // RepIndex 25 Pauser : ObjectProperty — NULL-only, initial send only
    // (never "changes" in our model, so only fires on the full/open send).
    if (prev == nullptr) {
        w.WriteIntMax(rep::WorldInfo::Pauser, kWorldInfoMaxIndex);
        WriteActorRef(w, 0);
        ++n;
    }

    // RepIndex 26 TimeDilation : FloatProperty
    if (prev == nullptr || cur.time_dilation != prev->time_dilation) {
        w.WriteIntMax(rep::WorldInfo::TimeDilation, kWorldInfoMaxIndex);
        WriteFloatProp(w, cur.time_dilation);
        ++n;
    }

    // RepIndex 27 bHighPriorityLoading : BoolProperty (1 bit)
    if (prev == nullptr || cur.b_high_priority_loading != prev->b_high_priority_loading) {
        w.WriteIntMax(rep::WorldInfo::bHighPriorityLoading, kWorldInfoMaxIndex);
        w.WriteBit(cur.b_high_priority_loading);
        ++n;
    }

    return n;
}

uint32_t EncodeGRIRep(BitWriter& w, const ServerGameReplicationInfo& cur,
                      const ServerGameReplicationInfo* prev,
                      std::vector<uint32_t>* field_ends) {
    uint32_t n = 0;
    const uint32_t M = kGRIMaxIndex;

    auto int_field = [&](uint32_t rep_index, int32_t cur_v, const int32_t* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteInt(static_cast<uint32_t>(cur_v));
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    };
    auto bool_field = [&](uint32_t rep_index, bool cur_v, const bool* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteBit(cur_v);
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    };
    auto str_field = [&](uint32_t rep_index, const std::string& cur_v, const std::string* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            WriteStrProp(w, cur_v);
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    };

    // --- Engine.GameReplicationInfo (RepIndex 23-42) ---
    int_field(rep::GRI::MatchID, cur.match_id, prev ? &prev->match_id : nullptr);

    // RepIndex 24 Winner : ObjectProperty — NULL-only, initial send only
    // (see rep_encode.h gap notes; `winner` here doesn't hold a real
    // object reference, so we can't represent a non-null value).
    if (prev == nullptr) {
        w.WriteIntMax(rep::GRI::Winner, M);
        WriteActorRef(w, 0);
        ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
    }
    // RepIndex 25 MessageOfTheDay, 26 ServerRegion — not modeled, skipped.

    // Engine GameReplicationInfo: server name, limits, RemainingTime and
    // ElapsedTime go only with the open (bNetInitial); RemainingMinute only
    // after it (!bNetInitial && bNetDirty) -- retail sends no per-second
    // GRI clock, only AA3's iMissionTimeRemaining (doc sec 36).
    if (prev == nullptr) {
        str_field(rep::GRI::AdminEmail, cur.admin_email, nullptr);
        str_field(rep::GRI::AdminName, cur.admin_name, nullptr);
        str_field(rep::GRI::ShortName, cur.short_name, nullptr);
        str_field(rep::GRI::ServerName, cur.server_name, nullptr);
        int_field(rep::GRI::MaxLives, cur.max_lives, nullptr);
        int_field(rep::GRI::TimeLimit, cur.time_limit, nullptr);
        int_field(rep::GRI::GoalScore, cur.goal_score, nullptr);
        int_field(rep::GRI::ElapsedTime, cur.elapsed_time, nullptr);
        int_field(rep::GRI::RemainingTime, cur.remaining_time, nullptr);
    } else {
        int_field(rep::GRI::RemainingMinute, cur.remaining_minute, &prev->remaining_minute);
    }
    // RepIndex 37 bTrackStats, 38 bIsArbitrated — not modeled, skipped.
    bool_field(rep::GRI::bMatchIsOver, cur.b_match_is_over, prev ? &prev->b_match_is_over : nullptr);
    bool_field(rep::GRI::bMatchHasBegun, cur.b_match_has_begun, prev ? &prev->b_match_has_begun : nullptr);
    bool_field(rep::GRI::bStopCountDown, cur.b_stop_count_down, prev ? &prev->b_stop_count_down : nullptr);
    // RepIndex 42 GameClass (ClassProperty) — not modeled, skipped.

    // --- AA3Baseline.AA_GameReplicationInfo (RepIndex 43-132) ---
    // Only the subset server_actors.h actually models with a confirmed
    // real-property match (checked against the full 133-field
    // `--rep-layout` dump — see rep_encode.h for the fields that turned
    // out NOT to exist: second_count, b_show_hud_timer,
    // b_role_swapping_disabled).
    int_field(rep::GRI::iVIPRawTeamIndex, cur.i_vip_raw_team_index, prev ? &prev->i_vip_raw_team_index : nullptr);
    str_field(rep::GRI::sMissionVariant, cur.s_mission_variant, prev ? &prev->s_mission_variant : nullptr);
    str_field(rep::GRI::sWeather, cur.s_weather, prev ? &prev->s_weather : nullptr);
    str_field(rep::GRI::sTOD, cur.s_tod, prev ? &prev->s_tod : nullptr);
    str_field(rep::GRI::sMissionType, cur.s_mission_type, prev ? &prev->s_mission_type : nullptr);
    str_field(rep::GRI::sBaseMapName, cur.s_base_map_name, prev ? &prev->s_base_map_name : nullptr);
    int_field(rep::GRI::iMaxRoundsPerMatch, cur.i_max_rounds_per_match, prev ? &prev->i_max_rounds_per_match : nullptr);
    int_field(rep::GRI::iCurrentRound, cur.i_current_round, prev ? &prev->i_current_round : nullptr);
    bool_field(rep::GRI::bAllowPreplanning, cur.b_allow_preplanning, prev ? &prev->b_allow_preplanning : nullptr);
    bool_field(rep::GRI::bAllowRespawns, cur.b_allow_respawns, prev ? &prev->b_allow_respawns : nullptr);
    bool_field(rep::GRI::bTeamSideSwappingRound, cur.b_team_side_swapping_round, prev ? &prev->b_team_side_swapping_round : nullptr);
    bool_field(rep::GRI::bTournamentDiscardRound, cur.b_tournament_discard_round, prev ? &prev->b_tournament_discard_round : nullptr);

    // --- Match setup the client's menus read (doc sec 27; retail values
    // in server_actors.h). Static arrays: RepIndex, then a BYTE element
    // index, then the value (UnChan.cpp:1512).
    auto float_field = [&](uint32_t rep_index, float cur_v, const float* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            WriteFloatProp(w, cur_v);
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    };
    auto str_element = [&](uint32_t rep_index, uint8_t element, const std::string& cur_v,
                           const std::string* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteByte(element);
            WriteStrProp(w, cur_v);
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    };
    auto int_element = [&](uint32_t rep_index, uint8_t element, int32_t cur_v,
                           const int32_t* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteByte(element);
            w.WriteInt(static_cast<uint32_t>(cur_v));
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    };
    if (prev == nullptr && cur.game_class_object_index != 0) {
        // GameClass: a static class reference (bDynamic=0 + ObjectIndex).
        w.WriteIntMax(rep::GRI::GameClass, M);
        w.WriteBit(0);
        w.WriteIntMax(cur.game_class_object_index, 0x80000000);
        ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
    }
    if (prev == nullptr || cur.mp_state_name != prev->mp_state_name) {
        // NameProperty: AA3 FName = FString + INT32 Number (doc sec 22).
        w.WriteIntMax(rep::GRI::MPStateName, M);
        WriteStrProp(w, cur.mp_state_name);
        w.WriteInt(0);
        ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
    }
    for (uint8_t i = 0; i < 2; ++i) {
        str_element(rep::GRI::asTeamNames, i, cur.team_names[i], prev ? &prev->team_names[i] : nullptr);
        str_element(rep::GRI::asMissionTypes, i, cur.mission_types[i], prev ? &prev->mission_types[i] : nullptr);
        str_element(rep::GRI::asMissionBriefings, i, cur.mission_briefings[i],
                    prev ? &prev->mission_briefings[i] : nullptr);
        int_element(rep::GRI::aiEmptyTeamSlots, i, cur.empty_team_slots[i],
                    prev ? &prev->empty_team_slots[i] : nullptr);
    }
    str_field(rep::GRI::sMissionActiveSubLevel, cur.mission_active_sublevel,
              prev ? &prev->mission_active_sublevel : nullptr);
    int_field(rep::GRI::iMissionTimeRemaining, cur.mission_time_remaining,
              prev ? &prev->mission_time_remaining : nullptr);
    if (prev == nullptr || cur.spectator_mode != prev->spectator_mode) {
        w.WriteIntMax(rep::GRI::SpectatorMode, M);
        w.WriteBits(cur.spectator_mode, 2);   // eSpectatorMode: 5 names -> 2 bits (doc sec 11.2)
        ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
    }
    int_field(rep::GRI::iIncapacitationWithdrawDelay, cur.incapacitation_withdraw_delay,
              prev ? &prev->incapacitation_withdraw_delay : nullptr);
    float_field(rep::GRI::fpSpectatorKnockdownTraceTimeSeconds, cur.spectator_knockdown_trace_seconds,
                prev ? &prev->spectator_knockdown_trace_seconds : nullptr);
    float_field(rep::GRI::fpKnockdownTraceTimeSeconds, cur.knockdown_trace_seconds,
                prev ? &prev->knockdown_trace_seconds : nullptr);
    // Server settings retail's open sends as False/0 -- i.e. AA3's class
    // defaults are the opposite (sprint-mantle disabled, MP cheats on...).
    // Fixed values, initial only (GamePlayPCAP_Legit_Server frame 54; doc sec 31).
    if (prev == nullptr) {
        for (uint32_t idx : {rep::GRI::bMPCheatsEnabled, rep::GRI::bEnableKnockdownTracesForMP,
                             rep::GRI::bUseReducedAISuppression, rep::GRI::bServerForceAccuratePlayerShadows,
                             rep::GRI::bDisableSprintMantle, rep::GRI::bUseFootstepSoundDistanceOverride,
                             rep::GRI::bDisableKickVote, rep::GRI::bDisableMapVote}) {
            w.WriteIntMax(idx, M);
            w.WriteBit(false);
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
        for (uint8_t elem = 0; elem < 2; ++elem) {               // CheatsEnabled[0..1] = 0
            w.WriteIntMax(rep::GRI::CheatsEnabled, M);
            w.WriteByte(elem);
            w.WriteInt(0);
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    }
#define GRI_BOOL(idx, field) bool_field(rep::GRI::idx, cur.field, prev ? &prev->field : nullptr)
    GRI_BOOL(bAdvancedInventoryManagement, advanced_inventory_management);
    GRI_BOOL(bShowOpponentExtractionVolume, show_opponent_extraction_volume);
    GRI_BOOL(bAllowSpectatorBFT, allow_spectator_bft);
    GRI_BOOL(bAllowSpectatorsBackOntoTeams, allow_spectators_back_onto_teams);
    GRI_BOOL(bShowEnemyNeutralizedScoringMessages, show_enemy_neutralized_scoring_messages);
    GRI_BOOL(bEnableCoopHeadshotToIncap, enable_coop_headshot_to_incap);
    GRI_BOOL(bEnableMPHeadshotToIncap, enable_mp_headshot_to_incap);
    GRI_BOOL(bDisableSurefireFlashlights, disable_surefire_flashlights);
    GRI_BOOL(bAllowVoteReady, allow_vote_ready);
#undef GRI_BOOL

    // Round setup + countdowns (doc sec 28).
#define GRI_INT(idx, field) int_field(rep::GRI::idx, cur.field, prev ? &prev->field : nullptr)
#define GRI_STR(idx, field) str_field(rep::GRI::idx, cur.field, prev ? &prev->field : nullptr)
    GRI_INT(iDeploymentTimeRemaining, deployment_time_remaining);
    GRI_INT(iRoleSelectionTimeRemaining, role_selection_time_remaining);
    GRI_INT(iDeploymentTime, deployment_time);
    GRI_INT(iRoleSelectionTime, role_selection_time);
    GRI_INT(iMissionTimeSeconds, mission_time_seconds);
    GRI_INT(iMinPlayerCount, min_player_count);
    GRI_INT(iMaxPlayerCount, max_player_count);
    GRI_STR(sSummary, s_summary);
    GRI_STR(sType, s_type);
    GRI_STR(sAuthor, s_author);
    GRI_STR(sMissionName, s_mission_name);
    GRI_INT(iReadyVotesRequired, ready_votes_required);
    GRI_INT(iReadyVotes, ready_votes);
    GRI_INT(iMissionTimeLimitSeconds, mission_time_limit_seconds);
    int_field(rep::GRI::iRespawnPlayersNeededTeam0, cur.respawn_players_needed[0],
              prev ? &prev->respawn_players_needed[0] : nullptr);
    int_field(rep::GRI::iRespawnPlayersNeededTeam1, cur.respawn_players_needed[1],
              prev ? &prev->respawn_players_needed[1] : nullptr);

    // Round results (doc sec 36). On the open only when set: the GRI open
    // sits near the 4096-bit bunch limit (doc sec 27).
    static const ServerGameReplicationInfo kFresh;
    const ServerGameReplicationInfo& was = prev ? *prev : kFresh;
    for (uint8_t t = 0; t < 2; ++t) {
        // atsTeamScores[t]: a custom struct -- its three ints in order.
        if (cur.team_scores[t] != was.team_scores[t]) {
            w.WriteIntMax(rep::GRI::atsTeamScores, M);
            w.WriteByte(t);
            w.WriteInt(static_cast<uint32_t>(cur.team_scores[t].won));
            w.WriteInt(static_cast<uint32_t>(cur.team_scores[t].lost));
            w.WriteInt(static_cast<uint32_t>(cur.team_scores[t].tied));
            ++n; if (field_ends) field_ends->push_back(w.GetBitCount());
        }
    }
    if (cur.last_round_tied != was.last_round_tied)
        bool_field(rep::GRI::bLastRoundTied, cur.last_round_tied, prev ? &prev->last_round_tied : nullptr);
    if (cur.winning_objective != was.winning_objective)
        str_field(rep::GRI::sWinningObjective, cur.winning_objective, prev ? &prev->winning_objective : nullptr);
    if (cur.winning_action_player != was.winning_action_player)
        str_field(rep::GRI::sWinningActionPlayer, cur.winning_action_player,
                  prev ? &prev->winning_action_player : nullptr);
    if (cur.battle_planning_time_remaining != was.battle_planning_time_remaining)
        int_field(rep::GRI::iBattlePlanningTimeRemaining, cur.battle_planning_time_remaining,
                  prev ? &prev->battle_planning_time_remaining : nullptr);
#undef GRI_STR
#undef GRI_INT

    return n;
}

uint32_t EncodeTeamInfoRep(BitWriter& w, const ServerTeamInfo& cur,
                          const ServerTeamInfo* prev) {
    uint32_t n = 0;
    const uint32_t M = kTeamInfoMaxIndex;

    // RepIndex 23 TeamIndex : IntProperty
    if (prev == nullptr || cur.team_index != prev->team_index) {
        w.WriteIntMax(rep::TeamInfo::TeamIndex, M);
        w.WriteInt(static_cast<uint32_t>(cur.team_index));
        ++n;
    }

    // RepIndex 24 Score : FloatProperty — `score` is modeled as int32_t
    // (see rep_encode.h gap note); cast to float on the wire.
    if (prev == nullptr || cur.score != prev->score) {
        w.WriteIntMax(rep::TeamInfo::Score, M);
        WriteFloatProp(w, static_cast<float>(cur.score));
        ++n;
    }

    // RepIndex 25 TeamName : StrProperty
    if (prev == nullptr || cur.team_name != prev->team_name) {
        w.WriteIntMax(rep::TeamInfo::TeamName, M);
        WriteStrProp(w, cur.team_name);
        ++n;
    }

    if (prev == nullptr || cur.restrict_ft_roles_by_team_size != prev->restrict_ft_roles_by_team_size) {
        w.WriteIntMax(rep::TeamInfo::bRestrictFTRolesByTeamSize, M);
        w.WriteBit(cur.restrict_ft_roles_by_team_size);
        ++n;
    }
    return n;
}

uint32_t EncodePRIRep(BitWriter& w, const ServerPlayerReplicationInfo& cur,
                      const ServerPlayerReplicationInfo* prev, const RepContext* ctx) {
    uint32_t n = 0;
    const uint32_t M = kPRIMaxIndex;

    auto int_field = [&](uint32_t rep_index, int32_t cur_v, const int32_t* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteInt(static_cast<uint32_t>(cur_v));
            ++n;
        }
    };
    auto float_field = [&](uint32_t rep_index, int32_t cur_v, const int32_t* prev_v) {
        // Deaths/Score are FloatProperty on the wire but modeled as int32_t
        // here — cast, exact for whole-number values.
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            WriteFloatProp(w, static_cast<float>(cur_v));
            ++n;
        }
    };
    auto bool_field = [&](uint32_t rep_index, bool cur_v, const bool* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteBit(cur_v);
            ++n;
        }
    };
    auto str_field = [&](uint32_t rep_index, const std::string& cur_v, const std::string* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            WriteStrProp(w, cur_v);
            ++n;
        }
    };

    bool_field(rep::PRI::bBot, cur.b_bot, prev ? &prev->b_bot : nullptr);
    bool_field(rep::PRI::bOutOfLives, cur.b_out_of_lives, prev ? &prev->b_out_of_lives : nullptr);
    bool_field(rep::PRI::bOnlySpectator, cur.b_only_spectator, prev ? &prev->b_only_spectator : nullptr);
    bool_field(rep::PRI::bIsSpectator, cur.b_is_spectator, prev ? &prev->b_is_spectator : nullptr);
    bool_field(rep::PRI::bAdmin, cur.b_admin, prev ? &prev->b_admin : nullptr);
    // PlayerReplicationInfo.TeamID is never sent: retail's PRI open and its
    // join update carry only Team (frames 74, 139). Sending TeamID=-1 left
    // the real client's team menu showing FULL until the player re-joined
    // Spectator (doc sec 28).
    str_field(rep::PRI::PlayerName, cur.player_name, prev ? &prev->player_name : nullptr);

    // RepIndex 46 Ping : ByteProperty, no Enum — plain 8 bits (doc sec 11.2).
    if (prev == nullptr || cur.ping != prev->ping) {
        w.WriteIntMax(rep::PRI::Ping, M);
        w.WriteByte(cur.ping);
        ++n;
    }

    float_field(rep::PRI::Deaths, cur.deaths, prev ? &prev->deaths : nullptr);
    if (prev && cur.b_dead != prev->b_dead) {
        w.WriteIntMax(rep::PRI::bIsDead, M); w.WriteBit(cur.b_dead); ++n;
    }
    if (prev && cur.b_incapacitated != prev->b_incapacitated) {
        w.WriteIntMax(rep::PRI::bIsIncapacitated, M); w.WriteBit(cur.b_incapacitated); ++n;
    }
    if (prev && cur.i_died != prev->i_died) {
        w.WriteIntMax(rep::PRI::iDied, M); w.WriteInt(static_cast<uint32_t>(cur.i_died)); ++n;
    }
    float_field(rep::PRI::Score, cur.score, prev ? &prev->score : nullptr);


    // PlayerID, the soldier record and PRI.Team (doc sec 27). Team is an
    // actor reference -> the TeamInfo's channel on this connection; null
    // until ServerJoinTeam (spectating). Needs a RepContext to resolve.
    int_field(rep::PRI::PlayerID, cur.player_id, prev ? &prev->player_id : nullptr);
    str_field(rep::PRI::sAuthSoldierName, cur.player_name, prev ? &prev->player_name : nullptr);
    int_field(rep::PRI::iAccessRights, cur.access_rights, prev ? &prev->access_rights : nullptr);
    for (uint32_t idx : {rep::PRI::iSDMTierLevel, rep::PRI::iAutoRiflemanTierLevel,
                         rep::PRI::iAdvRiflemanTierLevel, rep::PRI::iGrenadierTierLevel})
        int_field(idx, cur.tier_level, prev ? &prev->tier_level : nullptr);
    for (uint32_t idx : {rep::PRI::bMOUTCompleted, rep::PRI::bBCTMGrenadesCompleted,
                         rep::PRI::bBCTM320Completed, rep::PRI::bBCTM249Completed,
                         rep::PRI::bES2TrainingCompleted, rep::PRI::bCanDoFaster320Reload,
                         rep::PRI::bCanDoAdvancedReloads, rep::PRI::bCanRecoverEnemyWeapons})
        bool_field(idx, cur.qualified, prev ? &prev->qualified : nullptr);
    // Assigned class role (doc sec 28; retail frame 204).
    int_field(rep::PRI::iAssignedClassRoleIndex, cur.assigned_class_role_index,
              prev ? &prev->assigned_class_role_index : nullptr);
    str_field(rep::PRI::sCurrentClassRoleID, cur.current_class_role_id,
              prev ? &prev->current_class_role_id : nullptr);
    str_field(rep::PRI::sClassRoleName, cur.class_role_name, prev ? &prev->class_role_name : nullptr);
    int_field(rep::PRI::iPrimaryOneAmmo, cur.primary_one_ammo, prev ? &prev->primary_one_ammo : nullptr);
    bool_field(rep::PRI::bHasHadRoleAssigned, cur.has_had_role_assigned,
               prev ? &prev->has_had_role_assigned : nullptr);
    bool_field(rep::PRI::bIsFireTeamLeader, cur.is_fire_team_leader,
               prev ? &prev->is_fire_team_leader : nullptr);
    bool_field(rep::PRI::bVoteReady, cur.vote_ready, prev ? &prev->vote_ready : nullptr);
    if (ctx) {
        RefWriter ref{w, *ctx, M, prev == nullptr, n};
        ref(rep::PRI::Team, cur.team);
        ref(rep::PRI::aaftiActiveFireTeam, cur.active_fire_team);
        // Class references (static): the weapon and its rail mods.
        auto class_ref = [&](const std::string& pkg, uint32_t net) {
            game::DataValue v;
            v.kind = pkg.empty() ? game::DataValue::Kind::kNone : game::DataValue::Kind::kObj;
            v.s = pkg;
            v.net = net;
            WriteDataValue(w, v, *ctx);
        };
        if (prev == nullptr ? !cur.primary_weapon_pkg.empty()
                            : (cur.primary_weapon_pkg != prev->primary_weapon_pkg ||
                               cur.primary_weapon_net != prev->primary_weapon_net)) {
            w.WriteIntMax(rep::PRI::aawpnPrimaryOne, M);
            class_ref(cur.primary_weapon_pkg, cur.primary_weapon_net);
            ++n;
        }
        if (prev == nullptr ? !cur.primary_weapon_pkg.empty() : cur.primary_mods != prev->primary_mods) {
            // mriPrimaryWeaponOneMods: bForceMods, then Main/Top/Left/Right/
            // Bottom/RearSight rail classes.
            w.WriteIntMax(rep::PRI::mriPrimaryWeaponOneMods, M);
            w.WriteBit(0);
            for (const auto& m : cur.primary_mods) class_ref(m.first, m.second);
            ++n;
        }
    }
    return n;
}

uint32_t EncodePCRep(BitWriter& w, const ServerPlayerController& cur,
                     const ServerPlayerController* prev, const RepContext& ctx) {
    uint32_t n = 0;
    const bool initial = prev == nullptr;
    RefWriter ref{w, ctx, kPCMaxIndex, initial, n};
    // Initial-only, as in retail's PC open (doc sec 27): Role under Role's
    // index carries the server's RemoteRole -- AutonomousProxy for the
    // owning client (UnChan.cpp:1942 swap; doc sec 21). Retail doesn't send
    // RemoteRole here. Then bNetOwner and AA_PlayerController.bUseFlashSAI.
    if (initial) {
        constexpr uint32_t kRoleAutonomousProxy = 2;
        w.WriteIntMax(rep::PC::Role, kPCMaxIndex);
        w.WriteBits(kRoleAutonomousProxy, 2);
        w.WriteIntMax(rep::PC::bNetOwner, kPCMaxIndex);
        w.WriteBit(ctx.for_owner);
        n += 2;
    }
    if (initial || cur.use_flash_sai != prev->use_flash_sai) {
        w.WriteIntMax(rep::PC::bUseFlashSAI, kPCMaxIndex);
        w.WriteBit(cur.use_flash_sai);
        ++n;
    }
    // Dynamic actor refs (retail: PRI = ch 10, aassmSteamStatManager = ch 3,
    // PlayerProxy = ch 18, Pawn = dynamic ch 159/161/163).
    ref(rep::PC::PlayerReplicationInfo, cur.pri);
    ref(rep::PC::Pawn, cur.pawn);
    ref(rep::PC::aassmSteamStatManager, cur.stat_manager);
    ref(rep::PC::PlayerProxy, cur.proxy);
    // Spectating (retail t=81.83 death, t=227.34 round end; doc sec 36):
    // bCollideWorld, the spectate hints and SpectatorMode -- only once they
    // differ from the class defaults the client starts with.
    static const ServerPlayerController kFresh;
    const ServerPlayerController& was = initial ? kFresh : *prev;
    if (cur.collide_world != was.collide_world) {
        w.WriteIntMax(rep::PC::bCollideWorld, kPCMaxIndex);
        w.WriteBit(cur.collide_world);
        ++n;
    }
    if (cur.right_spectate_info != was.right_spectate_info) {
        w.WriteIntMax(rep::PC::RightSpectateInfoString, kPCMaxIndex);
        WriteStrProp(w, cur.right_spectate_info);
        ++n;
    }
    if (cur.left_spectate_info != was.left_spectate_info) {
        w.WriteIntMax(rep::PC::LeftSpectateInfoString, kPCMaxIndex);
        WriteStrProp(w, cur.left_spectate_info);
        ++n;
    }
    if (cur.spectator_mode != was.spectator_mode) {
        w.WriteIntMax(rep::PC::SpectatorMode, kPCMaxIndex);
        w.WriteBits(cur.spectator_mode, 2);   // eSpectatorMode, as the GRI's (doc sec 11.2)
        ++n;
    }
    return n;
}

uint32_t EncodePlayerStorageProxyRep(BitWriter& w, const game::ServerPlayerStorageProxy& cur,
                                     const game::ServerPlayerStorageProxy* prev,
                                     const RepContext& ctx) {
    namespace P = rep::PlayerStorageProxy;
    constexpr uint32_t M = P::kMaxIndex;
    const bool initial = prev == nullptr;
    uint32_t n = 0;
    RefWriter ref{w, ctx, M, initial, n};
    // Retail's open (Ch=18): Owner, bNetOwner, bIsDead, bActive, RepPRI,
    // RepController (doc sec 27).
    ref(P::Owner, cur.controller);
    if (initial) {
        w.WriteIntMax(P::bNetOwner, M);
        w.WriteBit(ctx.for_owner);
        ++n;
    }
    if (initial || cur.is_dead != prev->is_dead) {
        w.WriteIntMax(P::bIsDead, M);
        w.WriteBit(cur.is_dead);
        ++n;
    }
    if (prev && cur.incapacitated != prev->incapacitated) {
        w.WriteIntMax(P::bIncapacitated, M);
        w.WriteBit(cur.incapacitated);
        ++n;
    }
    if ((prev && cur.is_spectating != prev->is_spectating) || (!prev && cur.is_spectating)) {
        w.WriteIntMax(P::bIsSpectating, M);
        w.WriteBit(cur.is_spectating);
        ++n;
    }
    if (initial || cur.active != prev->active) {
        w.WriteIntMax(P::bActive, M);
        w.WriteBit(cur.active);
        ++n;
    }
    ref(P::RepPRI, cur.pri);
    ref(P::RepController, cur.controller);
    return n;
}

uint32_t EncodePawnRep(BitWriter& w, const ServerPawn& cur, const ServerPawn* prev,
                       const RepContext& ctx) {
    uint32_t n = 0;
    const uint32_t M = kPawnMaxIndex;
    const bool initial = prev == nullptr;
    const bool owner = ctx.for_owner;
    const bool do_move = ctx.fields != RepContext::Fields::kNoMovement;
    const bool do_rest = ctx.fields != RepContext::Fields::kMovementOnly;
    RefWriter ref{w, ctx, M, initial, n};

    auto float_field = [&](uint32_t rep_index, float cur_v, const float* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            WriteFloatProp(w, cur_v);
            ++n;
        }
    };
    auto byte_field = [&](uint32_t rep_index, uint8_t cur_v, const uint8_t* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteByte(cur_v);
            ++n;
        }
    };
    auto bool_field = [&](uint32_t rep_index, bool cur_v, const bool* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteBit(cur_v);
            ++n;
        }
    };
    auto int_field = [&](uint32_t rep_index, int32_t cur_v, const int32_t* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            w.WriteInt(static_cast<uint32_t>(cur_v));
            ++n;
        }
    };
    auto str_field = [&](uint32_t rep_index, const std::string& cur_v, const std::string* prev_v) {
        if (prev == nullptr || *prev_v != cur_v) {
            w.WriteIntMax(rep_index, M);
            WriteStrProp(w, cur_v);
            ++n;
        }
    };

    // ---- Movement (Actor.uc): Location/Rotation go to a SimulatedProxy on
    // init and while bUpdateSimulatedPosition (Pawn default: true), and to
    // the AutonomousProxy owner on init only. Velocity is SimulatedProxy-
    // only. Retail never sends Location to the owner after init (doc sec
    // 21 census: owner pawn updates are Health/Stamina/AimOffset/...).
    if (do_move) {
        const bool force = ctx.force_movement;
        if (initial || (!owner)) {
            const float* cl = cur.location;
            const float* pl = prev ? prev->location : nullptr;
            if (initial || force || cl[0] != pl[0] || cl[1] != pl[1] || cl[2] != pl[2]) {
                w.WriteIntMax(rep::Pawn::Location, M);
                WriteCompressedVector(w, cl[0], cl[1], cl[2]);
                ++n;
            }
            if (initial || force || cur.rotation_pitch != prev->rotation_pitch ||
                cur.rotation_yaw != prev->rotation_yaw || cur.rotation_roll != prev->rotation_roll) {
                w.WriteIntMax(rep::Pawn::Rotation, M);
                WriteCompressedRotator(w, cur.rotation_pitch, cur.rotation_yaw, cur.rotation_roll);
                ++n;
            }
        }
        if (!owner) {
            const float* cv = cur.velocity;
            const float* pv = prev ? prev->velocity : nullptr;
            if (initial || force || cv[0] != pv[0] || cv[1] != pv[1] || cv[2] != pv[2]) {
                w.WriteIntMax(rep::Pawn::Velocity, M);
                WriteCompressedVector(w, cv[0], cv[1], cv[2]);
                ++n;
            }
            // Pawn.uc: "variables sent to all but the owning client".
            if (initial || force || cur.remote_view_pitch != prev->remote_view_pitch) {
                w.WriteIntMax(rep::Pawn::RemoteViewPitch, M);
                w.WriteByte(cur.remote_view_pitch);
                ++n;
            }
        }
    }
    if (!do_rest) return n;

    // ---- Initial-only Actor fields: `Role,RemoteRole,bNetOwner,bTearOff`.
    // UnChan.cpp:1942 swaps Role/RemoteRole on the wire: the value under
    // Role's index is the server's RemoteRole, which becomes the client's
    // Role. Owner: AutonomousProxy (possessed by its PlayerController);
    // anyone else: SimulatedProxy (UnChan.cpp:1803 downgrade). Without
    // this the client derives roles from class defaults and treats even
    // its own pawn as a SimulatedProxy. ENetRole = 2 bits (doc sec 11.2).
    if (initial) {
        constexpr uint32_t kRoleSimulatedProxy = 1, kRoleAutonomousProxy = 2, kRoleAuthority = 3;
        w.WriteIntMax(rep::Pawn::Role, M);
        w.WriteBits(owner ? kRoleAutonomousProxy : kRoleSimulatedProxy, 2);
        w.WriteIntMax(rep::Pawn::RemoteRole, M);
        w.WriteBits(kRoleAuthority, 2);
        w.WriteIntMax(rep::Pawn::bNetOwner, M);
        w.WriteBit(owner);
        n += 3;
    }
    // Death (retail t=81.83): bTearOff -- clients keep the body as a
    // ragdoll and stop taking updates for it.
    if (cur.torn_off && (!prev || !prev->torn_off)) {
        w.WriteIntMax(rep::Pawn::bTearOff, M);
        w.WriteBit(true);
        ++n;
    }
    // Round end (retail t=227.34): the owner's pawn drops to SimulatedProxy
    // and bNetOwner False once its controller lets go (doc sec 36).
    if (owner && cur.unpossessed && prev && !prev->unpossessed) {
        constexpr uint32_t kRoleSimulatedProxy = 1;
        w.WriteIntMax(rep::Pawn::Role, M);
        w.WriteBits(kRoleSimulatedProxy, 2);
        w.WriteIntMax(rep::Pawn::bNetOwner, M);
        w.WriteBit(false);
        n += 2;
    }
    // Physics: SimulatedProxy only (with Velocity). EPhysics = 4 bits.
    if (!owner && (initial || cur.physics != prev->physics)) {
        w.WriteIntMax(rep::Pawn::Physics, M);
        w.WriteBits(cur.physics, 4);
        ++n;
    }

    // ---- Pawn.uc "send ALL clients" (bNetDirty).
    if (initial) {  // DrivenVehicle: NULL, no vehicle system exists.
        w.WriteIntMax(rep::Pawn::DrivenVehicle, M);
        WriteActorRef(w, 0);
        ++n;
    }
    ref(rep::Pawn::PlayerReplicationInfo, cur.pri);
    bool_field(rep::Pawn::bSimulateGravity, cur.b_simulate_gravity, prev ? &prev->b_simulate_gravity : nullptr);
    bool_field(rep::Pawn::bIsWalking, cur.b_is_walking, prev ? &prev->b_is_walking : nullptr);
    // bIsProne is AA3's addition to Engine.Pawn (no stock condition) — all.
    bool_field(rep::Pawn::bIsProne, cur.b_is_prone, prev ? &prev->b_is_prone : nullptr);

    if (owner) {
        // ---- Owner only: Health (bNetOwner || bReplicateHealthToAll),
        // Controller + movement tuning (`bNetDirty && bNetOwner`).
        float_field(rep::Pawn::Health, cur.health, prev ? &prev->health : nullptr);
        // HealthTarget moves with Health on damage (retail t=262.49); the
        // open leaves it to the class default like retail.
        if (prev && cur.health_target != prev->health_target) {
            w.WriteIntMax(rep::Pawn::HealthTarget, M); WriteFloatProp(w, cur.health_target); ++n;
        }
        ref(rep::Pawn::Controller, cur.controller);
        ref(rep::Pawn::InvManager, cur.inv_manager);
        ref(rep::Pawn::DumpPouch, cur.dump_pouch);
        if (cur.next_mag_pouch || (prev && prev->next_mag_pouch)) {
            ref(rep::Pawn::NextStorageMagPouch, cur.next_mag_pouch);
            ref(rep::Pawn::NextMagPouch, cur.next_mag_pouch);
        }
        // AirControl/JumpZ/AccelRate/AirSpeed/WaterSpeed: not sent, the
        // client's AA3 class defaults apply (retail frame 366; doc sec 30).
        float_field(rep::Pawn::GroundSpeed, cur.ground_speed, prev ? &prev->ground_speed : nullptr);
        ref(rep::Pawn::Owner, cur.controller);
        ref(rep::Pawn::aaController, cur.controller);
        ref(rep::Pawn::PlayerProxy, cur.proxy);
        float_field(rep::Pawn::MovementSpeedModifier, cur.movement_speed_modifier,
                    prev ? &prev->movement_speed_modifier : nullptr);
        if (initial) {
            // Inventory association name (= every inventory item's), the
            // owning team, and the posture state graph slot 2 (retail frame 366).
            w.WriteIntMax(rep::Pawn::namePawnInventoryAssociation, M);
            WriteStrProp(w, cur.inventory_association);
            w.WriteInt(static_cast<uint32_t>(cur.inventory_association_number));
            w.WriteIntMax(rep::Pawn::iOriginalOwningTeamIndex, M);
            w.WriteInt(static_cast<uint32_t>(cur.original_owning_team));
            n += 2;
        }
    } else {
        // ---- Non-owners only: `bIsCrouched, FlashCount, FiringMode`.
        bool_field(rep::Pawn::bIsCrouched, cur.b_is_crouched, prev ? &prev->b_is_crouched : nullptr);
        byte_field(rep::Pawn::FlashCount, cur.flash_count, prev ? &prev->flash_count : nullptr);
        byte_field(rep::Pawn::FiringMode, cur.firing_mode, prev ? &prev->firing_mode : nullptr);
    }

    // ---- AA_Pawn additions. AA3's own replication conditions aren't
    // recoverable (decompiler corruption, doc sec 12.7), so these go to
    // every connection as before.
    str_field(rep::Pawn::sBodySoldierName, cur.body_soldier_name, prev ? &prev->body_soldier_name : nullptr);
    int_field(rep::Pawn::iBodyTeamIndex, cur.body_team_index, prev ? &prev->body_team_index : nullptr);

    // The rest only once it differs from the model's starting state: retail's
    // pawn open sends none of these, and an explicit zero overrides the
    // client's class defaults -- Stamina=0 left the real client unable to
    // sprint (doc sec 30).
    static const ServerPawn kStart;
    const ServerPawn& base = prev ? *prev : kStart;

    // PawnShotEffectInfo : StructProperty AA_PawnHitEffectInfo — custom
    // (non-native) struct, both members unconditional once the outer
    // RepIndex fires (doc sec 11.1, 12.6).
    const float* shv = cur.shot_effect_hit_location;
    const float* pshv = base.shot_effect_hit_location;
    bool shot_effect_changed =
        shv[0] != pshv[0] || shv[1] != pshv[1] || shv[2] != pshv[2] ||
        cur.shot_effect_hit_rotation_pitch != base.shot_effect_hit_rotation_pitch ||
        cur.shot_effect_hit_rotation_yaw   != base.shot_effect_hit_rotation_yaw   ||
        cur.shot_effect_hit_rotation_roll  != base.shot_effect_hit_rotation_roll;
    if (shot_effect_changed) {
        w.WriteIntMax(rep::Pawn::PawnShotEffectInfo, M);
        WriteCompressedVector(w, shv[0], shv[1], shv[2]);
        WriteCompressedRotator(w, cur.shot_effect_hit_rotation_pitch,
                               cur.shot_effect_hit_rotation_yaw,
                               cur.shot_effect_hit_rotation_roll);
        ++n;
    }

    // (The float/int helpers always send on an initial open, so compare
    // against the starting state explicitly here.)
    auto float_if = [&](uint32_t idx, float v, float was) {
        if (v == was) return;
        w.WriteIntMax(idx, M);
        WriteFloatProp(w, v);
        ++n;
    };
    float_if(rep::Pawn::fDesiredLeanDirection, cur.lean_direction, base.lean_direction);
    // Only once set (retail never sends these as None).
    if (cur.last_known_pri || (prev && prev->last_known_pri)) ref(rep::Pawn::LastKnownPRI, cur.last_known_pri);
    if (cur.active_inventory || (prev && prev->active_inventory)) ref(rep::Pawn::ActiveInventory, cur.active_inventory);
    // RepGraphStates: each graph that differs from the last send (on the
    // open, from an all-zero graph; retail's open carries only graph 2).
    for (int g = 0; g < ServerPawn::kGraphCount; ++g) {
        const ServerPawn::GraphState& gs = cur.graphs[g];
        if (!(gs != (prev ? prev->graphs[g] : ServerPawn::GraphState{}))) continue;
        w.WriteIntMax(rep::Pawn::RepGraphStates, M);
        w.WriteByte(static_cast<uint8_t>(g));                 // element
        w.WriteByte(gs.current[0]);
        w.WriteByte(gs.current[1]);
        w.WriteByte(gs.destination[0]);
        w.WriteByte(gs.destination[1]);
        w.WriteBit(true);                                     // bNewData
        ++n;
    }
    float_if(rep::Pawn::Stamina, cur.stamina, base.stamina);
    float_if(rep::Pawn::Asphyx, cur.asphyx, base.asphyx);
    if (cur.moving_to_target != base.moving_to_target) {
        w.WriteIntMax(rep::Pawn::bAdvMvmt_MovingToTarget, M); w.WriteBit(cur.moving_to_target); ++n;
    }
    if (cur.holding_breath != base.holding_breath) {
        w.WriteIntMax(rep::Pawn::bHoldingBreath, M); w.WriteBit(cur.holding_breath); ++n;
    }
    if (cur.asphyxiating != base.asphyxiating) {
        w.WriteIntMax(rep::Pawn::bAsphyxiating, M); w.WriteBit(cur.asphyxiating); ++n;
    }
    if (cur.aim_offset != base.aim_offset) {
        w.WriteIntMax(rep::Pawn::AimOffset, M);
        w.WriteInt(static_cast<uint32_t>(cur.aim_offset));
        ++n;
    }
    // IncapacitationLoc : eHitLocation, 19 names -> 5 bits (doc sec 12.5).
    if (cur.incapacitation_loc != base.incapacitation_loc) {
        w.WriteIntMax(rep::Pawn::IncapacitationLoc, M);
        w.WriteBits(cur.incapacitation_loc, 5);
        ++n;
    }
    float_if(rep::Pawn::AdvMvmt_VaultMantleHeight, cur.vault_mantle_height, base.vault_mantle_height);
    // Injury and incapacitation (doc sec 35). IncapacitationType is a
    // static class reference (bDynamic=0 + ObjectIndex).
    const bool injured = cur.health < 40.0f, was_injured = base.health < 40.0f;   // InjuredAnimThreshold
    if (injured != was_injured) {
        w.WriteIntMax(rep::Pawn::bPlayingInjuredAnims, M); w.WriteBit(injured); ++n;
    }
    if (cur.incap_type_index != base.incap_type_index) {
        w.WriteIntMax(rep::Pawn::IncapacitationType, M);
        w.WriteBit(false);
        w.WriteIntMax(cur.incap_type_index, 0x80000000);
        ++n;
    }
    if (cur.bleeding != base.bleeding) {
        w.WriteIntMax(rep::Pawn::bIsBleeding, M); w.WriteBit(cur.bleeding); ++n;
    }
    if (cur.ragdoll != base.ragdoll) {
        w.WriteIntMax(rep::Pawn::bRagdollTick, M); w.WriteBit(cur.ragdoll);
        w.WriteIntMax(rep::Pawn::bRagdollStuck, M); w.WriteBit(cur.ragdoll);
        n += 2;
    }
    return n;
}

// Custom-struct NetSerializeItem (doc sec 11.1): every field, in
// declaration order, no per-field index. Driven by the generated field
// table (game/player_stats_gen.h) — the same table the JSON stats export
// uses, so the wire and the export can't disagree about the layout.
static void WriteStatStruct(BitWriter& w, const void* base, const game::StatField* fields,
                            size_t count) {
    const auto* p = static_cast<const uint8_t*>(base);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* f = p + fields[i].offset;
        switch (fields[i].kind) {
            case game::StatKind::kBool:  w.WriteBit(*reinterpret_cast<const bool*>(f)); break;
            case game::StatKind::kFloat: WriteFloatProp(w, *reinterpret_cast<const float*>(f)); break;
            case game::StatKind::kInt:   w.WriteInt(static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(f))); break;
            case game::StatKind::kByte:  w.WriteByte(*f); break;
        }
    }
}

static bool StatStructEqual(const void* a, const void* b, const game::StatField* fields,
                            size_t count) {
    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < count; ++i) {
        size_t o = fields[i].offset;
        size_t n = 0;
        switch (fields[i].kind) {
            case game::StatKind::kBool:  n = sizeof(bool); break;
            case game::StatKind::kFloat: n = sizeof(float); break;
            case game::StatKind::kInt:   n = sizeof(int32_t); break;
            case game::StatKind::kByte:  n = sizeof(uint8_t); break;
        }
        if (std::memcmp(pa + o, pb + o, n) != 0) return false;
    }
    return true;
}

uint32_t EncodeSteamStatManagerRep(BitWriter& w, const ServerSteamStatManager& cur,
                                   const ServerSteamStatManager* prev) {
    // Each struct is one StructProperty: when anything in it changes the
    // whole struct goes out (retail: both at round end, sec 23).
    uint32_t n = 0;
    using game::PlayerMatchStats;
    using game::PlayerRoundStats;
    if (!prev || !StatStructEqual(&cur.match, &prev->match, game::PlayerMatchStatsFields(),
                                  PlayerMatchStats::kNumFields)) {
        w.WriteIntMax(rep::SteamStatManager::aamppmsReplicatedPlayerMatchStats,
                      kSteamStatManagerMaxIndex);
        WriteStatStruct(w, &cur.match, game::PlayerMatchStatsFields(), PlayerMatchStats::kNumFields);
        ++n;
    }
    if (!prev || !StatStructEqual(&cur.round, &prev->round, game::PlayerRoundStatsFields(),
                                  PlayerRoundStats::kNumFields)) {
        w.WriteIntMax(rep::SteamStatManager::aamppsReplicatedPlayerRoundStats,
                      kSteamStatManagerMaxIndex);
        WriteStatStruct(w, &cur.round, game::PlayerRoundStatsFields(), PlayerRoundStats::kNumFields);
        ++n;
    }
    return n;
}

// FVector::SerializeCompressed (UnMath.cpp:51-77), send side.
void WriteDataValue(BitWriter& w, const game::DataValue& v, const RepContext& ctx) {
    using K = game::DataValue::Kind;
    switch (v.kind) {
    case K::kNone:   WriteActorRef(w, 0); break;   // NULL reference
    case K::kInt:    w.WriteInt(static_cast<uint32_t>(v.i)); break;
    case K::kFloat:  WriteFloatProp(w, static_cast<float>(v.f)); break;
    case K::kBool:   w.WriteBit(v.i != 0); break;
    case K::kStr:    WriteStrProp(w, v.s); break;
    case K::kName:   WriteStrProp(w, v.s); w.WriteInt(static_cast<uint32_t>(v.i)); break;
    case K::kByte:   w.WriteBits(static_cast<uint32_t>(v.i), v.bits); break;
    case K::kVector: WriteCompressedVector(w, static_cast<float>(v.v3[0]), static_cast<float>(v.v3[1]),
                                           static_cast<float>(v.v3[2])); break;
    case K::kRotator: WriteCompressedRotator(w, v.v3[0], v.v3[1], v.v3[2]); break;
    case K::kObj: {
        uint32_t idx = ctx.object_index ? ctx.object_index(v.s, v.net) : 0;
        if (idx == 0) { WriteActorRef(w, 0); break; }
        w.WriteBit(0);                         // static: bDynamic=0 + ObjectIndex
        w.WriteIntMax(idx, 0x80000000);
        break;
    }
    case K::kActor:
        WriteActorRef(w, v.actor && ctx.channel_of ? ctx.channel_of(v.actor) : 0);
        break;
    case K::kStruct:
        // Struct members serialize in order, unconditionally (doc sec 12.6).
        for (const auto& m : v.members) WriteDataValue(w, m, ctx);
        break;
    case K::kRaw:
        // Bits copied from retail (RPC parameters with no live references).
        for (char c : v.s) w.WriteBit(c == '1');
        break;
    }
}

uint32_t EncodeRoundActor(BitWriter& w, const game::ServerRoundActor& a,
                          std::map<std::pair<uint32_t, int32_t>, std::string>& sent,
                          const RepContext& ctx) {
    return EncodeDataFields(w, a.fields, a.max_index, sent, ctx);
}

uint32_t EncodeDataFields(BitWriter& w, const std::vector<game::DataField>& fields,
                          uint32_t max_index,
                          std::map<std::pair<uint32_t, int32_t>, std::string>& sent,
                          const RepContext& ctx) {
    uint32_t n = 0;
    // Properties first, raw RPC parameter bits last: retail's inventory opens
    // end with ClientGivenTo/ClientWeaponSet, and AA3's ClientWeaponSet is 2
    // bits on the wire, not its package parameter list -- anything after it
    // would be read at the wrong offset (doc sec 30).
    std::vector<const game::DataField*> order;
    for (const auto& f : fields) if (f.v.kind != game::DataValue::Kind::kRaw) order.push_back(&f);
    for (const auto& f : fields) if (f.v.kind == game::DataValue::Kind::kRaw) order.push_back(&f);
    for (const game::DataField* fp : order) {
        const game::DataField& f = *fp;
        BitWriter value;
        WriteDataValue(value, f.v, ctx);
        std::string key(reinterpret_cast<const char*>(value.GetData().data()), value.GetData().size());
        key += std::to_string(value.GetBitCount()) + "#" + std::to_string(f.revision);
        auto& last = sent[{f.idx, f.elem}];
        if (!last.empty() && last == key) continue;
        last = key;
        w.WriteIntMax(f.idx, max_index);
        if (f.elem >= 0) w.WriteByte(static_cast<uint8_t>(f.elem));
        w.AppendBits(value);
        ++n;
    }
    return n;
}

void WriteCompressedVector(BitWriter& w, float x, float y, float z) {
    auto round_to_int = [](float v) -> int32_t {
        return static_cast<int32_t>(v >= 0.0f ? v + 0.5f : v - 0.5f); // appRound
    };
    int32_t int_x = round_to_int(x);
    int32_t int_y = round_to_int(y);
    int32_t int_z = round_to_int(z);

    auto abs_i = [](int32_t v) { return v < 0 ? -v : v; };
    uint32_t max_component = static_cast<uint32_t>(
        std::max({abs_i(int_x), abs_i(int_y), abs_i(int_z)}));

    // appCeilLogTwo(1 + max_component), clamped to [1,20], then -1.
    uint32_t n_val = 1 + max_component;
    uint32_t ceil_log2 = 0;
    for (uint32_t v = 1; v < n_val; v <<= 1) ++ceil_log2;
    if (ceil_log2 < 1) ceil_log2 = 1;
    if (ceil_log2 > 20) ceil_log2 = 20;
    uint32_t bits = ceil_log2 - 1;

    w.WriteIntMax(bits, 20);
    int32_t bias = 1 << (bits + 1);
    uint32_t max_val = 1u << (bits + 2);
    w.WriteIntMax(static_cast<uint32_t>(int_x + bias), max_val);
    w.WriteIntMax(static_cast<uint32_t>(int_y + bias), max_val);
    w.WriteIntMax(static_cast<uint32_t>(int_z + bias), max_val);
}

// FRotator::SerializeCompressed (UnMath.cpp:84-129), send side. Per axis:
// only the high byte of the 16-bit angle (angle>>8) is sent, and only if
// non-zero (1-bit flag first).
void WriteCompressedRotator(BitWriter& w, int32_t pitch, int32_t yaw, int32_t roll) {
    auto write_axis = [&](int32_t angle) {
        uint8_t hi_byte = static_cast<uint8_t>((static_cast<uint32_t>(angle) >> 8) & 0xFF);
        bool nonzero = hi_byte != 0;
        w.WriteBit(nonzero);
        if (nonzero) w.WriteByte(hi_byte);
    };
    write_axis(pitch);
    write_axis(yaw);
    write_axis(roll);
}

void WriteSerializeCompressedInitial(BitWriter& w, const ServerActor& actor,
                                     bool b_net_initial_rotation) {
    WriteCompressedVector(w, actor.location[0], actor.location[1], actor.location[2]);
    if (b_net_initial_rotation) {
        WriteCompressedRotator(w, actor.rotation_pitch, actor.rotation_yaw, actor.rotation_roll);
    }
    // AA3 addition, read by its UActorChannel::ReceivedBunch right after
    // SerializeCompressedInitial (AA3Game.exe 0x9f4b59..0x9f4b88): two bits
    // (ReadBit 0xc80a90 x2) and a SerializeInt(32) (0xc80a60). The first bit
    // picks an alternate SpawnActor call, the second is an argument to it,
    // the int lands at actor+0x4b8. Retail sends 0,1,0 for nearly every
    // actor (GamePlayPCAP_Legit_Server); without these 7 bits the client
    // read NetPlayerIndex=46 and aborted (doc sec 26).
    w.WriteBit(0);
    w.WriteBit(1);
    w.WriteIntMax(0, 32);
}

} // namespace network
} // namespace aa3
