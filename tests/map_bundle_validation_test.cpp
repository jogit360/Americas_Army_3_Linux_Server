// Checks every baked map bundle (data/maps/*.aa3map) for what the client
// needs to accept the server's actors:
//   - each actor's MaxIndex is its class's (config/class_maxindex_table.tsv);
//     a wrong one changes the RepIndex bit width and desyncs the bunch
//   - each object reference (actor, class default, field) is a NetIndex
//     inside its package's net object range in the bundle's USES list
//   - every role has a weapon, every fire team a spawn marker the rules know
//   - every spawn start stands on collision (ground within 300 units below)
//
//   map_bundle_validation_test [bundle.aa3map ...]

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine/map_bundle.h"
#include "engine/mission_extractor.h"
#include "game/collision.h"
#include "util/json.h"
#include "util/logging.h"

using namespace aa3;

namespace {

int CheckBundle(const std::string& path, const std::map<std::string, uint32_t>& max_table) {
    engine::MapBundle bundle;
    if (!bundle.Load(path)) {
        std::printf("FAIL %s: does not load\n", path.c_str());
        return 1;
    }
    std::vector<std::string> errs;
    auto manifest = bundle.GetManifest();
    if (!manifest) {
        std::printf("FAIL %s: no manifest\n", path.c_str());
        return 1;
    }
    std::map<std::string, int32_t> net_objects;
    for (const auto& u : manifest->uses) net_objects[u.pkg] = u.net_objects;
    auto check_ref = [&](const std::string& what, const std::string& pkg, int64_t net) {
        auto it = net_objects.find(pkg);
        if (it == net_objects.end()) errs.push_back(what + ": package " + pkg + " not in USES");
        else if (net < 0 || net >= it->second)
            errs.push_back(what + ": " + pkg + " NetIndex " + std::to_string(net) + " >= " + std::to_string(it->second));
    };

    int roles = 0, armed = 0;
    std::vector<int64_t> fire_team_markers;
    for (auto type : {engine::SectionType::kMapActors, engine::SectionType::kRoundActors,
                      engine::SectionType::kJoinActors, engine::SectionType::kMatchActors}) {
        std::string json;
        if (!bundle.GetActorJson(type, json)) continue;
        auto doc = util::ParseJson(json);
        if (!doc) { errs.push_back(std::string("bad JSON in ") + engine::SectionTypeName(type)); continue; }
        for (const auto& a : (*doc)["actors"].arr) {
            const std::string cls = a["class"].Str();
            const util::JsonValue& ref = a.Has("obj") ? a["obj"] : a["cdo"];
            check_ref(cls, ref["pkg"].Str(), ref["net"].Int(-1));
            auto mi = max_table.find(cls);
            if (mi == max_table.end() || mi->second != a["max_index"].Int())
                errs.push_back(cls + ": max_index " + std::to_string(a["max_index"].Int()) + " != class table " +
                               (mi == max_table.end() ? "?" : std::to_string(mi->second)));
            bool weapon = false;
            for (const auto& f : a["fields"].arr) {
                if (f["idx"].Int() >= a["max_index"].Int()) errs.push_back(cls + ": field index past MaxIndex");
                if (f["kind"].Str() == "obj") check_ref(cls + " field", f["pkg"].Str(), f["net"].Int(-1));
                if (cls == "AA_MCClassRoleReplicationInfo" && f["idx"].Int() == 88) weapon = true;   // wpnPrimaryOne
                if (cls == "AA_FireTeamInfo" && f["idx"].Int() == 27) fire_team_markers.push_back(f["net"].Int());
            }
            if (cls == "AA_MCClassRoleReplicationInfo") { ++roles; armed += weapon; }
        }
    }
    if (armed != roles) errs.push_back(std::to_string(roles - armed) + " role(s) without wpnPrimaryOne");

    // Spawns: fire team markers' starts, else the team spawn points.
    std::vector<std::array<float, 3>> starts;
    std::string rules_json;
    if (bundle.GetMissionRulesJson(rules_json)) {
        auto rules = util::ParseJson(rules_json);
        if (!rules) {
            errs.push_back("bad mission rules JSON");
        } else {
            std::vector<int64_t> known;
            for (const auto& m : (*rules)["spawn_markers"].arr) {
                known.push_back(m["net"].Int());
                for (const auto& s : m["starts"].arr)
                    starts.push_back({static_cast<float>(s.arr[0].num), static_cast<float>(s.arr[1].num),
                                      static_cast<float>(s.arr[2].num)});
            }
            for (int64_t m : fire_team_markers)
                if (std::find(known.begin(), known.end(), m) == known.end())
                    errs.push_back("fire team spawn marker " + std::to_string(m) + " not in the rules");
        }
    }
    if (starts.empty())
        for (int t = 0; t < 2; ++t) {
            float x, y, z;
            int yaw;
            if (bundle.GetSpawnPoint(t, x, y, z, yaw)) starts.push_back({x, y, z});
        }

    game::CollisionWorld col;
    const uint8_t* cp = nullptr;
    size_t cs = 0;
    int floating = 0;
    if (bundle.GetCollisionData(cp, cs) && col.LoadFromMemory(cp, cs, bundle.MapName())) {
        for (const auto& s : starts) {
            auto hit = col.LineTrace({s[0], s[1], s[2] + 20.0f}, {s[0], s[1], s[2] - 300.0f});
            if (!hit.hit) {
                ++floating;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "no ground under spawn (%.0f, %.0f, %.0f)", s[0], s[1], s[2]);
                errs.push_back(buf);
            }
        }
    } else {
        errs.push_back("no collision");
    }

    std::printf("%s %-32s roles=%2d spawns=%2zu errors=%zu\n", errs.empty() ? "ok  " : "FAIL",
                bundle.MapName().c_str(), roles, starts.size(), errs.size());
    for (size_t k = 0; k < errs.size() && k < 8; ++k) std::printf("       %s\n", errs[k].c_str());
    return errs.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    LOG_INIT();
    LOG_SET_LEVEL(LOG_LEVEL_WARN);
    auto max_table = engine::LoadClassMaxIndexTable("config/class_maxindex_table.tsv");
    if (max_table.empty()) {
        std::printf("FAIL: config/class_maxindex_table.tsv not found (run from the repo root)\n");
        return 1;
    }
    std::vector<std::string> bundles;
    for (int i = 1; i < argc; ++i) bundles.push_back(argv[i]);
    if (bundles.empty() && std::filesystem::exists("data/maps"))
        for (const auto& e : std::filesystem::directory_iterator("data/maps"))
            if (e.path().extension() == ".aa3map" ||
                (e.path().extension() == ".zst" && e.path().stem().extension() == ".aa3map"))
                bundles.push_back(e.path().string());
    std::sort(bundles.begin(), bundles.end());
    if (bundles.empty()) {
        std::printf("FAIL: no bundles in data/maps\n");
        return 1;
    }
    int failed = 0;
    for (const auto& b : bundles) failed += CheckBundle(b, max_table);
    std::printf("%zu bundle(s), %d failed\n", bundles.size(), failed);
    return failed ? 1 : 0;
}
