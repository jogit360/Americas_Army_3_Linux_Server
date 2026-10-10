#ifndef AA3_ENGINE_PACKAGE_READER_H
#define AA3_ENGINE_PACKAGE_READER_H

// Read UE3 v568 cooked packages (the `.aa3` / `.upk` / `.u` files AA3 ships).
//
// Scope: just enough of the FPackageFileSummary, name table, and import
// table to assemble a map manifest (Guid, PackageFlags, generations, list of
// top-level imported package names). For packages with PKG_StoreCompressed
// or PKG_StoreFullyCompressed set, the body is LZO1X-compressed in chunks
// described by the summary's CompressedChunks array; this header decompresses
// transparently using liblzo2.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aa3 {
namespace engine {

constexpr uint32_t UE3_PACKAGE_TAG = 0x9E2A83C1;
constexpr uint16_t UE3_VERSION_AA3 = 568;

// PackageFlags bits we care about (from UE3 PackageFlags enum).
constexpr uint32_t PKG_AllowDownload        = 0x00000001;
constexpr uint32_t PKG_ClientOptional       = 0x00000002;
constexpr uint32_t PKG_ServerSideOnly       = 0x00000004;
constexpr uint32_t PKG_ContainsMap          = 0x00000020;
constexpr uint32_t PKG_ContainsScript       = 0x00000200;
constexpr uint32_t PKG_StoreCompressed      = 0x00010000;
constexpr uint32_t PKG_StoreFullyCompressed = 0x02000000;

// CompressionFlags bits.
constexpr uint32_t COMPRESS_ZLIB = 0x01;
constexpr uint32_t COMPRESS_LZO  = 0x02;
constexpr uint32_t COMPRESS_LZX  = 0x04;

struct Generation {
    int32_t export_count = 0;
    int32_t name_count   = 0;
    int32_t net_object_count = 0;
};

struct CompressedChunk {
    int32_t uncompressed_offset = 0;
    int32_t uncompressed_size   = 0;
    int32_t compressed_offset   = 0;
    int32_t compressed_size     = 0;
};

struct PackageSummary {
    uint32_t tag = 0;
    uint16_t version          = 0;
    uint16_t licensee_version = 0;
    int32_t  total_header_size = 0;
    std::string folder_name;
    uint32_t package_flags = 0;
    int32_t  name_count = 0,   name_offset = 0;
    int32_t  export_count = 0, export_offset = 0;
    int32_t  import_count = 0, import_offset = 0;
    int32_t  depends_offset = 0;
    std::array<uint8_t, 16> guid{};
    std::vector<Generation> generations;
    int32_t  engine_version = 0;
    int32_t  cooked_content_version = 0;
    uint32_t compression_flags = 0;
    std::vector<CompressedChunk> compressed_chunks;
    uint32_t package_source = 0;
    std::vector<std::string> additional_packages_to_cook;

    // FGuid::String() format: 4 little-endian DWORDs, uppercase hex, no dashes.
    std::string GuidString() const;

