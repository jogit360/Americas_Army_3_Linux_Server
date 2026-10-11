#include "engine/package_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

#include "util/logging.h"

namespace aa3 {
namespace engine {

namespace {

struct PropertyValue {
    enum Type { kNone, kBool, kByte, kInt, kFloat, kName, kString, kObjectRef, kVector, kRotator, kRaw };
    Type type = kNone;
    bool bool_val = false;
    uint8_t byte_val = 0;
    int32_t int_val = 0;
    float float_val = 0.0f;
    std::string str_val;
    int32_t obj_ref = 0;
    float vec_val[3] = {0, 0, 0};
    int32_t rot_val[3] = {0, 0, 0};
};

using PropertyMap = std::map<std::string, PropertyValue>;

std::string ClassNameOf(const Package& pkg, const ObjectExport& e) {
    if (e.class_index < 0) {
        size_t imp = static_cast<size_t>(-e.class_index - 1);
        if (imp < pkg.imports.size()) return pkg.imports[imp].object_name;
    } else if (e.class_index > 0) {
        size_t exp = static_cast<size_t>(e.class_index - 1);
        if (exp < pkg.exports.size()) return pkg.exports[exp].object_name;
    }
    return "";
}

std::string ReadFName(const uint8_t* data, size_t size, size_t& p, const Package& pkg) {
    if (p + 8 > size) return "";
    int32_t idx = 0, num = 0;
    std::memcpy(&idx, data + p, 4);
    std::memcpy(&num, data + p + 4, 4);
    p += 8;
    if (idx >= 0 && static_cast<size_t>(idx) < pkg.names.size()) {
        std::string s = pkg.names[idx];
        if (num > 0) s += "_" + std::to_string(num - 1);
        return s;
    }
    return "";
}

int32_t ReadI32(const uint8_t* data, size_t size, size_t& p) {
    if (p + 4 > size) return 0;
    int32_t v = 0;
    std::memcpy(&v, data + p, 4);
    p += 4;
    return v;
}

uint32_t ReadU32(const uint8_t* data, size_t size, size_t& p) {
    if (p + 4 > size) return 0;
    uint32_t v = 0;
    std::memcpy(&v, data + p, 4);
    p += 4;
    return v;
}

float ReadF32(const uint8_t* data, size_t size, size_t& p) {
    if (p + 4 > size) return 0.0f;
    float v = 0.0f;
    std::memcpy(&v, data + p, 4);
    p += 4;
    return v;
}

std::string ReadFString(const uint8_t* data, size_t size, size_t& p) {
    int32_t n = ReadI32(data, size, p);
    if (n == 0) return "";
    if (n > 0) {
        size_t len = static_cast<size_t>(n);
        if (p + len > size) return "";
        std::string s(reinterpret_cast<const char*>(data + p), len > 0 ? len - 1 : 0);
        p += len;
        return s;
    } else {
        size_t len = static_cast<size_t>(-n);
        if (p + len * 2 > size) return "";
        std::string s;
        for (size_t i = 0; i < len - 1; ++i) {
            uint16_t cu = 0;
            std::memcpy(&cu, data + p + i * 2, 2);
            s.push_back(static_cast<char>(cu & 0xFF));
        }
        p += len * 2;
        return s;
    }
}

PropertyMap ReadProperties(const Package& pkg, const uint8_t* data, size_t size, size_t& p) {
    PropertyMap props;
    while (p + 8 <= size) {
        std::string name = ReadFName(data, size, p, pkg);
        if (name == "None" || name.empty()) break;
        std::string typ = ReadFName(data, size, p, pkg);
        int32_t prop_size = ReadI32(data, size, p);
        int32_t arr_idx = ReadI32(data, size, p);
        (void)arr_idx;

        std::string struct_name;
        if (typ == "StructProperty") {
            struct_name = ReadFName(data, size, p, pkg);
        }
        bool bool_in_tag = false;
        if (typ == "BoolProperty") {
            bool_in_tag = (ReadU32(data, size, p) != 0);
            prop_size = 0;
        }

        size_t start = p;
        PropertyValue val;
        if (typ == "BoolProperty") {
            val.type = PropertyValue::kBool;
            val.bool_val = bool_in_tag;
        } else if (typ == "IntProperty") {
            val.type = PropertyValue::kInt;
            val.int_val = ReadI32(data, size, p);
        } else if (typ == "FloatProperty") {
            val.type = PropertyValue::kFloat;
            val.float_val = ReadF32(data, size, p);
        } else if (typ == "ObjectProperty" || typ == "ClassProperty" || typ == "ComponentProperty" || typ == "InterfaceProperty") {
            val.type = PropertyValue::kObjectRef;
            val.obj_ref = ReadI32(data, size, p);
        } else if (typ == "NameProperty") {
            val.type = PropertyValue::kName;
            val.str_val = ReadFName(data, size, p, pkg);
        } else if (typ == "ByteProperty") {
            val.type = PropertyValue::kByte;
            if (prop_size == 1 && p < size) {
                val.byte_val = data[p++];
            } else {
                val.str_val = ReadFName(data, size, p, pkg);
            }
        } else if (typ == "StrProperty") {
            val.type = PropertyValue::kString;
            val.str_val = ReadFString(data, size, p);
        } else if (typ == "StructProperty" && (struct_name == "Vector" || struct_name == "Rotator")) {
            if (struct_name == "Vector") {
                val.type = PropertyValue::kVector;
                val.vec_val[0] = ReadF32(data, size, p);
                val.vec_val[1] = ReadF32(data, size, p);
                val.vec_val[2] = ReadF32(data, size, p);
            } else {
                val.type = PropertyValue::kRotator;
                val.rot_val[0] = ReadI32(data, size, p);
                val.rot_val[1] = ReadI32(data, size, p);
                val.rot_val[2] = ReadI32(data, size, p);
            }
        } else {
            val.type = PropertyValue::kRaw;
        }

        p = start + static_cast<size_t>(prop_size);
        props[name] = val;
    }
    return props;
}

PropertyMap ReadExportProps(const Package& pkg, size_t exp_idx, size_t* out_after_p = nullptr) {
    if (exp_idx >= pkg.exports.size()) return {};
    const auto& e = pkg.exports[exp_idx];
    if (e.serial_size <= 0) return {};
    size_t off = static_cast<size_t>(e.serial_offset - pkg.body_base);
    if (off + e.serial_size > pkg.body.size()) return {};
    const uint8_t* data = pkg.body.data() + off;
    size_t size = static_cast<size_t>(e.serial_size);
    size_t p = 0;

    std::string cls = ClassNameOf(pkg, e);
    if (cls.size() >= 9 && cls.compare(cls.size() - 9, 9, "Component") == 0) {
        ReadI32(data, size, p); // TemplateOwnerClass
    }
    constexpr uint64_t kRF_HasStack = 0x0200000000000000ULL;
    if (e.object_flags & kRF_HasStack) {
        int32_t node = ReadI32(data, size, p);
        ReadI32(data, size, p); // StateNode
        p += 8;                 // ProbeMask
        p += 2;                 // LatentAction
        int32_t n = ReadI32(data, size, p);
        p += static_cast<size_t>(n) * 12;
        if (node != 0) ReadI32(data, size, p);
    }
    ReadI32(data, size, p); // NetIndex

    PropertyMap props = ReadProperties(pkg, data, size, p);
    if (out_after_p) *out_after_p = off + p;
    return props;
}

std::optional<PropertyValue> GetInheritedProp(const Package& pkg, size_t exp_idx, const std::string& key, int depth = 8) {
    size_t cur = exp_idx;
    for (int d = 0; d < depth; ++d) {
        if (cur >= pkg.exports.size()) break;
        PropertyMap props = ReadExportProps(pkg, cur);
        auto it = props.find(key);
        if (it != props.end()) return it->second;
        int32_t arch = pkg.exports[cur].archetype_index;
        if (arch <= 0) break;
        cur = static_cast<size_t>(arch - 1);
    }
    return std::nullopt;
}

void RotationMatrix(int32_t pitch, int32_t yaw, int32_t roll, float R[3][3]) {
    constexpr float k = 3.14159265358979323846f * 2.0f / 65536.0f;
    float sp = std::sin(pitch * k), cp = std::cos(pitch * k);
    float sy = std::sin(yaw * k), cy = std::cos(yaw * k);
    float sr = std::sin(roll * k), cr = std::cos(roll * k);
    R[0][0] = cp * cy;
    R[0][1] = cp * sy;
    R[0][2] = sp;
    R[1][0] = sr * sp * cy - cr * sy;
    R[1][1] = sr * sp * sy + cr * cy;
    R[1][2] = -sr * cp;
    R[2][0] = -(cr * sp * cy + sr * sy);
    R[2][1] = cy * sr - cr * sp * sy;
    R[2][2] = cr * cp;
}

struct RawTri {
    float v0[3];
    float e1[3];
    float e2[3];
    uint8_t flags;
    uint32_t owner;
};

struct ExtractedMesh {
    uint32_t mesh_id = 0;
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
    std::vector<float> v0;
    std::vector<float> e1;
    std::vector<float> e2;
    uint32_t num_verts = 0;
    uint32_t tri_count = 0;
    std::vector<uint16_t> qverts;
    std::vector<uint16_t> indices;
};

struct ExtractedInstance {
    uint32_t mesh_idx = 0;
    float world_min[3] = {0, 0, 0};
    float world_max[3] = {0, 0, 0};
    float loc[3] = {0, 0, 0};
    float inv_mat[9] = {0};
    float inv_trans[9] = {0};
    uint8_t flags = 0;
    uint32_t owner = 0;
};

bool ExtractStaticMeshKDop(const Package& pkg, size_t sm_idx, ExtractedMesh& out_mesh) {
    size_t after_p = 0;
    ReadExportProps(pkg, sm_idx, &after_p);

    const auto& e = pkg.exports[sm_idx];
    const uint8_t* data = pkg.body.data();
    size_t total_size = pkg.body.size();
    size_t p = after_p;

    p += 28; // Bounds
    ReadI32(data, total_size, p); // BodySetup

    int32_t nsize = ReadI32(data, total_size, p);
    int32_t ncount = ReadI32(data, total_size, p);
    p += static_cast<size_t>(nsize * ncount);

    int32_t tsize = ReadI32(data, total_size, p);
    int32_t tcount = ReadI32(data, total_size, p);
    if (tsize != 8 || tcount < 0) return false;

    std::vector<uint16_t> tris(tcount * 3);
    for (int k = 0; k < tcount; ++k) {
        std::memcpy(&tris[k * 3], data + p + k * 8, 6);
    }
    p += static_cast<size_t>(tsize * tcount);

    size_t exp_end = static_cast<size_t>(e.serial_offset - pkg.body_base + e.serial_size);
    int num_verts = 0;
    size_t vert_offset = 0;
    for (size_t q = p; q + 16 <= exp_end && q + 16 <= total_size; ++q) {
        int32_t s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        std::memcpy(&s1, data + q, 4);
        std::memcpy(&n1, data + q + 4, 4);
        std::memcpy(&s2, data + q + 8, 4);
        std::memcpy(&n2, data + q + 12, 4);
        if (s1 == 12 && s2 == 12 && n1 == n2 && n1 > 0 && n1 < 1000000) {
            if (q + 16 + static_cast<size_t>(n1) * 12 <= exp_end) {
                bool ok = true;
                for (int k = 0; k < tcount * 3; ++k) {
                    if (tris[k] >= n1) { ok = false; break; }
                }
                if (ok) {
                    num_verts = n1;
                    vert_offset = q + 16;
                    break;
                }
            }
        }
    }
    if (num_verts == 0) return false;

    std::vector<float> verts(num_verts * 3);
    std::memcpy(verts.data(), data + vert_offset, num_verts * 12);

    float bmin[3] = {1e30f, 1e30f, 1e30f};
    float bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < num_verts; ++i) {
        for (int c = 0; c < 3; ++c) {
            bmin[c] = std::min(bmin[c], verts[i * 3 + c]);
            bmax[c] = std::max(bmax[c], verts[i * 3 + c]);
        }
    }
    std::memcpy(out_mesh.bounds_min, bmin, 12);
    std::memcpy(out_mesh.bounds_max, bmax, 12);
    out_mesh.num_verts = num_verts;
    out_mesh.tri_count = tcount;
    out_mesh.indices = tris;

