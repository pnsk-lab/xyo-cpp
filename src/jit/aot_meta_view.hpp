// AOT v3 read-only metadata cursor: optional yyjson view avoids allocating
// thousands of temporary std::map nodes. The document owns all cursor data.
#pragma once
#include "../core/json.hpp"
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#ifdef XYO_AOT_YYJSON
#include <yyjson.h>
#endif

namespace xyo {

struct AotMetaNode;
struct AotMetaArray {
    const JsonArray* legacy = nullptr;
#ifdef XYO_AOT_YYJSON
    const yyjson_val* fast = nullptr;
#endif
    AotMetaArray() = default;
    explicit AotMetaArray(const JsonArray* v) : legacy(v) {}
#ifdef XYO_AOT_YYJSON
    explicit AotMetaArray(const yyjson_val* v) : fast(v) {}
#endif
    bool empty() const { return size() == 0; }
    size_t size() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_arr_size(fast);
#endif
        return legacy ? legacy->size() : 0;
    }
    struct Iterator {
        const AotMetaArray* parent;
        size_t index;
#ifdef XYO_AOT_YYJSON
        yyjson_arr_iter iter{};
        const yyjson_val* current = nullptr;
#endif
        using iterator_category = std::input_iterator_tag;
        using value_type = AotMetaNode;
        using reference = AotMetaNode;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        Iterator(const AotMetaArray* p, size_t n) : parent(p), index(n) {
#ifdef XYO_AOT_YYJSON
            if (parent->fast && n == 0) {
                iter = yyjson_arr_iter_with(parent->fast);
                current = yyjson_arr_iter_next(&iter);
            }
#endif
        }
        AotMetaNode operator*() const;
        Iterator& operator++() {
            ++index;
#ifdef XYO_AOT_YYJSON
            if (parent->fast) current = yyjson_arr_iter_next(&iter);
#endif
            return *this;
        }
        bool operator!=(const Iterator& r) const {
            return parent != r.parent || index != r.index;
        }
    };
    Iterator begin() const { return Iterator(this, 0); }
    Iterator end() const { return Iterator(this, size()); }
};

struct AotMetaNode {
    const JsonValue* legacy = nullptr;
#ifdef XYO_AOT_YYJSON
    const yyjson_val* fast = nullptr;
#endif
    AotMetaNode() = default;
    explicit AotMetaNode(const JsonValue* p) : legacy(p) {}
#ifdef XYO_AOT_YYJSON
    explicit AotMetaNode(const yyjson_val* p) : fast(p) {}
#endif
    AotMetaNode get(const char* k) const {
#ifdef XYO_AOT_YYJSON
        if (fast) return AotMetaNode(yyjson_obj_get(fast, k));
#endif
        return AotMetaNode(legacy ? &legacy->get(k) : nullptr);
    }
    AotMetaNode at(size_t n) const {
#ifdef XYO_AOT_YYJSON
        if (fast) return AotMetaNode(yyjson_arr_get(fast, n));
#endif
        return AotMetaNode(legacy ? &legacy->at(n) : nullptr);
    }
    bool has(const char* k) const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_obj_get(fast, k) != nullptr;
#endif
        return legacy && legacy->has(k);
    }
    bool isObject() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_is_obj(fast);
#endif
        return legacy && legacy->isObject();
    }
    bool isArray() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_is_arr(fast);
#endif
        return legacy && legacy->isArray();
    }
    bool isBool() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_is_bool(fast);
#endif
        return legacy && legacy->isBool();
    }
    bool isNumber() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_is_num(fast);
#endif
        return legacy && legacy->isNumber();
    }
    bool asBool() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_is_bool(fast) && yyjson_get_bool(fast);
#endif
        return legacy && legacy->asBool();
    }
    double asNumber() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_is_num(fast) ? yyjson_get_num(fast) : 0.;
#endif
        return legacy ? legacy->asNumber() : 0.;
    }
    // Borrowed for the lifetime of AotMetaDocument; avoids a temporary
    // string allocation on every lookup, including the legacy parser.
    std::string_view asString() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return yyjson_is_str(fast)
            ? std::string_view(yyjson_get_str(fast), yyjson_get_len(fast))
            : std::string_view{};
#endif
        return legacy ? std::string_view(legacy->asString()) : std::string_view{};
    }
    AotMetaArray asArray() const {
#ifdef XYO_AOT_YYJSON
        if (fast) return AotMetaArray(fast);
#endif
        return AotMetaArray(legacy && legacy->isArray() ? &legacy->asArray() : nullptr);
    }
};
inline AotMetaNode AotMetaArray::Iterator::operator*() const {
#ifdef XYO_AOT_YYJSON
    if (parent->fast) return AotMetaNode(current);
#endif
    return AotMetaNode(parent->legacy ? &(*parent->legacy)[index] : nullptr);
}

class AotMetaDocument {
    JsonValue legacy_;
#ifdef XYO_AOT_YYJSON
    struct Deleter { void operator()(yyjson_doc* d) const { yyjson_doc_free(d); } };
    std::unique_ptr<yyjson_doc, Deleter> fast_;
    bool useFast_ = false;
#endif
public:
    explicit AotMetaDocument(const char* source) {
#ifdef XYO_AOT_YYJSON
        const char* e = std::getenv("XYO_AOT_FAST_META");
        if (e && e[0] == '1') {
            useFast_ = true;
            fast_.reset(yyjson_read(source, std::strlen(source), YYJSON_READ_NOFLAG));
            return; // Invalid fast JSON fails closed rather than falling back.
        }
#endif
        legacy_ = jsonParse(source);
    }
    AotMetaNode root() const {
#ifdef XYO_AOT_YYJSON
        if (useFast_)
            return AotMetaNode(fast_ ? yyjson_doc_get_root(fast_.get()) : nullptr);
#endif
        return AotMetaNode(&legacy_);
    }
};

} // namespace xyo
