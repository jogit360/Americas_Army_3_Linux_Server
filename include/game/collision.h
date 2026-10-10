#ifndef AA3_GAME_COLLISION_H
#define AA3_GAME_COLLISION_H

// Server-side collision world (doc sec 32): the map's static geometry --
// static mesh kDOP triangles, terrain heightmaps and BSP -- exported by
// aa3-tools/collision/export_collision.py to data/collision/<map>.col, held
// in a bounding-volume hierarchy for line traces (bullets, floor checks,
// vault/ladder probes).

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
    uint32_t triangle = 0;
    uint32_t owner = 0;         // map NetIndex + 1 of a dynamic actor (window/door), 0 = static
};

class CollisionWorld {
public:
    // Triangle flags (export_collision.py).
    static constexpr uint8_t kBlockPawn = 1;
    static constexpr uint8_t kBlockTrace = 2;

    bool Load(const std::string& path);
    bool Loaded() const { return !tris_.empty(); }
    size_t TriangleCount() const { return tris_.size(); }

    // Closest hit on the segment start->end against triangles whose flags
    // include any bit of `mask` (both faces count).
    TraceHit LineTrace(const Vec3& start, const Vec3& end, uint8_t mask = kBlockTrace) const;

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
    void Build();
    uint32_t BuildRange(uint32_t begin, uint32_t end, std::vector<Vec3>& cent);

    std::vector<Tri> tris_;
    std::vector<uint32_t> order_;
    std::vector<Node> nodes_;
    std::vector<uint32_t> disabled_owners_;
};

} // namespace game
} // namespace aa3

#endif // AA3_GAME_COLLISION_H
