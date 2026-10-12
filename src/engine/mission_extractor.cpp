#include "engine/mission_extractor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <set>
#include <sstream>

#include "engine/package_props.h"
#include "network/rep_layout_gen.h"
#include "util/json.h"

namespace aa3 {
namespace engine {

namespace {

namespace CR = network::rep::ClassRoleRI;
namespace FT = network::rep::FireTeamInfo;

// Class default objects in AA3Baseline (its NetIndex; the same on every map).
// Retail spawns these per round (GamePlayPCAP_Legit_Server, doc sec 28/36).
constexpr uint32_t kCdoES2Manager = 8697;
constexpr uint32_t kCdoFireTeamInfo = 9764;
constexpr uint32_t kCdoExtractionMarker = 16373;
constexpr uint32_t kCdoWindowManager = 3688;
// MaxIndex (config/class_maxindex_table.tsv) of classes without a generated layout.
constexpr uint32_t kMaxExtractionMarker = 36;
constexpr uint32_t kMaxWindowManager = 25;

const char* const kFireTeamNames[] = {"Alpha", "Bravo", "Charlie", "Delta",
                                      "Echo", "Foxtrot", "Golf", "Hotel", "Squad Leader"};

std::string Fmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

std::string ObjRef(const std::string& pkg, int32_t net) {
    return "{\"kind\":\"obj\",\"pkg\":" + util::JsonQuote(pkg) + ",\"net\":" + std::to_string(net) + "}";
}

// Read-only view of a level's Kismet graph.
class Graph {
public:
    explicit Graph(const Package& pkg) : pkg_(pkg) {}

    std::string Class(size_t i) const { return ExportClassName(pkg_, i); }

    const PropList& Props(size_t i) {
        auto it = cache_.find(i);
        if (it == cache_.end()) {
            auto d = ReadExportData(pkg_, i);
            it = cache_.emplace(i, d ? std::move(*d) : ExportData{}).first;
        }
        return it->second.props;
    }
    int32_t NetIndex(size_t i) {
        Props(i);
        return cache_[i].net_index;
    }
    size_t PropsEnd(size_t i) {
        Props(i);
        return cache_[i].props_end;
    }

    int32_t Int(size_t i, const char* name, int32_t def = 0) {
        const PropValue* p = FindProp(Props(i), name);
        return p && (p->kind == PropValue::kInt || p->kind == PropValue::kByte) ? p->i : def;
    }
    std::string Str(size_t i, const char* name) {
        const PropValue* p = FindProp(Props(i), name);
        return p ? p->s : "";
    }
    // Export referenced by object property `name` (static element `index`).
    int32_t Ref(size_t i, const char* name, int32_t index = 0) {
        const PropValue* p = FindProp(Props(i), name, index);
        return p && p->kind == PropValue::kObject ? RefToExport(p->i) : -1;
    }

    // Variables linked to `op`'s variable link labelled `desc`.
    std::vector<size_t> Vars(size_t op, const std::string& desc) {
        std::vector<size_t> out;
        const PropValue* links = FindProp(Props(op), "VariableLinks");
        if (!links) return out;
        for (const PropList& l : links->elements) {
            const PropValue* d = FindProp(l, "LinkDesc");
            if (!d || d->s != desc) continue;
            if (const PropValue* v = FindProp(l, "LinkedVariables"))
                for (int32_t r : v->ints)
                    if (RefToExport(r) >= 0) out.push_back(static_cast<size_t>(RefToExport(r)));
        }
        return out;
    }
    // Every (op, input link) `op`'s outputs lead to.
    std::vector<std::pair<size_t, int32_t>> Outputs(size_t op) {
        std::vector<std::pair<size_t, int32_t>> out;
        const PropValue* links = FindProp(Props(op), "OutputLinks");
        if (!links) return out;
        for (const PropList& ol : links->elements) {
            const PropValue* ls = FindProp(ol, "Links");
            if (!ls) continue;
            for (const PropList& l : ls->elements) {
                const PropValue* to = FindProp(l, "LinkedOp");
                const PropValue* in = FindProp(l, "InputLinkIdx");
                if (to && RefToExport(to->i) >= 0)
                    out.push_back({static_cast<size_t>(RefToExport(to->i)), in ? in->i : 0});
            }
        }
        return out;
    }

