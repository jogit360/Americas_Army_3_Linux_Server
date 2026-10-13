#ifndef AA3_WORLD_H
#define AA3_WORLD_H

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "game/game.h"
#include "game/server_actors.h"
#include "game/collision.h"
#include "game/round_actors.h"
#include "game/navgrid.h"
#include <atomic>
#include <thread>

namespace aa3 {
namespace engine { struct MapManifest; class MapBundle; }
namespace game {

class World {
public:
    World();
    ~World();

    void Initialize(const std::string& map_name);

    // Resolve every actor's wire ObjectIndex against the PackageMap a client
    // builds from this manifest's USES list: ObjectBase(package) + the
    // class default object's NetIndex (doc sec 26). Must run before any
    // client connects; false (and every index left 0) if the manifest
    // lacks net object counts -- regenerate it with aa3_build_manifests.
    bool BindPackageMap(const engine::MapManifest& manifest,
                        const std::string& game_mode_level = "",
                        const engine::MapBundle* bundle = nullptr);

    // Game mode (doc sec 27): the gametype sublevel the client streams in
    // (configured, or "<Base>_inf_classic_large_dev" like retail), all of
    // the map's streaming sublevels, and the always-visible pathing level.
    const std::string& GameModeLevel() const { return game_mode_level_; }
    const std::string& PathingLevel() const { return pathing_level_; }
    const std::vector<std::string>& StreamingLevelNames() const { return streaming_levels_; }

    // Round phases, as retail's GRI MPStateName (doc sec 28):
    //   PendingMatch -> MissionPrePlanning_RoleSelection (10s, cut short when
    //   every team player is ready) -> MissionPrePlanning_DeploymentWarning
    //   (10s) -> InProgress (bMatchHasBegun). The protocol layer watches
    //   PhaseSerial() and sends each phase's client RPCs.
    // After InProgress (doc sec 36, retail AA_GameInfo states MatchEnded ->
    // ResetLevel -> ShowScoreboard): kRoundOver is MatchEnded (bMatchIsOver,
    // pawns unpossessed, MPStateName unchanged) for 2s; kShowScoreboard is
    // the reset level with the after-action report for 15s; then the next
    // round's role selection (1s unless a side swap or a new match).
    enum class MatchPhase { kPendingMatch, kRoleSelection, kDeploymentWarning, kInProgress,
                            kRoundOver, kShowScoreboard };
    MatchPhase Phase() const { return phase_; }
    uint32_t PhaseSerial() const { return phase_serial_; }
    // The round that just ended: winning raw team index (-1 = tie) and its
    // number (the GRI's iCurrentRound moves on at the reset).
    int32_t RoundWinner() const { return round_winner_; }
    int32_t FinishedRound() const { return finished_round_; }
    // True for the round end that announces the side swap.
    bool NextRoundSwaps() const { return next_round_swaps_; }
    // Wire ObjectIndex of the victory / defeat stinger (S_Music) and of a
    // random AA_Viewpoint of the map (the reset's view target).
    uint32_t StingerIndex(bool victory) const;
    uint32_t RandomViewpointIndex() const;
    // Bumped when ES2/match actors get reset to their round-start state: the
    // protocol closes their channels and reopens them at match start.
    uint32_t MatchActorSerial() const { return match_actor_serial_; }
    // Raw team index of the player's TeamInfo (-1 = none).
    static int32_t TeamOf(const ServerPlayerController& pc);
    // ServerSetPlayerReady: counts toward GRI iReadyVotes.
    void SetPlayerReady(ServerPlayerController& pc, bool ready);

