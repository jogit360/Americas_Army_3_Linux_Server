#include "game/collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Axis(const Vec3& v, int k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }

inline Vec3 TransformVec(const Vec3& v, const float m[3][3]) {
    return {
        v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0],
        v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1],
        v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2]
    };
}

constexpr uint32_t kLeafSize = 8;
constexpr uint32_t kTlasLeafSize = 4;

#pragma pack(push, 1)
struct Col03Header {
    char magic[8];
    uint32_t version;
    uint32_t num_meshes;
    uint32_t num_instances;
    uint32_t num_direct_tris;
    uint32_t total_equivalent_tris;
    uint64_t meshes_offset;
    uint64_t instances_offset;
    uint64_t direct_offset;
    uint8_t reserved[12];
};

struct Col04Header {
    char magic[8];
    uint32_t version;
    uint32_t num_meshes;
    uint32_t num_instances;
    uint32_t num_direct_tris;
    uint32_t total_equivalent_tris;
    uint64_t meshes_offset;
    uint64_t instances_offset;
    uint64_t direct_offset;
    uint8_t reserved[12];
};
#pragma pack(pop)

}  // namespace

bool CollisionWorld::LoadFromMemory(const uint8_t* data, size_t size, const std::string& source_name) {
    if (!data || size < 12) {
        LOG_ERROR("CollisionWorld::LoadFromMemory: invalid buffer in %s", source_name.c_str());
        return false;
    }

    // Reset previous collision state
    tris_.clear();
    order_.clear();
    nodes_.clear();
    meshes_.clear();
    instances_.clear();
    tlas_order_.clear();
    tlas_nodes_.clear();
    total_triangles_ = 0;

    const char* magic = reinterpret_cast<const char*>(data);
    if (std::memcmp(magic, "AA3MAP01", 8) == 0 && size >= 256) {
        uint32_t num_sections = 0;
        std::memcpy(&num_sections, data + 12, 4);
        size_t off = 256;
        for (uint32_t s = 0; s < num_sections && off + 32 <= size; ++s) {
            uint32_t stype = 0;
            std::memcpy(&stype, data + off, 4);
            uint64_t soff = 0, ssize = 0;
            std::memcpy(&soff, data + off + 8, 8);
            std::memcpy(&ssize, data + off + 16, 8);
            if (stype == 2 /* kCollision */ && soff + ssize <= size) {
                return LoadFromMemory(data + soff, static_cast<size_t>(ssize), source_name);
            }
            off += 32;
        }
        LOG_ERROR("No collision section found in bundle %s", source_name.c_str());
        return false;
    }
    if (std::memcmp(magic, "AA3COL04", 8) == 0) {
        return LoadCol04(data, size, source_name);
    }
    if (std::memcmp(magic, "AA3COL03", 8) == 0) {
        return LoadCol03(data, size, source_name);
    }

    const bool v2 = std::memcmp(magic, "AA3COL02", 8) == 0;
    if (!v2 && std::memcmp(magic, "AA3COL01", 8) != 0) {
        LOG_ERROR("Bad collision file %s (unrecognized magic)", source_name.c_str());
        return false;
    }
    return LoadCol01_02(data, size, v2, source_name);
}

bool CollisionWorld::LoadCol01_02(const uint8_t* data, size_t size, bool v2, const std::string& source_name) {
    uint32_t n = 0;
    std::memcpy(&n, data + 8, 4);
    const size_t rec_size = v2 ? 41 : 37;
    const size_t expected_size = 12 + static_cast<size_t>(n) * rec_size;
    if (size < expected_size) {
        LOG_ERROR("Truncated collision data in %s (expected %zu, got %zu)",
                  source_name.c_str(), expected_size, size);
        return false;
    }

    tris_.resize(n);
    const uint8_t* ptr = data + 12;
    for (uint32_t i = 0; i < n; ++i) {
        float v[9];
        std::memcpy(v, ptr, 36);
        Tri& t = tris_[i];
        t.v0 = {v[0], v[1], v[2]};
        t.e1 = Sub({v[3], v[4], v[5]}, t.v0);
        t.e2 = Sub({v[6], v[7], v[8]}, t.v0);
        t.flags = ptr[36];
        t.owner = 0;
        if (v2) std::memcpy(&t.owner, ptr + 37, 4);
        ptr += rec_size;
    }
    total_triangles_ = n;
    BuildFlatBvh();
    LOG_INFO("Collision (COL0%d): %u triangles, %zu BVH nodes from %s",
             v2 ? 2 : 1, n, nodes_.size(), source_name.c_str());
    return true;
}

