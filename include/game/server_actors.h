#ifndef AA3_GAME_SERVER_ACTORS_H
#define AA3_GAME_SERVER_ACTORS_H

// Server-side actor data model mirroring the subset of AA3's UE3 class
// hierarchy that the client expects to be replicated. Each struct here
// represents one actor instance the server holds in memory; the network
// layer reads fields directly to fill property-update bunches.
//
// This is the foundation of Path 3: instead of synthesizing bytes blind,
// we maintain a typed model of the world and let the replication layer
// derive wire data from it.
//
// Property names match the AA3 decomp (OnlineSubsystemAA3/uscript_decompiled/).
// Defaults match what AA_GameInfo's spawn logic sets at server boot.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/player_stats_gen.h"

namespace aa3 {
namespace game {

// Engine.Actor — the root of every replicated thing. Most fields below
// only matter for non-static actors, but we keep them for completeness.
struct ServerActor {
    // Stable identifier within the world. Maps 1:1 to an actor channel
    // index when the actor is being replicated.
    uint32_t actor_id = 0;

    // PackageMap ObjectIndex the client uses to resolve this actor's
    // class via SerializeObject. Filled by the world builder.
    uint32_t object_index = 0;

    // True if this actor is server-spawned (transient) — needs
    // SerializeCompressedInitial when its channel opens. False for
    // map-placed actors loaded from the .aa3 package.
    bool transient = false;

    // Engine.Actor replicated fields.
    float location[3] = {0.0f, 0.0f, 0.0f};
    int32_t rotation_pitch = 0;
    int32_t rotation_yaw   = 0;
    int32_t rotation_roll  = 0;
    float velocity[3] = {0.0f, 0.0f, 0.0f};

    // Display name for logging / debugging.
    std::string label;
};

// Engine.WorldInfo — environmental settings that affect all clients.
// Always Ch=2 on the wire; placed-actor (loaded from the level package).
struct ServerWorldInfo : ServerActor {
    // Replicated:
    std::string replicated_music_track;
    float time_dilation = 1.0f;
    float world_gravity_z = -520.0f;       // AA3 default
    bool  b_high_priority_loading = false;
    uint8_t pauser_index = 0;              // PlayerController index that paused

    // Non-replicated but useful for the simulation tick:
    float real_time_seconds = 0.0f;
};

// Engine.TeamInfo — one per team. AA3 always emits 4 of these (one per
// real team + Neutral / Spectator slots) even when only Allies/Enemy are
// used in BCT or INF gametypes.
// One replicated value, typed the way NetSerializeItem writes it.
struct DataValue {
    enum class Kind { kNone, kInt, kFloat, kBool, kStr, kName, kObj, kActor, kByte,
                      kVector, kRotator, kStruct, kRaw };
    Kind kind = Kind::kNone;
    int64_t i = 0;              // kInt, kBool, kByte (value), kName (Number)
    double f = 0.0;             // kFloat
    uint32_t bits = 0;          // kByte width (enum -> CeilLog2(NumEnums-1), else 8)
    std::string s;              // kStr, kName, kObj (package), kRaw ('0'/'1' bits)
    uint32_t net = 0;           // kObj: NetIndex within `s`
    const ServerActor* actor = nullptr;  // kActor: written as its channel on the connection
    int32_t v3[3] = {0, 0, 0};  // kVector, kRotator
    std::vector<DataValue> members;      // kStruct, in declaration order