    bool IsFullyCompressed() const {
        return (package_flags & PKG_StoreFullyCompressed) != 0;
    }
    bool IsChunkCompressed() const {
        return (package_flags & PKG_StoreCompressed) != 0;
    }
};

struct ObjectImport {
    std::string class_package;
    std::string class_name;
    int32_t     outer_index = 0;
    std::string object_name;
};

// FObjectExport, v568 layout. NetObjects is a TArray<INT> we just count
// (the bytes are skipped over for length-only callers).
struct ObjectExport {
    int32_t  class_index = 0;        // negative -> import[-idx-1], positive -> export[idx-1]
    int32_t  super_index = 0;
    int32_t  outer_index = 0;
    std::string object_name;
    int32_t  archetype_index = 0;
    uint64_t object_flags = 0;       // QWORD in v568
    int32_t  serial_size = 0;
    int32_t  serial_offset = 0;
    uint32_t export_flags = 0;       // EF_ForcedExport = 0x1
    int32_t  net_object_count = 0;   // length of GenerationNetObjectCount
    // FObjectExport::GenerationNetObjectCount: per generation, the net
    // object count of a forced-export package (EF_ForcedExport). The
    // client's PackageMap uses it as that package's ObjectCount when the
    // package is cooked into a BASEPKG (doc sec 26).
    std::vector<int32_t> generation_net_object_count;
    std::array<uint8_t, 16> package_guid{};  // FGuid embedded in the export entry
    uint32_t package_flags = 0;      // mirrored package flags
};

// ExportFlags bits.
constexpr uint32_t EF_ForcedExport = 0x00000001;

// PackageFlags bit AA3 cookers set on every package with obfuscated export
// tables. UEViewer's UnPackage3.cpp comments call this the AA3 obfuscation
// flag; bit value derived empirically as 0x00200000.
constexpr uint32_t PKG_AA3Obfuscated = 0x00200000;

struct Package {
    PackageSummary summary;
    std::vector<std::string> names;
    std::vector<ObjectImport> imports;
    std::vector<ObjectExport> exports;
    // Decompressed body buffer + body_base_offset. Non-empty only when
    // PackageReader::ReadFile is called on a fully-compressed package
    // (PKG_StoreFullyCompressed). Used by UClass/UField parsing to seek
    // into a specific export's serial data without re-decompressing.
    std::vector<uint8_t> body;
    int32_t              body_base = 0;

    // Convenience: distinct top-level imported package names, preserving order
    // of first occurrence. A top-level import is one whose OuterIndex == 0 and
    // ClassName == "Package".
    std::vector<std::string> TopLevelImportPackages() const;

    // Names of every child UPackage forced-exported INTO this package (i.e.
    // exports whose Class is UPackage and ExportFlags has EF_ForcedExport).
    // These are the packages that get BASEPKG=<this_package_name> on the wire.
    std::vector<std::string> ForcedExportPackages() const;

    // Name + embedded PackageGuid for each forced-export UPackage child. The
    // GUID is what the retail server reports on the wire for a forced-export
    // shard that has no on-disk file.
    struct ForcedChild {
        std::string name;
        std::array<uint8_t, 16> guid;
        uint32_t package_flags;
        std::vector<int32_t> gen_net_objects;  // GenerationNetObjectCount
    };
    std::vector<ForcedChild> ForcedExportChildren() const;

    // Find an export by ObjectName + class name (e.g. {"GameReplicationInfo",
    // "Class"}). Returns -1 if not found. Class name match is exact against
    // the import-resolved class.
    int32_t FindExport(const std::string& object_name,
                       const std::string& class_name = "") const;

