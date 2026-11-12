// xyo-cpp: JSON パーサ実装（再帰下降、UTF-8 入力）。
#include "json.hpp"
#ifdef XYO_HAVE_YYJSON
#include <yyjson.h>
#include <cstdlib>
#endif

#include <cctype>
#include <charconv>
#include <cstdio>

namespace xyo {

namespace {

#ifdef XYO_HAVE_YYJSON
JsonValue fromYyjson(const yyjson_val* value) {
    if (yyjson_is_null(value)) return nullptr;
    if (yyjson_is_bool(value)) return yyjson_get_bool(value);
    if (yyjson_is_raw(value)) {
        const char* begin = yyjson_get_raw(value);
        const size_t length = yyjson_get_len(value);
        double number = 0.0;
        const auto parsed = std::from_chars(
            begin, begin + length, number, std::chars_format::general);
        if (parsed.ec == std::errc() && parsed.ptr == begin + length)
            return number;
        // Preserve overflow and underflow semantics of legacy strtod.
        return std::strtod(std::string(begin, length).c_str(), nullptr);
    }
    if (yyjson_is_num(value)) return yyjson_get_num(value);
    if (yyjson_is_str(value))
        return std::string(yyjson_get_str(value), yyjson_get_len(value));
    if (yyjson_is_arr(value)) {
        JsonArray array;
        array.reserve(yyjson_arr_size(value));
        size_t index, count;
        yyjson_val* child;
        yyjson_arr_foreach(value, index, count, child)
            array.push_back(fromYyjson(child));
        return array;
    }
    if (yyjson_is_obj(value)) {
        JsonObject object;
        size_t index, count;
        yyjson_val *key, *child;
        yyjson_obj_foreach(value, index, count, key, child)
            object.emplace(std::string(yyjson_get_str(key), yyjson_get_len(key)),
                           fromYyjson(child));
        return object;
    }
    return nullptr;
}

// yyjson interprets integer -0 as unsigned zero. Detect a bare -0 token
// outside quoted strings to preserve Scratch's negative-zero semantics.
// Ordinary projects use yyjson's faster native number conversion.
bool hasNegativeIntegerZero(std::string_view text) {
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c != '-' || i + 1 >= text.size() || text[i + 1] != '0')
            continue;
        const bool before = i == 0 || text[i - 1] == ':' ||
            text[i - 1] == ',' || text[i - 1] == '[' ||
            isSpace(text[i - 1]);
        const bool after = i + 2 == text.size() ||
            text[i + 2] == ',' || text[i + 2] == ']' ||
            text[i + 2] == '}' || isSpace(text[i + 2]);
        if (before && after) return true;
    }
    return false;
}