    static DataValue Int(int64_t v) { DataValue d; d.kind = Kind::kInt; d.i = v; return d; }
    static DataValue Bool(bool v) { DataValue d; d.kind = Kind::kBool; d.i = v; return d; }
    static DataValue Str(const std::string& v) { DataValue d; d.kind = Kind::kStr; d.s = v; return d; }
    static DataValue Actor(const ServerActor* a) {
        DataValue d; d.kind = a ? Kind::kActor : Kind::kNone; d.actor = a; return d;
    }
};

// A property (or static-array element: elem >= 0) at a RepIndex.
struct DataField {
    uint32_t idx = 0;
    int32_t elem = -1;
    DataValue v;
    uint32_t revision = 0;   // bumped to re-send an unchanged value (ServerRoundActor::Touch)
    bool owner_only = false; // only to the connection owning the actor (bNetOwner fields)
};

struct ServerTeamInfo : ServerActor {
    // Retail's four TeamInfos have indices 0, 1, 3, 4 (doc sec 27).
    int32_t team_index = 0;
    int32_t score = 0;
    int32_t kills = 0;
    int32_t deaths = 0;
    std::string team_name;
    bool restrict_ft_roles_by_team_size = false;   // AA_TeamInfo
    // Role structure and membership (doc sec 28), as generic fields:
    // aamcrpiClassRoles[], iTotalClassRoles, aaftiFireTeams[],
    // aamcfsmPlayerSelectable[], assault/defend flags, and per member
    // aiTeamMemberPlayerIDs[] / ListOfProxies[] /
    // aiTeamMemberRoleSelectionPriority[]. The client's role menu reads
    // aamcrpiClassRoles.
    std::vector<DataField> round_fields;
    void SetRoundField(uint32_t idx, int32_t elem, const DataValue& v) {
        for (auto& f : round_fields)
            if (f.idx == idx && f.elem == elem) { f.v = v; return; }
        round_fields.push_back({idx, elem, v});
    }
};

// Engine.GameReplicationInfo + AA3 fields. The single most important
// actor for "is the match running?" state. Always Ch=4 on the wire.
struct ServerGameReplicationInfo : ServerActor {
    // Engine.GameReplicationInfo replicated:
    int32_t match_id = 0;
    int32_t winner = -1;
    bool    b_match_has_begun = false;     // → flip to true on player join, watch client
    bool    b_match_is_over   = false;
    // Engine default True; retail clears it ~1s into MatchInProgress and the
    // client acts on the change (doc sec 31).
    bool    b_stop_count_down = true;
    int32_t remaining_minute  = 0;
    int32_t remaining_time    = 0;
    int32_t elapsed_time      = 0;
    float   second_count      = 0.0f;
    int32_t goal_score        = 0;
    int32_t time_limit        = 600;       // 10-minute default
    int32_t max_lives         = 0;
    std::string server_name;
    std::string short_name;
    std::string admin_name;
    std::string admin_email;

    // AA3 additions (subset — fill the rest as we wire them):
    bool   b_tournament_discard_round = false;
    bool   b_show_hud_timer = true;
    bool   b_team_side_swapping_round = false;
    bool   b_allow_respawns = true;
    bool   b_allow_preplanning = false;
    bool   b_role_swapping_disabled = false;
    int32_t i_current_round = 1;
    int32_t i_max_rounds_per_match = 6;    // retail AA_GameInfo default (doc sec 36)
    int32_t i_vip_raw_team_index = -1;
    std::string s_base_map_name;
    // Co-op reinforcements: the HUD's countdown to the next wave
    // (AA_MCMissionManager.UpdateReinforcementPlayerInformation).
    float respawn_time_seconds = 0.0f;
    std::string s_mission_type;
    std::string s_mission_variant;
    std::string s_tod;
    std::string s_weather;

    // Match setup the client's menus read; defaults are retail's values
    // for Bridge classic (GamePlayPCAP_Legit_Server frame 73, doc sec 27).
    uint32_t game_class_object_index = 0;          // GameClass (AGP_GameInfo)
    std::string mp_state_name = "PendingMatch";    // MPStateName (FName)
    std::array<std::string, 2> team_names = {"Black Team", "Gold Team"};
    std::array<std::string, 2> mission_types = {"Assault", "Defend"};
    std::array<std::string, 2> mission_briefings = {
        "Rally at Location Zulu", "Prevent the enemy from reaching location Zulu"};
    std::string mission_active_sublevel;           // the gametype level
    // Free slots per team; 0 makes the team menu show FULL.
    std::array<int32_t, 2> empty_team_slots = {6, 6};
    int32_t mission_time_remaining = 420;
    uint8_t spectator_mode = 1;                    // eSpectatorMode, 2 bits
    int32_t incapacitation_withdraw_delay = 1;
    float   spectator_knockdown_trace_seconds = 5.0f;
    float   knockdown_trace_seconds = 15.0f;
    bool    advanced_inventory_management = true;
    bool    show_opponent_extraction_volume = true;
    bool    allow_spectator_bft = true;
    bool    allow_spectators_back_onto_teams = true;
    bool    show_enemy_neutralized_scoring_messages = true;
    bool    enable_coop_headshot_to_incap = true;
    bool    enable_mp_headshot_to_incap = true;
    bool    disable_surefire_flashlights = true;
    bool    allow_vote_ready = true;

    // Round setup + countdowns (retail frame 175 onward; doc sec 28). Mission
    // metadata is retail's for Bridge classic.
    int32_t role_selection_time = 10;
    int32_t role_selection_time_remaining = 0;
    int32_t deployment_time = 10;
    int32_t deployment_time_remaining = 0;
    int32_t mission_time_seconds = 360;
    int32_t min_player_count = 2;
    int32_t max_player_count = 12;
    std::string s_summary = "Bridge Classic";
    std::string s_type = "classic";
    std::string s_author = "Jonathan Breese";
    std::string s_mission_name = "Bridge - Classic";
    int32_t ready_votes_required = 1;
    int32_t ready_votes = 0;
    // Set when the match starts (retail frame 438): the HUD's round timer
    // counts iMissionTimeRemaining down from iMissionTimeLimitSeconds.
    int32_t mission_time_limit_seconds = 0;
    std::array<int32_t, 2> respawn_players_needed = {0, 0};   // Team0, Team1
    // Round results (doc sec 36): atsTeamScores[2] (TeamScores struct),
    // bLastRoundTied, and on an objective win the objective's PreplanSymbol
    // and the player who completed it. iBattlePlanningTimeRemaining = 30
    // while the scoreboard shows.
    struct TeamScore {
        int32_t won = 0, lost = 0, tied = 0;
        bool operator!=(const TeamScore& o) const { return won != o.won || lost != o.lost || tied != o.tied; }
    };
    std::array<TeamScore, 2> team_scores{};
    bool last_round_tied = false;
    std::string winning_objective;
    std::string winning_action_player;
    int32_t battle_planning_time_remaining = 0;

