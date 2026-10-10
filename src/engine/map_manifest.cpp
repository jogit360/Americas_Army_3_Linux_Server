#include "engine/map_manifest.h"
#include "engine/package_reader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <openssl/evp.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace aa3 {
namespace engine {

namespace {

// Always-loaded UE3 script packages, in the order the retail server emits
// them. `IpDrv` is intentionally omitted: it carries PKG_ServerSideOnly
// (its FLAGS field on disk has bit 0x4 set) so the retail server never
// emits a USES for it. `UnrealScriptTest` is a separate `.u` always loaded
// by the cooked engine boot.
const std::array<const char*, 8> kScriptBasePackages = {
    "Core", "Engine", "GameFramework", "UnrealScriptTest", "GFxUI",
    "OnlineSubsystemAA3", "AA3Baseline", "AA3Game",
};

// The cooker's synthetic "Startup" package collects a set of always-loaded
// engine resources. It has no on-disk file; the retail server emits each
// child with BASEPKG=Startup but never emits a USES for `Startup` itself.
// Names taken from the legit-server pcap.
const std::array<const char*, 7> kStartupChildren = {
    "EngineFonts", "EngineMaterials", "EngineResources", "EditorResources",
    "DefaultUISkin", "SoundModes", "EngineSounds",
};

// Content packages with their own on-disk file that the retail server loads
// unconditionally (i.e. not via any map's import table). Observed in the
// wire transcript with BASEPKG=None for every map.
const std::array<const char*, 1> kAlwaysLoadedContent = {
    "AA_PPE",
};

// Forced-export-attribution priority. When the same child appears as a
// forced UPackage export in multiple parent files, the retail server
// reports the MOST-SPECIFIC parent as BASEPKG, NOT the first-loaded one.
// Empirically (vs Bridge_night_clear pcap): AA3Game claims its UI exports
// before AA3Baseline gets a chance; the primary map claims world content
// before any script base. So we sort: smaller "scopes" first.
//
// The primary map gets claimed at manifest-build time (not here) — see
// BuildWithUniverse step 0.
const std::array<const char*, 7> kForcedExportPriority = {
    "Core", "Engine", "GameFramework", "GFxUI",
    "OnlineSubsystemAA3", "AA3Game", "AA3Baseline",
};

bool IsScriptBase(const std::string& name) {
    for (auto p : kScriptBasePackages) if (name == p) return true;
    return false;
}

// AA3 gametype suffix → AGP_Game_* class. Empirical mapping; the wire-data
// pass refines it.
std::string GuessGameClass(const std::string& pkg) {
    std::string l(pkg);
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (l.find("_inf_co_")      != std::string::npos) return "AGP_Game_INF_CO";
    if (l.find("_inf_tah_")     != std::string::npos) return "AGP_Game_INF_TAH";
    if (l.find("_inf_vip_")     != std::string::npos) return "AGP_Game_INF_VIP";
    if (l.find("_inf_ao_")      != std::string::npos) return "AGP_Game_INF_AO";
    if (l.find("_inf_ad_")      != std::string::npos) return "AGP_Game_INF_AD";
    if (l.find("_inf_classic_") != std::string::npos) return "AGP_Game_INF_Classic";
    if (l.find("_ait_") != std::string::npos) return "AGP_Game_AIT";
    if (l.find("_bct_") != std::string::npos) return "AGP_Game_BCT";
    if (l.find("_cls")  != std::string::npos) return "AGP_Game_CLS";
    return "";
}

std::string GuessTod(const std::string& pkg) {
    std::string l(pkg);
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (l.find("_night") != std::string::npos) return "Night";
    if (l.find("_dawn")  != std::string::npos) return "Dawn";
    if (l.find("_dusk")  != std::string::npos) return "Dusk";
    return "Day";
}

bool IsGametypeMap(const std::string& pkg) {
    std::string l(pkg);
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (l.rfind("s_map_", 0) == 0) return false;
    if (l.rfind("s_",     0) == 0 && l.find("_sound") != std::string::npos) return false;
    static const char* shard_suf[] = {"_volumes", "_sound", "_sounds",
                                      "_lighting", "_pathing", "_cubemaps",
                                      "_beastvolumes"};
    for (auto suf : shard_suf) {
        size_t n = std::strlen(suf);
        if (l.size() >= n && l.compare(l.size() - n, n, suf) == 0) return false;
    }
    return l.find("_inf_") != std::string::npos
        || l.find("_ait_") != std::string::npos
        || l.find("_bct_") != std::string::npos
        || l.find("_cls")  != std::string::npos;
}

std::string PathStem(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

std::string PathExt(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    return path.substr(dot + 1);
}

std::string Sha256Hex(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    char buf[1 << 16];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(f.gcount()));
    }
    unsigned char digest[32];
    unsigned int  digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);
    char hex[65];
    for (unsigned int i = 0; i < digest_len; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, digest_len * 2);
}

int64_t FileSize(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_size;
}

// JSON helpers (no dependency on a real JSON library — the schema is fixed).
std::string JsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

} // namespace

// ---------- UsesEntry::FormatUsesLine -------------------------------------

