#ifndef AA3_STEAM_AUTH_H
#define AA3_STEAM_AUTH_H

#include <cstdint>
#include <string>

#include "steam/steam_gameserver.h"

namespace aa3 {
namespace auth {

struct SteamServerProperties {
    std::string product           = "AA3";
    std::string game_description  = "America's Army 3";
    std::string mod_dir           = "AA3";
    std::string version           = "1.0.0.0";
    std::string server_name       = "AA3 Server";
    std::string map_name          = "Compound";
    std::string game_tags;
    int32_t     max_players       = 32;
    int32_t     bot_count         = 0;
    bool        password_protected = false;
    bool        secure            = false;
};

class IPlayerAuthSink {
public:
    virtual ~IPlayerAuthSink() = default;
    virtual void OnAuthResult(uint64_t steam_id,
                              uint64_t owner_steam_id,
                              bool approved,
                              int deny_reason) = 0;
};

class SteamAuthManager {
public:
    SteamAuthManager();
    ~SteamAuthManager();

    SteamAuthManager(const SteamAuthManager&) = delete;
    SteamAuthManager& operator=(const SteamAuthManager&) = delete;

    bool Initialize(uint32_t bind_ip,
                    uint16_t game_port,
                    uint16_t query_port,
                    const SteamServerProperties& props);

    void RunFrame();
    void Shutdown();

    bool IsInitialized() const { return initialized_; }
    bool IsLoggedOn()    const { return logged_on_; }
    bool IsAdvertising() const { return advertising_; }

    bool BeginAuthSession(uint64_t steam_id,
                          const uint8_t* ticket,
                          uint32_t ticket_len);
    void EndAuthSession(uint64_t steam_id);

    void SetAuthSink(IPlayerAuthSink* sink) { sink_ = sink; }

private:
    SteamServerProperties props_;
    bool initialized_ = false;
    bool logged_on_   = false;
    bool advertising_ = false;
    IPlayerAuthSink* sink_ = nullptr;

    void TryAdvertise();

    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnSteamServersConnected,
                              SteamServersConnected_t);
    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnSteamServerConnectFailure,
                              SteamServerConnectFailure_t);
    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnSteamServersDisconnected,
                              SteamServersDisconnected_t);
    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnPolicyResponse,
                              GSPolicyResponse_t);
    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnValidateAuthTicket,
                              ValidateAuthTicketResponse_t);
    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnClientApprove,
                              GSClientApprove_t);
    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnClientDeny,
                              GSClientDeny_t);
    STEAM_GAMESERVER_CALLBACK(SteamAuthManager, OnClientKick,
                              GSClientKick_t);
};

} // namespace auth
} // namespace aa3

#endif // AA3_STEAM_AUTH_H
