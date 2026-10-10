#ifndef AA3_NETWORK_LAN_BEACON_H
#define AA3_NETWORK_LAN_BEACON_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aa3 {
namespace network {

// AA3 in-game LAN server browser support (doc sec 24), from the retail
// server's real replies:
//  1. Client broadcasts "34983~1" to UDP 9001 (its log: "MBS LAN Server
//     Listener port: 9001, Client Listener port: 9000").
//  2. Server answers "34983~0~<ip>~<detailsPort>~3.3" to UDP 9000.
//  3. Client sends an ACPU get_server_state (message 2) to <ip>:<detailsPort>.
//  4. Server replies ACPU {attributeNames[36], attributeValues[36],
//     resultCode=1}, zlib-compressed, SSC-encrypted.
class LanBeacon {
public:
    struct ServerInfo {
        std::string ip;                 // advertised LAN address
        uint16_t game_port = 8777;
        uint16_t details_port = 9002;
        std::string host_name = "AA3 Server";
        std::string map_name;
        std::string mission_type = "co", mission_size = "large";
        std::string mission_tour = "inf", mission_variant = "ai";
        int max_players = 20, max_spectators = 4, player_count = 0;
    };

    ~LanBeacon();
    // Binds the query (9001) and details (9002) sockets. False if either fails.
    bool Start(uint16_t query_port = 9001, uint16_t client_port = 9000,
               uint16_t details_port = 9002);
    void SetInfoProvider(std::function<ServerInfo()> fn) { info_ = std::move(fn); }
    // Non-blocking: services every pending query on both sockets.
    void Poll();

    // ---- wire helpers (static, unit-tested against the retail fixture) ----
    static std::string BuildAnnounce(const ServerInfo& info);
    // The 36 (name, value) pairs, in retail's exact order.
    static std::vector<std::pair<std::string, std::string>> Attributes(const ServerInfo& info);
    // Header = u16 magic 0x354a | u16 FragmentChecksum(payload) | u8 2 |
    // u8 flags | u16 message | u16 field count. The client verifies the
    // checksum and silently drops (and re-requests) on mismatch (doc sec 24).
    static std::vector<uint8_t> BuildStateReply(const ServerInfo& info);
    // libaa3 acpu_compile @0x630d113b: over the encrypted fragment payload
    // (bytes after the 10-byte header), h=0x14fe as two bytes,
    // h[i%2] ^= key30[i%30] ^ data[i] ^ data[i-1] (no data[i-1] for i=0).
    // Verified against two real client requests and the retail reply.
    static uint16_t FragmentChecksum(const uint8_t* payload, size_t len);

    struct AcpuField {
        std::string key;
        uint8_t type = 0;                 // 2 int, 6 string, 7 string array
        int32_t i = 0;
        std::string s;
        std::vector<std::string> arr;
    };
    struct AcpuMessage {
        uint32_t txid = 0;
        uint16_t message_id = 0;          // low byte = id, 0x0200 = compressed
        std::vector<AcpuField> fields;
    };
    static bool ParseAcpu(const uint8_t* data, size_t len, AcpuMessage* out);
    // Picks this host's primary IPv4 (the one a default route would use).
    static std::string DetectLanIp();

private:
    int query_fd_ = -1;
    int details_fd_ = -1;
    uint16_t client_port_ = 9000;
    std::function<ServerInfo()> info_;
};

}  // namespace network
}  // namespace aa3

#endif  // AA3_NETWORK_LAN_BEACON_H
