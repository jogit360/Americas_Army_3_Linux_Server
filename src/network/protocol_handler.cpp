#include "network/protocol_handler.h"
#include "network/rep_encode.h"
#include "network/rpc_encode.h"
#include "network/rpc_decode.h"
#include "engine/map_manifest.h"
#include "game/world.h"
#include "util/logging.h"
#include "util/bits.h"
#include <algorithm>
#include <array>
#include <vector>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <ctime>

namespace aa3 {
namespace network {

namespace {
// FVector::SerializeCompressed, receive side (same as rpc_decode.cpp's).
void ReadCompressedVector(util::BitReader& r, int32_t out[3]) {
    uint32_t bits = r.ReadIntMax(20);
    int32_t bias = 1 << (bits + 1);
    uint32_t max_val = 1u << (bits + 2);
    for (int k = 0; k < 3; ++k) out[k] = static_cast<int32_t>(r.ReadIntMax(max_val)) - bias;
}
}  // namespace


// AA3 packet format (confirmed 2026-05-14 from FUN_009e85c0 — see
// REVERSE_ENGINEERING_NOTES.md §2). The whole UDP payload is one UE3
// bit-stream LSB-first per byte. There is NO outer envelope.
//
//   PacketSequence : RIM(0x4000)
//   while (cursor < terminator):
//      bIsAck:1
//      if bIsAck: AckedPacketID_raw:RIM(0x4000)
//      else: bHasOpenClose:1 [bOpen:1 bClose:1] bReliable:1
//            ChIndex:RIM(0x800)
//            if bReliable: ChSeqRaw:RIM(0x400)  ChType:RIM(8)
//            elif bOpen:   ChType:RIM(8)
//            BunchDataBits:RIM(kMaxPayloadBits)
//            <BunchDataBits bits>
//   terminator:1, zero-pad to byte boundary
static constexpr uint32_t kControlChIndex = 0;   // legit handshake uses Ch=0
static constexpr uint32_t kControlChType  = 1;   // CHTYPE_Control
static constexpr uint32_t kActorChType    = 2;   // CHTYPE_Actor
// This client's own actors (doc sec 26). Retail opens the PlayerController
// on Ch=2 (Default__AGP_PlayerController, NetPlayerIndex 0), then StatManager
// 3, GRI 4, MOS qualification 5, TeamInfo 6-9, PRI 10. The own Pawn takes
// the next slot; other players' actors allocate from kFirstRemoteChannel.
static constexpr uint32_t kPCChannel      = 2;
static constexpr uint32_t kOwnPawnChannel = 11;
static constexpr uint32_t kProxyChannel   = 12;   // AA_PlayerStorageProxy (retail Ch=18)
// MaxBunchBits empirically = 4096 (legit challenge frame: 49B payload = 53
// header bits + 336 BDB + 1 terminator = 390 bits, only fits when BDB is
// RIM(0x1000) — see decode logs).
static constexpr uint32_t kMaxPayloadBits = 4096;
// Cap a storm packet's bit budget so we leave headroom for the terminator
// and stay well under any per-packet limit the client enforces.
static constexpr uint32_t kMaxBunchBits   = 3500;

enum NMT_AA3 : uint8_t {
    NMT_HandshakeStart = 21,
    NMT_HandshakeChallenge = 4,
    NMT_HandshakeResponse = 7,
    NMT_HandshakeComplete = 5,
    NMT_Welcome = 5
};

// Historical hardcoded manifest, kept as a fallback when the engine couldn't
// load a real JSON manifest. The runtime path uses `manifest_->uses` from
// MapManifest. New work should go through the manifest builder, not here.
struct PackageInfo {
    std::string guid;
    std::string name;
    std::string ext;
    uint32_t flags;
    uint32_t gen;
    std::string base;
};

[[maybe_unused]] static const std::vector<PackageInfo> FULL_MANIFEST = {
    {"240780FD4F226E9ED9583EAC13976460", "Core", "u", 44564488, 1, "None"},
    {"0F002C2B458151E8E9B4FCBA1A0B740F", "Engine", "u", 44564488, 1, "None"},
    {"1E3EC5834FD3A796613E08BB6973FE43", "EngineFonts", "upk", 1, 1, "Startup"},
    {"CE4FAEFE4CB339F370A2EAAD3E4F320F", "EngineMaterials", "upk", 1, 1, "Startup"},
    {"8CB9EDAB452DC20C775776A13FA55188", "NodeBuddies", "upk", 1, 1, "Engine"},
    {"8061AE9E439A798FFECE7C9D494FF898", "EditorResources", "upk", 1, 1, "Startup"},
    {"2EA3840740634CAEDEB9FE91A51D3C59", "EngineMeshes", "upk", 1, 1, "Engine"},
    {"B9E809B04AE6BBAB855C52939E1830DA", "SoundNodeVOIP_Test", "", 1, 1, "AA3Baseline"},
    {"4B6ED74E41DCADB427559798E48BA9F1", "EditorMeshes", "upk", 1, 1, "Engine"},
    {"1AA08D324EF52D7F08D2F2AC8987DDF2", "EditorMaterials", "upk", 1, 1, "Engine"},
    {"9F537FD14734DA8D38ABE69348C80560", "EngineResources", "upk", 1, 1, "Startup"},
    {"47CE68C64398ECBD24C078A667C62CCB", "GameFramework", "u", 44564488, 1, "None"},
    {"EF54A6F946F6AEABC0099D8495B278BE", "UnrealScriptTest", "u", 44564488, 1, "None"},
    {"23A74ADD472B162B76AC969B6EBA6E04", "GFxUI", "u", 44564489, 1, "None"},
    {"2AA0EB744954EE22C6D33AB23C9E5738", "RadialMenu", "upk", 1, 1, "AA3Baseline"},
    {"160BDF924D5BD14691E7AD83FCF2BBA9", "AA_UIDefaultSkin", "upk", 0, 1, "AA3Game"},
    {"E1D929BB4CBC253B019512BDC65C3B3D", "AA_UIDerivedSkin", "upk", 0, 1, "AA3Game"},
    {"2F739D4B4170E9105D0C69BDC9EC264D", "OnlineSubsystemAA3", "u", 44564488, 1, "None"},
    {"6E7E92D048CFDA538455CABFEA6CEB1C", "AA3Baseline", "u", 44564489, 1, "None"},
    {"CB06D260448A305119674FB5BC6CC5B2", "Sold_Tech", "upk", 1, 1, "AA3Game"},
    {"7C10681D45107EBC2DC02E9A39889175", "AA_AuthUI", "upk", 0, 1, "AA3Baseline"},
    {"ADFE12634FAA1DD311320792AC8B5137", "AA_FlashBang_Effects_Materials", "upk", 1, 1, "AA3Game"},
    {"B9AAA6C2403C62A50E9FE0A8479B4531", "MasterMaterials", "upk", 1, 1, "Bridge_night_clear"},
    {"7F1CCCF444F3EE19B5ECC6B8A9931EAD", "AA_TC_UIScenes", "upk", 1, 1, "AA3Baseline"},
    {"9F293C7641701B120B91EB8D5C1B6CA1", "AA_UI", "upk", 1, 1, "AA3Baseline"},
    {"6FC5465046C9689EBB841D9B3702D001", "AA_UIFrontEnd", "upk", 1, 1, "AA3Baseline"},
    {"49F4EE344495D9A0CD73B08F9318FD55", "AGP_HUD", "upk", 1, 1, "AA3Game"},
    {"35836BDF4C1067D9A3FE579372C8259A", "Editor_UI", "", 1, 1, "AA3Baseline"},
    {"2B08EB7E4CCE52F2971A37B34A420319", "Medkit", "upk", 1, 1, "AA3Baseline"},
    {"F56EE5E04B653D8695FA34BC376E8937", "Sold_Anims", "upk", 1, 1, "AA3Game"},
    {"5F201B3941BC9126137582A9BBC14EEB", "Mod_Grenade", "upk", 1, 1, "AA3Baseline"},
    {"06C05D74428A1FB2BB2E45A915A33723", "NMEWeap_obran", "upk", 1, 1, "AA3Baseline"},
    {"27C8D33E43CDD783F17326A79B9DB377", "USWeap_AR15", "upk", 1, 1, "AA3Baseline"},
    {"E32A00E8449F2483FDE356B28F5AD9C7", "USWeap_grenades", "upk", 1, 1, "AA3Baseline"},
    {"0DCA4D514A0639255A9012B2A84E7F40", "USWeap_m249", "upk", 1, 1, "AA3Baseline"},
    {"CE5E8A1445F242DC480D04AC059B59F0", "Doors_01", "upk", 1, 1, "Bridge_night_clear"},
    {"A74EEC574D510671BEDD8F809B98FAA8", "S_AudioFX", "upk", 1, 1, "AA3Baseline"},
    {"5A2F99B146DA5A4038C14AB84C210BDB", "Decals_01", "upk", 1, 1, "Bridge_night_clear"},
    {"1C539D5A432B04CDAB3963A9B8C40E4F", "fX", "upk", 1, 1, "Bridge_pathing"},
    {"E1E170594F261DF25A3B4F9447200258", "Sold_Head", "upk", 1, 1, "AA3Baseline"},
    {"51D9A96E4403B5079BB936B5D03F3742", "AA_InGame_Mission_UI", "upk", 1, 1, "AA3Baseline"},
    {"86448C1A43AAD8CF5EAB7FA9A5F2FD3C", "AA_InGame_UI", "upk", 1, 1, "AA3Baseline"},
    {"86F7F2D64DCE807F2A5816A4D454845E", "AA_InGameACP_UI", "upk", 1, 1, "AA3Baseline"},
    {"6C598F6A427BC24C23A8E4873F478721", "AA_LeaderBoard", "upk", 1, 1, "AA3Baseline"},
    {"3902C2F44E4235C784ED4CA6A4FCA0DD", "AA_Main_UI", "upk", 1, 1, "AA3Baseline"},
    {"6232472C4DC1EF2FEAA56D97FF49A073", "AA_Main_UI_MBS", "upk", 1, 1, "AA3Baseline"},
    {"B93BF98245E53DB0936B3E8F17387967", "AA_OutOfGameUI", "upk", 1, 1, "AA3Baseline"},
    {"D1BEE1EE46CC2931E7CEC98D7D846721", "AA_PNB", "upk", 1, 1, "AA3Baseline"},
    {"D4DF361C475117E9A49C31B33F5118DF", "AA_Scaleform_Auth_UI", "upk", 1, 1, "AA3Baseline"},
    {"2DAAA19F4BC40601B77EDFBFD1FD4F00", "AA_Scaleform_TC", "upk", 1, 1, "AA3Baseline"},
    {"C3E05C8F4EE55C38F2BC9ABAE10867EC", "AA_Training_UI", "upk", 1, 1, "AA3Baseline"},
    {"0332E6434B570F8B167DBCA4527D39B2", "ARGH", "upk", 1, 1, "AA3Baseline"},
    {"94443A7D43CB6B7B38B0C48895742395", "TestSAI", "", 1, 1, "AA3Baseline"},
    {"914F21C642888BE1303F97A46DFD7F94", "AA_LensFlare", "upk", 1, 1, "AA3Baseline"},
    {"E187831047B4BDEF278A9EB5EB62C7B9", "AA_NPCTools", "upk", 1, 1, "AA3Baseline"},
    {"ECE343D645D9C67F0A227A851858414A", "Civ_Legs", "upk", 1, 1, "AA3Baseline"},
    {"BA81FA034CF9C7C8C2D4F8815D78804E", "Civ_Torso", "upk", 1, 1, "AA3Baseline"},
    {"613FE75F414433FD4E21B0B82985037D", "Electronics_01", "upk", 1, 1, "Bridge_night_clear"},
    {"4BE9B9D04D4EA0674C2A4EBE5F1F3B21", "Engine_MI_Shaders", "upk", 1, 1, "AA3Baseline"},
    {"EB60A49245FA0A8896DC92B3A80CE313", "ES2_Objects", "upk", 1, 1, "Bridge_inf_classic_large_dev"},
    {"46C9210A4ECACC3C0F843395BE8CE551", "Island_Assets", "", 1, 1, "AA3Baseline"},
    {"ADF749A7423D1291D85224B739065C85", "Mod_Ammo", "upk", 1, 1, "AA3Baseline"},
    {"5DC1207642BF62694F5A4F9BEBE5E432", "Mod_Lights", "upk", 1, 1, "AA3Baseline"},
    {"DEC9C88C4F43591C5D47B097AB4DF599", "Mod_Sights", "upk", 1, 1, "AA3Baseline"},
    {"BF56D2B24D66769F7E598098A1DF07C5", "Mod_Stocks", "upk", 1, 1, "AA3Baseline"},
    {"4535D19B4CA24398CD0DCCB6BA04BF1F", "MT_Common", "upk", 1, 1, "Bridge_night_clear"},
    {"4B5A4DA34520D7EE40BAA99FD444B609", "mt_prop", "upk", 1, 1, "Bridge_night_clear"},
    {"B257BBA240BC655B595A65BC760D64F6", "NMEWeap_Comp", "upk", 1, 1, "AA3Baseline"},
    {"2A89320644C72FFF57C0DAB97FF4CAA4", "NMEWeap_grenades", "upk", 1, 1, "AA3Baseline"},
    {"ED28307E4B4995E48B51E1898986B47E", "Sold_Arms", "upk", 1, 1, "AA3Baseline"},
    {"EA46C8CE41B60948D1EDD9BD6CC36AAC", "Sold_Comp", "upk", 1, 1, "AA3Baseline"},
    {"ABC833674FF10D65931EC7B3C8442CDC", "Sold_Equipment", "upk", 1, 1, "Bridge_night_clear"},
    {"87AF9A8941D99B9B08EE688FE2C377B8", "Sold_Eyewear", "upk", 1, 1, "AA3Baseline"},
    {"A5B7251C44B310501775EA8ED2062C1C", "Sold_Feet", "upk", 1, 1, "AA3Baseline"},
    {"22032E3741BBB265E31494A5E038715C", "Sold_Hands", "upk", 1, 1, "AA3Baseline"},
    {"448E473A41B0D851934476938FBA3BD0", "Sold_HeadGear_Mat", "upk", 1, 1, "AA3Baseline"},
    {"65F29A544B27B9E99599899B3F5FA456", "Sold_Legs", "upk", 1, 1, "AA3Baseline"},
    {"74881DB345CA3946AEAA6780AE3829C5", "Sold_Packs", "upk", 1, 1, "AA3Baseline"},
    {"C5D63A8A45256AC015D5BE8429F442F6", "Sold_Torso", "upk", 1, 1, "AA3Baseline"},
    {"56B717484C25F855D8E8FAB5FFE42177", "Storage_01", "upk", 1, 1, "Bridge_night_clear"},
    {"C6BB54544E7F487A965682947329353A", "SurfMetal_01", "upk", 1, 1, "Bridge_night_clear"},
    {"952A76CB4875B3F4298FA48E5076CABA", "Training_01", "upk", 1, 1, "Bridge_night_clear"},
    {"ECD6C8E7400486CCE39C8CBD22D29EB8", "USWeap_Comp", "upk", 1, 1, "AA3Baseline"},
    {"BB694A6B4361572ACFA520B7A102188F", "ARCDecoration", "upk", 1, 1, "Bridge_night_clear"},
    {"ED4246534A3F6DBA86CA67A2F1EB7875", "AA_MPNPC_Voice", "upk", 1, 1, "AA3Baseline"},
    {"A62AF5DC4B9D03BE3CA395ABDD103130", "AGP_UI", "upk", 1, 1, "AA3Baseline"},
    {"7E3A52C742EAECD3E553B0B0C971FA63", "CubeMaps_01", "upk", 1, 1, "Bridge_night_clear"},
    {"A18E0DE747473436952B90BF6A0BB496", "Detail_01", "upk", 1, 1, "Bridge_night_clear"},
    {"977280DD4E6BE9BAFE0382BE1D9A81D3", "Furniture_01", "upk", 1, 1, "Bridge_night_clear"},
    {"039DD4944FF59298BE1FF78BCB7FC8F3", "MCEditorResources", "", 1, 1, "AA3Baseline"},
    {"1A252ADD44DBC2EFEE9ECBB103465A4E", "MT_CommonBasic", "upk", 1, 1, "Bridge_night_clear"},
    {"960098BA4E83F7808997618799C5FB6D", "mt_commontx", "upk", 1, 1, "Bridge_night_clear"},
    {"8948F99240862FFF442925B7D05591A3", "S_Amb_NonLoop", "upk", 1, 1, "Bridge_night_clear"},
    {"D887E98841AA14255082488F760AFCF6", "S_Char_Foley_Footsteps", "upk", 1, 1, "Bridge_night_clear"},
    {"CE0A6DD649C3ED61AB9567ABEBC07345", "S_Char_Foley_Health", "upk", 1, 1, "AA3Baseline"},
    {"CFBC4F324EEF1498288A5DABF0A8A777", "S_Char_Foley_Movement", "upk", 1, 1, "AA3Baseline"},
    {"655636364227F8A614B7728C83304B3F", "S_Char_VO_Soldier", "upk", 1, 1, "AA3Game"},
    {"24FA187D4FAB98451569CBAB8AA342B7", "S_ES2", "upk", 1, 1, "AA3Baseline"},
    {"259781E7484F42BEAFF549A0DA5B1330", "S_HolidayAudio", "upk", 1, 1, "AA3Baseline"},
    {"D160A4E8493A58DC8A48589C197BF782", "S_Map_Airfield", "upk", 1, 1, "AA3Baseline"},
    {"E42EB3934CEF01299968D0AFAF29D410", "S_Music", "upk", 1, 1, "Bridge_inf_classic_large_dev"},
    {"568C5BE14E3174899BAFAFA8250CBB9C", "S_UI", "upk", 1, 1, "AA3Baseline"},
    {"1DE7590F46411A4489024E8731A43CA1", "S_Weap_Bullet", "upk", 1, 1, "Bridge_night_clear"},
    {"3884322048057ED7C04ADA8E90CA0BCC", "S_Weap_Grenades", "upk", 1, 1, "Bridge_night_clear"},
    {"1FD2C7D44AF2B10C23562AAAF791ADDD", "S_Weap_MG", "upk", 1, 1, "AA3Baseline"},
    {"E7BE91964B4F2E672F396E8C4818C85D", "S_Weap_Rifles", "upk", 1, 1, "AA3Baseline"},
    {"03EF52084D83401088DD29BCEBA40E4C", "Sold_Armor", "upk", 1, 1, "AA3Baseline"},
    {"95239E164248122BA47B7295C5420992", "SurfTerrain_01", "upk", 1, 1, "Bridge_night_clear"},
    {"7713531046F520C29D83F0A206F2C7F4", "USWeap_m67", "upk", 1, 1, "AA3Baseline"},
    {"1016474340B5B65D19369DAE10549441", "Windows_01", "upk", 1, 1, "AA3Baseline"},
    {"F3F610E845811AEEEC8CAF8F273EB91B", "AGP_UI_MP_InGame", "upk", 1, 1, "AA3Game"},
    {"BB680C3845F6B7B2C6D031B08110EF43", "Editor_AISupportIcons", "", 1, 1, "AA3Baseline"},
    {"9A0A9C414DAB93630602F8ADA561643E", "Sky_01", "upk", 1, 1, "Bridge_night_clear"},
    {"EFE1128C4F7ACDF601F5919053E83FAF", "ZOMBIE_Glass", "upk", 1, 1, "AA3Baseline"},
    {"3B211FCD42A2A1E729885BA9A6741B94", "AA3Game", "u", 44564489, 1, "None"},
    {"C22A21314F7E80C76A33338BE28E1DD1", "ZHudTest", "upk", 1, 1, "AA3Game"},
    {"B0DA2C1F4DDAA683073F4C90F793598A", "Leavenworth_01", "upk", 1, 1, "Bridge_night_clear"},
    {"40AEBD95413D1746B783D2B7BD51B579", "S_VoiceShouts", "upk", 1, 1, "AA3Game"},
    {"1D3B2E4B4BA073DDAACDE5853B4E8296", "DefaultUISkin", "upk", 0, 1, "Startup"},
    {"0ECABE284027A273B41399B2119BA422", "SoundModes", "upk", 1, 1, "Startup"},
    {"1CCA7F2949714FA8D3425DA856614151", "EngineSounds", "upk", 1, 1, "Startup"},
    {"C6FD2A31437B62FDAE5AFEAC1F946EAE", "AA_PPE", "upk", 524297, 1, "None"},
    {"33C2536D4D91BC96F13FF38A6B99DAC6", "Vehicles_01", "upk", 1, 1, "Bridge_night_clear"},
    {"B6791D5E4DAC1D08CD08E5980BBB03FE", "Bridge_night_clear", "aa3", 42598409, 1, "None"},
    {"9F928FF848DCFA1292C438B535298A45", "Graffiti_01", "upk", 1, 1, "Bridge_night_clear"},
    {"D41304DF4D3C1A9E7983E3A0806211CD", "Accessories_01", "upk", 1, 1, "Bridge_night_clear"},
    {"BE77651D4D32F4EFFC94FEB6BB3A8F74", "architectureczer_01", "upk", 1, 1, "Bridge_night_clear"},
    {"2543E2824818053A06B6E4815B1E138D", "ArchitectureNEUT_01", "upk", 1, 1, "Bridge_night_clear"},
    {"766AB1D14D3969C3834A9C8D8A65431B", "ARCStructures", "upk", 1, 1, "Bridge_night_clear"},
    {"40B983A6448EF5987111EAA78DCD0894", "Buildings_01", "upk", 1, 1, "Bridge_night_clear"},
    {"6BD63E484ACB4695662AD38714210EAA", "DCRDebris", "upk", 1, 1, "Bridge_night_clear"},
    {"8276BB1E409F94F4F14276B6794EF924", "DCRFoliage", "upk", 1, 1, "Bridge_night_clear"},
    {"1328774942D6D146E4763D82BBAFE182", "EngineVolumetrics", "upk", 1, 1, "Bridge_night_clear"},
    {"1AF07F374FF7DD662AE059B6D3F4969B", "Foliage_01", "upk", 1, 1, "Bridge_night_clear"},
    {"7EFBA7714EE57D3ABDF3E5AAF4899097", "machinery_01", "upk", 1, 1, "Bridge_night_clear"},
    {"66B5579F463DD511ACC89CAF98A764E9", "MT_Architecture", "upk", 1, 1, "Bridge_night_clear"},
    {"23ADB35B423FA7F007C596A4E66F179E", "mt_base", "upk", 1, 1, "Bridge_night_clear"},
    {"381BE6B149F64DCA51EDC99F8F38A388", "Plumbing_01", "upk", 1, 1, "Bridge_night_clear"},
    {"D251CE2A4E7A41E3010E8C9DBB5135A0", "SurfBrick_02", "upk", 1, 1, "Bridge_night_clear"},
    {"0C7D3EC24FE0F0B1BFA52CB5C1594BC2", "SurfConcrete_01", "upk", 1, 1, "Bridge_night_clear"},
    {"8771B43146F2F906AC31E5994E334058", "SurfConcrete_02", "upk", 1, 1, "Bridge_night_clear"},
    {"0A7C4202462024DCF4273B908594D119", "SurfConcrete_03", "upk", 1, 1, "Bridge_night_clear"},
    {"F49F62684CEF6CBA7BC52EA34FE10C48", "SurfConcrete_06", "upk", 1, 1, "Bridge_night_clear"},
    {"CF67B3BB43433215FC15C48DF0A714BF", "SurfGlass_01", "upk", 1, 1, "Bridge_night_clear"},
    {"1A7C770A4551A2822022C2B476AC3824", "SurfPlaster_01", "upk", 1, 1, "Bridge_night_clear"},
    {"96DB0F834FDFC7D675A68BA9C94755A9", "SurfPlaster_02", "upk", 1, 1, "Bridge_night_clear"},
    {"998FC4294BBB607D6B8F02A37092DECE", "SurfTile_03", "upk", 1, 1, "Bridge_night_clear"},
    {"157B197844D39D864710D69F1D60325B", "SurfWood_01", "upk", 1, 1, "Bridge_night_clear"},
    {"6B71FCF24528ADC832E8668DE8BFE4E6", "SYSDecoration", "upk", 1, 1, "Bridge_night_clear"},
    {"8ACA7714433241822DFC0B8039A7BF47", "Vehicles_02", "upk", 1, 1, "Bridge_night_clear"},
    {"D158C47B4CD4361644BDF588606E0AC8", "CVRLarge", "upk", 1, 1, "Bridge_night_clear"},
    {"A2E9730F4D6760E2312C468BACAEDB82", "CVRSmall", "upk", 1, 1, "Bridge_night_clear"},
    {"B0AAF6A04F5B459FBEF3C3917BD82D51", "DCRFurniture", "upk", 1, 1, "Bridge_night_clear"},
    {"DF36549D4E9B8422700832B9E9CEBAB8", "DCRLights", "upk", 1, 1, "Bridge_night_clear"},
    {"98D33BA04F628AE71BC6F99142B48618", "DCRMasonry", "upk", 1, 1, "Bridge_night_clear"},
    {"F9A9580A4BDC42586A16BF96A2863865", "SYS_Sidewalks", "upk", 1, 1, "Bridge_night_clear"},
    {"5DB8B9DA4CB27313627A9B90D3201B6E", "SYSExterior", "upk", 1, 1, "Bridge_night_clear"},
    {"9DF554E541CCD20565C78D89DEAE9FF7", "SYSIndustrial", "upk", 1, 1, "Bridge_night_clear"},
    {"CA8E675A4E783E4735ACC5A2A819C681", "SYSInteractive", "upk", 1, 1, "Bridge_night_clear"},
    {"678EB5174520B6245244869D694310C9", "VH_T72", "upk", 1, 1, "Bridge_night_clear"},
    {"D2FAD7ED4480D2761D584085FE29BA43", "DCR_Water", "upk", 1, 1, "Bridge_night_clear"},
    {"6B900FB3473D45BA11F89D81E987C1BF", "DM_PipelineProjectors", "upk", 1, 1, "Bridge_night_clear"},
    {"E58ACEDE43856E03C41F67B17BA80862", "FX_Placeable", "upk", 1, 1, "Bridge_night_clear"},
    {"9FA275DA4F435A8FC1431EA2220A1AF6", "S_Amb_Loop", "upk", 1, 1, "Bridge_night_clear"},
    {"D3C4AED0417E18CD78320AA589EC6681", "SurfGrass_04", "upk", 1, 1, "Bridge_night_clear"},
    {"98284319444E6C7E99EE0798958E7765", "Bridge_Water", "upk", 1, 1, "Bridge_night_clear"},
    {"B5EA10694875D3479FC8A6A11828DC48", "Bridge_inf_classic_large_dev", "aa3", 42598409, 1, "None"},
    {"8191350D4C9CDEA174F169A78E8B7A0F", "Bridge_pathing", "aa3", 42598409, 1, "None"},
};

// Write a single bunch's metadata (no payload). The 1-bit "bIsAck=0" tag
// that distinguishes a bunch from an ack entry is written here. Caller
// supplies the bunch payload bit-count and follows with that many bits.
static void WriteBunchHeader(util::BitWriter& w,
                             uint32_t ch_index,
                             uint32_t ch_type,
                             uint32_t ch_sequence,
                             uint32_t bunch_data_bits,
                             bool bOpen,
                             bool bReliable = true,
                             bool bClose = false) {
    w.WriteBit(0);                                        // bIsAck = 0 (bunch)
    w.WriteBit((bOpen || bClose) ? 1 : 0);                // bHasOpenClose
    if (bOpen || bClose) {
        w.WriteBit(bOpen ? 1 : 0);                        // bOpen
        w.WriteBit(bClose ? 1 : 0);                       // bClose
    }
    w.WriteBit(bReliable ? 1 : 0);                        // bReliable
    w.WriteIntMax(ch_index, 0x800);                       // ChIndex
    if (bReliable) {
        w.WriteIntMax(ch_sequence, 0x400);                // ChSequence (reliable path)
        w.WriteIntMax(ch_type, 0x8);                      // ChType  (reliable path)
    } else if (bOpen) {
        w.WriteIntMax(ch_type, 0x8);                      // ChType  (unreliable open)
    }
    w.WriteIntMax(bunch_data_bits, kMaxPayloadBits);      // BunchDataBits
}

// Header-only cost in bits of a bunch with the given bOpen flag. Used to
// budget packets before deciding to flush. Exact, not a worst-case.
static uint32_t BunchHeaderBits(bool bOpen) {
    // 1 (bIsAck) + 1 (HasOC) + (bOpen ? 2 : 0) + 1 (bReliable)
    // + 11 (ChIndex RIM(0x800)) + 10 (ChSeq RIM(0x400)) + 3 (ChType RIM(8))
    // + 12 (BDB RIM(0x1000))
    return 1 + 1 + (bOpen ? 2 : 0) + 1 + 11 + 10 + 3 + 12;
}

// Emit an ACK entry into the packet (bIsAck=1 + AckedPacketID via RIM(0x4000)).
// Per-element, not a "bunch" — the bIsAck bit here is the *element-discrim*
// bit, distinct from the bIsAck-of-bunch flag that WriteBunchHeader writes.
static void AppendAck(util::BitWriter& w, uint32_t acked_packet_id) {
    w.WriteBit(1);
    w.WriteIntMax(acked_packet_id & 0x3FFF, 0x4000);
}

// Cost in bits of one FString record inside a bunch payload: 4-byte little-
// endian length followed by the text bytes including null terminator.
// (No leading opcode byte — confirmed from legit pcap frame 4 bunch[1]: each
// USES entry starts with the length directly.)
static uint32_t FStringBits(const std::string& s) {
    return 32 + static_cast<uint32_t>((s.length() + 1) * 8);
}

// Append one FString record at the writer's current bit position. Caller is
// responsible for having written a bunch header that promised this many bits.
static void WriteFString(util::BitWriter& w, const std::string& s) {
    w.WriteInt(static_cast<uint32_t>(s.length() + 1));   // LE int32, includes null
    for (char c : s) w.WriteByte(static_cast<uint8_t>(c));
    w.WriteByte(0);
}

// Wire limits (UnNet.h): PacketSequence and ack ids wrap at MAX_PACKETID,
// ChSequence at MAX_CHSEQUENCE. RELIABLE_BUFFER caps unacked reliable
// bunches per channel.
static constexpr int32_t kMaxPacketId = 0x4000;
static constexpr int32_t kMaxChSequence = 0x400;
static constexpr uint32_t kReliableBuffer = 128;

// UE3 MakeRelative/BestSignedDifference (UnNet.h): unwrap a wire value to
// the integer nearest `reference`.
static int32_t MakeRelative(int32_t value, int32_t reference, int32_t max) {
    return reference + (((value - reference + max / 2) & (max - 1)) - max / 2);
}

// Parse a whole packet into its acks and bunches (payloads copied out). On
// a malformed element, keeps what parsed cleanly before it and returns false.
static bool ParsePacket(const uint8_t* data, uint32_t len, uint32_t& packet_seq,
                        std::vector<uint32_t>& acks, std::vector<InBunch>& bunches) {
    util::BitReader r = util::BitReader::FromPacket(data, len);
    if (r.BitsRemaining() < 14) return false;
    packet_seq = r.ReadIntMax(kMaxPacketId);
    try {
    while (r.BitsRemaining() > 0) {
        if (r.ReadBit()) {
            if (r.BitsRemaining() < 14) return false;
            acks.push_back(r.ReadIntMax(kMaxPacketId));
            continue;
        }
        InBunch b;
        if (r.BitsRemaining() < 1) return false;
        if (r.ReadBit()) {
            b.open = r.ReadBit();
            b.close = r.ReadBit();
        }
        b.reliable = r.ReadBit();
        b.ch_index = r.ReadIntMax(0x800);
        if (b.reliable) {
            b.ch_seq = r.ReadIntMax(kMaxChSequence);
            b.ch_type = r.ReadIntMax(8);
        } else if (b.open) {
            b.ch_type = r.ReadIntMax(8);
        }
        b.bits = r.ReadIntMax(kMaxPayloadBits);
        if (b.bits > r.BitsRemaining()) return false;
        b.data.assign((b.bits + 7) / 8, 0);
        for (uint32_t i = 0; i < b.bits; ++i)
            if (r.ReadBit()) b.data[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        bunches.push_back(std::move(b));
    }
    } catch (const std::out_of_range&) {
        return false;   // element header ran past the terminator
    }
    return true;
}

static uint64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

ProtocolHandler::ProtocolHandler(SocketManager& socket,
                                 const engine::MapManifest* manifest,
                                 game::World* world)
    : socket_(socket), manifest_(manifest), world_(world) {
}

ProtocolHandler::~ProtocolHandler() {
}

void ProtocolHandler::OnDataReceived(uint32_t client_id, const uint8_t* data, uint32_t len) {
    if (len < 2) return;   // PacketSequence alone is 14 bits
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    ClientSession& sess = it->second;

    uint32_t packet_seq = 0;
    std::vector<uint32_t> acks;
    std::vector<InBunch> bunches;
    bool clean = ParsePacket(data, len, packet_seq, acks, bunches);
    sess.last_packet_id = packet_seq;
    sess.last_receive_us = NowUs();

    LOG_INFO("RX client=%u len=%u seq=%u state=%d",
             client_id, len, packet_seq, (int)sess.state);

    // Acks first, in every state: the client acks our challenge and storm
    // packets while still mid-handshake (doc sec 25).
    for (uint32_t a : acks) ReceivedAck(sess, a);
    ResendLost(client_id, sess, sess.last_receive_us);

    // The state machine still uses byte-length heuristics — those came
    // from the legit pcap and remain valid as a coarse classifier even
    // though the bit-level structure is different from what we used to
    // think. Stage 1 = NMT_Hello (40-70 byte range), Stage 3 = the
    // Hello + HandshakeResponse bundle (90-140 byte range).
    bool stage1 = sess.state == ConnectionState::HANDSHAKE_HELLO && len >= 40 && len <= 70;
    bool stage3 = sess.state == ConnectionState::HANDSHAKE_CHALLENGE && len >= 90 && len <= 140;
    if (stage1 || stage3) {
        // Owe an ack for these too. Retail acks the HELLO (packet 0) in its
        // first storm packet; acking only the bundle made the real client
        // NAK packet 0 and resend its HELLO (doc sec 25).
        sess.queued_acks.push_back(packet_seq);
        // These Ch=0 bunches (HELLO = ChSeq 1, NETSPEED/login = 2 in retail)
        // are consumed here, not by ProcessPostStormPacket, so record them
        // as received: the client's HAVE stream continues at the next
        // ChSequence and would otherwise wait forever for a "missing" 1..2.
        if (clean) {
            for (const InBunch& b : bunches) {
                if (!b.reliable || b.ch_index != kControlChIndex || b.ch_type != kControlChType)
                    continue;
                int32_t& last = sess.in_reliable[kControlChIndex];
                int32_t seq = MakeRelative(static_cast<int32_t>(b.ch_seq), last, kMaxChSequence);
                if (seq > last) last = seq;
            }
        }
    }
    if (stage1) {
        LOG_INFO(">>> Stage 1: Hello detected (len=%u). Sending Challenge.", len);
        SendHandshakeChallenge(client_id);
        return;
    }
    if (stage3) {
        LOG_INFO(">>> Stage 3: 116-byte bundle detected (len=%u). Sending Storm + Welcome.", len);
        // The bundle carries "LOGIN RESPONSE=0 URL=<map>?Name=<soldier>?..."; retail
        // uses that name for PRI.PlayerName/sAuthSoldierName (doc sec 27).
        for (const InBunch& b : bunches) {
            if (b.ch_index != kControlChIndex) continue;
            std::string text(reinterpret_cast<const char*>(b.data.data()), b.bits / 8);
            size_t at = text.find("LOGIN ");
            size_t name = at == std::string::npos ? at : text.find("?Name=", at);
            if (name == std::string::npos) continue;
            name += 6;
            size_t end = text.find_first_of(std::string("?\0", 2), name);
            std::string soldier = text.substr(name, end == std::string::npos ? std::string::npos
                                                                             : end - name);
            if (!soldier.empty()) {
                sess.player_name = soldier;
                LOG_INFO("Client %u logs in as '%s'", client_id, soldier.c_str());
            }
        }
        // reliable_out_seq is already 2 here — SendHandshakeChallenge
        // consumed ChSeqRaw=1 on the control channel via the same
        // increment-on-send pattern SendStormAndWelcome uses below.
        SendStormAndWelcome(client_id);
        ScheduleKeepalives(client_id, NowUs());
        // Advance state so the size-based Stage 3 trigger above can't fire
        // again. Without this the client's frame-78-equivalent (a 105-byte
        // re-ack of our storm) lands inside [90,140] and we re-run the
        // entire storm — the loop the user observed in TestComms4.
        sess.state = ConnectionState::AUTHENTICATING;
        return;
    }

    // Post-storm: a stream of reliable Ch=0/ChType=1 bunches whose payload
    // is concatenated FString records: "HAVE GUID=<32hex> GEN=1" per
    // package, terminating with "JOIN"; then actor-channel RPCs.
    if (sess.state == ConnectionState::AUTHENTICATING ||
        sess.state == ConnectionState::ESTABLISHED) {
        if (!clean)
            LOG_WARN("RX client=%u seq=%u: malformed element; processing the %zu "
                     "bunch(es) before it", client_id, packet_seq, bunches.size());
        try {
            ProcessPostStormPacket(client_id, bunches, packet_seq);
        } catch (const std::exception& e) {
            LOG_DEBUG("Protocol error: %s", e.what());
        }
    }
}

void ProtocolHandler::OnClientConnected(uint32_t client_id, bool connected) {
    if (connected) {
        LOG_INFO("Client %u connected", client_id);
        ClientSession s;
        s.client_id = client_id;
        s.state = ConnectionState::HANDSHAKE_HELLO;
        s.version = 0;
        s.in_seq = 0;
        // Server's outgoing PacketSequence starts at 0 — Challenge=0, then
        // storm packets 1..N, then post-storm pings.
        s.out_seq = 0;
        s.reliable_out_seq = 1;
        s.last_packet_id = 0;
        s.have_count = 0;
        s.join_seen = false;
        s.actor_burst_sent = false;
        s.next_keepalive_us = 0;
        sessions_[client_id] = s;
    } else {
        auto it = sessions_.find(client_id);
        if (it != sessions_.end())
            LOG_INFO("Client %u disconnected (reliable: %u resent, %u unacked, "
                     "%u duplicate + %u out-of-order received)", client_id,
                     it->second.stat_resent, (unsigned)it->second.out_rec.size(),
                     it->second.stat_dup_in, it->second.stat_queued_in);
        else
            LOG_INFO("Client %u disconnected", client_id);
        if (stats_ && world_) {
            if (const game::ServerPlayerController* pc = world_->FindPlayerController(client_id))
                stats_->OnPlayerLeft(*pc, static_cast<uint64_t>(std::time(nullptr)));
        }
        if (world_) world_->RemovePlayerController(client_id);
        sessions_.erase(client_id);
    }
}

void ProtocolHandler::HandleControlMessage(uint32_t client_id, uint8_t nmt_type, util::BitReader& reader) {
}

void ProtocolHandler::SendHandshakeChallenge(uint32_t client_id) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;

    // Built from the confirmed packet grammar, not replayed. Decoding the
    // retail Frame 2 capture (GamePlayPCAP_Legit_Server.pcapng, challenge
    // "410E8D72") against the grammar at the top of this file shows it is
    // nothing but a plain reliable Ch=0/ChType=1 bunch carrying one FString
    // ("CHALLENGE VER=4206 CHALLENGE=<hex>") — no ack element, bOpen=false —
    // byte-for-byte the same shape as the USES/WELCOME bunches
    // SendStormAndWelcome already builds this way. Only the challenge value
    // itself is server-chosen (rand()); framing is derived from session state.
    uint32_t challenge_val = static_cast<uint32_t>(rand());
    char hex_buf[9];
    snprintf(hex_buf, sizeof(hex_buf), "%08X", challenge_val);

    char text_buf[64];
    snprintf(text_buf, sizeof(text_buf), "CHALLENGE VER=4206 CHALLENGE=%s", hex_buf);
    std::string challenge_str = text_buf;

    uint32_t seq = it->second.out_seq;
    uint32_t ch_seq = it->second.reliable_out_seq++;
    util::BitWriter b;
    WriteBunchHeader(b, kControlChIndex, kControlChType, ch_seq,
                     FStringBits(challenge_str), /*bOpen=*/false);
    WriteFString(b, challenge_str);
    SendBunch(client_id, it->second, std::move(b), kControlChIndex, ch_seq, AckMode::kNone);
    LOG_INFO("Sent HandshakeChallenge 'CHALLENGE VER=4206 CHALLENGE=%s' seq=%u ChSeq=%u",
             hex_buf, seq, ch_seq);

    it->second.state = ConnectionState::HANDSHAKE_CHALLENGE;
}

// Build the next storm-packet bunch payload. Greedily packs FString records
// (USES/DLMGR/WELCOME) into one bunch until adding the next record would
// blow the packet budget. The bunch's BunchDataBits is bit-exact.
//
// Layout (matches legit pcap frame 4 bunch[1]):
//   <int32 LE length_1><text_1 + \0><int32 LE length_2><text_2 + \0>...
// No leading opcode byte. One bunch per packet, ChType=1 (control), bOpen=0.
void ProtocolHandler::SendStormAndWelcome(uint32_t client_id) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    if (!manifest_) {
        LOG_ERROR("SendStormAndWelcome: no manifest loaded; aborting handshake "
                  "for client=%u", client_id);
        return;
    }