    // Pointers to the team list, by team index. Hosted as references
    // into the world's team vector — keep raw pointers since the world
    // owns the storage and lifetime is tied to it.
    std::array<ServerTeamInfo*, 4> teams = {nullptr, nullptr, nullptr, nullptr};
};

// AA3 PlayerReplicationInfo — one per connected human (and one per bot).
// First PRI gets Ch=10 on the wire; subsequent slots get Ch=11+.
struct ServerPlayerReplicationInfo : ServerActor {
    std::string player_name;
    int32_t player_id = 0;                // PlayerReplicationInfo.PlayerID
    ServerTeamInfo* team = nullptr;       // PlayerReplicationInfo.Team; null or the
                                          // Spectator TeamInfo = not playing
    bool OnPlayableTeam() const { return team && team->team_index >= 0 && team->team_index <= 1; }
    // AA3 soldier record. A fully qualified soldier, as retail's PRI open
    // for its test account (doc sec 27): the class/role menu reads these.
    int32_t access_rights = 5;
    int32_t tier_level = 4;               // SDM/AR/AutoRifleman/Grenadier tiers
    bool    qualified = true;             // BCT/MOUT/ES2 completed, reload/recover perks

    // Assigned class role (ServerSelectClassRole_ByIndex; doc sec 28).
    int32_t assigned_class_role_index = -1;
    std::string current_class_role_id;    // e.g. "T0F0R1"
    std::string class_role_name;          // e.g. "FTL0"
    const ServerActor* active_fire_team = nullptr;
    std::string primary_weapon_pkg;       // aawpnPrimaryOne: class reference
    uint32_t primary_weapon_net = 0;
    int32_t primary_one_ammo = 0;
    // mriPrimaryWeaponOneMods rails: Main, Top, Left, Right, Bottom, RearSight
    // (package + NetIndex of the mod class; empty package = None).
    std::array<std::pair<std::string, uint32_t>, 6> primary_mods{};
    // Grenades carried (AA_PlayerReplicationInfo.iFragGrenades ...): the
    // role's counts at spawn, one less per grenade taken in hand (retail
    // frames 376, 40214, 40898; doc sec 37). The InventoryManager keeps one
    // preloaded AA_ThrowWeapon of each type; these say how many are left.
    int32_t frag_grenades = 0;            // M67
    int32_t smoke_grenades = 0;           // M83
    int32_t m106_grenades = 0;            // M106
    int32_t stun_grenades = 0;            // M84
    int32_t incendiary_grenades = 0;      // AN-M14 TH3
    // bHasReviveKit: an IFAK in the pouch (AA_InventoryContainer_Pouch_IFAK
    // sets it; retail sends True at spawn). The client's treat prompt needs it.
    bool has_revive_kit = false;
    bool has_had_role_assigned = false;
    bool is_fire_team_leader = false;
    bool vote_ready = false;
    int32_t score   = 0;
    int32_t deaths  = 0;
    int32_t kills   = 0;
    bool    b_dead  = false;            // AA_PlayerReplicationInfo.bIsDead (retail t=81.83)
    bool    b_incapacitated = false;    // AA_PlayerReplicationInfo.bIsIncapacitated (doc sec 35)
    int32_t i_died  = 0;                // AA_PlayerReplicationInfo.iDied
    int32_t team_index = -1;            // -1 = spectator / unassigned
    uint8_t ping = 0;
    bool    b_is_spectator = false;
    bool    b_only_spectator = false;
    bool    b_admin = false;
    bool    b_out_of_lives = false;
    bool    b_bot = false;
    std::string unique_id_hex;          // FUniqueNetId Stringified
};

// PlayerController per connected human. NOT a placed actor — spawned
// server-side when a client joins. NetPlayerIndex maps a PC to its
// owning UNetConnection slot (0 = main, 1+ = split-screen children).
struct ServerPlayerController : ServerActor {
    uint8_t net_player_index = 0;
    uint32_t owning_client_id = 0;       // SocketManager::Connection id
    ServerPlayerReplicationInfo* pri = nullptr;
    struct ServerPawn* pawn = nullptr;   // Controller.Pawn — set once the Pawn exists
    // Carried objective notices for the protocol (Bridge AI capture
    // 2026-10-12): picked up -> SetNotificationMessage + ReceiveLocalizedMessage;
    // dropped (carrier killed) -> ClientSendObjectiveDroppedNotification +
    // ClientNotifyDiscardInventory(22).
    uint32_t objective_pickup_serial = 0;
    uint32_t objective_drop_serial = 0;
    uint32_t objective_incap_serial = 0;   // ClientNotifyIncapacitatedWithObjective()
    // ClientMessage(S, Type) calls for the protocol to send (co-op
    // reinforcement notices), oldest first.
    std::vector<std::pair<std::string, std::string>> pending_messages;
    // An enemy NPC neutralized by this player: ClientNotifyNeutralizedNME +
    // ClientNotifyScoringEventDetails(7, 32, +10) (retail 13:42:54).
    uint32_t npc_kill_serial = 0;
    std::string last_npc_kill;
    // Where the next spawn goes instead of the fire team's start (a co-op
    // reinforcement point).
    bool has_respawn_spot = false;
    float respawn_spot[3] = {0, 0, 0};
    int32_t respawn_spot_yaw = 0;
    struct ServerSteamStatManager* stat_manager = nullptr; // this client's Ch=3 actor
    struct ServerMOSQualificationBCT* mos_qual = nullptr;  // this client's Ch=5 actor
    // The other MOS qualifications retail opens beside BCT (LAN servers
    // authorize everything): DRM/Grenadier/AR/Rifleman AIT and Combat
    // Medic, which the client's treatment checks need (doc sec 39.7).
    std::vector<struct ServerMOSQualificationBCT*> extra_mos_quals;
    struct ServerPlayerStorageProxy* proxy = nullptr;     // AA_PlayerController.PlayerProxy
    // AA_PlayerController.bUseFlashSAI: retail's PC open sets it (doc sec 27).
    bool use_flash_sai = true;
    // Dead this round: no new pawn until respawn_at (World clock; < 0 = never
    // this round). Retail Bridge classic waits for the next round.
    bool died = false;
    double respawn_at = -1.0;
    // Spectating (death / round end; doc sec 36): Actor.bCollideWorld and
    // AA_PlayerController.SpectatorMode / Left|RightSpectateInfoString, as
    // retail sets them for a player without a pawn.
    bool collide_world = false;
    uint8_t spectator_mode = 0;          // eSpectatorMode, 2 bits
    std::string right_spectate_info;
    std::string left_spectate_info;