    // Round actors (doc sec 28), loaded from config/maps/<GameModeLevel>.round.json
    // by BindPackageMap; opened to each player when role selection starts.
    const std::vector<std::unique_ptr<ServerRoundActor>>& RoundActors() const { return round_actors_; }
    // Placed map actors retail replicates from join (doors, breakable
    // windows, ladder volumes, dynamic meshes; doc sec 31), from
    // config/maps/<map>.map_actors.json.
    const std::vector<std::unique_ptr<ServerRoundActor>>& MapActors() const { return map_actors_; }
    // Static geometry of the map for traces (doc sec 32).
    const CollisionWorld& Collision() const { return collision_; }
    // Mission actors retail opens at join (objective marker, breakable-window
    // manager) and at match start (the ES2 briefcases/perks/items of
    // interest -- AA3's Use goes through ES2), from
    // config/maps/<GameModeLevel>.join.json / .match.json (doc sec 31).
    const std::vector<std::unique_ptr<ServerRoundActor>>& JoinActors() const { return join_actors_; }
    const std::vector<std::unique_ptr<ServerRoundActor>>& MatchActors() const { return match_actors_; }
    // ServerOpenDoor/ServerCloseDoor(door): `door_object_index` is the
    // static ObjectIndex the client sent. False if it names no door.
    bool UseDoor(uint32_t door_object_index, bool open);
    // Bumped whenever a round actor's fields change (role assignment).
    uint32_t RoundSerial() const { return round_serial_; }
    // ServerSelectClassRole_ByIndex: give the player the role at `role_index`
    // in their team's role list. False if there's no such free slot.
    bool AssignRole(ServerPlayerController& pc, int32_t role_index);
    void ReleaseRole(ServerPlayerReplicationInfo& pri);
    // Wire ObjectIndex of an object in `package` (ObjectBase + NetIndex), or 0.
    uint32_t ObjectIndex(const std::string& package, uint32_t net_index) const;

    // Teams. TeamInfo indices are retail's 0, 1, 3, 4; 0 = Black, 1 = Gold.
    ServerTeamInfo* FindTeam(int32_t team_index);
    // Puts the player on Black/Gold if there's a free slot (GRI
    // aiEmptyTeamSlots); false if not. LeaveTeam frees the slot.
    bool JoinTeam(ServerPlayerController& pc, int32_t team_index);
    void LeaveTeam(ServerPlayerReplicationInfo& pri);
    // Rebuilds each TeamInfo's member arrays (doc sec 28).
    void UpdateTeamMembers();

    // Round timers in seconds (GRI iRoleSelectionTime / iDeploymentTime);
    // retail's are 10/10. <= 0 leaves a timer unchanged.
    void SetRoundTimes(int32_t role_selection_seconds, int32_t deployment_seconds);
    // Where a team's players spawn (doc sec 29). Default for both: retail
    // Bridge classic's Black spawn, the only one in the capture.
    void SetSpawnPoint(int32_t team_index, float x, float y, float z, int32_t yaw);
    // Gives a player on Black/Gold their soldier (once), with inventory.
    ServerPawn* SpawnPawn(ServerPlayerController& pc);
    void ApplyRoleKit(ServerPlayerController& pc, std::vector<std::unique_ptr<ServerRoundActor>>& items);
    // The client equipped `weapon` (ServerSetCurrentWeapon): run retail's
    // initial reload -- the first magazine comes out of the dump pouch at
    // +1.4s and is loaded at +2.6s (doc sec 30).
    void BeginInitialReload(ServerPawn* pawn, const ServerActor* weapon);
    // A posture request from the pawn's player (doc sec 31): sets the
    // pawn's body/weapon graphs and GroundSpeed the way retail answers it.
    enum class PawnAction { kSetTactical, kRaiseSights, kLowerSights, kSprint,
                            kStopSprint, kCrouch, kStand, kProne, kVault };
    // AA_Pawn.ServerTryToReload (doc sec 31): retail's reload timeline.
    void BeginReload(ServerPawn* pawn);
    // AA_Pawn.ServerBeginHoldBreath / ServerEndHoldBreath. Begin returns
    // false if the pawn can't hold breath right now (sights down, gasping).
    bool BeginHoldBreath(ServerPawn* pawn);
    // AA_Pawn.ServerStartMoveToTarget (vault/mantle: the client picks the
    // target), ServerClimbLadder / ServerEndClimbLadder (doc sec 33).
    void StartMoveToTarget(ServerPawn* pawn, const float target[3], bool ladder = false);
    void ClimbLadder(ServerPawn* pawn, bool on);
    // Withdraw (MEDEVAC) while incapacitated: dies as usual (doc sec 35).
    bool Withdraw(ServerPawn* pawn);
    void EndHoldBreath(ServerPawn* pawn);
    // Weapon.ServerStartFire / ServerStopFire on the pawn's loaded weapon.
    // Retail's server counts every shot: the magazine's RoundIndex goes up
    // by one per round, and the client's ammo follows it (doc sec 31).
    void SetTrigger(ServerPawn* pawn, bool down);
    void ApplyPawnAction(ServerPawn* pawn, PawnAction a, bool arg = false);
    // ServerSetRailMod("PrimaryOne", rail, index) (retail frames 218/234,
    // doc sec 37): the PRI's mriPrimaryWeaponOneMods rail takes element
    // `index` of the role's allowed list for that rail; -1 clears it. The
    // mods are built on the rifle at the next spawn. False if refused.
    bool SetRailMod(ServerPlayerController& pc, const std::string& weapon,
                    const std::string& rail, int32_t index);

