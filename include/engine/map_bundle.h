#ifndef AA3_ENGINE_MAP_BUNDLE_H
#define AA3_ENGINE_MAP_BUNDLE_H

// Unified Map Bundle (.aa3map) architecture for the AA3 Dedicated Server.
//
// Consolidates all per-map server knowledge (manifest/USES, collision geometry,
// replicated map actors, mission/round actors, interactive windows, spawn points,
// and rules) into a single, versioned, checksummed, memory-mappable file.
//
// Eliminates the need for 1.2+ GB cooked client packages (.aa3/.upk/.u) and
// fragmented loose JSON/.col files on dedicated servers.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/map_manifest.h"

namespace aa3 {
namespace engine {

constexpr char kMapBundleMagic[8] = {'A', 'A', '3', 'M', 'A', 'P', '0', '1'};
constexpr uint32_t kMapBundleVersion = 1;

enum class SectionType : uint32_t {
    kUnknown            = 0,
    kManifest           = 1,  // MapManifest JSON (USES, object counts, GUIDs)
    kCollision          = 2,  // AA3COL02 binary collision geometry
    kMapActors          = 3,  // Placed map actors JSON
    kRoundActors        = 4,  // Round actors JSON
    kJoinActors         = 5,  // Mission join actors JSON
    kMatchActors        = 6,  // Mission match actors JSON
    kMissionRules       = 7,  // Mission rules JSON (extraction, viewpoints)
    kWindows            = 8,  // Windows table JSON (NetIndex -> window manager bit)
    kSpawns             = 9,  // Team spawn points JSON
    kSpawnInventory     = 10, // Default role inventory JSON
    kInstancedCollision = 11, // Future Phase 3 BLAS/TLAS instanced collision
};

const char* SectionTypeName(SectionType type);

#pragma pack(push, 1)
struct MapBundleHeader {
    char     magic[8];             // "AA3MAP01"
    uint32_t version;              // Format version (1)
    uint32_t section_count;        // Number of SectionEntry records
    char     map_name[64];         // Base map name (e.g. "Bridge_night_clear")
    char     gametype_level[64];   // Sublevel name (e.g. "Bridge_inf_classic_large_dev")
    uint8_t  source_guid[16];      // Source cooked package GUID (if available)
    uint8_t  source_sha256[32];    // Source cooked package SHA256 (if available)
    uint64_t file_size;            // Total file size in bytes
    uint32_t header_crc32;         // CRC32 of preceding header fields
    uint8_t  reserved[52];         // Reserved padding to 256 bytes
};
static_assert(sizeof(MapBundleHeader) == 256, "MapBundleHeader must be 256 bytes");

struct SectionEntry {
    uint32_t type;                 // SectionType
    uint32_t flags;                // 0 = uncompressed
    uint64_t offset;               // 64-byte aligned file offset
    uint64_t size;                 // Payload size in bytes
    uint32_t crc32;                // CRC32 of payload
    uint8_t  reserved[4];          // Reserved padding to 32 bytes
};
static_assert(sizeof(SectionEntry) == 32, "SectionEntry must be 32 bytes");
#pragma pack(pop)

class MapBundle {
public:
    MapBundle();
    ~MapBundle();

    // Disable copy, allow move
    MapBundle(const MapBundle&) = delete;
    MapBundle& operator=(const MapBundle&) = delete;
    MapBundle(MapBundle&& other) noexcept;
    MapBundle& operator=(MapBundle&& other) noexcept;

    // Load bundle from file (uses memory-mapping if possible, falls back to read)
    bool Load(const std::string& path);

    // Load bundle from memory buffer
    bool LoadFromMemory(const uint8_t* data, size_t size, const std::string& name = "memory");

    // Close and release memory mapping / buffers
    void Close();

    bool IsLoaded() const { return loaded_; }
    const MapBundleHeader& Header() const { return header_; }
    const std::string& MapName() const { return map_name_; }
    const std::string& GameModeLevel() const { return gametype_level_; }
    size_t FileSize() const { return size_; }
    const std::vector<SectionEntry>& Sections() const { return sections_; }

    // Check if section exists
    bool HasSection(SectionType type) const;

    // Direct pointer to section payload
    const uint8_t* GetSectionData(SectionType type, size_t* out_size = nullptr) const;

    // Get section payload as string
    bool GetSectionString(SectionType type, std::string& out) const;

    // High-level accessors for engine components:
    std::optional<MapManifest> GetManifest() const;
    bool GetCollisionData(const uint8_t*& out_ptr, size_t& out_size) const;
    bool GetWindows(std::map<uint32_t, int>& out_windows) const;
    bool GetSpawnPoint(int team, float& x, float& y, float& z, int& yaw) const;
    bool GetMissionRulesJson(std::string& out_json) const;
    bool GetActorJson(SectionType type, std::string& out_json) const;

private:
    bool ParseAndValidate(const uint8_t* data, size_t size, const std::string& source_name);

    bool loaded_ = false;
    MapBundleHeader header_{};
    std::string map_name_;
    std::string gametype_level_;
    std::vector<SectionEntry> sections_;

    // Storage backing (mmap or buffer)
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    void* mapped_mem_ = nullptr;
    size_t mapped_size_ = 0;
    std::vector<uint8_t> owned_buffer_;
};

// Builder to create and write .aa3map bundles
class MapBundleWriter {
public:
    MapBundleWriter(const std::string& map_name, const std::string& gametype_level = "");

    void SetSourceGuid(const uint8_t guid[16]);
    void SetSourceSha256(const uint8_t sha256[32]);

    // Add sections (payload will be copied and aligned)
    bool AddSection(SectionType type, const uint8_t* data, size_t size, uint32_t flags = 0);
    bool AddSectionString(SectionType type, const std::string& str, uint32_t flags = 0);

    // Write final bundle to disk
    bool WriteToFile(const std::string& out_path, std::string* err = nullptr);

    // Or write to memory buffer
    bool WriteToBuffer(std::vector<uint8_t>& out_buf, std::string* err = nullptr);

private:
    struct PendingSection {
        SectionType type;
        uint32_t flags = 0;
        std::vector<uint8_t> data;
    };

    std::string map_name_;
    std::string gametype_level_;
    uint8_t source_guid_[16]{};
    uint8_t source_sha256_[32]{};
    std::vector<PendingSection> pending_sections_;
};

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_MAP_BUNDLE_H