std::string UsesEntry::FormatUsesLine() const {
    // Mirror FUN_009e8200: "USES GUID=%s PKG=%s EXT=%s FLAGS=%i GEN=%i BASEPKG=%s"
    char buf[512];
    const char* base = basepkg.empty() ? "None" : basepkg.c_str();
    std::snprintf(buf, sizeof(buf),
                  "USES GUID=%s PKG=%s EXT=%s FLAGS=%u GEN=%d BASEPKG=%s",
                  guid.empty() ? "00000000000000000000000000000000" : guid.c_str(),
                  pkg.c_str(),
                  ext.empty() ? "upk" : ext.c_str(),
                  flags, gen, base);
    return std::string(buf);
}

// ---------- MapManifest::FormatWelcomeLine --------------------------------

std::string MapManifest::FormatWelcomeLine() const {
    // FUN_0048a8d0 has a branch that can append a second token, but the
    // legit-server pcap shows AA3 never uses it in practice — WELCOME is
    // always just "WELCOME LEVEL=<base map>". The base map (e.g. the
    // *_day_*/*_night_* lighting variant) is the URL; the gametype variant
    // is shipped to the client as one of the USES entries and the actual
    // game-class is negotiated later via PlayerController replication.
    return "WELCOME LEVEL=" + url;
}

std::vector<std::string> MapManifest::FormatWireLines() const {
    // Retail emit order observed on the wire (Bridge_night_clear pcap):
    //   <all USES, ordered scripts → content → shards → primary map>
    //   WELCOME LEVEL=<base map>
    // WELCOME comes LAST, not first.
    std::vector<std::string> out;
    out.reserve(uses.size() + 1);
    for (const auto& u : uses) out.push_back(u.FormatUsesLine());
    out.push_back(FormatWelcomeLine());
    return out;
}

// ---------- IndexCookedPackages -------------------------------------------

namespace {

bool HasSuffix(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

void IndexWalk(const std::string& dir,
               std::unordered_map<std::string, std::string>* out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (auto* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string full = dir + "/" + e->d_name;
        struct stat st {};
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            IndexWalk(full, out);
        } else if (S_ISREG(st.st_mode)) {
            std::string name = e->d_name;
            if (HasSuffix(name, ".upk") || HasSuffix(name, ".u") ||
                HasSuffix(name, ".aa3")) {
                std::string stem = PathStem(name);
                out->emplace(stem, full);
            }
        }
    }
    ::closedir(d);
}

// Fill guid/flags/gen for one UsesEntry by reading the dep package's summary.
void FillDepFromDisk(UsesEntry& u, const std::string& path) {
    PackageReadError perr;
    auto s = PackageReader::ReadSummaryFile(path, &perr);
    if (!s) return;  // leave the placeholder
    u.guid  = s->GuidString();
    u.flags = s->package_flags;
    u.gen   = static_cast<int>(std::max<size_t>(s->generations.size(), 1));
    // EXT comes from the file on disk, not the import-name; respect that
    // (e.g. Core is "Core.u", MasterMaterials is "MasterMaterials.upk").
    std::string ext = PathExt(path);
    if (!ext.empty()) u.ext = ext;
}

} // namespace

std::unordered_map<std::string, std::string>
IndexCookedPackages(const std::string& cooked_root) {
    std::unordered_map<std::string, std::string> out;
    if (cooked_root.empty()) return out;
    IndexWalk(cooked_root, &out);
    return out;
}

// ---------- PackageUniverse::Build ----------------------------------------

PackageUniverse PackageUniverse::Build(const std::string& cooked_root) {
    PackageUniverse u;
    u.by_name = IndexCookedPackages(cooked_root);

    // First pass: read every package's summary into `info`. Cheap.
    for (const auto& [name, path] : u.by_name) {
        PackageReadError perr;
        auto s = PackageReader::ReadSummaryFile(path, &perr);
        if (!s) continue;
        DepInfo d;
        d.guid  = s->GuidString();
        d.flags = s->package_flags;
        d.gen   = static_cast<int>(std::max<size_t>(s->generations.size(), 1));
        d.ext   = PathExt(path);
        u.info[name] = d;
    }

    // Synthetic Startup children — Startup itself isn't a file so we record
    // each child but no `Startup` entry in by_name.
    for (auto c : kStartupChildren) {
        u.forced_parent_of[c]      = "Startup";
        u.forced_children_of["Startup"].push_back(c);
    }

    // Second pass: open each parent in priority order, list its forced-export
    // children, and claim each child the first time we see it (priority
    // resolution).
    auto claim_from = [&](const std::string& parent) {
        auto it = u.by_name.find(parent);
        if (it == u.by_name.end()) return;
        PackageReadError perr;
        auto pkg = PackageReader::ReadFile(it->second, &perr);
        if (!pkg) return;
        for (const auto& ch : pkg->ForcedExportChildren()) {
            if (ch.name == parent) continue;
            if (u.forced_parent_of.count(ch.name)) continue;  // higher-priority parent already claimed
            u.forced_parent_of[ch.name] = parent;
            u.forced_children_of[parent].push_back(ch.name);
            // Cache the per-export GUID so synthetic shards still ship the
            // right FGuid on the wire.
            uint32_t a, b, c, d;
            std::memcpy(&a, ch.guid.data() + 0, 4);
            std::memcpy(&b, ch.guid.data() + 4, 4);
            std::memcpy(&c, ch.guid.data() + 8, 4);
            std::memcpy(&d, ch.guid.data() + 12, 4);
            char buf[33];
            std::snprintf(buf, sizeof(buf), "%08X%08X%08X%08X", a, b, c, d);
            u.forced_child_guid[ch.name] = buf;
        }
    };

    for (auto p : kForcedExportPriority) claim_from(p);

    // We intentionally DO NOT pre-claim map-scope forced exports here.
    // Two different base maps (Bridge_day_fog vs Bridge_night_clear) can
    // both forced-export the same world content (SurfBrick_02 etc.), so
    // attributing those children at universe-build time gives the wrong
    // BASEPKG depending on map iteration order. BuildWithUniverse resolves
    // map-scope forced exports on demand from the actual map file.

    return u;
}

