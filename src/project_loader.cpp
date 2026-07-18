#include "sjit/project_loader.hpp"
#include "sjit/jit.hpp"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(value); }
    bool isBool() const { return std::holds_alternative<bool>(value); }
    bool isNumber() const { return std::holds_alternative<double>(value); }
    bool isString() const { return std::holds_alternative<std::string>(value); }
    bool isArray() const { return std::holds_alternative<Array>(value); }
    bool isObject() const { return std::holds_alternative<Object>(value); }
    bool asBool(bool fallback = false) const { return isBool() ? std::get<bool>(value) : fallback; }
    double asNumber(double fallback = 0.0) const { return isNumber() ? std::get<double>(value) : fallback; }
    const std::string &asString() const {
        static const std::string empty;
        return isString() ? std::get<std::string>(value) : empty;
    }
    const Array &asArray() const {
        static const Array empty;
        return isArray() ? std::get<Array>(value) : empty;
    }
    const Object &asObject() const {
        static const Object empty;
        return isObject() ? std::get<Object>(value) : empty;
    }
};

const Json *objectGet(const Json &object, const std::string &key) {
    if (!object.isObject()) {
        return nullptr;
    }
    const auto &map = object.asObject();
    auto it = map.find(key);
    return it == map.end() ? nullptr : &it->second;
}

std::string objectString(const Json &object, const std::string &key, const std::string &fallback = "") {
    const Json *value = objectGet(object, key);
    return value && value->isString() ? value->asString() : fallback;
}

double objectNumber(const Json &object, const std::string &key, double fallback = 0.0) {
    const Json *value = objectGet(object, key);
    return value && value->isNumber() ? value->asNumber() : fallback;
}

int objectInt(const Json &object, const std::string &key, int fallback = 0) {
    const double value = objectNumber(object, key, static_cast<double>(fallback));
    if (!std::isfinite(value) ||
        value < static_cast<double>(std::numeric_limits<int>::lowest()) ||
        value > static_cast<double>(std::numeric_limits<int>::max())) {
        return fallback;
    }
    return static_cast<int>(value);
}

bool objectBool(const Json &object, const std::string &key, bool fallback = false) {
    const Json *value = objectGet(object, key);
    return value && value->isBool() ? value->asBool() : fallback;
}

void appendUtf8(std::string &out, uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    Json parse() {
        Json value = parseValue();
        skipWhitespace();
        if (pos_ != text_.size()) {
            fail("trailing data");
        }
        return value;
    }

private:
    Json parseValue() {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            fail("unexpected end");
        }
        const char ch = text_[pos_];
        if (ch == 'n') {
            consumeLiteral("null");
            return Json{nullptr};
        }
        if (ch == 't') {
            consumeLiteral("true");
            return Json{true};
        }
        if (ch == 'f') {
            consumeLiteral("false");
            return Json{false};
        }
        if (ch == '"') {
            return Json{parseString()};
        }
        if (ch == '[') {
            return Json{parseArray()};
        }
        if (ch == '{') {
            return Json{parseObject()};
        }
        return Json{parseNumber()};
    }

    Json::Array parseArray() {
        expect('[');
        Json::Array array;
        skipWhitespace();
        if (peek(']')) {
            ++pos_;
            return array;
        }
        while (true) {
            array.push_back(parseValue());
            skipWhitespace();
            if (peek(']')) {
                ++pos_;
                break;
            }
            expect(',');
        }
        return array;
    }

    Json::Object parseObject() {
        expect('{');
        Json::Object object;
        skipWhitespace();
        if (peek('}')) {
            ++pos_;
            return object;
        }
        while (true) {
            skipWhitespace();
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            object.emplace(std::move(key), parseValue());
            skipWhitespace();
            if (peek('}')) {
                ++pos_;
                break;
            }
            expect(',');
        }
        return object;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            char ch = text_[pos_++];
            if (ch == '"') {
                return out;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) {
                fail("bad escape");
            }
            const char esc = text_[pos_++];
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                out.push_back(esc);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
                appendUtf8(out, parseHex4());
                break;
            default:
                fail("unknown escape");
            }
        }
        fail("unterminated string");
        return {};
    }

    uint32_t parseHex4() {
        if (pos_ + 4 > text_.size()) {
            fail("short unicode escape");
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[pos_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<uint32_t>(10 + ch - 'a');
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<uint32_t>(10 + ch - 'A');
            } else {
                fail("bad unicode escape");
            }
        }
        return value;
    }

    double parseNumber() {
        const size_t start = pos_;
        if (peek('-')) {
            ++pos_;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        if (peek('.')) {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (peek('e') || peek('E')) {
            ++pos_;
            if (peek('+') || peek('-')) {
                ++pos_;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (start == pos_) {
            fail("expected number");
        }
        return std::strtod(text_.c_str() + start, nullptr);
    }

    void consumeLiteral(const char *literal) {
        const size_t length = std::strlen(literal);
        if (text_.compare(pos_, length, literal) != 0) {
            fail("bad literal");
        }
        pos_ += length;
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool peek(char ch) const {
        return pos_ < text_.size() && text_[pos_] == ch;
    }

    void expect(char ch) {
        skipWhitespace();
        if (!peek(ch)) {
            fail(std::string("expected '") + ch + "'");
        }
        ++pos_;
    }

    [[noreturn]] void fail(const std::string &message) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + message);
    }

    std::string text_;
    size_t pos_ = 0;
};

uint16_t readU16(const std::vector<unsigned char> &data, size_t offset) {
    return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

uint32_t readU32(const std::vector<unsigned char> &data, size_t offset) {
    return static_cast<uint32_t>(data[offset] |
        (data[offset + 1] << 8) |
        (data[offset + 2] << 16) |
        (data[offset + 3] << 24));
}

std::vector<unsigned char> readFileBytes(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open " + path);
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> data(static_cast<size_t>(size));
    if (!data.empty()) {
        file.read(reinterpret_cast<char *>(data.data()), size);
    }
    return data;
}

std::string inflateRaw(const unsigned char *data, size_t compressed_size, size_t uncompressed_size) {
    std::string out(uncompressed_size, '\0');
    z_stream stream{};
    stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data));
    stream.avail_in = static_cast<uInt>(compressed_size);
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("zlib inflateInit2 failed");
    }
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (result != Z_STREAM_END) {
        throw std::runtime_error("zlib inflate failed");
    }
    out.resize(stream.total_out);
    return out;
}

std::string extractZipEntry(const std::string &path, const std::string &entry_name) {
    std::vector<unsigned char> data = readFileBytes(path);
    for (size_t offset = 0; offset + 30 <= data.size();) {
        const uint32_t signature = readU32(data, offset);
        if (signature != 0x04034b50u) {
            break;
        }
        const uint16_t flags = readU16(data, offset + 6);
        const uint16_t method = readU16(data, offset + 8);
        const uint32_t compressed_size = readU32(data, offset + 18);
        const uint32_t uncompressed_size = readU32(data, offset + 22);
        const uint16_t name_length = readU16(data, offset + 26);
        const uint16_t extra_length = readU16(data, offset + 28);
        if ((flags & 0x08u) != 0) {
            throw std::runtime_error("zip data descriptors are not supported yet");
        }
        const size_t name_offset = offset + 30;
        const size_t content_offset = name_offset + name_length + extra_length;
        if (content_offset + compressed_size > data.size()) {
            throw std::runtime_error("truncated zip entry");
        }
        const std::string name(reinterpret_cast<const char *>(data.data() + name_offset), name_length);
        if (name == entry_name) {
            const unsigned char *content = data.data() + content_offset;
            if (method == 0) {
                return std::string(reinterpret_cast<const char *>(content), compressed_size);
            }
            if (method == 8) {
                return inflateRaw(content, compressed_size, uncompressed_size);
            }
            throw std::runtime_error("unsupported zip compression method " + std::to_string(method));
        }
        offset = content_offset + compressed_size;
    }
    throw std::runtime_error(entry_name + " not found in " + path);
}

std::string extractAttributeValue(const std::string &text, const std::string &attribute) {
    const std::string needle = attribute + "=\"";
    const size_t start = text.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    const size_t value_start = start + needle.size();
    const size_t value_end = text.find('"', value_start);
    if (value_end == std::string::npos || value_end <= value_start) {
        return {};
    }
    return text.substr(value_start, value_end - value_start);
}