JsonValue parseYyjson(std::string_view text, bool& parsed) {
    const auto flags = hasNegativeIntegerZero(text)
        ? YYJSON_READ_NUMBER_AS_RAW : YYJSON_READ_NOFLAG;
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), flags);
    if (!doc) { parsed = false; return nullptr; }
    JsonValue value = fromYyjson(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    parsed = true;
    return value;
}
#endif

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool eat(char c) {
        ws();
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }
    bool match(const char* lit) {
        ws();
        const char* q = p;
        while (*lit) {
            if (q >= end || *q != *lit) return false;
            ++q; ++lit;
        }
        p = q;
        return true;
    }

    JsonValue value() {
        ws();
        if (p >= end) { ok = false; return nullptr; }
        switch (*p) {
        case '{': return object();
        case '[': return array();
        case '"': { auto s = str(); if (!ok) return nullptr; return JsonValue(std::move(s)); }
        case 't': if (match("true")) return JsonValue(true); break;
        case 'f': if (match("false")) return JsonValue(false); break;
        case 'n': if (match("null")) return JsonValue(nullptr); break;
        default: return number();
        }
        ok = false;
        return nullptr;
    }

    JsonValue object() {
        ++p; // '{'
        JsonObject o;
        ws();
        if (p < end && *p == '}') { ++p; return JsonValue(std::move(o)); }
        for (;;) {
            ws();
            if (p >= end || *p != '"') { ok = false; return nullptr; }
            std::string key = str();
            if (!ok) return nullptr;
            if (!eat(':')) { ok = false; return nullptr; }
            JsonValue val = value();
            if (!ok) return nullptr;
            o.emplace(std::move(key), std::move(val));
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return JsonValue(std::move(o)); }
            ok = false;
            return nullptr;
        }
    }

    JsonValue array() {
        ++p; // '['
        JsonArray a;
        ws();
        if (p < end && *p == ']') { ++p; return JsonValue(std::move(a)); }
        for (;;) {
            JsonValue val = value();
            if (!ok) return nullptr;
            a.push_back(std::move(val));
            ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return JsonValue(std::move(a)); }
            ok = false;
            return nullptr;
        }
    }

    void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    uint32_t hex4() {
        uint32_t r = 0;
        for (int i = 0; i < 4; ++i) {
            if (p >= end) { ok = false; return 0; }
            char c = *p++;
            r <<= 4;
            if (c >= '0' && c <= '9') r |= c - '0';
            else if (c >= 'a' && c <= 'f') r |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') r |= c - 'A' + 10;
            else { ok = false; return 0; }
        }
        return r;
    }

    std::string str() {
        ++p; // '"'
        std::string out;
        while (p < end) {
            char c = *p++;
            if (c == '"') return out;
            if (c == '\\') {
                if (p >= end) break;
                char e = *p++;
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (!ok) return out;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end &&
                        p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo = hex4();
                        if (!ok) return out;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: ok = false; return out;
                }
            } else {
                out.push_back(c);
            }
        }
        ok = false;
        return out;
    }

    JsonValue number() {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && (std::isdigit((unsigned char)*p) || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) ++p;
        if (p == start) { ok = false; return nullptr; }
        double d = std::strtod(start, nullptr);
        return JsonValue(d);
    }
};

void appendEscaped(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char uc : s) {
        const char c = char(uc);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uc < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void stringifyInto(std::string& out, const JsonValue& v) {
    if (v.isNull()) { out += "null"; return; }
    if (v.isBool()) { out += v.asBool() ? "true" : "false"; return; }
    if (v.isNumber()) {
        // 最短の round-trip 表現（%.17g より短く、値は厳密に一致する）。
        // JSON に NaN/Infinity は無いので null にする（JSON.stringify 準拠）。
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof(buf), v.asNumber());
        if (r.ec == std::errc()) out.append(buf, r.ptr);
        else out += "null";
        return;
    }
    if (v.isString()) {
        appendEscaped(out, v.asString());
        return;
    }
    if (v.isArray()) {
        out += '[';
        bool first = true;
        for (auto& e : v.asArray()) {
            if (!first) out += ',';
            first = false;
            stringifyInto(out, e);
        }
        out += ']';
        return;
    }
    out += '{';
    bool first = true;
    for (auto& [k, e] : v.asObject()) {
        if (!first) out += ',';
        first = false;
        appendEscaped(out, k);
        out += ':';
        stringifyInto(out, e);
    }
    out += '}';
}

} // namespace

JsonValue jsonParse(std::string_view text) {
#ifdef XYO_HAVE_YYJSON
    const char* useYyjson = std::getenv("XYO_JSON_YYJSON");
    if (!useYyjson || useYyjson[0] != '0') {
        bool parsed = false;
        JsonValue value = parseYyjson(text, parsed);
        if (parsed) return value;
        // Existing parser decides malformed/extended legacy JSON compatibility.
    }
#endif
    Parser ps{text.data(), text.data() + text.size()};
    JsonValue v = ps.value();
    ps.ws();
    if (!ps.ok || ps.p != ps.end) return JsonValue(nullptr);
    return v;
}

std::string jsonStringify(const JsonValue& v) {
    std::string out;
    stringifyInto(out, v);
    return out;
}

} // namespace xyo