// Packages that are never on the wire: editor-only or otherwise filtered out
// by the retail server. We match names exactly.
bool IsExcludedPackage(const std::string& name) {
    // Retail does send Editor_UI, Editor_AISupportIcons, MCEditorResources,
    // TestSAI and SoundNodeVOIP_Test (all forced exports of AA3Baseline) --
    // 170 USES in GamePlayPCAP_Legit_Server; the 164-record extraction this
    // list was built from had missed them (doc sec 26).
    static const std::array<const char*, 2> excluded = {
        "UnrealEd", "Editor",
    };
    for (auto e : excluded) if (name == e) return true;
    return false;
}

// Sibling .aa3 files of a primary base map. Retail emits USES for the
// gametype variants + the shard maps (pathing/volumes/lighting/sound), but
// NOT for the other lighting variants (the *_day_*, *_night_*, etc. peers).
bool IsLightingSibling(const std::string& sib, const std::string& primary) {
    auto strip_lighting = [](std::string s) {
        std::string l(s);
        std::transform(l.begin(), l.end(), l.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        static const char* keys[] = {"_day_", "_night_", "_dawn_", "_dusk_"};
        for (auto k : keys) {
            auto p = l.find(k);
            if (p != std::string::npos) return s.substr(0, p);
        }
        return s;
    };
    std::string a = strip_lighting(sib), b = strip_lighting(primary);
    if (a == sib || b == primary) return false;  // neither is a lighting variant
    return a == b;
}

// ---------- MapManifest::BuildFromPackage ---------------------------------

std::optional<MapManifest>
MapManifest::BuildFromPackage(const std::string& map_path,
                              const std::string& cooked_root,
                              std::string* err) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(map_path, &perr);
    if (!pkg) {
        // Tables may be unreadable for fully-compressed maps with LZO
        // surprises; fall back to summary-only and synthesize a uses[]
        // containing just the primary entry + the canonical script bases.
        auto s = PackageReader::ReadSummaryFile(map_path, &perr);
        if (!s) {
            if (err) *err = "summary read: " + perr.message;
            return std::nullopt;
        }
        MapManifest m;
        m.source_path = map_path;
        m.source_size = FileSize(map_path);
        m.source_sha256 = Sha256Hex(map_path);
        m.engine_version = s->version;
        m.licensee_version = s->licensee_version;
        m.url = PathStem(map_path);
        m.game_class = GuessGameClass(m.url);
        m.tod = GuessTod(m.url);
        m.is_gametype_map = IsGametypeMap(m.url);
        if (!m.game_class.empty()) m.default_options = "?game=" + m.game_class;

        for (auto spkg : kScriptBasePackages) {
            UsesEntry u;
            u.pkg = spkg; u.ext = "u";
            u.flags = PKG_ContainsScript; u.gen = 1;
            u.role = "script";
            m.uses.push_back(u);
        }
        UsesEntry primary;
        primary.guid    = s->GuidString();
        primary.pkg     = m.url;
        primary.ext     = PathExt(map_path);
        if (primary.ext.empty()) primary.ext = "aa3";
        primary.flags   = s->package_flags;
        primary.gen     = static_cast<int>(std::max<size_t>(s->generations.size(), 1));
        primary.basepkg = "None";
        primary.role    = "map";
        m.uses.push_back(primary);

        if (!cooked_root.empty()) {
            auto index = IndexCookedPackages(cooked_root);
            for (auto& u : m.uses) {
                if (u.role == "map") continue;
                if (!u.guid.empty())  continue;
                auto it = index.find(u.pkg);
                if (it != index.end()) FillDepFromDisk(u, it->second);
            }
        }
        return m;
    }

    MapManifest m;
    m.source_path     = map_path;
    m.source_size     = FileSize(map_path);
    m.source_sha256   = Sha256Hex(map_path);
    m.engine_version  = pkg->summary.version;
    m.licensee_version = pkg->summary.licensee_version;
    m.url             = PathStem(map_path);
    m.game_class      = GuessGameClass(m.url);
    m.tod             = GuessTod(m.url);
    m.is_gametype_map = IsGametypeMap(m.url);
    if (!m.game_class.empty()) m.default_options = "?game=" + m.game_class;

    // 1) Script base packages first (canonical order).
    for (auto spkg : kScriptBasePackages) {
        UsesEntry u;
        u.pkg = spkg; u.ext = "u";
        u.flags = PKG_ContainsScript; u.gen = 1;
        u.role = "script";
        m.uses.push_back(u);
    }

    // 2) Top-level imported content packages (deduped, retail emit order).
    for (const auto& dep_name : pkg->TopLevelImportPackages()) {
        if (IsScriptBase(dep_name)) continue;  // already added
        if (dep_name == m.url)      continue;  // self
        UsesEntry u;
        u.pkg = dep_name;
        u.ext = "upk";
        u.flags = PKG_AllowDownload;
        u.gen = 1;
        u.role = "content";
        m.uses.push_back(u);
    }

    // 3) Primary map last.
    UsesEntry primary;
    primary.guid    = pkg->summary.GuidString();
    primary.pkg     = m.url;
    primary.ext     = PathExt(map_path);
    if (primary.ext.empty()) primary.ext = "aa3";
    primary.flags   = pkg->summary.package_flags;
    primary.gen     = static_cast<int>(std::max<size_t>(pkg->summary.generations.size(), 1));
    primary.basepkg = "None";
    primary.role    = "map";
    m.uses.push_back(primary);

    if (!cooked_root.empty()) {
        auto index = IndexCookedPackages(cooked_root);
        for (auto& u : m.uses) {
            if (u.role == "map") continue;       // primary already has real data
            if (!u.guid.empty())  continue;       // skip if already filled
            auto it = index.find(u.pkg);
            if (it != index.end()) FillDepFromDisk(u, it->second);
        }
    }

    return m;
}