    // Build the ordered FString queue from the loaded manifest:
    //   USES_0, USES_1, ..., DLMGR (if non-empty), WELCOME.
    std::vector<std::string> queue;
    queue.reserve(manifest_->uses.size() + 2);
    for (const auto& u : manifest_->uses) {
        queue.push_back(u.FormatUsesLine());
    }
    if (!manifest_->dlmgr.empty()) queue.push_back(manifest_->dlmgr);
    queue.push_back(manifest_->FormatWelcomeLine());

    const uint32_t bunch_overhead_bits = BunchHeaderBits(/*bOpen=*/false);
    // Per-packet budget for the BUNCH PAYLOAD (excluding header/terminator/ack).
    // kMaxBunchBits caps total packet bits; subtract everything else we know
    // about: PacketSeq RIM(0x4000) reads up to 14 bits, ack-element ~15 bits,
    // bunch header, and 1 terminator bit.
    const uint32_t packet_fixed_bits = 14 + 15 + bunch_overhead_bits + 1;
    if (kMaxBunchBits <= packet_fixed_bits) return;          // misconfigured
    const uint32_t max_bunch_payload_bits = kMaxBunchBits - packet_fixed_bits;

    size_t i = 0;
    while (i < queue.size()) {
        // 1) Decide which queue entries fit in this packet's single bunch.
        uint32_t bunch_payload_bits = 0;
        size_t j = i;
        while (j < queue.size()) {
            uint32_t s_bits = FStringBits(queue[j]);
            if (bunch_payload_bits + s_bits > max_bunch_payload_bits) break;
            bunch_payload_bits += s_bits;
            ++j;
        }
        if (j == i) {
            // A single entry exceeds the budget — emit it alone, oversized.
            // Better to overrun than to stall the storm. Should not happen
            // for sane USES lines (~90B = 720 bits ≪ 3500-cap budget).
            bunch_payload_bits = FStringBits(queue[i]);
            j = i + 1;
        }

        // 2) Build the packet: PacketSequence + ack + 1 bunch + terminator.
        uint32_t seq = it->second.out_seq;
        uint32_t ch_seq = it->second.reliable_out_seq++;
        util::BitWriter b;
        WriteBunchHeader(b, kControlChIndex, kControlChType, ch_seq,
                         bunch_payload_bits, /*bOpen=*/false);
        for (size_t k = i; k < j; ++k) WriteFString(b, queue[k]);
        SendBunch(client_id, it->second, std::move(b), kControlChIndex, ch_seq);
        LOG_INFO("Sent Storm Packet seq=%u ChSeq=%u entries=%zu..%zu",
                 seq, ch_seq, i, j - 1);

        i = j;
    }
}

// Build a packet that's just `PacketSequence` + N ACKs + terminator.
// Used for the post-storm trailing ack packets and the periodic ping
// pattern observed in the legit pcap (frames 61/62/63 of TestComms3).
static std::vector<uint8_t> BuildAckOnlyPacket(uint32_t packet_seq,
                                               const std::vector<uint32_t>& acks) {
    util::BitWriter w;
    w.WriteIntMax(packet_seq, 0x4000);
    for (uint32_t a : acks) AppendAck(w, a);
    w.WriteTerminator();
    return w.GetData();
}

void ProtocolHandler::ScheduleKeepalives(uint32_t client_id, uint64_t now_us) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    auto& s = it->second;

