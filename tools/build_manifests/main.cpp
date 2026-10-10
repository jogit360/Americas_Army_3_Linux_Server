// aa3_build_manifests
//
// Walk a maps directory, build a v1 manifest per `.aa3` package using the
// engine's PackageReader (LZO decompression included), and write JSON
// manifests to the server's config/maps directory.
//
// Usage:
//   aa3_build_manifests --maps-root <dir> --out-dir <dir>
//                       [--cooked-root <dir>]
//   aa3_build_manifests --single <map.aa3> [--out-dir <dir>]
//                       [--cooked-root <dir>]
//   aa3_build_manifests --wire <map.aa3> [--cooked-root <dir>]
//                                          # print the WELCOME+USES sequence
//                                          # to stdout, no JSON written.
//
// --cooked-root <dir> enables dep resolution: every `.u`/`.upk`/`.aa3` under
// <dir> gets indexed by stem, and each USES entry's guid/flags/gen is filled
// from the matching package's FPackageFileSummary.

#include "engine/map_manifest.h"
#include "engine/package_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace aa3::engine;

namespace {

bool IsAa3(const std::string& name) {
    if (name.size() < 4) return false;
    return name.compare(name.size() - 4, 4, ".aa3") == 0;
}

void WalkAa3(const std::string& root, std::vector<std::string>* out) {
    DIR* d = ::opendir(root.c_str());
    if (!d) return;
    while (auto* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string full = root + "/" + e->d_name;
        struct stat st {};
        if (::stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            WalkAa3(full, out);
        } else if (S_ISREG(st.st_mode) && IsAa3(e->d_name)) {
            out->push_back(full);
        }
    }
    ::closedir(d);
}

int Usage(int code) {
    std::fprintf(stderr,
        "usage:\n"
        "  aa3_build_manifests --maps-root <dir> --out-dir <dir> [--cooked-root <dir>]\n"
        "  aa3_build_manifests --single <map.aa3> [--out-dir <dir>] [--cooked-root <dir>]\n"
        "  aa3_build_manifests --wire   <map.aa3> [--cooked-root <dir>]\n");
    return code;
}

int DoBatch(const std::string& root, const std::string& out_dir,
            const std::string& cooked_root) {
    std::vector<std::string> maps;
    WalkAa3(root, &maps);
    std::sort(maps.begin(), maps.end());

    // Build the universe once and reuse across all maps.
    std::optional<PackageUniverse> uni;
    if (!cooked_root.empty()) {
        std::fprintf(stderr, "Building package universe from %s ...\n",
                     cooked_root.c_str());
        uni = PackageUniverse::Build(cooked_root);
        std::fprintf(stderr, "Universe: %zu packages, %zu forced-export children\n",
                     uni->info.size(), uni->forced_parent_of.size());
    }

    int ok = 0, fail = 0;
    for (const auto& p : maps) {
        std::string err;
        std::optional<MapManifest> m;
        if (uni) {
            m = MapManifest::BuildWithUniverse(p, *uni, &err);
        } else {
            m = MapManifest::BuildFromPackage(p, "", &err);
        }
        if (m && uni) {
            size_t unknown = FillNetObjectCounts(*m, uni->by_name);
            if (unknown)
                std::fprintf(stderr, "[warn] %s: %zu USES entries without a net object "
                             "count (ObjectIndex resolution will fail)\n", p.c_str(), unknown);
        }
        if (!m) {
            std::fprintf(stderr, "[fail] %s: %s\n", p.c_str(), err.c_str());
            ++fail;
            continue;
        }
        std::string out_path = out_dir + "/" +
            std::string(p).substr(p.find_last_of("/\\") + 1);
        // Strip ".aa3" → ".aa3.manifest.json"
        if (out_path.size() >= 4 &&
            out_path.compare(out_path.size() - 4, 4, ".aa3") == 0) {
            out_path = out_path.substr(0, out_path.size() - 4) +
                       ".aa3.manifest.json";
        }
        std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
        if (!f) { ++fail; continue; }
        f << m->ToJson();
        std::printf("[ok]  %s -> %s\n", p.c_str(), out_path.c_str());
        ++ok;
    }
    std::printf("\nwrote %d manifest(s); %d failed\n", ok, fail);
    return fail == 0 ? 0 : 2;
}

int DoSingle(const std::string& path, const std::string& out_dir,
             const std::string& cooked_root) {
    std::string err;
    auto out = BuildAndWriteManifest(path, out_dir, cooked_root, &err);
    if (!out) {
        std::fprintf(stderr, "build failed: %s\n", err.c_str());
        return 2;
    }
    std::printf("%s\n", out->c_str());
    return 0;
}

int DoWire(const std::string& path, const std::string& cooked_root) {
    std::string err;
    std::optional<MapManifest> m;
    if (!cooked_root.empty()) {
        std::fprintf(stderr, "Building package universe from %s ...\n",
                     cooked_root.c_str());
        auto uni = PackageUniverse::Build(cooked_root);
        std::fprintf(stderr, "Universe: %zu packages, %zu forced-export children\n",
                     uni.info.size(), uni.forced_parent_of.size());
        m = MapManifest::BuildWithUniverse(path, uni, &err);
        if (m) FillNetObjectCounts(*m, uni.by_name);
    } else {
        m = MapManifest::BuildFromPackage(path, "", &err);
    }
    if (!m) {
        std::fprintf(stderr, "build failed: %s\n", err.c_str());
        return 2;
    }
    for (const auto& line : m->FormatWireLines()) {
        std::printf("%s\n", line.c_str());
    }
    return 0;
}

int DoRoundtrip(const std::string& manifest_json_path) {
    std::ifstream f(manifest_json_path);
    if (!f) { std::fprintf(stderr, "open: %s\n", manifest_json_path.c_str()); return 2; }
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    std::string err;
    auto m = MapManifest::FromJson(buf, &err);
    if (!m) {
        std::fprintf(stderr, "FromJson failed: %s\n", err.c_str());
        return 2;
    }
    std::printf("# %s parsed OK\n", manifest_json_path.c_str());
    std::printf("  url:       %s\n", m->url.c_str());
    std::printf("  dlmgr:     %s\n", m->dlmgr.c_str());
    std::printf("  uses:      %zu\n", m->uses.size());
    std::printf("\nwire preview:\n");
    int n = 0;
    for (const auto& line : m->FormatWireLines()) {
        if (n < 3 || n + 3 >= static_cast<int>(m->uses.size() + 1)) {
            std::printf("  %s\n", line.c_str());
        } else if (n == 3) {
            std::printf("  ... (%zu lines elided) ...\n", m->uses.size() - 5);
        }
        ++n;
    }
    return 0;
}

int DoDumpUniverse(const std::string& cooked_root, const std::string& parent) {
    auto uni = PackageUniverse::Build(cooked_root);
    std::printf("# universe: %zu packages, %zu forced-export children\n",
                uni.info.size(), uni.forced_parent_of.size());
    if (parent.empty()) {
        std::printf("# parents with forced-export children:\n");
        std::vector<std::pair<std::string, size_t>> sorted;
        for (const auto& [p, kids] : uni.forced_children_of)
            sorted.push_back({p, kids.size()});
        std::sort(sorted.begin(), sorted.end(),
                  [](auto& a, auto& b){ return a.second > b.second; });
        for (const auto& [p, n] : sorted) std::printf("  %-40s %zu\n", p.c_str(), n);
        return 0;
    }
    auto it = uni.forced_children_of.find(parent);
    if (it == uni.forced_children_of.end()) {
        std::printf("# no forced-export children attributed to '%s'\n", parent.c_str());
        return 0;
    }
    std::printf("# %s has %zu forced-export children:\n",
                parent.c_str(), it->second.size());
    for (const auto& c : it->second) std::printf("%s\n", c.c_str());
    return 0;
}

int DoDumpForced(const std::string& path) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(path, &perr);
    if (!pkg) {
        std::fprintf(stderr, "read failed: %s\n", perr.message.c_str());
        return 2;
    }
    auto children = pkg->ForcedExportPackages();
    std::printf("# %s: %zu forced-export package children\n",
                path.c_str(), children.size());
    for (const auto& c : children) std::printf("%s\n", c.c_str());
    return 0;
}