bool CollisionWorld::LoadCol03(const uint8_t* data, size_t size, const std::string& source_name) {
    if (size < sizeof(Col03Header)) {
        LOG_ERROR("Truncated AA3COL03 header in %s (size %zu)", source_name.c_str(), size);
        return false;
    }

    Col03Header hdr;
    std::memcpy(&hdr, data, sizeof(Col03Header));

    if (hdr.version != 3) {
        LOG_ERROR("Unsupported AA3COL03 version %u in %s", hdr.version, source_name.c_str());
        return false;
    }

    if (hdr.meshes_offset > hdr.instances_offset ||
        hdr.instances_offset > hdr.direct_offset ||
        hdr.direct_offset > size) {
        LOG_ERROR("Corrupt section offsets in %s", source_name.c_str());
        return false;
    }

    // 1. Unique Meshes & per-mesh BLAS
    meshes_.resize(hdr.num_meshes);
    const uint8_t* mptr = data + hdr.meshes_offset;
    for (uint32_t i = 0; i < hdr.num_meshes; ++i) {
        if (mptr + 32 > data + hdr.instances_offset) {
            LOG_ERROR("Truncated mesh header at mesh %u in %s", i, source_name.c_str());
            return false;
        }
        uint32_t mesh_id = 0, tri_count = 0;
        std::memcpy(&mesh_id, mptr, 4);
        std::memcpy(&tri_count, mptr + 4, 4);
        UniqueMesh& m = meshes_[i];
        m.mesh_id = mesh_id;
        std::memcpy(&m.bounds_min, mptr + 8, 12);
        std::memcpy(&m.bounds_max, mptr + 20, 12);
        mptr += 32;

        const size_t mesh_tris_size = static_cast<size_t>(tri_count) * 36;
        if (mptr + mesh_tris_size > data + hdr.instances_offset) {
            LOG_ERROR("Truncated mesh triangles at mesh %u in %s", i, source_name.c_str());
            return false;
        }

        m.tris.resize(tri_count);
        for (uint32_t j = 0; j < tri_count; ++j) {
            float f[9];
            std::memcpy(f, mptr, 36);
            m.tris[j].v0 = {f[0], f[1], f[2]};
            m.tris[j].e1 = {f[3], f[4], f[5]};
            m.tris[j].e2 = {f[6], f[7], f[8]};
            mptr += 36;
        }

        BuildMeshBlas(m);
    }

    // 2. Instances Table
    const size_t inst_table_size = static_cast<size_t>(hdr.num_instances) * 120;
    if (data + hdr.instances_offset + inst_table_size > data + hdr.direct_offset) {
        LOG_ERROR("Truncated instances table in %s", source_name.c_str());
        return false;
    }

    instances_.resize(hdr.num_instances);
    const uint8_t* iptr = data + hdr.instances_offset;
    for (uint32_t i = 0; i < hdr.num_instances; ++i) {
        MeshInstance& inst = instances_[i];
        std::memcpy(&inst.mesh_idx, iptr, 4);
        std::memcpy(&inst.world_min, iptr + 4, 12);
        std::memcpy(&inst.world_max, iptr + 16, 12);
        std::memcpy(&inst.loc, iptr + 28, 12);
        std::memcpy(inst.inv_mat, iptr + 40, 36);
        std::memcpy(inst.inv_trans, iptr + 76, 36);
        inst.flags = iptr[112];
        std::memcpy(&inst.owner, iptr + 113, 4);
        iptr += 120;
    }

    // 3. Direct Triangles Table (BSP + Terrain)
    const size_t direct_table_size = static_cast<size_t>(hdr.num_direct_tris) * 41;
    if (data + hdr.direct_offset + direct_table_size > data + size) {
        LOG_ERROR("Truncated direct triangles table in %s", source_name.c_str());
        return false;
    }

    tris_.resize(hdr.num_direct_tris);
    const uint8_t* dptr = data + hdr.direct_offset;
    for (uint32_t i = 0; i < hdr.num_direct_tris; ++i) {
        Tri& t = tris_[i];
        float f[9];
        std::memcpy(f, dptr, 36);
        t.v0 = {f[0], f[1], f[2]};
        t.e1 = {f[3], f[4], f[5]};
        t.e2 = {f[6], f[7], f[8]};
        t.flags = dptr[36];
        std::memcpy(&t.owner, dptr + 37, 4);
        dptr += 41;
    }

    total_triangles_ = hdr.total_equivalent_tris;

    if (!tris_.empty()) {
        BuildFlatBvh();
    }
    if (!instances_.empty()) {
        BuildTlas();
    }

    LOG_INFO("Collision (COL03): %u unique meshes, %u instances, %u direct tris "
             "(%zu equivalent tris) from %s",
             hdr.num_meshes, hdr.num_instances, hdr.num_direct_tris,
             total_triangles_, source_name.c_str());
    return true;
}

