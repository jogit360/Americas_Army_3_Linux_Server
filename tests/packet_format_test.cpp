// Quick standalone sanity test for the new packet writer. Builds three
// representative packets (ack-only ping, ack + small bunch, multi-ack tail)
// and dumps them so we can run them through the Python decoder.
// Build with:
//   g++ -std=c++17 -I include -I src test_packet_format.cpp src/util/bits.cpp -o /tmp/aa3_pkt_test
#include "util/bits.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace aa3::util;

static void dump(const char* label, const std::vector<uint8_t>& bytes) {
    printf("%-30s len=%zu  hex=", label, bytes.size());
    for (auto b : bytes) printf("%02x", b);
    printf("\n");
}

static std::vector<uint8_t> ping(uint32_t seq) {
    BitWriter w;
    w.WriteIntMax(seq, 0x4000);
    w.WriteTerminator();
    return w.GetData();
}

static std::vector<uint8_t> ack_packet(uint32_t seq, uint32_t acked) {
    BitWriter w;
    w.WriteIntMax(seq, 0x4000);
    w.WriteBit(1);                       // bIsAck
    w.WriteIntMax(acked, 0x4000);
    w.WriteTerminator();
    return w.GetData();
}

static std::vector<uint8_t> two_acks(uint32_t seq, uint32_t a, uint32_t b) {
    BitWriter w;
    w.WriteIntMax(seq, 0x4000);
    w.WriteBit(1);  w.WriteIntMax(a, 0x4000);
    w.WriteBit(1);  w.WriteIntMax(b, 0x4000);
    w.WriteTerminator();
    return w.GetData();
}

static std::vector<uint8_t> n_acks(uint32_t seq, std::initializer_list<uint32_t> acks) {
    BitWriter w;
    w.WriteIntMax(seq, 0x4000);
    for (uint32_t a : acks) { w.WriteBit(1); w.WriteIntMax(a, 0x4000); }
    w.WriteTerminator();
    return w.GetData();
}

// Build one storm packet as SendStormAndWelcome does: PacketSeq + ack + one
// reliable bunch on Ch=0 ChType=1 containing N concatenated FStrings.
static std::vector<uint8_t> storm_packet(uint32_t seq, uint32_t ch_seq,
                                         uint32_t ack_id,
                                         const std::vector<std::string>& strings) {
    uint32_t payload_bits = 0;
    for (auto& s : strings) payload_bits += 32 + (s.size() + 1) * 8;
    BitWriter w;
    w.WriteIntMax(seq, 0x4000);
    w.WriteBit(1); w.WriteIntMax(ack_id, 0x4000);   // Ack element
    // Bunch header (closed reliable bunch on Ch=0 ChType=1)
    w.WriteBit(0); w.WriteBit(0); w.WriteBit(1);    // bIsAck=0 OC=0 bReliable=1
    w.WriteIntMax(0,  0x800);                       // ChIndex=0
    w.WriteIntMax(ch_seq, 0x400);                   // ChSequence
    w.WriteIntMax(1, 8);                            // ChType=1
    w.WriteIntMax(payload_bits, 4096);              // BunchDataBits
    for (auto& s : strings) {
        w.WriteInt(static_cast<uint32_t>(s.size() + 1));
        for (char c : s) w.WriteByte(static_cast<uint8_t>(c));
        w.WriteByte(0);
    }
    w.WriteTerminator();
    return w.GetData();
}

int main() {
    dump("ping seq=60",            ping(60));
    dump("ping seq=4",             ping(4));
    dump("ack seq=2 ack=0",        ack_packet(2, 0));
    dump("frame64 (seq=2,acks=0,58,59,60)", n_acks(2, {0, 58, 59, 60}));
    dump("frame65 (seq=3,acks=0,58,59,60)", n_acks(3, {0, 58, 59, 60}));
    dump("frame61 (seq=58,acks=0,1)",       n_acks(58, {0, 1}));
    dump("frame62 (seq=59,acks=1)",         n_acks(59, {1}));

    // Legit frame 4 packs FOUR USES in one bunch (legit BDB=3200 = 400B).
    std::vector<std::string> first4 = {
        "USES GUID=240780FD4F226E9ED9583EAC13976460 PKG=Core EXT=u FLAGS=44564488 GEN=1 BASEPKG=None",
        "USES GUID=0F002C2B458151E8E9B4FCBA1A0B740F PKG=Engine EXT=u FLAGS=44564488 GEN=1 BASEPKG=None",
        "USES GUID=1E3EC5834FD3A796613E08BB6973FE43 PKG=EngineFonts EXT=upk FLAGS=1 GEN=1 BASEPKG=Startup",
        "USES GUID=CE4FAEFE4CB339F370A2EAAD3E4F320F PKG=EngineMaterials EXT=upk FLAGS=1 GEN=1 BASEPKG=Startup",
    };
    dump("storm pkt#1 (4 USES)", storm_packet(1, 2, 0, first4));
    return 0;
}