int DoDumpSummary(const std::string& path) {
    PackageReadError perr;
    auto s = PackageReader::ReadSummaryFile(path, &perr);
    if (!s) {
        std::fprintf(stderr, "read failed: %s\n", perr.message.c_str());
        return 2;
    }
    std::printf("# %s\n", path.c_str());
    std::printf("  version=%u/%u  total_header_size=%d\n",
                s->version, s->licensee_version, s->total_header_size);
    std::printf("  package_flags=0x%08X compression_flags=0x%08X\n",
                s->package_flags, s->compression_flags);
    std::printf("  names: count=%d off=0x%08X\n", s->name_count, s->name_offset);
    std::printf("  imports: count=%d off=0x%08X\n", s->import_count, s->import_offset);
    std::printf("  exports: count=%d off=0x%08X\n", s->export_count, s->export_offset);
    std::printf("  depends_off=0x%08X\n", s->depends_offset);
    std::printf("  generations: %zu\n", s->generations.size());
    for (size_t g = 0; g < s->generations.size(); ++g)
        std::printf("    gen %zu: exports=%d names=%d net_objects=%d\n", g + 1,
                    s->generations[g].export_count, s->generations[g].name_count,
                    s->generations[g].net_object_count);
    std::printf("  compressed_chunks: %zu\n", s->compressed_chunks.size());
    int n = std::min<int>(8, static_cast<int>(s->compressed_chunks.size()));
    for (int i = 0; i < n; ++i) {
        const auto& c = s->compressed_chunks[i];
        std::printf("    [%d] unc_off=0x%08X unc_size=0x%08X  cmp_off=0x%08X cmp_size=0x%08X\n",
                    i, c.uncompressed_offset, c.uncompressed_size,
                    c.compressed_offset, c.compressed_size);
    }
    if (s->compressed_chunks.size() > 8) {
        std::printf("    ...\n");
        const auto& c = s->compressed_chunks.back();
        std::printf("    [last %zu] unc_off=0x%08X unc_size=0x%08X  cmp_off=0x%08X cmp_size=0x%08X\n",
                    s->compressed_chunks.size() - 1,
                    c.uncompressed_offset, c.uncompressed_size,
                    c.compressed_offset, c.compressed_size);
    }
    return 0;
}

int DoDumpNames(const std::string& path, int limit) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(path, &perr);
    if (!pkg) { std::fprintf(stderr, "read failed: %s\n", perr.message.c_str()); return 2; }
    std::printf("# %s: %zu names\n", path.c_str(), pkg->names.size());
    int n = std::min<int>(limit, static_cast<int>(pkg->names.size()));
    for (int i = 0; i < n; ++i) std::printf("[%5d] %s\n", i, pkg->names[i].c_str());
    return 0;
}

int DoDumpImports(const std::string& path, int limit) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(path, &perr);
    if (!pkg) { std::fprintf(stderr, "read failed: %s\n", perr.message.c_str()); return 2; }
    std::printf("# %s: %zu imports\n", path.c_str(), pkg->imports.size());
    int n = std::min<int>(limit, static_cast<int>(pkg->imports.size()));
    for (int i = 0; i < n; ++i) {
        const auto& m = pkg->imports[i];
        std::printf("[%5d] cls_pkg=%-16s cls=%-16s outer=%-4d name=%s\n",
                    i, m.class_package.c_str(), m.class_name.c_str(),
                    m.outer_index, m.object_name.c_str());
    }
    return 0;
}

