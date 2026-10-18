// xyo-cpp: 最小 JSON DOM パーサ（project.json 用）。
// 数値は double、文字列は UTF-8 std::string（必要箇所で UTF-16 変換する）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace xyo {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
    using Var = std::variant<std::nullptr_t, bool, double, std::string,
                             JsonArray, JsonObject>;
    Var v;

    JsonValue() : v(nullptr) {}
    JsonValue(std::nullptr_t) : v(nullptr) {}
    JsonValue(bool b) : v(b) {}
    JsonValue(double d) : v(d) {}
    JsonValue(int i) : v(double(i)) {}
    JsonValue(std::string s) : v(std::move(s)) {}
    JsonValue(JsonArray a) : v(std::move(a)) {}
    JsonValue(JsonObject o) : v(std::move(o)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool isBool() const { return std::holds_alternative<bool>(v); }
    bool isNumber() const { return std::holds_alternative<double>(v); }
    bool isString() const { return std::holds_alternative<std::string>(v); }
    bool isArray() const { return std::holds_alternative<JsonArray>(v); }
    bool isObject() const { return std::holds_alternative<JsonObject>(v); }

    bool asBool() const { return isBool() ? std::get<bool>(v) : false; }
    double asNumber() const { return isNumber() ? std::get<double>(v) : 0.0; }
    const std::string& asString() const {
        static const std::string kEmpty;
        return isString() ? std::get<std::string>(v) : kEmpty;
    }
    const JsonArray& asArray() const {
        static const JsonArray kEmpty;
        return isArray() ? std::get<JsonArray>(v) : kEmpty;
    }
    const JsonObject& asObject() const {
        static const JsonObject kEmpty;
        return isObject() ? std::get<JsonObject>(v) : kEmpty;
    }
    JsonArray& arrayRef() { return std::get<JsonArray>(v); }
    JsonObject& objectRef() { return std::get<JsonObject>(v); }

    /// obj["key"]（存在しなければ null）
    const JsonValue& get(const std::string& key) const {
        static const JsonValue kNull;
        if (!isObject()) return kNull;
        auto& o = std::get<JsonObject>(v);
        auto it = o.find(key);
        return it == o.end() ? kNull : it->second;
    }
    bool has(const std::string& key) const {
        return isObject() && std::get<JsonObject>(v).count(key) != 0;
    }
    const JsonValue& at(size_t i) const {
        static const JsonValue kNull;
        auto& a = asArray();
        return i < a.size() ? a[i] : kNull;
    }
    size_t size() const {
        if (isArray()) return std::get<JsonArray>(v).size();
        if (isObject()) return std::get<JsonObject>(v).size();
        return 0;
    }
};

/// JSON テキストをパース。失敗時は null を返す。
JsonValue jsonParse(std::string_view text);

/// JSON への再シリアライズ（mutation デバッグ用、最小限）
std::string jsonStringify(const JsonValue& v);

} // namespace xyo
