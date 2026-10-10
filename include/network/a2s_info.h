#ifndef AA3_A2S_INFO_H
#define AA3_A2S_INFO_H

#include <cstdint>
#include <string>
#include <vector>
#include <netinet/in.h>
#include "network/network.h"

namespace aa3 {
namespace network {

class A2SHandler {
public:
    A2SHandler(SocketManager& socket);
    ~A2SHandler();

    void OnQueryReceived(const struct sockaddr_in& addr, const uint8_t* data, uint32_t len);

    // Metadata for A2S_INFO response
    void SetServerInfo(const std::string& name, const std::string& map, const std::string& game);
    void SetPlayerCount(uint8_t players, uint8_t max_players, uint8_t bots);

private:
    void HandleInfoRequest(const struct sockaddr_in& addr);

    SocketManager& socket_;

    // Server info cache
    std::string server_name_;
    std::string map_name_;
    std::string game_name_;
    uint8_t player_count_;
    uint8_t max_players_;
    uint8_t bot_count_;
};

} // namespace network
} // namespace aa3

#endif // AA3_A2S_INFO_H
