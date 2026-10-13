#ifndef AA3_NETWORK_RPC_ENCODE_H
#define AA3_NETWORK_RPC_ENCODE_H

#include <string>
#include "util/bits.h"

// RPC (remote function call) wire encoding — UnScript.cpp:2842-3036
// (`InternalProcessRemoteFunction`), the send side of the "FieldCache is a
// UFunction" branch the packet-grammar comment atop protocol_handler.cpp
// left as "…" (never resolved until now). Distinct from property
// replication (network/rep_encode.h) but shares its RepIndex mechanism —
// `FieldNetIndex : RIM(ClassMaxIndex)`, the *same* per-class table
// properties use, since functions and properties share one FieldNetIndex
// sequence (see any `--rep-layout` dump's interleaved Function/Property
// rows) — then parameters follow in declaration order.
//
// Per-parameter rule, read directly from source (UnScript.cpp:2980-3010):
// `BoolProperty` has no "Send" flag — always sent, 1 raw bit. Every other
// parameter type gets a 1-bit "Send" flag FIRST: 1 = this parameter's value
// differs from its type's zero/default and the value follows in
// NetSerializeItem format; 0 = skip entirely, receiver uses the default.
// This is a genuinely different rule from a struct's inner fields
// (rep_encode.h's `PawnShotEffectInfo`, doc sec 12.6), which have no Send
// flag at all and always serialize unconditionally — easy to conflate the
// two since both are "fields inside something triggered by one outer
// RepIndex," so this distinction is worth keeping explicit.
//
// The resulting bunch is shaped exactly like a property-update bunch
// (bOpen=false, ChType=2 Actor, RepIndex-prefixed payload) — so
// `ProtocolHandler::SendActorUpdateBunch`, already built for property
// diffs, sends these completely unchanged; only the payload differs.