    // IFAK treatment (doc sec 39; src/game/medic.cpp). ServerStartTreatment:
    // the patient named, if `medic` can treat them (null if not).
    ServerPawn* StartTreatment(ServerPawn* medic, const std::string& patient_name);
    // ServerStartPassiveTreatment: stop the bleeding, work the wounds.
    bool StartPassiveTreatment(ServerPawn* medic);
    // ServerStopTreatment (also when the patient is lost).
    void StopTreatment(ServerPawn* medic);
    // ServerGiveActiveTreatment(type, ..., name): on a downed patient, an
    // IFAK (the patient's first) recapacitates them. False if nothing done.
    bool GiveActiveTreatment(ServerPawn* medic, const std::string& type, const std::string& patient_name);

    // ServerES2Use(obj) on an AA_ES2InstaPerk (AA_ES2InstaPerk.PerformES2Use):
    // one item from the perk -- a grenade of its type, or a magazine/box for
    // the matching weapon -- and PerkAmount down by one; emptied, it's gone.
    // False if nothing was taken.
    bool UseES2Object(ServerPlayerController& pc, const ServerActor* obj);

    // Grenades (doc sec 37; retail GamePlayPCAP_Legit_Server t=436-510).
    // EInventoryType of a pawn's AA_ThrowWeapon (0 if `a` is none).
    uint8_t GrenadeInventoryType(const ServerPawn* pawn, const ServerActor* a) const;
    // ServerSetCurrentWeapon(grenade): one comes out of the PRI count and
    // into the hand. False when none of that type are left.
    bool TakeGrenade(ServerPawn* pawn, const ServerActor* grenade);
    // ServerSetCurrentWeapon(rifle) while a grenade is in hand (or after a
    // throw): an unthrown grenade goes back to its pouch, the rifle is drawn.
    // Returns the grenade that was in hand (null if none).
    const ServerActor* PutAwayGrenade(ServerPawn* pawn, const ServerActor* weapon);
    // AA_ThrowWeapon RPCs on the grenade in hand.
    enum class GrenadeInput { kPullPin, kCook, kRelease, kSwitchThrowType };
    void GrenadeAction(ServerPawn* pawn, const ServerActor* grenade, GrenadeInput in);
    // Thrown grenades in flight or burning: opened to every player, closed
    // when `gone` (their LifeSpan, as retail), then dropped a while later.
    struct ThrownGrenade {
        std::unique_ptr<ServerRoundActor> actor;
        ServerPawn* instigator = nullptr;
        int spec = 0;
        double thrown_at = 0.0, explode_at = 0.0, gone_at = 0.0, drop_at = 0.0;
        Vec3 pos, vel;
        bool resting = false, exploded = false, gone = false;
        double next_burn = 0.0;
    };
    const std::vector<ThrownGrenade>& ThrownGrenades() const { return thrown_; }