bool CollisionWorld::LoadCol04(const uint8_t* data, size_t size, const std::string& source_name) {
    if (size < sizeof(Col04Header)) {
        LOG_ERROR("Truncated AA3COL04 header in %s (size %zu)", source_name.c_str(), size);
        return false;
    }

    Col04Header hdr;
    std::memcpy(&hdr, data, sizeof(Col04Header));

    if (hdr.version != 4) {
        LOG_ERROR("Unsupported AA3COL04 version %u in %s", hdr.version, source_name.c_str());
        return false;
    }

    if (hdr.meshes_offset > hdr.instances_offset ||
        hdr.instances_offset > hdr.direct_offset ||
        hdr.direct_offset > size) {
        LOG_ERROR("Corrupt section offsets in %s", source_name.c_str());
        return false;
    }

    // 1. Quantized Meshes & per-mesh BLAS
    meshes_.resize(hdr.num_meshes);
    const uint8_t* mptr = data + hdr.meshes_offset;
    for (uint32_t i = 0; i < hdr.num_meshes; ++i) {
        if (mptr + 36 > data + hdr.instances_offset) {
            LOG_ERROR("Truncated mesh header at mesh %u in %s", i, source_name.c_str());
            return false;
        }
        uint32_t mesh_id = 0, num_verts = 0, num_tris = 0;
        std::memcpy(&mesh_id, mptr, 4);
        std::memcpy(&num_verts, mptr + 4, 4);
        std::memcpy(&num_tris, mptr + 8, 4);
        UniqueMesh& m = meshes_[i];
        m.mesh_id = mesh_id;
        std::memcpy(&m.bounds_min, mptr + 12, 12);
        std::memcpy(&m.bounds_max, mptr + 24, 12);
        mptr += 36;

        const size_t v_bytes = static_cast<size_t>(num_verts) * 6;
        const size_t idx_bytes = static_cast<size_t>(num_tris) * 6;
        if (mptr + v_bytes + idx_bytes > data + hdr.instances_offset) {
            LOG_ERROR("Truncated mesh data at mesh %u in %s", i, source_name.c_str());
            return false;
        }

        const uint16_t* qv = reinterpret_cast<const uint16_t*>(mptr);
        mptr += v_bytes;
        const uint16_t* idx = reinterpret_cast<const uint16_t*>(mptr);
        mptr += idx_bytes;

        // Dequantize vertices to float
        std::vector<Vec3> dverts(num_verts);
        const float extent[3] = {
            m.bounds_max.x - m.bounds_min.x,
            m.bounds_max.y - m.bounds_min.y,
            m.bounds_max.z - m.bounds_min.z
        };
        for (uint32_t v = 0; v < num_verts; ++v) {
            dverts[v].x = m.bounds_min.x + (qv[v * 3 + 0] / 65535.0f) * extent[0];
            dverts[v].y = m.bounds_min.y + (qv[v * 3 + 1] / 65535.0f) * extent[1];
            dverts[v].z = m.bounds_min.z + (qv[v * 3 + 2] / 65535.0f) * extent[2];
        }

        // Expand indexed triangles into local Moller-Trumbore triangles
        m.tris.resize(num_tris);
        for (uint32_t t = 0; t < num_tris; ++t) {
            uint16_t i0 = idx[t * 3 + 0];
            uint16_t i1 = idx[t * 3 + 1];
            uint16_t i2 = idx[t * 3 + 2];
            const Vec3& p0 = (i0 < num_verts) ? dverts[i0] : Vec3{};
            const Vec3& p1 = (i1 < num_verts) ? dverts[i1] : Vec3{};
            const Vec3& p2 = (i2 < num_verts) ? dverts[i2] : Vec3{};
            m.tris[t].v0 = p0;
            m.tris[t].e1 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
            m.tris[t].e2 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        }

        BuildMeshBlas(m);
    }

    // 2. Instances Table
    const size_t inst_table_size = static_cast<size_t>(hdr.num_instances) * 120;
    if (data + hdr.instances_offset + inst_table_size > data + hdr.direct_offset) {
        LOG_ERROR("Truncated instances table in %s", source_name.c_str());
        return false;
    }

    instances_.resize(hdr.num_instances);
    const uint8_t* iptr = data + hdr.instances_offset;
    for (uint32_t i = 0; i < hdr.num_instances; ++i) {
        MeshInstance& inst = instances_[i];
        std::memcpy(&inst.mesh_idx, iptr, 4);
        std::memcpy(&inst.world_min, iptr + 4, 12);
        std::memcpy(&inst.world_max, iptr + 16, 12);
        std::memcpy(&inst.loc, iptr + 28, 12);
        std::memcpy(inst.inv_mat, iptr + 40, 36);
        std::memcpy(inst.inv_trans, iptr + 76, 36);
        inst.flags = iptr[112];
        std::memcpy(&inst.owner, iptr + 113, 4);
        iptr += 120;
    }

    // 3. Direct Triangles Table (BSP + Terrain)
    const size_t direct_table_size = static_cast<size_t>(hdr.num_direct_tris) * 41;
    if (data + hdr.direct_offset + direct_table_size > data + size) {
        LOG_ERROR("Truncated direct triangles table in %s", source_name.c_str());
        return false;
    }

    tris_.resize(hdr.num_direct_tris);
    const uint8_t* dptr = data + hdr.direct_offset;
    for (uint32_t i = 0; i < hdr.num_direct_tris; ++i) {
        Tri& t = tris_[i];
        float f[9];
        std::memcpy(f, dptr, 36);
        t.v0 = {f[0], f[1], f[2]};
        t.e1 = {f[3], f[4], f[5]};
        t.e2 = {f[6], f[7], f[8]};
        t.flags = dptr[36];
        std::memcpy(&t.owner, dptr + 37, 4);
        dptr += 41;
    }

    total_triangles_ = hdr.total_equivalent_tris;

    if (!tris_.empty()) {
        BuildFlatBvh();
    }
    if (!instances_.empty()) {
        BuildTlas();
    }

    LOG_INFO("Collision (COL04 - Quantized): %u unique meshes, %u instances, %u direct tris "
             "(%zu equivalent tris) from %s",
             hdr.num_meshes, hdr.num_instances, hdr.num_direct_tris,
             total_triangles_, source_name.c_str());
    return true;
}

