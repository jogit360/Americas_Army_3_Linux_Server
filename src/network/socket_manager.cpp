#include <chrono>
#include <cstdlib>
#include "network/network.h"
#include "util/logging.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

namespace aa3 {
namespace network {

SocketManager::SocketManager()
    : game_socket_fd_(-1), query_socket_fd_(-1), 
      game_port_(0), query_port_(0), next_client_id_(1) {
}

SocketManager::~SocketManager() {
    Shutdown();
}

int SocketManager::CreateUDPSocket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }

    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        LOG_WARN("Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        LOG_ERROR("Failed to set non-blocking mode: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind socket to port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

bool SocketManager::Initialize(uint16_t game_port, uint16_t query_port, int32_t max_connections) {
    LOG_INFO("SocketManager::Initialize - GamePort=%d, QueryPort=%d, MaxConn=%d", game_port, query_port, max_connections);

    game_socket_fd_ = CreateUDPSocket(game_port);
    if (game_socket_fd_ < 0) return false;

    query_socket_fd_ = CreateUDPSocket(query_port);
    if (query_socket_fd_ < 0) {
        close(game_socket_fd_);
        game_socket_fd_ = -1;
        return false;
    }

    game_port_ = game_port;
    query_port_ = query_port;
    
    LOG_INFO("Sockets initialized: Game=%d, Query=%d", game_port, query_port);
    return true;
}

void SocketManager::Shutdown() {
    if (game_socket_fd_ >= 0) {
        close(game_socket_fd_);
        game_socket_fd_ = -1;
    }
    if (query_socket_fd_ >= 0) {
        close(query_socket_fd_);
        query_socket_fd_ = -1;
    }
    LOG_INFO("SocketManager shutdown");
}

bool SocketManager::SendToClient(uint32_t client_id, const uint8_t* data, uint32_t len) {
    if (game_socket_fd_ < 0) return false;

    auto it = clients_by_id_.find(client_id);
    if (it == clients_by_id_.end()) return false;

    char hex[256];
    size_t hex_len = (len > 64) ? 64 : len;
    for (size_t i = 0; i < hex_len; ++i) std::sprintf(hex + (i * 3), "%02x ", data[i]);
    LOG_INFO("SendTo %u (%u bytes): %s", client_id, len, hex);

    ssize_t sent = sendto(game_socket_fd_, data, len, 0, (struct sockaddr*)&it->second.addr, sizeof(it->second.addr));
    return sent >= 0;
}

bool SocketManager::SendQueryResponse(const struct sockaddr_in& addr, const uint8_t* data, uint32_t len) {
    if (query_socket_fd_ < 0) return false;

    ssize_t sent = sendto(query_socket_fd_, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    return sent >= 0;
}

bool SocketManager::BroadcastToAll(const uint8_t* data, uint32_t len) {
    bool all_ok = true;
    for (auto const& [id, info] : clients_by_id_) {
        if (!SendToClient(id, data, len)) all_ok = false;
    }
    return all_ok;
}

void SocketManager::Poll() {
    uint8_t buffer[2048];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    static uint32_t poll_count = 0;
    if (++poll_count % 1000 == 0) LOG_INFO("SocketManager::Poll heartbeat %u", poll_count);

    // Poll Game Socket
    if (game_socket_fd_ >= 0) {
        while (true) {
            ssize_t received = recvfrom(game_socket_fd_, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);
            if (received < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("recvfrom error: %s", strerror(errno));
                }
                break;
            }

            std::string addr_str = AddrToString(client_addr);
            uint32_t client_id;

            if (addr_to_id_.find(addr_str) == addr_to_id_.end()) {
                client_id = next_client_id_++;
                addr_to_id_[addr_str] = client_id;
                
                ClientInfo info;
                info.id = client_id;
                info.addr = client_addr;
                clients_by_id_[client_id] = info;

                LOG_INFO("New game connection from %s, ID: %u", addr_str.c_str(), client_id);
                if (connection_callback_) connection_callback_(client_id, true);
            } else {
                client_id = addr_to_id_[addr_str];
            }
            clients_by_id_[client_id].last_recv_us = NowUs();

            LOG_INFO("Recv from %u: %d bytes. Address: %s", client_id, (int)received, addr_str.c_str());
            if (data_callback_) data_callback_(client_id, buffer, static_cast<uint32_t>(received));
        }
    }

    // Drop clients that have gone silent (UE3 UNetDriver::TickDispatch's
    // ConnectionTimeout check). Without this a departed player's actors and
    // other clients' channels to them would live forever.
    // AA3_CONNECTION_TIMEOUT_S overrides the default (tests, ops tuning).
    static const uint64_t timeout_us = [] {
        const char* e = std::getenv("AA3_CONNECTION_TIMEOUT_S");
        return e ? static_cast<uint64_t>(std::atof(e) * 1e6) : kConnectionTimeoutUs;
    }();
    const uint64_t now_us = NowUs();
    for (auto it = clients_by_id_.begin(); it != clients_by_id_.end();) {
        if (it->second.last_recv_us != 0 && now_us - it->second.last_recv_us > timeout_us) {
            uint32_t id = it->first;
            LOG_INFO("Client %u timed out (no packets for %.1fs)", id, timeout_us / 1e6);
            addr_to_id_.erase(AddrToString(it->second.addr));
            it = clients_by_id_.erase(it);
            if (connection_callback_) connection_callback_(id, false);
        } else {
            ++it;
        }
    }

    // Poll Query Socket
    if (query_socket_fd_ >= 0) {
        while (true) {
            ssize_t received = recvfrom(query_socket_fd_, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);
            if (received < 0) break;

            if (query_callback_) {
                query_callback_(client_addr, buffer, static_cast<uint32_t>(received));
            }
        }
    }
}

uint64_t SocketManager::NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string SocketManager::AddrToString(const struct sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // namespace network
} // namespace aa3