    // Mirror the legit server's "trailing" packets after a storm:
    //   t+200ms : PacketSequence + 2 acks
    //   t+400ms : PacketSequence + 1 ack
    //   t+600ms : PacketSequence-only ping
    // Each step uses the next PacketSequence and acks the client's last
    // known PacketSequence (the bundle / NMT_Hello).
    auto enqueue = [&](uint64_t delay_ms, std::vector<uint8_t> bytes) {
        s.pending.push_back(PendingSend{ now_us + delay_ms * 1000, std::move(bytes) });
    };
    enqueue(200, BuildAckOnlyPacket(s.out_seq++,
                                    { 0u, s.last_packet_id }));
    enqueue(400, BuildAckOnlyPacket(s.out_seq++,
                                    { s.last_packet_id }));
    enqueue(600, BuildAckOnlyPacket(s.out_seq++, {}));
}

uint32_t ProtocolHandler::TransmitPacket(uint32_t client_id, ClientSession& sess,
                                         const util::BitWriter* bunch, AckMode acks) {
    uint32_t seq = sess.out_seq++;
    util::BitWriter w;
    w.WriteIntMax(seq, kMaxPacketId);
    if (acks == AckMode::kQueued) {
        // UE3 PurgeAcks + SendAck: last packet's acks once more, then the new
        // ones; after this packet the new ones become next packet's resends.
        // Bounded so a long stall can't build an oversized packet.
        constexpr size_t kMaxAcks = 64;
        size_t n = 0;
        for (uint32_t a : sess.resend_acks)
            if (n++ < kMaxAcks / 2) AppendAck(w, a);
        n = 0;
        size_t skip = sess.queued_acks.size() > kMaxAcks / 2
                          ? sess.queued_acks.size() - kMaxAcks / 2 : 0;
        for (uint32_t a : sess.queued_acks)
            if (n++ >= skip) AppendAck(w, a);
        sess.resend_acks.swap(sess.queued_acks);
        sess.queued_acks.clear();
    }
    if (bunch) w.AppendBits(*bunch);
    w.WriteTerminator();
    socket_.SendToClient(client_id, w.GetData().data(), (w.GetBitCount() + 7) / 8);
    return seq;
}

void ProtocolHandler::SendBunch(uint32_t client_id, ClientSession& sess,
                                util::BitWriter bunch, uint32_t ch_index,
                                uint32_t ch_seq, AckMode acks) {
    uint32_t packet_id = TransmitPacket(client_id, sess, &bunch, acks);
    if (ch_seq == 0) return;   // unreliable: fire and forget
    OutReliableBunch rec;
    rec.ch_index = ch_index;
    rec.ch_seq = ch_seq;
    rec.packet_id = packet_id;
    rec.sent_us = NowUs();
    rec.bunch = std::move(bunch);
    sess.out_rec.push_back(std::move(rec));
    sess.out_rec_per_channel[ch_index]++;
}

void ProtocolHandler::FlushAcks(uint32_t client_id, ClientSession& sess) {
    if (sess.queued_acks.empty() && sess.resend_acks.empty()) return;
    TransmitPacket(client_id, sess, nullptr, AckMode::kQueued);
}

void ProtocolHandler::ReceivedAck(ClientSession& sess, uint32_t wire_ack) {
    int32_t ack = MakeRelative(static_cast<int32_t>(wire_ack), sess.out_ack_packet_id,
                               kMaxPacketId);
    if (ack > sess.out_ack_packet_id) sess.out_ack_packet_id = ack;
    for (auto r = sess.out_rec.begin(); r != sess.out_rec.end();) {
        if (static_cast<int32_t>(r->packet_id) == ack) {
            if (--sess.out_rec_per_channel[r->ch_index] == 0)
                sess.out_rec_per_channel.erase(r->ch_index);
            r = sess.out_rec.erase(r);
        } else {
            ++r;
        }
    }
}

void ProtocolHandler::ResendLost(uint32_t client_id, ClientSession& sess,
                                 uint64_t now_us) {
    // A packet older than the newest one the client acked, still unacked,
    // was lost: resend its reliable bunches (UNetConnection::ReceivedPacket's
    // virtual NAKs + "missed NAK" sweep). The timeout fallback covers a loss
    // with nothing newer acked -- e.g. a lost CHALLENGE, after which the
    // client only retransmits its HELLO: past the deadline (1s, backing off
    // to 8s), once the client has sent 3 more packets without acking it.
    // Silence alone never triggers it: the client may be loading the map,
    // and a client that just resumed sends before it has read its backlog.
    for (auto& r : sess.out_rec) {
        bool lost = static_cast<int32_t>(r.packet_id) < sess.out_ack_packet_id;
        if (!lost) {
            uint64_t timeout = 1'000'000ull << std::min<uint32_t>(r.resends, 3);
            if (now_us >= r.sent_us + timeout) r.late_rx++;
            lost = r.late_rx >= 3;
        }
        if (!lost) continue;
        uint32_t old_id = r.packet_id;
        r.packet_id = TransmitPacket(client_id, sess, &r.bunch,
                                     sess.state == ConnectionState::AUTHENTICATING ||
                                     sess.state == ConnectionState::ESTABLISHED
                                         ? AckMode::kQueued : AckMode::kNone);
        r.sent_us = now_us;
        r.late_rx = 0;
        r.resends++;
        sess.stat_resent++;
        LOG_INFO("Resent reliable bunch client=%u ch=%u ChSeq=%u (packet %u -> %u, "
                 "resend #%u, newest acked %d)", client_id, r.ch_index, r.ch_seq,
                 old_id, r.packet_id, r.resends, sess.out_ack_packet_id);
    }
}

bool ProtocolHandler::ChannelReady(const ClientSession& sess, uint32_t ch_index) const {
    auto it = sess.out_rec_per_channel.find(ch_index);
    return it == sess.out_rec_per_channel.end() || it->second < kReliableBuffer - 8;
}

void ProtocolHandler::ProcessPostStormPacket(uint32_t client_id,
                                             std::vector<InBunch>& bunches,
                                             uint32_t packet_seq) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    auto& sess = it->second;

    // UE3 acks every packet (UNetConnection::ReceivedPacket -> SendAck); an
    // ack we skip reads to the client as a lost packet, and it resends any
    // reliable bunch that was in it.
    sess.queued_acks.push_back(packet_seq);
    bool any_reliable_bunch = false;

    for (InBunch& b : bunches) {
        if (!b.reliable) {
            // Unreliable bunches are never queued (UChannel::ReceivedRawBunch).
            DispatchBunch(client_id, sess, b);
            continue;
        }
        any_reliable_bunch = true;
        int32_t& last = sess.in_reliable[b.ch_index];
        int32_t seq = MakeRelative(static_cast<int32_t>(b.ch_seq), last, kMaxChSequence);
        if (seq <= last) {
            // A retransmission of something already processed (our ack for
            // it was lost). Processing it again would replay the RPC.
            sess.stat_dup_in++;
            LOG_DEBUG("RX seq=%u ch=%u ChSeq=%d: duplicate (last %d), dropped",
                      packet_seq, b.ch_index, seq, last);
            continue;
        }
        if (seq != last + 1) {
            auto& q = sess.in_rec[b.ch_index];
            if (q.size() < kReliableBuffer && !q.count(seq)) {
                q.emplace(seq, std::move(b));
                sess.stat_queued_in++;
            }
            LOG_DEBUG("RX seq=%u ch=%u ChSeq=%d: ahead of %d, queued",
                      packet_seq, b.ch_index, seq, last + 1);
            continue;
        }
        DispatchBunch(client_id, sess, b);
        last = seq;
        // Release anything that was waiting on this one.
        auto qit = sess.in_rec.find(b.ch_index);
        while (qit != sess.in_rec.end() && !qit->second.empty() &&
               qit->second.begin()->first == last + 1) {
            InBunch next = std::move(qit->second.begin()->second);
            qit->second.erase(qit->second.begin());
            DispatchBunch(client_id, sess, next);
            last++;
        }
    }

    // Ack reliable traffic immediately: the client retransmits its HAVE
    // bunches until each packet is acked (TestComms4 frames 78→79).
    // Everything else is acked by the next packet we send anyway, or by
    // Tick's ack flush.
    if (any_reliable_bunch) FlushAcks(client_id, sess);

    if (sess.join_seen && sess.state != ConnectionState::ESTABLISHED) {
        sess.state = ConnectionState::ESTABLISHED;
        if (!sess.actor_burst_sent) {
            OpenInitialActorChannels(client_id);
            sess.actor_burst_sent = true;
        }
    }
}