// Batch-dumps every Function export's raw on-disk bytecode bytes (hex),
// filtered to short functions (<=max_size bytes) so the output is a
// tractable corpus for opcode-remapping analysis (doc sec 19): AA3 appears
// to use a custom EExprToken->opcode-byte mapping, the same pattern
// UELib's existing EngineBranchAA2 (for the prior UE2-based America's
// Army 2) already documents for that engine. Bytecode starts exactly 44
// bytes into a Function export (NetIndex(4)+None-tag(8)+SuperStruct(4)+
// Next(4)+ScriptText(4)+Children(4)+CppText(4)+Line(4)+TextPos(4)+
// ScriptBytecodeSize(4) — UStructReader::ReadHeader's own layout for a
// non-class UStruct).
int DoDumpAllBytecode(const std::string& path, int max_size) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(path, &perr);
    if (!pkg) {
        std::fprintf(stderr, "read failed: %s\n", perr.message.c_str());
        return 2;
    }
    constexpr int kHeaderBytes = 44;
    int shown = 0, total_funcs = 0;
    for (size_t i = 0; i < pkg->exports.size(); ++i) {
        const auto& e = pkg->exports[i];
        // Same 3-way class-name resolution DoDumpExports uses (class_index<0
        // -> import, ==0 -> "Class" meta-entry, >0 -> local export) — we
        // want class=="Function" specifically, which can arrive via either
        // the import or local-export branch depending on the package.
        std::string cls_name;
        if (e.class_index < 0) {
            size_t imp_idx = static_cast<size_t>(-e.class_index - 1);
            if (imp_idx < pkg->imports.size()) cls_name = pkg->imports[imp_idx].object_name;
        } else if (e.class_index > 0) {
            size_t exp_idx2 = static_cast<size_t>(e.class_index - 1);
            if (exp_idx2 < pkg->exports.size()) cls_name = pkg->exports[exp_idx2].object_name;
        }
        if (cls_name != "Function") continue;
        ++total_funcs;

        std::string err;
        auto hdr = UStructReader::ReadHeader(*pkg, e, /*is_class_object=*/false,
                                             /*read_struct_fields=*/true, &err);
        if (!hdr) continue;
        if (hdr->script_bytecode_size <= 0 || hdr->script_bytecode_size > max_size) continue;

        int32_t body_off = e.serial_offset - pkg->body_base + kHeaderBytes;
        if (body_off < 0 || static_cast<size_t>(body_off) + hdr->script_bytecode_size > pkg->body.size()) continue;

        // Resolve the immediate outer (the class this function is a member
        // of), same 3-way convention as class-name resolution above, so
        // same-named functions across classes (Reset, PostBeginPlay, ...)
        // can be told apart.
        std::string outer_name;
        if (e.outer_index < 0) {
            size_t imp_idx = static_cast<size_t>(-e.outer_index - 1);
            if (imp_idx < pkg->imports.size()) outer_name = pkg->imports[imp_idx].object_name;
        } else if (e.outer_index > 0) {
            size_t exp_idx2 = static_cast<size_t>(e.outer_index - 1);
            if (exp_idx2 < pkg->exports.size()) outer_name = pkg->exports[exp_idx2].object_name;
        }

        // iNative is the WORD immediately following the bytecode blob
        // (UFunction::Serialize calls Super::Serialize — which writes the
        // bytecode — before serializing iNative itself; UnClass.cpp:2636-2643).
        // Read it here so native-function-call hypotheses can be checked
        // against each function's OWN real iNative value directly, instead
        // of trusting a possibly-renumbered `native(N)` source literal.
        int32_t inative_off = body_off + hdr->script_bytecode_size;
        int inative = -1;
        if (inative_off >= 0 && static_cast<size_t>(inative_off) + 2 <= pkg->body.size()) {
            inative = pkg->body[inative_off] | (pkg->body[inative_off + 1] << 8);
        }

        std::printf("%-24s %-40s size=%3d iNative=%5d  ", outer_name.c_str(), e.object_name.c_str(), hdr->script_bytecode_size, inative);
        for (int32_t b = 0; b < hdr->script_bytecode_size; ++b) {
            std::printf("%02x ", pkg->body[body_off + b]);
        }
        std::printf("\n");
        ++shown;
    }
    std::fprintf(stderr, "# %d Function exports total, %d shown (size<=%d)\n",
                 total_funcs, shown, max_size);
    return 0;
}

int DoDumpClass(const std::string& path, const std::string& class_name) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(path, &perr);
    if (!pkg) {
        std::fprintf(stderr, "read failed: %s\n", perr.message.c_str());
        return 2;
    }
    int32_t exp_idx = pkg->FindExport(class_name, "Class");
    if (exp_idx < 0) {
        std::fprintf(stderr, "no UClass export named '%s' in %s\n",
                     class_name.c_str(), path.c_str());
        return 2;
    }
    const auto& exp = pkg->exports[exp_idx];
    std::printf("# %s::%s (export #%d, serial=%d@%d)\n",
                path.c_str(), class_name.c_str(),
                exp_idx + 1, exp.serial_size, exp.serial_offset);
    std::string err;
    auto hdr = UStructReader::ReadHeader(*pkg, exp,
                                         /*is_class_object=*/true,
                                         /*read_struct_fields=*/true, &err);
    if (!hdr) {
        std::fprintf(stderr, "  UStructReader: %s\n", err.c_str());
        return 2;
    }
    std::printf("  NetIndex:      %d\n", hdr->net_index);
    std::printf("  SuperStruct:   %d  → %s\n",
                hdr->super_struct, pkg->ResolveIndex(hdr->super_struct).c_str());
    std::printf("  Children:      %d  → %s\n",
                hdr->children, pkg->ResolveIndex(hdr->children).c_str());
    std::printf("  Line: %d  ScriptBytecodeSize: %d\n",
                hdr->line, hdr->script_bytecode_size);

    // Helpers to classify a child field by its import-resolved class name.
    auto class_of = [&](const ObjectExport& e) -> std::string {
        if (e.class_index < 0) {
            size_t i = static_cast<size_t>(-e.class_index - 1);
            return i < pkg->imports.size() ? pkg->imports[i].object_name : "";
        }
        if (e.class_index == 0) return "Class";
        size_t i = static_cast<size_t>(e.class_index - 1);
        return i < pkg->exports.size() ? pkg->exports[i].object_name : "";
    };
    auto is_property   = [](const std::string& c) {
        return c.size() > 8 && c.compare(c.size() - 8, 8, "Property") == 0;
    };
    auto is_struct_sub = [](const std::string& c) {
        return c == "Function" || c == "State" || c == "ScriptStruct";
    };

    // Read a UProperty's ArrayDim / PropertyFlags / RepOffset from its serial
    // body. Layout (v568, PC): NetIndex(4) None-tag(8) SuperField(4) Next(4)
    //   ArrayDim(4) PropertyFlags(8) Category(8) ArraySizeEnum(4)
    //   [RepOffset(2) iff CPF_Net]. Returns false if the range is short.
    constexpr uint64_t CPF_Net = 0x20;
    auto read_prop = [&](const ObjectExport& e, int32_t* array_dim,
                         uint64_t* prop_flags, uint16_t* rep_offset) -> bool {
        int32_t base = e.serial_offset - pkg->body_base;
        if (base < 0 || static_cast<size_t>(base) + 30 > pkg->body.size())
            return false;
        const uint8_t* p = pkg->body.data() + base;
        std::memcpy(array_dim,  p + 20, 4);
        std::memcpy(prop_flags, p + 24, 8);
        *rep_offset = 0;
        if ((*prop_flags & CPF_Net) &&
            static_cast<size_t>(base) + 46 <= pkg->body.size())
            std::memcpy(rep_offset, p + 44, 2);
        return true;
    };

    // Walk Children → Next. This is exactly UE3's TFieldIterator<UField> order,
    // which UClass::Link uses to assign RepIndex to CPF_Net properties whose
    // Outer is this class (UnClass.cpp:2128-2142). We reproduce that here.
    std::printf("\n  Field walk (Children→Next) — replicated props flagged [NET ...]:\n");
    std::vector<std::string> net_props;
    int32_t cur = hdr->children;
    int safety = 0;
    while (cur > 0 && safety++ < 4096) {
        size_t idx = static_cast<size_t>(cur - 1);
        if (idx >= pkg->exports.size()) { std::printf("    [bad index %d]\n", cur); break; }
        const auto& ce = pkg->exports[idx];
        const std::string fc = class_of(ce);

        std::string tag;
        if (is_property(fc)) {
            int32_t ad = 1; uint64_t pf = 0; uint16_t ro = 0;
            if (read_prop(ce, &ad, &pf, &ro)) {
                if (pf & CPF_Net) {
                    char buf[80];
                    // repoff = UE3 RepOffset (bytecode offset of the replication
                    // condition); props sharing a condition share it. NOT RepIndex
                    // — RepIndex is this property's ordinal in the walk below.
                    std::snprintf(buf, sizeof buf, "  [NET repoff=%u arrdim=%d flags=0x%llx]",
                                  ro, ad, static_cast<unsigned long long>(pf));
                    tag = buf;
                    net_props.push_back(ce.object_name);
                }
            }
        }
        std::printf("    %-6d %-16s %s%s\n", cur, fc.c_str(),
                    ce.object_name.c_str(), tag.c_str());

        std::string werr;
        auto ch = UStructReader::ReadHeader(*pkg, ce, /*is_class_object=*/false,
                                            /*read_struct_fields=*/is_struct_sub(fc), &werr);
        if (!ch) { std::printf("    (parse stopped: %s)\n", werr.c_str()); break; }
        cur = ch->next;
    }
    if (safety >= 4096) std::printf("    (truncated at 4096 fields)\n");

    std::printf("\n  This class's CPF_Net properties in RepIndex order (%zu):\n",
                net_props.size());
    for (size_t i = 0; i < net_props.size(); ++i)
        std::printf("    +%zu  %s\n", i, net_props[i].c_str());
    return 0;
}