    // Resolve an FPackageIndex into a human-readable name. Returns the
    // export's ObjectName when index > 0, the import's ObjectName when
    // index < 0, or "<null>" when index == 0.
    std::string ResolveIndex(int32_t package_index) const;
};

// Header fields read from a UField's serial data. UField is the common base
// of UStruct (→ UClass, UScriptStruct, UFunction, UState) and UProperty.
// Verified byte-for-byte against AA3 v568 (Engine.u GameReplicationInfo and
// its child properties). v568 < VER_MOVED_SUPERFIELD_TO_USTRUCT (756), so
// SuperField lives in the legacy UField slot rather than UStruct.
//
// The subtle part: UObject::Serialize (UnObj.cpp:1756-1762) calls
//   SerializeNetIndex(Ar);                                  // INT NetIndex
//   if (Class != UClass::StaticClass()) SerializeScriptProperties(Ar);
// The SerializeScriptProperties call writes the object's tagged UnrealScript
// property defaults, terminated by an FName("None") = 8 bytes (NameIndex INT +
// Number INT). Native meta-objects (UProperty/UFunction/...) have no such
// defaults, so it's exactly the 8-byte "None" terminator. A *UClass* instance
// has Class == UClass::StaticClass(), so this block is SKIPPED — which is why
// a class body has no None tag but every property/function body does.
//
//   UObject::Serialize:   INT   NetIndex
//                         FName "None"        // 8B — ONLY when !is_class_object
//   UField::Serialize:    INT   SuperField    // FPackageIndex → struct.SuperStruct
//                         INT   Next          // FPackageIndex → next sibling
//  (UStruct subclasses continue with the fields below; UProperty does not.)
//   UStruct::Serialize:   INT   ScriptText    // FPackageIndex (PC only)
//                         INT   Children      // FPackageIndex → first child field
//                         INT   CppText       // FPackageIndex (PC only)
//                         INT   Line          // (PC only)
//                         INT   TextPos       // (PC only)
//                         INT   ScriptBytecodeSize
//                         BYTE  Script[...]   // ScriptBytecodeSize bytes follow
//
// NOTE: there is NO separate on-disk "ScriptStorageSize" field in v568 — the
// bytecode bytes follow ScriptBytecodeSize directly.
struct UStructHeader {
    int32_t net_index   = 0;
    int32_t next        = 0;     // FPackageIndex: next sibling field
    int32_t super_struct = 0;    // FPackageIndex: parent class/struct
    int32_t script_text = 0;     // FPackageIndex (PC only; UStruct only)
    int32_t children    = 0;     // FPackageIndex: first child field (UStruct only)
    int32_t cpp_text    = 0;     // FPackageIndex (PC only; UStruct only)
    int32_t line        = 0;     // (UStruct only)
    int32_t text_pos    = 0;     // (UStruct only)
    int32_t script_bytecode_size = 0;  // (UStruct only)
};

class UStructReader {
public:
    // Parse the UField/UStruct header from `pkg`'s body at the export's
    // serial_offset. `is_class_object` must be true only for UClass instances
    // (export class == "Class"), which skip the SerializeScriptProperties
    // "None" tag; pass false for functions, properties, enums, structs, etc.
    // `read_struct_fields` reads the UStruct-only tail (ScriptText..Children..);
    // pass false for plain UProperty exports which stop after Next.
    // Requires pkg.body to be populated (ReadFile, not ReadSummaryFile).
    static std::optional<UStructHeader>
    ReadHeader(const Package& pkg, const ObjectExport& exp,
               bool is_class_object, bool read_struct_fields,
               std::string* err = nullptr);
};

// Errors surface as PackageReadError; methods return std::optional<Package>.
struct PackageReadError {
    std::string message;
    size_t      offset = 0;
};

class PackageReader {
public:
    // Parse the on-disk file at `path`. Returns std::nullopt on failure and
    // fills *err if provided.
    static std::optional<Package> ReadFile(const std::string& path,
                                           PackageReadError* err = nullptr);

    // Parse from an in-memory buffer.
    static std::optional<Package> ReadBuffer(const uint8_t* data, size_t size,
                                             PackageReadError* err = nullptr);

    // Read only the summary (cheap; no name/import table decompression).
    static std::optional<PackageSummary> ReadSummaryFile(const std::string& path,
                                                         PackageReadError* err = nullptr);
};

// Decompress the body of a fully-compressed UE3 package into a contiguous
// uncompressed buffer using `summary.compressed_chunks`. The output is laid
// out by `uncompressed_offset`, so reads at file-absolute uncompressed offsets
// (NameOffset, ImportOffset, etc.) Just Work after subtracting the first
// chunk's uncompressed_offset (which equals total_header_size).
//
// `compressed_body` must point at the byte immediately after the summary
// (i.e. file_data + total_header_size for chunk-compressed, or file_data +
// summary's last-parsed offset for fully-compressed). The function reads each
// chunk by its absolute compressed_offset relative to `file_data_origin`.
//
// Returns false on decompression failure.
bool DecompressPackageBody(const uint8_t* file_data, size_t file_size,
                           const PackageSummary& summary,
                           std::vector<uint8_t>* out_body,
                           int32_t* out_body_base_offset,
                           std::string* error_message);

// Convenience: mmap a file, parse its summary, decompress its body. Returns
// the body buffer + the absolute uncompressed offset its [0] byte represents.
bool DecompressBodyForFile(const std::string& path,
                           std::vector<uint8_t>* out_body,
                           int32_t* out_body_base,
                           PackageSummary* out_summary,
                           std::string* err);

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_PACKAGE_READER_H
