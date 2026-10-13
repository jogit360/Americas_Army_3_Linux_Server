// aa3_navgrid: walkable routes over a map bundle's collision (game/navgrid).
//
//   aa3_navgrid <bundle> --serve
//       builds the grid once, then answers one query per stdin line,
//       "x1 y1 z1 x2 y2 z2", with one JSON line: [[x, y, z, crouch, door], ...]
//       (crouch: too low to stand; door: the map NetIndex of a door to open,
//       else -1; [] when there's no route). "block x y z r" takes the cells
//       within r of a floor point out of later routes ("ok").
//   aa3_navgrid <bundle> x1 y1 z1 x2 y2 z2
//       one query, then exit.
//
// Doors come from the bundle's map actors (AA_Door); they count as open.
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include "engine/map_bundle.h"
#include "game/navgrid.h"
#include "util/json.h"

using namespace aa3;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: aa3_navgrid <bundle> --serve | x1 y1 z1 x2 y2 z2\n");
        return 2;
    }
    engine::MapBundle bundle;
    if (!bundle.Load(argv[1])) return 1;
    const uint8_t* col = nullptr;
    size_t col_size = 0;
    if (!bundle.GetCollisionData(col, col_size)) return 1;
    game::CollisionWorld world;
    if (!world.LoadFromMemory(col, col_size)) return 1;
    std::vector<uint32_t> doors;
    std::string actors;
    if (bundle.GetActorJson(engine::SectionType::kMapActors, actors))
        if (auto doc = util::ParseJson(actors, nullptr))
            for (const auto& a : (*doc)["actors"].arr)
                if (a["class"].Str() == "AA_Door") doors.push_back(static_cast<uint32_t>(a["obj"]["net"].Int()));
    // The mission's area: every spawn, NPC start, respawn point, the carried
    // objective and the extraction zones, plus a margin.
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    auto grow = [&](const util::JsonValue& v) {
        if (v.arr.size() < 3) return;
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], static_cast<float>(v.arr[static_cast<size_t>(k)].num));
            hi[k] = std::max(hi[k], static_cast<float>(v.arr[static_cast<size_t>(k)].num));
        }
    };
    std::string rules;
    if (bundle.GetMissionRulesJson(rules))
        if (auto r = util::ParseJson(rules, nullptr)) {
            for (const auto& m : (*r)["spawn_markers"].arr) for (const auto& st : m["starts"].arr) grow(st);
            for (const auto& m : (*r)["respawn_markers"].arr) grow(m["location"]);
            for (const auto& st : (*r)["npc"]["starts"].arr) grow(st["location"]);
            grow((*r)["carry"]["location"]);
            for (const auto& e : (*r)["extraction"].arr) for (const auto& b : e["boxes"].arr) { grow(b["min"]); grow(b["max"]); }
        }
    if (lo[0] > hi[0]) { lo[0] = lo[1] = -16000; hi[0] = hi[1] = 16000; lo[2] = 0; hi[2] = 0; }
    const float margin = 2500.0f;
    game::NavGrid grid;
    grid.Build(world, {lo[0] - margin, lo[1] - margin, lo[2] - 2000}, {hi[0] + margin, hi[1] + margin, hi[2] + 2000},
               32.0f, doors);
    std::fprintf(stderr, "navgrid: %zu cells, %zu doors\n", grid.CellCount(), doors.size());
    auto answer = [&](float x1, float y1, float z1, float x2, float y2, float z2) {
        std::string out = "[";
        const auto path = grid.FindPath({x1, y1, z1}, {x2, y2, z2});
        for (size_t i = 0; i < path.size(); ++i) {
            const int32_t c = grid.Nearest(path[i]);
            const auto& cell = grid.Cells()[static_cast<size_t>(c)];
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s[%.0f,%.0f,%.0f,%d,%d]", i ? "," : "", path[i].x, path[i].y, path[i].z,
                          cell.crouch ? 1 : 0, cell.door);
            out += buf;
        }
        std::printf("%s]\n", out.c_str());
        std::fflush(stdout);
    };
    if (std::string(argv[2]) == "--check-route" && argc > 3) {
        // Debug: points (x y z per line, a soldier's locations) -> where the
        // reachable area from the first one stops.
        FILE* f = std::fopen(argv[3], "r");
        float x, y, z;
        std::vector<game::Vec3> pts;
        while (f && std::fscanf(f, "%f %f %f", &x, &y, &z) == 3) pts.push_back({x, y, z - 48});
        if (pts.empty()) return 1;
        bool was = true;
        for (size_t i = 1; i < pts.size(); ++i) {
            const bool ok = !grid.FindPath(pts[0], pts[i]).empty();
            if (ok != was)
                std::printf("route point %zu (%.0f,%.0f,%.0f): %s\n", i, pts[i].x, pts[i].y, pts[i].z + 48,
                            ok ? "reachable again" : "NOT reachable");
            was = ok;
        }
        return 0;
    }
    if (std::string(argv[2]) == "--serve") {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.rfind("block ", 0) == 0) {          // "block x y z r"
                std::istringstream bs(line.substr(6));
                float b[4];
                if (bs >> b[0] >> b[1] >> b[2] >> b[3]) grid.Block({b[0], b[1], b[2]}, b[3]);
                std::printf("ok\n");
                std::fflush(stdout);
                continue;
            }
            std::istringstream ss(line);
            float v[6];
            if (ss >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5]) answer(v[0], v[1], v[2], v[3], v[4], v[5]);
            else std::printf("[]\n"), std::fflush(stdout);
        }
        return 0;
    }
    if (argc < 8) return 2;
    answer(std::stof(argv[2]), std::stof(argv[3]), std::stof(argv[4]), std::stof(argv[5]), std::stof(argv[6]),
           std::stof(argv[7]));
    return 0;
}
