// xyo-cpp: 拡張マニフェストの読み込み（Phase 1）。
#include "manifest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "../core/fileio.hpp"
#include "../core/json.hpp"
#include "../engine/runtime.hpp"
#include "wasm_provider.hpp"

namespace xyo {
namespace {

namespace fs = std::filesystem;

bool validId(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

bool validShape(const std::string& shape) {
    return shape.empty() || shape == "stack" || shape == "reporter" ||
           shape == "boolean" || shape == "hat" || shape == "end";
}

bool validArgType(const std::string& type) {
    return type == "number" || type == "string" || type == "boolean" ||
           type == "angle" || type == "dropdown";
}

bool parseColor(const JsonValue& value, uint32_t* out) {
    if (value.isNumber()) {
        const double d = value.asNumber();
        if (d < 0 || d > 0xFFFFFF) return false;
        *out = static_cast<uint32_t>(d);
        return true;
    }
    if (!value.isString()) return false;
    const std::string& s = value.asString();
    if (s.size() != 7 || s[0] != '#') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
    if (!end || *end != '\0') return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool parseArgs(const JsonValue& raw, std::vector<ExtensionArgDef>* out,
               std::string* error, const std::string& opcode) {
    if (!raw.isArray()) {
        if (error) *error = "args must be an array: " + opcode;
        return false;
    }
    std::vector<std::string> names;
    for (const JsonValue& entry : raw.asArray()) {
        if (!entry.isObject()) {
            if (error) *error = "arg must be an object: " + opcode;
            return false;
        }
        ExtensionArgDef arg;
        arg.name = entry.get("name").asString();
        if (arg.name.empty()) {
            if (error) *error = "arg name is empty: " + opcode;
            return false;
        }
        if (std::find(names.begin(), names.end(), arg.name) != names.end()) {
            if (error) *error = "duplicate arg name: " + arg.name;
            return false;
        }
        names.push_back(arg.name);
        arg.type = entry.get("type").asString();
        if (arg.type.empty()) arg.type = "string";
        if (!validArgType(arg.type)) {
            if (error) *error = "unknown arg type: " + arg.type;
            return false;
        }
        const JsonValue& def = entry.get("default");
        if (def.isString()) {
            arg.dflt = def.asString();
        } else if (def.isNumber()) {
            const double d = def.asNumber();
            if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 1e15) {
                arg.dflt = std::to_string(static_cast<long long>(d));
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", d);
                arg.dflt = buf;
            }
        }
        if (arg.type == "dropdown") {
            const JsonValue& options = entry.get("options");
            if (!options.isArray() || options.size() == 0) {
                if (error) *error = "dropdown options are required: " + arg.name;
                return false;
            }
            for (const JsonValue& opt : options.asArray()) {
                if (opt.isString()) {
                    arg.options.push_back(opt.asString());
                    arg.options.push_back(opt.asString());
                } else if (opt.isObject()) {
                    std::string label = opt.get("label").asString();
                    std::string value = opt.get("value").asString();
                    if (label.empty()) label = value;
                    if (value.empty()) value = label;
                    if (label.empty()) {
                        if (error) *error = "empty dropdown option: " + arg.name;
                        return false;
                    }
                    arg.options.push_back(label);
                    arg.options.push_back(value);
                } else {
                    if (error) *error = "invalid dropdown option: " + arg.name;
                    return false;
                }
            }
            if (arg.dflt.empty()) arg.dflt = arg.options[1];
        }
        out->push_back(std::move(arg));
    }
    return true;
}

bool parseBlock(const JsonValue& value, const std::string& id,
                ExtensionBlockDef* out, std::string* error) {
    if (!value.isObject()) {
        if (error) *error = "block must be an object";
        return false;
    }
    out->opcode = value.get("opcode").asString();
    if (out->opcode.empty()) {
        if (error) *error = "block opcode is empty";
        return false;
    }
    if (out->opcode.rfind(id + "_", 0) != 0) {
        if (error)
            *error = "block opcode does not start with '" + id + "_': " +
                     out->opcode;
        return false;
    }
    out->label = value.get("label").asString();
    if (out->label.empty()) {
        if (error) *error = "block label is empty: " + out->opcode;
        return false;
    }
    out->shape = value.get("shape").asString();
    if (out->shape.empty()) out->shape = "stack";
    if (!validShape(out->shape)) {
        if (error) *error = "unknown block shape: " + out->shape;
        return false;
    }
    out->hat = value.get("hat").asBool() || out->shape == "hat";
    out->edgeActivated = value.get("edge").asBool();
    out->restartExistingThreads = value.get("restart").asBool();
    out->conditional = value.get("conditional").asBool();
    out->loop = value.get("loop").asBool();
    if (value.has("args") &&
        !parseArgs(value.get("args"), &out->args, error, out->opcode))
        return false;
    return true;
}

bool readTextFile(const std::string& path, std::string* out) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    *out = std::move(text);
    return true;
}

char pathListSeparator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

static bool isPathListSeparator(char c) {
    // Linux/Windowsで同一文字列が同一に解釈されるよう両区切りを許容。
    // Windowsのドライブ文字直後の ':' (例 C:\x) は区切りと誤認し得るため
    // 呼び出し側で1文字目直後の ':' を保護する。
    return c == ';' || c == ':';
}

void addManifestFile(const fs::path& path, ExtensionLoadReport* report) {
    ExtensionDef def;
    std::string error;
    if (loadExtensionManifestFile(path.string(), &def, &error)) {
        const auto duplicate = std::find_if(
            report->loaded.begin(), report->loaded.end(),
            [&](const ExtensionDef& existing) { return existing.id == def.id; });
        if (duplicate != report->loaded.end())
            report->errors.push_back("duplicate extension id: " + def.id +
                                     " (" + path.string() + ")");
        else
            report->loaded.push_back(std::move(def));
    } else {
        report->errors.push_back(path.string() + ": " + error);
    }
}

void scanPathEntry(const fs::path& entry, ExtensionLoadReport* report) {
    std::error_code ec;
    if (fs::is_regular_file(entry, ec)) {
        addManifestFile(entry, report);
        return;
    }
    if (!fs::is_directory(entry, ec)) {
        report->errors.push_back("extension path not found: " + entry.string());
        return;
    }
    const fs::path direct = entry / "manifest.json";
    if (fs::is_regular_file(direct, ec)) {
        addManifestFile(direct, report);
        return;
    }
    std::vector<fs::path> children;
    for (const auto& child : fs::directory_iterator(entry, ec)) {
        if (ec) break;
        if (!child.is_directory()) continue;
        const fs::path manifest = child.path() / "manifest.json";
        if (fs::is_regular_file(manifest, ec)) children.push_back(manifest);
    }
    std::sort(children.begin(), children.end());
    for (const fs::path& manifest : children) addManifestFile(manifest, report);
}

class ManifestStubExtension final : public ExtensionInstance {
public:
    explicit ManifestStubExtension(ExtensionDef def) : def_(std::move(def)) {}

