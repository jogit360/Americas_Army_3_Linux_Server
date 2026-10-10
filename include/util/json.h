#ifndef AA3_UTIL_JSON_H
#define AA3_UTIL_JSON_H

// Minimal JSON reader for the server's data files (e.g. the round-actor
// files from aa3-tools/map_manifest/extract_round_actors.py). Parses the
// whole document into a tree; no writer, no streaming.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aa3 {
namespace util {

struct JsonValue {
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = Type::kNull;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    bool IsNull() const { return type == Type::kNull; }
    // Member lookup; returns a shared null value when absent or not an object.
    const JsonValue& operator[](const std::string& key) const;
    bool Has(const std::string& key) const { return type == Type::kObject && obj.count(key); }
    int64_t Int(int64_t fallback = 0) const {
        return type == Type::kNumber ? static_cast<int64_t>(num) : fallback;
    }
    const std::string& Str() const { return str; }
};

// nullopt (and `err` set) on malformed input.
std::optional<JsonValue> ParseJson(const std::string& text, std::string* err = nullptr);

} // namespace util
} // namespace aa3

#endif // AA3_UTIL_JSON_H