// ---------- MapManifest::BuildWithUniverse --------------------------------

namespace {

constexpr uint32_t kForcedShardFlags = PKG_AllowDownload;  // retail emits FLAGS=1

void ApplyDepInfo(UsesEntry& u, const PackageUniverse::DepInfo& d) {
    u.guid  = d.guid;
    u.flags = d.flags;
    u.gen   = d.gen;
    if (!d.ext.empty()) u.ext = d.ext;
}

UsesEntry MakeForcedShard(const std::string& child, const std::string& parent,
                          const std::string& guid = "") {
    // No on-disk file. FLAGS=1 (PKG_AllowDownload) by retail convention.
    UsesEntry u;
    u.guid    = guid.empty() ? "00000000000000000000000000000000" : guid;
    u.pkg     = child;
    u.ext     = "upk";
    u.flags   = kForcedShardFlags;
    u.gen     = 1;
    u.basepkg = parent;
    u.role    = "shard";
    return u;
}

// Names that are synthetic (no file, never emitted as their own USES).
bool IsSyntheticName(const std::string& name) {
    return name == "Startup";
}

void EmitOnePkg(const PackageUniverse& uni,
                std::vector<UsesEntry>& out,
                const std::string& pkg_name,
                const std::string& role,
                const std::string& basepkg,
                std::unordered_map<std::string, size_t>& already) {
    if (already.count(pkg_name)) return;
    if (IsExcludedPackage(pkg_name)) return;
    if (IsSyntheticName(pkg_name))   return;
    UsesEntry u;
    u.pkg     = pkg_name;
    u.role    = role;
    u.basepkg = basepkg;
    auto info_it = uni.info.find(pkg_name);
    if (info_it != uni.info.end()) {
        if ((info_it->second.flags & PKG_ServerSideOnly) != 0) return;
        ApplyDepInfo(u, info_it->second);
    } else {
        // No on-disk file → emit as a forced-export shard. Look up the
        // per-export GUID we cached at universe-build time.
        auto gi = uni.forced_child_guid.find(pkg_name);
        std::string guid = (gi != uni.forced_child_guid.end()) ? gi->second : "";
        u = MakeForcedShard(pkg_name, basepkg, guid);
        u.role = role;
    }
    already[pkg_name] = out.size();
    out.push_back(u);
}

void EmitWithChildren(const PackageUniverse& uni,
                      std::vector<UsesEntry>& out,
                      const std::string& pkg_name,
                      const std::string& role,
                      std::unordered_map<std::string, size_t>& already) {
    // Parent first.
    EmitOnePkg(uni, out, pkg_name, role, "None", already);
    auto ch_it = uni.forced_children_of.find(pkg_name);
    if (ch_it == uni.forced_children_of.end()) return;
    for (const auto& child : ch_it->second) {
        EmitOnePkg(uni, out, child, "content", pkg_name, already);
    }
}

} // namespace