    std::vector<size_t> OfClass(const std::string& cls) const {
        std::vector<size_t> out;
        for (size_t i = 0; i < pkg_.exports.size(); ++i)
            if (Class(i) == cls) out.push_back(i);
        return out;
    }

private:
    const Package& pkg_;
    std::map<size_t, ExportData> cache_;
};

struct FireTeam {
    int32_t team = 0;
    int32_t slot = 0;                   // = iFireTeamID
    std::vector<size_t> roles;          // role objects by position
    int32_t marker = -1;                // AA_MCFireteamSpawnMarker export
};

// ---- role templates + loadout overrides ----------------------------------

util::JsonValue* FieldByIdx(util::JsonValue& fields, uint32_t idx) {
    for (auto& f : fields.arr)
        if (f["idx"].Int(-1) == idx && !f.Has("elem")) return &f;
    return nullptr;
}

util::JsonValue Num(double v) {
    util::JsonValue j;
    j.type = util::JsonValue::Type::kNumber;
    j.num = v;
    return j;
}
util::JsonValue Text(const std::string& s) {
    util::JsonValue j;
    j.type = util::JsonValue::Type::kString;
    j.str = s;
    return j;
}

void SetIntField(util::JsonValue& fields, uint32_t idx, const std::string& name, int32_t v) {
    if (util::JsonValue* f = FieldByIdx(fields, idx)) {
        f->obj["v"] = Num(v);
        return;
    }
    util::JsonValue f;
    f.type = util::JsonValue::Type::kObject;
    f.obj["idx"] = Num(idx);
    f.obj["name"] = Text("AA_MCClassRoleReplicationInfo." + name);
    f.obj["kind"] = Text("int");
    f.obj["v"] = Num(v);
    fields.arr.push_back(f);
}

// Struct members (RoleLoadoutTypes) by field name, recursing into nested
// structs (mcpmaPrimaryOneModAmmo).
bool SetMember(util::JsonValue& members, const PropValue& src, std::vector<std::string>& warn,
               const std::string& where) {
    for (auto& m : members.arr) {
        if (m["field"].Str() != src.name) continue;
        if (src.kind == PropValue::kStruct) {
            bool ok = true;
            for (const auto& sub : src.members) ok = SetMember(m.obj["members"], sub, warn, where) && ok;
            return ok;
        }
        if (src.kind == PropValue::kBool) {
            m.obj["v"].type = util::JsonValue::Type::kBool;
            m.obj["v"].b = src.b;
        } else {
            m.obj["v"] = Num(src.i);
        }
        return true;
    }
    warn.push_back(where + ": no RoleLoadoutTypes member " + src.name);
    return false;
}

// MCStandardLoadout / MaxRoleLoadouts overrides on a role object -> the
// replication info fields they feed (AA_MCTeamBuilderSeqAct copies them).
void ApplyOverrides(Graph& g, size_t role, util::JsonValue& fields, std::vector<std::string>& warn,
                    const std::string& where) {
    static const std::set<std::string> kEditorOnly = {
        "ObjInstanceVersion", "ParentSequence", "ObjPosX", "ObjPosY", "DrawWidth", "DrawHeight",
        "ObjComment", "bOutputObjCommentToScreen", "bSuppressAutoComment",
        // Pouch meshes: the pawn's look, not replicated with the role.
        "plStandardLoadout", "plLightLoadout", "plHeavyLoadout"};
    static const std::map<std::string, std::pair<uint32_t, const char*>> kGrenades = {
        {"iFragGrenades", {CR::iFragGrenades, "iFragGrenades"}},
        {"iSmokeGrenades", {CR::iSmokeGrenades, "iSmokeGrenades"}},
        {"iM106SmokeGrenades", {CR::iM106SmokeGrenades, "iM106SmokeGrenades"}},
        {"iStunGrenades", {CR::iStunGrenades, "iStunGrenades"}},
        {"iIncendiaryGrenades", {CR::iIncendiaryGrenades, "iIncendiaryGrenades"}}};
    for (const PropValue& p : g.Props(role)) {
        if (kEditorOnly.count(p.name)) continue;
        if (p.name == "mcslStandardLoadout") {
            for (const PropValue& m : p.members) {
                if (m.name == "mcgiGrenades") {
                    for (const PropValue& gr : m.members) {
                        auto it = kGrenades.find(gr.name);
                        if (it != kGrenades.end()) SetIntField(fields, it->second.first, it->second.second, gr.i);
                        else warn.push_back(where + ": grenade " + gr.name + " not mapped");
                    }
                } else if (m.name == "iPrimaryWeaponOneAmmo") {
                    SetIntField(fields, CR::iPrimaryWeaponOneAmmo, "iPrimaryWeaponOneAmmo", m.i);
                } else if (m.name == "mcpmaPrimary1ModAmmo") {
                    for (const PropValue& a : m.members) {
                        if (a.name == "iM203FragGrenades")
                            SetIntField(fields, CR::iPrimaryOneModM203FragGrenades, "iPrimaryOneModM203FragGrenades", a.i);
                        else if (a.name == "iM203SmokeGrenades")
                            SetIntField(fields, CR::iPrimaryOneModM203SmokeGrenades, "iPrimaryOneModM203SmokeGrenades", a.i);
                        else warn.push_back(where + ": mod ammo " + a.name + " not mapped");
                    }
                } else {
                    warn.push_back(where + ": mcslStandardLoadout." + m.name + " override not supported");
                }
            }
        } else if (p.name == "mrlHeavyLoadout" || p.name == "mrlLightLoadout") {
            util::JsonValue* f = FieldByIdx(fields, p.name == "mrlHeavyLoadout" ? CR::rltHeavyLoadout
                                                                              : CR::rltLightLoadout);
            if (!f) { warn.push_back(where + ": template has no " + p.name); continue; }
            for (const PropValue& m : p.members) SetMember(f->obj["members"], m, warn, where);
        } else {
            warn.push_back(where + ": role property " + p.name + " ignored");
        }
    }
}

void SortFields(util::JsonValue& fields) {
    std::stable_sort(fields.arr.begin(), fields.arr.end(),
                     [](const util::JsonValue& a, const util::JsonValue& b) {
                         return a["idx"].Int() < b["idx"].Int();
                     });
}

}  // namespace

std::map<std::string, uint32_t> LoadClassMaxIndexTable(const std::string& path) {
    std::map<std::string, uint32_t> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string pkg, cls;
        uint32_t max = 0;
        if (std::getline(ss, pkg, '\t') && std::getline(ss, cls, '\t') && (ss >> max)) out[cls] = max;
    }
    return out;
}

MissionExtractor::MissionExtractor(const Package& level, const std::string& level_name,
                                   const Package& map, const std::string& map_name)
    : level_(level), level_name_(level_name), map_(map), map_name_(map_name) {}

bool MissionExtractor::LoadRoleTemplates(const std::string& json, std::string* err) {
    auto doc = util::ParseJson(json, err);
    if (!doc || (*doc)["roles"].IsNull()) {
        if (err && err->empty()) *err = "no \"roles\"";
        return false;
    }
    role_templates_json_ = json;
    return true;
}

bool MissionExtractor::Build(MissionData& out) {
    Graph g(level_);
    auto& warn = out.warnings;

    // Teams: AA_MCTeamInfo objects, reached from each team builder.
    std::map<size_t, int32_t> builder_team;
    std::map<int32_t, size_t> team_info;
    for (size_t tb : g.OfClass("AA_MCTeamBuilderSeqAct")) {
        auto ti = g.Vars(tb, "Team Info");
        if (ti.empty()) { warn.push_back("team builder without Team Info"); continue; }
        int32_t team = g.Int(ti[0], "iTeamID");
        if (team < 0 || team > 1) continue;
        builder_team[tb] = team;
        team_info[team] = ti[0];
    }
    if (builder_team.empty()) return false;

    // Fire teams: every fire team builder output into a team builder.
    std::vector<FireTeam> fts;
    for (size_t fb : g.OfClass("AA_MCFireteamInfoBuilderSeqAct")) {
        std::vector<size_t> roles;
        for (int pos = 1; pos <= 5; ++pos)
            for (size_t r : g.Vars(fb, "Position " + std::to_string(pos)))
                if (g.Class(r).rfind("AA_MCClassRole_", 0) == 0) roles.push_back(r);
        for (auto [to, slot] : g.Outputs(fb)) {
            auto bt = builder_team.find(to);
            if (bt == builder_team.end() || roles.empty()) continue;
            bool dup = false;
            for (const auto& f : fts) dup = dup || (f.team == bt->second && f.slot == slot);
            if (dup) { warn.push_back("two fire teams in one slot"); continue; }
            fts.push_back({bt->second, slot, roles, -1});
        }
    }
    std::sort(fts.begin(), fts.end(), [](const FireTeam& a, const FireTeam& b) {
        return a.team != b.team ? a.team < b.team : a.slot < b.slot;
    });

    // Spawn markers: team builder -> ... -> spawn selector, by fire team name.
    for (auto [tb, team] : builder_team) {
        int32_t selector = -1;
        std::deque<std::pair<size_t, int>> q{{tb, 0}};
        std::set<size_t> seen{tb};
        while (!q.empty() && selector < 0) {
            auto [op, depth] = q.front();
            q.pop_front();
            if (g.Class(op) == "AA_MCSpawnSelectorSeqAct") { selector = static_cast<int32_t>(op); break; }
            if (depth >= 4) continue;
            for (auto [to, in] : g.Outputs(op))
                if (seen.insert(to).second) q.push_back({to, depth + 1});
        }
        if (selector < 0) { warn.push_back("team " + std::to_string(team) + ": no spawn selector"); continue; }
        auto markers_for = [&](const std::string& desc) {
            std::vector<size_t> ms;
            for (size_t v : g.Vars(static_cast<size_t>(selector), desc)) {
                int32_t m = g.Ref(v, "ObjValue");
                if (m >= 0 && g.Class(static_cast<size_t>(m)) == "AA_MCFireteamSpawnMarker")
                    ms.push_back(static_cast<size_t>(m));
            }
            return ms;
        };
        for (auto& f : fts) {
            if (f.team != team) continue;
            std::vector<size_t> ms = markers_for(kFireTeamNames[std::min(f.slot, 8)]);
            if (ms.empty()) ms = markers_for("Extra Locations");
            if (ms.empty()) ms = markers_for("All");
            // Retail picks among several at random; the first is as good.
            if (!ms.empty()) f.marker = static_cast<int32_t>(ms[0]);
            else warn.push_back("fire team " + std::to_string(team) + "/" + std::to_string(f.slot) + " has no spawn marker");
        }
    }

    // ---- round actors -------------------------------------------------------
    std::string err;
    auto tdoc = util::ParseJson(role_templates_json_, &err);
    if (!tdoc) { warn.push_back("role templates: " + err); return false; }
    const util::JsonValue& templates = (*tdoc)["roles"];
    const std::string role_cdo = util::WriteJson((*tdoc)["cdo"]);
    const int64_t role_max = (*tdoc)["max_index"].Int(CR::kMaxIndex);

    std::ostringstream rs;
    rs << "{\"source\":" << util::JsonQuote(level_name_ + " (aa3_mapbake)") << ",\"actors\":[\n";
    rs << "{\"retail_ch\":1,\"class\":\"AA_ES2Manager\",\"cdo\":" << ObjRef("AA3Baseline", kCdoES2Manager)
       << ",\"max_index\":" << network::rep::ES2Manager::kMaxIndex << ",\"fields\":[]}";
    int64_t next_id = 1000;
    int roles_total = 0;
    for (const auto& f : fts) {
        int64_t ft_id = next_id++;
        std::vector<int64_t> role_ids;
        std::ostringstream roles_out;
        for (size_t pos = 0; pos < f.roles.size(); ++pos) {
            size_t r = f.roles[pos];
            std::string cls = g.Class(r);
            const util::JsonValue* t = &templates[cls];
            if (t->IsNull()) {
                warn.push_back(level_name_ + ": no role template for " + cls + ", using Rifleman");
                t = &templates["AA_MCClassRole_Rifleman"];
            }
            util::JsonValue fields = *t;
            std::string where = cls + " (team " + std::to_string(f.team) + ")";
            ApplyOverrides(g, r, fields, warn, where);
            if (f.slot != 0) SetIntField(fields, CR::iFireTeamID, "iFireTeamID", f.slot);
            if (f.team != 0) SetIntField(fields, CR::iTeamID, "iTeamID", f.team);
            char id[32];
            if (cls == "AA_MCClassRole_SquadLeader") std::snprintf(id, sizeof(id), "SL");
            else std::snprintf(id, sizeof(id), "T%dF%dR%zu", f.team, f.slot, pos);
            if (util::JsonValue* idf = FieldByIdx(fields, CR::sClassRoleID)) {
                idf->obj["v"] = Text(id);
            } else {
                util::JsonValue nf;
                nf.type = util::JsonValue::Type::kObject;
                nf.obj["idx"] = Num(CR::sClassRoleID);
                nf.obj["name"] = Text("AA_MCClassRoleReplicationInfo.sClassRoleID");
                nf.obj["kind"] = Text("str");
                nf.obj["v"] = Text(id);
                fields.arr.push_back(nf);
            }
            SortFields(fields);
            int64_t rid = next_id++;
            role_ids.push_back(rid);
            roles_out << ",\n{\"retail_ch\":" << rid << ",\"class\":\"AA_MCClassRoleReplicationInfo\",\"cdo\":"
                      << role_cdo << ",\"max_index\":" << role_max << ",\"role_class\":" << util::JsonQuote(cls)
                      << ",\"fields\":" << util::WriteJson(fields) << "}";
            ++roles_total;
        }
        rs << ",\n{\"retail_ch\":" << ft_id << ",\"class\":\"AA_FireTeamInfo\",\"cdo\":"
           << ObjRef("AA3Baseline", kCdoFireTeamInfo) << ",\"max_index\":" << FT::kMaxIndex << ",\"fields\":[";
        bool first = true;
        auto field = [&](const std::string& body) {
            rs << (first ? "" : ",") << body;
            first = false;
        };
        if (f.slot != 0)
            field("{\"idx\":" + std::to_string(FT::iFireTeamID) +
                  ",\"name\":\"AA_FireTeamInfo.iFireTeamID\",\"kind\":\"int\",\"v\":" + std::to_string(f.slot) + "}");
        for (size_t k = 0; k < role_ids.size(); ++k)
            field("{\"idx\":" + std::to_string(FT::aamcrriClassRoles) +
                  ",\"name\":\"AA_FireTeamInfo.aamcrriClassRoles\",\"kind\":\"actor\",\"retail_ch\":" +
                  std::to_string(role_ids[k]) + ",\"elem\":" + std::to_string(k) + "}");
        if (f.marker >= 0) {
            int32_t net = g.NetIndex(static_cast<size_t>(f.marker));
            for (uint32_t idx : {FT::aamcfsmDefaultSpawnMarker, FT::aamcfsmActiveSpawnMarker})
                field("{\"idx\":" + std::to_string(idx) + ",\"kind\":\"obj\",\"pkg\":" + util::JsonQuote(level_name_) +
                      ",\"net\":" + std::to_string(net) + "}");
        }
        field("{\"idx\":" + std::to_string(FT::nameFireTeamName) +
              ",\"name\":\"AA_FireTeamInfo.nameFireTeamName\",\"kind\":\"name\",\"v\":" +
              util::JsonQuote(kFireTeamNames[std::min(f.slot, 8)]) + ",\"num\":0}");
        rs << "]}" << roles_out.str();
    }
    rs << "\n]}\n";
    out.round_json = rs.str();

    // ---- objectives ----------------------------------------------------------
    const bool carryable = !g.OfClass("AA_MCCarryableObjectiveMarker").empty();
    std::ostringstream js, ex;
    js << "{\"source\":" << util::JsonQuote(level_name_ + " (aa3_mapbake)") << ",\"actors\":[\n";
    int64_t join_id = 1;
    bool first_ex = true;
    for (size_t m : g.OfClass("AA_MCExtractionObjectiveMarker")) {
        const PropValue* loc = FindProp(g.Props(m), "Location");
        float l[3] = {0, 0, 0};
        if (loc && loc->kind == PropValue::kVector) std::copy(loc->vec, loc->vec + 3, l);
        int32_t team = g.Int(m, "iExtractingTeam");
        js << "{\"retail_ch\":" << join_id++ << ",\"class\":\"AA_MCExtractionObjectiveMarker\",\"cdo\":"
           << ObjRef("AA3Baseline", kCdoExtractionMarker) << ",\"max_index\":"
           << kMaxExtractionMarker << ",\"location\":[" << Fmt(l[0]) << ","
           << Fmt(l[1]) << "," << Fmt(l[2]) << "],\"fields\":[";
        std::vector<size_t> volumes;
        for (int32_t k = 0; k < 16; ++k) {
            int32_t v = g.Ref(m, "aLinkedObjectiveItems", k);
            if (v < 0) continue;
            js << "{\"idx\":25,\"name\":\"AA_ObjectiveMarker.aLinkedObjectiveItems\",\"kind\":\"obj\",\"pkg\":"
               << util::JsonQuote(level_name_) << ",\"net\":" << g.NetIndex(static_cast<size_t>(v))
               << ",\"elem\":" << k << "},";
            if (g.Class(static_cast<size_t>(v)) == "AA_ExtractionVolume") volumes.push_back(static_cast<size_t>(v));
        }
        const PropValue* en = FindProp(g.Props(m), "bObjectiveEnabled");
        if (en && en->b) js << "{\"idx\":28,\"name\":\"AA_ObjectiveMarker.bObjectiveEnabled\",\"kind\":\"bool\",\"v\":true},";
        js << "{\"idx\":35,\"name\":\"AA_MCExtractionObjectiveMarker.iExtractingTeam\",\"kind\":\"int\",\"v\":"
           << team << "}]},\n";

        // Win rule: only where entering the zone is the whole objective (not
        // carry-the-item modes, which the server doesn't run yet).
        if (carryable) continue;
        if (volumes.empty()) volumes = g.OfClass("AA_ExtractionVolume");
        std::string sym = g.Str(m, "PreplanSymbol");
        ex << (first_ex ? "" : ",") << "{\"extracting_team\":" << team
           << ",\"min_players\":" << g.Int(m, "iMinimumPlayerCount")
           << ",\"min_percentage\":" << g.Int(m, "iMinPercentageToExtract")
           << ",\"objective\":" << util::JsonQuote(sym.empty() ? "EPOC_Zulu" : sym) << ",\"boxes\":[";
        first_ex = false;
        bool first_box = true;
        for (size_t v : volumes) {
            int32_t brush = g.Ref(v, "Brush");
            const PropValue* vl = FindProp(g.Props(v), "Location");
            if (brush < 0) continue;
            auto model = ReadExportData(level_, static_cast<size_t>(brush));
            const ObjectExport& me = level_.exports[static_cast<size_t>(brush)];
            size_t off = static_cast<size_t>(me.serial_offset - level_.body_base) + (model ? model->props_end : 0);
            if (!model || off + 24 > level_.body.size()) continue;
            float bounds[6];   // FBoxSphereBounds: Origin, BoxExtent (then SphereRadius)
            std::memcpy(bounds, level_.body.data() + off, sizeof(bounds));
            float lo[3], hi[3];
            for (int k = 0; k < 3; ++k) {
                float base = (vl && vl->kind == PropValue::kVector ? vl->vec[k] : 0.0f) + bounds[k];
                lo[k] = base - bounds[3 + k];
                hi[k] = base + bounds[3 + k];
            }
            ex << (first_box ? "" : ",") << "{\"min\":[" << Fmt(lo[0]) << "," << Fmt(lo[1]) << "," << Fmt(lo[2])
               << "],\"max\":[" << Fmt(hi[0]) << "," << Fmt(hi[1]) << "," << Fmt(hi[2]) << "]}";
            first_box = false;
        }
        ex << "]}";
    }
    js << "{\"retail_ch\":" << join_id++ << ",\"class\":\"AA_BreakableWindowManager\",\"cdo\":"
       << ObjRef("AA3Baseline", kCdoWindowManager) << ",\"max_index\":"
       << kMaxWindowManager << ",\"location\":[0,0,0],\"fields\":[]}\n]}\n";
    out.join_json = js.str();
    // ES2 pickups are spawned at random area markers by retail's factories;
    // none for now.
    out.match_json = "{\"source\":" + util::JsonQuote(level_name_ + " (aa3_mapbake)") + ",\"actors\":[]}\n";

    // ---- rules ---------------------------------------------------------------
    std::ostringstream rl;
    rl << "{\"level\":" << util::JsonQuote(level_name_) << ",\"map\":" << util::JsonQuote(map_name_)
       << ",\"extraction\":[" << ex.str() << "],\"viewpoints\":[";
    {
        bool first = true;
        for (size_t i = 0; i < map_.exports.size(); ++i) {
            if (ExportClassName(map_, i) != "AA_Viewpoint" || !ExportOuterPackage(map_, i).empty()) continue;
            int32_t net = ReadExportNetIndex(map_, i);
            if (net < 0) continue;
            rl << (first ? "" : ",") << net;
            first = false;
        }
    }
    rl << "]";
    auto mic = g.OfClass("AA_MCMissionInfoConnector");
    if (!mic.empty()) {
        const PropValue* mi = FindProp(g.Props(mic[0]), "miMissionInfo");
        if (mi) {
            auto mstr = [&](const char* n) { const PropValue* p = FindProp(mi->members, n); return p ? p->s : ""; };
            auto mint = [&](const char* n, int32_t d) { const PropValue* p = FindProp(mi->members, n); return p ? p->i : d; };
            const PropValue* pre = FindProp(mi->members, "bAllowPreplanning");
            rl << ",\"mission\":{\"name\":" << util::JsonQuote(mstr("sMissionName"))
               << ",\"author\":" << util::JsonQuote(mstr("sAuthor"))
               << ",\"type\":" << util::JsonQuote(mstr("sType"))
               << ",\"summary\":" << util::JsonQuote(mstr("sSummary"))
               << ",\"min_players\":" << mint("iMinPlayerCount", 2)
               << ",\"max_players\":" << mint("iMaxPlayerCount", 26)
               << ",\"mission_time_seconds\":" << mint("iMissionTimeSeconds", 360)
               << ",\"allow_preplanning\":" << (pre && pre->b ? "true" : "false") << "}";
        }
    }
    rl << ",\"teams\":[";
    {
        bool first = true;
        for (auto [team, ti] : team_info) {
            rl << (first ? "" : ",") << "{\"index\":" << team << ",\"name\":" << util::JsonQuote(g.Str(ti, "sTeamName"))
               << ",\"briefing\":" << util::JsonQuote(g.Str(ti, "sTeamBriefing"))
               << ",\"mission_type\":" << util::JsonQuote(g.Str(ti, "sMissionType")) << "}";
            first = false;
        }
    }
    rl << "],\"spawn_markers\":[";
    {
        bool first = true;
        for (const auto& f : fts) {
            if (f.marker < 0) continue;
            size_t m = static_cast<size_t>(f.marker);
            std::vector<MissionSpawn> starts;
            for (int32_t k = 0; k < 32; ++k) {
                int32_t ps = g.Ref(m, "anpPositions", k);
                if (ps < 0) continue;
                const PropValue* l = FindProp(g.Props(static_cast<size_t>(ps)), "Location");
                const PropValue* r = FindProp(g.Props(static_cast<size_t>(ps)), "Rotation");
                if (!l || l->kind != PropValue::kVector) continue;
                starts.push_back({l->vec[0], l->vec[1], l->vec[2], r && r->kind == PropValue::kRotator ? r->rot[1] : 0});
            }
            if (starts.empty()) {
                const PropValue* l = FindProp(g.Props(m), "Location");
                const PropValue* r = FindProp(g.Props(m), "Rotation");
                if (l && l->kind == PropValue::kVector)
                    starts.push_back({l->vec[0], l->vec[1], l->vec[2], r && r->kind == PropValue::kRotator ? r->rot[1] : 0});
            }
            if (starts.empty()) continue;
            if (!out.has_team_spawn[f.team]) {
                out.has_team_spawn[f.team] = true;
                out.team_spawn[f.team] = starts[0];
            }
            rl << (first ? "" : ",") << "{\"net\":" << g.NetIndex(m) << ",\"team\":" << f.team
               << ",\"fire_team\":" << f.slot << ",\"starts\":[";
            for (size_t k = 0; k < starts.size(); ++k)
                rl << (k ? "," : "") << "[" << Fmt(starts[k].x) << "," << Fmt(starts[k].y) << "," << Fmt(starts[k].z)
                   << "," << starts[k].yaw << "]";
            rl << "]}";
            first = false;
        }
    }
    rl << "]}\n";
    out.rules_json = rl.str();

    return true;
}

} // namespace engine
} // namespace aa3