// ---- Inheritance-chained net-field layout (FClassNetCache reconstruction) ----
//
// Reproduces UE3's on-wire field index. Per UnClass.cpp:2121 (UClass::Link) each
// class builds NetFields = its own CPF_Net properties + FUNC_Net functions with
// no super function, SORTED ascending by NetIndex. Per UnCoreNet.cpp:137
// (GetClassNetCache) these are layered base-class-first: a class's FieldsBase =
// super->GetMaxIndex(), and each field's wire index (FieldNetIndex, the value
// the sender writes via Bunch.WriteIntWrapped, UnChan.cpp:1952) is
// FieldsBase + its position in the sorted per-class NetFields. MaxIndex =
// FieldsBase + Fields.Num() of the most-derived class — the sentinel bound.

constexpr uint64_t kCPF_Net  = 0x20;
constexpr uint64_t kCPF_OptionalParm = 0x10;
constexpr uint64_t kCPF_Parm         = 0x80;
constexpr uint64_t kCPF_ReturnParm   = 0x400;
// AA3 SHUFFLES the FUNC_* bit assignments relative to stock UE3 (UnStack.h
// says FUNC_Net=0x40, NetReliable=0x80, NetServer=0x200000, NetClient=
// 0x1000000). Derived empirically (doc sec 19) from the one structural
// signal that doesn't depend on flag semantics: UFunction::Serialize writes
// RepOffset(2) iff FUNC_Net, so a function "is net" iff that extra field is
// in its export tail. Over 3172 functions in AA3Baseline+Engine, bit 0x100
// predicts RepOffset presence perfectly (523/523, 0 false positives); 0x200
// and 0x4000 partition those 523 exactly (266 + 257) and match Server*/
// Client* naming; 0x1 is clear on exactly the functions stock UE3 declares
// `unreliable` (ServerMove, ClientAdjustPosition, ServerSay, ...).
// The old 0x40 test silently dropped ~88% of net functions (ServerMove,
// ServerSay, ClientAdjustPosition, ...) and so shifted every RepIndex after
// the first missed one.
constexpr uint32_t kFUNC_Net         = 0x00000100;
constexpr uint32_t kFUNC_NetReliable = 0x00000001;
constexpr uint32_t kFUNC_NetServer   = 0x00000200;
constexpr uint32_t kFUNC_NetClient   = 0x00004000;

std::string LowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

std::string ClassNameOf(const Package& pkg, const ObjectExport& e) {
    if (e.class_index == 0) return "Class";
    if (e.class_index < 0) {
        size_t i = static_cast<size_t>(-e.class_index - 1);
        return i < pkg.imports.size() ? pkg.imports[i].object_name : "";
    }
    size_t i = static_cast<size_t>(e.class_index - 1);
    return i < pkg.exports.size() ? pkg.exports[i].object_name : "";
}

// Follow an import's outer_index up to the top-level Package import (outer==0),
// whose object_name is the home package (e.g. Object → "Core").
std::string ImportHomePackage(const Package& pkg, int32_t import_idx) {
    int32_t idx = import_idx;
    for (int guard = 0; guard < 64; ++guard) {
        if (idx < 0 || static_cast<size_t>(idx) >= pkg.imports.size()) return "";
        const auto& im = pkg.imports[idx];
        if (im.outer_index == 0) return im.object_name;  // top-level package import
        if (im.outer_index > 0)  return "";              // outer of an import is always an import
        idx = -im.outer_index - 1;
    }
    return "";
}

// On-demand package loader keyed by lowercase package name.
struct PkgCache {
    std::string dir;
    std::map<std::string, std::shared_ptr<Package>> cache;
    std::shared_ptr<Package> Get(const std::string& name) {
        std::string key = LowerStr(name);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        std::shared_ptr<Package> p;
        for (const char* ext : {".u", ".upk"}) {
            PackageReadError e;
            auto pk = PackageReader::ReadFile(dir + "/" + name + ext, &e);
            if (pk) { p = std::make_shared<Package>(std::move(*pk)); break; }
        }
        cache[key] = p;
        return p;
    }
};

struct NetField {
    std::string decl_class;
    std::string name;
    std::string type;
    bool        is_property = false;
    int32_t     net_index   = 0;
    int32_t     array_dim   = 1;
    uint64_t    prop_flags  = 0;
    uint16_t    rep_offset  = 0;
    int32_t     field_net_index = 0;  // assigned wire index
    std::string sub_type;             // ByteProperty's Enum / StructProperty's Struct name, if resolved
    uint32_t    func_flags = 0;       // Function only: raw FunctionFlags (0 for properties)
    int32_t     export_index = -1;    // 0-based position in decl_class's package export table
};

// Bare object name (no "(import #N)"/"(export #N)" suffix) for a raw
// FPackageIndex, resolved within `pkg`. Empty on failure.
std::string RawIndexName(const Package& pkg, int32_t idx) {
    if (idx == 0) return "";
    if (idx > 0) {
        size_t i = static_cast<size_t>(idx - 1);
        return i < pkg.exports.size() ? pkg.exports[i].object_name : "";
    }
    size_t i = static_cast<size_t>(-idx - 1);
    return i < pkg.imports.size() ? pkg.imports[i].object_name : "";
}