void ProtocolHandler::DispatchBunch(uint32_t client_id, ClientSession& sess,
                                    const InBunch& b) {
    util::BitReader reader(b.data.data(), b.bits);
    // Only Ch=0 ChType=1 carries the text-control FString stream.
    if (b.ch_index == kControlChIndex && b.ch_type == kControlChType) {
        // Parse concatenated FString records: int32 LE length (includes
        // null terminator) + length bytes (last byte must be 0).
        const std::vector<uint8_t>& payload = b.data;
        uint32_t pos = 0;
        const uint32_t payload_byte_len = b.bits / 8;
        while (pos + 4 <= payload_byte_len) {
            uint32_t slen = static_cast<uint32_t>(payload[pos]) |
                            (static_cast<uint32_t>(payload[pos + 1]) << 8) |
                            (static_cast<uint32_t>(payload[pos + 2]) << 16) |
                            (static_cast<uint32_t>(payload[pos + 3]) << 24);
            if (slen < 1 || slen > 512) break;
            if (pos + 4 + slen > payload_byte_len) break;
            if (payload[pos + 4 + slen - 1] != 0) break;

            std::string text(reinterpret_cast<const char*>(&payload[pos + 4]),
                             slen - 1);
            pos += 4 + slen;

            if (text == "JOIN") {
                sess.join_seen = true;
                LOG_INFO(">>> JOIN received from client=%u after %u HAVE record(s) — "
                         "handshake fully complete on client side",
                         client_id, sess.have_count);
            } else if (text.rfind("HAVE GUID=", 0) == 0) {
                sess.have_count++;
                LOG_DEBUG("RX HAVE #%u: %s", sess.have_count, text.c_str());
            } else {
                LOG_INFO("RX text on Ch=0: %s", text.c_str());
            }
        }
    } else if (b.ch_type == kActorChType ||
               (!b.reliable && !b.open && sess.channel_max_index.count(b.ch_index))) {
        // Unreliable, non-open bunches carry no ChType on the wire; the
        // channel's type is whatever it was opened as. Every channel in
        // channel_max_index is an actor channel we opened. Retail sends
        // ALL movement this way (ServerMove/DualServerMove/
        // ServerSetSpectatorLocation are unreliable RPCs — doc sec 21).
        // The reader spans exactly this bunch's BunchDataBits, so a partial
        // decode can't run into the next bunch (doc sec 10.3).
        try {
            DecodeActorBunch(client_id, b.ch_index, reader, b.bits, b.open);
        } catch (const std::exception& e) {
            LOG_DEBUG("RX ch=%u: actor bunch decode ran off its end: %s", b.ch_index, e.what());
        }
    } else {
        LOG_DEBUG("RX bunch ch=%u O=%d C=%d R=%d ChSeq=%u ChType=%u BDB=%u (skipped)",
                  b.ch_index, b.open ? 1 : 0, b.close ? 1 : 0,
                  b.reliable ? 1 : 0, b.ch_seq, b.ch_type, b.bits);
    }
}

// Decodes client->server RPCs on the PlayerController channel via the
// table in network/rpc_decode.cpp (ServerMove family, spectator location,
// ping, aim offset, sprint/sights, ServerViewSelf, ServerReturnToParty).
// Parameter lists come from raw UProperty metadata (`--struct-layout` on
// the Function export, CPF_Parm-filtered — doc sec 18/20), so AA3's
// decompiler corruption of function bodies doesn't matter. Validated
// against ~62k RPCs in the retail gameplay pcap (doc sec 20).
void ProtocolHandler::DecodeActorBunch(uint32_t client_id, uint32_t ch_index,
                                       util::BitReader& reader, uint32_t bdb,
                                       bool b_open) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    auto& sess = it->second;

    if (b_open) {
        // We open every actor channel ourselves; a client-opened one isn't
        // a case this server handles.
        LOG_DEBUG("RX Ch=%u: client-opened actor channel (unhandled)", ch_index);
        return;
    }

    auto max_it = sess.channel_max_index.find(ch_index);
    if (max_it == sess.channel_max_index.end()) {
        LOG_DEBUG("RX Ch=%u: actor bunch on a channel with no known class "
                 "table (unhandled)", ch_index);
        return;
    }
    uint32_t max_index = max_it->second;
    uint32_t bunch_end = reader.GetPos() + bdb;

    // Inventory manager: InventoryManager.ServerSetCurrentWeapon(DesiredWeapon)
    // (RepIndex 25). The client repeats it until the server answers, as
    // retail does ~10ms later: ClientNotifyChangedWeapon on the PC channel,
    // ClientNotifyRateOfFire on the weapon (frames 375/376/414; doc sec 30).
    for (const game::ServerRoundActor* a : sess.data_actors) {
        if (a->class_name != "AA_InventoryManager") continue;
        auto ch_it = sess.actor_channel.find(a);
        if (ch_it == sess.actor_channel.end() || ch_it->second != ch_index) continue;
        constexpr uint32_t kServerSetCurrentWeapon = 25;
        if (reader.ReadIntMax(a->max_index) != kServerSetCurrentWeapon) return;
        uint32_t weapon_ch = 0;
        if (reader.ReadBit() && reader.ReadBit()) weapon_ch = reader.ReadIntMax(0x800);  // Send, bDynamic
        if (weapon_ch == 0 || weapon_ch == sess.current_weapon_ch) return;
        sess.current_weapon_ch = weapon_ch;
        util::BitWriter d;
        BuildClientNotifyChangedWeaponRPC(d, weapon_ch);
        SendActorUpdateBunch(client_id, kPCChannel, d);
        for (const game::ServerRoundActor* wpn : sess.data_actors) {
            auto w_it = sess.actor_channel.find(wpn);
            if (w_it == sess.actor_channel.end() || w_it->second != weapon_ch) continue;
            if (wpn->class_name.rfind("AA_BulletWeapon", 0) == 0) {
                constexpr uint32_t kClientNotifyRateOfFire = 68;   // AA_BulletWeapon
                util::BitWriter rof;
                rof.WriteIntMax(kClientNotifyRateOfFire, wpn->max_index);
                rof.WriteBit(false);                       // NextROF: not sent (0), retail 8 bits
                SendActorUpdateBunch(client_id, weapon_ch, rof);
            }
        }
        // Retail's initial reload follows the equip (World::BeginInitialReload).
        if (world_) {
            if (game::ServerPlayerController* pc = world_->FindPlayerController(client_id))
                for (const game::ServerRoundActor* wpn : sess.data_actors) {
                    auto w_it = sess.actor_channel.find(wpn);
                    if (w_it != sess.actor_channel.end() && w_it->second == weapon_ch)
                        world_->BeginInitialReload(pc->pawn, wpn);
                }
        }
        LOG_INFO("Client %u: current weapon -> ch %u", client_id, weapon_ch);
        return;
    }

    // Own pawn: AA_Pawn.ServerTryToReload(eReloadType) (124) and
    // ServerSetGuardState(bool) (130). Doc sec 31.
    if (ch_index == kOwnPawnChannel && world_) {
        game::ServerPlayerController* pc = world_->FindPlayerController(client_id);
        while (reader.GetPos() < bunch_end) {
            uint32_t idx = reader.ReadIntMax(max_index);
            if (idx == 124) {                               // ServerTryToReload
                if (reader.ReadBit()) reader.ReadBits(2);   // ReloadType (eReloadType)
                if (pc && pc->pawn) world_->BeginReload(pc->pawn);
                LOG_INFO("Client %u: reload requested", client_id);
            } else if (idx == 130) {                        // ServerSetGuardState(bGuard)
                reader.ReadBit();
            } else if (idx == 97) {                         // ServerStartMoveToTarget(TargetLocation, ...)
                int32_t v[3] = {0, 0, 0};
                if (reader.ReadBit()) ReadCompressedVector(reader, v);
                const float t[3] = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
                if (pc && pc->pawn) world_->StartMoveToTarget(pc->pawn, t);
                LOG_INFO("Client %u: vault/mantle to (%d, %d, %d)", client_id, v[0], v[1], v[2]);
                return;                                     // rest: rotator, delegate, float -- unused
            } else if (idx == 103 || idx == 104) {          // ServerClimbLadder(L) / ServerEndClimbLadder(OldLadder)
                if (reader.ReadBit()) { if (reader.ReadBit()) reader.ReadIntMax(0x800); else reader.ReadIntMax(0x80000000); }
                if (pc && pc->pawn) world_->ClimbLadder(pc->pawn, idx == 103);
                LOG_INFO("Client %u: ladder %s", client_id, idx == 103 ? "mount" : "dismount");
            } else if (idx == 49) {                         // ServerBeginHoldBreath()
                if (pc && world_->BeginHoldBreath(pc->pawn)) {
                    util::BitWriter d;
                    d.WriteIntMax(50, max_index);           // ClientHoldBreathBegun()
                    SendActorUpdateBunch(client_id, kOwnPawnChannel, d);
                }
            } else if (idx == 51) {                         // ServerEndHoldBreath(bForceCancel)
                const bool force = reader.ReadBit();
                if (pc && pc->pawn && pc->pawn->holding_breath) {
                    world_->EndHoldBreath(pc->pawn);
                    util::BitWriter d;
                    d.WriteIntMax(52, max_index);           // ClientEndHoldBreath(bForceCancel)
                    d.WriteBit(force);
                    SendActorUpdateBunch(client_id, kOwnPawnChannel, d);
                }
            } else {
                return;                                     // unknown: rest skipped via BDB
            }
        }
        return;
    }
    // Rifle: AA_Weapon.ServerSetWeaponFireMode(byte) (44) is acknowledged
    // with SetWeaponFireModeRecieved (45); AA_BulletWeapon.ServerSetRateOfFire
    // (eRateOfFire) (65) sets CurrentROF, which goes out with
    // ClientNotifyRateOfFire (see Tick). Start/StopFire (31/32): no server
    // ballistics yet.
    for (const game::ServerRoundActor* a : sess.data_actors) {
        if (a->class_name.rfind("AA_BulletWeapon", 0) != 0) continue;
        auto ch_it = sess.actor_channel.find(a);
        if (ch_it == sess.actor_channel.end() || ch_it->second != ch_index) continue;
        auto* weapon = const_cast<game::ServerRoundActor*>(a);
        while (reader.GetPos() < bunch_end) {
            uint32_t idx = reader.ReadIntMax(a->max_index);
            if (idx == 31 || idx == 32) {                   // ServerStartFire / ServerStopFire
                if (game::ServerPlayerController* pc = world_ ? world_->FindPlayerController(client_id) : nullptr)
                    world_->SetTrigger(pc->pawn, idx == 31);
                continue;
            }
            if (idx == 44) {
                uint8_t mode = reader.ReadBit() ? static_cast<uint8_t>(reader.ReadBits(8)) : 0;
                util::BitWriter d;
                d.WriteIntMax(45, a->max_index);            // SetWeaponFireModeRecieved()
                SendActorUpdateBunch(client_id, ch_index, d);
                LOG_INFO("Client %u: fire mode %u", client_id, mode);
            } else if (idx == 65) {
                uint8_t rof = reader.ReadBit() ? static_cast<uint8_t>(reader.ReadBits(2)) : 0;
                game::DataValue v; v.kind = game::DataValue::Kind::kByte; v.bits = 2; v.i = rof;
                weapon->Set(58, -1, v);
                if (game::ServerPlayerController* rpc_pc = world_ ? world_->FindPlayerController(client_id) : nullptr)
                    if (rpc_pc->pawn && rof != 0) rpc_pc->pawn->selected_rof = rof;
                LOG_INFO("Client %u: rate of fire %u", client_id, rof);
            } else {
                return;
            }
        }
        return;
    }

    if (max_index != kPCMaxIndex) {
        LOG_DEBUG("RX Ch=%u: actor bunch on a non-PC channel (MaxIndex=%u) — "
                  "no client->server fields decoded there yet", ch_index, max_index);
        return;
    }

    while (reader.GetPos() < bunch_end) {
        uint32_t rep_index = 0;
        DecodedRpc rpc;
        RpcDecodeResult res = DecodePcRpc(reader, bunch_end, max_index, &rep_index, &rpc);
        if (res == RpcDecodeResult::kOk) {
            ApplyPcRpc(client_id, sess, rpc);
            continue;
        }
        // Any failure: we no longer know where the next entry starts, so stop
        // here; the caller resyncs via BDB. Never guess a width.
        if (res == RpcDecodeResult::kUnknownIndex)
            LOG_INFO("RX Ch=%u: RepIndex=%u (not in the RPC table) from client %u — "
                     "skipping rest of bunch via BDB", ch_index, rep_index, client_id);
        else if (res == RpcDecodeResult::kUndecodable)
            LOG_INFO("RX Ch=%u: %s with a non-default unresolved struct param from "
                     "client %u — skipping rest of bunch via BDB", ch_index, rpc.name, client_id);
        else
            LOG_INFO("RX Ch=%u: RepIndex=%u overran the bunch from client %u",
                     ch_index, rep_index, client_id);
        return;
    }
}