bool CollisionWorld::Load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LOG_WARN("No collision file %s: traces disabled", path.c_str());
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size <= 0) {
        LOG_ERROR("Empty collision file %s", path.c_str());
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
        LOG_ERROR("Failed to read collision file %s", path.c_str());
        return false;
    }
    return LoadFromMemory(buf.data(), buf.size(), path);
}

void CollisionWorld::BuildFlatBvh() {
    order_.resize(tris_.size());
    std::vector<Vec3> cent(tris_.size());
    for (uint32_t i = 0; i < tris_.size(); ++i) {
        order_[i] = i;
        const Tri& t = tris_[i];
        cent[i] = {t.v0.x + (t.e1.x + t.e2.x) / 3.0f,
                   t.v0.y + (t.e1.y + t.e2.y) / 3.0f,
                   t.v0.z + (t.e1.z + t.e2.z) / 3.0f};
    }
    nodes_.clear();
    nodes_.reserve(tris_.size() / kLeafSize * 2 + 1);
    BuildRange(0, static_cast<uint32_t>(tris_.size()), cent);
}

uint32_t CollisionWorld::BuildRange(uint32_t begin, uint32_t end, std::vector<Vec3>& cent) {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    Vec3 clo = lo, chi = hi;
    for (uint32_t i = begin; i < end; ++i) {
        const Tri& t = tris_[order_[i]];
        const Vec3 p[3] = {t.v0,
                           {t.v0.x + t.e1.x, t.v0.y + t.e1.y, t.v0.z + t.e1.z},
                           {t.v0.x + t.e2.x, t.v0.y + t.e2.y, t.v0.z + t.e2.z}};
        for (const Vec3& q : p) {
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        const Vec3& c = cent[order_[i]];
        clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
        chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
    }
    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({lo, hi, begin, end - begin});
    if (end - begin <= kLeafSize) return index;

    const Vec3 ext = Sub(chi, clo);
    const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end,
                     [&](uint32_t a, uint32_t b) { return Axis(cent[a], axis) < Axis(cent[b], axis); });
    BuildRange(begin, mid, cent);
    const uint32_t right = BuildRange(mid, end, cent);
    nodes_[index].first = right;
    nodes_[index].count = 0;
    return index;
}