    // AA3 posture state (doc sec 22), driven by ServerSetCrouch/SetProne/
    // StandUp/HoldSprint/StopSprint/ToggleStance; mirrored into the pawn's
    // bIsCrouched/bIsProne and reported back via NotifyPostureChange.
    enum class Posture : uint8_t { kWalk, kCrouch, kProne, kSprint };
    Posture posture = Posture::kWalk;
    bool tactical_combat = false;

    // Client-side choices recorded for game logic / stats (doc sec 22).
    int32_t requested_team = -1;     // ServerJoinTeam
    int32_t class_role_index = -1;   // ServerSelectClassRole_ByIndex
    bool ready = false;              // ServerSetPlayerReady

    // Stats the server can measure itself today (doc sec 23), exported
    // alongside the AA3 stat blocks. Not part of any AA3 wire struct.
    struct ObservedStats {
        uint64_t connected_at_unix = 0;
        double seconds_played = 0.0;     // time since join (updated on export)
        double distance_uu = 0.0;        // sum of accepted ServerMove deltas
        uint32_t moves = 0;
        uint32_t sprints = 0;            // transitions into Sprint
        uint32_t posture_changes = 0;
        uint32_t stance_toggles = 0;
    } observed;
};

// AA_Pawn — the physical body a PlayerController possesses. Spawned
// alongside the PC/PRI on connect (this server has no team-select/deploy
// flow yet, so there's no later point to defer spawning to — see
// docs/AA3_ACTOR_REPLICATION.md sec 12.3). `location`/`rotation_*`/
// `velocity` are inherited from ServerActor and default to the origin —
// this server has no spawn-point data yet, so that's honest, not a bug.
struct ServerPawn : ServerActor {
    // An enemy NPC soldier (AA_NPCEnemyPawn; retail Bridge AI capture
    // 2026-10-12): no controller or PRI on the wire, its name in
    // AA_NPCPawn.sNPCName (= sBodySoldierName), iBodyTeamIndex 4.
    // AA_Pawn.bFirstReloadFinished: set when the drawn rifle's initial
    // reload ends; until then TakeDamage ignores every hit ("no damage during
    // initial reload to avoid spawn campers").
    bool first_reload_done = false;
    bool is_npc = false;
    std::string npc_name;
    struct ServerPawn* npc_last_hit_by = nullptr;   // NPC: who last shot it (it turns to face them)
    double npc_last_hit_at = -100.0;
    uint32_t last_hit_by_client = 0;  // the player whose shot last hurt it (kill credit)
    float health = 100.0f;               // Pawn.Health
    // Spawn inventory (doc sec 30): Pawn.InvManager and AA_Pawn.DumpPouch,
    // owner-only references (retail frame 368).
    const ServerActor* inv_manager = nullptr;
    const ServerActor* dump_pouch = nullptr;
    // AA_Pawn.NextMagPouch / NextStorageMagPouch: where a reload takes its
    // magazine from (retail t=23.79, the dump pouch). Without it the
    // client's reload loads nothing (doc sec 31).
    const ServerActor* next_mag_pouch = nullptr;
    // AA_Pawn.ActiveInventory: the equipped weapon. The client repeats
    // ServerSetCurrentWeapon until this names it (retail frame 376; doc sec 30).
    const ServerActor* active_inventory = nullptr;
    // Initial reload (doc sec 30): the magazine World::UpdateReloads loaded
    // into `loaded_weapon`; the protocol sends AA_Weapon.ClientLoadAmmoMag
    // when the serial changes.
    const ServerActor* loaded_weapon = nullptr;
    const ServerActor* loaded_mag = nullptr;
    uint32_t loaded_mag_serial = 0;
    // Reload notices for the owner's PlayerController (doc sec 31): bumped
    // by World, sent by the protocol when they change.
    uint32_t confirm_reload_serial = 0;   // AA_PlayerController.ConfirmReload()
    uint32_t mag_count_serial = 0;        // ClientAdvancedInventory*AmmoMagazineCount(s)
    int32_t spare_mags = 6;               // retail kit: 6 spare magazines (frame 365)
    // HUD inventory types of the kit (Engine EInventoryType): weapon 14 M4A1,
    // 16 M16A4, 15 M16DMR, 13 M249; ammo 17 556mmClip, 18 556mmLinked.
    uint8_t weapon_inv_type = 14;
    uint8_t ammo_inv_type = 17;
    // Trigger (doc sec 31): the server counts rounds like retail does.
    bool trigger_held = false;
    uint8_t shots_fired = 0;          // ImpactRepInfo.RepIdx (retail counts 1,2,3...)
    // eRateOfFire: 0 Safe, 1 Semi, 2 Burst, 3 Auto. The rifle is Safe until
    // the initial reload ends and while sprinting; selected_rof is what it
    // returns to (live retail capture t=29.75/31.05/32.79; doc sec 32).
    uint8_t selected_rof = 1;
    // Rounds in one of the kit's magazines (AmmoContainer RoundCount: 30,
    // the M249's box 200); the magazine is spent when RoundIndex reaches it.
    int32_t mag_rounds = 30;
    double next_shot_at = 0.0;
    int32_t burst_left = 0;
    ServerPlayerReplicationInfo* pri = nullptr;   // Pawn.PlayerReplicationInfo
    ServerPlayerController* controller = nullptr; // Pawn.Controller