void ProtocolHandler::ApplyPcRpc(uint32_t client_id, ClientSession& sess,
                                 const DecodedRpc& rpc) {
    game::ServerPlayerController* pc = world_ ? world_->FindPlayerController(client_id) : nullptr;
    const auto& p = rpc.params;

    // ServerMove(TimeStamp, InAccel, ClientLoc, MoveFlags, ClientRoll, View);
    // DualServerMove's second half carries the same fields for the newer
    // move (indices 4..9). PlayerController.uc ServerMove: ignore stale
    // TimeStamps; View = (Yaw << 16) | (Pitch & 0xFFFF); ClientRoll = Roll >> 8.
    // No server-side physics yet: ClientLoc is accepted as authoritative and
    // every move is acked good (no ClientAdjustPosition). That trusts the
    // client completely — fine for a first playable server, not anti-cheat.
    auto apply_move = [&](float ts, const RpcValue& loc, uint8_t roll, int32_t view) {
        if (ts <= sess.current_move_timestamp) return;
        const float prev_ts = sess.current_move_timestamp;
        sess.current_move_timestamp = ts;
        sess.pending_ack_timestamp = ts;
        ++sess.moves_received;
        if (!pc) return;
        ++pc->observed.moves;
        int32_t pitch = view & 0xFFFF;
        int32_t yaw = static_cast<int32_t>((static_cast<uint32_t>(view) >> 16) & 0xFFFF);
        pc->rotation_pitch = pitch;
        pc->rotation_yaw = yaw;
        if (pc->pawn) {
            // ClientLoc==(1,2,3) is UE3's "no location" sentinel (DualServerMove
            // passes it for the first half; PlayerController.uc:2835).
            bool has_loc = loc.sent && !(loc.v[0] == 1 && loc.v[1] == 2 && loc.v[2] == 3);
            if (has_loc) {
                // Velocity for non-owners' SimulatedProxy extrapolation, from
                // consecutive client positions (no server physics to supply
                // it). Skip the first move and implausible gaps (>0.5s:
                // respawn/teleport/stall) — zero there is the safe value.
                float dt = ts - prev_ts;
                bool plausible = prev_ts > 0.0f && dt > 0.0f && dt < 0.5f;
                if (plausible) {
                    double dx = loc.v[0] - pc->pawn->location[0];
                    double dy = loc.v[1] - pc->pawn->location[1];
                    double dz = loc.v[2] - pc->pawn->location[2];
                    pc->observed.distance_uu += std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                for (int k = 0; k < 3; ++k) {
                    float nl = static_cast<float>(loc.v[k]);
                    pc->pawn->velocity[k] = (prev_ts > 0.0f && dt > 0.0f && dt < 0.5f)
                        ? (nl - pc->pawn->location[k]) / dt : 0.0f;
                    pc->pawn->location[k] = nl;
                }
            }
            pc->pawn->rotation_yaw = yaw;
            pc->pawn->rotation_roll = static_cast<int32_t>(roll) << 8;
            // Pawn.RemoteViewPitch (non-owners only): high byte of view pitch.
            pc->pawn->remote_view_pitch = static_cast<uint8_t>((pitch >> 8) & 0xFF);
        }
        if (sess.moves_received == 1 || sess.moves_received % 500 == 0)
            LOG_INFO("RX client %u moves=%u t=%.3f loc=(%d,%d,%d) yaw=%d pitch=%d",
                     client_id, sess.moves_received, ts, loc.v[0], loc.v[1], loc.v[2],
                     yaw, pitch);
    };

    switch (rpc.id) {
        case PcRpc::kServerMove:
            apply_move(p[0].f, p[2], static_cast<uint8_t>(p[4].i), p[5].i);
            break;
        case PcRpc::kDualServerMove:
            // First half (TimeStamp0/InAccel0/PendingFlags/View0) is the older
            // pending move; with no physics to replay it, only the newer
            // move's state matters.
            apply_move(p[4].f, p[6], static_cast<uint8_t>(p[8].i), p[9].i);
            break;
        case PcRpc::kOldServerMove:
            // Redundant resend of an older, important move (accel only, no
            // location) — nothing to apply without server-side physics.
            break;
        case PcRpc::kServerSetSpectatorLocation:
            if (pc && p[0].sent)
                for (int k = 0; k < 3; ++k) pc->location[k] = static_cast<float>(p[0].v[k]);
            break;
        case PcRpc::kServerUpdatePing:
            // PlayerController.uc: PRI.Ping = Min(0.25 * NewPing, 250).
            if (pc && pc->pri) {
                int32_t ping = p[0].i / 4;
                pc->pri->ping = static_cast<uint8_t>(ping < 0 ? 0 : (ping > 250 ? 250 : ping));
            }
            break;
        // ---- Posture (doc sec 22). Retail replies with NotifyPostureChange
        // naming the posture being LEFT plus whether the weapon was at
        // high-ready in it (false only while sprinting); toggle-stance
        // flips tactical-combat posture and replies with a fixed 4-RPC
        // bunch. All reliable on the PC channel.
        case PcRpc::kServerSetCrouch:
        case PcRpc::kServerSetProne:
        case PcRpc::kServerStandUp:
        case PcRpc::kServerHoldSprint:
        case PcRpc::kServerTapSprint:
        case PcRpc::kServerStopSprint: {
            if (!pc) break;
            using P = game::ServerPlayerController::Posture;
            P old = pc->posture, nu = old;
            switch (rpc.id) {
                case PcRpc::kServerSetCrouch: nu = p[0].i ? P::kCrouch : (old == P::kCrouch ? P::kWalk : old); break;
                case PcRpc::kServerSetProne:  nu = p[0].i ? P::kProne  : (old == P::kProne  ? P::kWalk : old); break;
                case PcRpc::kServerStandUp:   nu = P::kWalk; break;
                case PcRpc::kServerStopSprint: nu = old == P::kSprint ? P::kWalk : old; break;
                default:                      nu = P::kSprint; break;  // Hold/TapSprint
            }
            if (world_ && pc->pawn) {
                using A = game::World::PawnAction;
                switch (rpc.id) {
                    case PcRpc::kServerSetCrouch:  world_->ApplyPawnAction(pc->pawn, p[0].i ? A::kCrouch : A::kStand); break;
                    case PcRpc::kServerStandUp:    world_->ApplyPawnAction(pc->pawn, A::kStand); break;
                    case PcRpc::kServerSetProne:   world_->ApplyPawnAction(pc->pawn, p[0].i ? A::kProne : A::kStand); break;
                    case PcRpc::kServerHoldSprint:
                    case PcRpc::kServerTapSprint:  world_->ApplyPawnAction(pc->pawn, A::kSprint); break;
                    case PcRpc::kServerStopSprint: world_->ApplyPawnAction(pc->pawn, A::kStopSprint); break;
                    default: break;
                }
            }
            if (nu == old) break;
            pc->posture = nu;
            ++pc->observed.posture_changes;
            if (nu == P::kSprint) ++pc->observed.sprints;
            if (pc->pawn) {
                pc->pawn->b_is_crouched = nu == P::kCrouch;
                pc->pawn->b_is_prone = nu == P::kProne;
            }
            // Retail (live retail-server capture, 2026-10-09): every change is
            // a pair, the posture left then the one entered, plus the
            // tactical/weapon-ready state around sprinting. Sending only the
            // "End" half left the client in no posture at all, which blocks
            // Use (doors, vault, ladders) -- doc sec 31.
            static const char* kEndName[] = {"EndWalk", "EndCrouch", "EndProne", "EndSprint"};
            static const char* kName[]    = {"Walk", "Crouch", "Prone", "Sprint"};
            util::BitWriter d;
            BuildNotifyPostureChangeRPC(d, kEndName[static_cast<int>(old)], old != P::kSprint);
            if (nu == P::kSprint) {
                BuildNotifyPostureChangeRPC(d, kName[static_cast<int>(nu)], false);
                BuildNotifyTacticalCombatPostureChangeRPC(d, false);
                BuildNotifyWeaponReadyChangeRPC(d, false);
            } else {
                if (old == P::kSprint) {
                    BuildNotifyTacticalCombatPostureChangeRPC(d, false);
                    BuildNotifyWeaponReadyChangeRPC(d, true);
                }
                BuildNotifyPostureChangeRPC(d, kName[static_cast<int>(nu)], true);
            }
            SendActorUpdateBunch(client_id, kPCChannel, d);
            LOG_DEBUG("client %u posture %s -> %d", client_id, kEndName[static_cast<int>(old)], (int)nu);
            break;
        }
        // Incapacitated (doc sec 35): the client reports its incap state
        // (ServerIncapChangeState(nameNextState)) as it goes down, and its
        // "withdraw" (MEDEVAC) option -- offered after GRI
        // iIncapacitationWithdrawDelay -- sends ServerSuicide(): the downed
        // soldier dies and respawns as usual. Only honoured while down.
        case PcRpc::kServerIncapChangeState:
            LOG_INFO("RX client %u: ServerIncapChangeState('%s')", client_id, p[0].s.c_str());
            break;
        case PcRpc::kServerSuicide:
            if (world_ && pc && world_->Withdraw(pc->pawn))
                LOG_INFO("Client %u: withdrew (MEDEVAC)", client_id);
            break;
        case PcRpc::kServerTryVault:
            if (world_ && pc && pc->pawn) {
                world_->ApplyPawnAction(pc->pawn, game::World::PawnAction::kVault);
                LOG_INFO("Client %u: vault", client_id);
            }
            break;
        case PcRpc::kServerOpenDoor:
        case PcRpc::kServerCloseDoor: {
            // The door is a placed map actor: a static ObjectIndex.
            const bool open = rpc.id == PcRpc::kServerOpenDoor;
            bool ok = world_ && p[0].sent && !p[0].dynamic &&
                      world_->UseDoor(static_cast<uint32_t>(p[0].i), open);
            LOG_INFO("RX client %u: %s(obj %d) -> %s", client_id,
                     open ? "ServerOpenDoor" : "ServerCloseDoor", p[0].i, ok ? "ok" : "no such door");
            break;
        }
        case PcRpc::kServerRaiseSights:
        case PcRpc::kServerLowerSights: {
            const bool raise = rpc.id == PcRpc::kServerRaiseSights;
            if (world_ && pc)
                world_->ApplyPawnAction(pc->pawn, raise ? game::World::PawnAction::kRaiseSights
                                                        : game::World::PawnAction::kLowerSights);
            if (pc) {
                // Retail (live capture t=33.11/36.97): the posture re-stated,
                // tactical on while sighted, weapon at high ready.
                using P = game::ServerPlayerController::Posture;
                static const char* kEndName[] = {"EndWalk", "EndCrouch", "EndProne", "EndSprint"};
                static const char* kName[]    = {"Walk", "Crouch", "Prone", "Sprint"};
                const int cur = static_cast<int>(pc->posture == P::kSprint ? P::kWalk : pc->posture);
                util::BitWriter d;
                BuildNotifyPostureChangeRPC(d, kEndName[cur], true);
                BuildNotifyPostureChangeRPC(d, kName[cur], true);
                BuildNotifyTacticalCombatPostureChangeRPC(d, raise || pc->tactical_combat);
                BuildNotifyWeaponReadyChangeRPC(d, true);
                SendActorUpdateBunch(client_id, kPCChannel, d);
            }
            break;
        }
        case PcRpc::kServerToggleStance: {
            if (!pc) break;
            pc->tactical_combat = !pc->tactical_combat;
            ++pc->observed.stance_toggles;
            if (world_) world_->ApplyPawnAction(pc->pawn, game::World::PawnAction::kSetTactical,
                                                pc->tactical_combat);
            // Exact retail reply shape (frames 16526/16625/...): posture
            // notify, tactical, weapon-ready, tactical again.
            util::BitWriter d;
            BuildNotifyPostureChangeRPC(d, "EndWalk", true);
            BuildNotifyTacticalCombatPostureChangeRPC(d, pc->tactical_combat);
            BuildNotifyWeaponReadyChangeRPC(d, true);
            BuildNotifyTacticalCombatPostureChangeRPC(d, pc->tactical_combat);
            SendActorUpdateBunch(client_id, kPCChannel, d);
            break;
        }
        case PcRpc::kServerJoinTeam: {
            if (!pc) break;
            pc->requested_team = p[0].i;
            bool ok = world_ && world_->JoinTeam(*pc, p[0].i);
            LOG_INFO("RX client %u: ServerJoinTeam(%d) -> %s", client_id, p[0].i,
                     ok ? "joined" : "refused");
            if (!ok) break;
            // Retail's reply (frame 139, ~20ms after the request): PRI.Team
            // (goes out with the next PRI diff), then on the PC channel
            // TeamMessage(PRI, "joined <team>", 'Say') and
            // EnterWaitingForPlayers(false). Doc sec 27.
            util::BitWriter d;
            if (!pc->pri->OnPlayableTeam()) break;   // Spectator: no join reply in retail's capture
            const int32_t joined = pc->pri->team->team_index;
            const std::string& team_name = world_->GetGRI()
                ? world_->GetGRI()->team_names[static_cast<size_t>(joined)] : std::string("team");
            BuildTeamMessageRPC(d, sess.actor_channel.count(pc->pri) ? sess.actor_channel[pc->pri] : 0,
                                "joined " + team_name);
            BuildEnterWaitingForPlayersRPC(d, /*hide_only=*/false);
            SendActorUpdateBunch(client_id, kPCChannel, d);
            break;
        }
        case PcRpc::kServerSelectClassRole_ByIndex: {
            if (!pc) break;
            pc->class_role_index = p[0].i;
            bool ok = world_ && world_->AssignRole(*pc, p[0].i);
            LOG_INFO("RX client %u: ServerSelectClassRole_ByIndex(%d) -> %s", client_id, p[0].i,
                     ok ? "assigned" : "refused");
            // The PRI and the role/fire-team changes go out with the next
            // replication tick (doc sec 28).
            break;
        }
        case PcRpc::kServerSetPlayerReady:
            if (pc && world_) world_->SetPlayerReady(*pc, p[0].i != 0);
            LOG_INFO("RX client %u: ServerSetPlayerReady(%d)", client_id, p[0].i);
            break;
        case PcRpc::kServerUpdateLevelVisibility:
            LOG_INFO("RX client %u: level '%s' visible=%d", client_id, p[0].s.c_str(), p[1].i);
            break;
        case PcRpc::kAskForPawn:
            LOG_INFO("RX client %u: AskForPawn()", client_id);
            break;
        case PcRpc::kServerReturnToParty:
            LOG_INFO("RX client %u: ServerReturnToParty(bPartyMembersOnly=%d)", client_id, p[0].i);
            break;
        case PcRpc::kServerViewSelf:
            LOG_INFO("RX client %u: ServerViewSelf()", client_id);
            break;
        default:
            LOG_DEBUG("RX client %u: %s (no server-side effect yet)", client_id, rpc.name);
            break;
    }
}

// Synthesized post-JOIN replication. First experiment: open Ch=2 with
// the WorldInfo actor (a placed actor in the level, not transient and not
// a PlayerController, so no SerializeCompressedInitial / NetPlayerIndex).
// BDB=32 = bDynamic(1) + ObjectIndex(31). The client's UActorChannel::
// ReceivedBunch (UnChan.cpp:1477) then attempts ReadInt(MaxIndex) which
// fails on the exhausted bunch and the property loop exits cleanly.
//
// ObjectIndex value taken from retail capture (frame 72 Ch=2 open) since
// our PackageMap index computation still has a ~29k overshoot. Both sides
// resolve the same value to the same actor because both built their
// PackageMap from the identical USES manifest the server shipped earlier.
void ProtocolHandler::PossessPawn(uint32_t client_id, ClientSession& sess,
                                  const game::ServerPlayerController& pc) {
    // Retail's spawn, GamePlayPCAP_Legit_Server frames 365-372 (doc sec 29):
    //   1. ClientRestart(None), ClientSetCameraMode('FirstPerson'),
    //      ClientSetRotation(spawn rotation), ClientMatchStarting()
    //   2. ClientUpdateLevelStreamingStatus: the sound/volume sublevels now
    //      load too (S_Map_<Base>, <Base>_Volumes: loaded, visible, blocking)
    //   3. the Pawn's channel opens (owner = this PC, AutonomousProxy)
    //   4. ClientSetViewTarget(pawn), GivePawn(pawn),
    //      ClientAdjustPosition(TimeStamp, 'PlayerWalking', PHYS_Falling,
    //      location), ClientSetBlockOnAsyncLoading()
    // After this the client sends ServerMove and gets ClientAckGoodMove.
    const game::ServerPawn& pawn = *pc.pawn;
    // A new soldier starts its notice serials at zero: line the session up
    // with them so nothing from the previous life (incapacitation, a hit,
    // the ladder sling...) is re-announced at spawn (doc sec 35).
    sess.hit_sent = pawn.hit_serial;
    sess.incap_sent = pawn.incap_serial;
    sess.sling_sent = pawn.sling_serial;
    sess.vault_sent = pawn.vault_finish_serial;
    sess.breath_end_sent = pawn.breath_end_serial;
    util::BitWriter d;
    // Retail R22 opens with the weapon-ready / posture notifications that
    // initialize the client's posture state (doc sec 30).
    BuildNotifyWeaponReadyChangeRPC(d, true);
    BuildNotifyPostureChangeRPC(d, "Walk", false);
    BuildClientSetViewTargetRPC(d, 0);
    BuildNotifyWeaponReadyChangeRPC(d, true);
    BuildNotifyTacticalCombatPostureChangeRPC(d, false);
    BuildClientRestartRPC(d, 0);
    BuildClientSetCameraModeRPC(d, "FirstPerson");
    BuildClientSetRotationRPC(d, pawn.rotation_pitch, pawn.rotation_yaw, pawn.rotation_roll,
                              /*reset_camera=*/false);
    BuildClientMatchStartingRPC(d);
    SendActorUpdateBunch(client_id, kPCChannel, d);

    util::BitWriter streaming;
    for (const auto& level : StreamingLevels()) {
        bool extra = level.package.rfind("S_Map_", 0) == 0 ||
                     (level.package.size() > 8 &&
                      level.package.compare(level.package.size() - 8, 8, "_Volumes") == 0);
        if (extra)
            BuildClientUpdateLevelStreamingStatusRPC(streaming, level.package, true, true, true);
    }
    if (streaming.GetBitCount()) SendActorUpdateBunch(client_id, kPCChannel, streaming);

    if (sess.channel_max_index.count(kOwnPawnChannel)) {
        // A previous pawn of this player: retire its channel first.
        CloseActorChannel(client_id, kOwnPawnChannel);
    }
    // Spawn inventory, in retail's order (frames 365-366): the items, then
    // the pawn, then the InventoryManager, tracer info and weapon mod.
    // References between them resolve as their channels open (doc sec 30).
    const auto* inventory = world_ ? world_->PawnInventory(&pawn) : nullptr;
    auto after_pawn = [](const game::ServerRoundActor* a) {
        return a->class_name == "AA_InventoryManager" || a->class_name == "AA_TracerReplicationInfo" ||
               a->class_name.rfind("AA_WeaponMod", 0) == 0;
    };
    if (inventory)
        for (const auto& it : *inventory)
            if (!after_pawn(it.get())) OpenDataActor(client_id, sess, it.get());
    util::BitWriter pawn_payload;
    EncodePawnRep(pawn_payload, pawn, nullptr, MakeRepContext(sess, kOwnPawnChannel, /*for_owner=*/true));
    OpenActorChannel(client_id, kOwnPawnChannel, pawn.object_index, &pawn_payload, &pawn);
    sess.channel_max_index[kOwnPawnChannel] = kPawnMaxIndex;
    sess.actor_channel[&pawn] = kOwnPawnChannel;
    sess.last_sent_pawn = pawn;
    uint32_t weapon_ch = 0;
    if (inventory)
        for (const auto& it : *inventory) {
            if (after_pawn(it.get())) OpenDataActor(client_id, sess, it.get());
            if (it->class_name.rfind("AA_BulletWeapon", 0) == 0) weapon_ch = sess.actor_channel[it.get()];
        }

    util::BitWriter g;
    BuildClientSetViewTargetRPC(g, kOwnPawnChannel);
    BuildGivePawnRPC(g, kOwnPawnChannel);
    const float vel[3] = {0.0f, 0.0f, 0.0f};
    constexpr uint8_t kPhysFalling = 2;
    BuildClientAdjustPositionRPC(g, sess.current_move_timestamp, "PlayerWalking", kPhysFalling,
                                 pawn.location, vel);
    // Retail R31: right after ClientAdjustPosition, the posture is re-stated.
    BuildNotifyPostureChangeRPC(g, "EndWalk", true);
    BuildNotifyPostureChangeRPC(g, "Walk", true);
    // ClientUpdateScoreboard(bVisibilityOverride=False): retail closes the
    // pre-round scoreboard here (frame 371). Left open, its focused button
    // would take the Spacebar ("Clicked" alias) before PlayerController.Use
    // (doc sec 31).
    g.WriteIntMax(rep::AGP_PC::ClientUpdateScoreboard, kPCMaxIndex);
    g.WriteBit(false);
    BuildClientSetBlockOnAsyncLoadingRPC(g);
    SendActorUpdateBunch(client_id, kPCChannel, g);
    if (inventory) {
        // Retail's HUD inventory notices (frames 365: EInventoryType 17 = the
        // rifle with 6 magazines, 14, 7) and the weapon in hand (frame 376).
        util::BitWriter h;
        BuildClientSendHUDNotifyAddedInventoryRPC(h, pawn.ammo_inv_type, false);
        BuildClientAdvancedInventoryInitializeAmmoMagazineCountRPC(h, pawn.ammo_inv_type, pawn.spare_mags);
        BuildClientSendHUDNotifyAddedInventoryRPC(h, pawn.weapon_inv_type, false);
        BuildClientSendHUDNotifyAddedInventoryRPC(h, 7, false);
        SendActorUpdateBunch(client_id, kPCChannel, h);
        // ClientNotifyChangedWeapon waits for the client's
        // ServerSetCurrentWeapon, as in retail (see DecodeActorBunch).
        (void)weapon_ch;
    }
    sess.possessed_pawn = &pawn;
    sess.current_weapon_ch = 0;
    LOG_INFO("Client %u: possessed pawn on ch %u at (%.0f, %.0f, %.0f)", client_id, kOwnPawnChannel,
             pawn.location[0], pawn.location[1], pawn.location[2]);
}

void ProtocolHandler::OpenMatchActors(uint32_t client_id, ClientSession& sess) {
    // Retail opens the ES2 objects ~1s before the pawn (live retail capture
    // t=22.15; doc sec 31).
    if (!world_ || sess.match_opened) return;
    sess.match_opened = true;
    sess.match_actor_serial_seen = world_->MatchActorSerial();
    for (const auto& a : world_->MatchActors()) OpenDataActor(client_id, sess, a.get());
}

void ProtocolHandler::OpenRoundActors(uint32_t client_id, ClientSession& sess) {
    // Retail opens these right before role selection (t=6.87-7.2s): role
    // slots first here, so FireTeamInfo.aamcrriClassRoles resolve in the
    // fire teams' open bunches (doc sec 28).
    if (!world_ || sess.round_opened) return;
    sess.round_opened = true;
    std::vector<game::ServerRoundActor*> order;
    for (const auto& a : world_->RoundActors())
        if (a->class_name == "AA_MCClassRoleReplicationInfo") order.push_back(a.get());
    for (const auto& a : world_->RoundActors())
        if (a->class_name != "AA_MCClassRoleReplicationInfo") order.push_back(a.get());
    for (game::ServerRoundActor* a : order) OpenDataActor(client_id, sess, a);
    LOG_INFO("Client %u: opened %zu round actors", client_id, order.size());
}

uint32_t ProtocolHandler::OpenDataActor(uint32_t client_id, ClientSession& sess,
                                        const game::ServerRoundActor* a) {
    if (sess.next_free_channel >= 0x800) {
        LOG_WARN("Client %u: out of channels", client_id);
        return 0;
    }
    uint32_t ch = sess.next_free_channel++;
    sess.actor_channel[a] = ch;
    sess.channel_max_index[ch] = a->max_index;
    util::BitWriter payload;
    EncodeRoundActor(payload, *a, sess.round_sent[a], MakeRepContext(sess, ch, /*for_owner=*/true));
    OpenActorChannel(client_id, ch, a->object_index, &payload, a->transient ? a : nullptr);
    sess.data_actors.push_back(a);
    return ch;
}

void ProtocolHandler::SendPhaseRPCs(uint32_t client_id, game::World::MatchPhase phase) {
    // Retail's per-phase PC-channel calls (GamePlayPCAP_Legit_Server frames
    // 151/162, 226, 350; doc sec 28). After a round (doc sec 36) the next
    // round's phases use retail's round-2 variants (live capture t=244-256).
    using P = game::World::MatchPhase;
    auto sit = sessions_.find(client_id);
    if (sit == sessions_.end()) return;
    ClientSession& sess = sit->second;
    if (phase != P::kPendingMatch) OpenRoundActors(client_id, sess);
    const game::ServerGameReplicationInfo* gri = world_ ? world_->GetGRI() : nullptr;
    const bool later_round = gri && gri->i_current_round > 1;
    const game::ServerPlayerController* pc = world_ ? world_->FindPlayerController(client_id) : nullptr;
    util::BitWriter d;
    switch (phase) {
    case P::kRoleSelection: {
        if (later_round) {
            BuildPCCallRPC(d, rep::PC::TS3ClientChangeChannel);
            BuildClientEnterPrePlanningToolRPC(d, true, false, false);
            break;
        }
        BuildClientEnterPrePlanningToolRPC(d, false, false, false);
        SendActorUpdateBunch(client_id, kPCChannel, d);
        // Retail sends ClientGotoState in a bunch of its own (frame 162).
        d = util::BitWriter();
        BuildClientGotoStateRPC(d, "WaitingForOtherPlayers");
        break;
    }
    case P::kDeploymentWarning:
        if (later_round && sess.channel_max_index.count(3)) {
            // AA_SteamStatManager.ClientTransmitLastRoundStats() (t=245.33).
            util::BitWriter st;
            st.WriteIntMax(rep::SteamStatManager::ClientTransmitLastRoundStats,
                           rep::SteamStatManager::kMaxIndex);
            SendActorUpdateBunch(client_id, 3, st);
        }
        BuildClientDeploymentWarningRPC(d, gri ? gri->deployment_time : 10);
        if (!later_round) BuildClientUpdatePlayerRallyPointToReachRPC(d);
        break;
    case P::kInProgress:
        // A reset ES2 set: retail closes the old objects and opens the new
        // ones at match start (t=255.34).
        if (sess.match_opened && sess.match_actor_serial_seen != world_->MatchActorSerial()) {
            for (const auto& a : world_->MatchActors()) CloseDataActor(client_id, sess, a.get());
            sess.match_opened = false;
        }
        BuildClientClosePrePlanningToolRPC(d, false);
        BuildClientSteamStatUpdateMissionStartRPC(d, !later_round);
        SendActorUpdateBunch(client_id, kPCChannel, d);
        sess.match_actor_serial_seen = world_->MatchActorSerial();
        OpenMatchActors(client_id, sess);
        LOG_INFO("Client %u: sent round-phase RPCs (phase %d)", client_id, static_cast<int>(phase));
        return;
    case P::kRoundOver: {
        // GameHasEnded (retail t=227.34, 81.87, 367.91): the stinger --
        // victory for the winners, defeat otherwise -- and for a player
        // still in the fight the unpossess into FreeCam spectating; then
        // ClientGameEnded (bIsWinner is False in every retail sample) and
        // ClientGotoState('Spectating').
        const int32_t team = pc ? game::World::TeamOf(*pc) : -1;
        const bool won = team >= 0 && team == world_->RoundWinner();
        BuildClientPlaySoundRPC(d, world_->StingerIndex(won));
        if (world_->NextRoundSwaps()) BuildTeamMessageRPC(d, 0, "Teams will swap sides the next round.");
        if (sess.possessed_pawn && !sess.possessed_pawn->torn_off) {
            BuildClientSetViewTargetRPC(d, kPCChannel);
            BuildPCCallRPC(d, rep::PC::ClientUnpossess);
            BuildPCCallRPC(d, rep::PC::TS3ClientChangeChannel);
            BuildClientSetCameraModeRPC(d, "FreeCam");
            BuildClientSetSpectatorModeRPC(d, "FreeCam");
            BuildHideScoreboardRPC(d, true);
            BuildPCCallRPC(d, rep::PC::ClientUpdateSpectatorViewType);
        }
        BuildClientGameEndedRPC(d, false);
        SendActorUpdateBunch(client_id, kPCChannel, d);
        d = util::BitWriter();
        BuildClientUpdateScoreboardRPC(d, false);
        BuildClientGotoStateRPC(d, "Spectating");
        break;
    }
    case P::kShowScoreboard: {
        // ResetLevel (retail t=229.31): ClientReset, first-person view from
        // one of the map's AA_Viewpoints, every soldier and its inventory
        // gone, then the after-action report (ClientDisplayScoreboard).
        const uint32_t view = world_->RandomViewpointIndex();
        BuildPCCallRPC(d, rep::PC::ClientReset);
        BuildClientSetCameraModeRPC(d, "FirstPerson");
        BuildClientSetSpectatorModeRPC(d, "FirstPerson");
        if (view) BuildClientSetViewTargetObjectRPC(d, view);
        SendActorUpdateBunch(client_id, kPCChannel, d);
        RetireOwnPawn(client_id, sess);
        d = util::BitWriter();
        BuildClientDisplayScoreboardRPC(d, 10, world_->FinishedRound());
        SendActorUpdateBunch(client_id, kPCChannel, d);
        d = util::BitWriter();
        BuildClientGotoStateRPC(d, "WaitingForMatchStart");
        break;
    }
    default:
        return;
    }
    SendActorUpdateBunch(client_id, kPCChannel, d);
    LOG_INFO("Client %u: sent round-phase RPCs (phase %d)", client_id, static_cast<int>(phase));
}

std::vector<ProtocolHandler::StreamingLevel> ProtocolHandler::StreamingLevels() const {
    // Retail makes the gametype level and <Base>_pathing loaded+visible and
    // every other sublevel unloaded (frame 72; doc sec 27).
    std::vector<StreamingLevel> out;
    if (!world_) return out;
    for (const auto& name : world_->StreamingLevelNames())
        out.push_back({name, name == world_->GameModeLevel() || name == world_->PathingLevel()});
    return out;
}

void ProtocolHandler::AppendLoginRPCs(util::BitWriter& w, const game::ServerPlayerController& pc) {
    // Retail's PC-open RPCs, same order (doc sec 27). Skipped:
    // ClientEnableTS3Voice -- no TeamSpeak server behind this one.
    constexpr int32_t kBandwidthCap = 10000;
    BuildClientSetHUDRPC(w, world_ ? world_->HUDClassIndex() : 0);
    BuildClientCapBandwidthRPC(w, kBandwidthCap);
    for (const auto& level : StreamingLevels())
        BuildClientUpdateLevelStreamingStatusRPC(w, level.package, level.visible,
                                                 level.visible, /*block=*/false);
    BuildClientFlushLevelStreamingRPC(w);
    BuildClientCapBandwidthRPC(w, kBandwidthCap);
    BuildOpenPostAuthMenuRPC(w);
    (void)pc;
}

void ProtocolHandler::OpenInitialActorChannels(uint32_t client_id) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    auto& sess = it->second;

    LOG_INFO("Client %u reached ESTABLISHED. Opening initial actor channels.",
             client_id);

    if (!world_) {
        LOG_ERROR("No world bound; cannot open actor channels.");
        return;
    }

    // Open channels for the server-side actor instances. ObjectIndex values
    // come from the world model (which initializes them from the legit-pcap
    // retail values for now). Both sides resolve the same value via the
    // PackageMap built from the shared USES manifest.
    //
    // Most opens are still BDB=32 (bDynamic(0) + ObjectIndex(31), no
    // property data) — the client's ReceivedBunch property loop hits
    // IsError on the exhausted bunch and exits cleanly, leaving the channel
    // bound to the class with default property values. WorldInfo and GRI
    // (Ch=2/4) now carry their real initial state via network/rep_encode.h
    // — see the two open_for_actor calls below with an explicit payload.
    auto open_for_actor = [&](uint32_t ch, const game::ServerActor* actor,
                              const char* tag, uint32_t max_index,
                              const util::BitWriter* payload = nullptr) {
        if (!actor || actor->object_index == 0) {
            LOG_WARN("  → Ch=%u %s SKIPPED (no actor or no object_index)", ch, tag);
            return;
        }
        // actor->transient actors (server-spawned, e.g. a PRI) need the
        // SerializeCompressedInitial preamble; placed actors don't.
        OpenActorChannel(client_id, ch, actor->object_index, payload,
                         actor->transient ? actor : nullptr);
        // Record the class table an incoming bunch on this channel must
        // decode RepIndex against (DecodeActorBunch), and register the
        // channel so later references to this actor resolve to it.
        sess.channel_max_index[ch] = max_index;
        sess.actor_channel[actor] = ch;
        LOG_INFO("  → Ch=%u %s [obj_idx=%u, label=%s]%s%s",
                 ch, tag, actor->object_index, actor->label.c_str(),
                 payload ? " (+initial state)" : "",
                 actor->transient ? " (transient)" : "");
    };

    // No WorldInfo channel: it's a placed level actor the client already
    // has from the map, and retail doesn't open one here. Ch=2 used to
    // carry it under ObjectIndex 172588 -- which is actually
    // Default__AGP_PlayerController, so the real client spawned a second
    // PC from WorldInfo property bits and read NetPlayerIndex=206 out of
    // them (doc sec 26).
    const game::ServerWorldInfo* world_info = world_->GetWorldInfo();

    // Spawn this client's PlayerController (+ its PRI/Pawn/StatManager).
    game::ServerPlayerController* pc =
        world_->CreatePlayerController(client_id, sess.player_name);

    if (pc && pc->object_index != 0) {
        // First, like retail. Not routed through open_for_actor: the PC is
        // the one actor that also needs NetPlayerIndex (UnChan.cpp:1792-1797)
        // after the transient preamble; NetPlayerIndex 0 is what makes the
        // client call HandleClientPlayer and leave its "Connecting" screen
        // (UnGame.cpp:3581). PC.Pawn and PC.PlayerReplicationInfo go out NULL
        // here (their channels open later); the first Tick re-sends them --
        // retail's Mapped=0 retry, via RepContext::sent_refs.
        util::BitWriter pc_payload;
        EncodePCRep(pc_payload, *pc, nullptr, MakeRepContext(sess, kPCChannel, /*for_owner=*/true));
        AppendLoginRPCs(pc_payload, *pc);
        OpenActorChannel(client_id, kPCChannel, pc->object_index, &pc_payload,
                         pc, &pc->net_player_index);
        sess.channel_max_index[kPCChannel] = kPCMaxIndex;
        sess.actor_channel[pc] = kPCChannel;
        LOG_INFO("  → Ch=2 PlayerController (this client) [obj_idx=%u, label=%s] (transient)",
                 pc->object_index, pc->label.c_str());

        // First real RPC call this server sends: PlayerController.
        // ClientMessage (network/rpc_encode.h), on the PC's own channel —
        // an RPC targets a specific actor instance, so it must go out on
        // that actor's channel, same as a property update would. Safe to
        // send now: the channel-open bunch just above already replicated
        // the actor, satisfying UnScript.cpp:2890's "channel must be open
        // before an RPC can ride on it" requirement.
        std::string name = pc->pri ? pc->pri->player_name : "Player";
        util::BitWriter welcome_rpc;
        BuildClientMessageRPC(welcome_rpc, "Welcome to the server, " + name + "!");
        SendActorUpdateBunch(client_id, kPCChannel, welcome_rpc);
        LOG_INFO("  → Sent ClientMessage RPC (welcome) to client %u", client_id);
    }

    util::BitWriter stat_mgr_payload;
    if (pc && pc->stat_manager) {
        EncodeSteamStatManagerRep(stat_mgr_payload, *pc->stat_manager, nullptr);
        open_for_actor(3, pc->stat_manager, "SteamStatManager (this client)",
                       kSteamStatManagerMaxIndex, &stat_mgr_payload);
    }

    const game::ServerGameReplicationInfo* gri = world_->GetGRI();
    // The GRI's full initial state (~4.3k bits) is more than one bunch can
    // carry: BunchDataBits is capped at 4096 (one packet), and an oversized
    // value wraps -- the client read a corrupt open and kept AA3's class
    // defaults for every GRI flag (doc sec 31). Like UE3 when a bunch fills
    // up, the open carries the fields that fit and the rest follow in the
    // next reliable bunch on the channel.
    util::BitWriter gri_payload;
    std::vector<uint32_t> gri_field_ends;
    if (gri) EncodeGRIRep(gri_payload, *gri, nullptr, &gri_field_ends);
    std::vector<util::BitWriter> gri_rest;
    constexpr uint32_t kBunchPropBudget = 3200;
    if (gri_payload.GetBitCount() > kBunchPropBudget) {
        // Greedy: each piece is the longest run of whole fields within budget.
        std::vector<uint32_t> cuts{0};
        uint32_t last_fit = 0;
        for (uint32_t end : gri_field_ends) {
            if (end - cuts.back() > kBunchPropBudget) { cuts.push_back(last_fit); }
            last_fit = end;
        }
        cuts.push_back(gri_payload.GetBitCount());
        const auto& bytes = gri_payload.GetData();
        std::vector<util::BitWriter> pieces;
        for (size_t k = 0; k + 1 < cuts.size(); ++k) {
            util::BitReader r(bytes.data(), gri_payload.GetBitCount());
            for (uint32_t b = 0; b < cuts[k]; ++b) r.ReadBit();
            util::BitWriter piece;
            for (uint32_t b = cuts[k]; b < cuts[k + 1]; ++b) piece.WriteBit(r.ReadBit());
            pieces.push_back(std::move(piece));
        }
        gri_payload = std::move(pieces.front());
        gri_rest.assign(std::make_move_iterator(pieces.begin() + 1), std::make_move_iterator(pieces.end()));
    }
    open_for_actor(4, gri, "GameReplicationInfo", kGRIMaxIndex, &gri_payload);
    for (auto& piece : gri_rest) SendActorUpdateBunch(client_id, 4, piece);

    if (pc && pc->mos_qual) {
        // No property payload: AA_MOSQualificationBCT has zero own
        // replicated properties (MaxIndex=23, Actor-base only) — the
        // channel-open carries just the ObjectIndex + transient preamble.
        open_for_actor(5, pc->mos_qual, "MOSQualificationBCT (this client)",
                       kMOSQualificationBCTMaxIndex);
    }

    std::array<util::BitWriter, 4> team_payloads;
    for (size_t i = 0; i < world_->GetTeamCount() && i < 4; ++i) {
        game::ServerTeamInfo* team = world_->GetTeam(i);
        if (team) EncodeTeamInfoRep(team_payloads[i], *team, nullptr);
        open_for_actor(static_cast<uint32_t>(6 + i), team, "TeamInfo",
                       kTeamInfoMaxIndex, team ? &team_payloads[i] : nullptr);
    }

    // Ch=10 for this client's PRI, Ch=11 for the PC itself (pc was created
    // above, ahead of Ch=3).
    util::BitWriter pri_payload;
    if (pc && pc->pri) {
        RepContext pri_ctx = MakeRepContext(sess, 10, /*for_owner=*/true);
        EncodePRIRep(pri_payload, *pc->pri, nullptr, &pri_ctx);
        open_for_actor(10, pc->pri, "PlayerReplicationInfo (this client)",
                       kPRIMaxIndex, &pri_payload);
    }
    if (pc && pc->proxy) {
        // After the PC and PRI, so its Owner/RepController/RepPRI refs
        // resolve in the open bunch itself.
        util::BitWriter proxy_payload;
        EncodePlayerStorageProxyRep(proxy_payload, *pc->proxy, nullptr,
                                    MakeRepContext(sess, kProxyChannel, /*for_owner=*/true));
        open_for_actor(kProxyChannel, pc->proxy, "PlayerStorageProxy (this client)",
                       kPlayerStorageProxyMaxIndex, &proxy_payload);
        sess.last_sent_proxy = *pc->proxy;
        sess.proxy_sent_valid = true;
    }
    // Placed map actors (doors, windows, ladder volumes...): retail opens
    // them right after join, before anyone spawns (frames 85-130; doc sec 31).
    for (const auto& a : world_->MapActors()) OpenDataActor(client_id, sess, a.get());
    for (const auto& a : world_->JoinActors()) OpenDataActor(client_id, sess, a.get());
    if (world_->Phase() == game::World::MatchPhase::kInProgress) OpenMatchActors(client_id, sess);
    if (pc && pc->pawn) {
        util::BitWriter pawn_payload;
        EncodePawnRep(pawn_payload, *pc->pawn, nullptr, MakeRepContext(sess, kOwnPawnChannel, /*for_owner=*/true));
        open_for_actor(kOwnPawnChannel, pc->pawn, "Pawn (this client)", kPawnMaxIndex, &pawn_payload);

        // Post-spawn RPC batch: all target the PC's own channel (Ch=11) —
        // an RPC rides the channel of the actor it's called ON, and all
        // three of these are PlayerController functions. Real, meaningful
        // triggers, not synthetic demonstrations: point the camera at the
        // just-spawned Pawn, orient it (currently a no-op value since this
        // server has no spawn-point data — see rep_encode.h's Pawn notes —
        // but the RPC itself is genuine), and tell the client to run its
        // own best-weapon selection.
        util::BitWriter view_target_rpc;
        BuildClientSetViewTargetRPC(view_target_rpc, sess.actor_channel[pc->pawn]);
        SendActorUpdateBunch(client_id, kPCChannel, view_target_rpc);

        util::BitWriter set_rotation_rpc;
        BuildClientSetRotationRPC(set_rotation_rpc,
                                  pc->pawn->rotation_pitch, pc->pawn->rotation_yaw,
                                  pc->pawn->rotation_roll, /*reset_camera=*/true);
        SendActorUpdateBunch(client_id, kPCChannel, set_rotation_rpc);

        util::BitWriter switch_weapon_rpc;
        BuildClientSwitchToBestWeaponRPC(switch_weapon_rpc, /*force_new_weapon=*/false);
        SendActorUpdateBunch(client_id, kPCChannel, switch_weapon_rpc);

        LOG_INFO("  → Sent ClientSetViewTarget/ClientSetRotation/"
                 "ClientSwitchToBestWeapon RPCs to client %u", client_id);
    }

    // No join-time TeamMessage/ClientSetProgressMessage: retail sends
    // neither at join. Its TeamMessage ("joined Black Team") is the reply
    // to ServerJoinTeam (ApplyPcRpc; doc sec 27).

    // Seed the dirty-tracking snapshot with exactly what we just sent, so
    // the first Tick() diff is empty unless the world changed in between.
    if (world_info) sess.last_sent_world_info = *world_info;
    if (gri)        sess.last_sent_gri        = *gri;
    for (size_t i = 0; i < world_->GetTeamCount() && i < 4; ++i) {
        if (game::ServerTeamInfo* team = world_->GetTeam(i)) {
            sess.last_sent_teams[i] = *team;
        }
    }
    if (pc && pc->pri)  sess.last_sent_pri  = *pc->pri;
    if (pc)             sess.last_sent_pc   = *pc;
    if (pc && pc->pawn) sess.last_sent_pawn = *pc->pawn;
    if (pc && pc->stat_manager) sess.last_sent_stats = *pc->stat_manager;
    sess.has_last_sent_state = (world_info != nullptr) && (gri != nullptr);

    using namespace std::chrono;
    sess.next_keepalive_us = duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count() + 200'000;
}

