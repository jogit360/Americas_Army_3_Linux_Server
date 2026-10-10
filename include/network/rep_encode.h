#ifndef AA3_NETWORK_REP_ENCODE_H
#define AA3_NETWORK_REP_ENCODE_H

#include <vector>
#include <cstdint>
#include <functional>
#include <map>
#include "util/bits.h"
#include "game/server_actors.h"
#include "game/round_actors.h"
#include "network/rep_layout_gen.h"

// Property-replication wire encoding. Field lists, RepIndex assignments and
// per-type wire formats here are all traced to source
// (UnrealEngine3/Development/Src/Core/Src/UnProp.cpp,UnBits.cpp) and
// cross-verified against the real cooked packages via
// `aa3_build_manifests --rep-layout` — see docs/AA3_ACTOR_REPLICATION.md
// sec 9-11 for the derivation. Nothing here is guessed.
//
// Wire shape per field, matching UActorChannel::ReplicateActor
// (UnChan.cpp:1926-1985): for each property to send, write
// `RepIndex : RIM(ClassMaxIndex)` then the value in the property's own
// NetSerializeItem format. There is no in-band terminator — the bunch's
// declared BunchDataBits is the sole end-of-payload signal (doc sec 10.3),
// so the caller must size the bunch header from the writer's actual bit
// count *after* calling these, not guess it up front.