    // Movement tuning (Pawn.*, RepIndex 31-36). Values are Engine.Pawn's own
    // defaultproperties (UnrealEngine3/.../Engine/Classes/Pawn.uc:3645-3668)
    // — AA_Pawn.uc has no defaultproperties override for any of these
    // (checked both the "uscript" and "uscript_decompiled" AA3Baseline
    // trees), so the stock engine values are what a real AA_Pawn actually
    // carries, not a guess.
    float air_control  = 0.05f;
    float jump_z        = 420.0f;
    float accel_rate    = 2048.0f;
    float air_speed     = 600.0f;
    float water_speed   = 300.0f;
    // Retail's owner pawn open sends GroundSpeed=200 and none of the other
    // tuning fields (frame 366): the client keeps AA3's own class defaults.
    // Sending these stock-engine values overrode AA3's movement tuning
    // (doc sec 30), so only GroundSpeed goes out now.
    float ground_speed  = 200.0f;
    bool  b_simulate_gravity = true;

    // Retail's owner pawn open (frame 366, doc sec 30): the inventory
    // association name every inventory item also carries ('AA_Pawn_0'
    // Number 163), the posture/animation state graph RepGraphStates[2]
    // ({67,0} -> {67,0}, new data), MovementSpeedModifier.
    std::string inventory_association = "AA_Pawn_0";
    int32_t inventory_association_number = 163;
    // AA_Pawn.RepGraphStates[]: AA3's animation state graphs, one
    // StateGraphState {CurrentStateID[2], DestinationStateID[2], bNewData}
    // per graph. The server drives them; the client's weapon leaves its
    // initial reload only when graph 4 reaches 79 (retail; doc sec 30).
    struct GraphState {
        uint8_t current[2] = {0, 0};
        uint8_t destination[2] = {0, 0};
        bool operator!=(const GraphState& o) const {
            return current[0] != o.current[0] || current[1] != o.current[1] ||
                   destination[0] != o.destination[0] || destination[1] != o.destination[1];
        }
    };
    // Server-side posture (doc sec 31): what graphs 2 (body) / 3 (weapon
    // carry) and GroundSpeed answer the client's posture requests with.
    bool tactical = false, sights = false, sprinting = false, crouched = false, prone = false;
    // Graph-2 transitions finish with a settled {state -> state} a moment
    // later (retail); queued here by World::ApplyPawnAction.
    struct GraphStep { double at; int graph; uint8_t cur, dst; };
    std::vector<GraphStep> graph_steps;
    double vault_gravity_at = -1.0;   // bSimulateGravity back on (vault landing)
    // Hold breath (doc sec 32): Asphyx fills 0..100 while held; full forces
    // the end (ClientEndHoldBreath(bForceCancel)) and bAsphyxiating until it
    // drains back under half. Rates are a guess -- no retail sample yet.
    // Death (doc sec 33): torn off like retail (bTearOff, Health 0, refs
    // cleared) -- the body stays on clients as a ragdoll and the channels close.
    bool torn_off = false;
    // Round end (doc sec 36): the controller let go of the pawn (retail
    // AA_PlayerController.GameHasEnded -> UnPossess) -- Owner/Controller/PRI
    // None, the owner's copy drops to SimulatedProxy. The body stays until
    // the level reset, which `destroyed` marks: every channel to it closes.
    bool unpossessed = false;
    bool destroyed = false;
    // Incapacitation (doc sec 35, retail capture 2026-10-09 t=227.04):
    // damage that leaves Health at or under AA_Pawn.IncapacitationHealth (25)
    // puts the soldier down instead of killing: Health clamped to 25,
    // ClientIncapacitated(), incap graphs, IncapacitationType = the damage
    // class. The soldier stays down until finished by a hit, treated, the
    // round ends, or (only if bleeding) Health drains to 0.
    bool incapacitated = false;
    double incap_at = -1.0;
    uint32_t incap_serial = 0;            // owner notices: ClientIncapacitated + PC notices
    uint32_t incap_type_index = 0;        // IncapacitationType wire ObjectIndex (static class ref)
    std::string incap_instigator = "Unknown";
    int incap_stage = 0;                  // 0 down, 1 +0.23 s, 2 ragdoll settled (+1.2 s)
    bool bleeding = false;                // AA_Pawn.bIsBleeding: Health drains (to HealthTarget, or while down)
    // Wounds and treatment (doc sec 39, retail capture 2026-10-11). Per
    // eHitLocation (18) up to MAX_WOUNDS (10): AA_Pawn.ReplicatedInjuryVariables
    // {BleedRate, WoundTreatmentProgress[10]} and RepNotifyInjuryVariables
    // {WoundStatus[10] = {WoundType, step, CurrentStatusLevel}}.
    static constexpr int kHitLocations = 18;
    static constexpr int kMaxWounds = 10;
    struct Wound {
        bool present = false;
        bool treated = false;             // CurrentStatusLevel 1
        float progress = 0.0f;            // WoundTreatmentProgress
        bool operator!=(const Wound& o) const {
            return present != o.present || treated != o.treated || progress != o.progress;
        }
    };
    std::array<std::array<Wound, kMaxWounds>, kHitLocations> wounds{};
    std::array<float, kHitLocations> bleed_rate{};
    const ServerPawn* treater = nullptr;  // AA_Pawn.CurrentTreatmentTreater[0]
    ServerPawn* patient = nullptr;        // AA_Pawn.CurrentTreatmentPatient (medic side)
    bool passive_treatment = false;       // AA_Pawn.bPassiveTreatmentActive
    bool recapacitated = false;           // AA_Pawn.bRecapacitated
    bool reset_weapon_ik = false;         // AA_Pawn.bResetWeaponIK
    uint32_t recap_serial = 0;            // revived: ClientRecapacitate + PC notices to the owner
    bool recap_used_own_ifak = false;     // ...and ClientNotifyDiscardInventory(IFAK)
    std::string last_attacker;            // instigator name for a bleed-out incapacitation
    float target_before_incap = 0.0f;     // HealthTarget when downed (the bleed still owed)
    bool HasInjury(bool lower) const {
        for (int l = 0; l < kHitLocations; ++l) {
            if ((l >= 12) != lower) continue;             // HL_UpperLegRight.. = lower body
            for (const Wound& w : wounds[static_cast<size_t>(l)])
                if (w.present && !w.treated) return true;
        }
        return false;
    }
    bool ragdoll = false;                 // bRagdollTick + bRagdollStuck: settle into the incap pose
    float health_target = 100.0f;     // AA_Pawn.HealthTarget: set with Health on a hit (retail)
    // Owner-side notices the protocol sends when these serials change.
    uint32_t hit_serial = 0;          // PC.ClientSendHitNotification(hit_location, DamageType)
    float hit_location[3] = {0, 0, 0};
    uint32_t hit_damage_class = 0;    // AA3Baseline class export (6464 bullet, 6486 fall)
    uint32_t sling_serial = 0;        // AA_Pawn.ClientSlingWeapon() (ladder mount)
    uint32_t vault_finish_serial = 0; // AA_Pawn.ClientFinishVaultMantleMoveToTarget()
    uint32_t ladder_finish_serial = 0; // AA_Pawn.ClientFinishLadderMoveToTarget()
    bool moving_to_target = false;    // AA_Pawn.bAdvMvmt_MovingToTarget
    bool move_to_ladder = false;      // the move's callback is FinishLadderMoveToTarget
    double vault_finish_at = -1.0;
    float vault_target[3] = {0, 0, 0};
    bool on_ladder = false;
    bool ladder_down = false;         // mounted from the top (climbing down)
    bool holding_breath = false;
    bool asphyxiating = false;
    uint32_t breath_end_serial = 0;   // forced ends for the protocol to announce
    double speed_at = -1.0;      // GroundSpeed becomes speed_after then (getting down/up)
    float speed_after = 0.0f;
    static constexpr int kGraphCount = 8;
    GraphState graphs[kGraphCount] = {{}, {}, {{67, 0}, {67, 0}}};
    void SetGraph(int i, uint8_t cur, uint8_t dst) {
        graphs[i].current[0] = cur; graphs[i].current[1] = 0;
        graphs[i].destination[0] = dst; graphs[i].destination[1] = 0;
    }
    float movement_speed_modifier = 0.8852f;
    int32_t original_owning_team = 0;          // Actor.iOriginalOwningTeamIndex
    const ServerActor* proxy = nullptr;        // AA_Pawn.PlayerProxy

