// Collision world check (doc sec 32): loads data/collision/<map>.col and
// traces straight down under retail player positions (one "x y z" per line,
// from ServerMove ClientLoc). A standing pawn's location is its collision
// half-height (~48) above the floor.
//   collision_test data/collision/Bridge_night_clear.col retail_locs.txt
#include <chrono>
#include <cstdio>
#include <fstream>
#include "game/collision.h"
#include "util/logging.h"

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <map.col> <locs.txt>\n", argv[0]); return 2; }
    LOG_INIT();
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    aa3::game::CollisionWorld w;
    if (!w.Load(argv[1])) return 1;
    auto t1 = clk::now();
    std::ifstream f(argv[2]);
    float x, y, z;
    int n = 0, standing = 0, miss = 0;
    auto t2 = clk::now();
    while (f >> x >> y >> z) {
        auto h = w.LineTrace({x, y, z + 40}, {x, y, z - 3000}, aa3::game::CollisionWorld::kBlockPawn);
        ++n;
        if (!h.hit) { ++miss; continue; }
        float dz = z - h.location.z;
        if (dz >= 44 && dz <= 60) ++standing;
    }
    auto t3 = clk::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("%zu tris, load+build %.0f ms; %d rays in %.2f ms: %d standing (44..60), %d misses\n",
                w.TriangleCount(), ms(t0, t1), n, ms(t2, t3), standing, miss);
    // Same bar as the Python validation: >= 95% standing, no misses.
    bool ok = miss == 0 && standing * 100 >= n * 95;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
