#include <cstdio>
#include <memory>
#include <string>

#include "engine/map_bundle.h"
#include "engine/map_manifest.h"
#include "game/world.h"
#include "util/logging.h"

using namespace aa3;
using namespace aa3::engine;
using namespace aa3::game;

int main(int argc, char** argv) {
    LOG_INIT();
    const char* bundle_path = argc > 1 ? argv[1] : "data/maps/Pipeline_dawn_clear.aa3map.zst";

    std::printf("========================================\n");
    std::printf(" Phase 4: Mission & Game-Mode Extraction Test\n");
    std::printf(" Testing bundle: %s\n", bundle_path);
    std::printf("========================================\n");

    auto bundle = std::make_unique<MapBundle>();
    if (!bundle->Load(bundle_path)) {
        std::fprintf(stderr, "Failed to load bundle: %s\n", bundle_path);
        return 1;
    }

    std::printf("[+] Bundle loaded successfully: map='%s', gametype='%s'\n",
                bundle->MapName().c_str(), bundle->GameModeLevel().c_str());

    // 1. Verify all mission sections exist
    bool has_manifest = bundle->HasSection(SectionType::kManifest);
    bool has_col = bundle->HasSection(SectionType::kCollision);
    bool has_map_actors = bundle->HasSection(SectionType::kMapActors);
    bool has_round_actors = bundle->HasSection(SectionType::kRoundActors);
    bool has_join_actors = bundle->HasSection(SectionType::kJoinActors);
    bool has_match_actors = bundle->HasSection(SectionType::kMatchActors);
    bool has_rules = bundle->HasSection(SectionType::kMissionRules);
    bool has_spawns = bundle->HasSection(SectionType::kSpawns);
    bool has_inventory = bundle->HasSection(SectionType::kSpawnInventory);

    std::printf("Section Validation:\n");
    std::printf("  Manifest:       %s\n", has_manifest ? "PRESENT" : "MISSING");
    std::printf("  Collision:      %s\n", has_col ? "PRESENT" : "MISSING");
    std::printf("  MapActors:      %s\n", has_map_actors ? "PRESENT" : "MISSING");
    std::printf("  RoundActors:    %s\n", has_round_actors ? "PRESENT" : "MISSING");
    std::printf("  JoinActors:     %s\n", has_join_actors ? "PRESENT" : "MISSING");
    std::printf("  MatchActors:    %s\n", has_match_actors ? "PRESENT" : "MISSING");
    std::printf("  MissionRules:   %s\n", has_rules ? "PRESENT" : "MISSING");
    std::printf("  Spawns:         %s\n", has_spawns ? "PRESENT" : "MISSING");
    std::printf("  SpawnInventory: %s\n", has_inventory ? "PRESENT" : "MISSING");

    if (!has_manifest || !has_round_actors || !has_map_actors || !has_join_actors || !has_match_actors || !has_rules) {
        std::fprintf(stderr, "[-] Critical mission sections are missing!\n");
        return 1;
    }

    // 2. Parse Manifest
    auto manifest = bundle->GetManifest();
    if (!manifest) {
        std::fprintf(stderr, "Failed to get manifest from bundle\n");
        return 1;
    }

    // 3. Initialize World and Bind Package Map
    World world;
    world.Initialize(manifest->url);

    if (!world.BindPackageMap(*manifest, bundle->GameModeLevel(), bundle.get())) {
        std::fprintf(stderr, "Failed to bind package map with bundle\n");
        return 1;
    }

    std::printf("\nWorld Ingestion Results:\n");
    std::printf("  Map Actors:     %zu instantiated\n", world.MapActors().size());
    std::printf("  Join Actors:    %zu instantiated\n", world.JoinActors().size());
    std::printf("  Match Actors:   %zu instantiated\n", world.MatchActors().size());

    // Verify team structures
    ServerTeamInfo* team0 = world.FindTeam(0);
    ServerTeamInfo* team1 = world.FindTeam(1);

    if (!team0 || !team1) {
        std::fprintf(stderr, "Failed to find Team 0 or Team 1\n");
        return 1;
    }

    std::printf("  Team 0 (Assault): valid, round_fields=%zu\n", team0->round_fields.size());
    std::printf("  Team 1 (Defend):  valid, round_fields=%zu\n", team1->round_fields.size());

    if (world.MapActors().empty()) {
        std::fprintf(stderr, "[-] No map actors were instantiated!\n");
        return 1;
    }

    std::printf("\n========================================\n");
    std::printf(" Phase 4 Mission Extraction: PASS\n");
    std::printf("========================================\n");
    return 0;
}
