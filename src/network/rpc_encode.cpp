#include "network/rpc_encode.h"
#include "network/rep_encode.h"  // kPCMaxIndex
#include <cstring>

namespace aa3 {
namespace network {

// StrProperty/FloatProperty NetSerializeItem (doc sec 10.4) — the same
// wire shape rep_encode.cpp's WriteStrProp/WriteFloatProp already use.
// Duplicated rather than shared, for the same reason rep_encode.cpp
// duplicates protocol_handler.cpp's WriteFString: keeps each file's wire
// primitives self-contained rather than exposing them across headers for
// two call sites.
static void WriteStrValue(util::BitWriter& w, const std::string& s) {
    w.WriteInt(static_cast<uint32_t>(s.length() + 1));
    for (char c : s) w.WriteByte(static_cast<uint8_t>(c));
    w.WriteByte(0);
}

static void WriteFloatValue(util::BitWriter& w, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    w.WriteInt(bits);
}

void WriteRPCStrParam(util::BitWriter& w, const std::string& value) {
    bool send = !value.empty();  // FString zero/default == empty
    w.WriteBit(send);
    if (send) WriteStrValue(w, value);
}

void WriteRPCFloatParam(util::BitWriter& w, float value) {
    bool send = (value != 0.0f);
    w.WriteBit(send);
    if (send) WriteFloatValue(w, value);
}

// ObjectProperty param referencing a spawned actor: Send flag (non-NULL),
// then rep_encode.h's WriteActorRef (bDynamic=1 + the actor's channel on
// this connection — doc sec 21; retail ClientSetViewTarget(pawn) = dynamic
// ch 159/161/163).
void WriteRPCObjectParam(util::BitWriter& w, uint32_t actor_channel) {
    bool send = (actor_channel != 0);
    w.WriteBit(send);
    if (send) WriteActorRef(w, actor_channel);
}

void WriteRPCRotatorParam(util::BitWriter& w, int32_t pitch, int32_t yaw, int32_t roll) {
    bool send = (pitch != 0 || yaw != 0 || roll != 0);
    w.WriteBit(send);
    if (send) WriteCompressedRotator(w, pitch, yaw, roll);
}

void WriteRPCByteEnumParam(util::BitWriter& w, uint8_t value, uint32_t num_bits) {
    bool send = (value != 0);
    w.WriteBit(send);
    if (send) w.WriteBits(value, num_bits);
}

void WriteRPCBoolParam(util::BitWriter& w, bool value) {
    w.WriteBit(value);  // no Send-flag — bools are always sent (source above)
}

void BuildClientMessageRPC(util::BitWriter& w, const std::string& message) {
    constexpr uint32_t kClientMessageRepIndex = rep::PC::ClientMessage;
    w.WriteIntMax(kClientMessageRepIndex, kPCMaxIndex);

    WriteRPCStrParam(w, message);  // S

    // Type (Name) and MsgLifeTime (float): left at defaults, Send=0 for
    // both. Neither is a BoolProperty, so each still costs exactly the
    // 1-bit Send flag even though skipped — matches source exactly, not
    // an approximation.
    w.WriteBit(false);  // Type: Send=0
    w.WriteBit(false);  // MsgLifeTime: Send=0
}

void BuildClientSetRotationRPC(util::BitWriter& w, int32_t pitch, int32_t yaw,
                               int32_t roll, bool reset_camera) {
    constexpr uint32_t kClientSetRotationRepIndex = rep::PC::ClientSetRotation;
    w.WriteIntMax(kClientSetRotationRepIndex, kPCMaxIndex);
    WriteRPCRotatorParam(w, pitch, yaw, roll);  // NewRotation
    WriteRPCBoolParam(w, reset_camera);          // bResetCamera
}

void BuildClientSwitchToBestWeaponRPC(util::BitWriter& w, bool force_new_weapon) {
    constexpr uint32_t kClientSwitchToBestWeaponRepIndex = rep::PC::ClientSwitchToBestWeapon;
    w.WriteIntMax(kClientSwitchToBestWeaponRepIndex, kPCMaxIndex);
    WriteRPCBoolParam(w, force_new_weapon);  // bForceNewWeapon
}

void BuildTeamMessageRPC(util::BitWriter& w, uint32_t pri_channel,
                         const std::string& message) {
    constexpr uint32_t kTeamMessageRepIndex = rep::PC::TeamMessage;
    w.WriteIntMax(kTeamMessageRepIndex, kPCMaxIndex);
    WriteRPCObjectParam(w, pri_channel);       // PRI
    WriteRPCStrParam(w, message);              // S
    w.WriteBit(false);                         // Type: Send=0
    w.WriteBit(false);                         // MsgLifeTime: Send=0
}

void BuildClientSetProgressMessageRPC(util::BitWriter& w, uint8_t message_type,
                                      const std::string& message,
                                      bool ignore_future_network_messages) {
    constexpr uint32_t kClientSetProgressMessageRepIndex = rep::PC::ClientSetProgressMessage;
    constexpr uint32_t kProgressMessageTypeBits = 3;  // CeilLog2(7-1), see header
    w.WriteIntMax(kClientSetProgressMessageRepIndex, kPCMaxIndex);
    WriteRPCByteEnumParam(w, message_type, kProgressMessageTypeBits);  // MessageType
    WriteRPCStrParam(w, message);                                     // Message
    w.WriteBit(false);                                                // Title: Send=0
    WriteRPCBoolParam(w, ignore_future_network_messages);             // bIgnoreFutureNetworkMessages
}

void BuildClientSetViewTargetRPC(util::BitWriter& w, uint32_t actor_channel) {
    constexpr uint32_t kClientSetViewTargetRepIndex = rep::PC::ClientSetViewTarget;
    w.WriteIntMax(kClientSetViewTargetRepIndex, kPCMaxIndex);
    WriteRPCObjectParam(w, actor_channel);       // A
    w.WriteBit(false);                           // TransitionParams: Send=0
}

void BuildClientAckGoodMoveRPC(util::BitWriter& w, float time_stamp) {
    w.WriteIntMax(rep::PC::ClientAckGoodMove, kPCMaxIndex);
    // Non-bool param: Send flag, then 32 raw float bits iff nonzero.
    uint32_t bits;
    std::memcpy(&bits, &time_stamp, sizeof(bits));
    w.WriteBit(bits != 0);
    if (bits != 0) w.WriteInt(bits);
}

void WriteRPCNameParam(util::BitWriter& w, const std::string& name, int32_t number) {
    bool send = !name.empty() && name != "None";
    w.WriteBit(send);
    if (!send) return;
    WriteStrValue(w, name);
    w.WriteInt(static_cast<uint32_t>(number));
}

void BuildNotifyPostureChangeRPC(util::BitWriter& w, const std::string& posture,
                                 bool in_high_ready) {
    w.WriteIntMax(rep::PC::NotifyPostureChange, kPCMaxIndex);
    WriteRPCNameParam(w, posture);       // strPosture
    WriteRPCBoolParam(w, in_high_ready); // bInHighReady
}

void BuildNotifyTacticalCombatPostureChangeRPC(util::BitWriter& w, bool in_tactical) {
    w.WriteIntMax(rep::PC::NotifyTacticalCombatPostureChange, kPCMaxIndex);
    WriteRPCBoolParam(w, in_tactical);   // bInTactical
}

void BuildNotifyWeaponReadyChangeRPC(util::BitWriter& w, bool in_high_ready) {
    w.WriteIntMax(rep::PC::NotifyWeaponReadyChange, kPCMaxIndex);
    WriteRPCBoolParam(w, in_high_ready); // bInHighReady
}

void BuildClientSetHUDRPC(util::BitWriter& w, uint32_t hud_class_object_index) {
    w.WriteIntMax(rep::PC::ClientSetHUD, kPCMaxIndex);
    // newHUDType: Send, then SerializeObject's static form -- bDynamic=0 +
    // ObjectIndex RIM(2^31).
    bool send = hud_class_object_index != 0;
    w.WriteBit(send);
    if (send) {
        w.WriteBit(0);
        w.WriteIntMax(hud_class_object_index, 0x80000000);
    }
    w.WriteBit(false);  // newScoringType: Send=0
}

void BuildClientCapBandwidthRPC(util::BitWriter& w, int32_t cap) {
    w.WriteIntMax(rep::PC::ClientCapBandwidth, kPCMaxIndex);
    w.WriteBit(cap != 0);
    if (cap != 0) w.WriteInt(static_cast<uint32_t>(cap));
}

void BuildClientUpdateLevelStreamingStatusRPC(util::BitWriter& w, const std::string& package,
                                              bool should_be_loaded, bool should_be_visible,
                                              bool should_block_on_load) {
    w.WriteIntMax(rep::PC::ClientUpdateLevelStreamingStatus, kPCMaxIndex);
    WriteRPCNameParam(w, package);               // PackageName
    WriteRPCBoolParam(w, should_be_loaded);      // bNewShouldBeLoaded
    WriteRPCBoolParam(w, should_be_visible);     // bNewShouldBeVisible
    WriteRPCBoolParam(w, should_block_on_load);  // bNewShouldBlockOnLoad
}

void BuildClientFlushLevelStreamingRPC(util::BitWriter& w) {
    w.WriteIntMax(rep::PC::ClientFlushLevelStreaming, kPCMaxIndex);
}

void BuildOpenPostAuthMenuRPC(util::BitWriter& w) {
    w.WriteIntMax(rep::PC::OpenPostAuthMenu, kPCMaxIndex);
}

void BuildEnterWaitingForPlayersRPC(util::BitWriter& w, bool hide_only) {
    w.WriteIntMax(rep::PC::EnterWaitingForPlayers, kPCMaxIndex);
    WriteRPCBoolParam(w, hide_only);   // bHideOnly
}

void BuildClientGotoStateRPC(util::BitWriter& w, const std::string& state) {
    w.WriteIntMax(rep::PC::ClientGotoState, kPCMaxIndex);
    WriteRPCNameParam(w, state);   // NewState
    w.WriteBit(false);             // NewLabel: Send=0
}

void BuildClientEnterPrePlanningToolRPC(util::BitWriter& w, bool battle_planner,
                                        bool enter_spectating_state, bool force) {
    w.WriteIntMax(rep::PC::ClientEnterPrePlanningTool, kPCMaxIndex);
    WriteRPCBoolParam(w, battle_planner);
    WriteRPCBoolParam(w, enter_spectating_state);
    WriteRPCBoolParam(w, force);
}

void BuildClientDeploymentWarningRPC(util::BitWriter& w, int32_t warning_time) {
    w.WriteIntMax(rep::PC::ClientDeploymentWarning, kPCMaxIndex);
    w.WriteBit(warning_time != 0);
    if (warning_time != 0) w.WriteInt(static_cast<uint32_t>(warning_time));
}

void BuildClientUpdatePlayerRallyPointToReachRPC(util::BitWriter& w) {
    w.WriteIntMax(rep::PC::ClientUpdatePlayerRallyPointToReach, kPCMaxIndex);
    w.WriteBit(false);             // WorldLoc: Send=0
}

void BuildClientClosePrePlanningToolRPC(util::BitWriter& w, bool enter_spectating_state) {
    w.WriteIntMax(rep::PC::ClientClosePrePlanningTool, kPCMaxIndex);
    WriteRPCBoolParam(w, enter_spectating_state);
}

void BuildClientSteamStatUpdateMissionStartRPC(util::BitWriter& w, bool increment_match) {
    w.WriteIntMax(rep::PC::ClientSteamStatUpdateMissionStart, kPCMaxIndex);
    WriteRPCBoolParam(w, increment_match);
}

void BuildClientRestartRPC(util::BitWriter& w, uint32_t pawn_channel) {
    w.WriteIntMax(rep::PC::ClientRestart, kPCMaxIndex);
    WriteRPCObjectParam(w, pawn_channel);   // NewPawn
}

void BuildClientSetCameraModeRPC(util::BitWriter& w, const std::string& mode) {
    w.WriteIntMax(rep::PC::ClientSetCameraMode, kPCMaxIndex);
    WriteRPCNameParam(w, mode);             // NewCamMode
}

void BuildClientMatchStartingRPC(util::BitWriter& w) {
    w.WriteIntMax(rep::PC::ClientMatchStarting, kPCMaxIndex);
}

void BuildGivePawnRPC(util::BitWriter& w, uint32_t pawn_channel) {
    w.WriteIntMax(rep::PC::GivePawn, kPCMaxIndex);
    WriteRPCObjectParam(w, pawn_channel);   // NewPawn
}

void BuildClientAdjustPositionRPC(util::BitWriter& w, float time_stamp, const std::string& state,
                                  uint8_t physics, const float loc[3], const float vel[3]) {
    w.WriteIntMax(rep::PC::ClientAdjustPosition, kPCMaxIndex);
    WriteRPCFloatParam(w, time_stamp);
    WriteRPCNameParam(w, state);
    WriteRPCByteEnumParam(w, physics, 4);   // EPhysics: 14 names -> 4 bits (doc sec 11.2)
    for (int i = 0; i < 3; ++i) WriteRPCFloatParam(w, loc[i]);
    for (int i = 0; i < 3; ++i) WriteRPCFloatParam(w, vel[i]);
    w.WriteBit(false);                      // NewBase: Send=0 (None)
}

void BuildClientSetBlockOnAsyncLoadingRPC(util::BitWriter& w) {
    w.WriteIntMax(rep::PC::ClientSetBlockOnAsyncLoading, kPCMaxIndex);
}

void BuildClientNotifyChangedWeaponRPC(util::BitWriter& w, uint32_t weapon_channel) {
    w.WriteIntMax(rep::PC::ClientNotifyChangedWeapon, kPCMaxIndex);
    WriteRPCObjectParam(w, weapon_channel);   // awepNew
    w.WriteBit(false);                        // aawpnOld: Send=0 (None)
}

void BuildClientSendHUDNotifyAddedInventoryRPC(util::BitWriter& w, uint8_t inventory_type,
                                               bool has_weapon_mod) {
    w.WriteIntMax(rep::PC::ClientSendHUDNotifyAddedInventory, kPCMaxIndex);
    WriteRPCByteEnumParam(w, inventory_type, 5);
    WriteRPCBoolParam(w, has_weapon_mod);
}

void BuildClientAdvancedInventoryInitializeAmmoMagazineCountRPC(util::BitWriter& w,
                                                                uint8_t inventory_type,
                                                                int32_t magazine_count) {
    w.WriteIntMax(rep::PC::ClientAdvancedInventoryInitializeAmmoMagazineCount, kPCMaxIndex);
    WriteRPCByteEnumParam(w, inventory_type, 5);
    w.WriteBit(magazine_count != 0);
    if (magazine_count != 0) w.WriteInt(static_cast<uint32_t>(magazine_count));
}

} // namespace network
} // namespace aa3