    // Grenade in hand (doc sec 37, retail GamePlayPCAP_Legit_Server
    // t=436-451): taken out with ServerSetCurrentWeapon, pin pulled on
    // ServerStartFire, cooked with CookIt, thrown 0.49 s after
    // ServerStopFire. `rifle` is the weapon it replaced, re-drawn on the
    // switch back.
    const ServerActor* held_grenade = nullptr;
    const ServerActor* rifle = nullptr;
    double grenade_equip_at = -1.0;
    int grenade_stage = 0;               // 0 holstering rifle, 1 grenade shown, 2 in hand
    bool pin_pulled = false;
    bool cooking = false;
    bool underhand = false;              // AA_ThrowWeapon.SwitchThrowType
    double cook_start = 0.0;
    double throw_at = -1.0;
    double rifle_draw_at = -1.0;         // rifle back on the pawn (+0.2 s)
    uint32_t switch_best_serial = 0;     // Controller.ClientSwitchToBestWeapon(True) after a throw
    // A cook (AA_ThrowWeapon.CookIt) runs its own clock, thrown or not
    // (retail t=447.84): bCooking clears when the fuse would have run out
    // (+5.0 s for the M67, t=452.80) and CookingDelayedRestoreTimer (fuse +
    // 0.25) -> RequestWeaponRestoreTimer (+0.9) has the server put the
    // previous weapon back in hand: InventoryManager.SetCurrentWeapon(rifle)
    // to the client (t=453.94).
    const ServerActor* cooked_grenade = nullptr;
    double cook_clear_at = -1.0;
    double cook_restore_at = -1.0;
    uint32_t weapon_restore_serial = 0;  // InventoryManager.SetCurrentWeapon(rifle) to send
    // Held past the fuse: AA_ThrowWeapon.HolderDied -> ClientHolderDied().
    const ServerActor* holder_died_grenade = nullptr;
    uint32_t holder_died_serial = 0;