bool parseSvgHexColor(const std::string &text, int &r, int &g, int &b, int &a) {
    if (text.empty() || text[0] != '#') {
        return false;
    }
    auto hexValue = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    };
    auto readByte = [&](size_t index) -> int {
        const int hi = hexValue(text[index]);
        const int lo = hexValue(text[index + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        return (hi << 4) | lo;
    };
    auto readNibble = [&](size_t index) -> int {
        const int value = hexValue(text[index]);
        return value < 0 ? -1 : (value << 4) | value;
    };

    switch (text.size()) {
    case 4:
        r = readNibble(1);
        g = readNibble(2);
        b = readNibble(3);
        a = 255;
        return r >= 0 && g >= 0 && b >= 0;
    case 5:
        r = readNibble(1);
        g = readNibble(2);
        b = readNibble(3);
        a = readNibble(4);
        return r >= 0 && g >= 0 && b >= 0 && a >= 0;
    case 7:
        r = readByte(1);
        g = readByte(3);
        b = readByte(5);
        a = 255;
        return r >= 0 && g >= 0 && b >= 0;
    case 9:
        r = readByte(1);
        g = readByte(3);
        b = readByte(5);
        a = readByte(7);
        return r >= 0 && g >= 0 && b >= 0 && a >= 0;
    default:
        return false;
    }
}

void parseSvgColorAttribute(
    const std::string &svg,
    const std::string &attribute,
    int fallback_r,
    int fallback_g,
    int fallback_b,
    int fallback_a,
    int &r,
    int &g,
    int &b,
    int &a) {
    r = fallback_r;
    g = fallback_g;
    b = fallback_b;
    a = fallback_a;

    const std::string value = extractAttributeValue(svg, attribute);
    if (parseSvgHexColor(value, r, g, b, a)) {
        return;
    }

    if (value.rfind("url(", 0) == 0) {
        const std::string stop_color = extractAttributeValue(svg, "stop-color");
        if (parseSvgHexColor(stop_color, r, g, b, a)) {
            return;
        }
    }
}

double parseSvgDimension(const std::string &svg, const std::string &attribute, double fallback) {
    const std::string value = extractAttributeValue(svg, attribute);
    if (!value.empty()) {
        try {
            const double parsed = std::stod(value);
            if (parsed > 0.0) {
                return parsed;
            }
        } catch (const std::exception &) {
        }
    }
    return fallback;
}

void parseSvgViewBoxDimensions(const std::string &svg, double &width, double &height) {
    const std::string value = extractAttributeValue(svg, "viewBox");
    if (value.empty()) {
        return;
    }
    std::string normalized = value;
    for (char &ch : normalized) {
        if (ch == ',') {
            ch = ' ';
        }
    }
    std::istringstream stream(normalized);
    double min_x = 0.0;
    double min_y = 0.0;
    double view_width = 0.0;
    double view_height = 0.0;
    if (stream >> min_x >> min_y >> view_width >> view_height) {
        if (view_width > 0.0) {
            width = view_width;
        }
        if (view_height > 0.0) {
            height = view_height;
        }
    }
}

struct Block {
    std::string id;
    std::string opcode;
    std::string next;
    bool top_level = false;
    Json inputs;
    Json fields;
    Json mutation;
};

struct ProcedureInfo {
    std::string proccode;
    std::string start_id;
    bool warp_mode = false;
    std::vector<std::string> argument_ids;
    std::vector<std::string> argument_names;
};

struct TargetBuild {
    int target_id = 0;
    bool is_stage = false;
    std::map<std::string, Block> blocks;
    std::map<std::string, ProcedureInfo> procedures;
};

struct HatRegistration {
    bool valid = false;
    int opcode_id = 0;
    std::string match_value;
    int restart_existing_threads = 1;
    int edge_activated = 0;
};

SExpr *makeLiteralString(const std::string &text) {
    SValue value = sjit_make_string(text.c_str());
    SExpr *expr = sjit_expr_create_literal(value);
    sjit_value_destroy(value);
    return expr;
}

SExpr *makeLiteralNumber(double number) {
    SValue value = sjit_make_number(number);
    return sjit_expr_create_literal(value);
}

SExpr *makeLiteralBool(bool value) {
    SValue scratch_value = sjit_make_bool(value);
    SExpr *expr = sjit_expr_create_literal(scratch_value);
    sjit_value_destroy(scratch_value);
    return expr;
}

SValue jsonToScratchValue(const Json &value) {
    if (value.isNumber()) {
        return sjit_make_number(value.asNumber());
    }
    if (value.isString()) {
        return sjit_make_string(value.asString().c_str());
    }
    if (value.isBool()) {
        return sjit_make_bool(value.asBool());
    }
    return sjit_make_string("");
}

void pushJsonValue(SList *list, const Json &value) {
    SValue scratch_value = jsonToScratchValue(value);
    sjit_list_push_move(list, scratch_value);
}

int scalarKindFromJson(const Json &value) {
    if (value.isNumber()) {
        return SJIT_SCALAR_NUMBER;
    }
    if (value.isBool()) {
        return SJIT_SCALAR_BOOL;
    }
    if (value.isString()) {
        return SJIT_SCALAR_STRING;
    }
    return SJIT_SCALAR_DYNAMIC;
}

int mergeScalarKind(int current, int incoming) {
    if (incoming == 0) {
        return SJIT_SCALAR_DYNAMIC;
    }
    if (current < 0) {
        return incoming;
    }
    if (current == incoming) {
        return current;
    }
    return SJIT_SCALAR_DYNAMIC;
}

SVariable *lookupRuntimeVariable(
    SRuntime *runtime,
    int current_target_id,
    const std::string &scratch_id,
    const std::string &name) {
    if (!runtime) {
        return nullptr;
    }
    return sjit_runtime_lookup_variable_by_scratch_id(
        runtime,
        current_target_id,
        scratch_id.c_str(),
        name.c_str(),
        SJIT_VAR_SCALAR);
}

std::string fieldName(const Block &block, const std::string &field_name) {
    const Json *field = objectGet(block.fields, field_name);
    if (!field || !field->isArray() || field->asArray().empty()) {
        return "";
    }
    const Json &name = field->asArray()[0];
    return name.isString() ? name.asString() : "";
}

std::string fieldId(const Block &block, const std::string &field_name) {
    const Json *field = objectGet(block.fields, field_name);
    if (!field || !field->isArray() || field->asArray().size() < 2) {
        return "";
    }
    const Json &id = field->asArray()[1];
    return id.isString() ? id.asString() : "";
}

std::string variableReferenceKey(const std::string &scratch_id, const std::string &name) {
    return scratch_id.empty() ? "name:" + name : "id:" + scratch_id;
}

const Block *findBlock(const TargetBuild &target, const std::string &id) {
    auto it = target.blocks.find(id);
    return it == target.blocks.end() ? nullptr : &it->second;
}

const Json *inputValue(const Block &block, const std::string &name) {
    return objectGet(block.inputs, name);
}

SExpr *compileInputExpr(const TargetBuild &target, const Block &block, const std::string &input_name);
SExpr *compileBlockExpr(const TargetBuild &target, const std::string &block_id);
std::vector<SStatement> compileStatementChain(const TargetBuild &target, const std::string &start_id);

std::string mutationString(const Block &block, const std::string &key) {
    return objectString(block.mutation, key);
}

bool mutationBool(const Block &block, const std::string &key) {
    const Json *value = objectGet(block.mutation, key);
    if (!value) {
        return false;
    }
    if (value->isBool()) {
        return value->asBool();
    }
    return value->isString() && value->asString() == "true";
}

std::vector<std::string> mutationStringArray(const Block &block, const std::string &key) {
    std::vector<std::string> out;
    const std::string raw = mutationString(block, key);
    if (raw.empty()) {
        return out;
    }
    try {
        Json parsed = JsonParser(raw).parse();
        if (!parsed.isArray()) {
            return out;
        }
        for (const Json &item : parsed.asArray()) {
            out.push_back(item.isString() ? item.asString() : "");
        }
    } catch (const std::exception &) {
        return {};
    }
    return out;
}

std::string inputBlockId(const Block &block, const std::string &name) {
    const Json *input = inputValue(block, name);
    if (!input || !input->isArray() || input->asArray().size() < 2) {
        return "";
    }
    const Json &primary = input->asArray()[1];
    return primary.isString() ? primary.asString() : "";
}

HatRegistration hatRegistrationForBlock(const TargetBuild &target, const Block &block) {
    if (!block.top_level || block.next.empty()) {
        return {};
    }
    if (block.opcode == "event_whenflagclicked") {
        return {true, SJIT_HAT_EVENT_WHENFLAGCLICKED, "", 1, 0};
    }
    if (block.opcode == "event_whenthisspriteclicked" && !target.is_stage) {
        return {true, SJIT_HAT_EVENT_WHENTHISSPRITECLICKED, std::to_string(target.target_id), 1, 0};
    }
    if (block.opcode == "event_whenstageclicked" && target.is_stage) {
        return {true, SJIT_HAT_EVENT_WHENSTAGECLICKED, std::to_string(target.target_id), 1, 0};
    }
    if (block.opcode == "event_whenkeypressed") {
        return {true, SJIT_HAT_EVENT_WHENKEYPRESSED, fieldName(block, "KEY_OPTION"), 0, 1};
    }
    if (block.opcode == "event_whenbroadcastreceived") {
        return {true, SJIT_HAT_EVENT_WHENBROADCASTRECEIVED, fieldName(block, "BROADCAST_OPTION"), 1, 0};
    }
    if (block.opcode == "event_whenbackdropswitchesto") {
        return {true, SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO, fieldName(block, "BACKDROP"), 1, 0};
    }
    return {};
}

int inferredVariableKind(
    SRuntime *runtime,
    const TargetBuild &target,
    const std::map<std::string, int> &known_kinds,
    const std::string &scratch_id,
    const std::string &name) {
    auto known = known_kinds.find(variableReferenceKey(scratch_id, name));
    if (known != known_kinds.end()) {
        return known->second;
    }
    SVariable *variable = lookupRuntimeVariable(runtime, target.target_id, scratch_id, name);
    return variable ? variable->scalar_kind : SJIT_SCALAR_DYNAMIC;
}

int literalArrayKind(
    SRuntime *runtime,
    const TargetBuild &target,
    const std::map<std::string, int> &known_kinds,
    const Json &literal) {
    if (!literal.isArray() || literal.asArray().size() < 2) {
        return SJIT_SCALAR_STRING;
    }
    const Json &kind = literal.asArray()[0];
    const Json &value = literal.asArray()[1];
    const std::string scratch_id = literal.asArray().size() >= 3 &&
        literal.asArray()[2].isString() ? literal.asArray()[2].asString() : "";
    if (kind.isNumber() && static_cast<int>(kind.asNumber()) == 12 && value.isString()) {
        return inferredVariableKind(
            runtime, target, known_kinds, scratch_id, value.asString());
    }
    if (kind.isNumber() && static_cast<int>(kind.asNumber()) == 13 && value.isString()) {
        return SJIT_SCALAR_DYNAMIC;
    }
    if (value.isNumber()) {
        return SJIT_SCALAR_NUMBER;
    }
    if (value.isBool()) {
        return SJIT_SCALAR_BOOL;
    }
    if (value.isString()) {
        return SJIT_SCALAR_STRING;
    }
    return SJIT_SCALAR_DYNAMIC;
}

int inputExprKind(
    SRuntime *runtime,
    const TargetBuild &target,
    const std::map<std::string, int> &known_kinds,
    const Block &block,
    const std::string &input_name);

int blockExprKind(
    SRuntime *runtime,
    const TargetBuild &target,
    const std::map<std::string, int> &known_kinds,
    const std::string &block_id) {
    const Block *block = findBlock(target, block_id);
    if (!block) {
        return SJIT_SCALAR_STRING;
    }
    if (block->opcode == "operator_lt" ||
        block->opcode == "operator_equals" ||
        block->opcode == "operator_gt" ||
        block->opcode == "operator_and" ||
        block->opcode == "operator_or" ||
        block->opcode == "operator_not" ||
        block->opcode == "data_listcontainsitem" ||
        block->opcode == "sensing_mousedown" ||
        block->opcode == "sensing_keypressed") {
        return SJIT_SCALAR_BOOL;
    }
    if (block->opcode == "operator_add" ||
        block->opcode == "operator_subtract" ||
        block->opcode == "operator_multiply" ||
        block->opcode == "operator_divide" ||
        block->opcode == "operator_mod" ||
        block->opcode == "operator_random" ||
        block->opcode == "operator_round" ||
        block->opcode == "operator_length" ||
        block->opcode == "operator_mathop" ||
        block->opcode == "data_itemnumoflist" ||
        block->opcode == "data_lengthoflist" ||
        block->opcode == "sensing_timer" ||
        block->opcode == "sensing_dayssince2000" ||
        block->opcode == "sensing_mousex" ||
        block->opcode == "sensing_mousey" ||
        block->opcode == "motion_direction") {
        return SJIT_SCALAR_NUMBER;
    }
    if (block->opcode == "operator_join" ||
        block->opcode == "operator_letter_of" ||
        block->opcode == "sensing_keyoptions" ||
        block->opcode == "pen_menu_colorParam") {
        return SJIT_SCALAR_STRING;
    }
    if (block->opcode == "data_variable") {
        return inferredVariableKind(
            runtime,
            target,
            known_kinds,
            fieldId(*block, "VARIABLE"),
            fieldName(*block, "VARIABLE"));
    }
    return SJIT_SCALAR_DYNAMIC;
}

int inputExprKind(
    SRuntime *runtime,
    const TargetBuild &target,
    const std::map<std::string, int> &known_kinds,
    const Block &block,
    const std::string &input_name) {
    const Json *input = inputValue(block, input_name);
    if (!input || !input->isArray() || input->asArray().size() < 2) {
        return SJIT_SCALAR_STRING;
    }
    const Json &primary = input->asArray()[1];
    if (primary.isString()) {
        return blockExprKind(runtime, target, known_kinds, primary.asString());
    }
    if (primary.isArray()) {
        return literalArrayKind(runtime, target, known_kinds, primary);
    }
    if (input->asArray().size() >= 3 && input->asArray()[2].isArray()) {
        return literalArrayKind(runtime, target, known_kinds, input->asArray()[2]);
    }
    return SJIT_SCALAR_STRING;
}

void inferTargetScalarKinds(SRuntime *runtime, const TargetBuild &target) {
    std::map<std::string, int> inferred;
    std::map<std::string, std::pair<std::string, std::string>> references;
    auto update = [&](const Block &block, int incoming) {
        const std::string name = fieldName(block, "VARIABLE");
        const std::string scratch_id = fieldId(block, "VARIABLE");
        const std::string key = variableReferenceKey(scratch_id, name);
        references.emplace(key, std::make_pair(scratch_id, name));
        SVariable *variable = lookupRuntimeVariable(runtime, target.target_id, scratch_id, name);
        const int base = inferred.count(key) ? inferred[key] :
            (variable ? variable->scalar_kind : -1);
        const int actual_incoming = block.opcode == "data_setvariableto" ?
            inputExprKind(runtime, target, inferred, block, "VALUE") : incoming;
        inferred[key] = mergeScalarKind(base, actual_incoming);
    };
    for (const auto &[id, block] : target.blocks) {
        (void)id;
        if (block.opcode == "data_setvariableto") {
            update(block, SJIT_SCALAR_DYNAMIC);
        } else if (block.opcode == "data_changevariableby") {
            update(block, SJIT_SCALAR_NUMBER);
        } else if (block.opcode == "control_for_each") {
            update(block, SJIT_SCALAR_NUMBER);
        }
    }
    for (const auto &[key, kind] : inferred) {
        const auto reference = references.find(key);
        if (reference == references.end()) {
            continue;
        }
        SVariable *variable = sjit_runtime_lookup_or_create_variable_by_scratch_id(
            runtime,
            target.target_id,
            reference->second.first.c_str(),
            reference->second.second.c_str(),
            SJIT_VAR_SCALAR);
        sjit_variable_set_scalar_kind(variable, kind);
    }
}

void assignSubstack(
    const TargetBuild &target,
    const Block &block,
    const std::string &input_name,
    SStatement *&out_statements,
    int &out_count) {
    const std::string start = inputBlockId(block, input_name);
    if (start.empty()) {
        return;
    }
    std::vector<SStatement> body = compileStatementChain(target, start);
    out_count = static_cast<int>(body.size());
    out_statements = static_cast<SStatement *>(std::calloc(body.size(), sizeof(SStatement)));
    for (size_t i = 0; i < body.size(); ++i) {
        out_statements[i] = body[i];
    }
}

void setStatementVariableReference(
    SStatement &statement,
    const Block &block,
    const std::string &field_name) {
    const std::string name = fieldName(block, field_name);
    const std::string id = fieldId(block, field_name);
    statement.variable_name = sjit_string_new(name.c_str());
    statement.variable_id = sjit_string_new(id.c_str());
}

SExpr *compileLiteralArray(const Json &literal) {
    if (!literal.isArray() || literal.asArray().size() < 2) {
        return makeLiteralString("");
    }
    const Json &kind = literal.asArray()[0];
    const Json &value = literal.asArray()[1];
    const std::string variable_id = literal.asArray().size() >= 3 &&
        literal.asArray()[2].isString() ? literal.asArray()[2].asString() : "";
    if (kind.isNumber() && static_cast<int>(kind.asNumber()) == 12 && value.isString()) {
        return sjit_expr_create_variable_with_id(
            variable_id.c_str(), value.asString().c_str());
    }
    if (kind.isNumber() && static_cast<int>(kind.asNumber()) == 13 && value.isString()) {
        return sjit_expr_create_list_variable_with_id(
            variable_id.c_str(), value.asString().c_str());
    }
    if (value.isNumber()) {
        return makeLiteralNumber(value.asNumber());
    }
    if (value.isString()) {
        return makeLiteralString(value.asString());
    }
    if (value.isBool()) {
        SValue bool_value = sjit_make_bool(value.asBool());
        SExpr *expr = sjit_expr_create_literal(bool_value);
        return expr;
    }
    return makeLiteralString("");
}

SExpr *compileInputExpr(const TargetBuild &target, const Block &block, const std::string &input_name) {
    const Json *input = inputValue(block, input_name);
    if (!input || !input->isArray() || input->asArray().size() < 2) {
        return makeLiteralString("");
    }
    const Json &primary = input->asArray()[1];
    if (primary.isString()) {
        return compileBlockExpr(target, primary.asString());
    }
    if (primary.isArray()) {
        return compileLiteralArray(primary);
    }
    if (input->asArray().size() >= 3 && input->asArray()[2].isArray()) {
        return compileLiteralArray(input->asArray()[2]);
    }
    return makeLiteralString("");
}

SExpr *compileBlockExpr(const TargetBuild &target, const std::string &block_id) {
    const Block *block = findBlock(target, block_id);
    if (!block) {
        return makeLiteralString("");
    }
    if (block->opcode == "operator_lt") {
        return sjit_expr_create_binary(
            SJIT_EXPR_LT,
            compileInputExpr(target, *block, "OPERAND1"),
            compileInputExpr(target, *block, "OPERAND2"));
    }
    if (block->opcode == "operator_equals") {
        return sjit_expr_create_binary(
            SJIT_EXPR_EQ,
            compileInputExpr(target, *block, "OPERAND1"),
            compileInputExpr(target, *block, "OPERAND2"));
    }
    if (block->opcode == "operator_gt") {
        return sjit_expr_create_binary(
            SJIT_EXPR_GT,
            compileInputExpr(target, *block, "OPERAND1"),
            compileInputExpr(target, *block, "OPERAND2"));
    }
    if (block->opcode == "operator_and") {
        return sjit_expr_create_binary(
            SJIT_EXPR_AND,
            compileInputExpr(target, *block, "OPERAND1"),
            compileInputExpr(target, *block, "OPERAND2"));
    }
    if (block->opcode == "operator_or") {
        return sjit_expr_create_binary(
            SJIT_EXPR_OR,
            compileInputExpr(target, *block, "OPERAND1"),
            compileInputExpr(target, *block, "OPERAND2"));
    }
    if (block->opcode == "operator_not") {
        return sjit_expr_create_unary(
            SJIT_EXPR_NOT,
            compileInputExpr(target, *block, "OPERAND"));
    }
    if (block->opcode == "operator_add") {
        return sjit_expr_create_binary(
            SJIT_EXPR_ADD,
            compileInputExpr(target, *block, "NUM1"),
            compileInputExpr(target, *block, "NUM2"));
    }
    if (block->opcode == "operator_subtract") {
        return sjit_expr_create_binary(
            SJIT_EXPR_SUB,
            compileInputExpr(target, *block, "NUM1"),
            compileInputExpr(target, *block, "NUM2"));
    }
    if (block->opcode == "operator_multiply") {
        return sjit_expr_create_binary(
            SJIT_EXPR_MUL,
            compileInputExpr(target, *block, "NUM1"),
            compileInputExpr(target, *block, "NUM2"));
    }
    if (block->opcode == "operator_divide") {
        return sjit_expr_create_binary(
            SJIT_EXPR_DIV,
            compileInputExpr(target, *block, "NUM1"),
            compileInputExpr(target, *block, "NUM2"));
    }
    if (block->opcode == "operator_mod") {
        return sjit_expr_create_binary(
            SJIT_EXPR_MOD,
            compileInputExpr(target, *block, "NUM1"),
            compileInputExpr(target, *block, "NUM2"));
    }
    if (block->opcode == "operator_random") {
        return sjit_expr_create_binary(
            SJIT_EXPR_RANDOM,
            compileInputExpr(target, *block, "FROM"),
            compileInputExpr(target, *block, "TO"));
    }
    if (block->opcode == "operator_round") {
        return sjit_expr_create_unary(
            SJIT_EXPR_ROUND,
            compileInputExpr(target, *block, "NUM"));
    }
    if (block->opcode == "operator_join") {
        return sjit_expr_create_binary(
            SJIT_EXPR_JOIN,
            compileInputExpr(target, *block, "STRING1"),
            compileInputExpr(target, *block, "STRING2"));
    }
    if (block->opcode == "operator_contains") {
        return sjit_expr_create_binary(
            SJIT_EXPR_CONTAINS,
            compileInputExpr(target, *block, "STRING1"),
            compileInputExpr(target, *block, "STRING2"));
    }
    if (block->opcode == "operator_length") {
        return sjit_expr_create_unary(
            SJIT_EXPR_LENGTH,
            compileInputExpr(target, *block, "STRING"));
    }
    if (block->opcode == "operator_letter_of") {
        return sjit_expr_create_binary(
            SJIT_EXPR_LETTER_OF,
            compileInputExpr(target, *block, "LETTER"),
            compileInputExpr(target, *block, "STRING"));
    }
    if (block->opcode == "operator_mathop") {
        return sjit_expr_create_mathop(
            fieldName(*block, "OPERATOR").c_str(),
            compileInputExpr(target, *block, "NUM"));
    }
    if (block->opcode == "argument_reporter_boolean" &&
        fieldName(*block, "VALUE") == "is TurboWarp?") {
        // TurboWarp provides this legacy compatibility reporter even though
        // it is serialized as an argument reporter. It is always true in
        // TurboWarp, including when it is used outside a custom block.
        return makeLiteralBool(true);
    }
    if (block->opcode == "argument_reporter_string_number" || block->opcode == "argument_reporter_boolean") {
        return sjit_expr_create_argument(fieldName(*block, "VALUE").c_str());
    }
    if (block->opcode == "data_variable") {
        const std::string name = fieldName(*block, "VARIABLE");
        const std::string id = fieldId(*block, "VARIABLE");
        return sjit_expr_create_variable_with_id(id.c_str(), name.c_str());
    }
    if (block->opcode == "data_itemoflist") {
        const std::string name = fieldName(*block, "LIST");
        const std::string id = fieldId(*block, "LIST");
        return sjit_expr_create_list_item_with_id(
            id.c_str(),
            name.c_str(),
            compileInputExpr(target, *block, "INDEX"));
    }
    if (block->opcode == "data_itemnumoflist") {
        const std::string name = fieldName(*block, "LIST");
        const std::string id = fieldId(*block, "LIST");
        return sjit_expr_create_list_item_number_with_id(
            id.c_str(),
            name.c_str(),
            compileInputExpr(target, *block, "ITEM"));
    }
    if (block->opcode == "data_lengthoflist") {
        const std::string name = fieldName(*block, "LIST");
        const std::string id = fieldId(*block, "LIST");
        return sjit_expr_create_list_length_with_id(id.c_str(), name.c_str());
    }
    if (block->opcode == "data_listcontainsitem") {
        const std::string name = fieldName(*block, "LIST");
        const std::string id = fieldId(*block, "LIST");
        return sjit_expr_create_list_contains_with_id(
            id.c_str(),
            name.c_str(),
            compileInputExpr(target, *block, "ITEM"));
    }
    if (block->opcode == "sensing_timer") {
        return sjit_expr_create_timer();
    }
    if (block->opcode == "sensing_dayssince2000") {
        return sjit_expr_create_days_since_2000();
    }
    if (block->opcode == "sensing_mousex") {
        return sjit_expr_create_mouse_x();
    }
    if (block->opcode == "sensing_mousey") {
        return sjit_expr_create_mouse_y();
    }
    if (block->opcode == "motion_xposition") {
        return sjit_expr_create_x_position();
    }
    if (block->opcode == "motion_yposition") {
        return sjit_expr_create_y_position();
    }
    if (block->opcode == "motion_direction") {
        return sjit_expr_create_direction();
    }
    if (block->opcode == "sensing_mousedown") {
        return sjit_expr_create_mouse_down();
    }
    if (block->opcode == "sensing_keypressed") {
        return sjit_expr_create_key_pressed(compileInputExpr(target, *block, "KEY_OPTION"));
    }
    if (block->opcode == "sensing_keyoptions") {
        return makeLiteralString(fieldName(*block, "KEY_OPTION"));
    }
    if (block->opcode == "pen_menu_colorParam") {
        return makeLiteralString(fieldName(*block, "colorParam"));
    }
    if (block->opcode == "looks_backdrops") {
        return makeLiteralString(fieldName(*block, "BACKDROP"));
    }
    throw std::runtime_error(
        "unsupported reporter opcode '" + block->opcode +
        "' in block '" + block->id + "'");
}

SStatement compileStatement(const TargetBuild &target, const Block &block) {
    SStatement statement{};
    statement.opcode = SJIT_STMT_NOOP;

    if (block.opcode == "sensing_resettimer") {
        statement.opcode = SJIT_STMT_RESET_TIMER;
    } else if (block.opcode == "data_setvariableto") {
        statement.opcode = SJIT_STMT_SET_VARIABLE;
        setStatementVariableReference(statement, block, "VARIABLE");
        statement.value = compileInputExpr(target, block, "VALUE");
    } else if (block.opcode == "data_changevariableby") {
        statement.opcode = SJIT_STMT_CHANGE_VARIABLE;
        setStatementVariableReference(statement, block, "VARIABLE");
        statement.value = compileInputExpr(target, block, "VALUE");
    } else if (block.opcode == "data_showvariable" || block.opcode == "data_hidevariable") {
        statement.opcode = block.opcode == "data_showvariable" ?
            SJIT_STMT_MONITOR_SHOW : SJIT_STMT_MONITOR_HIDE;
        setStatementVariableReference(statement, block, "VARIABLE");
    } else if (block.opcode == "data_showlist" || block.opcode == "data_hidelist") {
        statement.opcode = block.opcode == "data_showlist" ?
            SJIT_STMT_MONITOR_SHOW : SJIT_STMT_MONITOR_HIDE;
        setStatementVariableReference(statement, block, "LIST");
    } else if (block.opcode == "data_addtolist") {
        statement.opcode = SJIT_STMT_LIST_ADD;
        setStatementVariableReference(statement, block, "LIST");
        statement.value = compileInputExpr(target, block, "ITEM");
    } else if (block.opcode == "data_deleteoflist") {
        statement.opcode = SJIT_STMT_LIST_DELETE;
        setStatementVariableReference(statement, block, "LIST");
        statement.index = compileInputExpr(target, block, "INDEX");
    } else if (block.opcode == "data_deletealloflist") {
        statement.opcode = SJIT_STMT_LIST_DELETE_ALL;
        setStatementVariableReference(statement, block, "LIST");
    } else if (block.opcode == "data_insertatlist") {
        statement.opcode = SJIT_STMT_LIST_INSERT;
        setStatementVariableReference(statement, block, "LIST");
        statement.index = compileInputExpr(target, block, "INDEX");
        statement.value = compileInputExpr(target, block, "ITEM");
    } else if (block.opcode == "data_replaceitemoflist") {
        statement.opcode = SJIT_STMT_LIST_REPLACE;
        setStatementVariableReference(statement, block, "LIST");
        statement.index = compileInputExpr(target, block, "INDEX");
        statement.value = compileInputExpr(target, block, "ITEM");
    } else if (block.opcode == "control_repeat") {
        statement.opcode = SJIT_STMT_REPEAT;
        statement.times = compileInputExpr(target, block, "TIMES");
        assignSubstack(target, block, "SUBSTACK", statement.substack, statement.substack_count);
    } else if (block.opcode == "control_repeat_until") {
        statement.opcode = SJIT_STMT_REPEAT_UNTIL;
        statement.condition = compileInputExpr(target, block, "CONDITION");
        assignSubstack(target, block, "SUBSTACK", statement.substack, statement.substack_count);
    } else if (block.opcode == "control_while") {
        statement.opcode = SJIT_STMT_WHILE;
        statement.condition = compileInputExpr(target, block, "CONDITION");
        assignSubstack(target, block, "SUBSTACK", statement.substack, statement.substack_count);
    } else if (block.opcode == "control_if") {
        statement.opcode = SJIT_STMT_IF;
        statement.condition = compileInputExpr(target, block, "CONDITION");
        assignSubstack(target, block, "SUBSTACK", statement.substack, statement.substack_count);
    } else if (block.opcode == "control_if_else") {
        statement.opcode = SJIT_STMT_IF_ELSE;
        statement.condition = compileInputExpr(target, block, "CONDITION");
        assignSubstack(target, block, "SUBSTACK", statement.substack, statement.substack_count);
        assignSubstack(target, block, "SUBSTACK2", statement.substack2, statement.substack2_count);
    } else if (block.opcode == "control_forever") {
        statement.opcode = SJIT_STMT_FOREVER;
        assignSubstack(target, block, "SUBSTACK", statement.substack, statement.substack_count);
    } else if (block.opcode == "control_for_each") {
        statement.opcode = SJIT_STMT_FOR_EACH;
        setStatementVariableReference(statement, block, "VARIABLE");
        statement.times = compileInputExpr(target, block, "VALUE");
        assignSubstack(target, block, "SUBSTACK", statement.substack, statement.substack_count);
    } else if (block.opcode == "control_stop" && fieldName(block, "STOP_OPTION") == "this script") {
        statement.opcode = SJIT_STMT_STOP_THIS_SCRIPT;
    } else if (block.opcode == "control_stop" && fieldName(block, "STOP_OPTION") == "other scripts in sprite") {
        statement.opcode = SJIT_STMT_STOP_OTHER_SCRIPTS;
    } else if (block.opcode == "control_stop" && fieldName(block, "STOP_OPTION") == "all") {
        statement.opcode = SJIT_STMT_STOP_ALL;
    } else if (block.opcode == "control_wait") {
        statement.opcode = SJIT_STMT_WAIT;
        statement.value = compileInputExpr(target, block, "DURATION");
    } else if (block.opcode == "control_wait_until") {
        statement.opcode = SJIT_STMT_WAIT_UNTIL;
        statement.condition = compileInputExpr(target, block, "CONDITION");
    } else if (block.opcode == "event_broadcast") {
        statement.opcode = SJIT_STMT_BROADCAST;
        statement.value = compileInputExpr(target, block, "BROADCAST_INPUT");
    } else if (block.opcode == "procedures_call") {
        const std::string proccode = mutationString(block, "proccode");
        auto it = target.procedures.find(proccode);
        if (it != target.procedures.end()) {
            statement.opcode = SJIT_STMT_PROCEDURE_CALL;
            statement.procedure_name = sjit_string_new(proccode.c_str());
            const ProcedureInfo &procedure = it->second;
            statement.argument_count = static_cast<int>(procedure.argument_names.size());
            if (statement.argument_count > 0) {
                statement.arguments = static_cast<SArgumentExpr *>(
                    std::calloc(static_cast<size_t>(statement.argument_count), sizeof(SArgumentExpr)));
                if (!statement.arguments) {
                    throw std::runtime_error("failed to allocate procedure arguments");
                }
            }
            for (int i = 0; i < statement.argument_count; ++i) {
                statement.arguments[i].name = sjit_string_new(procedure.argument_names[i].c_str());
                const std::string input_name = i < static_cast<int>(procedure.argument_ids.size()) ?
                    procedure.argument_ids[i] : "";
                statement.arguments[i].value = compileInputExpr(target, block, input_name);
            }
        }
    } else if (block.opcode == "looks_sayforsecs") {
        statement.opcode = SJIT_STMT_LOOKS_SAY_FOR_SECS;
        statement.value = compileInputExpr(target, block, "MESSAGE");
        statement.index = compileInputExpr(target, block, "SECS");
    } else if (block.opcode == "looks_say") {
        statement.opcode = SJIT_STMT_SAY;
        statement.value = compileInputExpr(target, block, "MESSAGE");
    } else if (block.opcode == "looks_show") {
        statement.opcode = SJIT_STMT_LOOKS_SHOW;
    } else if (block.opcode == "looks_hide") {
        statement.opcode = SJIT_STMT_LOOKS_HIDE;
    } else if (block.opcode == "looks_switchbackdropto") {
        statement.opcode = SJIT_STMT_LOOKS_SWITCH_BACKDROP;
        statement.value = compileInputExpr(target, block, "BACKDROP");
    } else if (block.opcode == "looks_seteffectto" ||
        block.opcode == "looks_changeeffectby") {
        statement.opcode = block.opcode == "looks_seteffectto" ?
            SJIT_STMT_LOOKS_SET_EFFECT : SJIT_STMT_LOOKS_CHANGE_EFFECT;
        statement.looks_effect = sjit_looks_effect_from_name(
            fieldName(block, "EFFECT").c_str());
        statement.looks_effect_cache_valid = 1;
        statement.value = compileInputExpr(
            target,
            block,
            block.opcode == "looks_seteffectto" ? "VALUE" : "CHANGE");
    } else if (block.opcode == "looks_cleargraphiceffects") {
        statement.opcode = SJIT_STMT_LOOKS_CLEAR_EFFECTS;
    } else if (block.opcode == "looks_setsizeto") {
        statement.opcode = SJIT_STMT_LOOKS_SET_SIZE;
        statement.value = compileInputExpr(target, block, "SIZE");
    } else if (block.opcode == "sensing_setdragmode") {
        statement.opcode = SJIT_STMT_SENSING_SET_DRAG_MODE;
        statement.drag_mode = fieldName(block, "DRAG_MODE") == "draggable" ? 1 : 0;
    } else if (block.opcode == "motion_gotoxy") {
        statement.opcode = SJIT_STMT_MOTION_GOTO_XY;
        statement.value = compileInputExpr(target, block, "X");
        statement.index = compileInputExpr(target, block, "Y");
    } else if (block.opcode == "motion_setx") {
        statement.opcode = SJIT_STMT_MOTION_SET_X;
        statement.value = compileInputExpr(target, block, "X");
    } else if (block.opcode == "motion_sety") {
        statement.opcode = SJIT_STMT_MOTION_SET_Y;
        statement.value = compileInputExpr(target, block, "Y");
    } else if (block.opcode == "motion_changexby") {
        statement.opcode = SJIT_STMT_MOTION_CHANGE_X;
        statement.value = compileInputExpr(target, block, "DX");
    } else if (block.opcode == "motion_changeyby") {
        statement.opcode = SJIT_STMT_MOTION_CHANGE_Y;
        statement.value = compileInputExpr(target, block, "DY");
    } else if (block.opcode == "pen_clear") {
        statement.opcode = SJIT_STMT_PEN_CLEAR;
    } else if (block.opcode == "pen_penDown") {
        statement.opcode = SJIT_STMT_PEN_DOWN;
    } else if (block.opcode == "pen_penUp") {
        statement.opcode = SJIT_STMT_PEN_UP;
    } else if (block.opcode == "pen_setPenSizeTo") {
        statement.opcode = SJIT_STMT_PEN_SET_SIZE;
        statement.value = compileInputExpr(target, block, "SIZE");
    } else if (block.opcode == "pen_setPenColorToColor") {
        statement.opcode = SJIT_STMT_PEN_SET_COLOR;
        statement.value = compileInputExpr(target, block, "COLOR");
    } else if (block.opcode == "pen_changePenColorParamBy") {
        statement.opcode = SJIT_STMT_PEN_CHANGE_COLOR_PARAM;
        statement.index = compileInputExpr(target, block, "COLOR_PARAM");
        statement.value = compileInputExpr(target, block, "VALUE");
    }

    if (statement.opcode == SJIT_STMT_NOOP) {
        throw std::runtime_error(
            "unsupported statement opcode '" + block.opcode +
            "' in block '" + block.id + "'");
    }
    return statement;
}

std::vector<SStatement> compileStatementChain(const TargetBuild &target, const std::string &start_id) {
    std::vector<SStatement> statements;
    std::string id = start_id;
    int guard = 0;
    while (!id.empty() && guard++ < 10000) {
        const Block *block = findBlock(target, id);
        if (!block) {
            break;
        }
        statements.push_back(compileStatement(target, *block));
        id = block->next;
    }
    return statements;
}

void copyStatementsToScript(SCompiledScript *script, const std::vector<SStatement> &statements) {
    for (size_t i = 0; i < statements.size(); ++i) {
        script->statements[i] = statements[i];
    }
}

void copyProceduresToScript(SCompiledScript *script, const TargetBuild &target) {
    if (!script || target.procedures.empty()) {
        return;
    }
    script->procedure_count = static_cast<int>(target.procedures.size());
    script->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(static_cast<size_t>(script->procedure_count), sizeof(SCompiledProcedure)));
    if (!script->procedures) {
        script->procedure_count = 0;
        throw std::runtime_error("failed to allocate compiled procedures");
    }

    int index = 0;
    for (const auto &[proccode, info] : target.procedures) {
        (void)proccode;
        SCompiledProcedure &procedure = script->procedures[index++];
        procedure.name = sjit_string_new(info.proccode.c_str());
        procedure.warp_mode = info.warp_mode ? 1 : 0;
        procedure.argument_count = static_cast<int>(info.argument_names.size());
        if (procedure.argument_count > 0) {
            procedure.argument_names = static_cast<SString **>(
                std::calloc(static_cast<size_t>(procedure.argument_count), sizeof(SString *)));
            if (!procedure.argument_names) {
                throw std::runtime_error("failed to allocate procedure argument names");
            }
        }
        for (int i = 0; i < procedure.argument_count; ++i) {
            procedure.argument_names[i] = sjit_string_new(info.argument_names[i].c_str());
        }

        std::vector<SStatement> body = compileStatementChain(target, info.start_id);
        procedure.statement_count = static_cast<int>(body.size());
        if (procedure.statement_count > 0) {
            procedure.statements = static_cast<SStatement *>(
                std::calloc(static_cast<size_t>(procedure.statement_count), sizeof(SStatement)));
            if (!procedure.statements) {
                throw std::runtime_error("failed to allocate procedure statements");
            }
        }
        for (int i = 0; i < procedure.statement_count; ++i) {
            procedure.statements[i] = body[static_cast<size_t>(i)];
        }
    }
}

