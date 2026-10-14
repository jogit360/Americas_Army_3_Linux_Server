#include "engine/package_props.h"

#include <cstring>
#include <set>

namespace aa3 {
namespace engine {

namespace {

constexpr uint64_t kRF_HasStack = 0x0200000000000000ULL;

// Immutable (natively serialized) structs: raw bytes, no tags.
const std::set<std::string> kNativeStructs = {
    "Guid", "Color", "LinearColor", "Vector2D", "Vector4", "Box", "Plane",
    "Matrix", "Quat", "IntPoint", "TwoVectors", "BoxSphereBounds"};

struct Cursor {
    const uint8_t* d;
    size_t size;
    size_t p;
    bool ok = true;

    bool Need(size_t n) {
        if (p + n > size) ok = false;
        return ok;
    }
    int32_t I32() {
        if (!Need(4)) return 0;
        int32_t v;
        std::memcpy(&v, d + p, 4);
        p += 4;
        return v;
    }
    float F32() {
        if (!Need(4)) return 0.0f;
        float v;
        std::memcpy(&v, d + p, 4);
        p += 4;
        return v;
    }
    std::string Name(const Package& pkg) {
        int32_t idx = I32(), num = I32();
        if (!ok || idx < 0 || static_cast<size_t>(idx) >= pkg.names.size()) {
            ok = false;
            return "";
        }
        std::string s = pkg.names[static_cast<size_t>(idx)];
        if (num > 0) s += "_" + std::to_string(num - 1);
        return s;
    }
    std::string FString() {
        int32_t n = I32();
        if (!ok || n == 0) return "";
        if (n > 0) {
            if (!Need(static_cast<size_t>(n))) return "";
            std::string s(reinterpret_cast<const char*>(d + p), static_cast<size_t>(n - 1));
            p += static_cast<size_t>(n);
            return s;
        }
        size_t len = static_cast<size_t>(-static_cast<int64_t>(n));
        if (!Need(len * 2)) return "";
        std::string s;
        for (size_t k = 0; k + 1 < len; ++k) s.push_back(static_cast<char>(d[p + k * 2]));
        p += len * 2;
        return s;
    }
};

bool ReadTagged(const Package& pkg, Cursor& c, size_t end, PropList& out, int depth);

// Arrays carry no element type in the tag. Try, in order: an array of
// tagged structs (every element must parse and end exactly at the array's
// end), then an array of 4-byte values (ints / object references).
void ReadArray(const Package& pkg, const uint8_t* data, size_t size, PropValue& v, int depth) {
    if (size < 4) { v.kind = PropValue::kRaw; return; }
    int32_t count;
    std::memcpy(&count, data, 4);
    if (count == 0) { v.kind = PropValue::kIntArray; return; }
    if (count > 0 && depth < 8) {
        Cursor c{data, size, 4};
        std::vector<PropList> elems;
        bool good = true;
        for (int32_t k = 0; k < count && good; ++k) {
            PropList el;
            good = ReadTagged(pkg, c, size, el, depth + 1) && c.ok;
            elems.push_back(std::move(el));
        }
        if (good && c.p == size) {
            v.kind = PropValue::kStructArray;
            v.elements = std::move(elems);
            return;
        }
    }
    // Native NavReference / ActorReference elements (Actor + 16-byte Guid,
    // e.g. AA_NPCBasePlayerStart.aanpcfpFiringPositions): the references.
    if (count > 0 && static_cast<size_t>(count) * 20 + 4 == size) {
        v.kind = PropValue::kIntArray;
        v.struct_name = "NavReference";
        v.ints.resize(static_cast<size_t>(count));
        for (int32_t k = 0; k < count; ++k)
            std::memcpy(&v.ints[static_cast<size_t>(k)], data + 4 + static_cast<size_t>(k) * 20, 4);
        return;
    }
    if (count > 0 && static_cast<size_t>(count) * 4 + 4 == size) {
        v.kind = PropValue::kIntArray;
        v.ints.resize(static_cast<size_t>(count));
        std::memcpy(v.ints.data(), data + 4, static_cast<size_t>(count) * 4);
        return;
    }
    v.kind = PropValue::kRaw;
}

// Tagged properties until "None". False if the stream is malformed.
bool ReadTagged(const Package& pkg, Cursor& c, size_t end, PropList& out, int depth) {
    while (c.p + 8 <= end) {
        std::string name = c.Name(pkg);
        if (!c.ok) return false;
        if (name == "None") return true;
        PropValue v;
        v.name = name;
        std::string type = c.Name(pkg);
        int32_t size = c.I32();
        v.index = c.I32();
        if (!c.ok || size < 0) return false;
        if (type == "StructProperty") v.struct_name = c.Name(pkg);
        if (type == "BoolProperty") {
            v.kind = PropValue::kBool;
            v.b = c.I32() != 0;            // v568: UBOOL in the tag
            if (!c.ok) return false;
            out.push_back(std::move(v));
            continue;
        }
        size_t start = c.p;
        if (start + static_cast<size_t>(size) > end) return false;
        Cursor body{c.d, start + static_cast<size_t>(size), start};
        if (type == "IntProperty") { v.kind = PropValue::kInt; v.i = body.I32(); }
        else if (type == "FloatProperty") { v.kind = PropValue::kFloat; v.f = body.F32(); }
        else if (type == "ObjectProperty" || type == "ClassProperty" ||
                 type == "ComponentProperty" || type == "InterfaceProperty") {
            v.kind = PropValue::kObject; v.i = body.I32();
        } else if (type == "NameProperty") { v.kind = PropValue::kName; v.s = body.Name(pkg); }
        else if (type == "ByteProperty") {
            v.kind = PropValue::kByte;
            if (size == 1) v.i = c.d[start];
            else v.s = body.Name(pkg);
        } else if (type == "StrProperty") { v.kind = PropValue::kString; v.s = body.FString(); }
        else if (type == "StructProperty" && v.struct_name == "Vector") {
            v.kind = PropValue::kVector;
            for (float& x : v.vec) x = body.F32();
        } else if (type == "StructProperty" && v.struct_name == "Rotator") {
            v.kind = PropValue::kRotator;
            for (int32_t& x : v.rot) x = body.I32();
        } else if (type == "StructProperty" && !kNativeStructs.count(v.struct_name) && depth < 8) {
            v.kind = PropValue::kStruct;
            if (!ReadTagged(pkg, body, start + static_cast<size_t>(size), v.members, depth + 1))
                v.kind = PropValue::kRaw;
        } else if (type == "ArrayProperty") {
            ReadArray(pkg, c.d + start, static_cast<size_t>(size), v, depth);
        } else {
            v.kind = PropValue::kRaw;
        }
        c.p = start + static_cast<size_t>(size);
        out.push_back(std::move(v));
    }
    return true;
}

} // namespace

const PropValue* FindProp(const PropList& props, const std::string& name, int32_t index) {
    for (const auto& p : props)
        if (p.name == name && p.index == index) return &p;
    return nullptr;
}

std::string ExportClassName(const Package& pkg, size_t exp_idx) {
    if (exp_idx >= pkg.exports.size()) return "";
    int32_t ci = pkg.exports[exp_idx].class_index;
    if (ci < 0 && static_cast<size_t>(-ci - 1) < pkg.imports.size())
        return pkg.imports[static_cast<size_t>(-ci - 1)].object_name;
    if (ci > 0 && static_cast<size_t>(ci - 1) < pkg.exports.size())
        return pkg.exports[static_cast<size_t>(ci - 1)].object_name;
    return ci == 0 ? "Class" : "";
}

int32_t RefToExport(int32_t package_index) {
    return package_index > 0 ? package_index - 1 : -1;
}

std::optional<ExportData> ReadExportData(const Package& pkg, size_t exp_idx) {
    if (exp_idx >= pkg.exports.size() || pkg.body.empty()) return std::nullopt;
    const ObjectExport& e = pkg.exports[exp_idx];
    int64_t off = static_cast<int64_t>(e.serial_offset) - pkg.body_base;
    if (e.serial_size <= 0 || off < 0 ||
        static_cast<size_t>(off) + static_cast<size_t>(e.serial_size) > pkg.body.size())
        return std::nullopt;
    Cursor c{pkg.body.data() + off, static_cast<size_t>(e.serial_size), 0};
    std::string cls = ExportClassName(pkg, exp_idx);
    if (cls.size() >= 9 && cls.compare(cls.size() - 9, 9, "Component") == 0)
        c.I32();                                   // UComponent: TemplateOwnerClass
    if (e.object_flags & kRF_HasStack) {
        int32_t node = c.I32();
        c.I32();                                   // StateNode
        c.p += 8 + 2;                              // ProbeMask, LatentAction
        int32_t stack = c.I32();
        if (stack < 0 || stack > 64) return std::nullopt;
        c.p += static_cast<size_t>(stack) * 12;
        if (node != 0) c.I32();                    // Offset
    }
    ExportData out;
    out.net_index = c.I32();
    if (!c.ok) return std::nullopt;
    ReadTagged(pkg, c, c.size, out.props, 0);
    out.props_end = c.p;
    return out;
}

int32_t ReadExportNetIndex(const Package& pkg, size_t exp_idx) {
    auto d = ReadExportData(pkg, exp_idx);
    return d ? d->net_index : -1;
}

std::string ExportOuterPackage(const Package& pkg, size_t exp_idx) {
    std::string top;
    for (int guard = 0; exp_idx < pkg.exports.size() && guard < 64; ++guard) {
        const ObjectExport& e = pkg.exports[exp_idx];
        if (ExportClassName(pkg, exp_idx) == "Package") top = e.object_name;
        if (e.outer_index <= 0) break;
        exp_idx = static_cast<size_t>(e.outer_index - 1);
    }
    return top;
}

} // namespace engine
} // namespace aa3