    // Runtime animation/stance state (Pawn.*, RepIndex 37-43, unchanged by doc sec 19). No
    // defaultproperties for these — they're live state, not spawn-time
    // constants — so 0/false is the correct "nothing happening yet" value,
    // not a placeholder.
    uint8_t firing_mode = 0;
    uint8_t flash_count = 0;
    uint8_t remote_view_pitch = 0;
    uint8_t physics = 1;  // EPhysics: PHYS_Walking (sent to non-owners only)
    bool    b_is_prone = false;
    bool    b_is_crouched = false;
    bool    b_is_walking = false;

    // Body identity (AA_Pawn.*, rep::Pawn::sBodySoldierName/iBodyTeamIndex). No real soldier-model
    // selection logic exists yet, so these stay at the honest "unset"
    // default rather than a fabricated model name.
    std::string body_soldier_name;
    int32_t body_team_index = 0;

    // AA3-specific state (AA_Pawn.*, see rep_layout_gen.h for indices). No
    // defaultproperties override exists for any of these either (same
    // check as the movement-tuning block above), and unlike that block
    // these are plausible "accumulator" / "no offset" meters where the
    // script-default 0 is a safe, sensible value — not skipped over
    // MovementSpeedModifier/HealthTarget, which read as multiplier/
    // lerp-target fields where a bare 0 would be actively misleading (see
    // docs/AA3_ACTOR_REPLICATION.md sec 12.5).
    float   lean_direction = 0.0f;                // fDesiredLeanDirection
    ServerPlayerReplicationInfo* last_known_pri = nullptr; // LastKnownPRI
    float   stamina = 100.0f;     // AA3 default; retail never sends it at rest
    float   asphyx = 0.0f;
    int32_t aim_offset = 0;
    uint8_t incapacitation_loc = 0;                // eHitLocation, 5 bits
    float   vault_mantle_height = 0.0f;            // AdvMvmt_VaultMantleHeight

