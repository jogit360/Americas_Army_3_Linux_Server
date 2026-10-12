#include "engine/map_bundle.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include "util/crc.h"
#include "util/json.h"
#include "util/logging.h"

namespace aa3 {
namespace engine {

namespace {

constexpr size_t kSectionAlignment = 64;

size_t AlignUp(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

} // namespace

const char* SectionTypeName(SectionType type) {
    switch (type) {
        case SectionType::kManifest:           return "Manifest";
        case SectionType::kCollision:          return "Collision";
        case SectionType::kMapActors:          return "MapActors";
        case SectionType::kRoundActors:        return "RoundActors";
        case SectionType::kJoinActors:         return "JoinActors";
        case SectionType::kMatchActors:        return "MatchActors";
        case SectionType::kMissionRules:       return "MissionRules";
        case SectionType::kWindows:            return "Windows";
        case SectionType::kSpawns:             return "Spawns";
        case SectionType::kSpawnInventory:     return "SpawnInventory";
        case SectionType::kInstancedCollision: return "InstancedCollision";
        default:                               return "Unknown";
    }
}

MapBundle::MapBundle() = default;

MapBundle::~MapBundle() {
    Close();
}

MapBundle::MapBundle(MapBundle&& other) noexcept {
    *this = std::move(other);
}

MapBundle& MapBundle::operator=(MapBundle&& other) noexcept {
    if (this != &other) {
        Close();
        loaded_ = other.loaded_;
        header_ = other.header_;
        map_name_ = std::move(other.map_name_);
        gametype_level_ = std::move(other.gametype_level_);
        sections_ = std::move(other.sections_);
        data_ = other.data_;
        size_ = other.size_;
        mapped_mem_ = other.mapped_mem_;
        mapped_size_ = other.mapped_size_;
        owned_buffer_ = std::move(other.owned_buffer_);

        other.loaded_ = false;
        other.mapped_mem_ = nullptr;
        other.mapped_size_ = 0;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void MapBundle::Close() {
    if (mapped_mem_ && mapped_size_ > 0) {
        ::munmap(mapped_mem_, mapped_size_);
        mapped_mem_ = nullptr;
        mapped_size_ = 0;
    }
    owned_buffer_.clear();
    sections_.clear();
    data_ = nullptr;
    size_ = 0;
    loaded_ = false;
}

namespace {

// A shipped bundle is the .aa3map compressed whole with zstd (data/maps/
// <map>.aa3map.zst, `zstd -19 --long=27`); decompressed into memory.
bool ReadZstdFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return false;
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 31);
    const unsigned long long known = ZSTD_getFrameContentSize(in.data(), in.size());
    out.clear();
    if (known != ZSTD_CONTENTSIZE_UNKNOWN && known != ZSTD_CONTENTSIZE_ERROR)
        out.reserve(static_cast<size_t>(known));
    std::vector<uint8_t> chunk(ZSTD_DStreamOutSize());
    ZSTD_inBuffer ib{in.data(), in.size(), 0};
    size_t ret = 1;
    while (ib.pos < ib.size) {
        ZSTD_outBuffer ob{chunk.data(), chunk.size(), 0};
        ret = ZSTD_decompressStream(dctx, &ob, &ib);
        if (ZSTD_isError(ret)) {
            LOG_ERROR("MapBundle: %s: zstd: %s", path.c_str(), ZSTD_getErrorName(ret));
            ZSTD_freeDCtx(dctx);
            return false;
        }
        out.insert(out.end(), chunk.data(), chunk.data() + ob.pos);
    }
    ZSTD_freeDCtx(dctx);
    if (ret != 0) {
        LOG_ERROR("MapBundle: %s: truncated zstd stream", path.c_str());
        return false;
    }
    return true;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool MapBundle::Load(const std::string& path) {
    Close();
    if (EndsWith(path, ".zst")) {
        if (!ReadZstdFile(path, owned_buffer_)) {
            LOG_WARN("MapBundle: unable to read %s", path.c_str());
            return false;
        }
        data_ = owned_buffer_.data();
        size_ = owned_buffer_.size();
        return ParseAndValidate(data_, size_, path);
    }
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOG_WARN("MapBundle: unable to open file %s", path.c_str());
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(MapBundleHeader))) {
        LOG_ERROR("MapBundle: invalid or too small file %s", path.c_str());
        ::close(fd);
        return false;
    }

    const size_t file_size = static_cast<size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);

