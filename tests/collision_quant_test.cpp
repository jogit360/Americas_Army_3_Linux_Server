#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <vector>

#include "engine/package_extractor.h"
#include "engine/package_reader.h"
#include "game/collision.h"
#include "util/logging.h"

using namespace aa3;
using namespace aa3::engine;
using namespace aa3::game;

int main(int argc, char** argv) {
    LOG_INIT();
    const char* pkg_path = argc > 1 ? argv[1] : "/home/odeyin/Documents/AA3/clean_packages/Bridge_night_clear.u";

    if (!std::filesystem::exists(pkg_path)) {
        std::printf("SKIP: no %s (needs an AA3 install)\n", pkg_path);
        return 77;
    }
    std::printf("========================================\n");
    std::printf(" Phase 3: Quantized Mesh Parity Test\n");
    std::printf(" Target package: %s\n", pkg_path);
    std::printf("========================================\n\n");

    auto pkg = PackageReader::ReadFile(pkg_path);
    if (!pkg) {
        std::fprintf(stderr, "Failed to read package: %s\n", pkg_path);
        return 1;
    }

    PackageExtractor extractor(*pkg);

    // 1. Extract unquantized AA3COL03
    std::vector<uint8_t> col03_bytes;
    uint32_t m3 = 0, i3 = 0, d3 = 0, t3 = 0;
    auto ext_t0 = std::chrono::steady_clock::now();
    if (!extractor.ExtractCollision(col03_bytes, m3, i3, d3, t3, /*quantize=*/false)) {
        std::fprintf(stderr, "Failed to extract COL03\n");
        return 1;
    }
    auto ext_t1 = std::chrono::steady_clock::now();
    double ms_ext03 = std::chrono::duration<double, std::milli>(ext_t1 - ext_t0).count();

    // 2. Extract quantized AA3COL04
    std::vector<uint8_t> col04_bytes;
    uint32_t m4 = 0, i4 = 0, d4 = 0, t4 = 0;
    auto ext_t2 = std::chrono::steady_clock::now();
    if (!extractor.ExtractCollision(col04_bytes, m4, i4, d4, t4, /*quantize=*/true)) {
        std::fprintf(stderr, "Failed to extract COL04\n");
        return 1;
    }
    auto ext_t3 = std::chrono::steady_clock::now();
    double ms_ext04 = std::chrono::duration<double, std::milli>(ext_t3 - ext_t2).count();

    std::printf("Extraction Results:\n");
    std::printf("  AA3COL03 (Unquantized): %zu bytes (%.2f MB) in %.2f ms\n",
                col03_bytes.size(), col03_bytes.size() / (1024.0 * 1024.0), ms_ext03);
    std::printf("  AA3COL04 (Quantized):   %zu bytes (%.2f MB) in %.2f ms\n",
                col04_bytes.size(), col04_bytes.size() / (1024.0 * 1024.0), ms_ext04);
    double size_saved_mb = (col03_bytes.size() - col04_bytes.size()) / (1024.0 * 1024.0);
    double pct_saved = 100.0 * (1.0 - static_cast<double>(col04_bytes.size()) / col03_bytes.size());
    std::printf("  -> Storage Savings:     %.2f MB (%.1f%% reduction!)\n\n", size_saved_mb, pct_saved);

    // 3. Load into CollisionWorld
    CollisionWorld w03, w04;
    auto load_t0 = std::chrono::steady_clock::now();
    if (!w03.LoadFromMemory(col03_bytes.data(), col03_bytes.size(), "COL03")) {
        std::fprintf(stderr, "Failed to load COL03\n");
        return 1;
    }
    auto load_t1 = std::chrono::steady_clock::now();
    double ms_load03 = std::chrono::duration<double, std::milli>(load_t1 - load_t0).count();

    auto load_t2 = std::chrono::steady_clock::now();
    if (!w04.LoadFromMemory(col04_bytes.data(), col04_bytes.size(), "COL04")) {
        std::fprintf(stderr, "Failed to load COL04\n");
        return 1;
    }
    auto load_t3 = std::chrono::steady_clock::now();
    double ms_load04 = std::chrono::duration<double, std::milli>(load_t3 - load_t2).count();

    std::printf("Load & BVH Build Times:\n");
    std::printf("  COL03 Load Time: %.2f ms\n", ms_load03);
    std::printf("  COL04 Load Time: %.2f ms (including dequantization)\n\n", ms_load04);

    // 4. Parity Testing across 2,000 rays
    std::printf("--- Running 2,000 Ray Parity Tests ---\n");
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> distX(-15000.0f, 15000.0f);
    std::uniform_real_distribution<float> distY(-15000.0f, 15000.0f);
    std::uniform_real_distribution<float> distZ(-2000.0f, 3000.0f);

    int total_rays = 2000;
    int both_hit = 0;
    int both_miss = 0;
    int mismatches = 0;
    double max_loc_diff = 0.0;
    double max_t_diff = 0.0;

    auto trace_t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < total_rays; ++i) {
        Vec3 start, end;
        if (i < 1000) {
            float x = distX(rng);
            float y = distY(rng);
            start = {x, y, 4000.0f};
            end = {x, y, -4000.0f};
        } else {
            start = {distX(rng), distY(rng), distZ(rng)};
            end = {distX(rng), distY(rng), distZ(rng)};
        }

        TraceHit h03 = w03.LineTrace(start, end);
        TraceHit h04 = w04.LineTrace(start, end);

        if (h03.hit && h04.hit) {
            both_hit++;
            double dt = std::fabs(h03.time - h04.time);
            double dx = h03.location.x - h04.location.x;
            double dy = h03.location.y - h04.location.y;
            double dz = h03.location.z - h04.location.z;
            double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            max_t_diff = std::max(max_t_diff, dt);
            max_loc_diff = std::max(max_loc_diff, dist);
        } else if (!h03.hit && !h04.hit) {
            both_miss++;
        } else {
            mismatches++;
        }
    }
    auto trace_t1 = std::chrono::steady_clock::now();
    double ms_trace = std::chrono::duration<double, std::milli>(trace_t1 - trace_t0).count();

    std::printf("Parity Test Results (%d rays in %.2f ms):\n", total_rays, ms_trace);
    std::printf("  Both Hit:        %d\n", both_hit);
    std::printf("  Both Miss:       %d\n", both_miss);
    std::printf("  Mismatches:      %d\n", mismatches);
    std::printf("  Max delta-t:     %.6f\n", max_t_diff);
    std::printf("  Max delta-pos:   %.4f units (< 1.0 units max quantization shift!)\n", max_loc_diff);

    // 5. Dynamic Window Toggling Test on COL04
    std::printf("\n--- Verifying Dynamic Window Toggling on COL04 ---\n");
    Vec3 win_start = {10686.1f, -550.0f, 50.0f};
    Vec3 win_end   = {10686.1f, -650.0f, 50.0f};

    TraceHit wh1 = w04.LineTrace(win_start, win_end);
    w04.SetOwnerEnabled(wh1.owner, false);
    TraceHit wh2 = w04.LineTrace(win_start, win_end);
    w04.SetOwnerEnabled(wh1.owner, true);
    TraceHit wh3 = w04.LineTrace(win_start, win_end);

    bool win_ok = (wh1.hit && wh1.owner == 1 && !wh2.hit && wh3.hit && wh3.owner == 1);
    std::printf("  Window test: %s (enabled=hit, disabled=pass, re-enabled=hit, owner=%u)\n",
                win_ok ? "PASS" : "FAIL", wh1.owner);

    if (mismatches == 0 && max_loc_diff < 1.0 && win_ok) {
        std::printf("\n========================================\n");
        std::printf(" Phase 3 Quantized Compression: PASS\n");
        std::printf("========================================\n");
        return 0;
    } else {
        std::fprintf(stderr, "\nPhase 3 Verification FAILED\n");
        return 1;
    }
}
