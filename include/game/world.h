#ifndef AA3_WORLD_H
#define AA3_WORLD_H

#include <array>
#include <deque>
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
    void MaybeLateJoin(ServerPlayerController& pc);
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
    // AA_Pawn.TryVaultOrMantle(True) on the server (ServerTryVault, and
    // FinishVaultMantleMoveToTarget after the move): the obstacle ahead,
    // measured from the map, picks Vault / Mantle / Climb (crouched
    // variants without headroom; out of a sprint the combat step or the
    // hood slide). False if there's nothing to get over.
    bool TryVaultOrMantle(ServerPawn* pawn);
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
    // ServerRepExec('Pickup', dropped rifle): the rifle with its magazine
    // into the player's inventory, on the right shoulder (doc sec 46).
    bool PickUpWeapon(ServerPlayerController& pc, const ServerActor* obj);
    // ServerSetCurrentWeapon(other rifle) with a rifle in hand: holster one,
    // draw the other. False if it isn't a switch between two rifles.
    bool SwitchRifle(ServerPawn* pawn, const ServerActor* to);

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
        bool npcs_told = false;                  // NotifyGrenadeBounced sent to nearby NPCs
        double next_burn = 0.0;
        // A 40 mm round: goes off where it hits once armed (ArmingTime
        // after launch); before that it bounces off as a dud.
        bool impact = false, dud = false;
        double arm_at = 0.0;
        Vec3 launch;                             // where it left the muzzle
    };
    const std::vector<ThrownGrenade>& ThrownGrenades() const { return thrown_; }

    // Sounds the server plays (doc sec 42). Retail's server plays the
    // enemies' gunfire, bullet impacts, fly-bys and radio chatter with
    // Actor.PlaySound, which reaches each player as
    // PlayerController.ClientHearSound (828 in the Bridge AI user session,
    // 2026-10-12), and near misses as AA_PlayerController.
    // ClientSuppressedByFire (the bullet crack). The protocol sends each
    // event to every player in `radius` of it, occlusion traced from them.
    struct SoundEvent {
        uint64_t seq = 0;
        double at = 0.0;
        const char* pkg = "";                    // the SoundCue: package + NetIndex in it
        uint32_t net = 0;
        const ServerActor* source = nullptr;     // SourceActor (None if not open to that player)
        Vec3 loc;                                // SourceLocation
        bool stop_when_destroyed = false, delay = false;
        float priority = 0.0f;                   // PriorityAdjustment
        float radius = 0.0f;
        const ServerPawn* listener = nullptr;    // a crack for this player only (`source` the shooter)
    };
    const std::deque<SoundEvent>& SoundEvents() const { return sound_events_; }
    uint64_t SoundSeq() const { return sound_seq_; }
    // Is the line between two points blocked by the map?
    bool Occluded(const Vec3& from, const Vec3& to) const;
    // The AI co-op mission briefing for a team's players at the round's
    // first spawn (S_AIMissionAudio <Map>.<Team>COMissionBriefingCue; retail
    // ClientPlaySound at t=21.16), 0 if the map has none.
    uint32_t BriefingSoundIndex(int32_t team) const;

    // XM320 grenade launcher under the rifle (doc sec 41; no retail
    // grenadier capture -- built from the scripts, the soldier state graph
    // in Sold_Tech.upk and the animation lengths in Mod_Grenade.upk).
    // AA_Weapon.ServerSetWeaponFireMode(mode): 1 raises the launcher, 0
    // goes back to the rifle. False if refused (no launcher, reloading).
    bool SetWeaponFireMode(ServerPawn* pawn, uint8_t mode);
    // Weapon.ServerStartFire(1) with the launcher up: a 40 mm round leaves
    // along the view, raised for the sight's range setting.
    void FireLauncher(ServerPawn& pawn);
    // ServerTryToReload with the launcher up (AA_Weapon.Reload: RT_WeaponMod).
    bool BeginLauncherReload(ServerPawn* pawn);
    // AA_Weapon.ServerWeaponModLoadAmmo("M433"/"M714"): switch round type
    // and reload with it.
    void LauncherLoadAmmo(ServerPawn* pawn, const std::string& type);
    // ServerCycleWeaponZeroingRange(bCycleUp) / ServerResetWeaponModZeroing.
    void CycleLauncherRange(ServerPawn* pawn, bool up, bool reset = false);

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
    void UpdateLauncher();
    void UpdateRifleSwitch(ServerPawn& p);
    void RifleInHand(ServerPawn& p, ServerRoundActor& to);
    void UpdateEncumbrance(ServerPawn& p);
    // Round statistics (doc sec 45): the soldier's weapon for a stat, and
    // the counts as things happen.
    enum StatWeapon : uint8_t { kStatNone, kStatM4A1, kStatM16A4, kStatDMR, kStatM249, kStatM320, kStatM67,
                                kStatM84, kStatM14, kStatKNP, kStatMSP, kStatLMK };
    PlayerRoundStats* LiveStats(const ServerPawn* p);
    uint8_t RifleStat(const ServerPawn& p);
    void StatShot(ServerPawn& p);
    void StatHit(ServerPawn& shooter, ServerPawn& victim, int zone, uint8_t weapon);
    void StatNeutralized(ServerPlayerController& pc, ServerPawn& victim);
    void PublishRoundStats(int32_t winner);
    void ResetRoundStats();
    void SetUpLauncher(ServerPlayerController& pc, std::vector<std::unique_ptr<ServerRoundActor>>& items,
                       ServerRoundActor& weapon, ServerRoundActor& launcher);
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
    std::deque<SoundEvent> sound_events_;
    uint64_t sound_seq_ = 0;
    void PlaySound(const char* pkg, uint32_t net, const ServerActor* source, const Vec3& loc, bool stop_when_destroyed,
                   bool delay, float priority, float radius);
    // An enemy round's sounds: its report, then a crack for anyone it
    // passed close by, the impact on whoever it hit or the fly-by where
    // it ended up.
    void BulletSounds(ServerPawn& shooter, const Vec3& start, const Vec3& end, const ServerPawn* victim, bool last_round,
                      int weapon);
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
    // AA_NPCEnemyPlayerStart: its own firing positions (indices into
    // npc_firing_positions_) and, with bManFiringPositionsOnly ("only"),
    // the states retail keeps it to (doc sec 40.8); iMaxSightDistance;
    // AA_NPCEnemySoldierFrightenedThinking (bravery 60, else 80).
    struct NpcStart {
        float loc[3] = {0, 0, 0}; int32_t yaw = 0; std::string weapon;
        std::vector<int> fp;
        bool only = false, random_spawn = false, frightened = false;
        int32_t sight = 19200;
    };
    // AA_NPCFiringLocationPathNode: where, facing, bCanShoot{Standing,
    // Crouched,Prone} / bCanThrowGrenades (flags 1/2/4/8), and its 16 cover
    // directions (every 4096 of yaw: 1 cover, 2 must crouch, 4 can stand).
    struct FiringPos {
        float loc[3] = {0, 0, 0}; int32_t yaw = 0; int flags = 1;
        std::array<uint8_t, 16> cover{};
    };
    std::map<const ServerPawn*, std::string> npc_weapon_;       // each NPC's rifle class
    std::vector<NpcStart> npc_starts_;
    std::vector<FiringPos> npc_firing_positions_;
    float npc_fog_ = 0.0f;                      // WorldInfo.fpAIFogDistance (0: none)
    int32_t npc_max_ = 0;
    bool npcs_enabled_ = true;
    bool movement_checks_ = true;
    // One NPC's controller + thinking class (AA_NPCBaseSoldierController /
    // AA_NPCBaseSoldierBaseThinking; doc sec 40.8).
    enum class NpcState : uint8_t { kScan, kTargeting, kChangeFP, kHiding, kReloading, kJam, kThrowing,
                                    kFlashbanged, kFleeGrenade };
    struct NpcBrain {
        NpcState state = NpcState::kScan;
        double wait_until = 0.0;                // the state's current Sleep()
        int step = 0;                           // where in the state's script
        int start = -1;                         // npc_starts_ index
        int cur_fp = -1, dest_fp = -1;          // firing positions
        float floor_z = 0.0f;
        float sight = 2500.0f;                  // iMaxSightDistance (fog applied)
        // Thinking.
        int bravery = 80;
        bool danger_near = false;
        double danger_end_at = -1.0, think_at = 0.0;
        bool injured_by[2] = {false, false};
        // Targeting.
        ServerPawn* target = nullptr;
        Vec3 last_known;
        double lost_sight_at = 0.0, entered_targeting_at = 0.0, monitor_at = 0.0, spotted_at = 0.0;
        int burst_left = 0;
        double next_shot_at = 0.0, burst_end_at = 0.0;
        int rounds = 30, mag_size = 30;
        bool lmk = false, marksman = false;
        double last_supp_reaction = -100.0, supp_start = -100.0;
        double last_voice = -100.0;             // PlayNPCSoundCue's 5 s anti-spam
        int buzzes_here = 0;
        double last_frag = -100.0, last_smoke = -100.0;
        int frags = 3, smokes = 3;
        bool throw_smoke = false;
        Vec3 throw_at;
        NpcState after_hiding = NpcState::kScan;
        // Scanning and moving.
        double shift_at = 0.0, next_scan_at = 0.0;
        int scan_i = 0;
        int32_t scan_base = 0, want_yaw = 0;
        bool turning = false;
        double order_at = -100.0;               // combat manager's last order
        double flash_until = 0.0;
        float last_enemy_dist = 1e9f;
        double next_watch_at = 0.0;
        Vec3 home;                              // its start
        Vec3 grenade_at;                        // the frag it flees
        std::vector<Vec3> path;                 // floor points to walk
        size_t path_i = 0;
        float speed = 200.0f;                   // retail NPCs' top speed
        float cur_speed = 0.0f;                 // ramping up to it
    };
    // Smoke (M83 / M106 / U72) clouds: NPCs can't see through them
    // (AA_NPCCombatManager.DoSmokeGrenadesBlockOurVision).
    struct SecureEvent {
        uint64_t id = 0;
        const ServerPlayerController* by = nullptr;
        int32_t team = -1;
        bool alive = false;                     // secured (zip-cuffed) vs. a body confirmed
        std::string npc, message;
        int secured = 0, confirmed = 0;         // the player's counts after it
    };
    struct SmokeCloud { Vec3 at; double until; };
    std::vector<SmokeCloud> smoke_clouds_;
    std::vector<SecureEvent> secure_events_;
    uint64_t next_secure_id_ = 1;
    void CreditNpc(ServerPawn& victim, ServerPawn* killer);
    void StripNpc(ServerPawn& npc);
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
    void NpcThrowFrag(ServerPawn& npc, const Vec3& target, bool smoke = false);
    void UpdateWorldItems();
    void SpawnNPCs();
    void GiveNpcGear(ServerPawn& npc, const std::string& weapon);
    void UpdateNPCs(double dt);
    void NpcTick(ServerPawn& npc, NpcBrain& b, double dt);
    void NpcThink(ServerPawn& npc, NpcBrain& b);
    void NpcPosture(ServerPawn& npc, NpcBrain& b, int posture);     // 0 stand, 1 crouch, 2 prone
    void NpcEnterScan(ServerPawn& npc, NpcBrain& b);
    bool NpcEnterChangeFP(ServerPawn& npc, NpcBrain& b);
    void NpcTargetEnemy(ServerPawn& npc, NpcBrain& b, ServerPawn* enemy, bool contact = true);
    void NpcEnemyContact(ServerPawn& npc, ServerPawn& enemy);
    void NpcSuppressed(ServerPawn& npc, NpcBrain& b, ServerPawn* shooter);
    bool NpcWantsGrenade(const ServerPawn& npc, NpcBrain& b, bool smoke, bool just_lost_sight);
    void NpcBeginThrow(ServerPawn& npc, NpcBrain& b, bool smoke, const Vec3& at);
    float NpcThreat(const ServerPawn& npc, const NpcBrain& b, const ServerPawn& p) const;
    bool NpcCanSeePlayer(const ServerPawn& npc, const NpcBrain& b, const ServerPawn& target, bool fov) const;
    bool NpcSmokeBlocks(const Vec3& a, const Vec3& b) const;
    bool NpcCanSee(const ServerPawn& npc, const ServerPawn& target) const;
    void NpcShoot(ServerPawn& npc, NpcBrain& b, const Vec3& at_point);
    // AA_NPCBaseController.PlayNPCSoundCue: one of `cues` (S_Char_VO_Soldier
    // NetIndex) from the NPC, at most once in 5 s.
    void NpcVoice(ServerPawn& npc, NpcBrain& b, std::initializer_list<uint32_t> cues);
    void NpcVoice(ServerPawn& npc, std::initializer_list<uint32_t> cues);
    const FiringPos* NpcFP(const NpcBrain& b) const {
        return b.cur_fp >= 0 && static_cast<size_t>(b.cur_fp) < npc_firing_positions_.size()
                   ? &npc_firing_positions_[static_cast<size_t>(b.cur_fp)] : nullptr;
    }
public:
    // Hooks from the players' weapons: a shot's path (suppression and
    // hearing), a grenade landing near, smoke and a flashbang going off.
    void NpcBulletNear(ServerPawn& shooter, const Vec3& start, const Vec3& end, const ServerPawn* hit);
    // AGP_PlayerController.SendEnemyDownRequest -> ServerTryToIdentifyCorpse:
    // secure a downed enemy or confirm a dead one near the player.
    bool SecureEnemy(ServerPlayerController& pc, const std::string& name);
    const std::vector<SecureEvent>& SecureEvents() const { return secure_events_; }
    void NpcGrenadeNear(const Vec3& at, bool frag, const ServerPawn* thrower);
    void NpcSmoke(const Vec3& at, double seconds);
    void NpcFlashbang(const Vec3& at);
private:
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
    struct DeliberateReload { ServerPawn* pawn; double start; int stage; uint8_t rof; bool empty = false; int chosen = -1; };
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
