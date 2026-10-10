#ifndef AA3_NETWORK_H
#define AA3_NETWORK_H

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <map>
#include <netinet/in.h>

namespace aa3 {
namespace network {

// Network message types
enum class MessageType : uint8_t {
    PLAYER_JOIN = 0x01,
    PLAYER_LEAVE = 0x02,
    PLAYER_STATE = 0x03,
    GAME_STATE = 0x04,
    INPUT_COMMAND = 0x05,
    DAMAGE_REPORT = 0x06,
    OBJECTIVE_UPDATE = 0x07,
    HEARTBEAT = 0x08,
};

// Player state message: 116 bytes
struct PlayerStateMessage {
    uint32_t player_id;
    float position_x;
    float position_y;
    float position_z;
    float rotation_pitch;
    float rotation_yaw;
    float rotation_roll;
    uint8_t health;
    uint8_t ammo_primary;
    uint8_t ammo_secondary;
    uint16_t team_id;
    uint8_t state; // standing, crouching, prone, etc.
    uint8_t is_alive;
    uint8_t padding[83]; // Align to 116 bytes
};

class SocketManager {
public:
    SocketManager();
    ~SocketManager();

    // Socket management
    bool Initialize(uint16_t game_port, uint16_t query_port, int32_t max_connections = 32);
    void Shutdown();

    // Send/receive
    bool SendToClient(uint32_t client_id, const uint8_t* data, uint32_t len);
    bool SendQueryResponse(const struct sockaddr_in& addr, const uint8_t* data, uint32_t len);
    bool BroadcastToAll(const uint8_t* data, uint32_t len);

    // Poll for incoming data
    void Poll();

    // Callbacks
    using DataCallback = std::function<void(uint32_t client_id, const uint8_t* data, uint32_t len)>;
    void SetDataCallback(DataCallback callback) { data_callback_ = callback; }

    using QueryCallback = std::function<void(const struct sockaddr_in& addr, const uint8_t* data, uint32_t len)>;
    void SetQueryCallback(QueryCallback callback) { query_callback_ = callback; }

    using ConnectionCallback = std::function<void(uint32_t client_id, bool connected)>;
    void SetConnectionCallback(ConnectionCallback callback) { connection_callback_ = callback; }

private:
    int CreateUDPSocket(uint16_t port);

    int game_socket_fd_;
    int query_socket_fd_;
    
    uint16_t game_port_;
    uint16_t query_port_;

    DataCallback data_callback_;
    QueryCallback query_callback_;
    ConnectionCallback connection_callback_;

    // Client tracking
    struct ClientInfo {
        uint32_t id;
        struct sockaddr_in addr;
        uint64_t last_recv_us = 0;  // monotonic; drives the idle timeout
    };
    // AA3Engine.ini [IpDrv.TcpNetDriver] ConnectionTimeout=60.0: a client
    // silent this long is dropped (connection callback with connected=false).
    static constexpr uint64_t kConnectionTimeoutUs = 60'000'000;
    std::map<uint32_t, ClientInfo> clients_by_id_;
    std::map<std::string, uint32_t> addr_to_id_;
    uint32_t next_client_id_;

    std::string AddrToString(const struct sockaddr_in& addr);
    static uint64_t NowUs();
};

} // namespace network
} // namespace aa3

#endif // AA3_NETWORK_H