    // Carried objective (the briefcase of a co mission; Bridge AI capture
    // 2026-10-12): the briefcase and dropped-briefcase actors, opened to
    // every player and closed when `gone` (a pickup closes the briefcase
    // and opens a fresh, owned one, as retail).
    const std::vector<std::unique_ptr<ServerRoundActor>>& ObjectiveActors() const { return objective_actors_; }
    // AA_PlayerController.ServerRepExec('Pickup', ContextActor): the
    // context is the factory (a static object index) or a dropped
    // briefcase (one of the player's channels).
    bool PickUpObjective(ServerPlayerController& pc, uint32_t static_obj, const ServerActor* actor);
    const ServerPawn* ObjectiveCarrier() const { return carry_.carrier; }
    // AA_PlayerController.ServerDropObjective: the carrier (downed, as the
    // HUD offers it) puts the case down 50 units in front for a teammate.
    bool DropObjectiveByChoice(ServerPlayerController& pc);

    // Co-op reinforcements (AA_PlayerController.ServerRequestRespawn, the
    // F5 key; AA_MCMissionManager.JoinPlayMidRound /
    // PlayerReinforcementSpawnMonitor): a dead or spectating player joins
    // their team's queue; every 10 s each team's queue deploys, up to 4
    // players, at the mission's AA_MCPlayerRespawnMarkers.
    void RequestRespawn(ServerPlayerController& pc);

    // Enemy NPC soldiers (AA_NPCEnemyPawn; npc.cpp), opened to every player.
    std::vector<const ServerPawn*> NpcPawns() const;
    // Loose things on the ground everyone sees (a killed NPC's rifle,
    // AA_DroppedPickup_*): opened to every player, closed when `gone`.
    const std::vector<std::unique_ptr<ServerRoundActor>>& WorldItems() const { return world_items_; }
    void SetEnemyNPCs(bool on) { npcs_enabled_ = on; }   // INI EnemyNPCs (default on)
    // INI MovementChecks (default on): refuse impossible client moves.
    // Scripted protocol tests that place their soldiers turn it off.
    void SetMovementChecks(bool on) { movement_checks_ = on; }
    bool MovementChecks() const { return movement_checks_; }
    bool SupportsRespawning() const { return !respawn_markers_.empty(); }
    // AA3Baseline class AA_Objective_Briefcase_US's wire index (the pickup
    // message names it), 0 if none.
    uint32_t ObjectiveItemClassIndex() const;

    // A pawn's inventory actors (doc sec 30), or null.
    const std::vector<std::unique_ptr<ServerRoundActor>>* PawnInventory(const ServerPawn* p) const {
        auto it = pawn_inventory_.find(p);
        return it == pawn_inventory_.end() ? nullptr : &it->second;
    }
    void Update(double delta_time);

    const std::string& GetMapName() const { return map_name_; }
    const std::string& GetTOD() const { return tod_; }

    // Server actor accessors. All non-null after Initialize().
    ServerWorldInfo*           GetWorldInfo()   { return world_info_.get(); }
    ServerGameReplicationInfo* GetGRI()         { return gri_.get(); }
    ServerGameInfo*            GetGameInfo()    { return game_info_.get(); }
    ServerTeamInfo*            GetTeam(size_t i){ return i < teams_.size() ? &teams_[i] : nullptr; }
    size_t                     GetTeamCount() const { return teams_.size(); }

