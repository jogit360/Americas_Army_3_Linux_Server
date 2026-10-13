#ifndef AA3_GAME_COLLISION_H
#define AA3_GAME_COLLISION_H

// Server-side collision world (doc sec 32): the map's static geometry --
// static mesh kDOP triangles, terrain heightmaps and BSP.
// Supports both:
//   - Legacy flat collision (AA3COL01, AA3COL02): flat list of triangles
//   - Modern instanced collision (AA3COL03): unique meshes + BLAS, placed
//     instances + TLAS, plus direct terrain and BSP geometry. Reduces memory
//     from ~100MB to ~15MB and cuts load time dramatically.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace aa3 {
namespace game {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct TraceHit {
    bool hit = false;
    float time = 1.0f;          // fraction of the segment, 0..1
    Vec3 location;
    Vec3 normal;                // unit, facing the trace start
    uint32_t triangle = 0;      // direct triangle, or 0x80000000 | mesh instance index

    uint32_t owner = 0;         // map NetIndex + 1 of a dynamic actor (window/door), 0 = static
};

class CollisionWorld {
public:
    // Triangle flags (export_collision.py).
    static constexpr uint8_t kBlockPawn = 1;
    static constexpr uint8_t kBlockTrace = 2;

    bool Load(const std::string& path);
    bool LoadFromMemory(const uint8_t* data, size_t size, const std::string& source_name = "memory");
    bool Loaded() const { return total_triangles_ > 0 || !tris_.empty(); }
    size_t TriangleCount() const { return total_triangles_ > 0 ? total_triangles_ : tris_.size(); }

    // Closest hit on the segment start->end against triangles whose flags
    // include any bit of `mask` (both faces count).
    TraceHit LineTrace(const Vec3& start, const Vec3& end, uint8_t mask = kBlockTrace) const;

    // World bounds of a dynamic actor's triangles (owner = NetIndex + 1).
    bool OwnerBounds(uint32_t owner, Vec3& lo, Vec3& hi) const {
        bool any = false;
        for (const auto& in : instances_)
            if (in.owner == owner) {
                if (!any) { lo = in.world_min; hi = in.world_max; any = true; continue; }
                lo = {std::min(lo.x, in.world_min.x), std::min(lo.y, in.world_min.y), std::min(lo.z, in.world_min.z)};
                hi = {std::max(hi.x, in.world_max.x), std::max(hi.y, in.world_max.y), std::max(hi.z, in.world_max.z)};
            }
        return any;
    }
    // A mesh instance's world bounds (debugging: which object a hit is).
    bool InstanceBounds(uint32_t i, Vec3& lo, Vec3& hi) const {
        if (i >= instances_.size()) return false;
        lo = instances_[i].world_min; hi = instances_[i].world_max; return true;
    }

    // Stop/restart tracing against a dynamic actor's triangles (a broken window).
    void SetOwnerEnabled(uint32_t owner, bool enabled);

private:
    struct Tri {
        Vec3 v0, e1, e2;        // v0 and edges, for Moller-Trumbore
        uint8_t flags;
        uint32_t owner;
    };
    struct Node {
        Vec3 lo, hi;
        uint32_t first;         // leaf: first index into order_; inner: right child
        uint32_t count;         // leaf: triangle count; inner: 0
    };

    // Instanced geometry support (AA3COL03)
    struct UniqueMesh {
        uint32_t mesh_id = 0;
        Vec3 bounds_min, bounds_max;
        struct LocalTri {
            Vec3 v0, e1, e2;
        };
        std::vector<LocalTri> tris;
        std::vector<uint32_t> order;
        std::vector<Node> bvh;
    };

    struct MeshInstance {
        uint32_t mesh_idx = 0;
        Vec3 world_min, world_max;
        Vec3 loc;
        float inv_mat[3][3];    // local = (world - loc) @ inv_mat
        float inv_trans[3][3];  // for surface normal
        uint8_t flags = 0;
        uint32_t owner = 0;
    };

    bool LoadCol01_02(const uint8_t* data, size_t size, bool v2, const std::string& source_name);
    bool LoadCol03(const uint8_t* data, size_t size, const std::string& source_name);
    bool LoadCol04(const uint8_t* data, size_t size, const std::string& source_name);

    void BuildFlatBvh();
    uint32_t BuildRange(uint32_t begin, uint32_t end, std::vector<Vec3>& cent);

    void BuildTlas();
    uint32_t BuildTlasRange(uint32_t begin, uint32_t end, std::vector<Vec3>& cent);

    void BuildMeshBlas(UniqueMesh& mesh);
    uint32_t BuildMeshRange(UniqueMesh& mesh, uint32_t begin, uint32_t end, std::vector<Vec3>& cent);

    // Direct / flat triangles (used for BSP/Terrain in COL03, or all tris in COL01/02)
    std::vector<Tri> tris_;
    std::vector<uint32_t> order_;
    std::vector<Node> nodes_;

    // Instanced meshes and TLAS
    std::vector<UniqueMesh> meshes_;
    std::vector<MeshInstance> instances_;
    std::vector<uint32_t> tlas_order_;
    std::vector<Node> tlas_nodes_;

    size_t total_triangles_ = 0;
    std::vector<uint32_t> disabled_owners_;
};

} // namespace game
} // namespace aa3

#endif // AA3_GAME_COLLISION_H
