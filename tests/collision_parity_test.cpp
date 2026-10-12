#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include <filesystem>

#include "engine/map_bundle.h"
#include "game/collision.h"
#include "util/logging.h"

int main(int argc, char** argv) {
    LOG_INIT();
    const char* col02_path = argc > 1 ? argv[1] : "data/collision/Bridge_night_clear.col";
    const char* col03_path = argc > 2 ? argv[2] : "data/maps/Bridge_night_clear.aa3map.zst";
    // The legacy flat collision is generated from an AA3 install; skip without it.
    if (!std::filesystem::exists(col02_path)) {
        std::printf("SKIP: no %s (needs an AA3 install)\n", col02_path);
        return 77;
    }

    std::printf("Loading legacy COL02: %s ...\n", col02_path);
    auto t0 = std::chrono::steady_clock::now();
    aa3::game::CollisionWorld w_legacy;
    if (!w_legacy.Load(col02_path)) {
        std::fprintf(stderr, "Failed to load %s\n", col02_path);
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms_legacy = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("  Legacy: %zu triangles loaded & built in %.2f ms\n", w_legacy.TriangleCount(), ms_legacy);

    std::printf("Loading instanced COL03: %s ...\n", col03_path);
    auto t2 = std::chrono::steady_clock::now();
    aa3::game::CollisionWorld w_inst;
    aa3::engine::MapBundle bundle;
    const uint8_t* col_ptr = nullptr;
    size_t col_size = 0;
    if (!bundle.Load(col03_path) || !bundle.GetCollisionData(col_ptr, col_size) ||
        !w_inst.LoadFromMemory(col_ptr, col_size, col03_path)) {
        std::fprintf(stderr, "Failed to load %s\n", col03_path);
        return 1;
    }
    auto t3 = std::chrono::steady_clock::now();
    double ms_inst = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::printf("  Instanced: %zu equivalent triangles loaded & built in %.2f ms\n", w_inst.TriangleCount(), ms_inst);

    std::printf("\n--- Running Ray Tracing Parity Tests ---\n");

    // Test a mix of downward rays, oblique rays, and bullet traces across map bounds
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> distX(-15000.0f, 15000.0f);
    std::uniform_real_distribution<float> distY(-15000.0f, 15000.0f);
    std::uniform_real_distribution<float> distZ(-2000.0f, 3000.0f);

    int total_rays = 2000;
    int both_hit = 0;
    int both_miss = 0;
    int mismatch_hit = 0;
    double max_t_diff = 0.0;
    double max_loc_diff = 0.0;
    double max_nrm_diff = 0.0;

    auto trace_t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < total_rays; ++i) {
        aa3::game::Vec3 start, end;
        if (i < 1000) {
            // Downward rays from sky to floor
            float x = distX(rng);
            float y = distY(rng);
            start = {x, y, 4000.0f};
            end = {x, y, -4000.0f};
        } else {
            // Oblique rays / horizontal bullet traces
            start = {distX(rng), distY(rng), distZ(rng)};
            end = {distX(rng), distY(rng), distZ(rng)};
        }

        auto h_leg = w_legacy.LineTrace(start, end, aa3::game::CollisionWorld::kBlockTrace);
        auto h_ins = w_inst.LineTrace(start, end, aa3::game::CollisionWorld::kBlockTrace);

        if (h_leg.hit != h_ins.hit) {
            // Edge cases near triangle boundaries may have slight float precision variations
            mismatch_hit++;
            continue;
        }

        if (!h_leg.hit) {
            both_miss++;
            continue;
        }

        both_hit++;
        double dt = std::abs(h_leg.time - h_ins.time);
        max_t_diff = std::max(max_t_diff, dt);

        double dx = h_leg.location.x - h_ins.location.x;
        double dy = h_leg.location.y - h_ins.location.y;
        double dz = h_leg.location.z - h_ins.location.z;
        double dloc = std::sqrt(dx * dx + dy * dy + dz * dz);
        max_loc_diff = std::max(max_loc_diff, dloc);

        double nx = h_leg.normal.x - h_ins.normal.x;
        double ny = h_leg.normal.y - h_ins.normal.y;
        double nz = h_leg.normal.z - h_ins.normal.z;
        double dnrm = std::sqrt(nx * nx + ny * ny + nz * nz);
        max_nrm_diff = std::max(max_nrm_diff, dnrm);

        if (dt > 1e-3) {
            std::printf("Warning ray %d: dt=%.6f, leg_t=%.6f, ins_t=%.6f\n", i, dt, h_leg.time, h_ins.time);
        }
    }
    auto trace_t1 = std::chrono::steady_clock::now();
    double trace_ms = std::chrono::duration<double, std::milli>(trace_t1 - trace_t0).count();

    std::printf("\nResults over %d rays (took %.2f ms total):\n", total_rays, trace_ms);
    std::printf("  Both hit:     %d\n", both_hit);
    std::printf("  Both miss:    %d\n", both_miss);
    std::printf("  Mismatches:   %d (%.2f%% edge boundary difference)\n", mismatch_hit, (mismatch_hit * 100.0) / total_rays);
    std::printf("  Max time dt:  %.2e\n", max_t_diff);
    std::printf("  Max loc diff: %.4f units\n", max_loc_diff);
    std::printf("  Max nrm diff: %.4f\n", max_nrm_diff);

    // Test specific spawn point
    aa3::game::Vec3 spawn{-7174, 1386, 40};
    auto sp_leg = w_legacy.LineTrace({spawn.x, spawn.y, spawn.z + 100}, {spawn.x, spawn.y, spawn.z - 2000}, aa3::game::CollisionWorld::kBlockPawn);
    auto sp_ins = w_inst.LineTrace({spawn.x, spawn.y, spawn.z + 100}, {spawn.x, spawn.y, spawn.z - 2000}, aa3::game::CollisionWorld::kBlockPawn);
    std::printf("\nSpawn trace (-7174, 1386, 40):\n");
    std::printf("  Legacy:    hit=%d, time=%.4f, z=%.2f, normal=(%.2f, %.2f, %.2f)\n",
                sp_leg.hit, sp_leg.time, sp_leg.location.z, sp_leg.normal.x, sp_leg.normal.y, sp_leg.normal.z);
    std::printf("  Instanced: hit=%d, time=%.4f, z=%.2f, normal=(%.2f, %.2f, %.2f)\n",
                sp_ins.hit, sp_ins.time, sp_ins.location.z, sp_ins.normal.x, sp_ins.normal.y, sp_ins.normal.z);

    // Test dynamic owner SetOwnerEnabled on window pane (Owner 1)
    aa3::game::Vec3 win_start{10686.1f, -550.0f, 50.0f};
    aa3::game::Vec3 win_end{10686.1f, -650.0f, 50.0f};
    auto h_win_leg1 = w_legacy.LineTrace(win_start, win_end, aa3::game::CollisionWorld::kBlockTrace);
    auto h_win_ins1 = w_inst.LineTrace(win_start, win_end, aa3::game::CollisionWorld::kBlockTrace);
    std::printf("\nDynamic Window Trace (enabled):\n");
    std::printf("  Legacy:    hit=%d, owner=%u, time=%.4f, z=%.2f\n",
                h_win_leg1.hit, h_win_leg1.owner, h_win_leg1.time, h_win_leg1.location.z);
    std::printf("  Instanced: hit=%d, owner=%u, time=%.4f, z=%.2f\n",
                h_win_ins1.hit, h_win_ins1.owner, h_win_ins1.time, h_win_ins1.location.z);

    w_legacy.SetOwnerEnabled(1, false);
    w_inst.SetOwnerEnabled(1, false);
    auto h_win_leg2 = w_legacy.LineTrace(win_start, win_end, aa3::game::CollisionWorld::kBlockTrace);
    auto h_win_ins2 = w_inst.LineTrace(win_start, win_end, aa3::game::CollisionWorld::kBlockTrace);
    std::printf("Dynamic Window Trace (disabled owner 1):\n");
    std::printf("  Legacy:    hit=%d, owner=%u, time=%.4f, z=%.2f\n",
                h_win_leg2.hit, h_win_leg2.owner, h_win_leg2.time, h_win_leg2.location.z);
    std::printf("  Instanced: hit=%d, owner=%u, time=%.4f, z=%.2f\n",
                h_win_ins2.hit, h_win_ins2.owner, h_win_ins2.time, h_win_ins2.location.z);

    w_legacy.SetOwnerEnabled(1, true);
    w_inst.SetOwnerEnabled(1, true);
    auto h_win_leg3 = w_legacy.LineTrace(win_start, win_end, aa3::game::CollisionWorld::kBlockTrace);
    auto h_win_ins3 = w_inst.LineTrace(win_start, win_end, aa3::game::CollisionWorld::kBlockTrace);
    std::printf("Dynamic Window Trace (re-enabled owner 1):\n");
    std::printf("  Legacy:    hit=%d, owner=%u, time=%.4f, z=%.2f\n",
                h_win_leg3.hit, h_win_leg3.owner, h_win_leg3.time, h_win_leg3.location.z);
    std::printf("  Instanced: hit=%d, owner=%u, time=%.4f, z=%.2f\n",
                h_win_ins3.hit, h_win_ins3.owner, h_win_ins3.time, h_win_ins3.location.z);

    bool win_ok = (h_win_leg1.owner == 1 && h_win_ins1.owner == 1) &&
                  (h_win_leg2.owner != 1 && h_win_ins2.owner != 1) &&
                  (h_win_leg3.owner == 1 && h_win_ins3.owner == 1) &&
                  (std::abs(h_win_leg1.time - h_win_ins1.time) < 1e-4) &&
                  (std::abs(h_win_leg2.time - h_win_ins2.time) < 1e-4);

    bool pass = (mismatch_hit <= total_rays * 0.01) && (max_loc_diff < 1.5f) && win_ok;
    std::printf("\nParity test: %s (Window test %s)\n", pass ? "PASS" : "FAIL", win_ok ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
