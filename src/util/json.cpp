#include "util/json.h"

#include <cstdlib>

namespace aa3 {
namespace util {

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;

    void Ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool Fail(const char* what) {
        if (err.empty()) err = std::string(what) + " at offset " + std::to_string(i);
        return false;
    }
    bool Literal(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (s.compare(i, n, word) != 0) return Fail("bad literal");
        i += n;
        return true;
    }
    static void PutUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
               out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    bool String(std::string& out) {
        if (s[i] != '"') return Fail("expected string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return Fail("bad escape");
            char e = s[i++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > s.size()) return Fail("bad \\u escape");
                PutUtf8(out, static_cast<uint32_t>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16)));
                i += 4;
                break;
            }
            default: return Fail("bad escape");
            }
        }
        if (i >= s.size()) return Fail("unterminated string");
        ++i;
        return true;
    }
    bool Value(JsonValue& v) {
        Ws();
        if (i >= s.size()) return Fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            v.type = JsonValue::Type::kObject;
            ++i; Ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                Ws();
                std::string key;
                if (!String(key)) return false;
                Ws();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                ++i;
                if (!Value(v.obj[key])) return false;
                Ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = JsonValue::Type::kArray;
            ++i; Ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                v.arr.emplace_back();
                if (!Value(v.arr.back())) return false;
                Ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = JsonValue::Type::kString; return String(v.str); }
        if (c == 't') { v.type = JsonValue::Type::kBool; v.b = true; return Literal("true"); }
        if (c == 'f') { v.type = JsonValue::Type::kBool; v.b = false; return Literal("false"); }
        if (c == 'n') { v.type = JsonValue::Type::kNull; return Literal("null"); }
        char* end = nullptr;
        v.num = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return Fail("unexpected character");
        v.type = JsonValue::Type::kNumber;
        i = static_cast<size_t>(end - s.c_str());
        return true;
    }
};

} // namespace

const JsonValue& JsonValue::operator[](const std::string& key) const {
    static const JsonValue kNull;
    if (type != Type::kObject) return kNull;
    auto it = obj.find(key);
    return it == obj.end() ? kNull : it->second;
}

std::optional<JsonValue> ParseJson(const std::string& text, std::string* err) {
    Parser p{text};
    JsonValue v;
    if (!p.Value(v)) {
        if (err) *err = p.err;
        return std::nullopt;
    }
    p.Ws();
    if (p.i != text.size()) {
        if (err) *err = "trailing data at offset " + std::to_string(p.i);
        return std::nullopt;
    }
    return v;
}

} // namespace util
} // namespace aa3