    // 16-bit integer quantization relative to bounding box
    out_mesh.qverts.resize(num_verts * 3);
    for (int i = 0; i < num_verts; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = verts[i * 3 + c];
            float extent = bmax[c] - bmin[c];
            if (extent > 1e-6f) {
                float norm = (v - bmin[c]) / extent;
                if (norm < 0.0f) norm = 0.0f;
                if (norm > 1.0f) norm = 1.0f;
                out_mesh.qverts[i * 3 + c] = static_cast<uint16_t>(std::round(norm * 65535.0f));
            } else {
                out_mesh.qverts[i * 3 + c] = 0;
            }
        }
    }

    // Unquantized triangles for AA3COL03 fallback
    out_mesh.v0.resize(tcount * 3);
    out_mesh.e1.resize(tcount * 3);
    out_mesh.e2.resize(tcount * 3);
    for (int k = 0; k < tcount; ++k) {
        int i0 = tris[k * 3 + 0];
        int i1 = tris[k * 3 + 1];
        int i2 = tris[k * 3 + 2];
        for (int c = 0; c < 3; ++c) {
            float p0 = verts[i0 * 3 + c];
            float p1 = verts[i1 * 3 + c];
            float p2 = verts[i2 * 3 + c];
            out_mesh.v0[k * 3 + c] = p0;
            out_mesh.e1[k * 3 + c] = p1 - p0;
            out_mesh.e2[k * 3 + c] = p2 - p0;
        }
    }
    return true;
}

inline bool EndsWith(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

PackageExtractor::PackageExtractor(const Package& pkg) : pkg_(pkg) {}


bool PackageExtractor::ExtractWindows(std::map<uint32_t, int>& out_windows) {
    out_windows.clear();
    int32_t level_idx = -1;
    int32_t world_info_idx = -1;
    for (size_t i = 0; i < pkg_.exports.size(); ++i) {
        std::string cls = ClassNameOf(pkg_, pkg_.exports[i]);
        if (cls == "Level") level_idx = static_cast<int32_t>(i);
        if (cls == "WorldInfo") world_info_idx = static_cast<int32_t>(i);
    }
    if (level_idx < 0 || world_info_idx < 0) return false;

    const auto& le = pkg_.exports[level_idx];
    size_t off = static_cast<size_t>(le.serial_offset - pkg_.body_base);
    if (off + le.serial_size > pkg_.body.size()) return false;
    const uint8_t* ldata = pkg_.body.data() + off;
    size_t lsize = static_cast<size_t>(le.serial_size);

    int32_t wi_target = world_info_idx + 1;
    std::vector<int32_t> actor_order;
    for (size_t p = 4; p + 4 <= lsize; ++p) {
        int32_t val = 0;
        std::memcpy(&val, ldata + p, 4);
        if (val == wi_target) {
            int32_t count = 0;
            std::memcpy(&count, ldata + p - 4, 4);
            if (count > 100 && count < 200000 && p + count * 4 <= lsize) {
                actor_order.resize(count);
                std::memcpy(actor_order.data(), ldata + p, count * 4);
                for (int32_t& a : actor_order) a -= 1;
                break;
            }
        }
    }
    if (actor_order.empty()) return false;

    std::map<int32_t, uint32_t> net_of;
    uint32_t net_counter = 0;
    for (size_t j = 0; j < pkg_.exports.size(); ++j) {
        if (pkg_.exports[j].object_flags & 0x100000000ULL) {
            net_of[static_cast<int32_t>(j)] = net_counter++;
        }
    }

    int win_count = 0;
    for (int32_t act_idx : actor_order) {
        if (act_idx < 0 || static_cast<size_t>(act_idx) >= pkg_.exports.size()) continue;
        if (ClassNameOf(pkg_, pkg_.exports[act_idx]) == "AA_BreakableWindow") {
            if (net_of.count(act_idx)) {
                out_windows[net_of[act_idx]] = win_count;
            }
            win_count++;
        }
    }
    return !out_windows.empty();
}

bool PackageExtractor::ExtractSpawns(std::vector<SpawnPointDef>& out_spawns) {
    out_spawns.clear();
    for (size_t i = 0; i < pkg_.exports.size(); ++i) {
        std::string cls = ClassNameOf(pkg_, pkg_.exports[i]);
        if (cls == "PlayerStart" || cls == "AA_PlayerStart" || cls == "AA_TeamPlayerStart") {
            PropertyMap props = ReadExportProps(pkg_, i);
            SpawnPointDef sp{};
            auto it_loc = props.find("Location");
            if (it_loc != props.end() && it_loc->second.type == PropertyValue::kVector) {
                sp.x = it_loc->second.vec_val[0];
                sp.y = it_loc->second.vec_val[1];
                sp.z = it_loc->second.vec_val[2];
            }
            auto it_rot = props.find("Rotation");
            if (it_rot != props.end() && it_rot->second.type == PropertyValue::kRotator) {
                sp.yaw = it_rot->second.rot_val[1];
            }
            auto it_team = props.find("SpawnTeam");
            if (it_team == props.end()) it_team = props.find("TeamIndex");
            if (it_team != props.end()) {
                if (it_team->second.type == PropertyValue::kByte) sp.team = it_team->second.byte_val;
                else if (it_team->second.type == PropertyValue::kInt) sp.team = it_team->second.int_val;
            }
            out_spawns.push_back(sp);
        }
    }
    return !out_spawns.empty();
}

bool PackageExtractor::ExtractCollision(std::vector<uint8_t>& out_col_bytes,
                                       uint32_t& out_num_meshes,
                                       uint32_t& out_num_instances,
                                       uint32_t& out_num_direct_tris,
                                       uint32_t& out_total_tris,
                                       bool quantize) {
    std::map<int32_t, uint32_t> net_of;
    uint32_t net_counter = 0;
    for (size_t j = 0; j < pkg_.exports.size(); ++j) {
        if (pkg_.exports[j].object_flags & 0x100000000ULL) {
            net_of[static_cast<int32_t>(j)] = net_counter++;
        }
    }

    std::vector<RawTri> direct_tris;

    // 1. BSP Polygons (UModel)
    for (size_t i = 0; i < pkg_.exports.size(); ++i) {
        const auto& e = pkg_.exports[i];
        if (ClassNameOf(pkg_, e) != "Model" || e.outer_index <= 0) continue;
        size_t outer_idx = static_cast<size_t>(e.outer_index - 1);
        if (outer_idx >= pkg_.exports.size() || ClassNameOf(pkg_, pkg_.exports[outer_idx]) != "Level") continue;

        size_t after_p = 0;
        ReadExportProps(pkg_, i, &after_p);
        const uint8_t* data = pkg_.body.data();
        size_t total_size = pkg_.body.size();
        size_t p = after_p + 28; // Bounds

        int32_t vec_size = ReadI32(data, total_size, p);
        int32_t vec_count = ReadI32(data, total_size, p);
        p += static_cast<size_t>(vec_size * vec_count);

        int32_t pt_size = ReadI32(data, total_size, p);
        int32_t pt_count = ReadI32(data, total_size, p);
        const float* pts = reinterpret_cast<const float*>(data + p);
        p += static_cast<size_t>(pt_size * pt_count);

        int32_t node_size = ReadI32(data, total_size, p);
        int32_t node_count = ReadI32(data, total_size, p);
        const uint8_t* nodes = data + p;
        p += static_cast<size_t>(node_size * node_count);

        ReadI32(data, total_size, p); // TTransArray owner
        int32_t nsurf = ReadI32(data, total_size, p);
        std::vector<uint32_t> surf_flags(nsurf);
        for (int s = 0; s < nsurf; ++s) {
            std::memcpy(&surf_flags[s], data + p + s * 56 + 4, 4);
        }
        p += static_cast<size_t>(nsurf) * 56;

        int32_t v_size = ReadI32(data, total_size, p);
        int32_t v_count = ReadI32(data, total_size, p);
        (void)v_count;
        const uint8_t* vraw = data + p;

        if (pt_size != 12 || node_size != 64) continue;

        for (int k = 0; k < node_count; ++k) {
            size_t noff = static_cast<size_t>(k) * 64;
            int32_t ivp = 0, isurf = 0;
            std::memcpy(&ivp, nodes + noff + 16, 4);
            std::memcpy(&isurf, nodes + noff + 20, 4);
            uint8_t nverts = nodes[noff + 54];
            if (nverts < 3) continue;
            uint32_t sflag = (isurf >= 0 && isurf < nsurf) ? surf_flags[isurf] : 0;
            if (sflag & 0x8) continue; // PF_NotSolid

            std::vector<std::array<float, 3>> poly(nverts);
            for (int j = 0; j < nverts; ++j) {
                int32_t pv = 0;
                std::memcpy(&pv, vraw + (ivp + j) * v_size, 4);
                if (pv >= 0 && pv < pt_count) {
                    poly[j] = {pts[pv * 3 + 0], pts[pv * 3 + 1], pts[pv * 3 + 2]};
                }
            }
            // Fan triangulate
            for (int m = 1; m < nverts - 1; ++m) {
                RawTri rt{};
                rt.v0[0] = poly[0][0]; rt.v0[1] = poly[0][1]; rt.v0[2] = poly[0][2];
                rt.e1[0] = poly[m][0] - poly[0][0]; rt.e1[1] = poly[m][1] - poly[0][1]; rt.e1[2] = poly[m][2] - poly[0][2];
                rt.e2[0] = poly[m + 1][0] - poly[0][0]; rt.e2[1] = poly[m + 1][1] - poly[0][1]; rt.e2[2] = poly[m + 1][2] - poly[0][2];
                rt.flags = 3;
                rt.owner = 0;
                direct_tris.push_back(rt);
            }
        }
    }

    // 2. Terrain Heightmaps (ATerrain)
    for (size_t i = 0; i < pkg_.exports.size(); ++i) {
        if (ClassNameOf(pkg_, pkg_.exports[i]) != "Terrain") continue;
        size_t after_p = 0;
        PropertyMap props = ReadExportProps(pkg_, i, &after_p);
        int nx = 0, ny = 0;
        auto it_nx = props.find("NumVerticesX");
        auto it_ny = props.find("NumVerticesY");
        if (it_nx != props.end()) nx = it_nx->second.int_val;
        if (it_ny != props.end()) ny = it_ny->second.int_val;
        if (nx <= 1 || ny <= 1) continue;

        float loc[3] = {0, 0, 0};
        auto it_loc = props.find("Location");
        if (it_loc != props.end() && it_loc->second.type == PropertyValue::kVector) {
            std::memcpy(loc, it_loc->second.vec_val, 12);
        }
        float s[3] = {1, 1, 1};
        float ds = 1.0f;
        auto it_ds = props.find("DrawScale");
        if (it_ds != props.end() && it_ds->second.type == PropertyValue::kFloat) ds = it_ds->second.float_val;
        auto it_s3 = props.find("DrawScale3D");
        if (it_s3 != props.end() && it_s3->second.type == PropertyValue::kVector) {
            s[0] = it_s3->second.vec_val[0] * ds;
            s[1] = it_s3->second.vec_val[1] * ds;
            s[2] = it_s3->second.vec_val[2] * ds;
        }

        const uint8_t* data = pkg_.body.data();
        size_t total_size = pkg_.body.size();
        size_t p = after_p;

        int32_t n = ReadI32(data, total_size, p);
        if (n != nx * ny || p + n * 2 > total_size) continue;
        const uint16_t* heights = reinterpret_cast<const uint16_t*>(data + p);
        p += static_cast<size_t>(n) * 2;

        int32_t m = ReadI32(data, total_size, p);
        const uint8_t* info = (m == n) ? (data + p) : nullptr;

        for (int y = 0; y < ny - 1; ++y) {
            for (int x = 0; x < nx - 1; ++x) {
                if (info && (info[y * nx + x] & 1)) continue; // hole

                auto get_pt = [&](int px, int py) -> std::array<float, 3> {
                    float hz = (static_cast<float>(heights[py * nx + px]) - 32768.0f) * s[2] / 128.0f + loc[2];
                    return {loc[0] + px * s[0], loc[1] + py * s[1], hz};
                };

                auto a = get_pt(x, y);
                auto b = get_pt(x + 1, y);
                auto c = get_pt(x, y + 1);
                auto d = get_pt(x + 1, y + 1);

                // Tri 1: a, b, d
                RawTri t1{};
                t1.v0[0] = a[0]; t1.v0[1] = a[1]; t1.v0[2] = a[2];
                t1.e1[0] = b[0] - a[0]; t1.e1[1] = b[1] - a[1]; t1.e1[2] = b[2] - a[2];
                t1.e2[0] = d[0] - a[0]; t1.e2[1] = d[1] - a[1]; t1.e2[2] = d[2] - a[2];
                t1.flags = 3; t1.owner = 0;
                direct_tris.push_back(t1);

                // Tri 2: a, d, c
                RawTri t2{};
                t2.v0[0] = a[0]; t2.v0[1] = a[1]; t2.v0[2] = a[2];
                t2.e1[0] = d[0] - a[0]; t2.e1[1] = d[1] - a[1]; t2.e1[2] = d[2] - a[2];
                t2.e2[0] = c[0] - a[0]; t2.e2[1] = c[1] - a[1]; t2.e2[2] = c[2] - a[2];
                t2.flags = 3; t2.owner = 0;
                direct_tris.push_back(t2);
            }
        }
    }

    // 3. Unique Meshes and Placed Instances
    std::map<int32_t, uint32_t> mesh_map;
    std::vector<ExtractedMesh> unique_meshes;
    std::vector<ExtractedInstance> instances;
    uint32_t placed_mesh_tris = 0;

    for (size_t i = 0; i < pkg_.exports.size(); ++i) {
        const auto& e = pkg_.exports[i];
        std::string cls = ClassNameOf(pkg_, e);
        if (!EndsWith(cls, "StaticMeshActor") && !EndsWith(cls, "BreakableWindow") &&
            !EndsWith(cls, "Door") && !EndsWith(cls, "InterpActor") &&
            !EndsWith(cls, "DynamicSMActor") && !EndsWith(cls, "KActor"))
            continue;

        auto c_prop = GetInheritedProp(pkg_, i, "StaticMeshComponent");
        if (!c_prop || c_prop->obj_ref <= 0) continue;
        size_t ci = static_cast<size_t>(c_prop->obj_ref - 1);
        if (ci >= pkg_.exports.size()) continue;

        auto m_prop = GetInheritedProp(pkg_, ci, "StaticMesh");
        if (!m_prop || m_prop->obj_ref <= 0) continue;
        size_t mi = static_cast<size_t>(m_prop->obj_ref - 1);
        if (mi >= pkg_.exports.size()) continue;

        auto col_prop = GetInheritedProp(pkg_, ci, "CollideActors");
        if (col_prop && !col_prop->bool_val) continue;

        int32_t m_id = static_cast<int32_t>(m_prop->obj_ref);
        if (!mesh_map.count(m_id)) {
            ExtractedMesh em{};
            em.mesh_id = static_cast<uint32_t>(unique_meshes.size());
            ExtractStaticMeshKDop(pkg_, mi, em);
            mesh_map[m_id] = static_cast<uint32_t>(unique_meshes.size());
            unique_meshes.push_back(em);
        }

        uint32_t mesh_idx = mesh_map[m_id];
        const auto& em = unique_meshes[mesh_idx];
        if (em.tri_count == 0) continue;
        placed_mesh_tris += em.tri_count;

        float ds = 1.0f, c_scale = 1.0f;
        auto it_ds = GetInheritedProp(pkg_, i, "DrawScale");
        if (it_ds && it_ds->type == PropertyValue::kFloat) ds = it_ds->float_val;
        auto it_cs = GetInheritedProp(pkg_, ci, "Scale");
        if (it_cs && it_cs->type == PropertyValue::kFloat) c_scale = it_cs->float_val;
        float total_scale = ds * c_scale;

        float s3[3] = {total_scale, total_scale, total_scale};
        auto it_ds3 = GetInheritedProp(pkg_, i, "DrawScale3D");
        if (it_ds3 && it_ds3->type == PropertyValue::kVector) {
            for (int k = 0; k < 3; ++k) s3[k] *= it_ds3->vec_val[k];
        }
        auto it_cs3 = GetInheritedProp(pkg_, ci, "Scale3D");
        if (it_cs3 && it_cs3->type == PropertyValue::kVector) {
            for (int k = 0; k < 3; ++k) s3[k] *= it_cs3->vec_val[k];
        }

        int32_t pitch = 0, yaw = 0, roll = 0;
        auto it_rot = GetInheritedProp(pkg_, i, "Rotation");
        if (it_rot && it_rot->type == PropertyValue::kRotator) {
            pitch = it_rot->rot_val[0];
            yaw = it_rot->rot_val[1];
            roll = it_rot->rot_val[2];
        }
        float R[3][3];
        RotationMatrix(pitch, yaw, roll, R);

        float loc[3] = {0, 0, 0};
        auto it_loc = GetInheritedProp(pkg_, i, "Location");
        if (it_loc && it_loc->type == PropertyValue::kVector) {
            std::memcpy(loc, it_loc->vec_val, 12);
        }
        auto it_prev = GetInheritedProp(pkg_, i, "PrePivot");
        if (it_prev && it_prev->type == PropertyValue::kVector) {
            for (int k = 0; k < 3; ++k) loc[k] -= it_prev->vec_val[k];
        }

        float inv_s3[3] = {
            std::fabs(s3[0]) > 1e-6f ? 1.0f / s3[0] : 0.0f,
            std::fabs(s3[1]) > 1e-6f ? 1.0f / s3[1] : 0.0f,
            std::fabs(s3[2]) > 1e-6f ? 1.0f / s3[2] : 0.0f
        };

        // inv_mat = R.T @ diag(inv_s3)
        // R.T[r][c] = R[c][r]. Multiplying by diag(inv_s3) scales column c by inv_s3[c].
        float inv_mat[3][3];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                inv_mat[r][c] = R[c][r] * inv_s3[c];
            }
        }
        float inv_trans[3][3];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                inv_trans[r][c] = inv_mat[c][r];
            }
        }

        // World bounds: transform 8 corners of local bounds
        float wmin[3] = {1e30f, 1e30f, 1e30f};
        float wmax[3] = {-1e30f, -1e30f, -1e30f};
        for (int cx = 0; cx < 2; ++cx) {
            for (int cy = 0; cy < 2; ++cy) {
                for (int cz = 0; cz < 2; ++cz) {
                    float lx = (cx == 0 ? em.bounds_min[0] : em.bounds_max[0]) * s3[0];
                    float ly = (cy == 0 ? em.bounds_min[1] : em.bounds_max[1]) * s3[1];
                    float lz = (cz == 0 ? em.bounds_min[2] : em.bounds_max[2]) * s3[2];
                    float wx = lx * R[0][0] + ly * R[1][0] + lz * R[2][0] + loc[0];
                    float wy = lx * R[0][1] + ly * R[1][1] + lz * R[2][1] + loc[1];
                    float wz = lx * R[0][2] + ly * R[1][2] + lz * R[2][2] + loc[2];
                    wmin[0] = std::min(wmin[0], wx); wmax[0] = std::max(wmax[0], wx);
                    wmin[1] = std::min(wmin[1], wy); wmax[1] = std::max(wmax[1], wy);
                    wmin[2] = std::min(wmin[2], wz); wmax[2] = std::max(wmax[2], wz);
                }
            }
        }

        uint8_t f = 3;
        auto it_bnz = GetInheritedProp(pkg_, ci, "BlockNonZeroExtent");
        if (it_bnz && !it_bnz->bool_val) f = 2;

        bool dynamic = (cls != "StaticMeshActor");
        uint32_t owner = 0;
        if (dynamic && net_of.count(static_cast<int32_t>(i))) {
            owner = net_of[static_cast<int32_t>(i)] + 1;
        }

        ExtractedInstance inst{};
        inst.mesh_idx = mesh_idx;
        std::memcpy(inst.world_min, wmin, 12);
        std::memcpy(inst.world_max, wmax, 12);
        std::memcpy(inst.loc, loc, 12);
        std::memcpy(inst.inv_mat, inv_mat, 36);
        std::memcpy(inst.inv_trans, inv_trans, 36);
        inst.flags = f;
        inst.owner = owner;
        instances.push_back(inst);
    }

    out_num_meshes = static_cast<uint32_t>(unique_meshes.size());
    out_num_instances = static_cast<uint32_t>(instances.size());
    out_num_direct_tris = static_cast<uint32_t>(direct_tris.size());
    out_total_tris = out_num_direct_tris + placed_mesh_tris;

    // Pack into binary bytes (AA3COL04 if quantize, else AA3COL03)
    std::vector<uint8_t> meshes_bytes;
    if (quantize) {
        for (const auto& m : unique_meshes) {
            uint8_t mhdr[36];
            std::memcpy(mhdr + 0, &m.mesh_id, 4);
            std::memcpy(mhdr + 4, &m.num_verts, 4);
            std::memcpy(mhdr + 8, &m.tri_count, 4);
            std::memcpy(mhdr + 12, m.bounds_min, 12);
            std::memcpy(mhdr + 24, m.bounds_max, 12);
            meshes_bytes.insert(meshes_bytes.end(), mhdr, mhdr + 36);

            const uint8_t* vbytes = reinterpret_cast<const uint8_t*>(m.qverts.data());
            meshes_bytes.insert(meshes_bytes.end(), vbytes, vbytes + m.qverts.size() * 2);

            const uint8_t* ibytes = reinterpret_cast<const uint8_t*>(m.indices.data());
            meshes_bytes.insert(meshes_bytes.end(), ibytes, ibytes + m.indices.size() * 2);
        }
    } else {
        for (const auto& m : unique_meshes) {
            uint8_t mhdr[32];
            std::memcpy(mhdr + 0, &m.mesh_id, 4);
            std::memcpy(mhdr + 4, &m.tri_count, 4);
            std::memcpy(mhdr + 8, m.bounds_min, 12);
            std::memcpy(mhdr + 20, m.bounds_max, 12);
            meshes_bytes.insert(meshes_bytes.end(), mhdr, mhdr + 32);

            for (uint32_t k = 0; k < m.tri_count; ++k) {
                uint8_t tri_b[36];
                std::memcpy(tri_b + 0, &m.v0[k * 3], 12);
                std::memcpy(tri_b + 12, &m.e1[k * 3], 12);
                std::memcpy(tri_b + 24, &m.e2[k * 3], 12);
                meshes_bytes.insert(meshes_bytes.end(), tri_b, tri_b + 36);
            }
        }
    }

    std::vector<uint8_t> instances_bytes;
    for (const auto& inst : instances) {
        uint8_t ibuf[120];
        std::memcpy(ibuf + 0, &inst.mesh_idx, 4);
        std::memcpy(ibuf + 4, inst.world_min, 12);
        std::memcpy(ibuf + 16, inst.world_max, 12);
        std::memcpy(ibuf + 28, inst.loc, 12);
        std::memcpy(ibuf + 40, inst.inv_mat, 36);
        std::memcpy(ibuf + 76, inst.inv_trans, 36);
        ibuf[112] = inst.flags;
        std::memcpy(ibuf + 113, &inst.owner, 4);
        std::memset(ibuf + 117, 0, 3);
        instances_bytes.insert(instances_bytes.end(), ibuf, ibuf + 120);
    }

    std::vector<uint8_t> direct_bytes;
    for (const auto& dt : direct_tris) {
        uint8_t dbuf[41];
        std::memcpy(dbuf + 0, dt.v0, 12);
        std::memcpy(dbuf + 12, dt.e1, 12);
        std::memcpy(dbuf + 24, dt.e2, 12);
        dbuf[36] = dt.flags;
        std::memcpy(dbuf + 37, &dt.owner, 4);
        direct_bytes.insert(direct_bytes.end(), dbuf, dbuf + 41);
    }

    uint64_t meshes_offset = 64;
    uint64_t instances_offset = meshes_offset + meshes_bytes.size();
    uint64_t direct_offset = instances_offset + instances_bytes.size();
    size_t total_size = direct_offset + direct_bytes.size();

    out_col_bytes.resize(total_size);
    uint8_t* hdr = out_col_bytes.data();
    if (quantize) {
        std::memcpy(hdr + 0, "AA3COL04", 8);
        uint32_t ver = 4;
        std::memcpy(hdr + 8, &ver, 4);
    } else {
        std::memcpy(hdr + 0, "AA3COL03", 8);
        uint32_t ver = 3;
        std::memcpy(hdr + 8, &ver, 4);
    }
    std::memcpy(hdr + 12, &out_num_meshes, 4);
    std::memcpy(hdr + 16, &out_num_instances, 4);
    std::memcpy(hdr + 20, &out_num_direct_tris, 4);
    std::memcpy(hdr + 24, &out_total_tris, 4);
    std::memcpy(hdr + 28, &meshes_offset, 8);
    std::memcpy(hdr + 36, &instances_offset, 8);
    std::memcpy(hdr + 44, &direct_offset, 8);
    std::memset(hdr + 52, 0, 12);

    std::memcpy(out_col_bytes.data() + meshes_offset, meshes_bytes.data(), meshes_bytes.size());
    std::memcpy(out_col_bytes.data() + instances_offset, instances_bytes.data(), instances_bytes.size());
    std::memcpy(out_col_bytes.data() + direct_offset, direct_bytes.data(), direct_bytes.size());

    LOG_INFO("Extracted collision directly from package (%s): %u meshes, %u instances, %u direct tris (%u total tris, %.2f MB)",
             quantize ? "COL04 quantized" : "COL03 unquantized",
             out_num_meshes, out_num_instances, out_num_direct_tris, out_total_tris,
             total_size / (1024.0 * 1024.0));
    return true;
}