bool ReadPropWord(const Package& pkg, const ObjectExport& e, size_t rel, void* dst, size_t n) {
    int32_t base = e.serial_offset - pkg.body_base;
    if (base < 0 || static_cast<size_t>(base) + rel + n > pkg.body.size()) return false;
    std::memcpy(dst, pkg.body.data() + base + rel, n);
    return true;
}

// Gather one class's own net fields (properties + functions) via Children→Next.
// Appends to `out`. Uses the class's own package.
void CollectClassNetFields(const Package& pkg, int32_t class_exp_idx,
                           std::vector<NetField>* out) {
    const auto& cls = pkg.exports[class_exp_idx];
    std::string werr;
    auto chdr = UStructReader::ReadHeader(pkg, cls, /*is_class_object=*/true,
                                          /*read_struct_fields=*/true, &werr);
    if (!chdr) return;
    const std::string cls_name = cls.object_name;

    int32_t cur = chdr->children;
    int safety = 0;
    while (cur > 0 && safety++ < 8192) {
        size_t idx = static_cast<size_t>(cur - 1);
        if (idx >= pkg.exports.size()) break;
        const auto& ce = pkg.exports[idx];
        const std::string fc = ClassNameOf(pkg, ce);
        const bool is_prop = fc.size() > 8 && fc.compare(fc.size() - 8, 8, "Property") == 0;
        const bool is_func = fc == "Function";
        const bool is_struct_sub = is_func || fc == "State" || fc == "ScriptStruct";

        auto ch = UStructReader::ReadHeader(pkg, ce, /*is_class_object=*/false,
                                            /*read_struct_fields=*/is_struct_sub, &werr);
        if (!ch) break;

        if (is_prop) {
            // Layout: NetIndex(4) None(8) Super(4) Next(4) ArrayDim(4) PropertyFlags(8)...
            int32_t ad = 1; uint64_t pf = 0; uint16_t ro = 0;
            if (ReadPropWord(pkg, ce, 20, &ad, 4) && ReadPropWord(pkg, ce, 24, &pf, 8)) {
                if (pf & kCPF_Net) {
                    ReadPropWord(pkg, ce, 44, &ro, 2);
                    NetField f;
                    f.decl_class = cls_name; f.name = ce.object_name; f.type = fc;
                    f.is_property = true; f.net_index = ch->net_index;
                    f.array_dim = ad; f.prop_flags = pf; f.rep_offset = ro;
                    // UByteProperty::Serialize / UStructProperty::Serialize both
                    // append one trailing FPackageIndex (Enum / Struct resp.)
                    // right after RepOffset (UnProp.cpp:665, 4289).
                    if (fc == "ByteProperty" || fc == "StructProperty") {
                        int32_t sub_idx = 0;
                        if (ReadPropWord(pkg, ce, 46, &sub_idx, 4))
                            f.sub_type = RawIndexName(pkg, sub_idx);
                    }
                    out->push_back(std::move(f));
                }
            }
        } else if (is_func) {
            // Net function = (FunctionFlags & FUNC_Net) && no super function
            // (ch->super_struct==0). The on-disk bytecode is token-serialized so
            // its length != ScriptBytecodeSize; we can't skip forward to
            // FunctionFlags. But UFunction::Serialize (UnClass.cpp:2636) writes
            // FunctionFlags(4) [+ RepOffset(2) iff FUNC_Net] LAST, so read from
            // the end. Tail (PC, non-console): ...FunctionFlags(4) [RepOffset(2)
            // iff FUNC_Net] FriendlyName(FName, 8). So FunctionFlags is at
            // size-12 (not-net) or size-14 (net). Disambiguate with a validity
            // mask of all defined FUNC_ bits.
            // Net iff the word at size-14 carries kFUNC_Net plus a direction
            // bit AND the word at size-12 (which would be the flags if there
            // were no RepOffset) does not -- see kFUNC_Net above. The
            // direction-bit requirement rejects the lone false positive seen
            // (Object.SubtractEqual_StrStr, whose script tail bytes set 0x100).
            uint32_t ffA = 0, ffB = 0;
            size_t sz = static_cast<size_t>(ce.serial_size);
            bool got = sz >= 14 &&
                       ReadPropWord(pkg, ce, sz - 12, &ffA, 4) &&
                       ReadPropWord(pkg, ce, sz - 14, &ffB, 4);
            uint32_t ff = 0; bool is_net = false;
            if (got) {
                is_net = (ffB & kFUNC_Net) && (ffB & (kFUNC_NetServer | kFUNC_NetClient)) &&
                         !(ffA & kFUNC_Net);
                ff = is_net ? ffB : ffA;
            }
            if (is_net && ch->super_struct == 0) {
                NetField f;
                f.decl_class = cls_name; f.name = ce.object_name; f.type = fc;
                f.is_property = false; f.net_index = ch->net_index;
                f.func_flags = ff; f.export_index = static_cast<int32_t>(idx);
                out->push_back(std::move(f));
            }
        }
        cur = ch->next;
    }
}

// ---- Recursive struct-field layout (for custom AA3 StructProperty types) ----
//
// UStructProperty::NetSerializeItem (UnProp.cpp:4192-4283): five native struct
// types (Vector, Rotator, Quat, Plane, UniqueNetId) get a hand-written compact
// wire format; every OTHER struct type falls into the else-branch, which walks
// `TFieldIterator<UProperty>(Struct)` -- i.e. Children->Next, same traversal as
// a class -- and calls NetSerializeItem on EVERY property found, with NO CPF_Net
// filter and NO RepIndex/FieldNetIndex header per field (positional, since both
// ends already agree on the struct's exact layout). UArrayProperty::NetSerializeItem
// is a hard no-op (UnProp.cpp:3365-3368) -- a TArray member contributes zero bits.

const std::set<std::string>& NativeStructNames() {
    static const std::set<std::string> kNative = {"Vector", "Rotator", "Quat", "Plane", "UniqueNetId"};
    return kNative;
}

struct StructField {
    std::string name;
    std::string type;      // e.g. "IntProperty", "StructProperty", "ArrayProperty"
    std::string sub_type;  // Enum name (ByteProperty) or Struct name (StructProperty)
    int32_t     array_dim = 1;
    int         depth = 0;
    uint64_t    prop_flags = 0;  // raw PropertyFlags (0 for synthetic rows)
};

