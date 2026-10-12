#ifndef AA3_ENGINE_PACKAGE_PROPS_H
#define AA3_ENGINE_PACKAGE_PROPS_H

// Tagged-property trees for exports of a cooked UE3 v568 package, plus the
// export's on-disk NetIndex.
//
// UObject::Serialize (UnObj.cpp) writes, in order: TemplateOwnerClass (only
// components), the state frame (only RF_HasStack: Node, StateNode,
// ProbeMask(8), LatentAction(2), StateStack, Offset if Node), the NetIndex
// (SerializeNetIndex) and then the tagged properties. In cooked packages the
// client takes that NetIndex as is, so it is the one the wire uses
// (ObjectIndex = ObjectBase[package] + NetIndex) -- including placed level
// actors, which an export-order count gets wrong past the first forced
// export (e.g. Bridge_night_clear's WorldInfo is 15064, its export-order
// count 19595).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/package_reader.h"

namespace aa3 {
namespace engine {

struct PropValue {
    enum Kind { kNone, kBool, kByte, kInt, kFloat, kName, kString, kObject,
                kVector, kRotator, kStruct, kStructArray, kIntArray, kRaw };
    std::string name;
    int32_t index = 0;           // static array element
    Kind kind = kNone;
    std::string struct_name;     // kStruct
    bool b = false;
    int32_t i = 0;               // int, byte (numeric), object reference (FPackageIndex)
    float f = 0.0f;
    std::string s;               // name, string, enum byte
    float vec[3] = {0, 0, 0};
    int32_t rot[3] = {0, 0, 0};
    std::vector<PropValue> members;                 // kStruct
    std::vector<std::vector<PropValue>> elements;   // kStructArray
    std::vector<int32_t> ints;                      // kIntArray (ints or references)
};
using PropList = std::vector<PropValue>;

// First property `name` with static-array index `index`, or nullptr.
const PropValue* FindProp(const PropList& props, const std::string& name, int32_t index = 0);

struct ExportData {
    int32_t net_index = -1;
    PropList props;
    size_t props_end = 0;        // body offset just past the property list
};

// Class name of an export (import or export class).
std::string ExportClassName(const Package& pkg, size_t exp_idx);

// Export index for an FPackageIndex that names an export, else -1.
int32_t RefToExport(int32_t package_index);

// NetIndex + tagged properties of export `exp_idx`; nullopt when its body
// is not loaded (pkg.body empty) or out of range.
std::optional<ExportData> ReadExportData(const Package& pkg, size_t exp_idx);

// Just the on-disk NetIndex (-1 if unreadable).
int32_t ReadExportNetIndex(const Package& pkg, size_t exp_idx);

// Outermost package of an export: "" for the package itself, else the name
// of the forced-export UPackage the object belongs to (cooked content like
// meshes and materials keeps its own package's NetIndex numbering).
std::string ExportOuterPackage(const Package& pkg, size_t exp_idx);

} // namespace engine
} // namespace aa3

#endif // AA3_ENGINE_PACKAGE_PROPS_H