    const ExtensionDef& def() const override { return def_; }

    Value call(const std::string&, const ArgValues&, BlockUtility&) override {
        return Value::undefined();
    }

private:
    ExtensionDef def_;
};

} // namespace

bool parseExtensionManifest(const std::string& json, ExtensionDef* out,
                            std::string* error) {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!out) return fail("manifest output is null");

    JsonValue root = jsonParse(json);
    if (!root.isObject()) return fail("manifest is not a JSON object");

    ExtensionDef def;
    def.id = root.get("id").asString();
    if (!validId(def.id))
        return fail("invalid extension id (expected [a-z0-9-]+): " + def.id);
    def.name = root.get("name").asString();
    if (def.name.empty()) def.name = def.id;
    def.provider = root.get("provider").asString();
    if (root.has("color") && !parseColor(root.get("color"), &def.color))
        return fail("invalid color (expected #RRGGBB or 0xRRGGBB)");

    const JsonValue& blocks = root.get("blocks");
    if (!blocks.isArray() || blocks.size() == 0)
        return fail("blocks must be a non-empty array");

    std::vector<std::string> opcodes;
    for (const JsonValue& entry : blocks.asArray()) {
        ExtensionBlockDef block;
        if (!parseBlock(entry, def.id, &block, error)) return false;
        if (std::find(opcodes.begin(), opcodes.end(), block.opcode) !=
            opcodes.end())
            return fail("duplicate block opcode: " + block.opcode);
        opcodes.push_back(block.opcode);
        def.blocks.push_back(std::move(block));
    }