// Resolve a struct-typed field's home package (analogous to ImportHomePackage,
// but the reference may be an export in the SAME package too).
std::shared_ptr<Package> ResolveStructHome(PkgCache& cache, const std::shared_ptr<Package>& pkg,
                                           int32_t sub_idx, int32_t* out_exp_idx) {
    if (sub_idx > 0) {
        *out_exp_idx = sub_idx - 1;
        return pkg;
    }
    if (sub_idx == 0) return nullptr;
    int32_t imp = -sub_idx - 1;
    if (imp < 0 || static_cast<size_t>(imp) >= pkg->imports.size()) return nullptr;
    std::string name = pkg->imports[imp].object_name;
    std::string home = ImportHomePackage(*pkg, imp);
    auto next_pkg = home.empty() ? nullptr : cache.Get(home);
    if (!next_pkg) return nullptr;
    int32_t idx = next_pkg->FindExport(name, "ScriptStruct");
    if (idx < 0) return nullptr;
    *out_exp_idx = idx;
    return next_pkg;
}

void CollectStructFields(PkgCache& cache, const std::shared_ptr<Package>& pkg, int32_t struct_exp_idx,
                         const std::string& prefix, int depth, std::vector<StructField>* out) {
    if (depth > 6) { out->push_back({prefix, "<max-depth>", "", 1, depth}); return; }
    const auto& se = pkg->exports[struct_exp_idx];
    std::string werr;
    auto shdr = UStructReader::ReadHeader(*pkg, se, /*is_class_object=*/false,
                                          /*read_struct_fields=*/true, &werr);
    if (!shdr) { out->push_back({prefix, "<unreadable:" + werr + ">", "", 1, depth}); return; }

    int32_t cur = shdr->children;
    int safety = 0;
    while (cur > 0 && safety++ < 4096) {
        size_t idx = static_cast<size_t>(cur - 1);
        if (idx >= pkg->exports.size()) break;
        const auto& ce = pkg->exports[idx];
        std::string fc = ClassNameOf(*pkg, ce);
        bool is_prop = fc.size() > 8 && fc.compare(fc.size() - 8, 8, "Property") == 0;
        bool is_struct_sub = (fc == "Function" || fc == "State" || fc == "ScriptStruct");

        auto ch = UStructReader::ReadHeader(*pkg, ce, /*is_class_object=*/false,
                                            /*read_struct_fields=*/is_struct_sub, &werr);
        if (!ch) break;

        if (is_prop) {
            int32_t ad = 1; uint64_t pf = 0;
            if (ReadPropWord(*pkg, ce, 20, &ad, 4) &&
                ReadPropWord(*pkg, ce, 24, &pf, 8)) {
                // RepOffset(2B) is present iff CPF_Net -- unlike CollectClassNetFields,
                // struct members are NOT filtered by net-ness, so this varies per field.
                size_t sub_off = (pf & kCPF_Net) ? 46 : 44;
                std::string sub;
                if (fc == "ByteProperty" || fc == "StructProperty") {
                    int32_t sub_idx = 0;
                    if (ReadPropWord(*pkg, ce, sub_off, &sub_idx, 4))
                        sub = RawIndexName(*pkg, sub_idx);
                }
                std::string qual = prefix + "." + ce.object_name;
                out->push_back({qual, fc, sub, ad, depth, pf});
                if (fc == "StructProperty" && !sub.empty() && !NativeStructNames().count(sub)) {
                    int32_t sub_idx = 0;
                    ReadPropWord(*pkg, ce, sub_off, &sub_idx, 4);
                    int32_t next_exp = -1;
                    auto next_pkg = ResolveStructHome(cache, pkg, sub_idx, &next_exp);
                    if (next_pkg && next_exp >= 0)
                        CollectStructFields(cache, next_pkg, next_exp, qual, depth + 1, out);
                    else
                        out->push_back({qual, "<struct-unresolved:" + sub + ">", "", 1, depth + 1});
                }
            }
        }
        cur = ch->next;
    }
}

int DoStructLayout(const std::string& path, const std::string& struct_name,
                   const std::string& pkg_dir_in) {
    std::string pkg_dir = pkg_dir_in;
    if (pkg_dir.empty()) {
        size_t slash = path.find_last_of('/');
        pkg_dir = slash == std::string::npos ? "." : path.substr(0, slash);
    }
    PkgCache cache; cache.dir = pkg_dir;
    PackageReadError perr;
    auto start = PackageReader::ReadFile(path, &perr);
    if (!start) { std::fprintf(stderr, "read failed: %s\n", perr.message.c_str()); return 2; }
    auto start_sp = std::make_shared<Package>(std::move(*start));
    // Accept a Function export too, not just ScriptStruct: a UFunction IS a
    // UStruct (its Children ARE its parameters, each a UProperty with
    // CPF_Parm), and CollectStructFields already recurses into Function
    // exports for nested struct-typed fields (is_struct_sub check above) --
    // this just lets the *entry point* be a function's own parameter list,
    // independent of the UELib-based decompiler that mangles AA3's custom
    // bytecode. The parameter LIST comes from raw UProperty child metadata,
    // not bytecode, so it's unaffected by the corruption that breaks
    // decompiled function bodies -- see docs/AA3_ACTOR_REPLICATION.md sec 18.
    int32_t idx = start_sp->FindExport(struct_name, "ScriptStruct");
    if (idx < 0) idx = start_sp->FindExport(struct_name, "Function");
    if (idx < 0) {
        std::fprintf(stderr, "no ScriptStruct or Function '%s' in %s\n", struct_name.c_str(), path.c_str());
        return 2;
    }
    std::vector<StructField> fields;
    CollectStructFields(cache, start_sp, idx, struct_name, 0, &fields);
    // A Function's Children are its parameters AND its locals (and return
    // value). Only parameters go on the wire, so drop the rest -- plus any
    // nested struct rows under a dropped field. Property flags are NOT
    // shuffled in AA3 (unlike FUNC_*, doc sec 19): CPF_Parm=0x80,
    // OptionalParm=0x10, ReturnParm=0x400 verified on ServerMove (6 params
    // + 10 locals, matching PlayerController.uc exactly), ClientMessage,
    // GetAdjustedAimFor (doc sec 20).
    const bool is_function = start_sp->exports[idx].object_name == struct_name &&
                             ClassNameOf(*start_sp, start_sp->exports[idx]) == "Function";
    if (is_function) {
        std::vector<StructField> params;
        bool keep = false;
        for (const auto& f : fields) {
            if (f.depth == 0)
                keep = (f.prop_flags & kCPF_Parm) && !(f.prop_flags & kCPF_ReturnParm);
            if (keep) params.push_back(f);
        }
        fields.swap(params);
    }
    std::printf("# %s for %s (%zu fields, recursive)\n\n",
                is_function ? "Parameter list" : "Struct layout", struct_name.c_str(), fields.size());
    for (const auto& f : fields) {
        std::printf("  [%d] %-40s %-16s %-16s pf=0x%016llx%s\n", f.depth, f.name.c_str(),
                    f.type.c_str(), f.sub_type.c_str(),
                    static_cast<unsigned long long>(f.prop_flags),
                    f.array_dim > 1 ? (" [" + std::to_string(f.array_dim) + "]").c_str() : "");
        if (is_function && f.depth == 0 && (f.prop_flags & kCPF_OptionalParm))
            std::printf("        ^ optional\n");
    }
    return 0;
}

