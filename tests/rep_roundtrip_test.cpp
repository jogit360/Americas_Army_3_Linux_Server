// Round-trip check for the property/RPC encoders against the TOOL's layout,
// not against the encoders' own constants: each RepIndex read back is
// looked up in `aa3_build_manifests --rep-layout` output (passed on argv)
// and decoded using that row's property type. A wrong RepIndex, wrong
// MaxIndex (RIM width), or wrong value width desyncs the stream and fails.
//
//   rep_roundtrip_test <AA_Pawn layout.txt> <AGP_PlayerController layout.txt>
//
// Generate the layout files with:
//   aa3_build_manifests --rep-layout clean_packages/AA3Baseline.u AA_Pawn
//       --pkg-dir clean_packages > pawn.txt
//   aa3_build_manifests --rep-layout clean_packages/AA3Game.u AGP_PlayerController
//       --pkg-dir clean_packages > pc.txt
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include "game/server_actors.h"
#include "network/rep_encode.h"
#include "network/rpc_encode.h"
#include "util/bits.h"

using namespace aa3;

namespace {

struct Row { std::string type, sub, name; };
struct Layout { uint32_t max_index = 0; std::map<uint32_t, Row> rows; };

Layout LoadLayout(const char* path) {
    Layout L;
    std::ifstream f(path);
    std::string line;
    std::regex mx(R"(^\s+MaxIndex=(\d+))");
    std::smatch m;
    bool in_table = false;
    while (std::getline(f, line)) {
        if (line.find("Flattened wire fields") != std::string::npos) { in_table = true; continue; }
        if (std::regex_search(line, m, mx)) { L.max_index = std::stoul(m[1]); continue; }
        if (!in_table) continue;
        // Row: <idx> <type> [<sub-type>] <Decl.Name>[()] ... -- the qualified
        // name is the only token containing '.', so the sub-type is optional.
        std::istringstream ss(line);
        std::string idx, type, tok;
        if (!(ss >> idx >> type) || idx.find_first_not_of("0123456789") != std::string::npos) continue;
        Row r{type, "", ""};
        while (ss >> tok) {
            if (tok.find('.') != std::string::npos) {
                r.name = tok.substr(0, tok.find('('));
                break;
            }
            r.sub = tok;
        }
        L.rows[std::stoul(idx)] = r;
    }
    return L;
}

int g_fail = 0;
uint32_t g_last_ref = 0;  // last decoded object-ref / role value
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

void ReadCompressedVector(util::BitReader& r) {
    uint32_t bits = r.ReadIntMax(20);
    uint32_t max_val = 1u << (bits + 2);
    for (int i = 0; i < 3; ++i) r.ReadIntMax(max_val);
}
void ReadCompressedRotator(util::BitReader& r) {
    for (int i = 0; i < 3; ++i) if (r.ReadBit()) r.ReadByte();
}
std::string ReadStr(util::BitReader& r) {
    uint32_t n = r.ReadInt();
    std::string s;
    for (uint32_t i = 0; i < n; ++i) { char c = (char)r.ReadByte(); if (c) s += c; }
    return s;
}

// Decodes one property value by its tool-reported type. Returns false for a
// type this harness doesn't know how to size (test then fails loudly).
bool ReadPropValue(util::BitReader& r, const Row& row) {
    const std::string& t = row.type;
    if (t == "FloatProperty" || t == "IntProperty") { r.ReadInt(); return true; }
    if (t == "BoolProperty") { r.ReadBit(); return true; }
    if (t == "StrProperty") { ReadStr(r); return true; }
    if (t == "NameProperty") { ReadStr(r); r.ReadInt(); return true; }  // AA3 FName: FString + INT32
    if (t == "ObjectProperty" || t == "ClassProperty") {
        // SerializeObject: bDynamic=1 -> channel RIM(0x800), else static index.
        uint32_t v = r.ReadBit() ? r.ReadIntMax(0x800) : r.ReadIntMax(0x80000000);
        g_last_ref = v;
        return true;
    }
    if (t == "ByteProperty") {
        if (row.sub.empty()) { r.ReadByte(); return true; }
        if (row.sub == "eHitLocation") { r.ReadBits(5); return true; }  // doc sec 12.5
        if (row.sub == "ENetRole") { g_last_ref = r.ReadBits(2); return true; }  // doc sec 11.2
        if (row.sub == "EPhysics") { r.ReadBits(4); return true; }
        return false;
    }
    if (t == "StructProperty") {
        if (row.sub == "Vector") { ReadCompressedVector(r); return true; }
        if (row.sub == "Rotator") { ReadCompressedRotator(r); return true; }
        if (row.sub == "AA_PawnHitEffectInfo") { ReadCompressedVector(r); ReadCompressedRotator(r); return true; }
        if (row.sub == "StateGraphState") { r.ReadBits(32); r.ReadBit(); return true; }  // 2x2 bytes + bool
        return false;
    }
    return false;
}

// Decodes one encoder output against the tool layout; returns field name ->
// last decoded ref/role value (0 for other types). Fails on any desync.
std::map<std::string, uint32_t> DecodeAll(const Layout& L, const util::BitWriter& w, const char* what) {
    std::map<std::string, uint32_t> seen;
    util::BitReader r(w.GetData().data(), w.GetBitCount());
    while (r.GetPos() < w.GetBitCount()) {
        uint32_t idx = r.ReadIntMax(L.max_index);
        auto it = L.rows.find(idx);
        if (it == L.rows.end() || it->second.type == "Function") {
            CHECK(false, "%s: RepIndex %u is not a property in the tool layout", what, idx);
            return seen;
        }
        g_last_ref = 0;
        // Static array element: BYTE index after the RepIndex (UnChan.cpp:1512).
        const std::string& nm = it->second.name;
        if (nm.size() > 2 && nm.compare(nm.size() - 2, 2, "[]") == 0) r.ReadByte();
        if (!ReadPropValue(r, it->second)) {
            CHECK(false, "%s: harness can't size %s (%s %s)", what, it->second.name.c_str(),
                  it->second.type.c_str(), it->second.sub.c_str());
            return seen;
        }
        seen[it->second.name] = g_last_ref;
    }
    CHECK(r.GetPos() == w.GetBitCount(), "%s: consumed %u of %u bits", what, r.GetPos(), w.GetBitCount());
    return seen;
}

network::RepContext Ctx(bool owner, std::map<const game::ServerActor*, uint32_t>& chans,
                        std::map<uint32_t, uint32_t>& refs) {
    network::RepContext c;
    c.for_owner = owner;
    c.channel_of = [&chans](const game::ServerActor* a) -> uint32_t {
        auto it = chans.find(a);
        return it == chans.end() ? 0 : it->second;
    };
    c.sent_refs = &refs;
    return c;
}

void TestPawn(const Layout& L) {
    CHECK(L.max_index == network::kPawnMaxIndex, "Pawn MaxIndex tool=%u encoder=%u",
          L.max_index, network::kPawnMaxIndex);
    game::ServerPlayerReplicationInfo pri;
    game::ServerPlayerController pc;
    game::ServerPawn pawn;
    pawn.pri = &pri; pawn.controller = &pc; pawn.last_known_pri = &pri;
    pawn.body_soldier_name = "SoldierBody_01"; pawn.body_team_index = 1;
    pawn.location[0] = 11385; pawn.location[1] = -3308; pawn.location[2] = 30;
    pawn.velocity[0] = 12; pawn.velocity[1] = -3;
    pawn.incapacitation_loc = 7; pawn.stamina = 55.5f;
    std::map<const game::ServerActor*, uint32_t> chans{{&pri, 10}, {&pc, 11}, {&pawn, 12}};

    // Owner: roles say AutonomousProxy, refs are channels, no Velocity.
    {
        std::map<uint32_t, uint32_t> refs;
        util::BitWriter w;
        network::EncodePawnRep(w, pawn, nullptr, Ctx(true, chans, refs));
        auto f = DecodeAll(L, w, "Pawn(owner)");
        CHECK(f.count("Actor.Role") && f["Actor.Role"] == 2, "owner Role != AutonomousProxy");
        CHECK(f["Actor.RemoteRole"] == 3, "owner RemoteRole != Authority");
        CHECK(f["Pawn.PlayerReplicationInfo"] == 10, "PRI ref != ch 10 (got %u)", f["Pawn.PlayerReplicationInfo"]);
        CHECK(f["Pawn.Controller"] == 11, "Controller ref != ch 11");
        CHECK(f.count("Pawn.Health") && f.count("Pawn.GroundSpeed"), "owner missing owner-only fields");
        CHECK(!f.count("Actor.Velocity") && !f.count("Pawn.bIsCrouched") && !f.count("Pawn.RemoteViewPitch"),
              "owner got non-owner-only fields");
        std::printf("Pawn(owner): %zu fields, %u bits, decoded against tool layout\n", f.size(), w.GetBitCount());
    }
    // Non-owner: SimulatedProxy, movement + non-owner fields, no owner-only.
    {
        std::map<uint32_t, uint32_t> refs;
        util::BitWriter w;
        network::EncodePawnRep(w, pawn, nullptr, Ctx(false, chans, refs));
        auto f = DecodeAll(L, w, "Pawn(other)");
        CHECK(f["Actor.Role"] == 1, "non-owner Role != SimulatedProxy");
        for (const char* must : {"Actor.Location", "Actor.Rotation", "Actor.Velocity", "Actor.Physics",
                                 "Pawn.bIsCrouched", "Pawn.RemoteViewPitch", "AA_Pawn.sBodySoldierName"})
            CHECK(f.count(must), "non-owner missing %s", must);
        CHECK(!f.count("Pawn.Health") && !f.count("Pawn.Controller") && !f.count("Pawn.GroundSpeed"),
              "non-owner got owner-only fields");
        std::printf("Pawn(other): %zu fields, %u bits, decoded against tool layout\n", f.size(), w.GetBitCount());

        // Movement-only diff after a move: exactly Location (+Velocity/Rotation if changed).
        game::ServerPawn prev = pawn;
        pawn.location[0] += 40;
        network::RepContext c = Ctx(false, chans, refs);
        c.fields = network::RepContext::Fields::kMovementOnly;
        util::BitWriter m;
        uint32_t n = network::EncodePawnRep(m, pawn, &prev, c);
        auto mf = DecodeAll(L, m, "Pawn(move)");
        CHECK(n == 1 && mf.count("Actor.Location"), "movement diff sent %u fields", n);
    }
}

void TestPCRefs(const Layout& L) {
    game::ServerPlayerReplicationInfo pri;
    game::ServerPawn pawn;
    game::ServerPlayerController pc;
    pc.pri = &pri; pc.pawn = &pawn;
    std::map<const game::ServerActor*, uint32_t> chans{{&pri, 10}};  // pawn not open yet
    std::map<uint32_t, uint32_t> refs;
    util::BitWriter w;
    network::EncodePCRep(w, pc, nullptr, Ctx(true, chans, refs));
    auto f = DecodeAll(L, w, "PC(initial)");
    CHECK(f["Controller.PlayerReplicationInfo"] == 10 && f.count("Controller.Pawn") &&
          f["Controller.Pawn"] == 0, "PC initial refs wrong");
    chans[&pawn] = 12;  // pawn channel opens -> next diff must re-send Pawn
    util::BitWriter w2;
    uint32_t n = network::EncodePCRep(w2, pc, &pc, Ctx(true, chans, refs));
    auto f2 = DecodeAll(L, w2, "PC(retry)");
    CHECK(n == 1 && f2["Controller.Pawn"] == 12, "PC.Pawn not re-sent once its channel opened");
    std::printf("PC: NULL-then-retry reference resolution works (Pawn -> ch 12)\n");
}

void ExpectRPC(const Layout& L, const util::BitWriter& w, const char* fn) {
    util::BitReader r(w.GetData().data(), w.GetBitCount());
    uint32_t idx = r.ReadIntMax(L.max_index);
    auto it = L.rows.find(idx);
    std::string got = it == L.rows.end() ? "<none>" : it->second.name;
    CHECK(got == fn, "RPC: expected %s, RepIndex %u resolves to %s", fn, idx, got.c_str());
}

void TestPCRPCs(const Layout& L) {
    CHECK(L.max_index == network::kPCMaxIndex, "PC MaxIndex tool=%u encoder=%u",
          L.max_index, network::kPCMaxIndex);
    { util::BitWriter w; network::BuildClientMessageRPC(w, "hi"); ExpectRPC(L, w, "PlayerController.ClientMessage"); }
    { util::BitWriter w; network::BuildClientSetRotationRPC(w, 0, 16384, 0, false); ExpectRPC(L, w, "Controller.ClientSetRotation"); }
    { util::BitWriter w; network::BuildClientSwitchToBestWeaponRPC(w, false); ExpectRPC(L, w, "Controller.ClientSwitchToBestWeapon"); }
    { util::BitWriter w; network::BuildTeamMessageRPC(w, 10, "x"); ExpectRPC(L, w, "PlayerController.TeamMessage"); }
    { util::BitWriter w; network::BuildClientSetProgressMessageRPC(w, 1, "m", false); ExpectRPC(L, w, "PlayerController.ClientSetProgressMessage"); }
    { util::BitWriter w; network::BuildClientSetViewTargetRPC(w, 1234); ExpectRPC(L, w, "PlayerController.ClientSetViewTarget"); }
    std::printf("PC: 6 RPCs resolve to the right function in the tool layout\n");
}

}  // namespace

