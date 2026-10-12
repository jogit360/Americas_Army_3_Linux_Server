#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <filesystem>
#include <map>
#include <string>
#include "engine/engine.h"
#include "util/logging.h"

using namespace aa3;

// Global engine instance for signal handling
static engine::Engine* g_engine = nullptr;

void SignalHandler(int signal) {
    LOG_INFO("Received signal %d, shutting down...", signal);
    if (g_engine) {
        g_engine->Shutdown();
    }
    exit(0);
}

// The maps the server can run: data/maps/<map>.aa3map.zst (shipped) or
// <map>.aa3map (a local re-bake, which takes precedence; engine_init.cpp).
static int ListMaps() {
    namespace fs = std::filesystem;
    const fs::path dir = "data/maps";
    std::map<std::string, std::pair<fs::path, bool>> maps;   // name -> (file, local re-bake)
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string f = e.path().filename().string();
        for (const char* ext : {".aa3map.zst", ".aa3map"}) {
            const size_t n = std::strlen(ext);
            if (f.size() <= n || f.compare(f.size() - n, n, ext) != 0) continue;
            const bool raw = n == std::strlen(".aa3map");
            auto& m = maps[f.substr(0, f.size() - n)];
            if (m.first.empty() || raw) m = {e.path(), raw};
            break;
        }
    }
    if (maps.empty()) {
        std::printf("No maps found in %s/ (run from the server's root directory)\n", dir.c_str());
        return 1;
    }
    std::printf("Available maps (%s/):\n", dir.c_str());
    for (const auto& [name, m] : maps)
        std::printf("  %-26s %6.1f MB%s\n", name.c_str(), fs::file_size(m.first, ec) / 1048576.0,
                    m.second ? "  (local re-bake)" : "");
    std::printf("%zu maps. Start one with: -m <name>\n", maps.size());
    return 0;
}

int main(int argc, char* argv[]) {
    const char* config_file = "aa3_server.ini";
    int standalone_override = -1; // -1 = follow INI
    const char* map_override = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) {
                config_file = argv[++i];
            }
        } else if (strcmp(argv[i], "--map") == 0 || strcmp(argv[i], "-m") == 0) {
            if (i + 1 < argc) {
                map_override = argv[++i];
            }
        } else if (strcmp(argv[i], "--standalone") == 0 ||
                   strcmp(argv[i], "--no-steam") == 0) {
            standalone_override = 1;
        } else if (strcmp(argv[i], "--steam") == 0) {
            standalone_override = 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            return ListMaps();
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::printf(
                "Usage: %s [options]\n"
                "  -c, --config <file>   Path to INI config (default: aa3_server.ini)\n"
                "  -m, --map <name>      Override map name (e.g. Bridge_night_clear)\n"
                "  -l, --list            List the available maps and exit\n"
                "      --standalone      Run without Steam (alias: --no-steam)\n"
                "      --steam           Force Steam-authenticated mode\n"
                "  -h, --help            Show this help\n",
                argv[0]);
            return 0;
        }
    }

    LOG_INIT();
    LOG_INFO("========================================");
    LOG_INFO("AA3 Linux Server - Starting");
    LOG_INFO("========================================");
    LOG_INFO("Config file: %s", config_file);

    // Set up signal handlers
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    try {
        engine::Engine engine;
        g_engine = &engine;

        if (standalone_override >= 0) {
            engine.SetStandaloneOverride(standalone_override != 0);
        }
        if (map_override) {
            engine.SetMapOverride(map_override);
        }

        if (!engine.Initialize(config_file)) {
            LOG_ERROR("Failed to initialize engine");
            return 1;
        }

        LOG_INFO("Engine initialized successfully");
        engine.Run();

        LOG_INFO("Server shutting down");
        return 0;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Fatal exception: %s", e.what());
        return 1;
    }
}