void ProtocolHandler::OpenActorChannel(uint32_t client_id, uint32_t ch_index,
                                       uint32_t class_obj_idx,
                                       const util::BitWriter* prop_payload,
                                       const game::ServerActor* transient_actor,
                                       const uint8_t* net_player_index) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    auto& sess = it->second;

    // Transient (server-spawned) actors carry SerializeCompressedInitial
    // (Location/Rotation) right after the ObjectIndex — UnChan.cpp:1788-1790.
    // Built into its own writer first so its (variable, magnitude-dependent)
    // bit cost can be measured before sizing the bunch header, same pattern
    // as prop_payload.
    util::BitWriter transient_preamble;
    if (transient_actor) {
        WriteSerializeCompressedInitial(transient_preamble, *transient_actor);
    }

    util::BitWriter w;
    uint32_t seq = sess.out_seq;

    // Bunch payload = bDynamic(1) + ObjectIndex(31) preamble, plus the
    // transient preamble (if any), plus NetPlayerIndex (PlayerController
    // only, UnChan.cpp:1792-1797), plus whatever initial-state property
    // data the caller built (all already-sized, so the header's
    // BunchDataBits is exact — no guessing, per doc sec 10.3).
    constexpr uint32_t kObjRefBits = 32;
    uint32_t transient_bits = transient_preamble.GetBitCount();
    uint32_t npi_bits = net_player_index ? 8 : 0;
    uint32_t prop_bits = prop_payload ? prop_payload->GetBitCount() : 0;
    uint32_t bunch_data_bits = kObjRefBits + transient_bits + npi_bits + prop_bits;

    // First reliable bunch on this channel = ChSeqRaw 1.
    uint32_t ch_seq = 1;
    WriteBunchHeader(w, ch_index, kActorChType, ch_seq, bunch_data_bits,
                     /*bOpen=*/true);

    // Bunch payload: bDynamic=0 (static reference), then ObjectIndex
    // RIM-encoded against MAX_OBJECT_INDEX = 1<<31.
    w.WriteBit(0);
    w.WriteIntMax(class_obj_idx, 0x80000000);
    if (transient_actor) w.AppendBits(transient_preamble);
    if (net_player_index) w.WriteByte(*net_player_index);
    if (prop_payload) w.AppendBits(*prop_payload);

    uint32_t bunch_bits = w.GetBitCount();
    SendBunch(client_id, sess, std::move(w), ch_index, ch_seq);
    sess.channel_reliable_seq[ch_index] = ch_seq;
    LOG_INFO("OpenActorChannel ch=%u class_obj_idx=%u seq=%u bunch_bits=%u "
             "transient_bits=%u npi_bits=%u prop_bits=%u",
             ch_index, class_obj_idx, seq, bunch_bits, transient_bits, npi_bits, prop_bits);
}

