#ifndef AA3_NETWORK_RPC_DECODE_H
#define AA3_NETWORK_RPC_DECODE_H

#include <cstdint>
#include <string>
#include <vector>

#include "util/bits.h"

// Client->server RPC decoding for the PlayerController channel, table-driven:
// each known function is one row {RepIndex, name, parameter kinds}. RepIndex
// values come from rep_layout_gen.h; parameter lists from
// `aa3_build_manifests --struct-layout <pkg> <Function>` (CPF_Parm-filtered,
// doc sec 20). Wire rule per parameter (doc sec 15, UnScript.cpp
// InternalProcessRemoteFunction): BoolProperty = 1 bit; anything else =
// Send:1, then the value in NetSerializeItem format iff Send=1 (Send=0 means
// zero/default).
//
// Validated against retail: on GamePlayPCAP_Legit_Server.pcapng, every
// ServerMove/DualServerMove/OldServerMove/ServerSetSpectatorLocation/
// ServerOffsetAim/... bunch on the PC channel (~62k RPCs) decodes to exactly
// its declared BunchDataBits (aa3-tools/map_manifest/decode_servermove.py).

namespace aa3 {
namespace network {

enum class RpcParamKind : uint8_t {
    kFloat,     // 32 raw bits
    kInt,       // 32 raw bits
    kByte,      // 8 bits (no Enum)
    kBool,      // 1 bit, no Send flag
    kVector,    // FVector::SerializeCompressed (integer components)
    kString,    // FString: INT32 SaveNum (+ANSI / -UTF16 count) + chars
    kObject,    // SerializeObject: bDynamic:1, then RIM(0x800) channel or RIM(2^31) index
    kName,      // AA3 FName: FString + INT32 Number (NO bHardcoded bit — doc sec 22)
    kOpaque,    // struct whose layout isn't resolved: decodable only if Send=0
    kEnum6,     // ByteProperty of a 33-64 name enum: 6 bits (eAGP_CommoTypes)
};

struct RpcValue {
    RpcParamKind kind = RpcParamKind::kInt;
    bool sent = false;       // Send flag (always true for kBool)
    float f = 0.0f;
    int32_t i = 0;           // kInt, kByte, kBool
    int32_t v[3] = {0, 0, 0};
    std::string s;           // kString, kName (name text; Number in i)
    bool dynamic = false;    // kObject: true = `i` is a channel index
};

// Stable identifiers for the functions the server reacts to.
enum class PcRpc : uint8_t {
    kServerMove,
    kDualServerMove,
    kOldServerMove,
    kServerDrive,
    kServerSetNetSpeed,
    kServerUpdatePing,
    kServerSetSpectatorLocation,
    kServerViewSelf,
    kServerReturnToParty,
    kForceServerOffsetAimUpdate,
    kServerOffsetAim,
    kServerRaiseSights,
    kServerHoldSprint,
    kServerStopSprint,
    // Doc sec 22: every other PC RPC seen in the retail capture.
    kServerShortTimeout,
    kServerAcknowledgePossession,
    kServerThrowWeapon,
    kServerUse,
    kServerVerifyViewTarget,
    kServerViewNextPlayer,
    kServerViewPrevPlayer,
    kServerUpdateLevelVisibility,
    kServerSetTS3ClientID,
    kServerSetLoadoutPreference,
    kServerSetWeaponPreference,
    kServerLowerSights,
    kServerRequestPawnSynch,
    kServerToggleStance,
    kServerStandUp,
    kServerSetProne,
    kServerSetCrouch,
    kServerTapSprint,
    kServerTryVault,
    kServerSuicide,              // withdraw (MEDEVAC) while incapacitated (doc sec 35)
    kServerIncapChangeState,     // nameNextState: the client's incap state machine
    kServerUpdateScoreboard,
    kServerSetPlayerReady,
    kServerRemovePlayerFromScoreboard,
    kServerJoinTeam,
    kServerOpenDoor,
    kServerCloseDoor,
    kServerNotifySpectateHUDLoaded,
    kServerSelectClassRole_ByIndex,
    kServerSetPrimaryWeaponOne,
    kServerSetPrimaryWeaponAmmoTypeOne,
    kServerSetRailMod,
    kAskForPawn,
    kSendES2ObjectReportRequest,
    kServerES2Use,
    kServerES2Active,
    // IFAK treatment (doc sec 39).
    kServerStartTreatment,
    kServerStartPassiveTreatment,
    kServerStopTreatment,
    kServerGiveActiveTreatment,
    kServerWeaponRaise,
    kSendCommoRequest,
    // Carried objective and co-op reinforcements (Bridge AI capture 2026-10-12).
    kServerRepExec,
    kServerRequestRespawn,
    kServerDropObjective,
};

struct DecodedRpc {
    PcRpc id;
    const char* name;
    std::vector<RpcValue> params;  // declaration order
};

enum class RpcDecodeResult {
    kOk,
    kUnknownIndex,   // RepIndex isn't a function in the table (or a property)
    kUndecodable,    // known function, but a kOpaque param had Send=1
    kOverrun,        // ran past `end_bit`
};

// Reads one RepIndex + parameters at r's position, against the PC class
// table (`max_index` = the channel's class MaxIndex). On kUnknownIndex /
// kUndecodable the reader is left mid-bunch; the caller must resync via BDB.
RpcDecodeResult DecodePcRpc(util::BitReader& r, uint32_t end_bit, uint32_t max_index,
                            uint32_t* rep_index_out, DecodedRpc* out);

}  // namespace network
}  // namespace aa3

#endif  // AA3_NETWORK_RPC_DECODE_H