std::string indexedLlPath(const std::string &path, int index) {
    if (index <= 0) {
        return path;
    }
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return path.substr(0, dot) + "." + std::to_string(index) + path.substr(dot);
    }
    return path + "." + std::to_string(index) + ".ll";
}

} // namespace

namespace sjit {

ProjectProgram::ProjectProgram(ProjectProgram &&other) noexcept :
    scripts(std::move(other.scripts)),
    jit(std::move(other.jit)),
    render_targets(std::move(other.render_targets)) {
    other.scripts.clear();
}

ProjectProgram &ProjectProgram::operator=(ProjectProgram &&other) noexcept {
    if (this != &other) {
        /* Invalidate code containing borrowed AST addresses before freeing the
           old immutable script arena. */
        jit.reset();
        for (SCompiledScript *script : scripts) {
            sjit_compiled_script_destroy(script);
        }
        scripts = std::move(other.scripts);
        jit = std::move(other.jit);
        render_targets = std::move(other.render_targets);
        other.scripts.clear();
    }
    return *this;
}

ProjectProgram::~ProjectProgram() {
    /* JIT modules borrow the script trees below. */
    jit.reset();
    for (SCompiledScript *script : scripts) {
        sjit_compiled_script_destroy(script);
    }
}

ProjectLoadResult loadProjectSkeleton(const std::string &path) {
    if (path.empty()) {
        return {true, "project loader ready for .sb3 project.json extraction", {}};
    }
    return {true, "project loader skeleton accepted " + path, {}};
}

ProjectLoadResult loadProjectIntoRuntime(SRuntime *runtime, const std::string &path) {
    ProjectLoadResult result{false, "", {}};
    if (!runtime) {
        result.message = "runtime is null";
        return result;
    }

    try {
        const std::string project_json = extractZipEntry(path, "project.json");
        Json root = JsonParser(project_json).parse();
        const Json *targets_json = objectGet(root, "targets");
        if (!targets_json || !targets_json->isArray()) {
            throw std::runtime_error("project.json has no targets array");
        }

        std::vector<TargetBuild> targets;
        int script_id = 1000;

        for (const Json &target_json : targets_json->asArray()) {
            const bool is_stage = objectBool(target_json, "isStage", false);
            const std::string name = objectString(target_json, "name", is_stage ? "Stage" : "Sprite");
            SSprite *sprite = sjit_runtime_create_sprite(runtime, name.c_str(), is_stage ? 1 : 0);
            if (!sprite) {
                throw std::runtime_error("failed to create target " + name);
            }
            sprite->visible = is_stage ? 0 : (objectBool(target_json, "visible", true) ? 1 : 0);
            sprite->x = objectNumber(target_json, "x", 0.0);
            sprite->y = objectNumber(target_json, "y", 0.0);
            sprite->size = objectNumber(target_json, "size", 100.0);
            sprite->direction = objectNumber(target_json, "direction", 90.0);
            sprite->current_costume = objectInt(target_json, "currentCostume", 0);
            sprite->layer_order = objectInt(target_json, "layerOrder", sprite->base.id);

            TargetRenderInfo render_info;
            render_info.target_id = sprite->base.id;

            const Json *costumes = objectGet(target_json, "costumes");
            if (costumes && costumes->isArray()) {
                for (const Json &costume_json : costumes->asArray()) {
                    if (!costume_json.isObject()) {
                        continue;
                    }
                    CostumeRenderInfo costume;
                    costume.name = objectString(costume_json, "name", "");
                    costume.asset_id = objectString(costume_json, "assetId", "");
                    costume.data_format = objectString(costume_json, "dataFormat", "");
                    costume.rotation_center_x = objectNumber(costume_json, "rotationCenterX", 0.0);
                    costume.rotation_center_y = objectNumber(costume_json, "rotationCenterY", 0.0);

                    if (costume.data_format == "svg" && !costume.asset_id.empty()) {
                        try {
                            const std::string svg = extractZipEntry(path, costume.asset_id + ".svg");
                            costume.source_data = svg;
                            costume.width = parseSvgDimension(svg, "width", 0.0);
                            costume.height = parseSvgDimension(svg, "height", 0.0);
                            if (costume.width <= 0.0 || costume.height <= 0.0) {
                                parseSvgViewBoxDimensions(svg, costume.width, costume.height);
                            }
                            parseSvgColorAttribute(svg, "fill", costume.fill_r, costume.fill_g, costume.fill_b, costume.fill_a,
                                                   costume.fill_r, costume.fill_g, costume.fill_b, costume.fill_a);
                            parseSvgColorAttribute(svg, "stroke", costume.stroke_r, costume.stroke_g, costume.stroke_b, costume.stroke_a,
                                                   costume.stroke_r, costume.stroke_g, costume.stroke_b, costume.stroke_a);
                        } catch (const std::exception &) {
                            costume.width = 0.0;
                            costume.height = 0.0;
                        }
                    } else if (costume.data_format == "png" && !costume.asset_id.empty()) {
                        try {
                            costume.source_data = extractZipEntry(path, costume.asset_id + ".png");
                        } catch (const std::exception &) {
                            costume.source_data.clear();
                        }
                    }
                    render_info.costumes.push_back(std::move(costume));
                }
            }
            if (render_info.costumes.size() >
                static_cast<size_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("too many costumes for target " + name);
            }
            std::vector<const char *> costume_names;
            costume_names.reserve(render_info.costumes.size());
            for (const CostumeRenderInfo &costume : render_info.costumes) {
                costume_names.push_back(costume.name.c_str());
            }
            if (!sjit_sprite_set_costume_names(
                    sprite,
                    costume_names.empty() ? nullptr : costume_names.data(),
                    static_cast<int>(costume_names.size()))) {
                throw std::runtime_error("failed to store costumes for target " + name);
            }

            const Json *variables = objectGet(target_json, "variables");
            if (variables && variables->isObject()) {
                for (const auto &[id, variable_json] : variables->asObject()) {
                    if (!variable_json.isArray() || variable_json.asArray().empty()) {
                        continue;
                    }
                    const std::string variable_name = variable_json.asArray()[0].asString();
                    SVariable *variable = sjit_target_lookup_or_create_variable_by_scratch_id(
                        &sprite->base,
                        id.c_str(),
                        variable_name.c_str(),
                        SJIT_VAR_SCALAR);
                    if (variable && variable_json.asArray().size() >= 2) {
                        const Json &initial = variable_json.asArray()[1];
                        if (initial.isNumber()) {
                            sjit_variable_set(variable, sjit_make_number(initial.asNumber()));
                        } else if (initial.isString()) {
                            SValue value = sjit_make_string(initial.asString().c_str());
                            sjit_variable_set_move(variable, value);
                        } else if (initial.isBool()) {
                            sjit_variable_set(variable, sjit_make_bool(initial.asBool()));
                        }
                        sjit_variable_set_scalar_kind(variable, scalarKindFromJson(initial));
                    }
                    if (variable) {
                        sjit_runtime_register_variable_monitor(
                            runtime,
                            id.c_str(),
                            variable_name.c_str(),
                            sprite->base.id,
                            static_cast<int>(variable - sprite->base.variables),
                            SJIT_VAR_SCALAR);
                    }
                }
            }

            const Json *lists = objectGet(target_json, "lists");
            if (lists && lists->isObject()) {
                for (const auto &[id, list_json] : lists->asObject()) {
                    if (!list_json.isArray() || list_json.asArray().size() < 2) {
                        continue;
                    }
                    const std::string list_name = list_json.asArray()[0].asString();
                    const Json &items_json = list_json.asArray()[1];
                    SVariable *variable = sjit_target_lookup_or_create_variable_by_scratch_id(
                        &sprite->base,
                        id.c_str(),
                        list_name.c_str(),
                        SJIT_VAR_LIST);
                    if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr || !items_json.isArray()) {
                        continue;
                    }
                    SList *list = static_cast<SList *>(variable->value.ptr);
                    for (const Json &item : items_json.asArray()) {
                        pushJsonValue(list, item);
                    }
                    sjit_runtime_register_variable_monitor(
                        runtime,
                        id.c_str(),
                        list_name.c_str(),
                        sprite->base.id,
                        static_cast<int>(variable - sprite->base.variables),
                        SJIT_VAR_LIST);
                }
            }

            TargetBuild target;
            target.target_id = sprite->base.id;
            target.is_stage = is_stage;

            const Json *blocks = objectGet(target_json, "blocks");
            if (blocks && blocks->isObject()) {
                for (const auto &[id, block_json] : blocks->asObject()) {
                    if (!block_json.isObject()) {
                        continue;
                    }
                    Block block;
                    block.id = id;
                    block.opcode = objectString(block_json, "opcode");
                    block.top_level = objectBool(block_json, "topLevel", false);
                    if (const Json *next = objectGet(block_json, "next"); next && next->isString()) {
                        block.next = next->asString();
                    }
                    if (const Json *inputs = objectGet(block_json, "inputs")) {
                        block.inputs = *inputs;
                    }
                    if (const Json *fields = objectGet(block_json, "fields")) {
                        block.fields = *fields;
                    }
                    if (const Json *mutation = objectGet(block_json, "mutation")) {
                        block.mutation = *mutation;
                    }
                    target.blocks.emplace(id, std::move(block));
                }
            }
            for (const auto &[id, block] : target.blocks) {
                (void)id;
                if (block.opcode != "procedures_definition" || block.next.empty()) {
                    continue;
                }
                const Block *prototype = findBlock(target, inputBlockId(block, "custom_block"));
                if (!prototype) {
                    continue;
                }
                const std::string proccode = mutationString(*prototype, "proccode");
                if (!proccode.empty()) {
                    ProcedureInfo info;
                    info.proccode = proccode;
                    info.start_id = block.next;
                    info.warp_mode = mutationBool(*prototype, "warp");
                    info.argument_ids = mutationStringArray(*prototype, "argumentids");
                    info.argument_names = mutationStringArray(*prototype, "argumentnames");
                    if (info.argument_names.size() < info.argument_ids.size()) {
                        info.argument_names.resize(info.argument_ids.size());
                    }
                    target.procedures[proccode] = std::move(info);
                }
            }
            targets.push_back(std::move(target));
            result.program.render_targets.push_back(std::move(render_info));
        }

        const Json *monitors = objectGet(root, "monitors");
        if (monitors && monitors->isArray()) {
            for (const Json &monitor_json : monitors->asArray()) {
                if (!monitor_json.isObject()) {
                    continue;
                }
                const std::string opcode = objectString(monitor_json, "opcode");
                const std::string id = objectString(monitor_json, "id");
                SVariableMonitor *monitor = sjit_runtime_lookup_variable_monitor(runtime, id.c_str());
                if (!monitor ||
                    (opcode == "data_variable" && monitor->variable_type != SJIT_VAR_SCALAR) ||
                    (opcode == "data_listcontents" && monitor->variable_type != SJIT_VAR_LIST) ||
                    (opcode != "data_variable" && opcode != "data_listcontents")) {
                    continue;
                }

                const std::string mode_name = objectString(monitor_json, "mode", "default");
                int mode = SJIT_MONITOR_MODE_DEFAULT;
                if (opcode == "data_listcontents" || mode_name == "list") {
                    mode = SJIT_MONITOR_MODE_LIST;
                } else if (mode_name == "large") {
                    mode = SJIT_MONITOR_MODE_LARGE;
                } else if (mode_name == "slider") {
                    mode = SJIT_MONITOR_MODE_SLIDER;
                }
                sjit_runtime_configure_variable_monitor(
                    runtime,
                    id.c_str(),
                    objectBool(monitor_json, "visible", false) ? 1 : 0,
                    mode,
                    objectNumber(monitor_json, "x", 0.0),
                    objectNumber(monitor_json, "y", 0.0),
                    objectNumber(monitor_json, "width", 0.0),
                    objectNumber(monitor_json, "height", 0.0),
                    objectNumber(monitor_json, "sliderMin", 0.0),
                    objectNumber(monitor_json, "sliderMax", 100.0),
                    objectBool(monitor_json, "isDiscrete", true) ? 1 : 0);
            }
        }

        for (const TargetBuild &target : targets) {
            inferTargetScalarKinds(runtime, target);
        }

        std::unique_ptr<JitEngine> jit;
        try {
            jit = std::make_unique<JitEngine>();
        } catch (const std::exception &) {
            jit.reset();
        }

        int registered = 0;
        int jitted = 0;
        int ownership_parallel = 0;
        const bool disableScriptJit =
            std::getenv("SJIT_DISABLE_SCRIPT_JIT") != nullptr;
        for (const TargetBuild &target : targets) {
            for (const auto &[id, block] : target.blocks) {
                (void)id;
                const HatRegistration hat = hatRegistrationForBlock(target, block);
                if (!hat.valid) {
                    continue;
                }
                std::vector<SStatement> statements = compileStatementChain(target, block.next);
                SCompiledScript *script = sjit_compiled_script_create(target.target_id, static_cast<int>(statements.size()));
                if (!script) {
                    throw std::runtime_error("failed to allocate compiled script");
                }
                copyStatementsToScript(script, statements);
                copyProceduresToScript(script, target);
                SScriptEntryFn entry = sjit_script_interpreter_entry;
                if (jit && !disableScriptJit) {
                    try {
                        entry = jit->compileScript(
                            *script,
                            "sjit_script_entry_" + std::to_string(script_id),
                            runtime);
                        if (entry != sjit_script_interpreter_entry) {
                            ++jitted;
                        }
                    } catch (const std::exception &error) {
                        if (std::getenv("SJIT_LOG_JIT_FALLBACK") != nullptr) {
                            std::fprintf(
                                stderr,
                                "sjit: script %d JIT compilation failed: %s\n",
                                script_id,
                                error.what());
                        }
                        entry = sjit_script_interpreter_entry;
                    }
                }
                const int registered_script_id = script_id++;
                if (!sjit_runtime_register_script_with_data(
                        runtime,
                        target.target_id,
                        registered_script_id,
                        hat.opcode_id,
                        hat.match_value.c_str(),
                        hat.restart_existing_threads,
                        hat.edge_activated,
                        entry,
                        script)) {
                    sjit_compiled_script_destroy(script);
                    throw std::runtime_error("failed to register script");
                }
                const bool ownership_safe =
                    entry == sjit_script_interpreter_entry ?
                        sjit_runtime_analyze_script_ownership(
                            runtime,
                            registered_script_id,
                            script) != 0 :
                        jit && jit->certifyScriptOwnership(
                            runtime,
                            registered_script_id,
                            *script,
                            entry);
                if (ownership_safe) {
                    ++ownership_parallel;
                }
                result.program.scripts.push_back(script);
                ++registered;
            }
        }
        result.program.jit = std::move(jit);

        result.ok = true;
        result.message = "loaded " + path + ": " + std::to_string(targets.size()) +
            " targets, " + std::to_string(registered) + " hat scripts, " +
            std::to_string(jitted) + " LLVM JIT entries, " +
            std::to_string(ownership_parallel) +
            " ownership-proven parallel scripts";
        return result;
    } catch (const std::exception &error) {
        result.ok = false;
        result.message = error.what();
        return result;
    }
}

