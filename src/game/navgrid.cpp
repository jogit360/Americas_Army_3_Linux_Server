#include "game/navgrid.h"
#include <algorithm>
#include <cmath>
#include <queue>

namespace aa3 {
namespace game {

namespace {
constexpr float kHeadroom = 52.0f;      // crouched soldier's height clear above a floor
constexpr float kStandHeight = 96.0f;   // standing soldier
constexpr float kMaxStep = 36.0f;       // up a step / stair
constexpr float kMaxDrop = 160.0f;      // down a ledge (one way)
constexpr float kMinNormalZ = 0.7f;     // walkable slope (~45 degrees)
constexpr float kRadius = 26.0f;        // a soldier's collision radius (22) plus a little
constexpr uint8_t kPawn = CollisionWorld::kBlockPawn;

float Dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

void NavGrid::Build(CollisionWorld& w, const Vec3& lo, const Vec3& hi, float cell,
                    const std::vector<uint32_t>& doors) {
    struct DoorBox { uint32_t net; Vec3 lo, hi; };
    std::vector<DoorBox> boxes;
    for (uint32_t d : doors) {
        DoorBox b{d, {}, {}};
        if (w.OwnerBounds(d + 1, b.lo, b.hi)) boxes.push_back(b);
        w.SetOwnerEnabled(d + 1, false);
    }
    cell_ = cell; lo_ = lo; hi_ = hi;
    nx_ = static_cast<int32_t>((hi.x - lo.x) / cell) + 1;
    ny_ = static_cast<int32_t>((hi.y - lo.y) / cell) + 1;
    cells_.clear(); links_.clear();
    column_.assign(static_cast<size_t>(nx_) * static_cast<size_t>(ny_), {});
    // Floors, column by column, top to bottom.
    for (int32_t iy = 0; iy < ny_; ++iy)
        for (int32_t ix = 0; ix < nx_; ++ix) {
            const float x = lo.x + ix * cell, y = lo.y + iy * cell;
            float top = hi.z;
            for (int layer = 0; layer < 12 && top > lo.z; ++layer) {
                const TraceHit h = w.LineTrace({x, y, top}, {x, y, lo.z}, kPawn);
                if (!h.hit) break;
                const float fz = h.location.z;
                if (std::fabs(h.normal.z) >= kMinNormalZ &&
                    !w.LineTrace({x, y, fz + 2.0f}, {x, y, fz + kHeadroom}, kPawn).hit) {
                    // Room for a body: nothing within the soldier's radius
                    // around the cell at knee and chest height.
                    bool tight = false;
                    for (int k = 0; k < 8 && !tight; ++k) {
                        const float ang = k * 0.785398f;
                        const float ex = x + std::cos(ang) * kRadius, ey = y + std::sin(ang) * kRadius;
                        for (float h : {24.0f, 48.0f})
                            if (w.LineTrace({x, y, fz + h}, {ex, ey, fz + h}, kPawn).hit) { tight = true; break; }
                    }
                    if (tight) { top = fz - 8.0f; continue; }
                    const bool low = w.LineTrace({x, y, fz + 2.0f}, {x, y, fz + kStandHeight}, kPawn).hit;
                    int32_t door = -1;
                    for (const auto& b : boxes)
                        if (x > b.lo.x - 90 && x < b.hi.x + 90 && y > b.lo.y - 90 && y < b.hi.y + 90 &&
                            fz > b.lo.z - 60 && fz < b.hi.z) door = static_cast<int32_t>(b.net);
                    cells_.push_back({{x, y, fz}, low, door});
                    column_[static_cast<size_t>(iy) * nx_ + ix].push_back(static_cast<uint32_t>(cells_.size() - 1));
                }
                top = fz - 8.0f;
            }
        }
    links_.assign(cells_.size(), {});
    auto clear = [&](const Vec3& a, const Vec3& b) {
        for (float h : {22.0f, 44.0f})
            if (w.LineTrace({a.x, a.y, a.z + h}, {b.x, b.y, b.z + h}, kPawn).hit) return false;
        return true;
    };
    static const int32_t kDir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    for (int32_t iy = 0; iy < ny_; ++iy)
        for (int32_t ix = 0; ix < nx_; ++ix)
            for (uint32_t a : column_[static_cast<size_t>(iy) * nx_ + ix])
                for (const auto& d : kDir) {
                    const int32_t jx = ix + d[0], jy = iy + d[1];
                    if (jx < 0 || jy < 0 || jx >= nx_ || jy >= ny_) continue;
                    for (uint32_t b : column_[static_cast<size_t>(jy) * nx_ + jx]) {
                        const float dz = cells_[b].floor.z - cells_[a].floor.z;
                        // A step, or a walkable slope (up to ~40 degrees) over the cell.
                        const float run = cell_ * ((d[0] && d[1]) ? 1.414f : 1.0f);
                        if (dz > std::max(kMaxStep, run * 0.84f) || dz < -kMaxDrop) continue;
                        const Vec3& fa = cells_[a].floor;
                        const Vec3& fb = cells_[b].floor;
                        // Up a step: trace from the higher floor's height.
                        const Vec3 sa{fa.x, fa.y, std::max(fa.z, fb.z)};
                        const Vec3 sb{fb.x, fb.y, std::max(fa.z, fb.z)};
                        if (clear(sa, sb)) links_[a].push_back(b);
                    }
                }
    for (uint32_t d : doors) w.SetOwnerEnabled(d + 1, true);
    // Connected areas (links both ways), so a query snaps to the big one
    // rather than a pocket under a car.
    std::vector<std::vector<uint32_t>> undirected(cells_.size());
    for (uint32_t a = 0; a < cells_.size(); ++a)
        for (uint32_t b : links_[a]) { undirected[a].push_back(b); undirected[b].push_back(a); }
    component_.assign(cells_.size(), 0);
    std::vector<uint32_t> stack, members;
    for (uint32_t s = 0; s < cells_.size(); ++s) {
        if (component_[s]) continue;
        members.clear(); stack.assign(1, s); component_[s] = 1;
        while (!stack.empty()) {
            const uint32_t u = stack.back(); stack.pop_back(); members.push_back(u);
            for (uint32_t v : undirected[u]) if (!component_[v]) { component_[v] = 1; stack.push_back(v); }
        }
        for (uint32_t m : members) component_[m] = static_cast<uint32_t>(members.size());
    }
}

void NavGrid::Block(const Vec3& p, float r) {
    blocked_.resize(cells_.size(), 0);
    for (size_t i = 0; i < cells_.size(); ++i) {
        const Vec3& f = cells_[i].floor;
        if (std::fabs(f.z - p.z) < 80.0f && std::hypot(f.x - p.x, f.y - p.y) <= r) blocked_[i] = 1;
    }
}

int32_t NavGrid::Nearest(const Vec3& p) const {
    if (cells_.empty()) return -1;
    const int32_t ix = static_cast<int32_t>(std::lround((p.x - lo_.x) / cell_));
    const int32_t iy = static_cast<int32_t>(std::lround((p.y - lo_.y) / cell_));
    int32_t best = -1;
    float best_d = 1e9f;
    for (int32_t r = 0; r <= 6 && best < 0; ++r)
        for (int32_t dy = -r; dy <= r; ++dy)
            for (int32_t dx = -r; dx <= r; ++dx) {
                const int32_t jx = ix + dx, jy = iy + dy;
                if (jx < 0 || jy < 0 || jx >= nx_ || jy >= ny_) continue;
                for (uint32_t c : column_[static_cast<size_t>(jy) * nx_ + jx]) {
                    const Vec3& f = cells_[c].floor;
                    if (p.z - f.z > 150.0f || f.z - p.z > 150.0f) continue;
                    const float d = Dist(f, p) + (links_[c].empty() ? 1000.0f : 0.0f) +
                                    (component_[c] < 2000 ? 400.0f : 0.0f);
                    if (d < best_d) { best_d = d; best = static_cast<int32_t>(c); }
                }
            }
    return best;
}

std::vector<Vec3> NavGrid::FindPath(const Vec3& from, const Vec3& to) const {
    const int32_t s = Nearest(from), t = Nearest(to);
    if (s < 0 || t < 0) return {};
    std::vector<float> g(cells_.size(), 1e30f);
    std::vector<int32_t> prev(cells_.size(), -1);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    g[static_cast<size_t>(s)] = 0.0f;
    q.push({Dist(cells_[static_cast<size_t>(s)].floor, cells_[static_cast<size_t>(t)].floor), static_cast<uint32_t>(s)});
    while (!q.empty()) {
        const auto [f, u] = q.top();
        q.pop();
        if (u == static_cast<uint32_t>(t)) break;
        if (f - Dist(cells_[u].floor, cells_[static_cast<size_t>(t)].floor) > g[u] + 1e-3f) continue;
        for (uint32_t v : links_[u]) {
            if (!blocked_.empty() && blocked_[v] && v != static_cast<uint32_t>(t)) continue;
            const float nd = g[u] + Dist(cells_[u].floor, cells_[v].floor);
            if (nd < g[v]) {
                g[v] = nd; prev[v] = static_cast<int32_t>(u);
                q.push({nd + Dist(cells_[v].floor, cells_[static_cast<size_t>(t)].floor), v});
            }
        }
    }
    if (prev[static_cast<size_t>(t)] < 0 && s != t) return {};
    std::vector<Vec3> path;
    for (int32_t c = t; c >= 0; c = prev[static_cast<size_t>(c)]) path.push_back(cells_[static_cast<size_t>(c)].floor);
    std::reverse(path.begin(), path.end());
    return path;
}

}  // namespace game
}  // namespace aa3
