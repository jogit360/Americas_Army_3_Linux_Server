// aa3_mapbake
//
// Modern offline map compiler / bundler for the AA3 Dedicated Server.
// Compiles all server map data (manifest/USES, collision geometry, replicated
// actors, mission rules, windows table, and spawns) into a single unified
// .aa3map bundle.
//
// Usage:
//   aa3_mapbake --info <map.aa3map>
//   aa3_mapbake --pack-loose --map <map_name> [--out <out.aa3map>]
//                            [--maps-dir <dir>] [--col-dir <dir>]
//                            [--gametype-level <level>]
//                            [--spawn0 <x,y,z,yaw>] [--spawn1 <x,y,z,yaw>]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "engine/map_bundle.h"
#include "engine/map_manifest.h"
#include "engine/package_extractor.h"
#include "engine/package_reader.h"
#include "util/json.h"
#include "util/logging.h"

using namespace aa3;
using namespace aa3::engine;

namespace {

void PrintUsage() {
    std::cout << "aa3_mapbake - America's Army 3 Map Bundler\n\n"
              << "Usage:\n"
              << "  aa3_mapbake --info <bundle.aa3map>\n"
              << "  aa3_mapbake --bake --package <path.u> [options]\n"
              << "  aa3_mapbake --batch-bake [options]\n"
              << "  aa3_mapbake --pack-loose --map <map_name> [options]\n\n"
              << "Options for --bake (Direct-From-Package Ingestion):\n"
              << "  --package <path>          Path to map package (.u or .aa3)\n"
              << "  --sublevel <path>         Path to gametype sublevel package (.u or .aa3)\n"
              << "  --cooked-root <dir>       Directory containing cooked packages for dep resolution\n"
              << "  --out <path>              Output bundle path (default: data/maps/<map>.aa3map)\n"
              << "  --maps-dir <dir>          Directory containing mission/actors JSON overrides\n"
              << "  --spawn0 <x,y,z,yaw>      Team 0 spawn point override\n"
              << "  --spawn1 <x,y,z,yaw>      Team 1 spawn point override\n"
              << "  --inventory <path>        Path to spawn_inventory.json\n\n"
              << "Options for --batch-bake:\n"
              << "  --cooked-maps <dir>       Path to CookedPC/Maps directory\n"
              << "  --out-dir <dir>           Output directory for .aa3map files (default: data/maps)\n"
              << "  --maps-dir <dir>          Directory containing config/maps overrides (default: config/maps)\n"
              << "  --inventory <path>        Path to spawn_inventory.json (default: config/spawn_inventory.json)\n\n"
              << "Options for --pack-loose:\n"
              << "  --out <path>              Output bundle path (default: data/maps/<map>.aa3map)\n"
              << "  --maps-dir <dir>          Directory containing .manifest.json, .actors.json etc. (default: config/maps)\n"
              << "  --col-dir <dir>           Directory containing .col collision files (default: data/collision)\n"
              << "  --gametype-level <level>  Sublevel name (e.g. Bridge_inf_classic_large_dev)\n"
              << "  --spawn0 <x,y,z,yaw>      Team 0 spawn point override\n"
              << "  --spawn1 <x,y,z,yaw>      Team 1 spawn point override\n"
              << "  --inventory <path>        Path to spawn_inventory.json (default: config/spawn_inventory.json)\n";
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (sz < 0) return false;
    out.resize(static_cast<size_t>(sz));
    return f.read(reinterpret_cast<char*>(out.data()), sz).good();
}

bool ReadFileString(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

int DoInfo(const std::string& bundle_path) {
    MapBundle bundle;
    if (!bundle.Load(bundle_path)) {
        std::cerr << "Error: failed to load bundle: " << bundle_path << "\n";
        return 1;
    }

    const auto& hdr = bundle.Header();
    std::cout << "========================================\n"
              << " AA3 Map Bundle: " << bundle_path << "\n"
              << "========================================\n"
              << "Magic:          " << std::string(hdr.magic, 8) << "\n"
              << "Version:        " << hdr.version << "\n"
              << "Map Name:       " << bundle.MapName() << "\n"
              << "Gametype Level: " << bundle.GameModeLevel() << "\n"
              << "File Size:      " << bundle.FileSize() << " bytes\n"
              << "Sections:       " << hdr.section_count << "\n\n"
              << "Section Table:\n"
              << "------------------------------------------------------------------------\n"
              << " Index  Type                Offset      Size        CRC32\n"
              << "------------------------------------------------------------------------\n";

    for (size_t i = 0; i < bundle.Sections().size(); ++i) {
        const auto& s = bundle.Sections()[i];
        std::printf(" [%2zu]   %-18s  0x%08lX  %10lu  0x%08X\n",
                    i, SectionTypeName(static_cast<SectionType>(s.type)),
                    s.offset, s.size, s.crc32);
    }
    std::cout << "------------------------------------------------------------------------\n\n";

    if (auto m = bundle.GetManifest()) {
        std::cout << "Manifest:\n"
                  << "  URL:           " << m->url << "\n"
                  << "  USES Packages: " << m->uses.size() << "\n"
                  << "  TOD:           " << m->tod << "\n"
                  << "  GameClass:     " << m->game_class << "\n";
    }

    const uint8_t* col_ptr = nullptr;
    size_t col_size = 0;
    if (bundle.GetCollisionData(col_ptr, col_size) && col_size >= 12) {
        if (std::memcmp(col_ptr, "AA3COL04", 8) == 0 && col_size >= 28) {
            uint32_t ver = 0, n_meshes = 0, n_inst = 0, n_direct = 0, n_total = 0;
            std::memcpy(&ver, col_ptr + 8, 4);
            std::memcpy(&n_meshes, col_ptr + 12, 4);
            std::memcpy(&n_inst, col_ptr + 16, 4);
            std::memcpy(&n_direct, col_ptr + 20, 4);
            std::memcpy(&n_total, col_ptr + 24, 4);
            std::cout << "Collision:\n"
                      << "  Format:        AA3COL04 (Quantized BLAS/TLAS)\n"
                      << "  Meshes:        " << n_meshes << " unique meshes (16-bit quantized)\n"
                      << "  Instances:     " << n_inst << " placed instances\n"
                      << "  Direct Tris:   " << n_direct << " (BSP + Terrain)\n"
                      << "  Total Tris:    " << n_total << " equivalent triangles\n"
                      << "  Data Size:     " << col_size << " bytes ("
                      << (col_size / (1024.0 * 1024.0)) << " MB)\n";
        } else if (std::memcmp(col_ptr, "AA3COL03", 8) == 0 && col_size >= 28) {
            uint32_t ver = 0, n_meshes = 0, n_inst = 0, n_direct = 0, n_total = 0;
            std::memcpy(&ver, col_ptr + 8, 4);
            std::memcpy(&n_meshes, col_ptr + 12, 4);
            std::memcpy(&n_inst, col_ptr + 16, 4);
            std::memcpy(&n_direct, col_ptr + 20, 4);
            std::memcpy(&n_total, col_ptr + 24, 4);
            std::cout << "Collision:\n"
                      << "  Format:        AA3COL03 (Instanced BLAS/TLAS)\n"
                      << "  Meshes:        " << n_meshes << " unique meshes\n"
                      << "  Instances:     " << n_inst << " placed instances\n"
                      << "  Direct Tris:   " << n_direct << " (BSP + Terrain)\n"
                      << "  Total Tris:    " << n_total << " equivalent triangles\n"
                      << "  Data Size:     " << col_size << " bytes ("
                      << (col_size / (1024.0 * 1024.0)) << " MB)\n";
        } else {
            uint32_t tris = 0;
            std::memcpy(&tris, col_ptr + 8, 4);
            std::cout << "Collision:\n"
                      << "  Format:        " << std::string(reinterpret_cast<const char*>(col_ptr), 8) << "\n"
                      << "  Triangles:     " << tris << "\n"
                      << "  Data Size:     " << col_size << " bytes\n";
        }
    }

    std::map<uint32_t, int> windows;
    if (bundle.GetWindows(windows)) {
        std::cout << "Windows:\n"
                  << "  Breakable:     " << windows.size() << " windows mapped\n";
    }

    for (int t = 0; t < 2; ++t) {
        float x = 0, y = 0, z = 0;
        int yaw = 0;
        if (bundle.GetSpawnPoint(t, x, y, z, yaw)) {
            std::printf("Spawn Team %d:   (%.1f, %.1f, %.1f), yaw=%d\n", t, x, y, z, yaw);
        }
    }

    std::cout << "\nStatus: Valid AA3 Map Bundle\n";
    return 0;
}

int DoPackLoose(const std::string& map_name,
                std::string gametype_level,
                const std::string& maps_dir,
                const std::string& col_dir,
                std::string out_path,
                const std::string& spawn0_override,
                const std::string& spawn1_override,
                const std::string& inv_path) {
    if (out_path.empty()) {
        out_path = "data/maps/" + map_name + ".aa3map";
    }

    // Default sublevel convention if not specified
    if (gametype_level.empty()) {
        std::string base = map_name;
        auto us = base.find('_');
        if (us != std::string::npos) base = base.substr(0, us);
        gametype_level = base + "_inf_classic_large_dev";
    }

    std::cout << "Baking map bundle for: " << map_name << "\n"
              << "Gametype sublevel:     " << gametype_level << "\n"
              << "Output destination:    " << out_path << "\n";

    MapBundleWriter writer(map_name, gametype_level);

    // 1. Manifest
    std::string manifest_file = maps_dir + "/" + map_name + ".aa3.manifest.json";
    std::string manifest_str;
    if (ReadFileString(manifest_file, manifest_str)) {
        // Parse and check if gametype sublevel packages need merging
        std::string err;
        auto m = MapManifest::FromJson(manifest_str, &err);
        if (m) {
            std::string level_manifest_file = maps_dir + "/" + gametype_level + ".aa3.manifest.json";
            std::string level_manifest_str;
            if (ReadFileString(level_manifest_file, level_manifest_str)) {
                if (auto lm = MapManifest::FromJson(level_manifest_str, &err)) {
                    for (const auto& u : lm->uses) {
                        if (u.basepkg != gametype_level) continue;
                        bool have = false;
                        for (const auto& e : m->uses) have = have || e.pkg == u.pkg;
                        if (!have) m->uses.push_back(u);
                    }
                }
            }
            manifest_str = m->ToJson();
        }
        writer.AddSectionString(SectionType::kManifest, manifest_str);
        std::cout << "  [+] Packed Manifest (" << manifest_str.size() << " bytes)\n";
    } else {
        std::cerr << "  [-] Warning: manifest file not found: " << manifest_file << "\n";
    }

    // 2. Collision
    std::string col_file = col_dir + "/" + map_name + ".col";
    std::vector<uint8_t> col_bytes;
    if (ReadFileBytes(col_file, col_bytes)) {
        writer.AddSection(SectionType::kCollision, col_bytes.data(), col_bytes.size());
        std::cout << "  [+] Packed Collision (" << col_bytes.size() << " bytes)\n";
    } else {
        std::cerr << "  [-] Warning: collision file not found: " << col_file << "\n";
    }

    // 3. Map Actors
    std::string map_actors_file = maps_dir + "/" + map_name + ".map_actors.json";
    std::string map_actors_str;
    if (ReadFileString(map_actors_file, map_actors_str)) {
        writer.AddSectionString(SectionType::kMapActors, map_actors_str);
        std::cout << "  [+] Packed MapActors (" << map_actors_str.size() << " bytes)\n";
    }

    // 4. Round Actors
    std::string round_actors_file = maps_dir + "/" + gametype_level + ".round.json";
    std::string round_actors_str;
    if (ReadFileString(round_actors_file, round_actors_str)) {
        writer.AddSectionString(SectionType::kRoundActors, round_actors_str);
        std::cout << "  [+] Packed RoundActors (" << round_actors_str.size() << " bytes)\n";
    }

    // 5. Join Actors
    std::string join_actors_file = maps_dir + "/" + gametype_level + ".join.json";
    std::string join_actors_str;
    if (ReadFileString(join_actors_file, join_actors_str)) {
        writer.AddSectionString(SectionType::kJoinActors, join_actors_str);
        std::cout << "  [+] Packed JoinActors (" << join_actors_str.size() << " bytes)\n";
    }

    // 6. Match Actors
    std::string match_actors_file = maps_dir + "/" + gametype_level + ".match.json";
    std::string match_actors_str;
    if (ReadFileString(match_actors_file, match_actors_str)) {
        writer.AddSectionString(SectionType::kMatchActors, match_actors_str);
        std::cout << "  [+] Packed MatchActors (" << match_actors_str.size() << " bytes)\n";
    }

    // 7. Mission Rules
    std::string rules_file = maps_dir + "/" + gametype_level + ".rules.json";
    std::string rules_str;
    if (ReadFileString(rules_file, rules_str)) {
        writer.AddSectionString(SectionType::kMissionRules, rules_str);
        std::cout << "  [+] Packed MissionRules (" << rules_str.size() << " bytes)\n";
    }

    // 8. Windows
    std::string windows_file = maps_dir + "/" + map_name + ".windows.json";
    std::string windows_str;
    if (ReadFileString(windows_file, windows_str)) {
        writer.AddSectionString(SectionType::kWindows, windows_str);
        std::cout << "  [+] Packed Windows (" << windows_str.size() << " bytes)\n";
    }

    // 9. Spawns
    std::string s0 = spawn0_override.empty() ? "-7174,1386,40,16384" : spawn0_override;
    std::string s1 = spawn1_override.empty() ? "-4000,2000,40,0"     : spawn1_override;
    std::string spawns_json = "{\n  \"0\": \"" + s0 + "\",\n  \"1\": \"" + s1 + "\"\n}\n";
    writer.AddSectionString(SectionType::kSpawns, spawns_json);
    std::cout << "  [+] Packed Spawns (Team 0 & Team 1)\n";

    // 10. Spawn Inventory
    std::string inventory_str;
    if (ReadFileString(inv_path, inventory_str)) {
        writer.AddSectionString(SectionType::kSpawnInventory, inventory_str);
        std::cout << "  [+] Packed SpawnInventory (" << inventory_str.size() << " bytes)\n";
    }

    // Ensure parent directory exists
    std::filesystem::path out_p(out_path);
    if (out_p.has_parent_path()) {
        std::filesystem::create_directories(out_p.parent_path());
    }

    std::string write_err;
    if (!writer.WriteToFile(out_path, &write_err)) {
        std::cerr << "Error writing bundle: " << write_err << "\n";
        return 1;
    }

    std::cout << "\nSuccess: Created bundle " << out_path << "\n";
    return 0;
}

int DoBake(const std::string& pkg_path,
           const std::string& sublevel_path,
           const std::string& cooked_root,
           std::string out_path,
           const std::string& maps_dir,
           const std::string& spawn0_override,
           const std::string& spawn1_override,
           const std::string& inv_path) {
    PackageReadError perr;
    auto pkg_opt = PackageReader::ReadFile(pkg_path, &perr);
    if (!pkg_opt) {
        std::cerr << "Error reading package " << pkg_path << ": " << perr.message << "\n";
        return 1;
    }
    const Package& pkg = *pkg_opt;

    std::filesystem::path p(pkg_path);
    std::string map_name = p.stem().string();

    std::string gametype_level;
    if (!sublevel_path.empty()) {
        gametype_level = std::filesystem::path(sublevel_path).stem().string();
    } else {
        std::string base = map_name;
        auto us = base.find('_');
        if (us != std::string::npos) base = base.substr(0, us);
        gametype_level = base + "_inf_classic_large_dev";
    }

    if (out_path.empty()) {
        out_path = "data/maps/" + map_name + ".aa3map";
    }

    std::cout << "Baking map bundle directly from package: " << pkg_path << "\n"
              << "Map Name:              " << map_name << "\n"
              << "Gametype sublevel:     " << gametype_level << "\n"
              << "Output destination:    " << out_path << "\n";

    MapBundleWriter writer(map_name, gametype_level);
    PackageExtractor extractor(pkg);

    // 1. Manifest
    std::string manifest_str;
    std::string manifest_file = maps_dir + "/" + map_name + ".aa3.manifest.json";
    if (ReadFileString(manifest_file, manifest_str)) {
        std::string err;
        auto m = MapManifest::FromJson(manifest_str, &err);
        if (m) {
            std::string level_manifest_file = maps_dir + "/" + gametype_level + ".aa3.manifest.json";
            std::string level_manifest_str;
            if (ReadFileString(level_manifest_file, level_manifest_str)) {
                if (auto lm = MapManifest::FromJson(level_manifest_str, &err)) {
                    for (const auto& u : lm->uses) {
                        if (u.basepkg != gametype_level) continue;
                        bool have = false;
                        for (const auto& e : m->uses) have = have || e.pkg == u.pkg;
                        if (!have) m->uses.push_back(u);
                    }
                }
            }
            manifest_str = m->ToJson();
        }
    } else if (!cooked_root.empty()) {
        std::cout << "  [+] Building manifest with package universe from " << cooked_root << " ...\n";
        auto uni = PackageUniverse::Build(cooked_root);
        std::string err;
        auto m = MapManifest::BuildWithUniverse(pkg_path, uni, &err);
        if (m) {
            if (!sublevel_path.empty()) {
                auto lm = MapManifest::BuildWithUniverse(sublevel_path, uni, &err);
                if (lm) {
                    for (const auto& u : lm->uses) {
                        if (u.basepkg != gametype_level) continue;
                        bool have = false;
                        for (const auto& e : m->uses) have = have || e.pkg == u.pkg;
                        if (!have) m->uses.push_back(u);
                    }
                }
            }
            FillNetObjectCounts(*m, uni.by_name);
            manifest_str = m->ToJson();
        } else {
            std::cerr << "  [-] Warning: manifest build failed (" << err << "), checking maps-dir...\n";
        }
    }
    if (!manifest_str.empty()) {
        writer.AddSectionString(SectionType::kManifest, manifest_str);
        std::cout << "  [+] Packed Manifest (" << manifest_str.size() << " bytes)\n";
    } else {
        std::cerr << "  [-] Warning: no manifest generated\n";
    }

    // 2. Collision (AA3COL04 Quantized)
    std::vector<uint8_t> col_bytes;
    uint32_t n_meshes = 0, n_inst = 0, n_direct = 0, n_total = 0;
    std::cout << "  [+] Extracting collision geometry directly from package...\n";
    if (extractor.ExtractCollision(col_bytes, n_meshes, n_inst, n_direct, n_total, /*quantize=*/true)) {
        writer.AddSection(SectionType::kCollision, col_bytes.data(), col_bytes.size());
        std::cout << "  [+] Packed Collision AA3COL04 Quantized (" << col_bytes.size() << " bytes, "
                  << n_meshes << " meshes, " << n_inst << " instances, "
                  << n_direct << " direct, " << n_total << " equivalent tris)\n";
    } else {
        std::cerr << "  [-] Warning: direct collision extraction failed, falling back to col-dir...\n";
        std::string col_file = "data/collision/" + map_name + ".col";
        if (ReadFileBytes(col_file, col_bytes)) {
            writer.AddSection(SectionType::kCollision, col_bytes.data(), col_bytes.size());
            std::cout << "  [+] Packed Collision from file (" << col_bytes.size() << " bytes)\n";
        }
    }

    // 3. Windows
    std::map<uint32_t, int> windows;
    if (extractor.ExtractWindows(windows)) {
        std::string win_json = "{\n  \"source\": \"" + map_name + "\",\n  \"windows\": {\n";
        bool first = true;
        for (const auto& kv : windows) {
            if (!first) win_json += ",\n";
            win_json += "    \"" + std::to_string(kv.first) + "\": " + std::to_string(kv.second);
            first = false;
        }
        win_json += "\n  }\n}\n";
        writer.AddSectionString(SectionType::kWindows, win_json);
        std::cout << "  [+] Packed Windows (" << windows.size() << " mapped directly from package)\n";
    } else {
        std::string win_file = maps_dir + "/" + map_name + ".windows.json";
        std::string win_str;
        if (ReadFileString(win_file, win_str)) {
            writer.AddSectionString(SectionType::kWindows, win_str);
            std::cout << "  [+] Packed Windows from file (" << win_str.size() << " bytes)\n";
        }
    }

    // 4. Spawns
    std::string s0 = spawn0_override;
    std::string s1 = spawn1_override;
    if (s0.empty() || s1.empty()) {
        std::vector<SpawnPointDef> spawns;
        if (!sublevel_path.empty()) {
            auto sub_pkg = PackageReader::ReadFile(sublevel_path);
            if (sub_pkg) {
                PackageExtractor sub_ext(*sub_pkg);
                sub_ext.ExtractSpawns(spawns);
            }
        }
        if (spawns.empty()) {
            extractor.ExtractSpawns(spawns);
        }
        for (const auto& sp : spawns) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.1f,%.1f,%.1f,%d", sp.x, sp.y, sp.z, sp.yaw);
            if (sp.team == 0 && s0.empty()) s0 = buf;
            if (sp.team == 1 && s1.empty()) s1 = buf;
        }
    }
    if (s0.empty()) s0 = "-7174,1386,40,16384";
    if (s1.empty()) s1 = "-4000,2000,40,0";
    std::string spawns_json = "{\n  \"0\": \"" + s0 + "\",\n  \"1\": \"" + s1 + "\"\n}\n";
    writer.AddSectionString(SectionType::kSpawns, spawns_json);
    std::cout << "  [+] Packed Spawns (Team 0 & Team 1)\n";

    // 5. Map Actors
    std::string map_actors_str;
    if (ReadFileString(maps_dir + "/" + map_name + ".map_actors.json", map_actors_str)) {
        writer.AddSectionString(SectionType::kMapActors, map_actors_str);
        std::cout << "  [+] Packed MapActors from file (" << map_actors_str.size() << " bytes)\n";
    } else if (extractor.ExtractMapActors(map_actors_str, map_name)) {
        writer.AddSectionString(SectionType::kMapActors, map_actors_str);
        std::cout << "  [+] Extracted & Packed MapActors (" << map_actors_str.size() << " bytes)\n";
    }

    // 6. Round Actors
    std::string round_actors_str;
    if (ReadFileString(maps_dir + "/" + gametype_level + ".round.json", round_actors_str)) {
        writer.AddSectionString(SectionType::kRoundActors, round_actors_str);
        std::cout << "  [+] Packed RoundActors from file (" << round_actors_str.size() << " bytes)\n";
    } else if (extractor.ExtractRoundActors(round_actors_str, gametype_level)) {
        writer.AddSectionString(SectionType::kRoundActors, round_actors_str);
        std::cout << "  [+] Extracted & Packed RoundActors (" << round_actors_str.size() << " bytes)\n";
    }

    // 7. Join Actors
    std::string join_actors_str;
    if (ReadFileString(maps_dir + "/" + gametype_level + ".join.json", join_actors_str)) {
        writer.AddSectionString(SectionType::kJoinActors, join_actors_str);
        std::cout << "  [+] Packed JoinActors from file (" << join_actors_str.size() << " bytes)\n";
    } else if (extractor.ExtractJoinActors(join_actors_str, gametype_level)) {
        writer.AddSectionString(SectionType::kJoinActors, join_actors_str);
        std::cout << "  [+] Extracted & Packed JoinActors (" << join_actors_str.size() << " bytes)\n";
    }

    // 8. Match Actors
    std::string match_actors_str;
    if (ReadFileString(maps_dir + "/" + gametype_level + ".match.json", match_actors_str)) {
        writer.AddSectionString(SectionType::kMatchActors, match_actors_str);
        std::cout << "  [+] Packed MatchActors from file (" << match_actors_str.size() << " bytes)\n";
    } else if (extractor.ExtractMatchActors(match_actors_str, gametype_level)) {
        writer.AddSectionString(SectionType::kMatchActors, match_actors_str);
        std::cout << "  [+] Extracted & Packed MatchActors (" << match_actors_str.size() << " bytes)\n";
    }

    // 9. Mission Rules
    std::string rules_str;
    if (ReadFileString(maps_dir + "/" + gametype_level + ".rules.json", rules_str)) {
        writer.AddSectionString(SectionType::kMissionRules, rules_str);
        std::cout << "  [+] Packed MissionRules from file (" << rules_str.size() << " bytes)\n";
    } else if (extractor.ExtractMissionRules(rules_str, map_name, gametype_level)) {
        writer.AddSectionString(SectionType::kMissionRules, rules_str);
        std::cout << "  [+] Extracted & Packed MissionRules (" << rules_str.size() << " bytes)\n";
    }

    // 10. Spawn Inventory
    std::string inv_str;
    if (ReadFileString(inv_path, inv_str)) {
        writer.AddSectionString(SectionType::kSpawnInventory, inv_str);
        std::cout << "  [+] Packed SpawnInventory (" << inv_str.size() << " bytes)\n";
    }

    // Ensure parent directory exists
    std::filesystem::path out_p(out_path);
    if (out_p.has_parent_path()) {
        std::filesystem::create_directories(out_p.parent_path());
    }

    std::string write_err;
    if (!writer.WriteToFile(out_path, &write_err)) {
        std::cerr << "Error writing bundle: " << write_err << "\n";
        return 1;
    }

    std::cout << "\nSuccess: Created bundle " << out_path << "\n";
    return 0;
}

struct MapBakeTarget {
    const char* folder;
    const char* base_map;
    const char* sublevel;
};

static const MapBakeTarget kRetailMaps[] = {
    {"Airfield", "Airfield_day_cloudy.aa3", "Airfield_inf_co_large_dev.aa3"},
    {"Alley", "Alley_day_cloudy.aa3", "Alley_inf_co_large_dev.aa3"},
    {"Alley", "Alley_day_foggy.aa3", "Alley_inf_tah_large_east.aa3"},
    {"Bridge", "Bridge_night_clear.aa3", "Bridge_inf_classic_large_dev.aa3"},
    {"Bridge", "Bridge_day_fog.aa3", "Bridge_inf_ao_large_dev1.aa3"},
    {"Classroom", "Classroom_CLS.aa3", "Classroom.aa3"},
    {"ConexMOUT", "ConexMOUT.aa3", "ConexMOUT_bct_MOUT.aa3"},
    {"Impact", "Impact_day_cloudy.aa3", "Impact_inf_ao_large_dev1.aa3"},
    {"Leavenworth", "Leavenworth.aa3", ""},
    {"MultiRange", "MultiRange.aa3", "MultiRange_bct_brm.aa3"},
    {"ObstacleCourse", "obstaclecourse.aa3", "obstaclecourse_BCT_ftw.aa3"},
    {"Pipeline", "Pipeline_dawn_clear.aa3", "Pipeline_inf_ad_large_classic.aa3"},
    {"Ranch", "Ranch_day_fog.aa3", "Ranch_inf_ao_large_dev1.aa3"},
    {"RanchAI", "RanchAI_day_fog.aa3", "RanchAI_inf_ao_large_ai.aa3"},
    {"Shantytown", "shantytown_day_rain.aa3", "ShantyTown_inf_ao_large_dev.aa3"},
    {"Stronghold", "Stronghold_night_clear.aa3", "Stronghold_inf_ao_large_dev.aa3"},
    {"VRS", "VRS.aa3", ""},
    {"shortrange", "short-range.aa3", "short-range_ait_advr1.aa3"},
};

int DoBatchBake(std::string cooked_maps_dir,
                std::string out_dir,
                const std::string& maps_dir,
                const std::string& inv_path) {
    if (cooked_maps_dir.empty()) {
        if (std::filesystem::exists("/home/odeyin/Documents/AA3/AA3/AA3Game/CookedPC/Maps")) {
            cooked_maps_dir = "/home/odeyin/Documents/AA3/AA3/AA3Game/CookedPC/Maps";
        } else if (std::filesystem::exists("maps")) {
            cooked_maps_dir = "maps";
        } else {
            cooked_maps_dir = ".";
        }
    }
    if (out_dir.empty()) {
        out_dir = "data/maps";
    }

    std::cout << "========================================\n"
              << " AA3 Batch Map Bundler\n"
              << " Source Maps: " << cooked_maps_dir << "\n"
              << " Output Dir:  " << out_dir << "\n"
              << "========================================\n\n";

    std::filesystem::create_directories(out_dir);

    int succeeded = 0;
    int failed = 0;

    for (const auto& target : kRetailMaps) {
        std::filesystem::path folder_path = std::filesystem::path(cooked_maps_dir) / target.folder;
        std::filesystem::path base_pkg = folder_path / target.base_map;
        if (!std::filesystem::exists(base_pkg)) {
            std::string alt = std::filesystem::path(target.base_map).stem().string() + ".u";
            base_pkg = folder_path / alt;
        }
        if (!std::filesystem::exists(base_pkg)) {
            std::cerr << "[-] Skipping target " << target.base_map << ": file not found in " << folder_path << "\n";
            failed++;
            continue;
        }

        std::filesystem::path sub_pkg;
        if (target.sublevel && std::strlen(target.sublevel) > 0) {
            sub_pkg = folder_path / target.sublevel;
            if (!std::filesystem::exists(sub_pkg)) {
                std::string alt = std::filesystem::path(target.sublevel).stem().string() + ".u";
                sub_pkg = folder_path / alt;
            }
        }

        std::string map_stem = std::filesystem::path(target.base_map).stem().string();
        std::string out_path = (std::filesystem::path(out_dir) / (map_stem + ".aa3map")).string();

        std::cout << "\n----------------------------------------\n"
                  << "Baking " << map_stem << " ...\n"
                  << "----------------------------------------\n";

        int res = DoBake(base_pkg.string(), sub_pkg.empty() ? "" : sub_pkg.string(),
                         folder_path.string(), out_path, maps_dir, "", "", inv_path);
        if (res == 0) {
            succeeded++;
        } else {
            failed++;
        }
    }

    std::cout << "\n========================================\n"
              << " Batch Bake Complete: " << succeeded << " succeeded, " << failed << " failed\n"
              << "========================================\n";
    return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    LOG_INIT();
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "--info") {
        if (argc < 3) {
            std::cerr << "Error: --info requires <path.aa3map>\n";
            return 1;
        }
        return DoInfo(argv[2]);
    } else if (mode == "--bake") {
        std::string pkg_path;
        std::string sublevel_path;
        std::string cooked_root;
        std::string maps_dir = "config/maps";
        std::string out_path;
        std::string spawn0;
        std::string spawn1;
        std::string inv_path = "config/spawn_inventory.json";

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--package" && i + 1 < argc) {
                pkg_path = argv[++i];
            } else if (arg == "--sublevel" && i + 1 < argc) {
                sublevel_path = argv[++i];
            } else if (arg == "--cooked-root" && i + 1 < argc) {
                cooked_root = argv[++i];
            } else if (arg == "--maps-dir" && i + 1 < argc) {
                maps_dir = argv[++i];
            } else if (arg == "--out" && i + 1 < argc) {
                out_path = argv[++i];
            } else if (arg == "--spawn0" && i + 1 < argc) {
                spawn0 = argv[++i];
            } else if (arg == "--spawn1" && i + 1 < argc) {
                spawn1 = argv[++i];
            } else if (arg == "--inventory" && i + 1 < argc) {
                inv_path = argv[++i];
            } else {
                std::cerr << "Unknown argument: " << arg << "\n";
                PrintUsage();
                return 1;
            }
        }

        if (pkg_path.empty()) {
            std::cerr << "Error: --bake requires --package <path.u>\n";
            return 1;
        }

        return DoBake(pkg_path, sublevel_path, cooked_root, out_path,
                      maps_dir, spawn0, spawn1, inv_path);
    } else if (mode == "--batch-bake") {
        std::string cooked_maps_dir;
        std::string out_dir = "data/maps";
        std::string maps_dir = "config/maps";
        std::string inv_path = "config/spawn_inventory.json";

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--cooked-maps" && i + 1 < argc) {
                cooked_maps_dir = argv[++i];
            } else if (arg == "--out-dir" && i + 1 < argc) {
                out_dir = argv[++i];
            } else if (arg == "--maps-dir" && i + 1 < argc) {
                maps_dir = argv[++i];
            } else if (arg == "--inventory" && i + 1 < argc) {
                inv_path = argv[++i];
            } else {
                std::cerr << "Unknown argument: " << arg << "\n";
                PrintUsage();
                return 1;
            }
        }

        return DoBatchBake(cooked_maps_dir, out_dir, maps_dir, inv_path);
    } else if (mode == "--pack-loose") {
        std::string map_name;
        std::string gametype_level;
        std::string maps_dir = "config/maps";
        std::string col_dir = "data/collision";
        std::string out_path;
        std::string spawn0;
        std::string spawn1;
        std::string inv_path = "config/spawn_inventory.json";

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--map" && i + 1 < argc) {
                map_name = argv[++i];
            } else if (arg == "--gametype-level" && i + 1 < argc) {
                gametype_level = argv[++i];
            } else if (arg == "--maps-dir" && i + 1 < argc) {
                maps_dir = argv[++i];
            } else if (arg == "--col-dir" && i + 1 < argc) {
                col_dir = argv[++i];
            } else if (arg == "--out" && i + 1 < argc) {
                out_path = argv[++i];
            } else if (arg == "--spawn0" && i + 1 < argc) {
                spawn0 = argv[++i];
            } else if (arg == "--spawn1" && i + 1 < argc) {
                spawn1 = argv[++i];
            } else if (arg == "--inventory" && i + 1 < argc) {
                inv_path = argv[++i];
            } else {
                std::cerr << "Unknown argument: " << arg << "\n";
                PrintUsage();
                return 1;
            }
        }

        if (map_name.empty()) {
            std::cerr << "Error: --pack-loose requires --map <map_name>\n";
            return 1;
        }

        return DoPackLoose(map_name, gametype_level, maps_dir, col_dir,
                           out_path, spawn0, spawn1, inv_path);
    } else {
        std::cerr << "Unknown command: " << mode << "\n";
        PrintUsage();
        return 1;
    }
}

