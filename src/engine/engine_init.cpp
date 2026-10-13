#include <cstdio>
#include <ctime>
#include <filesystem>
#include "engine/engine.h"
#include "engine/map_bundle.h"
#include "auth/steam_auth.h"
#include "util/logging.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace aa3 {
namespace engine {

Engine::Engine()
    : delta_time_(0.0),
      tick_count_(0),
      is_running_(false),
      last_tick_time_(0) {
}

Engine::~Engine() {
    if (is_running_) {
        Shutdown();
    }
}

bool Engine::ConfigInitialAppId() {
    // Cross-check that steam_appid.txt next to the binary matches the INI's
    // SteamAppID. Steam reads the file directly; a mismatch means the INI is
    // misleading even though Steam will use the file.
    const int32_t ini_appid = GetConfigInt("SteamAppID", 0);
    if (ini_appid <= 0) {
        return true;
    }

    std::ifstream f("steam_appid.txt");
    if (!f.is_open()) {
        LOG_WARN("steam_appid.txt not found in working directory; "
                 "SteamGameServer_Init will fail unless run via Steam client");
        return true;
    }

    int file_appid = 0;
    f >> file_appid;
    if (file_appid != ini_appid) {
        LOG_WARN("steam_appid.txt (%d) and INI SteamAppID (%d) disagree; "
                 "Steam will use the file value",
                 file_appid, ini_appid);
    }
    return true;
}

auth::SteamServerProperties Engine::BuildSteamProps() {
    auth::SteamServerProperties p;
    p.product           = GetConfigValue("Product",          "AA3");
    p.game_description  = GetConfigValue("GameDescription",  "America's Army 3");
    p.mod_dir           = GetConfigValue("ModDir",           "AA3");
    p.version           = GetConfigValue("Version",          "1.0.0.0");
    p.server_name       = config_.server_name;
    p.map_name          = config_.map_name;
    p.game_tags         = GetConfigValue("GameTags",         "");
    p.max_players       = config_.max_players;
    p.bot_count         = GetConfigInt  ("BotCount",         0);
    p.password_protected = GetConfigInt ("PasswordProtected", 0) != 0;
    p.secure            = GetConfigInt  ("SecureMode",       0) != 0;
    return p;
}

bool Engine::Initialize(const std::string& config_path) {
    LOG_INFO("Engine::Initialize - Loading config from: %s", config_path.c_str());

    if (!LoadConfig(config_path)) {
        LOG_ERROR("Failed to load config file: %s", config_path.c_str());
        return false;
    }

    config_.server_name = GetConfigValue("ServerName", "AA3 Server");
    config_.map_name    = GetConfigValue("MapName",    "Compound");
    if (!map_override_.empty()) {
        config_.map_name = map_override_;
    }
    config_.port        = static_cast<uint16_t>(GetConfigInt("Port",       8777));
    config_.query_port  = static_cast<uint16_t>(GetConfigInt("QueryPort",  27015));
    config_.max_players = GetConfigInt("MaxPlayers", 32);

    // Standalone (no-Steam) mode: CLI override wins, otherwise read INI.
    config_.standalone = (standalone_override_ >= 0)
        ? (standalone_override_ != 0)
        : (GetConfigInt("Standalone", 0) != 0);

    LOG_INFO("Server Config:");
    LOG_INFO("  Name: %s",        config_.server_name.c_str());
    LOG_INFO("  Map: %s",         config_.map_name.c_str());
    LOG_INFO("  Game Port: %d",   config_.port);
    LOG_INFO("  Query Port: %d",  config_.query_port);
    LOG_INFO("  Max Players: %d", config_.max_players);
    LOG_INFO("  Mode: %s",        config_.standalone ? "STANDALONE (no Steam)"
                                                     : "Steam-authenticated");

    // Try loading unified MapBundle first (data/maps/<map_name>.aa3map)
    // The shipped maps are data/maps/<map>.aa3map.zst; an uncompressed
    // .aa3map beside it (a local re-bake) takes precedence.
    std::string bundle_path = GetConfigValue(
        "MapBundlePath", "data/maps/" + config_.map_name + ".aa3map");
    if (!std::filesystem::exists(bundle_path) && std::filesystem::exists(bundle_path + ".zst"))
        bundle_path += ".zst";
    std::unique_ptr<engine::MapBundle> map_bundle;
    if (std::filesystem::exists(bundle_path)) {
        auto bundle = std::make_unique<engine::MapBundle>();
        if (bundle->Load(bundle_path)) {
            auto m = bundle->GetManifest();
            if (m) {
                map_manifest_ = std::move(*m);
                // A mission bundle (e.g. Bridge_inf_co_large_ai, Bridge AI)
                // is named after its mission; the map the client loads is
                // the bundle's own map name.
                if (!bundle->MapName().empty() && bundle->MapName() != config_.map_name) {
                    LOG_INFO("Bundle '%s' is mission %s on map %s", config_.map_name.c_str(),
                             bundle->GameModeLevel().c_str(), bundle->MapName().c_str());
                    config_.map_name = bundle->MapName();
                }
                map_bundle = std::move(bundle);
                LOG_INFO("Using unified map bundle '%s'", bundle_path.c_str());
            } else {
                LOG_WARN("Map bundle '%s' has no manifest, falling back to loose files",
                         bundle_path.c_str());
            }
        } else {
            LOG_WARN("Failed to load map bundle '%s', falling back to loose files",
                     bundle_path.c_str());
        }
    }

    if (!map_manifest_) {
        // Fall back to loose config/maps/<map>.aa3.manifest.json
        std::string manifest_path = GetConfigValue(
            "MapManifestPath",
            "config/maps/" + config_.map_name + ".aa3.manifest.json");
        std::ifstream f(manifest_path);
        if (f) {
            std::string json((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            std::string mferr;
            auto m = MapManifest::FromJson(json, &mferr);
            if (m) {
                LOG_INFO("Loaded map manifest '%s' (%zu USES entries)",
                         manifest_path.c_str(), m->uses.size());
                map_manifest_ = std::move(*m);
                const std::string level = game::World::ResolveGameModeLevel(
                    *map_manifest_, GetConfigValue("GameModeLevel", ""));
                std::ifstream lf("config/maps/" + level + ".aa3.manifest.json");
                if (!level.empty() && lf) {
                    std::string ljson((std::istreambuf_iterator<char>(lf)),
                                      std::istreambuf_iterator<char>());
                    if (auto lm = MapManifest::FromJson(ljson, &mferr)) {
                        size_t added = 0;
                        for (const auto& u : lm->uses) {
                            if (u.basepkg != level) continue;
                            bool have = false;
                            for (const auto& e : map_manifest_->uses) have = have || e.pkg == u.pkg;
                            if (!have) { map_manifest_->uses.push_back(u); ++added; }
                        }
                        LOG_INFO("USES: +%zu package(s) of gametype level %s", added, level.c_str());
                    }
                }
            } else {
                LOG_ERROR("Failed to parse %s: %s",
                          manifest_path.c_str(), mferr.c_str());
                return false;
            }
        } else {
            LOG_ERROR("Map manifest not found at %s nor map bundle at %s.",
                      manifest_path.c_str(), bundle_path.c_str());
            return false;
        }
    }

    // Initialize world state with the map, then bind manifest and bundle
    world_.Initialize(config_.map_name);
    if (!world_.BindPackageMap(*map_manifest_,
                               GetConfigValue("GameModeLevel", ""),
                               map_bundle.get()))
        return false;

    // Round timers (retail 10s each) and spawn points (doc sec 29).
    world_.SetRoundTimes(GetConfigInt("RoleSelectionTime", 0),
                         GetConfigInt("DeploymentTime", 0));
    world_.SetRespawnDelay(std::atof(GetConfigValue("RespawnDelay", "0").c_str()));
    world_.SetIncapBleeding(GetConfigValue("IncapBleeding", "true") != "false");
    world_.SetTimeLimit(GetConfigInt("MissionTimeLimit", 0));
    world_.SetEnemyNPCs(GetConfigInt("EnemyNPCs", 1) != 0);     // AI missions' enemy soldiers
    world_.SetMovementChecks(GetConfigInt("MovementChecks", 1) != 0);

    for (int t = 0; t < 2; ++t) {
        std::string sp = GetConfigValue("SpawnPoint" + std::to_string(t), "");
        float x, y, z; int yaw;
        if (!sp.empty() && std::sscanf(sp.c_str(), "%f,%f,%f,%d", &x, &y, &z, &yaw) == 4) {
            world_.SetSpawnPoint(t, x, y, z, yaw);
        } else if (map_bundle && map_bundle->GetSpawnPoint(t, x, y, z, yaw)) {
            world_.SetSpawnPoint(t, x, y, z, yaw);
            LOG_INFO("Team %d spawn point loaded from bundle: %.1f, %.1f, %.1f, %d",
                     t, x, y, z, yaw);
        }
    }

    // Initialize network
    if (!network_.Initialize(config_.port, config_.query_port, config_.max_players)) {
        LOG_ERROR("Failed to initialize network on GamePort %d, QueryPort %d",
                  config_.port, config_.query_port);
        return false;
    }

    // Initialize protocol handler with a pointer to the loaded manifest +
    // the world model. Both outlive the handler (members of Engine), so
    // raw pointers are safe.
    protocol_ = std::make_unique<network::ProtocolHandler>(network_,
                                                           &(*map_manifest_),
                                                           &world_);
    // LAN browser beacon (doc sec 24): makes this server show up in the
    // client's Multiplayer > Local list. LanIP overrides the advertised
    // address (default: this host's primary IPv4).
    if (GetConfigInt("LanBeacon", 1) != 0) {
        auto beacon = std::make_unique<network::LanBeacon>();
        if (beacon->Start()) {
            network::LanBeacon::ServerInfo base;
            base.ip = GetConfigValue("LanIP", "");
            if (base.ip.empty()) base.ip = network::LanBeacon::DetectLanIp();
            base.game_port = config_.port;
            base.host_name = config_.server_name;
            base.map_name = config_.map_name;
            base.max_players = config_.max_players;
            // The mission's type and variant, as the level name gives them
            // (Bridge_inf_co_large_ai -> co / ai); the INI overrides.
            const auto* gri = world_.GetGRI();
            const std::string type = gri && !gri->s_mission_type.empty() ? gri->s_mission_type : "co";
            const std::string variant = gri && !gri->s_mission_variant.empty() ? gri->s_mission_variant : "ai";
            base.mission_type = GetConfigValue("MissionType", type);
            base.mission_size = GetConfigValue("MissionSize", "large");
            base.mission_tour = GetConfigValue("MissionTourID", "inf");
            base.mission_variant = GetConfigValue("MissionVariant", variant);
            beacon->SetInfoProvider([this, base]() {
                auto info = base;
                info.player_count = static_cast<int>(world_.GetPlayerControllers().size());
                return info;
            });
            LOG_INFO("LAN beacon advertising %s:%u as '%s'", base.ip.c_str(), base.game_port,
                     base.host_name.c_str());
            lan_beacon_ = std::move(beacon);
        }
    }

    // Stats export for the web side (doc sec 23). StatsFile="" disables.
    {
        std::string stats_file = GetConfigValue("StatsFile", "stats/players.json");
        if (!stats_file.empty()) {
            std::error_code ec;
            auto parent = std::filesystem::path(stats_file).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent, ec);
            stats_ = std::make_unique<game::StatsExporter>(stats_file, config_.server_name);
            stats_interval_us_ = static_cast<uint64_t>(
                GetConfigDouble("StatsIntervalSeconds", 5.0) * 1e6);
            protocol_->SetStatsExporter(stats_.get());
            LOG_INFO("Stats export: %s every %.1fs", stats_file.c_str(), stats_interval_us_ / 1e6);
        }
    }

    network_.SetDataCallback([this](uint32_t client_id, const uint8_t* data, uint32_t len) {
        protocol_->OnDataReceived(client_id, data, len);
    });
    network_.SetConnectionCallback([this](uint32_t client_id, bool connected) {
        protocol_->OnClientConnected(client_id, connected);
    });

    // Initialize A2S handler
    a2s_ = std::make_unique<network::A2SHandler>(network_);
    a2s_->SetServerInfo(config_.server_name, config_.map_name, "America's Army 3");
    network_.SetQueryCallback([this](const struct sockaddr_in& addr, const uint8_t* data, uint32_t len) {
        a2s_->OnQueryReceived(addr, data, len);
    });

    auto now = std::chrono::high_resolution_clock::now();
    last_tick_time_ = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    if (!config_.standalone) {
        ConfigInitialAppId();

        const auth::SteamServerProperties props = BuildSteamProps();
        if (!steam_.Initialize(0, config_.port, config_.query_port, props)) {
            LOG_ERROR("SteamAuthManager initialization failed. "
                      "Pass --standalone (or set Standalone=1) to run without Steam.");
            return false;
        }
        steam_active_ = true;
    } else {
        LOG_WARN("Standalone mode: Steam authentication and master-server "
                 "listing are disabled; players must connect by direct IP.");
    }

    is_running_ = true;
    tick_count_ = 0;

    LOG_INFO("Engine initialized");
    return true;
}

void Engine::Run() {
    LOG_INFO("Starting main game loop");

    while (is_running_) {
        auto now = std::chrono::high_resolution_clock::now();
        uint64_t current_time = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        delta_time_ = (current_time - last_tick_time_) / 1000000.0;
        last_tick_time_ = current_time;

        ++tick_count_;

        if (steam_active_) {
            steam_.RunFrame();
        }

        // Poll for network data
        network_.Poll();
        if (lan_beacon_) lan_beacon_->Poll();

        // Drain any scheduled protocol sends (post-storm keepalives etc.)
        if (protocol_) protocol_->Tick(current_time);

        // Update world state
        world_.Update(delta_time_);

        if (stats_ && current_time >= next_stats_write_us_) {
            stats_->Write(world_, static_cast<uint64_t>(std::time(nullptr)));
            next_stats_write_us_ = current_time + stats_interval_us_;
        }

        // Phase 2+: network polling, player updates, game logic, broadcast.

        if (delta_time_ < 0.01667) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>((0.01667 - delta_time_) * 1000))
            );
        }
    }

    LOG_INFO("Main game loop ended");
}

void Engine::Shutdown() {
    LOG_INFO("Engine::Shutdown");
    is_running_ = false;
    if (steam_active_) {
        steam_.Shutdown();
        steam_active_ = false;
    }
}

} // namespace engine
} // namespace aa3
