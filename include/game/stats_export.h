#ifndef AA3_GAME_STATS_EXPORT_H
#define AA3_GAME_STATS_EXPORT_H

#include <cstdint>
#include <deque>
#include <string>

#include "game/server_actors.h"

namespace aa3 {
namespace game {

class World;

// Writes per-player stats as JSON for an external web server to serve
// (doc sec 23). One file, rewritten atomically (tmp + rename) so a reader
// never sees a partial document. Contains every online player plus the
// most recent departed ones, each with:
//   - identity/session: name, client id, team, class role, ready, times
//   - "observed": what this server measures itself (time, distance,
//     moves, sprints, posture changes) — not part of any AA3 struct
//   - "match"/"round": the AA3 AA_MP_PlayerMatchStatistics /
//     AA_MP_PlayerStatistics blocks exactly as replicated to the client,
//     every field by its UnrealScript name (generated table).
class StatsExporter {
public:
    StatsExporter(std::string path, std::string server_name);

    // Snapshot a leaving player before the world drops them.
    void OnPlayerLeft(const ServerPlayerController& pc, uint64_t now_unix);
    bool Write(World& world, uint64_t now_unix);

    const std::string& path() const { return path_; }

private:
    std::string RenderPlayer(const ServerPlayerController& pc, bool online,
                             uint64_t now_unix) const;

    std::string path_;
    std::string server_name_;
    std::deque<std::string> departed_;  // pre-rendered, newest last
    static constexpr size_t kMaxDeparted = 200;
};

}  // namespace game
}  // namespace aa3

#endif  // AA3_GAME_STATS_EXPORT_H