void ProtocolHandler::SendActorUpdateBunch(uint32_t client_id, uint32_t ch_index,
                                           const util::BitWriter& prop_payload,
                                           bool reliable) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    auto& sess = it->second;

    util::BitWriter w;
    uint32_t seq = sess.out_seq;

    // Unreliable bunches carry no ChSequence and must not consume one --
    // the next reliable bunch on this channel continues the sequence.
    uint32_t ch_seq = reliable ? ++sess.channel_reliable_seq[ch_index] : 0;
    WriteBunchHeader(w, ch_index, kActorChType, ch_seq,
                     prop_payload.GetBitCount(), /*bOpen=*/false, reliable);
    w.AppendBits(prop_payload);
    SendBunch(client_id, sess, std::move(w), ch_index, ch_seq);
    // Unreliable traffic (move acks, ~30/s) would flood the log at INFO.
    if (reliable)
        LOG_INFO("SendActorUpdateBunch ch=%u seq=%u ChSeq=%u prop_bits=%u",
                 ch_index, seq, ch_seq, prop_payload.GetBitCount());
    else
        LOG_DEBUG("SendActorUpdateBunch ch=%u seq=%u (unreliable) prop_bits=%u",
                  ch_index, seq, prop_payload.GetBitCount());
}

void ProtocolHandler::Tick(uint64_t now_us) {
    for (auto& kv : sessions_) {
        auto& s = kv.second;

        // Drain scheduled sends (post-storm keepalives).
        if (!s.pending.empty()) {
            auto write = s.pending.begin();
            for (auto read = s.pending.begin(); read != s.pending.end(); ++read) {
                if (read->deadline_us <= now_us) {
                    socket_.SendToClient(kv.first, read->bytes.data(),
                                         static_cast<uint32_t>(read->bytes.size()));
                } else {
                    if (write != read) *write = std::move(*read);
                    ++write;
                }
            }
            s.pending.erase(write, s.pending.end());
        }

        // Every client packet gets acked within ~a server tick even when
        // nothing else is going out (retail acks each client packet in its
        // next ~30Hz packet). Doc sec 25.
        constexpr uint64_t kAckFlushIntervalUs = 33'000;
        if ((s.state == ConnectionState::AUTHENTICATING ||
             s.state == ConnectionState::ESTABLISHED) &&
            !s.queued_acks.empty() && now_us >= s.next_ack_flush_us) {
            FlushAcks(kv.first, s);
            s.next_ack_flush_us = now_us + kAckFlushIntervalUs;
        }

        // Move acks (doc sec 20): one unreliable ClientAckGoodMove per
        // ~server tick, acking only the newest move — retail does ~27/s.
        constexpr uint64_t kMoveAckIntervalUs = 33'000;
        if (s.state == ConnectionState::ESTABLISHED && s.pending_ack_timestamp != 0.0f &&
            now_us >= s.next_move_ack_us && s.channel_max_index.count(kPCChannel)) {
            util::BitWriter d;
            BuildClientAckGoodMoveRPC(d, s.pending_ack_timestamp);
            SendActorUpdateBunch(kv.first, kPCChannel, d, /*reliable=*/false);
            s.pending_ack_timestamp = 0.0f;
            s.next_move_ack_us = now_us + kMoveAckIntervalUs;
        }

        // Initial reload done (doc sec 30): AA_Weapon.ClientLoadAmmoMag(mag)
        // on the weapon's channel (retail frame 564).
        if (world_ && s.possessed_pawn && s.possessed_pawn->loaded_mag_serial != s.loaded_mag_serial_sent) {
            const game::ServerPawn* pawn = s.possessed_pawn;
            auto w_it = s.actor_channel.find(pawn->loaded_weapon);
            auto m_it = s.actor_channel.find(pawn->loaded_mag);
            auto max_it = w_it == s.actor_channel.end() ? s.channel_max_index.end()
                                                       : s.channel_max_index.find(w_it->second);
            if (w_it != s.actor_channel.end() && m_it != s.actor_channel.end() &&
                max_it != s.channel_max_index.end()) {
                // ClientLoadAmmoMag(byte FireModeNum, AA_AmmoContainerBase LoadedMag):
                // FireModeNum 0 is the default, so only its Send bit (0) goes
                // out -- without it the client reads the magazine ref as
                // FireModeNum=101 and rejects the load (doc sec 30).
                constexpr uint32_t kClientLoadAmmoMag = 52;   // AA_Weapon
                util::BitWriter d;
                d.WriteIntMax(kClientLoadAmmoMag, max_it->second);
                d.WriteBit(false);                            // FireModeNum: not sent (0)
                WriteRPCObjectParam(d, m_it->second);         // LoadedMag
                SendActorUpdateBunch(kv.first, w_it->second, d);
                LOG_INFO("Client %u: loaded magazine ch %u into weapon ch %u", kv.first,
                         m_it->second, w_it->second);
            }
            s.loaded_mag_serial_sent = pawn->loaded_mag_serial;
        }

        // Owner notices from the pawn (doc sec 33): a hit
        // (PC.ClientSendHitNotification(HitLocation, DamageType,
        // RemainingArmorPct) -- retail t=262.49), the ladder sling and the
        // vault finish (pawn RPCs).
        if (s.possessed_pawn) {
            game::ServerPawn* own = const_cast<game::ServerPawn*>(s.possessed_pawn);
            if (own->hit_serial != s.hit_sent) {
                s.hit_sent = own->hit_serial;
                util::BitWriter d;
                d.WriteIntMax(rep::AGP_PC::ClientSendHitNotification, kPCMaxIndex);
                d.WriteBit(true);
                WriteCompressedVector(d, own->hit_location[0], own->hit_location[1], own->hit_location[2]);
                d.WriteBit(true);                           // DamageType: class reference
                d.WriteBit(false);
                d.WriteIntMax(world_->ObjectIndex("AA3Baseline", own->hit_damage_class), 0x80000000);
                d.WriteBit(true);                           // RemainingArmorPct (retail 255 unarmoured)
                d.WriteByte(255);
                SendActorUpdateBunch(kv.first, kPCChannel, d);
            }
            // Incapacitated (doc sec 35, retail t=227.04): ClientIncapacitated()
            // on the pawn; on the controller ClientNotifyIncapacitated(True,
            // instigator), then NotifyPostureChange('EndWalk') and
            // ClientSetCameraMode('FreeCam').
            if (own->incap_serial != s.incap_sent && ChannelReady(s, kOwnPawnChannel)) {
                s.incap_sent = own->incap_serial;
                util::BitWriter p;
                p.WriteIntMax(rep::Pawn::ClientIncapacitated, rep::Pawn::kMaxIndex);
                SendActorUpdateBunch(kv.first, kOwnPawnChannel, p);
                util::BitWriter d;
                d.WriteIntMax(rep::AGP_PC::ClientNotifyIncapacitated, kPCMaxIndex);
                WriteRPCBoolParam(d, true);                 // bIncapacitateStart
                WriteRPCStrParam(d, own->incap_instigator); // sInstigatorName
                SendActorUpdateBunch(kv.first, kPCChannel, d);
                util::BitWriter e;
                BuildNotifyPostureChangeRPC(e, "EndWalk", true);
                BuildClientSetCameraModeRPC(e, "FreeCam");
                SendActorUpdateBunch(kv.first, kPCChannel, e);
            }
            if (own->sling_serial != s.sling_sent && ChannelReady(s, kOwnPawnChannel)) {
                s.sling_sent = own->sling_serial;
                util::BitWriter d;
                d.WriteIntMax(101, rep::Pawn::kMaxIndex);   // ClientSlingWeapon()
                SendActorUpdateBunch(kv.first, kOwnPawnChannel, d);
            }
            if (own->vault_finish_serial != s.vault_sent && ChannelReady(s, kOwnPawnChannel)) {
                s.vault_sent = own->vault_finish_serial;
                util::BitWriter d;
                d.WriteIntMax(100, rep::Pawn::kMaxIndex);   // ClientFinishVaultMantleMoveToTarget()
                SendActorUpdateBunch(kv.first, kOwnPawnChannel, d);
            }
        }

        // Hold breath forced to end (out of air / sights lowered): tell the
        // owner, ClientEndHoldBreath(bForceCancel=True) (doc sec 32).
        if (s.possessed_pawn && s.possessed_pawn->breath_end_serial != s.breath_end_sent) {
            s.breath_end_sent = s.possessed_pawn->breath_end_serial;
            util::BitWriter d;
            d.WriteIntMax(52, rep::Pawn::kMaxIndex);
            d.WriteBit(true);
            SendActorUpdateBunch(kv.first, kOwnPawnChannel, d);
        }

        // Reload notices (doc sec 31): ConfirmReload() right away, then the
        // advanced-inventory magazine count (retail frames 2414/2546).
        if (s.possessed_pawn) {
            const game::ServerPawn* pawn = s.possessed_pawn;
            if (pawn->confirm_reload_serial != s.confirm_reload_sent ||
                pawn->mag_count_serial != s.mag_count_sent) {
                util::BitWriter d;
                if (pawn->confirm_reload_serial != s.confirm_reload_sent)
                    d.WriteIntMax(rep::AGP_PC::ConfirmReload, kPCMaxIndex);
                if (pawn->mag_count_serial != s.mag_count_sent) {
                    BuildClientAdvancedInventoryInitializeAmmoMagazineCountRPC(d, pawn->ammo_inv_type, pawn->spare_mags);
                    d.WriteIntMax(rep::AGP_PC::ClientAdvancedInventoryUpdateAmmoMagazineCounts, kPCMaxIndex);
                }
                s.confirm_reload_sent = pawn->confirm_reload_serial;
                s.mag_count_sent = pawn->mag_count_serial;
                SendActorUpdateBunch(kv.first, kPCChannel, d);
            }
        }

        // Spawn (doc sec 29): when this player's pawn appears, run retail's
        // possession sequence once.
        if (world_ && s.state == ConnectionState::ESTABLISHED && s.actor_burst_sent) {
            const game::ServerPlayerController* pc = world_->FindPlayerController(kv.first);
            if (pc && pc->pawn && pc->pawn != s.possessed_pawn) PossessPawn(kv.first, s, *pc);
        }

        // Round phases (doc sec 28): each player on a team gets the current
        // phase's RPCs once -- including one who joins mid-round.
        if (world_ && s.state == ConnectionState::ESTABLISHED && s.actor_burst_sent &&
            s.phase_serial_seen != world_->PhaseSerial()) {
            const game::ServerPlayerController* pc = world_->FindPlayerController(kv.first);
            if (pc && pc->pri && pc->pri->OnPlayableTeam()) {
                SendPhaseRPCs(kv.first, world_->Phase());
                s.phase_serial_seen = world_->PhaseSerial();
            }
        }

        // Between rounds retail repeats ClientGotoState('WaitingForMatchStart')
        // every 2s, from the scoreboard until the next match start (live
        // capture t=229.44-253.44; doc sec 36).
        if (world_ && s.state == ConnectionState::ESTABLISHED && s.actor_burst_sent &&
            s.phase_serial_seen == world_->PhaseSerial()) {
            using P = game::World::MatchPhase;
            const P ph = world_->Phase();
            const game::ServerGameReplicationInfo* gri = world_->GetGRI();
            const game::ServerPlayerController* pc = world_->FindPlayerController(kv.first);
            const bool waiting = pc && pc->pri && pc->pri->OnPlayableTeam() &&
                (ph == P::kShowScoreboard ||
                 ((ph == P::kRoleSelection || ph == P::kDeploymentWarning) && gri && gri->i_current_round > 1));
            if (!waiting) {
                s.next_wait_state_us = 0;
            } else if (s.next_wait_state_us == 0) {
                s.next_wait_state_us = now_us + 2'000'000;
            } else if (now_us >= s.next_wait_state_us) {
                util::BitWriter d;
                BuildClientGotoStateRPC(d, "WaitingForMatchStart");
                SendActorUpdateBunch(kv.first, kPCChannel, d);
                s.next_wait_state_us += 2'000'000;
            }
        }

        // Other players' movement (doc sec 21): unreliable, self-paced at
        // 20Hz inside SyncRemotePlayers, so it runs every main-loop tick.
        if (world_ && s.state == ConnectionState::ESTABLISHED && s.has_last_sent_state)
            SyncRemotePlayers(kv.first, s, now_us, /*reliable_tick=*/false);

        // Post-JOIN replication tick. Cadence matches retail steady-state,
        // measured from GamePlayPCAP_Legit_Server.pcapng (127.0.0.1:8777 ->
        // client, t=100..104s): ~5 packets/sec, ~200ms spacing. Diffs every
        // actor this client has an open channel for (network/rep_encode.h)
        // and sends one update bunch per actor that actually changed;
        // otherwise falls back to a bare ack so the client's last
        // PacketSequence still gets acknowledged and the connection doesn't
        // look dead. WorldInfo isn't diffed per-tick (its fields essentially
        // never change after spawn in the current model). Each dirty actor
        // is its own packet — no multi-bunch batching yet, see doc sec 12.3.
        if (s.state == ConnectionState::ESTABLISHED &&
            s.next_keepalive_us != 0 && now_us >= s.next_keepalive_us) {
            bool sent_update = false;
            if (world_ && s.has_last_sent_state) {
                // PlayerStorageProxy: bIsDead goes False when the player
                // spawns (retail t=23.29). AA3's PlayerController.Use asks
                // PlayerProxy.IsDead() first and does nothing while it's
                // True -- the cause of dead doors/vault/ladders (traced in
                // the live client with gdb; doc sec 31).
                if (game::ServerPlayerController* ppc = world_->FindPlayerController(kv.first)) {
                    if (ppc->proxy && s.proxy_sent_valid && ChannelReady(s, kProxyChannel)) {
                        util::BitWriter d;
                        if (EncodePlayerStorageProxyRep(d, *ppc->proxy, &s.last_sent_proxy,
                                                        MakeRepContext(s, kProxyChannel, true)) > 0) {
                            SendActorUpdateBunch(kv.first, kProxyChannel, d);
                            sent_update = true;
                        }
                        s.last_sent_proxy = *ppc->proxy;
                    }
                }
                if (const game::ServerGameReplicationInfo* gri = world_->GetGRI()) {
                    util::BitWriter d;
                    if (ChannelReady(s, 4) && EncodeGRIRep(d, *gri, &s.last_sent_gri) > 0) {
                        SendActorUpdateBunch(kv.first, 4, d);
                        s.last_sent_gri = *gri;
                        sent_update = true;
                    }
                }
                for (size_t i = 0; i < world_->GetTeamCount() && i < 4; ++i) {
                    if (game::ServerTeamInfo* team = world_->GetTeam(i)) {
                        util::BitWriter d;
                        if (ChannelReady(s, static_cast<uint32_t>(6 + i)) &&
                            EncodeTeamInfoRep(d, *team, &s.last_sent_teams[i]) > 0) {
                            SendActorUpdateBunch(kv.first, static_cast<uint32_t>(6 + i), d);
                            s.last_sent_teams[i] = *team;
                            sent_update = true;
                        }
                    }
                }
                // Read via the world's own lookup, not a stale local from
                // OpenInitialActorChannels — the PC/PRI pointers there only
                // live for the duration of that one call.
                if (game::ServerPlayerController* pc =
                        world_->FindPlayerController(kv.first)) {
                    if (pc->pri) {
                        util::BitWriter d;
                        RepContext pri_ctx = MakeRepContext(s, 10, /*for_owner=*/true);
                        if (ChannelReady(s, 10) &&
                            EncodePRIRep(d, *pc->pri, &s.last_sent_pri, &pri_ctx) > 0) {
                            SendActorUpdateBunch(kv.first, 10, d);
                            s.last_sent_pri = *pc->pri;
                            sent_update = true;
                        }
                    }
                    // The pawn stays this connection's until it dies, or the
                    // level reset removes it (an unpossessed body at round end
                    // keeps its channel until then; doc sec 36).
                    if (s.possessed_pawn && !s.possessed_pawn->destroyed) {
                        const game::ServerPawn& own = *s.possessed_pawn;
                        util::BitWriter d;
                        if (ChannelReady(s, kOwnPawnChannel) && EncodePawnRep(d, own, &s.last_sent_pawn,
                                          MakeRepContext(s, kOwnPawnChannel, /*for_owner=*/true)) > 0) {
                            SendActorUpdateBunch(kv.first, kOwnPawnChannel, d);
                            s.last_sent_pawn = own;
                            sent_update = true;
                        }
                        if (own.torn_off) {             // death sent: retire the channel (doc sec 33)
                            SendDeathRPCs(kv.first, s);
                            RetireOwnPawn(kv.first, s);
                        }
                    } else if (s.possessed_pawn) {
                        RetireOwnPawn(kv.first, s);
                    }
                    if (pc->stat_manager) {
                        util::BitWriter st;
                        if (ChannelReady(s, 3) &&
                            EncodeSteamStatManagerRep(st, *pc->stat_manager, &s.last_sent_stats) > 0) {
                            SendActorUpdateBunch(kv.first, 3, st);
                            s.last_sent_stats = *pc->stat_manager;
                            sent_update = true;
                        }
                    }
                    util::BitWriter d;
                    if (ChannelReady(s, kPCChannel) && EncodePCRep(d, *pc, &s.last_sent_pc,
                                    MakeRepContext(s, kPCChannel, /*for_owner=*/true)) > 0) {
                        SendActorUpdateBunch(kv.first, kPCChannel, d);
                        s.last_sent_pc = *pc;
                        sent_update = true;
                    }
                }
            }
            if (world_ && s.has_last_sent_state)
                SyncRemotePlayers(kv.first, s, now_us, /*reliable_tick=*/true);
            // TeamInfo role structure / membership (doc sec 28).
            if (world_ && s.has_last_sent_state) {
                for (size_t i = 0; i < world_->GetTeamCount() && i < 4; ++i) {
                    game::ServerTeamInfo* team = world_->GetTeam(i);
                    uint32_t ch = static_cast<uint32_t>(6 + i);
                    if (!team || team->round_fields.empty() || !ChannelReady(s, ch)) continue;
                    util::BitWriter d;
                    if (EncodeDataFields(d, team->round_fields, kTeamInfoMaxIndex, s.team_round_sent[i],
                                         MakeRepContext(s, ch, /*for_owner=*/false)) > 0) {
                        SendActorUpdateBunch(kv.first, ch, d);
                        sent_update = true;
                    }
                }
            }
            // Round actors: role assignments, and references that weren't
            // mapped yet when the channel opened (doc sec 28).
            if (world_ && !s.data_actors.empty()) {
                for (const game::ServerRoundActor* a : s.data_actors) {
                    auto ch_it = s.actor_channel.find(a);
                    if (ch_it == s.actor_channel.end() || !ChannelReady(s, ch_it->second)) continue;
                    util::BitWriter d;
                    // A rifle's CurrentROF change goes with
                    // ClientNotifyRateOfFire(NextROF) in the same bunch
                    // (retail frames 39517/39772; doc sec 31).
                    if (a->class_name.rfind("AA_BulletWeapon", 0) == 0) {
                        const game::DataField* f = a->Get(58);
                        int rof = f ? static_cast<int>(f->v.i) : 0;
                        auto r_it = s.rof_sent.find(a);
                        if (r_it == s.rof_sent.end()) s.rof_sent[a] = rof;
                        else if (r_it->second != rof) {
                            r_it->second = rof;
                            d.WriteIntMax(68, a->max_index);    // ClientNotifyRateOfFire
                            d.WriteBit(rof != 0);               // NextROF: Send
                            if (rof != 0) d.WriteBits(static_cast<uint32_t>(rof), 2);
                        }
                    }
                    if (EncodeRoundActor(d, *a, s.round_sent[a],
                                         MakeRepContext(s, ch_it->second, /*for_owner=*/true)) > 0 ||
                        d.GetBitCount() > 0) {
                        SendActorUpdateBunch(kv.first, ch_it->second, d);
                        sent_update = true;
                    }
                }
            }
            if (!sent_update) {
                // Keepalive (KeepAliveTime=0.2). With no acks owed, re-ack the
                // client's newest packet so it still carries one.
                if (s.queued_acks.empty() && s.resend_acks.empty())
                    s.queued_acks.push_back(s.last_packet_id);
                TransmitPacket(kv.first, s, nullptr, AckMode::kQueued);
            }
            s.next_keepalive_us = now_us + 200'000;  // next tick in 200ms
        }
    }
}