std::vector<std::string> emitProjectLl(const std::string &project_path, const std::string &output_path) {
    SRuntime *runtime = sjit_runtime_create();
    if (!runtime) {
        throw std::runtime_error("failed to create runtime");
    }

    ProjectLoadResult loaded = loadProjectIntoRuntime(runtime, project_path);
    if (!loaded.ok) {
        const std::string message = loaded.message;
        sjit_runtime_destroy(runtime);
        throw std::runtime_error(message);
    }
    if (loaded.program.scripts.empty()) {
        sjit_runtime_destroy(runtime);
        throw std::runtime_error("project has no compiled hat scripts");
    }

    std::unique_ptr<JitEngine> fallback_jit;
    JitEngine *jit = loaded.program.jit.get();
    if (!jit) {
        fallback_jit = std::make_unique<JitEngine>();
        jit = fallback_jit.get();
    }

    std::vector<std::string> paths;
    paths.reserve(loaded.program.scripts.size());
    for (size_t i = 0; i < loaded.program.scripts.size(); ++i) {
        const std::string path = indexedLlPath(output_path, static_cast<int>(i));
        jit->emitScriptLl(
            *loaded.program.scripts[i],
            "sjit_script_entry_" + std::to_string(1000 + static_cast<int>(i)),
            path);
        paths.push_back(path);
    }

    sjit_runtime_destroy(runtime);
    return paths;
}

} // namespace sjit