int DoRepLayout(const std::string& path, const std::string& class_name,
                const std::string& pkg_dir_in) {
    std::string pkg_dir = pkg_dir_in;
    if (pkg_dir.empty()) {
        size_t slash = path.find_last_of('/');
        pkg_dir = slash == std::string::npos ? "." : path.substr(0, slash);
    }
    PkgCache cache; cache.dir = pkg_dir;

    // Load the start package into the cache too so the chain shares instances.
    PackageReadError perr;
    auto start = PackageReader::ReadFile(path, &perr);
    if (!start) { std::fprintf(stderr, "read failed: %s\n", perr.message.c_str()); return 2; }
    auto start_sp = std::make_shared<Package>(std::move(*start));

    // Build the inheritance chain derived→base by following SuperStruct, then
    // reverse to base→derived (the order GetClassNetCache layers indices in).
    struct Link { std::shared_ptr<Package> pkg; int32_t exp_idx; };
    std::vector<Link> chain;
    std::shared_ptr<Package> cur_pkg = start_sp;
    int32_t cur_idx = cur_pkg->FindExport(class_name, "Class");
    if (cur_idx < 0) {
        std::fprintf(stderr, "no UClass '%s' in %s\n", class_name.c_str(), path.c_str());
        return 2;
    }
    for (int guard = 0; guard < 64 && cur_idx >= 0; ++guard) {
        const auto& e = cur_pkg->exports[cur_idx];
        std::string werr;
        auto h = UStructReader::ReadHeader(*cur_pkg, e, true, true, &werr);
        if (!h) { std::fprintf(stderr, "header parse failed for %s: %s\n",
                               e.object_name.c_str(), werr.c_str()); break; }
        chain.push_back({cur_pkg, cur_idx});
        int32_t super = h->super_struct;
        if (super == 0) break;                       // reached Object (root)
        if (super > 0) { cur_idx = super - 1; continue; }
        // Cross-package: resolve the imported super class's home package.
        int32_t imp = -super - 1;
        if (imp < 0 || static_cast<size_t>(imp) >= cur_pkg->imports.size()) break;
        std::string super_name = cur_pkg->imports[imp].object_name;
        std::string home = ImportHomePackage(*cur_pkg, imp);
        auto next_pkg = home.empty() ? nullptr : cache.Get(home);
        if (!next_pkg) {
            std::fprintf(stderr, "! chain stops: super '%s' in package '%s' not found under %s\n",
                         super_name.c_str(), home.c_str(), pkg_dir.c_str());
            break;
        }
        int32_t next_idx = next_pkg->FindExport(super_name, "Class");
        if (next_idx < 0) {
            std::fprintf(stderr, "! chain stops: class '%s' not in %s\n",
                         super_name.c_str(), home.c_str());
            break;
        }
        cur_pkg = next_pkg; cur_idx = next_idx;
    }
    std::reverse(chain.begin(), chain.end());

    std::printf("# Rep layout for %s (chain base→derived, %zu classes)\n",
                class_name.c_str(), chain.size());
    std::printf("# pkg-dir=%s\n\n", pkg_dir.c_str());

    int32_t field_base = 0;
    std::vector<NetField> flat;
    for (const auto& lk : chain) {
        const auto& cls = lk.pkg->exports[lk.exp_idx];
        std::vector<NetField> own;
        CollectClassNetFields(*lk.pkg, lk.exp_idx, &own);
        // UClass::Link sorts NetFields ascending by NetIndex.
        std::stable_sort(own.begin(), own.end(),
            [](const NetField& a, const NetField& b){ return a.net_index < b.net_index; });
        int32_t this_base = field_base;
        int nprop = 0, nfunc = 0;
        for (auto& f : own) {
            f.field_net_index = field_base++;
            (f.is_property ? nprop : nfunc)++;
            flat.push_back(f);
        }
        std::printf("  %-24s FieldsBase=%-4d fields=%zu (%d prop, %d func)\n",
                    cls.object_name.c_str(), this_base, own.size(), nprop, nfunc);
    }
    int32_t max_index = field_base;

    std::printf("\n  Flattened wire fields (FieldNetIndex → field), MaxIndex=%d:\n", max_index);
    for (const auto& f : flat) {
        if (f.is_property)
            std::printf("    %4d  %-22s %-16s %s.%s%s\n", f.field_net_index,
                        f.type.c_str(), f.sub_type.c_str(), f.decl_class.c_str(), f.name.c_str(),
                        f.array_dim > 1 ? "[]" : "");
        else
            // Direction/reliability from AA3's shuffled FUNC_* bits (see
            // kFUNC_Net). Raw word printed too for anyone re-checking.
            std::printf("    %4d  %-22s %-16s %s.%s()  %s %s  ff=0x%08x exp=%d\n",
                        f.field_net_index, "Function", "", f.decl_class.c_str(),
                        f.name.c_str(),
                        (f.func_flags & kFUNC_NetServer) ? "server" : "client",
                        (f.func_flags & kFUNC_NetReliable) ? "reliable" : "unreliable",
                        f.func_flags, f.export_index);
    }
    std::printf("\n  MaxIndex=%d  (WriteIntWrapped bound / end-of-bunch sentinel value)\n",
                max_index);
    return 0;
}

int DoDumpBodyHex(const std::string& path, uint32_t at, int len) {
    // Use ReadFile so that already-decompressed "clean" packages (LZO+XOR
    // stripped by the pre-clean pipeline, flag bit still set, no chunk table)
    // are handled the same as genuinely chunk-compressed ones.
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(path, &perr);
    if (!pkg) {
        std::fprintf(stderr, "read failed: %s\n", perr.message.c_str());
        return 2;
    }
    const std::vector<uint8_t>& body = pkg->body;
    int32_t base = pkg->body_base;
    if (static_cast<int32_t>(at) < base ||
        static_cast<size_t>(at - base) >= body.size()) {
        std::fprintf(stderr, "offset 0x%X outside body [base=0x%X size=0x%zX]\n",
                     at, base, body.size());
        return 2;
    }
    size_t off = at - base;
    size_t to_show = std::min<size_t>(len, body.size() - off);
    std::printf("# %s @ uncompressed 0x%08X (body[0x%zX], total body=0x%zX)\n",
                path.c_str(), at, off, body.size());
    for (size_t i = 0; i < to_show; i += 16) {
        std::printf("%08zX  ", off + i);
        for (size_t j = 0; j < 16 && i + j < to_show; ++j)
            std::printf("%02x ", body[off + i + j]);
        std::printf(" ");
        for (size_t j = 0; j < 16 && i + j < to_show; ++j) {
            uint8_t b = body[off + i + j];
            std::printf("%c", (b >= 32 && b < 127) ? b : '.');
        }
        std::printf("\n");
    }
    return 0;
}