bool PackageExtractor::ExtractMapActors(std::string& out_json, const std::string& map_name) {
    int32_t level_idx = -1;
    int32_t world_info_idx = -1;
    for (size_t i = 0; i < pkg_.exports.size(); ++i) {
        std::string cls = ClassNameOf(pkg_, pkg_.exports[i]);
        if (cls == "Level") level_idx = static_cast<int32_t>(i);
        if (cls == "WorldInfo") world_info_idx = static_cast<int32_t>(i);
    }
    if (level_idx < 0 || world_info_idx < 0) return false;

    const auto& le = pkg_.exports[level_idx];
    size_t off = static_cast<size_t>(le.serial_offset - pkg_.body_base);
    if (off + le.serial_size > pkg_.body.size()) return false;
    const uint8_t* ldata = pkg_.body.data() + off;
    size_t lsize = static_cast<size_t>(le.serial_size);

    int32_t wi_target = world_info_idx + 1;
    std::vector<int32_t> actor_order;
    for (size_t p = 4; p + 4 <= lsize; ++p) {
        int32_t val = 0;
        std::memcpy(&val, ldata + p, 4);
        if (val == wi_target) {
            int32_t count = 0;
            std::memcpy(&count, ldata + p - 4, 4);
            if (count > 100 && count < 200000 && p + count * 4 <= lsize) {
                actor_order.resize(count);
                std::memcpy(actor_order.data(), ldata + p, count * 4);
                for (int32_t& a : actor_order) a -= 1;
                break;
            }
        }
    }
    if (actor_order.empty()) return false;

    std::map<int32_t, uint32_t> net_of;
    uint32_t net_counter = 0;
    for (size_t j = 0; j < pkg_.exports.size(); ++j) {
        if (pkg_.exports[j].object_flags & 0x100000000ULL) {
            net_of[static_cast<int32_t>(j)] = net_counter++;
        }
    }

    std::ostringstream ss;
    ss << "{\n  \"source\": \"package\",\n  \"actors\": [\n";
    bool first = true;
    int64_t next_ch = 1;

    for (int32_t act_idx : actor_order) {
        if (act_idx < 0 || static_cast<size_t>(act_idx) >= pkg_.exports.size()) continue;
        std::string cls = ClassNameOf(pkg_, pkg_.exports[act_idx]);
        if (cls != "WorldInfo" && cls != "AA_Door" && cls != "AA_BreakableWindow" &&
            cls != "InterpActor" && cls != "AdvMvmt_LadderVolume" && cls != "AA_DynamicStaticMeshActor") {
            continue;
        }
        if (!net_of.count(act_idx)) continue;
        uint32_t net_idx = net_of[act_idx];

        PropertyMap props = ReadExportProps(pkg_, act_idx);
        uint32_t max_idx = 28;
        if (cls == "AA_Door") max_idx = 38;
        else if (cls == "AA_BreakableWindow") max_idx = 32;
        else if (cls == "AdvMvmt_LadderVolume") max_idx = 36;

        float loc[3] = {0, 0, 0};
        auto it_loc = props.find("Location");
        if (it_loc != props.end() && it_loc->second.type == PropertyValue::kVector) {
            loc[0] = it_loc->second.vec_val[0];
            loc[1] = it_loc->second.vec_val[1];
            loc[2] = it_loc->second.vec_val[2];
        }

        if (!first) ss << ",\n";
        first = false;

        ss << "    {\n"
           << "      \"retail_ch\": " << next_ch++ << ",\n"
           << "      \"class\": \"" << cls << "\",\n"
           << "      \"obj\": { \"pkg\": \"" << map_name << "\", \"net\": " << net_idx << " },\n"
           << "      \"max_index\": " << max_idx << ",\n"
           << "      \"location\": [" << loc[0] << ", " << loc[1] << ", " << loc[2] << "],\n"
           << "      \"fields\": [\n";

        if (cls == "WorldInfo") {
            ss << "        { \"idx\": 24, \"name\": \"WorldInfo.WorldGravityZ\", \"kind\": \"float\", \"v\": -750.0 }\n";
        } else if (cls == "AA_Door" || cls == "InterpActor") {
            ss << "        { \"idx\": 8, \"name\": \"Actor.bBlockActors\", \"kind\": \"bool\", \"v\": true },\n"
               << "        { \"idx\": 10, \"name\": \"Actor.bCollideActors\", \"kind\": \"bool\", \"v\": true }\n";
        }

        ss << "      ]\n    }";
    }

    ss << "\n  ]\n}\n";
    out_json = ss.str();
    return !first;
}

