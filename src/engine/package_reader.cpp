#include "engine/package_reader.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lzo/lzo1x.h>
#include <lzo/lzoconf.h>

namespace aa3 {
namespace engine {

namespace {

// One-time LZO initialization. Safe to call repeatedly.
bool EnsureLzoInit() {
    static int init_result = []() { return lzo_init(); }();
    return init_result == LZO_E_OK;
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool Skip(size_t n) {
        if (pos_ + n > size_) return false;
        pos_ += n;
        return true;
    }
    bool Seek(size_t off) {
        if (off > size_) return false;
        pos_ = off;
        return true;
    }
    size_t Pos() const { return pos_; }
    size_t Remaining() const { return size_ - pos_; }

    bool U32(uint32_t* v) {
        if (Remaining() < 4) return false;
        std::memcpy(v, data_ + pos_, 4);
        pos_ += 4;
        return true;
    }
    bool I32(int32_t* v) {
        return U32(reinterpret_cast<uint32_t*>(v));
    }
    bool Bytes(void* dst, size_t n) {
        if (Remaining() < n) return false;
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return true;
    }

    // UE3 FString: int32 length.
    //   length > 0  -> ANSI; `length` bytes including trailing NUL
    //   length < 0  -> UTF-16LE; -length code units including trailing NUL
    //   length == 0 -> empty
    bool FString(std::string* out) {
        int32_t length = 0;
        if (!I32(&length)) return false;
        out->clear();
        if (length == 0) return true;
        if (length > 0) {
            if (Remaining() < static_cast<size_t>(length)) return false;
            const char* s = reinterpret_cast<const char*>(data_ + pos_);
            size_t n = static_cast<size_t>(length);
            while (n > 0 && s[n - 1] == '\0') --n;
            out->assign(s, n);
            pos_ += length;
            return true;
        }
        // Unicode.
        int32_t nchars = -length;
        size_t bytes = static_cast<size_t>(nchars) * 2;
        if (Remaining() < bytes) return false;
        // Lossy ASCII flatten — sufficient for engine names, which are 7-bit.
        for (int32_t i = 0; i < nchars - 1; ++i) {
            uint16_t cu;
            std::memcpy(&cu, data_ + pos_ + i * 2, 2);
            out->push_back(static_cast<char>(cu & 0xFF));
        }
        pos_ += bytes;
        return true;
    }

private:
    const uint8_t* data_;
    size_t         size_;
    size_t         pos_ = 0;
};

// Read FNameEntry: FString Name; uint64_t Flags;
bool ReadNameEntry(Reader& r, std::string* name) {
    if (!r.FString(name)) return false;
    return r.Skip(8); // QWORD flags
}

bool ReadSummary(Reader& r, PackageSummary* out, std::string* err) {
    if (!r.U32(&out->tag)) {
        if (err) *err = "short read on tag";
        return false;
    }
    if (out->tag != UE3_PACKAGE_TAG) {
        if (err) *err = "not a UE3 package (bad tag)";
        return false;
    }
    uint32_t version_word = 0;
    if (!r.U32(&version_word)) return false;
    out->version          = static_cast<uint16_t>(version_word & 0xFFFF);
    out->licensee_version = static_cast<uint16_t>(version_word >> 16);
    if (out->version != UE3_VERSION_AA3) {
        if (err) *err = "unsupported UE3 version";
        return false;
    }
    if (!r.I32(&out->total_header_size)) return false;
    if (!r.FString(&out->folder_name))   return false;
    if (!r.U32(&out->package_flags))     return false;
    if (!r.I32(&out->name_count))        return false;
    if (!r.I32(&out->name_offset))       return false;
    if (!r.I32(&out->export_count))      return false;
    if (!r.I32(&out->export_offset))     return false;
    if (!r.I32(&out->import_count))      return false;
    if (!r.I32(&out->import_offset))     return false;
    if (!r.I32(&out->depends_offset))    return false;
    if (!r.Bytes(out->guid.data(), 16))  return false;

    int32_t gen_count = 0;
    if (!r.I32(&gen_count)) return false;
    if (gen_count < 0 || gen_count > 64) {
        if (err) *err = "implausible generation count";
        return false;
    }
    out->generations.resize(gen_count);
    for (int32_t i = 0; i < gen_count; ++i) {
        if (!r.I32(&out->generations[i].export_count))     return false;
        if (!r.I32(&out->generations[i].name_count))       return false;
        if (!r.I32(&out->generations[i].net_object_count)) return false;
    }
    // The remaining summary fields are optional in some cooked variants; bail
    // gracefully if we run out of bytes.
    if (!r.I32(&out->engine_version))          return true;
    if (!r.I32(&out->cooked_content_version))  return true;
    if (!r.U32(&out->compression_flags))       return true;

    int32_t chunk_count = 0;
    if (!r.I32(&chunk_count)) return true;
    if (chunk_count < 0 || chunk_count > 65536) {
        if (err) *err = "implausible compressed chunk count";
        return false;
    }
    out->compressed_chunks.resize(chunk_count);
    for (int32_t i = 0; i < chunk_count; ++i) {
        if (!r.I32(&out->compressed_chunks[i].uncompressed_offset)) return false;
        if (!r.I32(&out->compressed_chunks[i].uncompressed_size))   return false;
        if (!r.I32(&out->compressed_chunks[i].compressed_offset))   return false;
        if (!r.I32(&out->compressed_chunks[i].compressed_size))     return false;
    }
    if (!r.U32(&out->package_source)) return true;

    int32_t addl_count = 0;
    if (!r.I32(&addl_count)) return true;
    if (addl_count < 0 || addl_count > 1024) {
        if (err) *err = "implausible additional packages count";
        return false;
    }
    out->additional_packages_to_cook.resize(addl_count);
    for (int32_t i = 0; i < addl_count; ++i) {
        if (!r.FString(&out->additional_packages_to_cook[i])) return false;
    }
    return true;
}

// UE3 fully-compressed package layout (PKG_StoreFullyCompressed):
//   First 16 bytes after the header opener are an FCompressedChunkInfo block:
//      uint32 magic = 0x9E2A83C1
//      uint32 block_size (max uncompressed chunk size, usually 0x20000)
//      uint32 compressed_size (total)
//      uint32 uncompressed_size (total)
//   Then N chunks back-to-back, each with its own FCompressedChunkInfo header
//   then the LZO payload.
//
// For PKG_StoreCompressed we use summary.compressed_chunks directly; each
// entry points at file-absolute compressed_offset and we decompress one chunk
// at a time into a chunk buffer.

struct ChunkHeader {
    uint32_t magic;
    uint32_t block_size;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
};

bool LzoDecompress(const uint8_t* src, size_t src_size,
                   uint8_t* dst, size_t dst_capacity,
                   size_t* dst_used, std::string* err) {
    if (!EnsureLzoInit()) {
        if (err) *err = "lzo_init failed";
        return false;
    }
    lzo_uint out_len = static_cast<lzo_uint>(dst_capacity);
    int rc = lzo1x_decompress_safe(src, static_cast<lzo_uint>(src_size),
                                   dst, &out_len, nullptr);
    if (rc != LZO_E_OK) {
        if (err) *err = "lzo1x_decompress_safe failed (rc="
                     + std::to_string(rc) + ")";
        return false;
    }
    *dst_used = out_len;
    return true;
}

bool DecompressOneChunk(const uint8_t* file_data, size_t file_size,
                        int32_t compressed_offset, int32_t compressed_size,
                        int32_t uncompressed_size,
                        std::vector<uint8_t>* dst, std::string* err) {
    if (compressed_offset < 0 ||
        static_cast<size_t>(compressed_offset) + sizeof(ChunkHeader) > file_size) {
        if (err) *err = "chunk header out of range";
        return false;
    }
    ChunkHeader top{};
    std::memcpy(&top, file_data + compressed_offset, sizeof(top));
    if (top.magic != UE3_PACKAGE_TAG) {
        if (err) *err = "bad chunk magic";
        return false;
    }
    // Walk sub-blocks. UE3 splits each chunk into N sub-blocks of up to
    // top.block_size uncompressed bytes; the first ChunkHeader is followed
    // by N (compressed_size, uncompressed_size) pairs (FCompressedChunkBlock
    // = uint32 + uint32), then the LZO payloads back-to-back.
    size_t pos = static_cast<size_t>(compressed_offset) + sizeof(ChunkHeader);
    size_t total_unc = top.uncompressed_size;
    if (static_cast<int32_t>(total_unc) != uncompressed_size) {
        // The summary's uncompressed_size is per-chunk; this should match.
        // Tolerate small discrepancies (some cookers pad).
    }
    size_t num_blocks = (total_unc + top.block_size - 1) / top.block_size;
    if (num_blocks == 0 || num_blocks > 1024) {
        if (err) *err = "implausible sub-block count";
        return false;
    }
    struct SubBlock { uint32_t compressed_size; uint32_t uncompressed_size; };
    std::vector<SubBlock> blocks(num_blocks);
    if (pos + num_blocks * sizeof(SubBlock) > file_size) {
        if (err) *err = "sub-block table out of range";
        return false;
    }
    std::memcpy(blocks.data(), file_data + pos, num_blocks * sizeof(SubBlock));
    pos += num_blocks * sizeof(SubBlock);

    dst->resize(total_unc);
    size_t out_off = 0;
    for (size_t i = 0; i < num_blocks; ++i) {
        if (pos + blocks[i].compressed_size > file_size) {
            if (err) *err = "sub-block payload out of range";
            return false;
        }
        size_t used = 0;
        if (!LzoDecompress(file_data + pos, blocks[i].compressed_size,
                           dst->data() + out_off, blocks[i].uncompressed_size,
                           &used, err)) {
            return false;
        }
        if (used != blocks[i].uncompressed_size) {
            if (err) *err = "lzo size mismatch";
            return false;
        }
        out_off += used;
        pos     += blocks[i].compressed_size;
    }
    (void)compressed_size; // size as declared in the outer chunk; informational
    return true;
}

bool ReadExports(const uint8_t* body, size_t body_size,
                 const PackageSummary& s, int32_t body_base_offset,
                 const std::vector<std::string>& names,
                 std::vector<ObjectExport>* out, std::string* err) {
    int32_t off = s.export_offset - body_base_offset;
    if (off < 0 || static_cast<size_t>(off) >= body_size) {
        if (err) *err = "export_offset out of decompressed body";
        return false;
    }
    Reader r(body, body_size);
    if (!r.Seek(static_cast<size_t>(off))) return false;
    out->reserve(s.export_count);
    const bool obfuscated = (s.package_flags & PKG_AA3Obfuscated) != 0;
    auto safe_name = [&](int32_t idx) -> std::string {
        if (idx < 0 || static_cast<size_t>(idx) >= names.size()) {
            return "<bad-name-" + std::to_string(idx) + ">";
        }
        return names[idx];
    };
    for (int32_t i = 0; i < s.export_count; ++i) {
        ObjectExport e{};
        int32_t obj_name_idx, obj_name_num;
        // AA3 obfuscation: a per-entry XOR key prefixes the export when the
        // package has PKG_AA3Obfuscated set. FName indices are NOT XORed.
        int32_t obfuscator = 0;
        if (obfuscated && !r.I32(&obfuscator)) {
            if (err) *err = "short read on AA3 obfuscator at export " + std::to_string(i);
            return false;
        }
        if (!r.I32(&e.class_index)     ||
            !r.I32(&e.super_index)     ||
            !r.I32(&e.outer_index)     ||
            !r.I32(&obj_name_idx)      || !r.I32(&obj_name_num) ||
            !r.I32(&e.archetype_index) ||
            !r.Bytes(&e.object_flags, 8) ||
            !r.I32(&e.serial_size)     ||
            !r.I32(&e.serial_offset)   ||
            !r.U32(&e.export_flags)    ||
            !r.I32(&e.net_object_count)) {
            if (err) *err = "short read in export header at " + std::to_string(i);
            return false;
        }
        if (obfuscated) {
            e.class_index     ^= obfuscator;
            e.super_index     ^= obfuscator;
            e.outer_index     ^= obfuscator;
            e.archetype_index ^= obfuscator;
            e.serial_size     ^= obfuscator;
            e.serial_offset   ^= obfuscator;
        }
        e.object_name = safe_name(obj_name_idx);
        (void)obj_name_num;
        // The TArray<INT> GenerationNetObjectCount payload. Defend against a misaligned read producing a garbage count by
        // capping at "rest of decompressed body". Saves partial progress in
        // `out` so callers can still use what we got.
        size_t net_bytes = static_cast<size_t>(e.net_object_count) * 4;
        if (e.net_object_count < 0 || net_bytes > r.Remaining()) {
            if (err) *err = "short read in NetObjects at export " + std::to_string(i) +
                            " (count=" + std::to_string(e.net_object_count) + ")";
            return false;
        }
        e.generation_net_object_count.resize(static_cast<size_t>(e.net_object_count));
        for (int32_t& c : e.generation_net_object_count) {
            if (!r.I32(&c)) {
                if (err) *err = "short read in NetObjects at export " + std::to_string(i);
                return false;
            }
        }
        if (!r.Bytes(e.package_guid.data(), 16) || !r.U32(&e.package_flags)) {
            if (err) *err = "short read in export trailer at " + std::to_string(i);
            return false;
        }
        out->push_back(e);
    }
    return true;
}

bool ReadImports(const uint8_t* body, size_t body_size,
                 const PackageSummary& s, int32_t body_base_offset,
                 const std::vector<std::string>& names,
                 std::vector<ObjectImport>* out, std::string* err) {
    int32_t off = s.import_offset - body_base_offset;
    if (off < 0 || static_cast<size_t>(off) >= body_size) {
        if (err) *err = "import_offset out of decompressed body";
        return false;
    }
    Reader r(body, body_size);
    if (!r.Seek(static_cast<size_t>(off))) return false;
    out->resize(s.import_count);
    auto safe_name = [&](int32_t idx) -> std::string {
        if (idx < 0 || static_cast<size_t>(idx) >= names.size()) {
            return "<bad-name-" + std::to_string(idx) + ">";
        }
        return names[idx];
    };
    for (int32_t i = 0; i < s.import_count; ++i) {
        int32_t cls_pkg_idx, cls_pkg_num;
        int32_t cls_name_idx, cls_name_num;
        int32_t outer;
        int32_t obj_name_idx, obj_name_num;
        if (!r.I32(&cls_pkg_idx) || !r.I32(&cls_pkg_num) ||
            !r.I32(&cls_name_idx) || !r.I32(&cls_name_num) ||
            !r.I32(&outer) ||
            !r.I32(&obj_name_idx) || !r.I32(&obj_name_num)) {
            if (err) *err = "short read in import table";
            return false;
        }
        (*out)[i].class_package = safe_name(cls_pkg_idx);
        (*out)[i].class_name    = safe_name(cls_name_idx);
        (*out)[i].outer_index   = outer;
        (*out)[i].object_name   = safe_name(obj_name_idx);
        (void)cls_pkg_num; (void)cls_name_num; (void)obj_name_num;
    }
    return true;
}

bool ReadNames(const uint8_t* body, size_t body_size,
               const PackageSummary& s, int32_t body_base_offset,
               std::vector<std::string>* out, std::string* err) {
    int32_t off = s.name_offset - body_base_offset;
    if (off < 0 || static_cast<size_t>(off) >= body_size) {
        if (err) *err = "name_offset out of decompressed body";
        return false;
    }
    Reader r(body, body_size);
    if (!r.Seek(static_cast<size_t>(off))) return false;
    out->resize(s.name_count);
    for (int32_t i = 0; i < s.name_count; ++i) {
        if (!ReadNameEntry(r, &(*out)[i])) {
            if (err) *err = "short read in name table at " + std::to_string(i);
            return false;
        }
    }
    return true;
}

} // namespace

// ---------- PackageSummary::GuidString ------------------------------------

std::string PackageSummary::GuidString() const {
    uint32_t a, b, c, d;
    std::memcpy(&a, guid.data() + 0,  4);
    std::memcpy(&b, guid.data() + 4,  4);
    std::memcpy(&c, guid.data() + 8,  4);
    std::memcpy(&d, guid.data() + 12, 4);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%08X%08X%08X%08X", a, b, c, d);
    return std::string(buf, 32);
}

// ---------- Package::TopLevelImportPackages -------------------------------

std::vector<std::string> Package::TopLevelImportPackages() const {
    std::vector<std::string> out;
    out.reserve(16);
    for (const auto& imp : imports) {
        if (imp.outer_index == 0 && imp.class_name == "Package") {
            bool seen = false;
            for (const auto& existing : out) {
                if (existing == imp.object_name) { seen = true; break; }
            }
            if (!seen) out.push_back(imp.object_name);
        }
    }
    return out;
}

int32_t Package::FindExport(const std::string& object_name,
                             const std::string& class_name) const {
    auto class_of = [&](const ObjectExport& e) -> std::string {
        // class_index == 0 means the export is itself a UClass instance
        // (a meta-class entry). Dump tools display this as "Class".
        if (e.class_index == 0) return "Class";
        if (e.class_index > 0) {
            size_t idx = static_cast<size_t>(e.class_index);
            if (idx > 0 && idx <= exports.size())
                return exports[idx - 1].object_name;
            return "";
        }
        size_t imp_idx = static_cast<size_t>(-e.class_index - 1);
        if (imp_idx < imports.size()) return imports[imp_idx].object_name;
        return "";
    };
    for (size_t i = 0; i < exports.size(); ++i) {
        if (exports[i].object_name != object_name) continue;
        if (!class_name.empty() && class_of(exports[i]) != class_name) continue;
        return static_cast<int32_t>(i);
    }
    return -1;
}

std::string Package::ResolveIndex(int32_t package_index) const {
    if (package_index == 0) return "<null>";
    if (package_index > 0) {
        size_t idx = static_cast<size_t>(package_index - 1);
        if (idx < exports.size()) {
            return exports[idx].object_name + " (export #" +
                   std::to_string(package_index) + ")";
        }
        return "<bad-export-" + std::to_string(package_index) + ">";
    }
    size_t imp_idx = static_cast<size_t>(-package_index - 1);
    if (imp_idx < imports.size()) {
        return imports[imp_idx].object_name + " (import #" +
               std::to_string(-package_index) + ")";
    }
    return "<bad-import-" + std::to_string(package_index) + ">";
}

std::optional<UStructHeader>
UStructReader::ReadHeader(const Package& pkg, const ObjectExport& exp,
                          bool is_class_object, bool read_struct_fields,
                          std::string* err) {
    if (pkg.body.empty()) {
        if (err) *err = "UStructReader: package body not decompressed";
        return std::nullopt;
    }
    int32_t off = exp.serial_offset - pkg.body_base;
    if (off < 0 ||
        static_cast<size_t>(off) >= pkg.body.size() ||
        exp.serial_size <= 0 ||
        static_cast<size_t>(off + exp.serial_size) > pkg.body.size()) {
        if (err) *err = "UStructReader: serial range out of body";
        return std::nullopt;
    }
    Reader r(pkg.body.data(), pkg.body.size());
    if (!r.Seek(static_cast<size_t>(off))) {
        if (err) *err = "UStructReader: seek failed";
        return std::nullopt;
    }
    UStructHeader h{};
    // UObject::SerializeNetIndex.
    if (!r.I32(&h.net_index)) goto short_read;
    // UObject::Serialize: every object whose Class != UClass::StaticClass()
    // (i.e. everything except a UClass instance) then serializes its tagged
    // UnrealScript properties, which for these native meta-objects is just the
    // FName("None") terminator = 8 bytes (NameIndex INT + Number INT).
    if (!is_class_object) {
        int32_t none_name = 0, none_number = 0;
        if (!r.I32(&none_name))   goto short_read;
        if (!r.I32(&none_number)) goto short_read;
    }
    // UField::Serialize (legacy path, v568 < VER_MOVED_SUPERFIELD_TO_USTRUCT=756):
    //   SuperField is read here and stored into UStruct::SuperStruct; Next follows.
    if (!r.I32(&h.super_struct)) goto short_read;
    if (!r.I32(&h.next))         goto short_read;
    // UStruct::Serialize body (only UStruct subclasses; UProperty stops above).
    if (read_struct_fields) {
        if (!r.I32(&h.script_text))  goto short_read;
        if (!r.I32(&h.children))     goto short_read;
        if (!r.I32(&h.cpp_text))     goto short_read;
        if (!r.I32(&h.line))         goto short_read;
        if (!r.I32(&h.text_pos))     goto short_read;
        if (!r.I32(&h.script_bytecode_size)) goto short_read;
    }
    return h;
short_read:
    if (err) *err = "UStructReader: short read in struct header";
    return std::nullopt;
}

std::vector<Package::ForcedChild> Package::ForcedExportChildren() const {
    std::vector<ForcedChild> out;
    out.reserve(32);
    for (const auto& e : exports) {
        if ((e.export_flags & EF_ForcedExport) == 0) continue;
        if (e.outer_index != 0) continue;
        if (e.class_index >= 0) continue;
        size_t imp_idx = static_cast<size_t>(-e.class_index - 1);
        if (imp_idx >= imports.size()) continue;
        if (imports[imp_idx].object_name != "Package") continue;
        bool seen = false;
        for (const auto& existing : out)
            if (existing.name == e.object_name) { seen = true; break; }
        if (seen) continue;
        out.push_back({e.object_name, e.package_guid, e.package_flags,
                       e.generation_net_object_count});
    }
    return out;
}

std::vector<std::string> Package::ForcedExportPackages() const {
    // A forced UPackage export has:
    //   ExportFlags & EF_ForcedExport
    //   ClassIndex points to an import whose ObjectName == "Package"
    //   OuterIndex == 0  (the export is itself a top-level package)
    // We resolve ClassIndex via the import table: index -N means imports[N-1].
    std::vector<std::string> out;
    out.reserve(32);
    for (const auto& e : exports) {
        if ((e.export_flags & EF_ForcedExport) == 0) continue;
        if (e.outer_index != 0) continue;
        if (e.class_index >= 0) continue;
        size_t imp_idx = static_cast<size_t>(-e.class_index - 1);
        if (imp_idx >= imports.size()) continue;
        if (imports[imp_idx].object_name != "Package") continue;
        // Dedupe (defensive — usually unique).
        bool seen = false;
        for (const auto& existing : out) {
            if (existing == e.object_name) { seen = true; break; }
        }
        if (!seen) out.push_back(e.object_name);
    }
    return out;
}

// ---------- DecompressPackageBody -----------------------------------------

bool DecompressPackageBody(const uint8_t* file_data, size_t file_size,
                           const PackageSummary& summary,
                           std::vector<uint8_t>* out_body,
                           int32_t* out_body_base_offset,
                           std::string* err) {
    if (summary.compressed_chunks.empty()) {
        if (err) *err = "no compressed chunks in summary";
        return false;
    }
    // The summary's chunks describe contiguous uncompressed-space coverage
    // starting at total_header_size. We reassemble a single buffer covering
    // [first.uncompressed_offset, last.uncompressed_offset + last.size).
    int32_t base = summary.compressed_chunks.front().uncompressed_offset;
    size_t total = 0;
    for (const auto& c : summary.compressed_chunks) {
        total = static_cast<size_t>(c.uncompressed_offset - base) +
                static_cast<size_t>(c.uncompressed_size);
    }
    out_body->assign(total, 0);

    for (const auto& c : summary.compressed_chunks) {
        std::vector<uint8_t> chunk;
        if (!DecompressOneChunk(file_data, file_size,
                                c.compressed_offset, c.compressed_size,
                                c.uncompressed_size,
                                &chunk, err)) {
            return false;
        }
        size_t dst = static_cast<size_t>(c.uncompressed_offset - base);
        if (dst + chunk.size() > out_body->size()) {
            if (err) *err = "chunk overruns reassembled buffer";
            return false;
        }
        std::memcpy(out_body->data() + dst, chunk.data(), chunk.size());
    }
    *out_body_base_offset = base;
    return true;
}

// ---------- DecompressBodyForFile ------------------------------------------

bool DecompressBodyForFile(const std::string& path,
                           std::vector<uint8_t>* out_body,
                           int32_t* out_body_base,
                           PackageSummary* out_summary,
                           std::string* err) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (err) *err = std::string("open: ") + std::strerror(errno); return false; }
    struct stat st {};
    if (::fstat(fd, &st) != 0) { ::close(fd); if (err) *err = "fstat"; return false; }
    void* m = ::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) { if (err) *err = "mmap"; return false; }

    Reader r(static_cast<const uint8_t*>(m), st.st_size);
    if (!ReadSummary(r, out_summary, err)) { ::munmap(m, st.st_size); return false; }
    bool ok = DecompressPackageBody(static_cast<const uint8_t*>(m), st.st_size,
                                    *out_summary, out_body, out_body_base, err);
    ::munmap(m, st.st_size);
    return ok;
}