int DoDumpExports(const std::string& path, int limit) {
    PackageReadError perr;
    auto pkg = PackageReader::ReadFile(path, &perr);
    if (!pkg) {
        std::fprintf(stderr, "read failed: %s\n", perr.message.c_str());
        return 2;
    }
    std::printf("# %s: %zu names, %zu imports, %zu exports parsed\n",
                path.c_str(), pkg->names.size(), pkg->imports.size(),
                pkg->exports.size());
    int n = std::min<int>(limit, static_cast<int>(pkg->exports.size()));
    for (int i = 0; i < n; ++i) {
        const auto& e = pkg->exports[i];
        std::string class_name = "?";
        if (e.class_index < 0) {
            size_t imp_idx = static_cast<size_t>(-e.class_index - 1);
            if (imp_idx < pkg->imports.size())
                class_name = pkg->imports[imp_idx].object_name;
        } else if (e.class_index == 0) {
            class_name = "Class";
        } else if (static_cast<size_t>(e.class_index - 1) < pkg->exports.size()) {
            class_name = pkg->exports[e.class_index - 1].object_name;
        }
        std::string gen_counts;
        for (int32_t c : e.generation_net_object_count)
            gen_counts += (gen_counts.empty() ? "" : ",") + std::to_string(c);
        std::printf("[%4d] class=%-24s name=%-32s outer=%d serial=(%d@%d) "
                    "flags=0x%016llx exp_flags=0x%08x net=%d%s%s arch=%d\n",
                    i, class_name.c_str(), e.object_name.c_str(),
                    e.outer_index, e.serial_size, e.serial_offset,
                    static_cast<unsigned long long>(e.object_flags),
                    e.export_flags, e.net_object_count,
                    gen_counts.empty() ? "" : " gen_net_objects=", gen_counts.c_str(),
                    e.archetype_index);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string maps_root, out_dir, single, wire, dump_forced, dump_exports, dump_summary, cooked_root;
    int dump_limit = 8;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto take = [&](std::string* dst) -> bool {
            if (i + 1 >= argc) return false;
            *dst = argv[++i];
            return true;
        };
        if      (a == "--maps-root")     { if (!take(&maps_root))    return Usage(1); }
        else if (a == "--out-dir")       { if (!take(&out_dir))      return Usage(1); }
        else if (a == "--single")        { if (!take(&single))       return Usage(1); }
        else if (a == "--wire")          { if (!take(&wire))         return Usage(1); }
        else if (a == "--dump-forced")   { if (!take(&dump_forced))  return Usage(1); }
        else if (a == "--dump-exports")  { if (!take(&dump_exports)) return Usage(1); }
        else if (a == "--dump-names")    { std::string v; if (!take(&v)) return Usage(1); return DoDumpNames(v, dump_limit); }
        else if (a == "--dump-imports")  { std::string v; if (!take(&v)) return Usage(1); return DoDumpImports(v, dump_limit); }
        else if (a == "--dump-universe") {
            std::string root, parent;
            if (!take(&root)) return Usage(1);
            if (i + 1 < argc && argv[i+1][0] != '-') parent = argv[++i];
            return DoDumpUniverse(root, parent);
        }
        else if (a == "--dump-body-hex") {
            std::string pth, at_s, len_s;
            if (!take(&pth) || !take(&at_s) || !take(&len_s)) return Usage(1);
            return DoDumpBodyHex(pth, std::stoul(at_s, nullptr, 0),
                                 std::stoi(len_s, nullptr, 0));
        }
        else if (a == "--dump-summary")  { if (!take(&dump_summary)) return Usage(1); }
        else if (a == "--dump-class")    {
            std::string pkg_path, class_name;
            if (!take(&pkg_path) || !take(&class_name)) return Usage(1);
            return DoDumpClass(pkg_path, class_name);
        }
        else if (a == "--dump-all-bytecode") {
            std::string pkg_path, size_s;
            if (!take(&pkg_path) || !take(&size_s)) return Usage(1);
            return DoDumpAllBytecode(pkg_path, std::stoi(size_s));
        }
        else if (a == "--rep-layout")    {
            // --rep-layout <pkg.u> <ClassName> [--pkg-dir <dir>]
            std::string pkg_path, class_name, pkg_dir;
            if (!take(&pkg_path) || !take(&class_name)) return Usage(1);
            if (i + 2 < argc && std::string(argv[i+1]) == "--pkg-dir") {
                i++; pkg_dir = argv[++i];
            }
            return DoRepLayout(pkg_path, class_name, pkg_dir);
        }
        else if (a == "--struct-layout") {
            // --struct-layout <pkg.u> <StructName> [--pkg-dir <dir>]
            std::string pkg_path, struct_name, pkg_dir;
            if (!take(&pkg_path) || !take(&struct_name)) return Usage(1);
            if (i + 2 < argc && std::string(argv[i+1]) == "--pkg-dir") {
                i++; pkg_dir = argv[++i];
            }
            return DoStructLayout(pkg_path, struct_name, pkg_dir);
        }
        else if (a == "--roundtrip")     { std::string v; if (!take(&v)) return Usage(1); return DoRoundtrip(v); }
        else if (a == "--limit")         { std::string v; if (!take(&v)) return Usage(1); dump_limit = std::stoi(v); }
        else if (a == "--cooked-root")   { if (!take(&cooked_root))  return Usage(1); }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return Usage(1); }
    }
    if (!dump_forced.empty())                return DoDumpForced(dump_forced);
    if (!dump_exports.empty())               return DoDumpExports(dump_exports, dump_limit);
    if (!dump_summary.empty())               return DoDumpSummary(dump_summary);
    if (!wire.empty())                       return DoWire(wire, cooked_root);
    if (!single.empty()) {
        if (out_dir.empty()) out_dir = ".";
        return DoSingle(single, out_dir, cooked_root);
    }
    if (!maps_root.empty() && !out_dir.empty())
        return DoBatch(maps_root, out_dir, cooked_root);
    return Usage(1);
}