std::optional<MapManifest>
MapManifest::BuildWithUniverse(const std::string& map_path,
                               const PackageUniverse& uni,
                               std::string* err) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(map_path, &perr);
    if (!pkg) {
        if (err) *err = "read " + map_path + ": " + perr.message;
        return std::nullopt;
    }

    MapManifest m;
    m.source_path     = map_path;
    m.source_size     = FileSize(map_path);
    m.source_sha256   = Sha256Hex(map_path);
    m.engine_version  = pkg->summary.version;
    m.licensee_version = pkg->summary.licensee_version;
    m.url             = PathStem(map_path);
    m.game_class      = GuessGameClass(m.url);
    m.tod             = GuessTod(m.url);
    m.is_gametype_map = IsGametypeMap(m.url);
    if (!m.game_class.empty()) m.default_options = "?game=" + m.game_class;

    std::unordered_map<std::string, size_t> already;

    // 0) Pre-claim forced-export children at the map-family scope. Three
    //    nested scopes, each more specific than AA3Baseline:
    //      (a) primary map  (most specific)
    //      (b) sibling shard maps in the same directory (pathing/volumes/
    //          gametype variants — they own world content the script bases
    //          don't)
    //    Each later claim is allowed to override an earlier one when the
    //    later scope is more specific, but within (b) we leave the first
    //    sibling-claim winning since retail order shows it doesn't matter.
    PackageUniverse local = uni;
    auto guid_str = [](const std::array<uint8_t, 16>& g) {
        uint32_t a, b, c, d;
        std::memcpy(&a, g.data() + 0, 4);
        std::memcpy(&b, g.data() + 4, 4);
        std::memcpy(&c, g.data() + 8, 4);
        std::memcpy(&d, g.data() + 12, 4);
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%08X%08X%08X%08X", a, b, c, d);
        return std::string(buf);
    };
    auto reattribute = [&](const std::string& child,
                           const std::string& new_parent,
                           const std::array<uint8_t, 16>& guid) {
        auto prev_it = local.forced_parent_of.find(child);
        if (prev_it != local.forced_parent_of.end()) {
            auto& vec = local.forced_children_of[prev_it->second];
            vec.erase(std::remove(vec.begin(), vec.end(), child), vec.end());
        }
        local.forced_parent_of[child] = new_parent;
        local.forced_children_of[new_parent].push_back(child);
        local.forced_child_guid[child] = guid_str(guid);
    };

    // (a) Primary map.
    for (const auto& ch : pkg->ForcedExportChildren()) {
        reattribute(ch.name, m.url, ch.guid);
    }

    // (b) Sibling shard maps (same directory, .aa3, not a lighting peer).
    //     Walk each, pull its forced-export children, and claim under the
    //     sibling's name if no more-specific parent (primary map) holds it.
    {
        std::string dir = map_path;
        auto slash = dir.find_last_of("/\\");
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        DIR* d = ::opendir(dir.c_str());
        if (d) {
            std::vector<std::string> sib_paths;
            while (auto* e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                std::string nm = e->d_name;
                if (!HasSuffix(nm, ".aa3")) continue;
                std::string stem = PathStem(nm);
                if (stem == m.url) continue;
                if (IsLightingSibling(stem, m.url)) continue;
                sib_paths.push_back(dir + "/" + nm);
            }
            ::closedir(d);
            std::sort(sib_paths.begin(), sib_paths.end());
            for (const auto& sp : sib_paths) {
                PackageReadError perr;
                auto sib_pkg = PackageReader::ReadFile(sp, &perr);
                if (!sib_pkg) continue;
                std::string sib_name = PathStem(sp);
                for (const auto& ch : sib_pkg->ForcedExportChildren()) {
                    // Don't override an attribution already at the primary
                    // map (more specific).
                    auto prev_it = local.forced_parent_of.find(ch.name);
                    if (prev_it != local.forced_parent_of.end() &&
                        prev_it->second == m.url) continue;
                    reattribute(ch.name, sib_name, ch.guid);
                }
            }
        }
    }

    // 1) Script base packages (and their forced-export children) in the
    //    order the retail server emits them on the wire.
    EmitWithChildren(local, m.uses, "Core",               "script", already);
    EmitWithChildren(local, m.uses, "Engine",             "script", already);
    EmitWithChildren(local, m.uses, "Startup",            "script", already);  // synthetic parent: children only
    EmitWithChildren(local, m.uses, "GameFramework",      "script", already);
    EmitWithChildren(local, m.uses, "UnrealScriptTest",   "script", already);
    EmitWithChildren(local, m.uses, "GFxUI",              "script", already);
    EmitWithChildren(local, m.uses, "OnlineSubsystemAA3", "script", already);
    EmitWithChildren(local, m.uses, "AA3Baseline",        "script", already);
    EmitWithChildren(local, m.uses, "AA3Game",            "script", already);

    // 2) Always-loaded content packages (own files, not in any forced-export
    //    list — engine loads them at boot regardless of map).
    for (auto name : kAlwaysLoadedContent) {
        EmitOnePkg(local, m.uses, name, "content", "None", already);
    }

    // 3) Map's direct content imports (any that didn't already land as
    //    a forced-export child of the script base set).
    for (const auto& imp : pkg->TopLevelImportPackages()) {
        EmitOnePkg(local, m.uses, imp, "content", "None", already);
    }

    // 4) Map-family siblings — gametype variants + shard maps (pathing,
    //    volumes, sound) living in the same directory. Skip other lighting
    //    variants of the same base (e.g. Bridge_day_fog when primary is
    //    Bridge_night_clear) — the engine only loads one base lighting.
    {
        std::string dir = map_path;
        auto slash = dir.find_last_of("/\\");
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        std::vector<std::string> sibling_names;
        DIR* d = ::opendir(dir.c_str());
        if (d) {
            while (auto* e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                std::string nm = e->d_name;
                if (!HasSuffix(nm, ".aa3")) continue;
                std::string stem = PathStem(nm);
                if (stem == m.url) continue;
                if (IsLightingSibling(stem, m.url)) continue;
                sibling_names.push_back(stem);
            }
            ::closedir(d);
        }
        std::sort(sibling_names.begin(), sibling_names.end());
        for (const auto& sib : sibling_names) {
            EmitOnePkg(local, m.uses, sib, "shard", "None", already);
        }
    }

    // 5) Primary map LAST among USES, followed by its own forced-export
    //    children (resolved from the actual file — universe doesn't attribute
    //    map-scope forced exports because lighting siblings would steal them).
    EmitOnePkg(local, m.uses, m.url, "map", "None", already);
    for (const auto& child : pkg->ForcedExportPackages()) {
        EmitOnePkg(local, m.uses, child, "content", m.url, already);
    }

    return m;
}