void CollisionWorld::BuildMeshBlas(UniqueMesh& mesh) {
    if (mesh.tris.empty()) {
        mesh.bvh.clear();
        return;
    }
    mesh.order.resize(mesh.tris.size());
    std::vector<Vec3> cent(mesh.tris.size());
    for (uint32_t i = 0; i < mesh.tris.size(); ++i) {
        mesh.order[i] = i;
        const auto& t = mesh.tris[i];
        cent[i] = {t.v0.x + (t.e1.x + t.e2.x) / 3.0f,
                   t.v0.y + (t.e1.y + t.e2.y) / 3.0f,
                   t.v0.z + (t.e1.z + t.e2.z) / 3.0f};
    }
    mesh.bvh.clear();
    mesh.bvh.reserve(mesh.tris.size() / kLeafSize * 2 + 1);
    BuildMeshRange(mesh, 0, static_cast<uint32_t>(mesh.tris.size()), cent);
}

uint32_t CollisionWorld::BuildMeshRange(UniqueMesh& mesh, uint32_t begin, uint32_t end, std::vector<Vec3>& cent) {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    Vec3 clo = lo, chi = hi;
    for (uint32_t i = begin; i < end; ++i) {
        const auto& t = mesh.tris[mesh.order[i]];
        const Vec3 p[3] = {t.v0,
                           {t.v0.x + t.e1.x, t.v0.y + t.e1.y, t.v0.z + t.e1.z},
                           {t.v0.x + t.e2.x, t.v0.y + t.e2.y, t.v0.z + t.e2.z}};
        for (const Vec3& q : p) {
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        const Vec3& c = cent[mesh.order[i]];
        clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
        chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
    }
    const uint32_t index = static_cast<uint32_t>(mesh.bvh.size());
    mesh.bvh.push_back({lo, hi, begin, end - begin});
    if (end - begin <= kLeafSize) return index;

    const Vec3 ext = Sub(chi, clo);
    const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(mesh.order.begin() + begin, mesh.order.begin() + mid, mesh.order.begin() + end,
                     [&](uint32_t a, uint32_t b) { return Axis(cent[a], axis) < Axis(cent[b], axis); });
    BuildMeshRange(mesh, begin, mid, cent);
    const uint32_t right = BuildMeshRange(mesh, mid, end, cent);
    mesh.bvh[index].first = right;
    mesh.bvh[index].count = 0;
    return index;
}

void CollisionWorld::BuildTlas() {
    if (instances_.empty()) {
        tlas_nodes_.clear();
        return;
    }
    tlas_order_.resize(instances_.size());
    std::vector<Vec3> cent(instances_.size());
    for (uint32_t i = 0; i < instances_.size(); ++i) {
        tlas_order_[i] = i;
        const auto& inst = instances_[i];
        cent[i] = {(inst.world_min.x + inst.world_max.x) * 0.5f,
                   (inst.world_min.y + inst.world_max.y) * 0.5f,
                   (inst.world_min.z + inst.world_max.z) * 0.5f};
    }
    tlas_nodes_.clear();
    tlas_nodes_.reserve(instances_.size() / kTlasLeafSize * 2 + 1);
    BuildTlasRange(0, static_cast<uint32_t>(instances_.size()), cent);
}

uint32_t CollisionWorld::BuildTlasRange(uint32_t begin, uint32_t end, std::vector<Vec3>& cent) {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    Vec3 clo = lo, chi = hi;
    for (uint32_t i = begin; i < end; ++i) {
        const auto& inst = instances_[tlas_order_[i]];
        lo = {std::min(lo.x, inst.world_min.x), std::min(lo.y, inst.world_min.y), std::min(lo.z, inst.world_min.z)};
        hi = {std::max(hi.x, inst.world_max.x), std::max(hi.y, inst.world_max.y), std::max(hi.z, inst.world_max.z)};
        const Vec3& c = cent[tlas_order_[i]];
        clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
        chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
    }
    const uint32_t index = static_cast<uint32_t>(tlas_nodes_.size());
    tlas_nodes_.push_back({lo, hi, begin, end - begin});
    if (end - begin <= kTlasLeafSize) return index;

    const Vec3 ext = Sub(chi, clo);
    const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(tlas_order_.begin() + begin, tlas_order_.begin() + mid, tlas_order_.begin() + end,
                     [&](uint32_t a, uint32_t b) { return Axis(cent[a], axis) < Axis(cent[b], axis); });
    BuildTlasRange(begin, mid, cent);
    const uint32_t right = BuildTlasRange(mid, end, cent);
    tlas_nodes_[index].first = right;
    tlas_nodes_[index].count = 0;
    return index;
}

TraceHit CollisionWorld::LineTrace(const Vec3& start, const Vec3& end, uint8_t mask) const {
    TraceHit best;
    const Vec3 d = Sub(end, start);
    const Vec3 inv{d.x != 0 ? 1.0f / d.x : 1e30f,
                   d.y != 0 ? 1.0f / d.y : 1e30f,
                   d.z != 0 ? 1.0f / d.z : 1e30f};
    float tmax = 1.0f;
    uint32_t best_direct_tri = 0;
    bool hit_direct = false;
    const MeshInstance* best_inst = nullptr;
    const UniqueMesh::LocalTri* best_mesh_tri = nullptr;

    // 1. Direct Triangles BVH
    if (!nodes_.empty()) {
        uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& n = nodes_[stack[--sp]];
            float t0 = 0.0f, t1 = tmax;
            bool miss = false;
            for (int k = 0; k < 3 && !miss; ++k) {
                const float o = Axis(start, k), iv = Axis(inv, k);
                float a = (Axis(n.lo, k) - o) * iv, b = (Axis(n.hi, k) - o) * iv;
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a);
                t1 = std::min(t1, b);
                miss = t0 > t1;
            }
            if (miss) continue;
            if (n.count > 0) {
                for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                    const Tri& t = tris_[order_[i]];
                    if (!(t.flags & mask)) continue;
                    if (t.owner && !disabled_owners_.empty() &&
                        std::find(disabled_owners_.begin(), disabled_owners_.end(), t.owner) != disabled_owners_.end())
                        continue;
                    const Vec3 pv = Cross(d, t.e2);
                    const float det = Dot(t.e1, pv);
                    if (std::fabs(det) < 1e-12f) continue;
                    const float id = 1.0f / det;
                    const Vec3 tv = Sub(start, t.v0);
                    const float u = Dot(tv, pv) * id;
                    if (u < 0 || u > 1) continue;
                    const Vec3 qv = Cross(tv, t.e1);
                    const float v = Dot(d, qv) * id;
                    if (v < 0 || u + v > 1) continue;
                    const float tt = Dot(t.e2, qv) * id;
                    if (tt < 0 || tt > tmax) continue;
                    tmax = tt;
                    best.hit = true;
                    best.time = tt;
                    best.owner = t.owner;
                    best_direct_tri = order_[i];
                    hit_direct = true;
                    best_inst = nullptr;
                    best_mesh_tri = nullptr;
                }
            } else {
                const uint32_t self = static_cast<uint32_t>(&n - nodes_.data());
                if (sp + 2 <= 64) {
                    stack[sp++] = n.first;     // right
                    stack[sp++] = self + 1;    // left
                }
            }
        }
    }

    // 2. Instanced Meshes TLAS
    if (!tlas_nodes_.empty()) {
        uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& n = tlas_nodes_[stack[--sp]];
            float t0 = 0.0f, t1 = tmax;
            bool miss = false;
            for (int k = 0; k < 3 && !miss; ++k) {
                const float o = Axis(start, k), iv = Axis(inv, k);
                float a = (Axis(n.lo, k) - o) * iv, b = (Axis(n.hi, k) - o) * iv;
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a);
                t1 = std::min(t1, b);
                miss = t0 > t1;
            }
            if (miss) continue;

            if (n.count > 0) {
                for (uint32_t idx = n.first; idx < n.first + n.count; ++idx) {
                    const MeshInstance& inst = instances_[tlas_order_[idx]];
                    if (!(inst.flags & mask)) continue;
                    if (inst.owner && !disabled_owners_.empty() &&
                        std::find(disabled_owners_.begin(), disabled_owners_.end(), inst.owner) != disabled_owners_.end())
                        continue;

                    // Box test against instance world bounds within [0, tmax]
                    float it0 = 0.0f, it1 = tmax;
                    bool imiss = false;
                    for (int k = 0; k < 3 && !imiss; ++k) {
                        const float o = Axis(start, k), iv = Axis(inv, k);
                        float a = (Axis(inst.world_min, k) - o) * iv, b = (Axis(inst.world_max, k) - o) * iv;
                        if (a > b) std::swap(a, b);
                        it0 = std::max(it0, a);
                        it1 = std::min(it1, b);
                        imiss = it0 > it1;
                    }
                    if (imiss) continue;

                    if (inst.mesh_idx >= meshes_.size()) continue;
                    const UniqueMesh& mesh = meshes_[inst.mesh_idx];
                    if (mesh.bvh.empty()) continue;

                    // Transform ray to instance local space
                    const Vec3 start_l = TransformVec(Sub(start, inst.loc), inst.inv_mat);
                    const Vec3 d_l = TransformVec(d, inst.inv_mat);
                    const Vec3 inv_l{d_l.x != 0 ? 1.0f / d_l.x : 1e30f,
                                     d_l.y != 0 ? 1.0f / d_l.y : 1e30f,
                                     d_l.z != 0 ? 1.0f / d_l.z : 1e30f};

                    // Traverse mesh BLAS
                    uint32_t bstack[64];
                    int bsp = 0;
                    bstack[bsp++] = 0;
                    while (bsp > 0) {
                        const Node& bn = mesh.bvh[bstack[--bsp]];
                        float bt0 = 0.0f, bt1 = tmax;
                        bool bmiss = false;
                        for (int k = 0; k < 3 && !bmiss; ++k) {
                            const float o = Axis(start_l, k), iv = Axis(inv_l, k);
                            float a = (Axis(bn.lo, k) - o) * iv, b = (Axis(bn.hi, k) - o) * iv;
                            if (a > b) std::swap(a, b);
                            bt0 = std::max(bt0, a);
                            bt1 = std::min(bt1, b);
                            bmiss = bt0 > bt1;
                        }
                        if (bmiss) continue;

                        if (bn.count > 0) {
                            for (uint32_t ti = bn.first; ti < bn.first + bn.count; ++ti) {
                                const auto& lt = mesh.tris[mesh.order[ti]];
                                const Vec3 pv = Cross(d_l, lt.e2);
                                const float det = Dot(lt.e1, pv);
                                if (std::fabs(det) < 1e-12f) continue;
                                const float idet = 1.0f / det;
                                const Vec3 tv = Sub(start_l, lt.v0);
                                const float u = Dot(tv, pv) * idet;
                                if (u < 0 || u > 1) continue;
                                const Vec3 qv = Cross(tv, lt.e1);
                                const float v = Dot(d_l, qv) * idet;
                                if (v < 0 || u + v > 1) continue;
                                const float tt = Dot(lt.e2, qv) * idet;
                                if (tt < 0 || tt > tmax) continue;

                                tmax = tt;
                                best.hit = true;
                                best.time = tt;
                                best.owner = inst.owner;
                                best_inst = &inst;
                                best_mesh_tri = &lt;
                                hit_direct = false;
                            }
                        } else {
                            const uint32_t bself = static_cast<uint32_t>(&bn - mesh.bvh.data());
                            if (bsp + 2 <= 64) {
                                bstack[bsp++] = bn.first;     // right
                                bstack[bsp++] = bself + 1;    // left
                            }
                        }
                    }
                }
            } else {
                const uint32_t self = static_cast<uint32_t>(&n - tlas_nodes_.data());
                if (sp + 2 <= 64) {
                    stack[sp++] = n.first;     // right
                    stack[sp++] = self + 1;    // left
                }
            }
        }
    }

    if (best.hit) {
        best.location = {start.x + d.x * best.time, start.y + d.y * best.time, start.z + d.z * best.time};
        Vec3 nrm{0, 0, 1};
        if (hit_direct) {
            const Tri& t = tris_[best_direct_tri];
            nrm = Cross(t.e1, t.e2);
            best.triangle = best_direct_tri;
        } else if (best_inst && best_mesh_tri) {
            Vec3 lnrm = Cross(best_mesh_tri->e1, best_mesh_tri->e2);
            nrm = TransformVec(lnrm, best_inst->inv_trans);
            best.triangle = 0;
        }
        const float len = std::sqrt(Dot(nrm, nrm));
        if (len > 0) nrm = {nrm.x / len, nrm.y / len, nrm.z / len};
        if (Dot(nrm, d) > 0) nrm = {-nrm.x, -nrm.y, -nrm.z};
        best.normal = nrm;
    }
    return best;
}

void CollisionWorld::SetOwnerEnabled(uint32_t owner, bool enabled) {
    auto it = std::find(disabled_owners_.begin(), disabled_owners_.end(), owner);
    if (enabled && it != disabled_owners_.end()) disabled_owners_.erase(it);
    if (!enabled && it == disabled_owners_.end()) disabled_owners_.push_back(owner);
}

} // namespace game
} // namespace aa3