namespace aa3 {
namespace network {

// Write one non-bool RPC parameter: Send-flag + value-if-sent. "Zero" per
// type: empty string for StrProperty, 0.0 for FloatProperty, index 0
// (NULL) for ObjectProperty, all-zero pitch/yaw/roll for the Rotator
// struct, value 0 for a ByteProperty/Enum — matches `UProperty::Matches`
// against a zero-initialized buffer (source above).
void WriteRPCStrParam(util::BitWriter& w, const std::string& value);
void WriteRPCFloatParam(util::BitWriter& w, float value);
// Object params take the referenced actor's CHANNEL on this connection
// (0 = NULL) — spawned actors are referenced by channel, doc sec 21.
void WriteRPCObjectParam(util::BitWriter& w, uint32_t actor_channel);
void WriteRPCRotatorParam(util::BitWriter& w, int32_t pitch, int32_t yaw, int32_t roll);
// `num_bits` = CeilLog2(NumEnums-1) for the parameter's specific Enum,
// resolved the same way as property ByteProperty/Enum fields (doc sec
// 11.2/12.5) — read `NumEnums` from the Enum's own export in the .u file,
// don't hand-count script source (verified to disagree at least once: see
// BuildClientSetProgressMessageRPC below).
void WriteRPCByteEnumParam(util::BitWriter& w, uint8_t value, uint32_t num_bits);

// BoolProperty RPC parameter: no Send-flag at all (source's `UBOOL Send=1`
// stays 1 for bools, unconditionally) — always exactly 1 raw bit.
void WriteRPCBoolParam(util::BitWriter& w, bool value);

// Engine.PlayerController.ClientMessage(coerce string S, optional Name
// Type, optional float MsgLifeTime) — rep::PC::ClientMessage in AA_PlayerController's
// class table (rep::PC::kMaxIndex, same table network::EncodePCRep uses; see
// docs/AA3_ACTOR_REPLICATION.md sec 15). Displays a system/chat message on
// the client's HUD. `Type`/`MsgLifeTime` are left at their UE3 defaults
// (Send=0 for both — Name "None", float 0.0) — this server has no
// message-type categorization or custom message durations yet.
void BuildClientMessageRPC(util::BitWriter& w, const std::string& message);
// ...with a message Type (retail's co-op notices: 'Notification').
void BuildClientMessageRPC(util::BitWriter& w, const std::string& message, const std::string& type);

// Engine.Controller.ClientSetRotation(rotator NewRotation, optional bool
// bResetCamera) — rep::PC::ClientSetRotation. Orients the client's pawn/camera.
void BuildClientSetRotationRPC(util::BitWriter& w, int32_t pitch, int32_t yaw,
                               int32_t roll, bool reset_camera);

// Engine.Controller.ClientSwitchToBestWeapon(optional bool
// bForceNewWeapon) — rep::PC::ClientSwitchToBestWeapon. Tells the client to run its own
// best-weapon-selection logic; needs no server-side inventory data since
// the client does the actual selection.
void BuildClientSwitchToBestWeaponRPC(util::BitWriter& w, bool force_new_weapon);

// Engine.PlayerController.TeamMessage(PlayerReplicationInfo PRI, coerce
// string S, name Type, optional float MsgLifeTime) — rep::PC::TeamMessage. `Type`/
// `MsgLifeTime` left at defaults (Send=0), same reasoning as
// BuildClientMessageRPC. `pri_channel` is the speaking player's PRI channel
// on this connection.
void BuildTeamMessageRPC(util::BitWriter& w, uint32_t pri_channel,
                         const std::string& message);

// Engine.PlayerController.ClientSetProgressMessage(EProgressMessageType
// MessageType, string Message, optional string Title, optional bool
// bIgnoreFutureNetworkMessages) — rep::PC::ClientSetProgressMessage. `MessageType`'s Enum
// (`EProgressMessageType`) has NumEnums=7 read directly from Engine.u —
// notably NOT 8, which is what counting the named values in
// UnrealEngine3/.../PlayerController.uc's script source gives; the cooked
// package is the actual ground truth for what's on the wire, and it
// disagrees with the script text this one time, confirming why this file
// always reads Enums from the .u data rather than counting source.
// `bits = CeilLog2(7-1) = 3`. `Title` left unset (Send=0).
void BuildClientSetProgressMessageRPC(util::BitWriter& w, uint8_t message_type,
                                      const std::string& message,
                                      bool ignore_future_network_messages);

// Engine.PlayerController.ClientSetViewTarget(Actor A, optional
// ViewTargetTransitionParams TransitionParams) — rep::PC::ClientSetViewTarget. Points the
// client's camera at the actor on `actor_channel` (typically the player's
// own Pawn). `TransitionParams` is always Send=0: it's a
// struct parameter, so it gets the same "differs from zero" Send-flag test
// as any other non-bool parameter (source above) — passing a default-
// constructed (all-zero) one means Send=0 unconditionally, so its own
// field layout never needs resolving at all.
void BuildClientSetViewTargetRPC(util::BitWriter& w, uint32_t actor_channel);

// PlayerController.ClientAckGoodMove(float TimeStamp) — unreliable, the
// server's per-tick reply to ServerMove when no correction is needed (doc
// sec 20; retail sends ~27/s on the PC channel). Tells the client it can
// drop saved moves up to TimeStamp.
void BuildClientAckGoodMoveRPC(util::BitWriter& w, float time_stamp);

// NameProperty RPC param, AA3 wire form (doc sec 22): Send flag (non-None),
// then FString + INT32 Number. There is NO bHardcoded bit — retail
// NotifyPostureChange("EndWalk") and the client's
// ServerUpdateLevelVisibility("Bridge_pathing") both decode exactly this way.
void WriteRPCNameParam(util::BitWriter& w, const std::string& name, int32_t number = 0);

// AA_PlayerController posture notifications (doc sec 22), all reliable
// client RPCs on the PC channel. Retail sends NotifyPostureChange with the
// posture being LEFT ("EndWalk"/"EndCrouch"/"EndProne"/"EndSprint") and
// whether the weapon was at high-ready in it (false only for Sprint).
void BuildNotifyPostureChangeRPC(util::BitWriter& w, const std::string& posture,
                                 bool in_high_ready);
void BuildNotifyTacticalCombatPostureChangeRPC(util::BitWriter& w, bool in_tactical);
void BuildNotifyWeaponReadyChangeRPC(util::BitWriter& w, bool in_high_ready);

// The login RPCs retail packs into the PlayerController's channel-open bunch
// (frame 72 of GamePlayPCAP_Legit_Server, decoded with
// aa3-tools/map_manifest/decode_rep.py; doc sec 27), in retail's order:
//   ClientSetHUD(AGP_HUD), ClientCapBandwidth(10000),
//   ClientUpdateLevelStreamingStatus(level, ...) per streaming level,
//   ClientFlushLevelStreaming(), ClientCapBandwidth(10000),
//   OpenPostAuthMenu(), ClientEnableTS3Voice(name).
// Without ClientSetHUD the client has no HUD (myHUD None) and so no
// in-game menus; OpenPostAuthMenu opens the team/class menu; the streaming
// status loads the gametype sublevel (spawns, objectives).

// ClientSetHUD(class<HUD> newHUDType, class<ScoreBoard> newScoringType):
// newHUDType is a static class reference (its PackageMap ObjectIndex);
// newScoringType is left None (retail sends only the HUD).
void BuildClientSetHUDRPC(util::BitWriter& w, uint32_t hud_class_object_index);
void BuildClientCapBandwidthRPC(util::BitWriter& w, int32_t cap);
void BuildClientUpdateLevelStreamingStatusRPC(util::BitWriter& w, const std::string& package,
                                              bool should_be_loaded, bool should_be_visible,
                                              bool should_block_on_load);
void BuildClientFlushLevelStreamingRPC(util::BitWriter& w);
void BuildOpenPostAuthMenuRPC(util::BitWriter& w);
// AA_PlayerController.EnterWaitingForPlayers(bool bHideOnly) -- retail's
// reply to ServerJoinTeam, after the TeamMessage (doc sec 27).
void BuildEnterWaitingForPlayersRPC(util::BitWriter& w, bool hide_only);

// Round-phase RPCs (doc sec 28), as retail sends them on the PC channel:
// role selection: ClientEnterPrePlanningTool(false,false,false) +
// ClientGotoState('WaitingForOtherPlayers'); deployment warning:
// ClientDeploymentWarning(10) + ClientUpdatePlayerRallyPointToReach();
// match start: ClientClosePrePlanningTool(false) +
// ClientSteamStatUpdateMissionStart(true).
void BuildClientGotoStateRPC(util::BitWriter& w, const std::string& state);
void BuildClientEnterPrePlanningToolRPC(util::BitWriter& w, bool battle_planner,
                                        bool enter_spectating_state, bool force);
void BuildClientDeploymentWarningRPC(util::BitWriter& w, int32_t warning_time);
// WorldLoc left zero (Send=0), as retail sends it.
void BuildClientUpdatePlayerRallyPointToReachRPC(util::BitWriter& w);
void BuildClientClosePrePlanningToolRPC(util::BitWriter& w, bool enter_spectating_state);
void BuildClientSteamStatUpdateMissionStartRPC(util::BitWriter& w, bool increment_match);

// Possession at match start (retail frames 365-372; doc sec 29), in order:
// ClientRestart(None), ClientSetCameraMode('FirstPerson'), ClientSetRotation,
// ClientMatchStarting(); then, once the pawn's channel is open,
// ClientSetViewTarget(pawn), GivePawn(pawn), ClientAdjustPosition(...),
// ClientSetBlockOnAsyncLoading().
void BuildClientRestartRPC(util::BitWriter& w, uint32_t pawn_channel);
void BuildClientSetCameraModeRPC(util::BitWriter& w, const std::string& mode);
void BuildClientMatchStartingRPC(util::BitWriter& w);
void BuildGivePawnRPC(util::BitWriter& w, uint32_t pawn_channel);
// ClientAdjustPosition(TimeStamp, NewState, newPhysics (EPhysics, 4 bits),
// NewLoc, NewVel, NewBase=None). Unreliable in the class, but retail sends
// it inside a reliable bunch at spawn.
void BuildClientAdjustPositionRPC(util::BitWriter& w, float time_stamp, const std::string& state,
                                  uint8_t physics, const float loc[3], const float vel[3]);
void BuildClientSetBlockOnAsyncLoadingRPC(util::BitWriter& w);

// Spawn inventory (retail frames 365/376; doc sec 30).
// ClientNotifyChangedWeapon(awepNew, aawpnOld=None): the weapon in hand.
void BuildClientNotifyChangedWeaponRPC(util::BitWriter& w, uint32_t weapon_channel,
                                       uint32_t old_weapon_channel = 0);
// Grenade HUD (retail frames 40844 / 41268, doc sec 37):
// ClientSendHUDNotifyChangeInventoryCount(InventoryType, NumClips[, ClipIndex])
// and ClientAdvancedInventoryUpdateGrenadeCounts(itActiveInventoryItem[, bNoUpdateCurrent]).
void BuildClientSendHUDNotifyChangeInventoryCountRPC(util::BitWriter& w, uint8_t inventory_type,
                                                     int32_t num_clips);
void BuildClientAdvancedInventoryUpdateGrenadeCountsRPC(util::BitWriter& w, uint8_t inventory_type,
                                                        bool no_update_current);
// EInventoryType is 5 bits (CeilLog2(NumEnums-1)).
void BuildClientSendHUDNotifyAddedInventoryRPC(util::BitWriter& w, uint8_t inventory_type,
                                               bool has_weapon_mod);
void BuildClientAdvancedInventoryInitializeAmmoMagazineCountRPC(util::BitWriter& w,
                                                                uint8_t inventory_type,
                                                                int32_t magazine_count);

// Round end / death / reset (doc sec 36).
void WriteRPCStaticObjectParam(util::BitWriter& w, uint32_t object_index);
// A PlayerController call without parameters (ClientPawnDied, ClientUnpossess,
// TS3ClientChangeChannel, ClientUpdateSpectatorViewType, ClientReset...).
void BuildPCCallRPC(util::BitWriter& w, uint32_t rep_index);
void BuildClientPlaySoundRPC(util::BitWriter& w, uint32_t sound_object_index);
void BuildClientSetViewTargetObjectRPC(util::BitWriter& w, uint32_t object_index);
void BuildClientSetSpectatorModeRPC(util::BitWriter& w, const std::string& mode);
void BuildHideScoreboardRPC(util::BitWriter& w, bool hide_steam_scoreboard);
void BuildClientUpdateScoreboardRPC(util::BitWriter& w, bool visibility_override);
void BuildClientGameEndedRPC(util::BitWriter& w, bool is_winner);
void BuildClientDisplayScoreboardRPC(util::BitWriter& w, int32_t aar_seconds, int32_t round);

} // namespace network
} // namespace aa3

#endif // AA3_NETWORK_RPC_ENCODE_H
