#include "game/stats_export.h"

#include <cstdio>
#include <sstream>

#include "game/world.h"
#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Emits {"field": value, ...} for one generated stat struct.
void RenderStatStruct(std::ostringstream& o, const void* base, const StatField* f, size_t n) {
    const auto* p = static_cast<const uint8_t*>(base);
    o << "{";
    for (size_t i = 0; i < n; ++i) {
        if (i) o << ",";
        o << "\"" << f[i].name << "\":";
        const uint8_t* v = p + f[i].offset;
        switch (f[i].kind) {
            case StatKind::kBool:  o << (*reinterpret_cast<const bool*>(v) ? "true" : "false"); break;
            case StatKind::kFloat: o << *reinterpret_cast<const float*>(v); break;
            case StatKind::kInt:   o << *reinterpret_cast<const int32_t*>(v); break;
            case StatKind::kByte:  o << static_cast<int>(*v); break;
        }
    }
    o << "}";
}

}  // namespace

StatsExporter::StatsExporter(std::string path, std::string server_name)
    : path_(std::move(path)), server_name_(std::move(server_name)) {}

std::string StatsExporter::RenderPlayer(const ServerPlayerController& pc, bool online,
                                        uint64_t now_unix) const {
    std::ostringstream o;
    const auto& ob = pc.observed;
    uint64_t played = now_unix > ob.connected_at_unix ? now_unix - ob.connected_at_unix : 0;
    o << "{\"name\":\"" << JsonEscape(pc.pri ? pc.pri->player_name : "") << "\""
      << ",\"client_id\":" << pc.owning_client_id
      << ",\"online\":" << (online ? "true" : "false")
      << ",\"connected_at\":" << ob.connected_at_unix
      << ",\"seconds_played\":" << played
      << ",\"team\":" << pc.requested_team
      << ",\"class_role\":" << pc.class_role_index
      << ",\"ready\":" << (pc.ready ? "true" : "false")
      << ",\"observed\":{\"distance_uu\":" << static_cast<int64_t>(ob.distance_uu)
      << ",\"moves\":" << ob.moves
      << ",\"sprints\":" << ob.sprints
      << ",\"posture_changes\":" << ob.posture_changes
      << ",\"stance_toggles\":" << ob.stance_toggles << "}";
    if (pc.stat_manager) {
        o << ",\"match\":";
        RenderStatStruct(o, &pc.stat_manager->match, PlayerMatchStatsFields(),
                         PlayerMatchStats::kNumFields);
        o << ",\"round\":";
        RenderStatStruct(o, &pc.stat_manager->round, PlayerRoundStatsFields(),
                         PlayerRoundStats::kNumFields);
    }
    o << "}";
    return o.str();
}

void StatsExporter::OnPlayerLeft(const ServerPlayerController& pc, uint64_t now_unix) {
    departed_.push_back(RenderPlayer(pc, /*online=*/false, now_unix));
    while (departed_.size() > kMaxDeparted) departed_.pop_front();
}

bool StatsExporter::Write(World& world, uint64_t now_unix) {
    std::ostringstream o;
    o << "{\"server\":\"" << JsonEscape(server_name_) << "\""
      << ",\"map\":\"" << JsonEscape(world.GetMapName()) << "\""
      << ",\"generated_at\":" << now_unix << ",\"players\":[";
    bool first = true;
    for (const auto& pc : world.GetPlayerControllers()) {
        if (!first) o << ",";
        first = false;
        o << RenderPlayer(*pc, /*online=*/true, now_unix);
    }
    for (const auto& d : departed_) {
        if (!first) o << ",";
        first = false;
        o << d;
    }
    o << "]}\n";

    const std::string tmp = path_ + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        LOG_WARN("StatsExporter: cannot write %s", tmp.c_str());
        return false;
    }
    const std::string s = o.str();
    bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || std::rename(tmp.c_str(), path_.c_str()) != 0) {
        LOG_WARN("StatsExporter: failed to publish %s", path_.c_str());
        return false;
    }
    return true;
}

}  // namespace game
}  // namespace aa3