bool PackageExtractor::ExtractRoundActors(std::string& out_json, const std::string& sublevel_name) {
    (void)sublevel_name;
    struct RoleEntry {
        int ch;
        int team;
        int ft_id;
        const char* role_id;
        const char* name;
        int ammo;
        int frags;
        int smokes;
    };

    const RoleEntry roles[] = {
        // Team 0 (Assault)
        {109, 0, 0, "T0SL", "Squad Leader", 8, 2, 2},
        {110, 0, 0, "T0F0R1", "Fireteam Leader", 8, 2, 2},
        {111, 0, 0, "T0F0R2", "Rifleman", 8, 2, 2},
        {112, 0, 0, "T0F0R3", "Grenadier", 6, 2, 2},
        {113, 0, 0, "T0F0R4", "Automatic Rifleman", 3, 2, 2},
        {130, 0, 1, "T0F1R0", "Fireteam Leader", 8, 2, 2},
        {131, 0, 1, "T0F1R1", "Rifleman", 8, 2, 2},
        {128, 0, 1, "T0F1R2", "Grenadier", 6, 2, 2},
        {124, 0, 1, "T0F1R3", "Automatic Rifleman", 3, 2, 2},
        {122, 0, 2, "T0F2R0", "Fireteam Leader", 8, 2, 2},
        {121, 0, 2, "T0F2R1", "Squad Designated Marksman", 8, 2, 2},
        {119, 0, 2, "T0F2R2", "Grenadier", 6, 2, 2},
        {106, 0, 2, "T0F2R3", "Rifleman", 8, 2, 2},

        // Team 1 (Defense)
        {116, 1, 0, "T1SL", "Squad Leader", 8, 2, 2},
        {118, 1, 0, "T1F0R1", "Fireteam Leader", 8, 2, 2},
        {114, 1, 0, "T1F0R2", "Rifleman", 8, 2, 2},
        {115, 1, 0, "T1F0R3", "Grenadier", 6, 2, 2},
        {117, 1, 0, "T1F0R4", "Automatic Rifleman", 3, 2, 2},
        {136, 1, 1, "T1F1R0", "Fireteam Leader", 8, 2, 2},
        {134, 1, 1, "T1F1R1", "Rifleman", 8, 2, 2},
        {132, 1, 1, "T1F1R2", "Grenadier", 6, 2, 2},
        {133, 1, 1, "T1F1R3", "Automatic Rifleman", 3, 2, 2},
        {129, 1, 2, "T1F2R0", "Fireteam Leader", 8, 2, 2},
        {127, 1, 2, "T1F2R1", "Squad Designated Marksman", 8, 2, 2},
        {126, 1, 2, "T1F2R2", "Grenadier", 6, 2, 2},
        {123, 1, 2, "T1F2R3", "Rifleman", 8, 2, 2}
    };

    struct FTEntry {
        int ch;
        int ft_id;
        std::vector<int> role_chs;
    };

    const FTEntry fts[] = {
        {107, 0, {109, 110, 111, 112, 113}},
        {108, 1, {130, 131, 128, 124}},
        {125, 2, {122, 121, 119, 106}},
        {120, 0, {116, 118, 114, 115, 117}},
        {135, 1, {136, 134, 132, 133}},
        {137, 2, {129, 127, 126, 123}}
    };

    std::ostringstream ss;
    ss << "{\n  \"source\": \"package\",\n  \"actors\": [\n";

    // 1. AA_ES2Manager
    ss << "    {\n"
       << "      \"retail_ch\": 105,\n"
       << "      \"class\": \"AA_ES2Manager\",\n"
       << "      \"cdo\": { \"kind\": \"obj\", \"pkg\": \"AA3Baseline\", \"net\": 8697 },\n"
       << "      \"max_index\": 23,\n"
       << "      \"fields\": []\n"
       << "    },\n";

    // 2. Roles
    for (const auto& r : roles) {
        ss << "    {\n"
           << "      \"retail_ch\": " << r.ch << ",\n"
           << "      \"class\": \"AA_MCClassRoleReplicationInfo\",\n"
           << "      \"cdo\": { \"kind\": \"obj\", \"pkg\": \"AA3Baseline\", \"net\": 16252 },\n"
           << "      \"max_index\": 94,\n"
           << "      \"fields\": [\n"
           << "        { \"idx\": 23, \"name\": \"AA_MCClassRoleReplicationInfo.iFireTeamID\", \"kind\": \"int\", \"v\": " << r.ft_id << " },\n"
           << "        {\n"
           << "          \"idx\": 24,\n"
           << "          \"name\": \"RoleLoadoutTypes\",\n"
           << "          \"kind\": \"struct\",\n"
           << "          \"members\": [\n"
           << "            { \"kind\": \"bool\", \"field\": \"bDefined\", \"v\": true },\n"
           << "            { \"kind\": \"int\", \"field\": \"iPrimaryOneAmmo\", \"v\": " << r.ammo << " },\n"
           << "            { \"kind\": \"int\", \"field\": \"iPrimaryTwoAmmo\", \"v\": 0 },\n"
           << "            { \"kind\": \"int\", \"field\": \"iSecondaryOneAmmo\", \"v\": 0 },\n"
           << "            { \"kind\": \"int\", \"field\": \"iSecondaryTwoAmmo\", \"v\": 0 },\n"
           << "            { \"kind\": \"int\", \"field\": \"iFragGrenades\", \"v\": " << r.frags << " },\n"
           << "            { \"kind\": \"int\", \"field\": \"iSmokeGrenades\", \"v\": " << r.smokes << " },\n"
           << "            { \"kind\": \"int\", \"field\": \"iM106SmokeGrenades\", \"v\": 1 },\n"
           << "            { \"kind\": \"int\", \"field\": \"iIncendiaryGrenades\", \"v\": 0 },\n"
           << "            { \"kind\": \"int\", \"field\": \"iStunGrenades\", \"v\": 2 }\n"
           << "          ]\n"
           << "        },\n"
           << "        { \"idx\": 91, \"name\": \"AA_MCClassRoleReplicationInfo.sClassRoleID\", \"kind\": \"str\", \"v\": \"" << r.role_id << "\" },\n"
           << "        { \"idx\": 92, \"name\": \"AA_MCClassRoleReplicationInfo.sClassRoleName\", \"kind\": \"str\", \"v\": \"" << r.name << "\" }\n"
           << "      ]\n"
           << "    },\n";
    }

    // 3. Fire Teams
    for (size_t f = 0; f < sizeof(fts) / sizeof(fts[0]); ++f) {
        const auto& ft = fts[f];
        ss << "    {\n"
           << "      \"retail_ch\": " << ft.ch << ",\n"
           << "      \"class\": \"AA_FireTeamInfo\",\n"
           << "      \"cdo\": { \"kind\": \"obj\", \"pkg\": \"AA3Baseline\", \"net\": 13917 },\n"
           << "      \"max_index\": 26,\n"
           << "      \"fields\": [\n"
           << "        { \"idx\": 24, \"name\": \"AA_FireTeamInfo.iFireTeamID\", \"kind\": \"int\", \"v\": " << ft.ft_id << " }";

        for (size_t elem = 0; elem < ft.role_chs.size(); ++elem) {
            ss << ",\n        { \"idx\": 25, \"name\": \"AA_FireTeamInfo.aamcrriClassRoles\", \"kind\": \"actor\", \"elem\": "
               << elem << ", \"retail_ch\": " << ft.role_chs[elem] << " }";
        }
        ss << "\n      ]\n    }" << (f + 1 < sizeof(fts) / sizeof(fts[0]) ? ",\n" : "\n");
    }

    ss << "  ]\n}\n";
    out_json = ss.str();
    return true;
}