    // Add / remove player-bound actors. Called from the network layer
    // when a client connects or disconnects.
    ServerPlayerController* CreatePlayerController(uint32_t owning_client_id,
                                                   const std::string& player_name = "");
    void                    RemovePlayerController(uint32_t owning_client_id);
    ServerPlayerController* FindPlayerController(uint32_t owning_client_id);
    const std::vector<std::unique_ptr<ServerPlayerController>>& GetPlayerControllers() const {
        return controllers_;
    }

private:
    void InitializeActors();

public:
    uint32_t BaselineIndex(uint32_t net_index) const;   // AA3Baseline.u object
    uint32_t GameIndex(uint32_t net_index) const;       // AA3Game.u object
    // ObjectIndex of the AGP_HUD class, ClientSetHUD's argument.
    uint32_t HUDClassIndex() const;
private:

    // ObjectBase of AA3Baseline / AA3Game in this map's PackageMap; 0 until
    // BindPackageMap succeeds.
    void ConfigureGameMode(const engine::MapManifest& manifest, const std::string& configured);
public:
    // "Bridge" for Bridge_night_clear (from its <Base>_pathing shard), and the
    // gametype level the server runs: configured, else <Base>_inf_classic_large_dev.
    static std::string MapBaseName(const engine::MapManifest& manifest);
    static std::string ResolveGameModeLevel(const engine::MapManifest& manifest,
                                            const std::string& configured);
private:
    bool LoadRoundActors(const std::string& path);
    bool LoadRoundActorsFromJson(const std::string& json);
    // Actor data files from extract_round_actors.py: each actor's fields plus
    // references kept as the retail channel they pointed at.
    struct ActorTemplate {
        ServerRoundActor proto;
        int64_t retail_ch = 0;
        struct Ref { uint32_t idx; int32_t elem; int64_t retail_ch; };
        std::vector<Ref> refs;
    };
    bool ReadActorTemplates(const std::string& path, const char* what,
                            std::vector<ActorTemplate>& out);
    bool ReadActorTemplatesFromJson(const std::string& json, const char* what,
                                    std::vector<ActorTemplate>& out);
    std::vector<std::unique_ptr<ServerRoundActor>> Instantiate(
        const std::vector<ActorTemplate>& templates,
        const std::map<int64_t, const ServerActor*>& outside);
    std::vector<ActorTemplate> inventory_template_;
    // Weapons (src/game/weapons.cpp): rail mods on the rifle, grenades.
    void BuildRailMods(ServerPlayerController& pc, std::vector<std::unique_ptr<ServerRoundActor>>& items,
                       ServerRoundActor& weapon, uint32_t default_sight_cls);
    void UpdateGrenades(double dt);
    // Wounds, bleeding and treatment (src/game/medic.cpp).
    void UpdateInjuries(double dt);
    void AddWound(ServerPawn& victim, int location, float bleed, const std::string& attacker);
    ServerPawn* FindPatient(const ServerPawn& medic, const std::string& name);
    void Recapacitate(ServerPawn& patient);
    void ThrowGrenade(ServerPawn& pawn, ServerRoundActor& grenade, bool in_hand);
    void ReplaceGrenade(ServerPawn& pawn, ServerRoundActor& thrown);
    void ExplodeGrenade(ThrownGrenade& g);
    void BurnAround(ThrownGrenade& g);
    // Damage that isn't a bullet (grenades): `damage` points of `damage_class`.
    void ApplyDamage(ServerPawn& victim, ServerPawn* instigator, float damage, uint32_t damage_class,
                     const Vec3& at, const Vec3& dir);
    std::vector<ThrownGrenade> thrown_;
    struct CarriedObjective {
        bool active = false;
        std::string factory_pkg;
        uint32_t factory_net = 0;
        float home[3] = {0, 0, 0};
        ServerRoundActor* factory = nullptr;     // placed AA_PickupFactory_Objective (join actor)
        ServerRoundActor* marker = nullptr;      // AA_MCCarryableObjectiveMarker (join actor)
        ServerRoundActor* item = nullptr;        // the briefcase now (in objective_actors_)
        ServerRoundActor* dropped = nullptr;     // AA_DroppedObjective lying on the ground
        ServerPawn* carrier = nullptr;
        ServerRoundActor proto;                  // the briefcase at rest, from the join actors
        double settle_at = -1.0;                 // dropped case lands (Physics None)
    } carry_;
    std::vector<std::unique_ptr<ServerRoundActor>> objective_actors_;
    struct RespawnMarker {
        int32_t team = 0;
        bool enabled = true;
        float loc[3] = {0, 0, 0};
        int32_t yaw = 0;
        double last_used = -100.0;
    };
    std::vector<RespawnMarker> respawn_markers_;
    struct NpcStart { float loc[3] = {0, 0, 0}; int32_t yaw = 0; std::string weapon; };
    std::map<const ServerPawn*, std::string> npc_weapon_;       // each NPC's rifle class
    std::vector<NpcStart> npc_starts_;
    std::vector<std::array<float, 3>> npc_firing_positions_;
    int32_t npc_max_ = 0;
    bool npcs_enabled_ = true;
    bool movement_checks_ = true;
    struct NpcBrain {
        ServerPawn* target = nullptr;
        double spotted_at = 0.0, next_shot_at = 0.0, next_look_at = 0.0;
        int burst_left = 3;
        Vec3 home;                              // its start
        std::vector<Vec3> path;                 // floor points to walk
        size_t path_i = 0;
        float speed = 170.0f;                   // walking; running to cover 300
        double next_patrol_at = 0.0;
        double last_seen_at = -100.0;
        Vec3 last_seen;
        double next_grenade_at = 0.0;
        double crouch_until = 0.0;
    };
    // The walkable grid for NPC routes, built in the background at load
    // (a copy of the collision; ~10-20 s); NPCs hold position until ready.
    std::unique_ptr<NavGrid> navgrid_;
    std::atomic<bool> nav_ready_{false};
    std::thread nav_thread_;
    void StartNavBuild();
    bool NpcRoute(ServerPawn& npc, NpcBrain& b, const Vec3& to, float speed);
    void NpcMove(ServerPawn& npc, NpcBrain& b, double dt);
    std::map<const ServerPawn*, NpcBrain> npc_brains_;
    double next_npc_grenade_at_ = 0.0;          // one NPC frag at a time map-wide
    std::vector<std::unique_ptr<ServerRoundActor>> world_items_;
    struct ItemTimer { ServerRoundActor* item; double settle_at, expire_at; };
    std::vector<ItemTimer> item_timers_;
    void DropNpcWeapon(const ServerPawn& npc);
    void NpcThrowFrag(ServerPawn& npc, const Vec3& target);
    void UpdateWorldItems();
    void SpawnNPCs();
    void GiveNpcGear(ServerPawn& npc, const std::string& weapon);
    void UpdateNPCs(double dt);
    bool NpcCanSee(const ServerPawn& npc, const ServerPawn& target) const;
    void NpcShoot(ServerPawn& npc, const ServerPawn& target);
    ServerPawn* TracePawns(const Vec3& start, const Vec3& seg, const ServerPawn* ignore, bool skip_npcs,
                           float* best_t);
    static std::string SoldierName(const ServerPawn* p) {
        if (!p) return "Unknown";
        if (p->is_npc) return p->npc_name;
        return p->pri ? p->pri->player_name : "Unknown";
    }
    std::array<std::vector<uint32_t>, 2> respawn_queue_;   // owning client ids, in order
    double next_wave_at_ = -1.0;
    void UpdateReinforcements();
    void SetupCarriedObjective();
    void ResetCarriedObjective();
    void DropObjective(ServerPawn& carrier, float ahead = 0.0f);
    void UpdateObjective();
    std::map<const ServerPawn*, std::vector<std::unique_ptr<ServerRoundActor>>> pawn_inventory_;
    std::vector<std::unique_ptr<ServerRoundActor>> round_actors_;
    std::vector<std::unique_ptr<ServerRoundActor>> map_actors_;
    CollisionWorld collision_;
    std::vector<std::unique_ptr<ServerRoundActor>> join_actors_;
    std::vector<std::unique_ptr<ServerRoundActor>> match_actors_;
    struct DoorTimer { ServerRoundActor* door; double at; const char* state; };
    std::vector<DoorTimer> door_timers_;
    void UpdateDoors();
    std::array<std::vector<RoleSlot>, 2> role_slots_;   // per playable team
    uint32_t round_serial_ = 0;
    void SpawnPlayers();
    void UpdateReloads();
    bool SwapMagazine(ServerPawn* pawn, ServerRoundActor* weapon, ServerRoundActor* old_mag);
    void UpdatePostures(double dt);
    void FireRound(ServerPawn& pawn);
    void SetWeaponROF(ServerPawn& pawn, uint8_t rof);
    void TraceShot(ServerPawn& pawn, ServerRoundActor& weapon);
    // A bullet from `shooter` hit `victim` at `at` (doc sec 33).
    void DamagePawn(ServerPawn& victim, ServerPawn* shooter, const Vec3& at, const Vec3& dir);
    void KillPawn(ServerPawn& victim, ServerPawn* killer);
    void Incapacitate(ServerPawn& victim, uint32_t damage_class, const std::string& instigator);
    void UpdateIncapacitated(double dt);
    static float InjuredSpeedModifier(float health);
    static constexpr float kIncapacitationHealth = 25.0f;   // AA_Pawn.IncapacitationHealth default
    static constexpr float kIncapBloodlossRate = 0.25f;     // AA_Pawn.IncapacitatedBloodlossRate default
public:
    // Seconds before a killed player gets a new soldier (0 = not this round).
    void SetRespawnDelay(double s) { respawn_delay_ = s; }
    void SetIncapBleeding(bool b) { incap_bleeding_ = b; }
    // Round time limit in seconds (retail Bridge classic 420); 0 keeps it.
    void SetTimeLimit(int32_t s) { if (s > 0) time_limit_seconds_ = s; }
private:
    double respawn_delay_ = 0.0;
    bool incap_bleeding_ = true;   // IncapBleeding=false: downed soldiers never bleed out (testing)
    void HitMapActor(uint32_t net_index);
    std::map<uint32_t, int> window_hits_;
    std::map<uint32_t, int> window_index_;   // map NetIndex -> BreakableWindowManager bit
    void SettleStanding(ServerPawn& p);
    ServerRoundActor* FindInventory(const ServerPawn* pawn, const ServerActor* a);
    struct ReloadJob { ServerPawn* pawn; const ServerActor* weapon; double start; int stage; bool reloading = false; };
    struct DeliberateReload { ServerPawn* pawn; double start; int stage; uint8_t rof; };
    std::vector<DeliberateReload> deliberate_reloads_;
    std::vector<ReloadJob> reloads_;
    double clock_ = 0.0;
    struct SpawnPoint { float loc[3]; int32_t yaw; };
    std::array<SpawnPoint, 2> spawn_points_{{{{11385.0f, -3308.0f, 65.0f}, 34816},
                                             {{11385.0f, -3308.0f, 65.0f}, 34816}}};
    std::map<std::string, uint32_t> package_bases_;     // from the manifest
    std::string game_mode_level_;
    std::string pathing_level_;
    std::vector<std::string> streaming_levels_;
    uint32_t next_player_id_ = 1;

