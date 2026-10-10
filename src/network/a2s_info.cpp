#include "network/a2s_info.h"
#include "util/logging.h"
#include <cstring>

namespace aa3 {
namespace network {

A2SHandler::A2SHandler(SocketManager& socket)
    : socket_(socket), player_count_(0), max_players_(32), bot_count_(0) {
    server_name_ = "AA3 Server";  // these should come from the config, and shouldn't be hardcoded
    map_name_ = "Bridge_night_clear";
    game_name_ = "America's Army 3";
}

A2SHandler::~A2SHandler() {
}

void A2SHandler::OnQueryReceived(const struct sockaddr_in& addr, const uint8_t* data, uint32_t len) {
    if (len < 5) return;

    // A2S packets start with 0xFFFFFFFF
    if (data[0] != 0xFF || data[1] != 0xFF || data[2] != 0xFF || data[3] != 0xFF) {
        return;
    }

    uint8_t type = data[4];
    if (type == 'T') { // A2S_INFO request: "TSource Engine Query\0"
        HandleInfoRequest(addr);
    }
}

void A2SHandler::SetServerInfo(const std::string& name, const std::string& map, const std::string& game) {
    server_name_ = name;
    map_name_ = map;
    game_name_ = game;
}

void A2SHandler::SetPlayerCount(uint8_t players, uint8_t max_players, uint8_t bots) {
    player_count_ = players;
    max_players_ = max_players;
    bot_count_ = bots;
}

void A2SHandler::HandleInfoRequest(const struct sockaddr_in& addr) {
    std::vector<uint8_t> response;

    // Header: -1 (0xFFFFFFFF)
    response.push_back(0xFF);
    response.push_back(0xFF);
    response.push_back(0xFF);
    response.push_back(0xFF);

    // Reply type: 'I' (0x49)
    response.push_back(0x49);

    // Protocol version
    response.push_back(17);

    // Server Name (null terminated)
    for (char c : server_name_) response.push_back(c);
    response.push_back(0);

    // Map Name (null terminated)
    for (char c : map_name_) response.push_back(c);
    response.push_back(0);

    // Folder (null terminated) - "AA3"
    const std::string folder = "AA3";
    for (char c : folder) response.push_back(c);
    response.push_back(0);

    // Game Name (null terminated)
    for (char c : game_name_) response.push_back(c);
    response.push_back(0);

    // Steam App ID (short) - 13180
    uint16_t app_id = 13180;
    response.push_back(app_id & 0xFF);
    response.push_back((app_id >> 8) & 0xFF);

    // Player Count
    response.push_back(player_count_);

    // Max Players
    response.push_back(max_players_);

    // Bot Count
    response.push_back(bot_count_);

    // Server Type: 'd' for dedicated
    response.push_back('d');

    // Environment: 'l' for linux
    response.push_back('l');

    // Visibility: 0 for public
    response.push_back(0);

    // VAC: 0 for disabled (until implemented)
    response.push_back(0);

    // Version (null terminated)
    const std::string version = "1.0.0.0";
    for (char c : version) response.push_back(c);
    response.push_back(0);

    // Extra Data Flag (EDF) - 0
    response.push_back(0);

    socket_.SendQueryResponse(addr, response.data(), static_cast<uint32_t>(response.size()));
    LOG_DEBUG("Sent A2S_INFO response to requester");
}

} // namespace network
} // namespace aa3