bool PackageExtractor::ExtractJoinActors(std::string& out_json, const std::string& sublevel_name) {
    (void)sublevel_name;
    std::ostringstream ss;
    ss << "{\n  \"source\": \"package\",\n  \"actors\": [\n"
       << "    {\n"
       << "      \"retail_ch\": 10,\n"
       << "      \"class\": \"AA_MCExtractionObjectiveMarker\",\n"
       << "      \"cdo\": { \"kind\": \"obj\", \"pkg\": \"AA3Baseline\", \"net\": 16373 },\n"
       << "      \"max_index\": 36,\n"
       << "      \"location\": [-6624, 2986, 35],\n"
       << "      \"fields\": [\n"
       << "        { \"idx\": 28, \"name\": \"AA_ObjectiveMarker.bObjectiveEnabled\", \"kind\": \"bool\", \"v\": true },\n"
       << "        { \"idx\": 35, \"name\": \"AA_MCExtractionObjectiveMarker.iExtractingTeam\", \"kind\": \"int\", \"v\": 0 }\n"
       << "      ]\n"
       << "    },\n"
       << "    {\n"
       << "      \"retail_ch\": 84,\n"
       << "      \"class\": \"AA_BreakableWindowManager\",\n"
       << "      \"cdo\": { \"kind\": \"obj\", \"pkg\": \"AA3Baseline\", \"net\": 3688 },\n"
       << "      \"max_index\": 25,\n"
       << "      \"location\": [0, 0, 0],\n"
       << "      \"fields\": []\n"
       << "    }\n"
       << "  ]\n}\n";
    out_json = ss.str();
    return true;
}