RepContext ProtocolHandler::MakeRepContext(ClientSession& sess, uint32_t ch,
                                           bool for_owner) {
    RepContext ctx;
    ctx.for_owner = for_owner;
    ClientSession* sp = &sess;
    ctx.channel_of = [sp](const game::ServerActor* a) -> uint32_t {
        auto it = sp->actor_channel.find(a);
        return it == sp->actor_channel.end() ? 0 : it->second;
    };
    ctx.sent_refs = &sess.sent_refs[ch];
    if (world_) {
        const game::World* world = world_;
        ctx.object_index = [world](const std::string& pkg, uint32_t net) {
            return world->ObjectIndex(pkg, net);
        };
    }
    return ctx;
}

// A data actor's channel (inventory item, ES2 object) closed and forgotten,
// so the actor can be opened afresh later.
void ProtocolHandler::CloseDataActor(uint32_t client_id, ClientSession& sess,
                                     const game::ServerRoundActor* a) {
    auto it = sess.actor_channel.find(a);
    if (it != sess.actor_channel.end()) CloseActorChannel(client_id, it->second);
    sess.data_actors.erase(std::remove(sess.data_actors.begin(), sess.data_actors.end(), a),
                           sess.data_actors.end());
    sess.round_sent.erase(a);
    sess.rof_sent.erase(a);
}

// The owner's soldier is gone (death, level reset): its inventory channels
// close -- retail t=81.83 closes the slots, grenades, pouches, IFAK and
// InventoryManager with the death -- then the pawn's own channel.
void ProtocolHandler::RetireOwnPawn(uint32_t client_id, ClientSession& sess) {
    const game::ServerPawn* pawn = sess.possessed_pawn;
    if (!pawn) return;
    if (const auto* inventory = world_ ? world_->PawnInventory(pawn) : nullptr)
        for (const auto& it : *inventory) CloseDataActor(client_id, sess, it.get());
    if (sess.channel_max_index.count(kOwnPawnChannel)) CloseActorChannel(client_id, kOwnPawnChannel);
    sess.channel_max_index.erase(kOwnPawnChannel);
    sess.actor_channel.erase(pawn);
    sess.possessed_pawn = nullptr;
    sess.current_weapon_ch = 0;
}

// The owner's death notices, retail t=81.83 (GamePlayPCAP_Legit_Server):
// ClientPawnDied, the camera to FreeCam spectating, the scoreboard hidden,
// then ClientGotoState('Spectating') (doc sec 36).
void ProtocolHandler::SendDeathRPCs(uint32_t client_id, ClientSession& sess) {
    (void)sess;
    util::BitWriter d;
    BuildPCCallRPC(d, rep::PC::ClientPawnDied);
    BuildClientSetCameraModeRPC(d, "FirstPerson");
    BuildPCCallRPC(d, rep::PC::TS3ClientChangeChannel);
    BuildClientSetCameraModeRPC(d, "FreeCam");
    BuildClientSetSpectatorModeRPC(d, "FreeCam");
    BuildHideScoreboardRPC(d, true);
    BuildPCCallRPC(d, rep::PC::ClientUpdateSpectatorViewType);
    SendActorUpdateBunch(client_id, kPCChannel, d);
    util::BitWriter g;
    BuildClientUpdateScoreboardRPC(g, false);
    BuildClientGotoStateRPC(g, "Spectating");
    SendActorUpdateBunch(client_id, kPCChannel, g);
}

void ProtocolHandler::CloseActorChannel(uint32_t client_id, uint32_t ch_index) {
    auto it = sessions_.find(client_id);
    if (it == sessions_.end()) return;
    auto& sess = it->second;
    // Empty reliable bunch with bClose=1 (UChannel::Close -> SendBunch).
    util::BitWriter w;
    uint32_t ch_seq = ++sess.channel_reliable_seq[ch_index];
    WriteBunchHeader(w, ch_index, kActorChType, ch_seq, 0, /*bOpen=*/false,
                     /*bReliable=*/true, /*bClose=*/true);
    SendBunch(client_id, sess, std::move(w), ch_index, ch_seq);
    sess.channel_max_index.erase(ch_index);
    sess.channel_reliable_seq.erase(ch_index);
    sess.sent_refs.erase(ch_index);
    for (auto a = sess.actor_channel.begin(); a != sess.actor_channel.end();) {
        if (a->second == ch_index) a = sess.actor_channel.erase(a); else ++a;
    }
    LOG_INFO("CloseActorChannel client=%u ch=%u", client_id, ch_index);
}

void ProtocolHandler::SyncRemotePlayers(uint32_t client_id, ClientSession& sess,
                                        uint64_t now_us, bool reliable_tick) {
    // Movement cadence for other players' pawns: unreliable property
    // bunches at 20Hz while changing, plus a 1s keyframe (unreliable bunches
    // can be lost, and with diffing a lost "stopped here" update would
    // otherwise leave the pawn frozen in the wrong place).
    constexpr uint64_t kMoveIntervalUs = 50'000;
    constexpr uint64_t kKeyframeIntervalUs = 1'000'000;

    if (reliable_tick) {
        // 1) Pawns that are gone: a death's tear-off goes out before the
        // channel closes, so the body stays as a ragdoll (doc sec 33); a
        // level reset just closes it (doc sec 36). Players who left lose
        // their PRI channel too. The PRI stays while a player is between
        // soldiers -- retail keeps it for the scoreboard.
        for (auto it = sess.remote_players.begin(); it != sess.remote_players.end();) {
            RemotePlayerChannels& rp = it->second;
            const game::ServerPlayerController* opc = world_->FindPlayerController(it->first);
            if (rp.pawn && rp.pawn->torn_off && !rp.pawn->destroyed && ChannelReady(sess, rp.pawn_ch)) {
                util::BitWriter d;
                if (EncodePawnRep(d, *rp.pawn, &rp.last_pawn_rest,
                                  MakeRepContext(sess, rp.pawn_ch, /*for_owner=*/false)) > 0)
                    SendActorUpdateBunch(client_id, rp.pawn_ch, d);
                rp.last_pawn_rest = *rp.pawn;
            }
            const bool left = !opc || opc->pri != rp.pri;
            const bool pawn_gone = rp.pawn && (rp.pawn->torn_off || rp.pawn->destroyed || left ||
                                               (opc->pawn && opc->pawn != rp.pawn));
            if (pawn_gone) {
                LOG_INFO("Client %u: closing remote player %u's pawn (ch=%u)", client_id, it->first, rp.pawn_ch);
                CloseActorChannel(client_id, rp.pawn_ch);
                rp.pawn = nullptr;
                rp.pawn_ch = 0;
            }
            if (left) {
                LOG_INFO("Client %u: closing remote player %u's PRI (ch=%u)", client_id, it->first, rp.pri_ch);
                CloseActorChannel(client_id, rp.pri_ch);
                it = sess.remote_players.erase(it);
            } else {
                ++it;
            }
        }

        // 2) Open channels for players this connection hasn't seen yet. Only
        // once they've finished their own initial burst (so their PC/PRI
        // exist). PRI first: the Pawn's PlayerReplicationInfo ref then
        // resolves on its very first send.
        for (auto& other : sessions_) {
            if (other.first == client_id || sess.remote_players.count(other.first)) continue;
            if (!other.second.actor_burst_sent) continue;
            const game::ServerPlayerController* opc = world_->FindPlayerController(other.first);
            if (!opc || !opc->pri) continue;
            if (sess.next_free_channel + 1 > 0x800) {
                LOG_WARN("Client %u: out of actor channels for remote players", client_id);
                break;
            }
            RemotePlayerChannels rp;
            rp.pri = opc->pri;
            rp.pri_ch = sess.next_free_channel++;
            util::BitWriter pri_payload;
            RepContext rp_pri_ctx = MakeRepContext(sess, rp.pri_ch, /*for_owner=*/false);
            EncodePRIRep(pri_payload, *rp.pri, nullptr, &rp_pri_ctx);
            OpenActorChannel(client_id, rp.pri_ch, rp.pri->object_index, &pri_payload, rp.pri);
            sess.channel_max_index[rp.pri_ch] = kPRIMaxIndex;
            sess.actor_channel[rp.pri] = rp.pri_ch;
            rp.last_pri = *rp.pri;
            LOG_INFO("Client %u: opened remote player %u (PRI ch=%u)", client_id, other.first, rp.pri_ch);
            sess.remote_players[other.first] = rp;
        }

        // 2b) Their soldier, whenever they have one this connection hasn't.
        for (auto& kv : sess.remote_players) {
            RemotePlayerChannels& rp = kv.second;
            const game::ServerPlayerController* opc = world_->FindPlayerController(kv.first);
            if (rp.pawn || !opc || !opc->pawn || opc->pawn->torn_off || opc->pawn->destroyed) continue;
            if (sess.next_free_channel >= 0x800) break;
            rp.pawn = opc->pawn;
            rp.pawn_ch = sess.next_free_channel++;
            util::BitWriter pawn_payload;
            EncodePawnRep(pawn_payload, *rp.pawn, nullptr,
                          MakeRepContext(sess, rp.pawn_ch, /*for_owner=*/false));
            OpenActorChannel(client_id, rp.pawn_ch, rp.pawn->object_index, &pawn_payload, rp.pawn);
            sess.channel_max_index[rp.pawn_ch] = kPawnMaxIndex;
            sess.actor_channel[rp.pawn] = rp.pawn_ch;
            rp.last_pawn_rest = *rp.pawn;
            rp.last_pawn_move = *rp.pawn;
            rp.next_move_us = now_us + kMoveIntervalUs;
            rp.next_keyframe_us = now_us + kKeyframeIntervalUs;
            LOG_INFO("Client %u: opened remote player %u's pawn (ch=%u)", client_id, kv.first, rp.pawn_ch);
        }

        // 3) Reliable diffs: PRI (name/score/ping...) and the pawn's
        // non-movement state.
        for (auto& kv : sess.remote_players) {
            auto& rp = kv.second;
            util::BitWriter d;
            RepContext rp_pri_ctx = MakeRepContext(sess, rp.pri_ch, /*for_owner=*/false);
            if (ChannelReady(sess, rp.pri_ch) &&
                EncodePRIRep(d, *rp.pri, &rp.last_pri, &rp_pri_ctx) > 0) {
                SendActorUpdateBunch(client_id, rp.pri_ch, d);
                rp.last_pri = *rp.pri;
            }
            if (!rp.pawn) continue;
            util::BitWriter p;
            RepContext ctx = MakeRepContext(sess, rp.pawn_ch, /*for_owner=*/false);
            ctx.fields = RepContext::Fields::kNoMovement;
            if (ChannelReady(sess, rp.pawn_ch) &&
                EncodePawnRep(p, *rp.pawn, &rp.last_pawn_rest, ctx) > 0) {
                SendActorUpdateBunch(client_id, rp.pawn_ch, p);
                rp.last_pawn_rest = *rp.pawn;
            }
        }
        return;
    }

    // 4) Movement: unreliable, 20Hz, keyframe every 1s.
    for (auto& kv : sess.remote_players) {
        auto& rp = kv.second;
        if (!rp.pawn || now_us < rp.next_move_us) continue;
        rp.next_move_us = now_us + kMoveIntervalUs;
        RepContext ctx = MakeRepContext(sess, rp.pawn_ch, /*for_owner=*/false);
        ctx.fields = RepContext::Fields::kMovementOnly;
        if (now_us >= rp.next_keyframe_us) {
            ctx.force_movement = true;
            rp.next_keyframe_us = now_us + kKeyframeIntervalUs;
        }
        util::BitWriter m;
        if (EncodePawnRep(m, *rp.pawn, &rp.last_pawn_move, ctx) > 0) {
            SendActorUpdateBunch(client_id, rp.pawn_ch, m, /*reliable=*/false);
            rp.last_pawn_move = *rp.pawn;
        }
    }
}

} // namespace network
} // namespace aa3