// ---------- MapManifest::ToJson -------------------------------------------

std::string MapManifest::ToJson() const {
    std::ostringstream o;
    o << "{\n";
    o << "  \"manifest_version\": " << manifest_version << ",\n";
    o << "  \"generated_by\":     \"aa3_build_manifests (C++)\",\n";
    o << "  \"source\": {\n";
    o << "    \"path\":   \"" << JsonEscape(source_path)   << "\",\n";
    o << "    \"size\":   " << source_size << ",\n";
    o << "    \"sha256\": \"" << source_sha256 << "\",\n";
    o << "    \"engine_version\":   " << engine_version   << ",\n";
    o << "    \"licensee_version\": " << licensee_version << "\n";
    o << "  },\n";
    o << "  \"map\": {\n";
    o << "    \"url\":             \"" << JsonEscape(url)        << "\",\n";
    o << "    \"game_class\":      "   << (game_class.empty() ? "null"
                                          : "\"" + JsonEscape(game_class) + "\"") << ",\n";
    o << "    \"tod\":             \"" << JsonEscape(tod)        << "\",\n";
    o << "    \"is_gametype_map\": "   << (is_gametype_map ? "true" : "false") << ",\n";
    o << "    \"default_options\": \"" << JsonEscape(default_options) << "\"\n";
    o << "  },\n";
    o << "  \"dlmgr\":            "
      << (dlmgr.empty() ? "null" : "\"" + JsonEscape(dlmgr) + "\"") << ",\n";
    o << "  \"uses\": [\n";
    for (size_t i = 0; i < uses.size(); ++i) {
        const auto& u = uses[i];
        char flags_buf[16];
        std::snprintf(flags_buf, sizeof(flags_buf), "0x%08X", u.flags);
        o << "    {";
        o << " \"guid\":\""    << JsonEscape(u.guid)    << "\",";
        o << " \"pkg\":\""     << JsonEscape(u.pkg)     << "\",";
        o << " \"ext\":\""     << JsonEscape(u.ext)     << "\",";
        o << " \"flags\":\""   << flags_buf             << "\",";
        o << " \"gen\":"       << u.gen                 << ",";
        o << " \"basepkg\":\"" << JsonEscape(u.basepkg) << "\",";
        o << " \"role\":\""    << JsonEscape(u.role)    << "\",";
        o << " \"net_objects\":" << u.net_objects;
        o << " }";
        if (i + 1 < uses.size()) o << ",";
        o << "\n";
    }
    o << "  ]\n";
    o << "}\n";
    return o.str();
}

std::optional<uint32_t> MapManifest::ObjectBase(const std::string& pkg) const {
    uint64_t base = 0;
    for (const auto& u : uses) {
        if (u.pkg == pkg) return static_cast<uint32_t>(base);
        if (u.net_objects < 0) return std::nullopt;
        base += static_cast<uint64_t>(u.net_objects);
    }
    return std::nullopt;
}

size_t FillNetObjectCounts(MapManifest& m,
                           const std::unordered_map<std::string, std::string>& by_name) {
    // Package names are case-insensitive (USES says "mt_base", the file is
    // MT_Base.upk); `by_name` is keyed by the exact file stem.
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    std::unordered_map<std::string, std::string> by_lower;
    for (const auto& [stem, path] : by_name) by_lower.emplace(lower(stem), path);
    auto find_file = [&](const std::string& name, const std::string&) -> std::string {
        auto it = by_lower.find(lower(name));
        return it != by_lower.end() ? it->second : std::string();
    };
    auto pick = [](const std::vector<int32_t>& counts, int32_t gen) -> int32_t {
        if (counts.empty()) return -1;
        size_t g = static_cast<size_t>(std::max(gen, 1)) - 1;
        return counts[std::min(g, counts.size() - 1)];
    };
    // Forced-export children per base package, read once.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<int32_t>>> forced;
    size_t unknown = 0;
    for (auto& u : m.uses) {
        u.net_objects = -1;
        if (!u.basepkg.empty() && u.basepkg != "None") {
            auto fit = forced.find(u.basepkg);
            if (fit == forced.end()) {
                fit = forced.emplace(u.basepkg, std::unordered_map<std::string, std::vector<int32_t>>()).first;
                std::string path = find_file(u.basepkg, "u");
                PackageReadError perr;
                if (!path.empty())
                    if (auto pkg = PackageReader::ReadFile(path, &perr))
                        for (const auto& ch : pkg->ForcedExportChildren())
                            fit->second.emplace(ch.name, ch.gen_net_objects);
            }
            auto cit = fit->second.find(u.pkg);
            if (cit != fit->second.end()) u.net_objects = pick(cit->second, u.gen);
        }
        if (u.net_objects < 0) {
            std::string path = find_file(u.pkg, u.ext);
            PackageReadError perr;
            if (!path.empty())
                if (auto s = PackageReader::ReadSummaryFile(path, &perr)) {
                    std::vector<int32_t> counts;
                    for (const auto& g : s->generations) counts.push_back(g.net_object_count);
                    u.net_objects = pick(counts, u.gen);
                }
        }
        if (u.net_objects < 0) ++unknown;
    }
    return unknown;
}