namespace aa3 {
namespace network {

// MaxIndex = FieldsBase + Fields.Num() for the class actually spawned at
// runtime (World::InitializeActors() uses WorldInfo,
// AA3Baseline.AA_GameReplicationInfo and AA3Baseline.AA_TeamInfo
// specifically — see config/class_maxindex_table.tsv). AA_TeamInfo confirmed
// (not just hypothesized) as the class behind the retail ObjectIndex=70992
// used for all 4 team channels: resolving that index against the corrected
// package-base table (config/live_packagemap_ground_truth.json) lands
// exactly on `Default__AA_TeamInfo` in AA3Baseline.u.
//
// All values come from the generated rep_layout_gen.h (never hand-typed).
// Before doc sec 19's FUNC_Net fix these were 28/133/66/207/91/95/25/23 --
// every class with net functions was undercounted, which changed each
// RepIndex's RIM bit-width on the wire.
constexpr uint32_t kWorldInfoMaxIndex = rep::WorldInfo::kMaxIndex;
constexpr uint32_t kGRIMaxIndex = rep::GRI::kMaxIndex;
constexpr uint32_t kTeamInfoMaxIndex = rep::TeamInfo::kMaxIndex;
constexpr uint32_t kPRIMaxIndex = rep::PRI::kMaxIndex;
// The PlayerController class is AGP_PlayerController (AA3Game.u), as in
// retail: same RepIndex values as AA_PlayerController for 0..472, but the
// index is written against its own MaxIndex (doc sec 26).
constexpr uint32_t kPCMaxIndex = rep::AGP_PC::kMaxIndex;
constexpr uint32_t kPawnMaxIndex = rep::Pawn::kMaxIndex;
constexpr uint32_t kSteamStatManagerMaxIndex = rep::SteamStatManager::kMaxIndex;
constexpr uint32_t kMOSQualificationBCTMaxIndex = rep::MOSQualificationBCT::kMaxIndex;
constexpr uint32_t kPlayerStorageProxyMaxIndex = rep::PlayerStorageProxy::kMaxIndex;

// Per-connection replication context (doc sec 21). Two things vary by
// connection and so can't live in the actor model:
//   - References to spawned actors are written as bDynamic=1 + the CHANNEL
//     INDEX the referenced actor has on THIS connection
//     (UPackageMapLevel::SerializeObject, UnNetDrv.cpp; retail: PC.Pawn =
//     dynamic ch 159/161/163). `channel_of` resolves that; 0 = not open
//     (yet) / NULL. `sent_refs` (RepIndex -> last wire value) makes a
//     reference sent as NULL re-send once its target opens -- retail's
//     Mapped=0 retry, without per-actor snapshot fields.
//   - Owner vs non-owner: Actor.uc/Pawn.uc replication conditions
//     (bNetOwner, RemoteRole==ROLE_SimulatedProxy/AutonomousProxy).
struct RepContext {
    bool for_owner = true;
    std::function<uint32_t(const game::ServerActor*)> channel_of;
    std::map<uint32_t, uint32_t>* sent_refs = nullptr;
    // Pawn only: split movement (Location/Rotation/Velocity/RemoteViewPitch,
    // sent unreliably at a higher rate to non-owners) from everything else.
    enum class Fields { kAll, kMovementOnly, kNoMovement };
    Fields fields = Fields::kAll;
    // Pawn movement pass only: send movement fields even if unchanged (a
    // periodic keyframe, since unreliable updates can be lost).
    bool force_movement = false;
    // Static object references (classes, map objects) by package + NetIndex
    // -> wire ObjectIndex (World::ObjectIndex; doc sec 26). 0 = None.
    std::function<uint32_t(const std::string&, uint32_t)> object_index;
};

// One generic replicated value (game::DataValue; doc sec 28).
void WriteDataValue(util::BitWriter& w, const game::DataValue& v, const RepContext& ctx);

// A round actor's fields. `sent` maps (RepIndex, element) to the bits last
// sent on this connection; only fields whose encoding differs are written
// (all of them when `sent` is empty), so actor references that weren't
// mapped yet go out again once their channel opens. Static-array elements
// carry their BYTE element index after the RepIndex (UnChan.cpp:1512).
// Generic field list (e.g. ServerTeamInfo::round_fields), same diff rule.
uint32_t EncodeDataFields(util::BitWriter& w, const std::vector<game::DataField>& fields,
                          uint32_t max_index,
                          std::map<std::pair<uint32_t, int32_t>, std::string>& sent,
                          const RepContext& ctx);
uint32_t EncodeRoundActor(util::BitWriter& w, const game::ServerRoundActor& a,
                          std::map<std::pair<uint32_t, int32_t>, std::string>& sent,
                          const RepContext& ctx);

// ObjectProperty value for an actor reference: bDynamic=1 + RIM(channel,
// MAX_CHANNELS=0x800). channel 0 = NULL, exactly UE3's NULL encoding.
void WriteActorRef(util::BitWriter& w, uint32_t channel);

// Appends RepIndex+value for every field whose value differs from `*prev`.
// `prev == nullptr` means "send everything this encoder knows how to send"
// (used for the initial full-state replication on channel open — matches
// retail's bNetInitial behavior, where every conditioned property fires
// once on spawn). Returns the number of fields written; 0 means nothing to
// send (the caller should skip the update bunch entirely, matching
// UnChan.cpp:1985's `if (Bunch.GetNumBits()) SendBunch(...)`).
//
// Only fields with a real, `--rep-layout`-confirmed RepIndex and a fully
// resolved wire type are encoded. Known gaps, left unsent rather than
// guessed:
//   - WorldInfo.ReplicatedMusicTrack (RepIndex 23): real type is a custom
//     `MusicTrackStruct`, not the plain string server_actors.h models it
//     as. Needs `--struct-layout MusicTrackStruct` before this can be
//     encoded correctly.
//   - WorldInfo.Pauser / GameReplicationInfo.Winner: real type is
//     ObjectProperty (an actor reference), not the plain
//     uint8_t/int32_t server_actors.h stores. Sent as a NULL object
//     reference on the initial (prev==nullptr) call only — correct for the
//     common "nobody paused / no winner yet" case, but a real reference is
//     never encoded (there is no object-binding path here yet).
//   - GameReplicationInfo.GameClass (RepIndex 42, ClassProperty),
//     .MessageOfTheDay, .ServerRegion, .bTrackStats, .bIsArbitrated: not
//     modeled in server_actors.h at all, so simply not sent (a valid
//     UE3 state — client keeps class defaults for anything never sent).
//   - server_actors.h's `second_count`, `b_show_hud_timer`,
//     `b_role_swapping_disabled` do not correspond to any real property in
//     the 133-field AA_GameReplicationInfo layout (checked against the
//     full `--rep-layout` dump) — never sent, left as a note for cleanup.
uint32_t EncodeWorldInfoRep(util::BitWriter& w, const game::ServerWorldInfo& cur,
                            const game::ServerWorldInfo* prev);
uint32_t EncodeGRIRep(util::BitWriter& w, const game::ServerGameReplicationInfo& cur,
                      const game::ServerGameReplicationInfo* prev,
                      std::vector<uint32_t>* field_ends = nullptr);

// AA_TeamInfo's own fields are just the base Engine.TeamInfo trio
// (TeamIndex/Score/TeamName, RepIndex 23-25) — server_actors.h doesn't model
// any of AA_TeamInfo's own RepIndex 26-65 additions (round-scoring,
// fireteam state, etc.), so only those 3 are ever sent. Two gaps, found the
// same way as EncodeGRIRep's:
//   - TeamInfo.Score (RepIndex 24) is a FloatProperty on the wire, but
//     `ServerTeamInfo::score` is modeled as int32_t — sent via a plain
//     numeric cast (exact for the whole-number scores this server produces
//     today).
//   - `ServerTeamInfo::kills` / `::deaths` do not correspond to any real
//     property on AA_TeamInfo (checked against the full `--rep-layout`
//     dump) — never sent.
uint32_t EncodeTeamInfoRep(util::BitWriter& w, const game::ServerTeamInfo& cur,
                          const game::ServerTeamInfo* prev);

// AA_PlayerReplicationInfo (MaxIndex=207: 23 Actor base + 27 base
// PlayerReplicationInfo (26 prop + 1 RPC) + 157 AA3 additions). Only the
// subset `ServerPlayerReplicationInfo` models: bBot(30), bOutOfLives(31),
// bOnlySpectator(34), bIsSpectator(35), bAdmin(37), TeamID(39),
// PlayerName(43), Ping(46, plain byte), Deaths(47)/Score(48) (both
// FloatProperty on the wire — `deaths`/`score` are int32_t here, sent via a
// numeric cast). Gap: `unique_id_hex` (meant for RepIndex 26 UniqueId, a
// `UniqueNetId` struct — raw QWORD per doc sec 11.1) is never populated
// anywhere in World, so left unsent rather than encoding 8 zero bytes for
// nothing; revisit once something actually sets it.
// `ctx` resolves PRI.Team to a channel; without one Team isn't sent.
uint32_t EncodePRIRep(util::BitWriter& w, const game::ServerPlayerReplicationInfo& cur,
                      const game::ServerPlayerReplicationInfo* prev,
                      const RepContext* ctx = nullptr);

// AGP_PlayerController (kPCMaxIndex = rep::AGP_PC::kMaxIndex = 491; RepIndex
// values are rep::PC's, which AGP_PC shares). Initial send = retail's PC
// open properties: Role (=AutonomousProxy), bNetOwner, bUseFlashSAI; then
// the actor references PlayerReplicationInfo, Pawn, aassmSteamStatManager
// and PlayerProxy, re-sent once their channels exist (doc sec 21, 27).
uint32_t EncodePCRep(util::BitWriter& w, const game::ServerPlayerController& cur,
                     const game::ServerPlayerController* prev, const RepContext& ctx);

// AA_PlayerStorageProxy (doc sec 27): Owner/RepController -> the PC's
// channel, RepPRI -> the PRI's, plus bNetOwner/bIsDead/bActive. Refs follow
// the same NULL-then-retry rule as EncodePCRep's.
uint32_t EncodePlayerStorageProxyRep(util::BitWriter& w,
                                     const game::ServerPlayerStorageProxy& cur,
                                     const game::ServerPlayerStorageProxy* prev,
                                     const RepContext& ctx);

// AA_Pawn (MaxIndex=131 per rep_layout_gen.h; the breakdown below predates doc sec 19's
// FUNC_Net fix, which added 6 missed AA_Pawn RPCs and shifted AA_Pawn fields +6).
// `ServerPawn` models a deliberately small slice: Velocity (RepIndex 4 —
// Actor-base, zero today since nothing moves a Pawn yet, but correct and
// free to include since ServerActor already carries it), Health (30),
// PlayerReplicationInfo (29) and Controller (44) (both real object
// references, same as EncodePCRep's), the 6 movement-tuning floats +
// bSimulateGravity (31-36, 40 — real Engine.Pawn defaultproperties values,
// see server_actors.h), 3 plain-byte fields FiringMode/FlashCount/
// RemoteViewPitch (37-39, none are Enum-typed — confirmed via the same
// empty-Enum-column check `--rep-layout` already used for Ping/PacketLoss),
// 3 stance bools bIsProne/bIsCrouched/bIsWalking (41-43), and body identity
// sBodySoldierName/iBodyTeamIndex (47-48). Location/Rotation (RepIndex
// 21/22) are deliberately NOT in this loop, even on the full
// (`prev==nullptr`) send: they're already covered by
// `SerializeCompressedInitial` on the channel-open bunch (see
// `WriteSerializeCompressedInitial`), and sending them again via the normal
// property path would be redundant with what the preamble already
// transmitted. They'll need to join this loop once something actually
// moves a Pawn after spawn (per-tick diffs need a way to update position,
// and only the property loop — not the open-only preamble — can do that).
// Also carries 7 more AA3-specific fields (RepIndex 58-70 subset post-sec-19):
// fDesiredLeanDirection (52), LastKnownPRI (53, real reference), Stamina
// (59), Asphyx (60), AimOffset (62), IncapacitationLoc (63 — ByteProperty
// Enum `eHitLocation`, 19 names → 5 bits; resolved via the same
// offset-20-in-the-UField-body technique doc sec 11.2 used, cross-checked
// against `ENetRole`/`ECollisionType`'s already-known widths before
// trusting it on a new enum), AdvMvmt_VaultMantleHeight (64). All default
// to a script-level 0, same status as the movement-tuning floats — see
// server_actors.h. `MovementSpeedModifier` (51) and `HealthTarget` (61) are
// deliberately excluded: both read as multiplier/lerp-target fields where
// the script-default 0 would be actively misleading (a 0 speed multiplier
// implies "can't move") rather than merely unset, and there's no evidence
// either way without native-code reach this project doesn't have yet.
//
// Also carries rep::Pawn::PawnShotEffectInfo (56; 50 pre-sec-19) — a custom StructProperty
// (`AA_PawnHitEffectInfo`), resolved via `--struct-layout` to 2 fields
// (HitVector, HitRotation). Per doc sec 11.1's "else" branch (the generic
// recursive struct path), both are serialized unconditionally whenever the
// outer RepIndex fires — no per-field RepIndex, no CPF_Net condition, just
// `TFieldIterator` declaration order — so this is the first custom
// (non-native) struct this file encodes; every earlier StructProperty
// (Vector/Rotator) used UE3's hardcoded native-type fast path instead.
// Zero vector/rotation is the honest "no hit has occurred" state; this
// server has no combat/damage system to generate a real one.
//
// Also sends 3 more ObjectProperty/ClassProperty fields as NULL, initial-
// send-only, same treatment as Winner/Pauser: DrivenVehicle (25),
// CurrentTreatmentPatient (55), IncapacitationType (58). These are a
// different category of NULL stub from Winner/Pauser though — those are
// NULL only because server_actors.h can't represent a *real* value yet.
// These three are NULL because that's definitionally correct: this server
// has no vehicle, medical-treatment, or incapacitation system at all, so
// "not driving/treating/incapacitated" isn't a guess about what real AA3
// does, it's simply true of this server's actual state. No backing field
// needed in ServerPawn for any of them — there's nothing to track.
//
// Investigated but deliberately NOT added, each for a different reason:
//   - Pawn.InvManager (24) / PlayerProxy (49): unlike the three above,
//     these are plausibly ALWAYS non-null on a real spawned pawn (an
//     inventory manager is normally auto-spawned with its owner) — NULL
//     here would misrepresent real behavior, not just omit unknown state,
//     so it's left unsent rather than asserted.
//   - AA_Pawn.aaController (70): almost certainly a same-object typed-cast
//     alias of Pawn.Controller (RepIndex 44) — extremely common UE3 idiom
//     (base class holds a generic reference, subclass caches a strongly-
//     typed one via PossessedBy) — but the one place that would confirm
//     it, `AA_Pawn::PossessedBy`, is one of the functions AA3's custom
//     bytecode breaks UELib's decompiler on ("Statement decompilation
//     error: Index was out of range"), so this can't actually be verified
//     from source. Left out rather than encoded on an educated guess —
//     the one gap in this file that's a plausible pattern match, not a
//     structural certainty like the three NULLs above.
//   - Injury state (all 3 are TArray) and CurrentTreatmentTreater[] (also
//     TArray): `UArrayProperty::NetSerializeItem` is a hard no-op on both
//     the send and receive side (doc sec 11.1), so even a "correct" RepIndex
//     write for these would carry zero payload bits and produce zero
//     client-side effect — functionally indistinguishable from never
//     sending them. Not worth the array-dirty-tracking machinery for a
//     wire event with no observable effect either way.
//
// Owner/non-owner split (doc sec 21, Actor.uc/Pawn.uc replication blocks):
// initial send carries Role/RemoteRole (wire-swapped, UnChan.cpp:1942) and
// bNetOwner; owner gets Controller/Health/movement tuning; non-owners get
// Location/Rotation/Velocity/Physics (RemoteRole==SimulatedProxy) and
// bIsCrouched/FlashCount/FiringMode/RemoteViewPitch.
uint32_t EncodePawnRep(util::BitWriter& w, const game::ServerPawn& cur,
                       const game::ServerPawn* prev, const RepContext& ctx);

// AA_SteamStatManager (MaxIndex=28): two StructProperty fields over
// `AA_MP_PlayerMatchStatistics` (12 fields) / `AA_MP_PlayerStatistics`
// (116), flat Bool/Float/Int/Byte, every field serialized in declaration
// order with no inner RepIndex (sec 11.1). Encoded from the real model on
// ServerSteamStatManager via the generated field table
// (game/player_stats_gen.h); a struct is re-sent whenever any field in it
// changes (doc sec 23).
uint32_t EncodeSteamStatManagerRep(util::BitWriter& w,
                                   const game::ServerSteamStatManager& cur,
                                   const game::ServerSteamStatManager* prev);

// FVector::SerializeCompressed / FRotator::SerializeCompressed
// (UnMath.cpp:51-129, send side) — the wire format for a StructProperty
// Vector/Rotator, and also the two pieces of SerializeCompressedInitial
// below. `WriteCompressedVector`'s `Bits` selection matches source exactly:
// clamp(CeilLogTwo(1+max(|round(x)|,|round(y)|,|round(z)|)), 1, 20) - 1.
void WriteCompressedVector(util::BitWriter& w, float x, float y, float z);
void WriteCompressedRotator(util::BitWriter& w, int32_t pitch, int32_t yaw, int32_t roll);

// Channel-open preamble for a TRANSIENT (server-spawned, not placed) actor —
// UnChan.cpp:1788-1790's `SerializeCompressedInitial`: Location
// unconditionally, then Rotation only if `b_net_initial_rotation`. The
// ObjectIndex/Archetype reference that precedes this (and NetPlayerIndex,
// PlayerController-only, that follows it) are written by the caller, not
// here — this is only the Location/Rotation piece, plus the 7 bits AA3
// appends to it (see the definition; doc sec 26).
//
// `b_net_initial_rotation` defaults to false: `game::ServerActor` doesn't
// model UE3's `bNetInitialRotation` flag, and none of the transient actors
// this server spawns today (PRI) have meaningful rotation, so omitting it
// is the correct behavior for every current caller, not a placeholder.
void WriteSerializeCompressedInitial(util::BitWriter& w, const game::ServerActor& actor,
                                     bool b_net_initial_rotation = false);

} // namespace network
} // namespace aa3

#endif // AA3_NETWORK_REP_ENCODE_H