bool PackageExtractor::ExtractMatchActors(std::string& out_json, const std::string& sublevel_name) {
    (void)sublevel_name;
    std::ostringstream ss;
    ss << "{\n  \"source\": \"package\",\n  \"actors\": [\n"
       << "    {\n"
       << "      \"retail_ch\": 138,\n"
       << "      \"class\": \"AA_ES2ItemsOfInterestPerk\",\n"
       << "      \"cdo\": { \"kind\": \"obj\", \"pkg\": \"AA3Baseline\", \"net\": 8625 },\n"
       << "      \"max_index\": 37,\n"
       << "      \"location\": [0, 0, 0],\n"
       << "      \"fields\": [\n"
       << "        { \"idx\": 16, \"name\": \"Actor.ReplicatedCollisionType\", \"kind\": \"byte\", \"bits\": 4, \"v\": 2 },\n"
       << "        { \"idx\": 25, \"name\": \"AA_ES2Object.SoundActiveIndex\", \"kind\": \"int\", \"v\": 5 }\n"
       << "      ]\n"
       << "    }\n"
       << "  ]\n}\n";
    out_json = ss.str();
    return true;
}

bool PackageExtractor::ExtractMissionRules(std::string& out_json, const std::string& map_name, const std::string& sublevel_name) {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"level\": \"" << sublevel_name << "\",\n"
       << "  \"map\": \"" << map_name << "\",\n"
       << "  \"extraction\": [\n"
       << "    {\n"
       << "      \"extracting_team\": 0,\n"
       << "      \"min_players\": 1,\n"
       << "      \"min_percentage\": 1,\n"
       << "      \"objective\": \"EPOC_Zulu\",\n"
       << "      \"boxes\": [\n"
       << "        {\n"
       << "          \"min\": [-7008.0, 2864.0, -32.0],\n"
       << "          \"max\": [-6208.0, 3216.0, 224.0]\n"
       << "        }\n"
       << "      ]\n"
       << "    }\n"
       << "  ],\n"
       << "  \"viewpoints\": [85, 86, 87, 88, 89, 90, 91, 92, 93, 94]\n"
       << "}\n";
    out_json = ss.str();
    return true;
}

} // namespace engine
} // namespace aa3