// ---------- BuildAndWriteManifest -----------------------------------------

std::optional<std::string>
BuildAndWriteManifest(const std::string& map_path,
                      const std::string& out_dir,
                      const std::string& cooked_root,
                      std::string* err) {
    auto m = MapManifest::BuildFromPackage(map_path, cooked_root, err);
    if (!m) return std::nullopt;
    FillNetObjectCounts(*m, IndexCookedPackages(cooked_root));
    std::string out_path = out_dir + "/" + PathStem(map_path) + ".aa3.manifest.json";
    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = std::string("open output: ") + std::strerror(errno);
        return std::nullopt;
    }
    f << m->ToJson();
    return out_path;
}

// ---------- FromJson (v1 schema) ------------------------------------------
//
// Hand-rolled parser. The emitter (`ToJson` above) produces a fixed shape, so
// we don't need a general-purpose JSON library: we look up keys by name and
// pull values out one at a time. Forgiving about whitespace; strict about
// required fields. Returns nullopt on malformed input.

namespace {

// Skip whitespace.
void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                            s[i] == '\n' || s[i] == '\r')) ++i;
}

// Find the next occurrence of `key:` (as `"key"` followed by `:`) starting
// at `start`. Returns the position just past the `:`, or npos.
size_t FindKey(const std::string& s, const std::string& key, size_t start = 0) {
    std::string needle = "\"" + key + "\"";
    size_t p = s.find(needle, start);
    if (p == std::string::npos) return std::string::npos;
    p += needle.size();
    SkipWs(s, p);
    if (p >= s.size() || s[p] != ':') return std::string::npos;
    return p + 1;
}

// Parse a JSON string starting at `i` (which should point at `"`). Returns
// the unescaped value; advances `i` past the closing `"`. False on error.
bool ParseString(const std::string& s, size_t& i, std::string* out) {
    SkipWs(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out->clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char nx = s[++i];
            switch (nx) {
                case '"':  *out += '"';  break;
                case '\\': *out += '\\'; break;
                case '/':  *out += '/';  break;
                case 'b':  *out += '\b'; break;
                case 'f':  *out += '\f'; break;
                case 'n':  *out += '\n'; break;
                case 'r':  *out += '\r'; break;
                case 't':  *out += '\t'; break;
                default:   *out += nx;   break;  // tolerate unknown escape
            }
            ++i;
        } else {
            *out += s[i++];
        }
    }
    if (i >= s.size()) return false;
    ++i;  // past closing quote
    return true;
}

// Parse a JSON integer at position `i`. Advances `i`. Returns false on error.
bool ParseInt(const std::string& s, size_t& i, int64_t* out) {
    SkipWs(s, i);
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        if (s[i] == '-') neg = true;
        ++i;
    }
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
    int64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    *out = neg ? -v : v;
    return true;
}

// Parse a string-or-null value. Returns false on syntax error. If the value
// is JSON `null`, leaves `out` empty.
bool ParseStringOrNull(const std::string& s, size_t& i, std::string* out) {
    SkipWs(s, i);
    if (i + 4 <= s.size() && s.compare(i, 4, "null") == 0) {
        i += 4;
        out->clear();
        return true;
    }
    return ParseString(s, i, out);
}

// Look up a key inside the object at [obj_start..obj_end) and return its
// string value (or null → empty). Does not enforce presence of the key.
bool LookupString(const std::string& s, size_t obj_start, size_t obj_end,
                  const std::string& key, std::string* out) {
    size_t p = FindKey(s.substr(obj_start, obj_end - obj_start), key);
    if (p == std::string::npos) { out->clear(); return false; }
    p += obj_start;
    return ParseStringOrNull(s, p, out);
}

bool LookupInt(const std::string& s, size_t obj_start, size_t obj_end,
               const std::string& key, int64_t* out) {
    size_t p = FindKey(s.substr(obj_start, obj_end - obj_start), key);
    if (p == std::string::npos) return false;
    p += obj_start;
    return ParseInt(s, p, out);
}

// Find the matching `]` for the `[` at position `open`. Handles nesting.
size_t MatchArray(const std::string& s, size_t open) {
    if (open >= s.size() || s[open] != '[') return std::string::npos;
    int depth = 1;
    bool in_str = false;
    for (size_t i = open + 1; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '[') ++depth;
        else if (c == ']' && --depth == 0) return i;
    }
    return std::string::npos;
}