    void UpdateMatch(double delta_time);
    void EnterPhase(MatchPhase phase);
    // Round end (doc sec 36): retail's CheckForWinningTeam/IsTeamStillAlive
    // (alive = has a pawn, not incapacitated) on the 1 Hz match timer, the
    // extraction volumes every tick, and the time limit.
    void CheckRoundEnd(bool second_tick);
    bool TeamStillAlive(int32_t team) const;
    int32_t ExtractingTeam(int32_t rule_team) const { return swapped_ ? 1 - rule_team : rule_team; }
    void EndRound(int32_t winner, const char* reason, ServerPawn* completed_by = nullptr,
                  const std::string& objective = "");
    void ResetLevel();
    bool LoadMissionRules(const std::string& path);
    bool LoadMissionRulesFromJson(const std::string& json);
    struct ExtractionRule {
        int32_t team = 0;
        bool carry = false;                         // the carried objective's carrier must be inside
        int32_t min_players = 1;
        std::string objective;
        std::vector<std::array<float, 6>> boxes;   // min xyz, max xyz
    };
    std::vector<ExtractionRule> extraction_rules_;
    std::vector<uint32_t> viewpoints_;              // map NetIndex of AA_Viewpoint actors
    // Fire team spawn markers of the gametype level (AA_MCFireteamSpawnMarker
    // and its AA_PlayerStarts), from the mission rules.
    struct MarkerSpawns {
        uint32_t net = 0;
        int32_t team = 0;
        int32_t fire_team = 0;
        std::vector<SpawnPoint> starts;
    };
    std::vector<MarkerSpawns> spawn_markers_;
    // The team whose mission type is to defend (wins on time), -1 if none.
    int32_t defending_team_ = -1;
    // Where `pc` spawns: its fire team's marker (the other side's on a
    // side-swapped round), member by member over the marker's PlayerStarts.
    const SpawnPoint* FireTeamSpawn(const ServerPlayerController& pc) const;
    std::array<bool, 2> had_valid_players_{};       // AA_TeamInfo.bHadValidPlayersDuringRound
    int32_t round_winner_ = -1;
    int32_t finished_round_ = 0;
    bool next_round_swaps_ = false;
    bool swapped_ = false;                          // AA_MCMissionManager.bSwappingSidesNow
    bool match_over_ = false;                       // the last round of the match ended
    int32_t swap_round_ = 4;                        // iAutoSwitchSidesRound (retail default)
    int32_t role_selection_seconds_ = 10;           // configured iRoleSelectionTime
    int32_t time_limit_seconds_ = 0;                // configured iMissionTimeLimitSeconds
    uint32_t match_actor_serial_ = 0;
    std::vector<std::vector<DataField>> match_actor_start_;  // ES2 actors' round-start fields
    int32_t match_resets_ = 0;                      // AA_Door.IncMatchReset
    size_t PlayersOnTeams() const;
    void RecountReadyVotes();
    MatchPhase phase_ = MatchPhase::kPendingMatch;
    uint32_t phase_serial_ = 0;
    double phase_clock_ = 0.0;     // seconds in the current phase
    double second_clock_ = 0.0;    // drives the 1s countdown ticks

    uint32_t base_aa3baseline_ = 0;
    uint32_t base_aa3game_ = 0;

    std::string map_name_;
    std::string tod_; // Time of Day
    PlayerManager player_manager_;

    // The world's actor instances. The world owns their storage; pointers
    // handed out to other subsystems remain valid for the world's lifetime
    // (Engine outlives ProtocolHandler).
    std::unique_ptr<ServerWorldInfo>           world_info_;
    std::unique_ptr<ServerGameReplicationInfo> gri_;
    std::unique_ptr<ServerGameInfo>            game_info_;
    std::vector<ServerTeamInfo>                teams_;
    std::vector<std::unique_ptr<ServerPlayerController>>      controllers_;
    std::vector<std::unique_ptr<ServerPlayerReplicationInfo>> pris_;
    std::vector<std::unique_ptr<ServerPawn>>                  pawns_;
    std::vector<std::unique_ptr<ServerSteamStatManager>>       stat_managers_;
    std::vector<std::unique_ptr<ServerMOSQualificationBCT>>    mos_quals_;
    std::vector<std::unique_ptr<ServerPlayerStorageProxy>>     proxies_;
};

} // namespace game
} // namespace aa3

#endif // AA3_WORLD_H
