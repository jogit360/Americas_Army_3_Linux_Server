// Runs network::DecodePcRpc over real retail client->server PC-channel
// bunches and checks they decode to exactly their BunchDataBits, plus a
// ClientAckGoodMove encode/decode round trip.
//
//   rpc_decode_pcap_test <bunches.txt>
//
// bunches.txt: one `<bits> <hex>` line per bunch, from
// aa3-tools/map_manifest/decode_servermove.py's export_bunches() run on
// GamePlayPCAP_Legit_Server.pcapng (client port 47026, Ch=2). Retail's PC
// class is AGP_PlayerController, so this decodes against its MaxIndex.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "network/rep_layout_gen.h"
#include "network/rpc_decode.h"
#include "network/rpc_encode.h"
#include "util/bits.h"

using namespace aa3;

int main(int argc, char** argv) {
    int fail = 0;

    // ClientAckGoodMove round trip.
    {
        util::BitWriter w;
        network::BuildClientAckGoodMoveRPC(w, 53.7264f);
        util::BitReader r(w.GetData().data(), w.GetBitCount());
        uint32_t idx = r.ReadIntMax(network::rep::PC::kMaxIndex);
        bool send = r.ReadBit();
        uint32_t bits = r.ReadInt();
        float ts;
        std::memcpy(&ts, &bits, 4);
        if (idx != network::rep::PC::ClientAckGoodMove || !send || ts != 53.7264f ||
            r.GetPos() != w.GetBitCount()) {
            std::printf("FAIL: ClientAckGoodMove round trip (idx=%u ts=%f)\n", idx, ts);
            ++fail;
        } else {
            std::printf("ClientAckGoodMove: %u bits, round-trips exactly\n", w.GetBitCount());
        }
    }

    // Retail toggle-stance reply, frame 16526 (server->client Ch=2, 169
    // bits): the 160 bits after RepIndex 309. Our 4-RPC reply must match
    // bit for bit (RIM(309/310/311) is 9 bits under both MaxIndex 473
    // and retail's 491, so the whole bunch is comparable).
    {
        const char* kRetailAfterIndex =
            "1000100000000000000000000000000001010001001110110001001101110101"
            "0100001100011011011010110000000000000000000000000000000000000000"
            "01111011001101101100111110110011";
        util::BitWriter w;
        network::BuildNotifyPostureChangeRPC(w, "EndWalk", true);
        network::BuildNotifyTacticalCombatPostureChangeRPC(w, true);
        network::BuildNotifyWeaponReadyChangeRPC(w, true);
        network::BuildNotifyTacticalCombatPostureChangeRPC(w, true);
        util::BitReader r(w.GetData().data(), w.GetBitCount());
        uint32_t idx = r.ReadIntMax(network::rep::PC::kMaxIndex);
        std::string ours;
        while (r.GetPos() < w.GetBitCount()) ours += r.ReadBit() ? '1' : '0';
        if (idx != network::rep::PC::NotifyPostureChange || ours != kRetailAfterIndex ||
            w.GetBitCount() != 169) {
            std::printf("FAIL: toggle-stance reply != retail frame 16526 (%u bits)\n%s\n%s\n",
                        w.GetBitCount(), ours.c_str(), kRetailAfterIndex);
            ++fail;
        } else {
            std::printf("Toggle-stance reply: 169 bits, bit-identical to retail frame 16526\n");
        }
    }

    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <bunches.txt>\n", argv[0]);
        return 2;
    }
    std::ifstream f(argv[1]);
    std::string line;
    std::map<std::string, int> exact;
    int bunches = 0, fully = 0, partial = 0;
    std::map<uint32_t, int> unknown;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        uint32_t nbits;
        std::string hex;
        if (!(ss >> nbits >> hex)) continue;
        std::vector<uint8_t> buf(hex.size() / 2);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = static_cast<uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
        ++bunches;
        util::BitReader r(buf.data(), nbits);
        bool ok = true;
        while (r.GetPos() < nbits) {
            uint32_t idx = 0;
            network::DecodedRpc rpc;
            auto res = network::DecodePcRpc(r, nbits, network::rep::AGP_PC::kMaxIndex, &idx, &rpc);
            if (res != network::RpcDecodeResult::kOk) {
                ok = false;
                if (res == network::RpcDecodeResult::kUnknownIndex) ++unknown[idx];
                break;
            }
            ++exact[rpc.name];
        }
        if (ok && r.GetPos() == nbits) ++fully; else ++partial;
    }
    std::printf("%d bunches: %d decoded to exactly BDB, %d stopped early (%.2f%% full)\n",
                bunches, fully, partial, bunches ? 100.0 * fully / bunches : 0.0);
    for (auto& kv : exact) std::printf("  %-28s %d\n", kv.first.c_str(), kv.second);
    std::printf("  unknown RepIndex (stopped there):");
    for (auto& kv : unknown) std::printf(" %u:%d", kv.first, kv.second);
    std::printf("\n");
    // The retail capture is ~99.99%% table-covered (doc sec 22); a big drop
    // means a decoder or RepIndex regression.
    if (bunches == 0 || fully < bunches * 999 / 1000) {
        std::printf("FAIL: coverage below 99.9%%\n");
        ++fail;
    }
    std::printf(fail ? "%d FAILURE(S)\n" : "ALL PASS\n", fail);
    return fail ? 1 : 0;
}