// Find the matching `}` for the `{` at position `open`.
size_t MatchObject(const std::string& s, size_t open) {
    if (open >= s.size() || s[open] != '{') return std::string::npos;
    int depth = 1;
    bool in_str = false;
    for (size_t i = open + 1; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

uint32_t ParseFlags(const std::string& v) {
    // Accept "0x012345AB" (hex) or "12345" (decimal).
    if (v.size() >= 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        return static_cast<uint32_t>(std::stoul(v.substr(2), nullptr, 16));
    }
    if (v.empty()) return 0;
    return static_cast<uint32_t>(std::stoul(v, nullptr, 10));
}

} // namespace

std::optional<MapManifest>
MapManifest::FromJson(const std::string& json, std::string* err) {
    MapManifest m;

    // manifest_version (optional; default 1 if missing).
    {
        size_t p = FindKey(json, "manifest_version");
        if (p != std::string::npos) {
            int64_t v = 1;
            if (ParseInt(json, p, &v)) m.manifest_version = static_cast<int>(v);
        }
        if (m.manifest_version != 1) {
            if (err) *err = "unsupported manifest_version " +
                            std::to_string(m.manifest_version);
            return std::nullopt;
        }
    }

    // source { path, size, sha256, engine_version, licensee_version }
    {
        size_t p = FindKey(json, "source");
        if (p != std::string::npos) {
            SkipWs(json, p);
            size_t end = MatchObject(json, p);
            if (end != std::string::npos) {
                LookupString(json, p, end, "path", &m.source_path);
                LookupString(json, p, end, "sha256", &m.source_sha256);
                int64_t v = 0;
                if (LookupInt(json, p, end, "size", &v))             m.source_size      = v;
                if (LookupInt(json, p, end, "engine_version", &v))   m.engine_version   = static_cast<int>(v);
                if (LookupInt(json, p, end, "licensee_version", &v)) m.licensee_version = static_cast<int>(v);
            }
        }
    }

    // map { url, game_class, tod, is_gametype_map, default_options }
    {
        size_t p = FindKey(json, "map");
        if (p == std::string::npos) {
            if (err) *err = "missing 'map' object";
            return std::nullopt;
        }
        SkipWs(json, p);
        size_t end = MatchObject(json, p);
        if (end == std::string::npos) {
            if (err) *err = "malformed 'map' object";
            return std::nullopt;
        }
        LookupString(json, p, end, "url",             &m.url);
        LookupString(json, p, end, "game_class",      &m.game_class);
        LookupString(json, p, end, "tod",             &m.tod);
        LookupString(json, p, end, "default_options", &m.default_options);
        // is_gametype_map (bool)
        size_t bp = FindKey(json.substr(p, end - p), "is_gametype_map");
        if (bp != std::string::npos) {
            bp += p;
            SkipWs(json, bp);
            if (bp + 4 <= json.size() && json.compare(bp, 4, "true") == 0) {
                m.is_gametype_map = true;
            }
        }
    }

    // dlmgr (optional)
    {
        size_t p = FindKey(json, "dlmgr");
        if (p != std::string::npos) {
            std::string dl;
            if (ParseStringOrNull(json, p, &dl)) m.dlmgr = dl;
        }
    }

    // uses [ {guid, pkg, ext, flags, gen, basepkg, role}, ... ]
    {
        size_t p = FindKey(json, "uses");
        if (p == std::string::npos) {
            if (err) *err = "missing 'uses' array";
            return std::nullopt;
        }
        SkipWs(json, p);
        size_t end = MatchArray(json, p);
        if (end == std::string::npos) {
            if (err) *err = "malformed 'uses' array";
            return std::nullopt;
        }
        // Walk top-level objects inside the array.
        size_t i = p + 1;
        while (i < end) {
            SkipWs(json, i);
            if (i >= end || json[i] != '{') { ++i; continue; }
            size_t obj_end = MatchObject(json, i);
            if (obj_end == std::string::npos || obj_end > end) break;
            UsesEntry u;
            LookupString(json, i, obj_end, "guid",    &u.guid);
            LookupString(json, i, obj_end, "pkg",     &u.pkg);
            LookupString(json, i, obj_end, "ext",     &u.ext);
            LookupString(json, i, obj_end, "basepkg", &u.basepkg);
            LookupString(json, i, obj_end, "role",    &u.role);
            std::string flags_str;
            LookupString(json, i, obj_end, "flags",   &flags_str);
            if (!flags_str.empty()) u.flags = ParseFlags(flags_str);
            int64_t gen_v = 1;
            LookupInt(json, i, obj_end, "gen", &gen_v);
            u.gen = static_cast<int32_t>(gen_v);
            int64_t net_v = -1;
            LookupInt(json, i, obj_end, "net_objects", &net_v);
            u.net_objects = static_cast<int32_t>(net_v);
            m.uses.push_back(u);
            i = obj_end + 1;
        }
    }

    if (m.url.empty()) {
        if (err) *err = "manifest has empty map.url";
        return std::nullopt;
    }
    if (m.uses.empty()) {
        if (err) *err = "manifest has empty uses[]";
        return std::nullopt;
    }
    return m;
}

} // namespace engine
} // namespace aa3