    if (mapped != MAP_FAILED) {
        mapped_mem_ = mapped;
        mapped_size_ = file_size;
        data_ = static_cast<const uint8_t*>(mapped);
        size_ = file_size;
    } else {
        LOG_WARN("MapBundle: mmap failed for %s, falling back to buffered read", path.c_str());
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        owned_buffer_.resize(file_size);
        if (!f.read(reinterpret_cast<char*>(owned_buffer_.data()), file_size)) {
            LOG_ERROR("MapBundle: failed reading %s", path.c_str());
            return false;
        }
        data_ = owned_buffer_.data();
        size_ = owned_buffer_.size();
    }

    return ParseAndValidate(data_, size_, path);
}

bool MapBundle::LoadFromMemory(const uint8_t* data, size_t size, const std::string& name) {
    Close();
    if (!data || size < sizeof(MapBundleHeader)) {
        LOG_ERROR("MapBundle: invalid buffer in %s", name.c_str());
        return false;
    }
    data_ = data;
    size_ = size;
    return ParseAndValidate(data_, size_, name);
}

bool MapBundle::ParseAndValidate(const uint8_t* data, size_t size, const std::string& source_name) {
    if (size < sizeof(MapBundleHeader)) {
        LOG_ERROR("MapBundle [%s]: file size %zu smaller than header %zu",
                  source_name.c_str(), size, sizeof(MapBundleHeader));
        return false;
    }

    std::memcpy(&header_, data, sizeof(MapBundleHeader));

    if (std::memcmp(header_.magic, kMapBundleMagic, sizeof(header_.magic)) != 0) {
        LOG_ERROR("MapBundle [%s]: bad magic (expected 'AA3MAP01')", source_name.c_str());
        return false;
    }

    if (header_.version != kMapBundleVersion) {
        LOG_ERROR("MapBundle [%s]: unsupported version %u (supported: %u)",
                  source_name.c_str(), header_.version, kMapBundleVersion);
        return false;
    }

    // Verify header CRC32 (covers fields up to header_crc32)
    const size_t crc_len = offsetof(MapBundleHeader, header_crc32);
    const uint32_t calc_hdr_crc = util::CRC32::Calculate(data, crc_len);
    if (header_.header_crc32 != 0 && header_.header_crc32 != calc_hdr_crc) {
        LOG_ERROR("MapBundle [%s]: header CRC mismatch (got 0x%08X, expected 0x%08X)",
                  source_name.c_str(), header_.header_crc32, calc_hdr_crc);
        return false;
    }

    map_name_ = std::string(header_.map_name, ::strnlen(header_.map_name, sizeof(header_.map_name)));
    gametype_level_ = std::string(header_.gametype_level, ::strnlen(header_.gametype_level, sizeof(header_.gametype_level)));

    const size_t section_table_offset = sizeof(MapBundleHeader);
    const size_t section_table_size = header_.section_count * sizeof(SectionEntry);
    if (section_table_offset + section_table_size > size) {
        LOG_ERROR("MapBundle [%s]: section table exceeds file size", source_name.c_str());
        return false;
    }

    sections_.resize(header_.section_count);
    const uint8_t* sec_ptr = data + section_table_offset;
    for (uint32_t i = 0; i < header_.section_count; ++i) {
        std::memcpy(&sections_[i], sec_ptr, sizeof(SectionEntry));
        sec_ptr += sizeof(SectionEntry);

        const SectionEntry& s = sections_[i];
        if (s.offset + s.size > size) {
            LOG_ERROR("MapBundle [%s]: section %u (%s) extends beyond file (%lu + %lu > %zu)",
                      source_name.c_str(), i, SectionTypeName(static_cast<SectionType>(s.type)),
                      s.offset, s.size, size);
            return false;
        }

        if (s.size > 0 && s.crc32 != 0) {
            const uint32_t calc_crc = util::CRC32::Calculate(data + s.offset, s.size);
            if (calc_crc != s.crc32) {
                LOG_ERROR("MapBundle [%s]: section %u (%s) CRC mismatch (file 0x%08X != calc 0x%08X)",
                          source_name.c_str(), i, SectionTypeName(static_cast<SectionType>(s.type)),
                          s.crc32, calc_crc);
                return false;
            }
        }
    }

    loaded_ = true;
    LOG_INFO("MapBundle [%s]: loaded '%s' (v%u, %u sections, %zu bytes)",
             source_name.c_str(), map_name_.c_str(), header_.version,
             header_.section_count, size);
    return true;
}

bool MapBundle::HasSection(SectionType type) const {
    if (!loaded_) return false;
    for (const auto& s : sections_) {
        if (s.type == static_cast<uint32_t>(type)) return true;
    }
    return false;
}

const uint8_t* MapBundle::GetSectionData(SectionType type, size_t* out_size) const {
    if (!loaded_) return nullptr;
    for (const auto& s : sections_) {
        if (s.type == static_cast<uint32_t>(type)) {
            if (out_size) *out_size = static_cast<size_t>(s.size);
            return data_ + s.offset;
        }
    }
    return nullptr;
}

bool MapBundle::GetSectionString(SectionType type, std::string& out) const {
    size_t sz = 0;
    const uint8_t* p = GetSectionData(type, &sz);
    if (!p || sz == 0) return false;
    out.assign(reinterpret_cast<const char*>(p), sz);
    return true;
}

std::optional<MapManifest> MapBundle::GetManifest() const {
    std::string json;
    if (!GetSectionString(SectionType::kManifest, json)) return std::nullopt;
    std::string err;
    return MapManifest::FromJson(json, &err);
}

bool MapBundle::GetCollisionData(const uint8_t*& out_ptr, size_t& out_size) const {
    // Check instanced collision first, then flat collision
    if (HasSection(SectionType::kInstancedCollision)) {
        out_ptr = GetSectionData(SectionType::kInstancedCollision, &out_size);
        return out_ptr != nullptr;
    }
    out_ptr = GetSectionData(SectionType::kCollision, &out_size);
    return out_ptr != nullptr;
}

bool MapBundle::GetWindows(std::map<uint32_t, int>& out_windows) const {
    out_windows.clear();
    std::string json;
    if (!GetSectionString(SectionType::kWindows, json)) return false;
    auto doc = util::ParseJson(json);
    if (!doc || !doc->Has("windows")) return false;
    for (const auto& [k, v] : (*doc)["windows"].obj) {
        out_windows[static_cast<uint32_t>(std::stoul(k))] = static_cast<int>(v.Int());
    }
    return true;
}

bool MapBundle::GetSpawnPoint(int team, float& x, float& y, float& z, int& yaw) const {
    std::string json;
    if (!GetSectionString(SectionType::kSpawns, json)) return false;
    auto doc = util::ParseJson(json);
    if (!doc) return false;
    std::string key = std::to_string(team);
    if (!doc->Has(key)) return false;
    const auto& item = (*doc)[key];
    if (item.type == util::JsonValue::Type::kString) {
        return std::sscanf(item.Str().c_str(), "%f,%f,%f,%d", &x, &y, &z, &yaw) == 4;
    } else if (item.type == util::JsonValue::Type::kObject) {
        x = static_cast<float>(item["x"].num);
        y = static_cast<float>(item["y"].num);
        z = static_cast<float>(item["z"].num);
        yaw = static_cast<int>(item["yaw"].Int());
        return true;
    }
    return false;
}

bool MapBundle::GetMissionRulesJson(std::string& out_json) const {
    return GetSectionString(SectionType::kMissionRules, out_json);
}

bool MapBundle::GetActorJson(SectionType type, std::string& out_json) const {
    return GetSectionString(type, out_json);
}

// ---------------------------------------------------------------------------
// MapBundleWriter
// ---------------------------------------------------------------------------

MapBundleWriter::MapBundleWriter(const std::string& map_name, const std::string& gametype_level)
    : map_name_(map_name), gametype_level_(gametype_level) {}

void MapBundleWriter::SetSourceGuid(const uint8_t guid[16]) {
    if (guid) std::memcpy(source_guid_, guid, 16);
}

void MapBundleWriter::SetSourceSha256(const uint8_t sha256[32]) {
    if (sha256) std::memcpy(source_sha256_, sha256, 32);
}

bool MapBundleWriter::AddSection(SectionType type, const uint8_t* data, size_t size, uint32_t flags) {
    PendingSection ps;
    ps.type = type;
    ps.flags = flags;
    if (data && size > 0) {
        ps.data.assign(data, data + size);
    }
    pending_sections_.push_back(std::move(ps));
    return true;
}

bool MapBundleWriter::AddSectionString(SectionType type, const std::string& str, uint32_t flags) {
    return AddSection(type, reinterpret_cast<const uint8_t*>(str.data()), str.size(), flags);
}

bool MapBundleWriter::WriteToBuffer(std::vector<uint8_t>& out_buf, std::string* err) {
    (void)err;
    out_buf.clear();

    MapBundleHeader hdr{};
    std::memcpy(hdr.magic, kMapBundleMagic, sizeof(hdr.magic));
    hdr.version = kMapBundleVersion;
    hdr.section_count = static_cast<uint32_t>(pending_sections_.size());
    ::strncpy(hdr.map_name, map_name_.c_str(), sizeof(hdr.map_name) - 1);
    ::strncpy(hdr.gametype_level, gametype_level_.c_str(), sizeof(hdr.gametype_level) - 1);
    std::memcpy(hdr.source_guid, source_guid_, sizeof(hdr.source_guid));
    std::memcpy(hdr.source_sha256, source_sha256_, sizeof(hdr.source_sha256));

    const size_t header_size = sizeof(MapBundleHeader);
    const size_t section_table_size = hdr.section_count * sizeof(SectionEntry);
    size_t current_offset = AlignUp(header_size + section_table_size, kSectionAlignment);

    std::vector<SectionEntry> entries(hdr.section_count);
    for (size_t i = 0; i < pending_sections_.size(); ++i) {
        const auto& ps = pending_sections_[i];
        SectionEntry& entry = entries[i];
        entry.type = static_cast<uint32_t>(ps.type);
        entry.flags = ps.flags;
        entry.offset = current_offset;
        entry.size = ps.data.size();
        entry.crc32 = ps.data.empty() ? 0 : util::CRC32::Calculate(ps.data.data(), ps.data.size());
        current_offset = AlignUp(current_offset + ps.data.size(), kSectionAlignment);
    }

    hdr.file_size = current_offset;
    const size_t crc_len = offsetof(MapBundleHeader, header_crc32);
    hdr.header_crc32 = util::CRC32::Calculate(reinterpret_cast<const uint8_t*>(&hdr), crc_len);

    out_buf.resize(current_offset, 0);
    std::memcpy(out_buf.data(), &hdr, sizeof(MapBundleHeader));
    std::memcpy(out_buf.data() + header_size, entries.data(), section_table_size);

    for (size_t i = 0; i < pending_sections_.size(); ++i) {
        const auto& ps = pending_sections_[i];
        if (!ps.data.empty()) {
            std::memcpy(out_buf.data() + entries[i].offset, ps.data.data(), ps.data.size());
        }
    }

    return true;
}

bool MapBundleWriter::WriteToFile(const std::string& out_path, std::string* err) {
    std::vector<uint8_t> buf;
    if (!WriteToBuffer(buf, err)) return false;

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        if (err) *err = "Failed to open output file: " + out_path;
        return false;
    }
    if (!out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) {
        if (err) *err = "Failed to write data to: " + out_path;
        return false;
    }
    return true;
}

} // namespace engine
} // namespace aa3
