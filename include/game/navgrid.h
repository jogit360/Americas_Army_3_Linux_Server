#ifndef AA3_GAME_NAVGRID_H
#define AA3_GAME_NAVGRID_H

// A walkable grid over a map's collision, for routes on foot (enemy NPCs,
// test bots). Every column of the grid is traced top-down for each floor it
// has (the bridge deck over the water is two), a floor being an upward-facing
// surface with a soldier's headroom above it. Neighbouring floors connect
// when the step between them is small (or a drop, one way) and knee and
// waist traces between them are clear.

#include <cstdint>
#include <vector>
#include "game/collision.h"

namespace aa3 {
namespace game {

class NavGrid {
public:
    struct Cell {
        Vec3 floor;
        bool crouch = false;     // too low to stand
        int32_t door = -1;       // in a door's way: its map NetIndex (open it to pass)
    };
    // `lo`/`hi`: the area to cover (x/y; z is the top/bottom of the search).
    // `doors`: the map NetIndex of each door; their collision is ignored
    // (a door opens) and the cells in their way are marked.
    void Build(CollisionWorld& w, const Vec3& lo, const Vec3& hi, float cell = 64.0f,
               const std::vector<uint32_t>& doors = {});
    bool Empty() const { return cells_.empty(); }
    size_t CellCount() const { return cells_.size(); }
    const std::vector<Cell>& Cells() const { return cells_; }
    const std::vector<std::vector<uint32_t>>& Links() const { return links_; }
    // The cell nearest `p` (a floor within 150 units below/above), or -1.
    int32_t Nearest(const Vec3& p) const;
    // A* from the cell nearest `from` to the one nearest `to`: floor points,
    // empty when there's no route.
    std::vector<Vec3> FindPath(const Vec3& from, const Vec3& to) const;
    // Take the cells within `r` of `p` out of routes (a spot found blocked
    // in play that the collision doesn't show).
    void Block(const Vec3& p, float r);

private:
    float cell_ = 64.0f;
    Vec3 lo_, hi_;
    int32_t nx_ = 0, ny_ = 0;
    std::vector<Cell> cells_;
    std::vector<std::vector<uint32_t>> column_;        // per (ix, iy): its floors' cells
    std::vector<std::vector<uint32_t>> links_;
    std::vector<uint32_t> component_;
    std::vector<char> blocked_;                     // per cell: its connected area's size
};

}  // namespace game
}  // namespace aa3

#endif  // AA3_GAME_NAVGRID_H
