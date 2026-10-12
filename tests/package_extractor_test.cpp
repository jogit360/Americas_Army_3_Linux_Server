#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "engine/map_bundle.h"
#include "engine/package_extractor.h"
#include "engine/package_reader.h"
#include "game/collision.h"
#include "util/logging.h"

using namespace aa3;
using namespace aa3::engine;

int main(int argc, char** argv) {
    LOG_INIT();
    const char* pkg_path = argc > 1 ? argv[1] : "/home/odeyin/Documents/AA3/clean_packages/Bridge_night_clear.u";
    const char* ref_col_path = argc > 2 ? argv[2] : "data/maps/Bridge_night_clear.aa3map.zst";
    if (!std::filesystem::exists(pkg_path)) {
        std::printf("SKIP: no %s (needs an AA3 install)\n", pkg_path);
        return 77;
    }

    std::printf("Testing PackageExtractor on %s ...\n", pkg_path);
    auto t0 = std::chrono::steady_clock::now();
    PackageReadError perr;
    auto pkg_opt = PackageReader::ReadFile(pkg_path, &perr);
    if (!pkg_opt) {
        std::fprintf(stderr, "Failed to read package: %s\n", perr.message.c_str());
        return 1;
    }
    const Package& pkg = *pkg_opt;
    PackageExtractor extractor(pkg);

    // 1. Windows test
    std::map<uint32_t, int> windows;
    if (!extractor.ExtractWindows(windows)) {
        std::fprintf(stderr, "ExtractWindows failed!\n");
        return 1;
    }
    std::printf("  [+] Windows extracted: %zu breakable windows mapped\n", windows.size());

    // 2. Spawns test
    std::vector<SpawnPointDef> spawns;
    if (extractor.ExtractSpawns(spawns)) {
        std::printf("  [+] Spawns extracted: %zu spawn points found\n", spawns.size());
    }

    // 3. Collision extraction test
    std::vector<uint8_t> col_bytes;
    uint32_t n_meshes = 0, n_inst = 0, n_direct = 0, n_total = 0;
    auto ct0 = std::chrono::steady_clock::now();
    if (!extractor.ExtractCollision(col_bytes, n_meshes, n_inst, n_direct, n_total)) {
        std::fprintf(stderr, "ExtractCollision failed!\n");
        return 1;
    }
    auto ct1 = std::chrono::steady_clock::now();
    double col_ms = std::chrono::duration<double, std::milli>(ct1 - ct0).count();
    std::printf("  [+] Collision extracted in %.2f ms: %u meshes, %u instances, %u direct tris, %u total tris (%.2f MB)\n",
                col_ms, n_meshes, n_inst, n_direct, n_total, col_bytes.size() / (1024.0 * 1024.0));

    // Write to /tmp/Bridge_cpp_extracted.col
    const char* out_tmp_col = "/tmp/Bridge_cpp_extracted.col";
    std::ofstream out_f(out_tmp_col, std::ios::binary);
    out_f.write(reinterpret_cast<const char*>(col_bytes.data()), col_bytes.size());
    out_f.close();

    // 4. Parity test against Python-exported reference
    std::printf("\n--- Verifying Ray Tracing Parity vs Python reference (%s) ---\n", ref_col_path);
    game::CollisionWorld ref_world;
    MapBundle ref_bundle;
    const uint8_t* ref_ptr = nullptr;
    size_t ref_size = 0;
    if (!ref_bundle.Load(ref_col_path) || !ref_bundle.GetCollisionData(ref_ptr, ref_size) ||
        !ref_world.LoadFromMemory(ref_ptr, ref_size, ref_col_path)) {
        std::fprintf(stderr, "Failed to load reference col %s\n", ref_col_path);
        return 1;
    }

    game::CollisionWorld cpp_world;
    if (!cpp_world.LoadFromMemory(col_bytes.data(), col_bytes.size(), "cpp_extracted")) {
        std::fprintf(stderr, "Failed to load cpp_extracted collision!\n");
        return 1;
    }

    int total_rays = 2000;
    int both_hit = 0, both_miss = 0, mismatch_hit = 0;
    double max_t_diff = 0.0, max_loc_diff = 0.0, max_nrm_diff = 0.0;

    for (int i = 0; i < total_rays; ++i) {
        float x = -15000.0f + (i * 17.317f) - (int)((i * 17.317f) / 30000.0f) * 30000.0f;
        float y = -15000.0f + (i * 29.541f) - (int)((i * 29.541f) / 30000.0f) * 30000.0f;
        game::Vec3 start{x, y, 4000.0f};
        game::Vec3 end{x, y, -4000.0f};

        auto h_ref = ref_world.LineTrace(start, end, game::CollisionWorld::kBlockTrace);
        auto h_cpp = cpp_world.LineTrace(start, end, game::CollisionWorld::kBlockTrace);

        if (h_ref.hit != h_cpp.hit) {
            mismatch_hit++;
            continue;
        }
        if (!h_ref.hit) {
            both_miss++;
            continue;
        }
        both_hit++;
        max_t_diff = std::max(max_t_diff, (double)std::abs(h_ref.time - h_cpp.time));
        float dx = h_ref.location.x - h_cpp.location.x;
        float dy = h_ref.location.y - h_cpp.location.y;
        float dz = h_ref.location.z - h_cpp.location.z;
        max_loc_diff = std::max(max_loc_diff, (double)std::sqrt(dx * dx + dy * dy + dz * dz));
    }

    std::printf("Parity results across %d rays:\n", total_rays);
    std::printf("  Both hit:     %d\n", both_hit);
    std::printf("  Both miss:    %d\n", both_miss);
    std::printf("  Mismatches:   %d\n", mismatch_hit);
    std::printf("  Max time dt:  %.2e\n", max_t_diff);
    std::printf("  Max loc diff: %.4f units\n", max_loc_diff);

    bool pass = (mismatch_hit <= total_rays * 0.01) && (max_loc_diff < 0.1);
    std::printf("\nPhase 2 Direct Extraction Test: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