// Retail round-end stat push (GamePlayPCAP_Legit_Server.pcapng, server->
// client Ch=3 at t=370s, 2191 bits): only AA_MP_PlayerStatistics changed.
// Loading retail's decoded values into our model must reproduce the bunch
// payload bit for bit (doc sec 23). Stored as the set-bit positions.
void TestStatsVsRetail() {
    static const int kSetBits[] = {3,4,5,6,10,11,12,13,15,17,18,19,20,21,22,23,24,27,29,30,
                                   32,33,34,37,39,41,42,43,1407,1447,2186};
    game::ServerSteamStatManager prev, cur;
    cur.round.bDataValid = true;
    cur.round.bDirty = true;
    uint32_t stamp_bits = 1322515832u;  // fpStampLastUpdate, raw float bits
    std::memcpy(&cur.round.fpStampLastUpdate, &stamp_bits, 4);
    cur.round.iM4A1ShotsFired = 29;
    cur.round.iObjectivesCompleted = 1;
    cur.round.iClassicExtraction = 1;
    cur.round.iES2ObjectsInRound = 8;
    util::BitWriter w;
    uint32_t n = network::EncodeSteamStatManagerRep(w, cur, &prev);
    std::string ours(w.GetBitCount(), '0'), retail(2191, '0');
    const auto& d = w.GetData();
    for (uint32_t i = 0; i < w.GetBitCount(); ++i) if ((d[i >> 3] >> (i & 7)) & 1) ours[i] = '1';
    for (int i : kSetBits) retail[i] = '1';
    CHECK(n == 1 && ours == retail, "stats: round push != retail (%u fields, %u bits)", n, w.GetBitCount());
    if (n == 1 && ours == retail)
        std::printf("Stats: round-end push (2191 bits) bit-identical to retail t=370s\n");
}

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s <pawn layout> <pc layout>\n", argv[0]); return 2; }
    TestPawn(LoadLayout(argv[1]));
    Layout pc = LoadLayout(argv[2]);
    TestPCRefs(pc);
    TestPCRPCs(pc);
    TestStatsVsRetail();
    std::printf(g_fail ? "%d FAILURE(S)\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