    *out = std::move(def);
    return true;
}

bool loadExtensionManifestFile(const std::string& path, ExtensionDef* out,
                               std::string* error) {
    std::string text;
    if (!readTextFile(path, &text)) {
        if (error) *error = "cannot read manifest: " + path;
        return false;
    }
    if (!parseExtensionManifest(text, out, error)) return false;
    if (out) out->sourceDir = fs::path(path).parent_path().string();
    return true;
}

ExtensionLoadReport loadExtensionManifests(const std::string& pathList) {
    ExtensionLoadReport report;
    if (pathList.empty()) return report;

    size_t start = 0;
    while (start <= pathList.size()) {
        size_t end = std::string::npos;
        for (size_t i = start; i < pathList.size(); ++i) {
            // Windowsドライブ (例 "C:\dir") のコロンは区切りにしない。
            if (i == start + 1 && pathList.size() > start + 2 &&
                pathList[i] == ':' &&
                ((pathList[start] >= 'A' && pathList[start] <= 'Z') ||
                 (pathList[start] >= 'a' && pathList[start] <= 'z')) &&
                (pathList[i + 1] == '\\' || pathList[i + 1] == '/'))
                continue;
            if (isPathListSeparator(pathList[i])) {
                end = i;
                break;
            }
        }
        const std::string part = pathList.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) scanPathEntry(fs::path(part), &report);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return report;
}

bool registerManifestExtension(Runtime& rt, ExtensionRegistry& registry,
                               const ExtensionDef& def, std::string* error) {
    return registry.add(rt, std::make_unique<ManifestStubExtension>(def),
                        error);
}

ExtensionLoadReport bootstrapExtensionsFromEnvironment(
    Runtime& rt, ExtensionRegistry& registry) {
    ExtensionLoadReport report;
    const char* path = std::getenv("XYO_EXTENSION_PATH");
    if (!path || !*path) return report;
    const bool debug = std::getenv("XYO_EXTENSION_DEBUG") != nullptr;

    ExtensionLoadReport parsed = loadExtensionManifests(path);
    report.errors = std::move(parsed.errors);
    for (ExtensionDef& def : parsed.loaded) {
        if (def.provider == "wasm") {
            const std::string wasmPath =
                (fs::path(def.sourceDir) / "ext.wasm").string();
            std::string error;
            if (registerWasmExtension(rt, registry, def, wasmPath, &error)) {
                if (debug)
                    std::fprintf(stderr, "[ext] loaded %s (%zu blocks, wasm)\n",
                                 def.id.c_str(), def.blocks.size());
                report.loaded.push_back(std::move(def));
                continue;
            }
            report.errors.push_back(def.id + ": " + error +
                                    " (fallback: inert blocks)");
        }
        std::string error;
        if (registerManifestExtension(rt, registry, def, &error)) {
            if (debug)
                std::fprintf(stderr, "[ext] loaded %s (%zu blocks)\n",
                             def.id.c_str(), def.blocks.size());
            report.loaded.push_back(std::move(def));
        } else {
            report.errors.push_back(def.id + ": " + error);
        }
    }
    if (debug) {
        for (const std::string& error : report.errors)
            std::fprintf(stderr, "[ext] error: %s\n", error.c_str());
    }
    return report;
}

} // namespace xyo
