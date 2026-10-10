#ifndef AA3_ENGINE_H
#define AA3_ENGINE_H

#include <cstdint>
#include <string>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "auth/steam_auth.h"
#include "engine/map_manifest.h"
#include "game/world.h"
#include "network/network.h"
#include "network/protocol_handler.h"
#include "game/stats_export.h"
#include "network/lan_beacon.h"
#include "network/a2s_info.h"

namespace aa3 {
namespace engine {

struct EngineConfig {
    std::string server_name;
    std::string map_name;
    uint16_t port;
    uint16_t query_port;
    int32_t max_players;
    std::string config_file;
    bool standalone = false; // true = run without Steam (no auth, no listing)
};

class Engine {
public:
    Engine();
    ~Engine();

    // Initialization
    bool Initialize(const std::string& config_path);

    // CLI overrides applied before Initialize.
    void SetStandaloneOverride(bool standalone) { standalone_override_ = standalone; }

    // Main loop
    void Run();
    void Shutdown();

    // Configuration
    bool LoadConfig(const std::string& config_path);
    std::string GetConfigValue(const std::string& key, const std::string& default_val = "");
    int32_t GetConfigInt(const std::string& key, int32_t default_val = 0);
    double GetConfigDouble(const std::string& key, double default_val = 0.0);

    // Timing
    double GetDeltaTime() const { return delta_time_; }
    uint64_t GetTickCount() const { return tick_count_; }

    // State
    bool IsRunning() const { return is_running_; }
    game::World& GetWorld() { return world_; }
    const MapManifest* GetMapManifest() const { return map_manifest_.has_value()
                                                       ? &(*map_manifest_) : nullptr; }

private:
    bool ConfigInitialAppId();
    auth::SteamServerProperties BuildSteamProps();

    EngineConfig config_;
    std::map<std::string, std::string> config_values_;

    auth::SteamAuthManager steam_;
    bool steam_active_ = false;

    network::SocketManager network_;
    std::unique_ptr<network::ProtocolHandler> protocol_;
    std::unique_ptr<network::A2SHandler> a2s_;

    game::World world_;

    // Stats JSON export (doc sec 23). Null when StatsFile is empty.
    std::unique_ptr<game::StatsExporter> stats_;

    // In-game LAN browser support (doc sec 24). Null when LanBeacon=0.
    std::unique_ptr<network::LanBeacon> lan_beacon_;
    uint64_t stats_interval_us_ = 5'000'000;
    uint64_t next_stats_write_us_ = 0;
    std::optional<MapManifest> map_manifest_;

    // -1 = unset (use INI), 0 = INI says steam, 1 = forced standalone via CLI.
    int standalone_override_ = -1;

    double delta_time_;
    uint64_t tick_count_;
    bool is_running_;

    uint64_t last_tick_time_;
};

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_H
