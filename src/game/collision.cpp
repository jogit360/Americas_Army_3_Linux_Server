#include "game/collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Axis(const Vec3& v, int k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }

constexpr uint32_t kLeafSize = 8;

}  // namespace

bool CollisionWorld::Load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LOG_WARN("No collision file %s: traces disabled", path.c_str());
        return false;
    }
    char magic[8];
    uint32_t n = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&n), 4);
    const bool v2 = f && std::memcmp(magic, "AA3COL02", 8) == 0;
    if (!f || (!v2 && std::memcmp(magic, "AA3COL01", 8) != 0)) {
        LOG_ERROR("Bad collision file %s", path.c_str());
        return false;
    }
    const size_t rec_size = v2 ? 41 : 37;
    tris_.resize(n);
    std::vector<char> rec(rec_size);
    for (uint32_t i = 0; i < n; ++i) {
        f.read(rec.data(), static_cast<std::streamsize>(rec_size));
        float v[9];
        std::memcpy(v, rec.data(), 36);
        Tri& t = tris_[i];
        t.v0 = {v[0], v[1], v[2]};
        t.e1 = Sub({v[3], v[4], v[5]}, t.v0);
        t.e2 = Sub({v[6], v[7], v[8]}, t.v0);
        t.flags = static_cast<uint8_t>(rec[36]);
        t.owner = 0;
        if (v2) std::memcpy(&t.owner, rec.data() + 37, 4);
    }
    if (!f) {
        LOG_ERROR("Truncated collision file %s", path.c_str());
        tris_.clear();
        return false;
    }
    Build();
    LOG_INFO("Collision: %u triangles, %zu BVH nodes from %s", n, nodes_.size(), path.c_str());
    return true;
}

void CollisionWorld::Build() {
    order_.resize(tris_.size());
    std::vector<Vec3> cent(tris_.size());
    for (uint32_t i = 0; i < tris_.size(); ++i) {
        order_[i] = i;
        const Tri& t = tris_[i];
        cent[i] = {t.v0.x + (t.e1.x + t.e2.x) / 3, t.v0.y + (t.e1.y + t.e2.y) / 3,
                   t.v0.z + (t.e1.z + t.e2.z) / 3};
    }
    nodes_.clear();
    nodes_.reserve(tris_.size() / kLeafSize * 2 + 1);
    BuildRange(0, static_cast<uint32_t>(tris_.size()), cent);
}

uint32_t CollisionWorld::BuildRange(uint32_t begin, uint32_t end, std::vector<Vec3>& cent) {
    // Bounds of the triangles in [begin, end).
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    Vec3 clo = lo, chi = hi;
    for (uint32_t i = begin; i < end; ++i) {
        const Tri& t = tris_[order_[i]];
        const Vec3 p[3] = {t.v0, {t.v0.x + t.e1.x, t.v0.y + t.e1.y, t.v0.z + t.e1.z},
                           {t.v0.x + t.e2.x, t.v0.y + t.e2.y, t.v0.z + t.e2.z}};
        for (const Vec3& q : p) {
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        const Vec3& c = cent[order_[i]];
        clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
        chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
    }
    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({lo, hi, begin, end - begin});
    if (end - begin <= kLeafSize) return index;

    // Split at the median centroid along the widest axis.
    const Vec3 ext = Sub(chi, clo);
    const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end,
                     [&](uint32_t a, uint32_t b) { return Axis(cent[a], axis) < Axis(cent[b], axis); });
    BuildRange(begin, mid, cent);                 // left child = index + 1
    const uint32_t right = BuildRange(mid, end, cent);
    nodes_[index].first = right;
    nodes_[index].count = 0;
    return index;
}

TraceHit CollisionWorld::LineTrace(const Vec3& start, const Vec3& end, uint8_t mask) const {
    TraceHit best;
    if (nodes_.empty()) return best;
    const Vec3 d = Sub(end, start);
    const Vec3 inv{d.x != 0 ? 1.0f / d.x : 1e30f, d.y != 0 ? 1.0f / d.y : 1e30f,
                   d.z != 0 ? 1.0f / d.z : 1e30f};
    float tmax = 1.0f;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& n = nodes_[stack[--sp]];
        // Slab test against the node box within [0, tmax].
        float t0 = 0.0f, t1 = tmax;
        bool miss = false;
        for (int k = 0; k < 3 && !miss; ++k) {
            const float o = Axis(start, k), iv = Axis(inv, k);
            float a = (Axis(n.lo, k) - o) * iv, b = (Axis(n.hi, k) - o) * iv;
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            miss = t0 > t1;
        }
        if (miss) continue;
        if (n.count > 0) {
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                const Tri& t = tris_[order_[i]];
                if (!(t.flags & mask)) continue;
                if (t.owner && !disabled_owners_.empty() &&
                    std::find(disabled_owners_.begin(), disabled_owners_.end(), t.owner) != disabled_owners_.end())
                    continue;
                const Vec3 pv = Cross(d, t.e2);
                const float det = Dot(t.e1, pv);
                if (std::fabs(det) < 1e-12f) continue;
                const float id = 1.0f / det;
                const Vec3 tv = Sub(start, t.v0);
                const float u = Dot(tv, pv) * id;
                if (u < 0 || u > 1) continue;
                const Vec3 qv = Cross(tv, t.e1);
                const float v = Dot(d, qv) * id;
                if (v < 0 || u + v > 1) continue;
                const float tt = Dot(t.e2, qv) * id;
                if (tt < 0 || tt > tmax) continue;
                tmax = tt;
                best.hit = true;
                best.time = tt;
                best.triangle = order_[i];
            }
        } else {
            const uint32_t self = static_cast<uint32_t>(&n - nodes_.data());
            if (sp + 2 <= 64) {
                stack[sp++] = n.first;     // right
                stack[sp++] = self + 1;    // left
            }
        }
    }
    if (best.hit) {
        const Tri& t = tris_[best.triangle];
        best.location = {start.x + d.x * best.time, start.y + d.y * best.time, start.z + d.z * best.time};
        Vec3 nrm = Cross(t.e1, t.e2);
        const float len = std::sqrt(Dot(nrm, nrm));
        if (len > 0) nrm = {nrm.x / len, nrm.y / len, nrm.z / len};
        if (Dot(nrm, d) > 0) nrm = {-nrm.x, -nrm.y, -nrm.z};
        best.normal = nrm;
        best.owner = t.owner;
    }
    return best;
}

void CollisionWorld::SetOwnerEnabled(uint32_t owner, bool enabled) {
    auto it = std::find(disabled_owners_.begin(), disabled_owners_.end(), owner);
    if (enabled && it != disabled_owners_.end()) disabled_owners_.erase(it);
    if (!enabled && it == disabled_owners_.end()) disabled_owners_.push_back(owner);
}

} // namespace game
} // namespace aa3
