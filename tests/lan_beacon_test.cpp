// LAN beacon wire checks against real retail bytes (doc sec 24).
//   lan_beacon_test <tests/fixtures dir>
// Fixtures: retail_lan_state_reply.bin (the retail server's actual
// get_server_state reply) and lan_state_request.bin (a request built by the
// verified aa3-backend codec, same format the client sends).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "network/lan_beacon.h"

using aa3::network::LanBeacon;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static std::vector<uint8_t> Load(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s <fixtures dir>\n", argv[0]); return 2; }
    std::string dir = argv[1];

    // 1. Our decoder reads the real retail reply.
    auto retail = Load(dir + "/retail_lan_state_reply.bin");
    LanBeacon::AcpuMessage m;
    CHECK(!retail.empty() && LanBeacon::ParseAcpu(retail.data(), retail.size(), &m), "retail reply parse");
    std::vector<std::string> rnames, rvalues;
    int rcode = -1;
    for (auto& f : m.fields) {
        if (f.key == "attributeNames") rnames = f.arr;
        if (f.key == "attributeValues") rvalues = f.arr;
        if (f.key == "resultCode") rcode = f.i;
    }
    CHECK(m.fields.size() == 3 && rnames.size() == 36 && rvalues.size() == 36 && rcode == 1,
          "retail reply shape (fields=%zu names=%zu values=%zu rc=%d)", m.fields.size(),
          rnames.size(), rvalues.size(), rcode);
    CHECK((m.message_id & 0xff) == 2 && (m.message_id & 0x0200), "retail msg id/compress flag");

    // 2. Our attribute set: same 36 names, same order as retail.
    LanBeacon::ServerInfo info;
    info.ip = "192.168.1.138";
    info.map_name = "Bridge_night_clear";
    auto ours = LanBeacon::Attributes(info);
    bool same = ours.size() == rnames.size();
    for (size_t i = 0; same && i < ours.size(); ++i) same = ours[i].first == rnames[i];
    CHECK(same, "attribute names/order differ from retail");
    // Values the client needs that must match retail semantics exactly.
    auto val = [&](const std::string& k, const std::vector<std::pair<std::string, std::string>>& a) {
        for (auto& p : a) if (p.first == k) return p.second;
        return std::string("<missing>");
    };
    for (const char* k : {"serverVersion", "serverIpAddress", "serverHostPort", "serverQueryPort",
                          "serverMapName", "serverMaxPlayers", "serverMissionType",
                          "serverMissionSize", "serverMissionTourID", "serverMissionVariant"}) {
        std::string rv;
        for (size_t i = 0; i < rnames.size(); ++i) if (rnames[i] == k) rv = rvalues[i];
        CHECK(val(k, ours) == rv, "%s: ours '%s' retail '%s'", k, val(k, ours).c_str(), rv.c_str());
    }

    // 3. Checksum: matches all three real samples, and our reply carries a
    //    valid one over its own payload.
    {
        auto hex = [](const char* h) {
            std::vector<uint8_t> v;
            for (size_t i = 0; h[i] && h[i + 1]; i += 2)
                v.push_back(static_cast<uint8_t>(std::stoi(std::string(h + i, 2), nullptr, 16)));
            return v;
        };
        auto cap1 = hex("4a35cf17020002000100693c176143671f2b3c516c4d420a504b317903112068");
        auto cap2 = hex("4a35a119020002000100693c176143671f2b3c516c4d42075d2a57305960526d0b2e3d");
        for (auto* pk : {&cap1, &cap2, &retail}) {
            uint16_t want = static_cast<uint16_t>((*pk)[2] | ((*pk)[3] << 8));
            uint16_t got = LanBeacon::FragmentChecksum(pk->data() + 10, pk->size() - 10);
            CHECK(got == want, "checksum %04x != real %04x", got, want);
        }
    }
    auto reply = LanBeacon::BuildStateReply(info);
    LanBeacon::AcpuMessage back;
    CHECK(LanBeacon::ParseAcpu(reply.data(), reply.size(), &back) && back.fields.size() == 3 &&
          back.fields[0].arr.size() == 36 && back.fields[2].i == 1,
          "our reply does not round-trip");
    CHECK((reply[2] | (reply[3] << 8)) ==
              LanBeacon::FragmentChecksum(reply.data() + 10, reply.size() - 10),
          "our reply checksum invalid");

    // 4. The request format parses (playerName).
    auto req = Load(dir + "/lan_state_request.bin");
    LanBeacon::AcpuMessage rq;
    CHECK(LanBeacon::ParseAcpu(req.data(), req.size(), &rq) && rq.fields.size() == 1 &&
          rq.fields[0].key == "playerName" && rq.fields[0].s == "Tester", "request parse");

    // 5. Announce string matches retail's form.
    info.details_port = 9002;
    CHECK(LanBeacon::BuildAnnounce(info) == "34983~0~192.168.1.138~9002~3.3", "announce '%s'",
          LanBeacon::BuildAnnounce(info).c_str());

    std::printf("retail reply: %zu attributes decoded; ours: %zu bytes, names/order identical\n",
                rnames.size(), reply.size());
    std::printf(g_fail ? "%d FAILURE(S)\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