// ---------- PackageReader::ReadBuffer / ReadFile / ReadSummary ------------

std::optional<PackageSummary>
PackageReader::ReadSummaryFile(const std::string& path, PackageReadError* err) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err) err->message = std::string("open: ") + std::strerror(errno);
        return std::nullopt;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        if (err) err->message = std::string("fstat: ") + std::strerror(errno);
        return std::nullopt;
    }
    // Map only the first 64 KiB; the summary always fits well inside that.
    size_t map_size = std::min<size_t>(st.st_size, 64 * 1024);
    void* m = ::mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        if (err) err->message = std::string("mmap: ") + std::strerror(errno);
        return std::nullopt;
    }
    Reader r(static_cast<const uint8_t*>(m), map_size);
    PackageSummary s;
    std::string msg;
    bool ok = ReadSummary(r, &s, &msg);
    ::munmap(m, map_size);
    if (!ok) {
        if (err) err->message = msg;
        return std::nullopt;
    }
    return s;
}

std::optional<Package>
PackageReader::ReadBuffer(const uint8_t* data, size_t size, PackageReadError* err) {
    Reader r(data, size);
    Package pkg;
    std::string msg;
    if (!ReadSummary(r, &pkg.summary, &msg)) {
        if (err) err->message = msg;
        return std::nullopt;
    }

    // Decide whether the name/import tables are in the raw file or in the
    // decompressed body. We keep the decompressed body on the Package so
    // later passes (UClass parsing etc.) can re-seek into specific export
    // serial ranges without re-decompressing.
    const uint8_t* body_data = data;
    size_t         body_size = size;
    int32_t        body_base = 0;

    // Only the chunk-table form (PKG_StoreCompressed, or a fully-compressed
    // package whose chunk table we parsed into the summary) is decompressible
    // here. A package with PKG_StoreFullyCompressed set but *no* compressed
    // chunks is already-decompressed on disk — that is exactly what the
    // aa3_decompress.py / aa3_deobfuscate.py pre-clean pipeline emits: it
    // strips the LZO+XOR body but leaves the flag bit in package_flags. Treat
    // that (and genuinely uncompressed packages) as a raw body.
    if (!pkg.summary.compressed_chunks.empty()) {
        std::string dmsg;
        if (!DecompressPackageBody(data, size, pkg.summary,
                                   &pkg.body, &body_base, &dmsg)) {
            if (err) err->message = "decompress: " + dmsg;
            return std::nullopt;
        }
        body_data = pkg.body.data();
        body_size = pkg.body.size();
        pkg.body_base = body_base;
    } else {
        // Uncompressed / already-decompressed packages: copy the body so
        // callers don't need to keep `data` alive. body_base stays 0 because
        // export SerialOffsets are absolute into this same buffer.
        pkg.body.assign(data, data + size);
        pkg.body_base = 0;
    }

    if (!ReadNames(body_data, body_size, pkg.summary, body_base,
                   &pkg.names, &msg)) {
        if (err) err->message = msg;
        return std::nullopt;
    }
    if (!ReadImports(body_data, body_size, pkg.summary, body_base,
                     pkg.names, &pkg.imports, &msg)) {
        if (err) err->message = msg;
        return std::nullopt;
    }
    // Exports are best-effort: misaligned FObjectExport layout in some
    // patched / partial cooks shouldn't take down the whole parse, since
    // names+imports alone still let the manifest builder synthesize wire
    // output. Surface the reason on stderr so callers can spot silent gaps.
    if (!ReadExports(body_data, body_size, pkg.summary, body_base,
                     pkg.names, &pkg.exports, &msg)) {
        // Keep partial results in pkg.exports so callers still see what we
        // got before the misalignment.
        std::fprintf(stderr, "  exports partial (%zu/%d read): %s\n",
                     pkg.exports.size(), pkg.summary.export_count,
                     msg.c_str());
    }
    return pkg;
}

std::optional<Package>
PackageReader::ReadFile(const std::string& path, PackageReadError* err) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err) err->message = std::string("open: ") + std::strerror(errno);
        return std::nullopt;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        if (err) err->message = std::string("fstat: ") + std::strerror(errno);
        return std::nullopt;
    }
    void* m = ::mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        if (err) err->message = std::string("mmap: ") + std::strerror(errno);
        return std::nullopt;
    }
    auto result = ReadBuffer(static_cast<const uint8_t*>(m),
                             static_cast<size_t>(st.st_size), err);
    ::munmap(m, st.st_size);
    return result;
}

} // namespace engine
} // namespace aa3
