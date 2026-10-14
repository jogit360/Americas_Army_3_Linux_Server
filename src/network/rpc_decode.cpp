#include "network/rpc_decode.h"

#include <cstring>
#include <stdexcept>

#include "network/rep_layout_gen.h"

namespace aa3 {
namespace network {

namespace {

using K = RpcParamKind;

struct RpcSpec {
    uint32_t rep_index;
    PcRpc id;
    const char* name;
    std::vector<K> params;
};

// Parameter lists: `--struct-layout Engine.u|AA3Baseline.u <fn>` (doc sec 20).
// RepIndex: rep::PC::* -- identical in AA_PlayerController and
// AGP_PlayerController, which only appends fields after AA_PC's.
const std::vector<RpcSpec>& Table() {
    static const std::vector<RpcSpec> t = {
        {rep::PC::ServerMove, PcRpc::kServerMove, "ServerMove",
         {K::kFloat, K::kVector, K::kVector, K::kByte, K::kByte, K::kInt}},
        {rep::PC::DualServerMove, PcRpc::kDualServerMove, "DualServerMove",
         {K::kFloat, K::kVector, K::kByte, K::kInt,
          K::kFloat, K::kVector, K::kVector, K::kByte, K::kByte, K::kInt}},
        {rep::PC::OldServerMove, PcRpc::kOldServerMove, "OldServerMove",
         {K::kFloat, K::kByte, K::kByte, K::kByte, K::kByte}},
        {rep::PC::ServerDrive, PcRpc::kServerDrive, "ServerDrive",
         {K::kFloat, K::kFloat, K::kFloat, K::kBool, K::kInt}},
        {rep::PC::ServerSetNetSpeed, PcRpc::kServerSetNetSpeed, "ServerSetNetSpeed", {K::kInt}},
        {rep::PC::ServerUpdatePing, PcRpc::kServerUpdatePing, "ServerUpdatePing", {K::kInt}},
        {rep::PC::ServerSetSpectatorLocation, PcRpc::kServerSetSpectatorLocation,
         "ServerSetSpectatorLocation", {K::kVector}},
        // TransitionParams (ViewTargetTransitionParams) layout unresolved.
        {rep::PC::ServerViewSelf, PcRpc::kServerViewSelf, "ServerViewSelf", {K::kOpaque}},
        {rep::PC::ServerReturnToParty, PcRpc::kServerReturnToParty, "ServerReturnToParty",
         {K::kBool}},
        {rep::PC::ForceServerOffsetAimUpdate, PcRpc::kForceServerOffsetAimUpdate,
         "ForceServerOffsetAimUpdate", {K::kInt}},
        {rep::PC::ServerOffsetAim, PcRpc::kServerOffsetAim, "ServerOffsetAim", {K::kInt}},
        {rep::PC::ServerRaiseSights, PcRpc::kServerRaiseSights, "ServerRaiseSights", {}},
        {rep::PC::ServerHoldSprint, PcRpc::kServerHoldSprint, "ServerHoldSprint", {}},
        {rep::PC::ServerStopSprint, PcRpc::kServerStopSprint, "ServerStopSprint", {}},
        // Doc sec 22: the rest of the client's PC traffic in the retail
        // capture (99.994% of 54,138 bunches decode exactly with these).
        {rep::PC::ServerShortTimeout, PcRpc::kServerShortTimeout, "ServerShortTimeout", {}},
        {rep::PC::ServerAcknowledgePossession, PcRpc::kServerAcknowledgePossession,
         "ServerAcknowledgePossession", {K::kObject}},
        {rep::PC::ServerThrowWeapon, PcRpc::kServerThrowWeapon, "ServerThrowWeapon", {}},
        {rep::PC::ServerUse, PcRpc::kServerUse, "ServerUse", {}},
        {rep::PC::ServerVerifyViewTarget, PcRpc::kServerVerifyViewTarget, "ServerVerifyViewTarget", {}},
        {rep::PC::ServerViewNextPlayer, PcRpc::kServerViewNextPlayer, "ServerViewNextPlayer", {}},
        {rep::PC::ServerViewPrevPlayer, PcRpc::kServerViewPrevPlayer, "ServerViewPrevPlayer", {}},
        {rep::PC::ServerUpdateLevelVisibility, PcRpc::kServerUpdateLevelVisibility,
         "ServerUpdateLevelVisibility", {K::kName, K::kBool}},
        {rep::PC::ServerSetTS3ClientID, PcRpc::kServerSetTS3ClientID, "ServerSetTS3ClientID", {K::kInt}},
        {rep::PC::ServerSetLoadoutPreference, PcRpc::kServerSetLoadoutPreference,
         "ServerSetLoadoutPreference", {K::kInt, K::kInt, K::kInt}},
        {rep::PC::ServerSetWeaponPreference, PcRpc::kServerSetWeaponPreference,
         "ServerSetWeaponPreference", {K::kInt, K::kInt, K::kString}},
        {rep::PC::ServerLowerSights, PcRpc::kServerLowerSights, "ServerLowerSights", {}},
        {rep::PC::ServerRequestPawnSynch, PcRpc::kServerRequestPawnSynch, "ServerRequestPawnSynch", {}},
        {rep::PC::ServerToggleStance, PcRpc::kServerToggleStance, "ServerToggleStance", {}},
        {rep::PC::ServerStandUp, PcRpc::kServerStandUp, "ServerStandUp", {}},
        {rep::PC::ServerSetProne, PcRpc::kServerSetProne, "ServerSetProne", {K::kBool}},
        {rep::PC::ServerSetCrouch, PcRpc::kServerSetCrouch, "ServerSetCrouch", {K::kBool}},
        {rep::PC::ServerTapSprint, PcRpc::kServerTapSprint, "ServerTapSprint", {}},
        {rep::PC::ServerTryVault, PcRpc::kServerTryVault, "ServerTryVault", {}},
        {rep::PC::ServerSuicide, PcRpc::kServerSuicide, "ServerSuicide", {}},
        {rep::PC::ServerIncapChangeState, PcRpc::kServerIncapChangeState, "ServerIncapChangeState", {K::kName}},
        {rep::PC::ServerUpdateScoreboard, PcRpc::kServerUpdateScoreboard, "ServerUpdateScoreboard", {}},
        {rep::PC::ServerSetPlayerReady, PcRpc::kServerSetPlayerReady, "ServerSetPlayerReady", {K::kBool}},
        {rep::PC::ServerRemovePlayerFromScoreboard, PcRpc::kServerRemovePlayerFromScoreboard,
         "ServerRemovePlayerFromScoreboard", {K::kString}},
        {rep::PC::ServerJoinTeam, PcRpc::kServerJoinTeam, "ServerJoinTeam", {K::kInt}},
        {rep::PC::ServerOpenDoor, PcRpc::kServerOpenDoor, "ServerOpenDoor", {K::kObject}},
        {rep::PC::ServerCloseDoor, PcRpc::kServerCloseDoor, "ServerCloseDoor", {K::kObject}},
        {rep::PC::ServerNotifySpectateHUDLoaded, PcRpc::kServerNotifySpectateHUDLoaded,
         "ServerNotifySpectateHUDLoaded", {}},
        {rep::PC::ServerSelectClassRole_ByIndex, PcRpc::kServerSelectClassRole_ByIndex,
         "ServerSelectClassRole_ByIndex", {K::kInt}},
        {rep::PC::ServerSetPrimaryWeaponOne, PcRpc::kServerSetPrimaryWeaponOne,
         "ServerSetPrimaryWeaponOne", {K::kObject}},
        {rep::PC::ServerSetPrimaryWeaponAmmoTypeOne, PcRpc::kServerSetPrimaryWeaponAmmoTypeOne,
         "ServerSetPrimaryWeaponAmmoTypeOne", {K::kObject}},
        {rep::PC::ServerSetRailMod, PcRpc::kServerSetRailMod, "ServerSetRailMod",
         {K::kString, K::kString, K::kInt}},
        {rep::PC::AskForPawn, PcRpc::kAskForPawn, "AskForPawn", {}},
        // ES2 objects (live client 2026-10-11: a perk pickup sends
        // ServerES2Active + ServerUse + ServerES2Use in one bunch).
        {rep::PC::ServerES2Use, PcRpc::kServerES2Use, "ServerES2Use", {K::kObject}},
        // IFAK treatment (retail capture 2026-10-11; doc sec 39). The active
        // treatment's WoundTreatmentAction struct goes out Send=0 (default).
        {rep::PC::ServerStartTreatment, PcRpc::kServerStartTreatment, "ServerStartTreatment",
         {K::kString}},
        {rep::PC::ServerStartPassiveTreatment, PcRpc::kServerStartPassiveTreatment,
         "ServerStartPassiveTreatment", {}},
        {rep::PC::ServerStopTreatment, PcRpc::kServerStopTreatment, "ServerStopTreatment", {}},
        {rep::PC::ServerGiveActiveTreatment, PcRpc::kServerGiveActiveTreatment,
         "ServerGiveActiveTreatment", {K::kName, K::kOpaque, K::kString}},
        {rep::PC::ServerWeaponRaise, PcRpc::kServerWeaponRaise, "ServerWeaponRaise", {K::kBool}},
        // Bridge AI capture 2026-10-12: ServerRepExec('Pickup', ContextActor)
        // picks up the briefcase (+ ServerUse); ServerRequestRespawn is F5.
        {rep::PC::ServerRepExec, PcRpc::kServerRepExec, "ServerRepExec", {K::kName, K::kObject}},
        {rep::PC::ServerRequestRespawn, PcRpc::kServerRequestRespawn, "ServerRequestRespawn", {}},
        // A downed carrier drops the case for a teammate (SafeDropObjective).
        {rep::PC::ServerDropObjective, PcRpc::kServerDropObjective, "ServerDropObjective", {K::kBool}},
        {rep::AGP_PC::SendCommoRequest, PcRpc::kSendCommoRequest, "SendCommoRequest", {K::kEnum6}},
        // Securing a downed enemy / confirming a dead one (+ ServerUse).
        {rep::AGP_PC::SendEnemyDownRequest, PcRpc::kSendEnemyDownRequest, "SendEnemyDownRequest",
         {K::kString, K::kObject}},
        {rep::PC::ServerES2Active, PcRpc::kServerES2Active, "ServerES2Active",
         {K::kObject, K::kObject, K::kBool}},
        // AGP_PlayerController-only (appended after AA_PC's 473 fields).
        {rep::AGP_PC::SendES2ObjectReportRequest, PcRpc::kSendES2ObjectReportRequest,
         "SendES2ObjectReportRequest", {K::kObject}},
    };
    return t;
}

// FVector::SerializeCompressed (UnMath.cpp:51-77), receive side; mirror of
// rep_encode.cpp's WriteCompressedVector.
void ReadCompressedVector(util::BitReader& r, int32_t out[3]) {
    uint32_t bits = r.ReadIntMax(20);
    int32_t bias = 1 << (bits + 1);
    uint32_t max_val = 1u << (bits + 2);
    for (int k = 0; k < 3; ++k)
        out[k] = static_cast<int32_t>(r.ReadIntMax(max_val)) - bias;
}

// FString (UnMisc.cpp operator<<): SaveNum > 0 = ANSI chars incl. NUL,
// < 0 = UTF-16 code units incl. NUL. Non-ASCII UTF-16 is narrowed to '?'
// (names/loadout strings are ASCII in practice). Bounded so a corrupt
// length can't allocate unboundedly (doc: aa3_server_vuln_surface).
bool ReadFString(util::BitReader& r, std::string* out) {
    int32_t n = static_cast<int32_t>(r.ReadInt());
    if (n < -1024 || n > 1024) return false;
    out->clear();
    if (n >= 0) {
        for (int32_t k = 0; k < n; ++k) {
            char c = static_cast<char>(r.ReadByte());
            if (c) out->push_back(c);
        }
    } else {
        for (int32_t k = 0; k < -n; ++k) {
            uint32_t u = r.ReadByte() | (static_cast<uint32_t>(r.ReadByte()) << 8);
            if (u) out->push_back(u < 0x80 ? static_cast<char>(u) : '?');
        }
    }
    return true;
}

}  // namespace

RpcDecodeResult DecodePcRpc(util::BitReader& r, uint32_t end_bit, uint32_t max_index,
                            uint32_t* rep_index_out, DecodedRpc* out) {
    try {
        uint32_t rep = r.ReadIntMax(max_index);
        if (rep_index_out) *rep_index_out = rep;
        if (r.GetPos() > end_bit) return RpcDecodeResult::kOverrun;

        const RpcSpec* spec = nullptr;
        for (const auto& s : Table())
            if (s.rep_index == rep) { spec = &s; break; }
        if (!spec) return RpcDecodeResult::kUnknownIndex;

        out->id = spec->id;
        out->name = spec->name;
        out->params.clear();
        for (K kind : spec->params) {
            RpcValue v;
            v.kind = kind;
            if (kind == K::kBool) {
                v.sent = true;
                v.i = r.ReadBit() ? 1 : 0;
            } else {
                v.sent = r.ReadBit();
                if (v.sent) {
                    switch (kind) {
                        case K::kFloat: {
                            uint32_t b = r.ReadInt();
                            std::memcpy(&v.f, &b, sizeof(b));
                            break;
                        }
                        case K::kInt:    v.i = static_cast<int32_t>(r.ReadInt()); break;
                        case K::kByte:   v.i = r.ReadByte(); break;
                        case K::kEnum6:  v.i = static_cast<int32_t>(r.ReadBits(6)); break;
                        case K::kVector: ReadCompressedVector(r, v.v); break;
                        case K::kString:
                            if (!ReadFString(r, &v.s)) return RpcDecodeResult::kOverrun;
                            break;
                        case K::kName:
                            if (!ReadFString(r, &v.s)) return RpcDecodeResult::kOverrun;
                            v.i = static_cast<int32_t>(r.ReadInt());
                            break;
                        case K::kObject:
                            v.dynamic = r.ReadBit();
                            v.i = static_cast<int32_t>(v.dynamic ? r.ReadIntMax(0x800)
                                                                 : r.ReadIntMax(0x80000000));
                            break;
                        case K::kOpaque: return RpcDecodeResult::kUndecodable;
                        case K::kBool:   break;
                    }
                }
            }
            out->params.push_back(v);
        }
        return r.GetPos() > end_bit ? RpcDecodeResult::kOverrun : RpcDecodeResult::kOk;
    } catch (const std::exception&) {
        return RpcDecodeResult::kOverrun;
    }
}

}  // namespace network
}  // namespace aa3