    // rep::Pawn::PawnShotEffectInfo : StructProperty AA_PawnHitEffectInfo
    // (2 fields, resolved via --struct-layout: HitVector then HitRotation,
    // both unconditional — struct members have no per-field RepIndex or
    // CPF_Net condition, see doc sec 12.6). Zero vector/rotation is the
    // honest "no hit has occurred" state — this server has no combat/damage
    // system, so there's never a real hit to report.
    float   shot_effect_hit_location[3] = {0.0f, 0.0f, 0.0f};
    int32_t shot_effect_hit_rotation_pitch = 0;
    int32_t shot_effect_hit_rotation_yaw   = 0;
    int32_t shot_effect_hit_rotation_roll  = 0;
};

// AA_SteamStatManager — one per connected human, holds match/round combat
// stat counters (shots fired, kills, hit locations, etc. — 128 fields total
// across its 2 replicated structs). No data members here at all: every one
// of those 128 fields is a counter this server has no combat system to
// ever increment, so "all zero / no stats recorded" isn't a placeholder,
// it's the complete and correct state. See rep_encode.h/.cpp and
// docs/AA3_ACTOR_REPLICATION.md sec 12.8.
// The per-player AA3 stat blocks (doc sec 23). The server is the stats
// authority: retail pushes both structs at round end and then calls
// ClientTransmitLastRoundStats so the client commits them (to Steam).
// Field names/types are generated from the cooked structs.
struct ServerSteamStatManager : ServerActor {
    PlayerMatchStats match;
    PlayerRoundStats round;
};

// AA_MOSQualificationBCT — one per connected human, marks that player's
// "Basic Combat Training" qualification. `--rep-layout` gives MaxIndex=23:
// zero own properties (only the inherited Actor-base 23, which this project
// consistently doesn't encode — see rep_encode.h's WorldInfo/GRI/etc.
// notes), so there's nothing this actor could ever replicate beyond its
// channel-open transient preamble. No data members needed for the same
// reason `ServerSteamStatManager` has none — there's no property to hold.
struct ServerMOSQualificationBCT : ServerActor {};

// AA_PlayerStorageProxy -- one per connected human; the client's
// AA_PlayerController.PlayerProxy, read by its use/weapon/inventory code
// (without it: "Accessed None 'PlayerProxy'" on every key). Retail opens it
// on Ch=18 with Owner/bNetOwner/bIsDead/bActive/RepPRI/RepController
// (doc sec 27).
struct ServerPlayerStorageProxy : ServerActor {
    struct ServerPlayerController* controller = nullptr;
    ServerPlayerReplicationInfo* pri = nullptr;
    bool is_dead = true;     // no pawn yet
    bool incapacitated = false;  // bIncapacitated (doc sec 35)
    bool is_spectating = false;  // bIsSpectating: dead / round over (doc sec 36)
    bool active = true;
};

// Engine.GameInfo — exists on the server only (no client replica). Holds
// the rules and spawns the GRI / TeamInfos / PCs. Not a `ServerActor` —
// no channel, no client replica — but we keep the data here so a future
// game-logic tick can read it.
struct ServerGameInfo {
    std::string class_name = "AGP_Game_INF_Classic"; // matches MapName in INI typically
    int32_t default_max_lives = 0;
    int32_t default_time_limit_seconds = 420;   // retail Bridge classic (iMissionTimeLimitSeconds)
    bool    b_team_game = true;
    bool    b_use_seamless_travel = false;
    int32_t num_players = 0;
    int32_t num_bots    = 0;
    int32_t num_spectators = 0;
};

} // namespace game
} // namespace aa3

#endif // AA3_GAME_SERVER_ACTORS_H
