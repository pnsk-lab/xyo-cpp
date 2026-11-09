// Node N-API adapter for the existing in-process Binaryen optimizer.
// Kept outside the ordinary xyo runtime: enabled only for host-side tests.
#include <node_api.h>
#include "../../src/wasm/binaryen_c_api.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {
napi_value optimize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok ||
        argc < 1) {
        napi_throw_type_error(env, nullptr, "expected Buffer and optimization level");
        return nullptr;
    }
    bool isBuffer = false;
    if (napi_is_buffer(env, args[0], &isBuffer) != napi_ok || !isBuffer) {
        napi_throw_type_error(env, nullptr, "expected Node Buffer input");
        return nullptr;
    }
    void* bytes = nullptr;
    size_t size = 0;
    if (napi_get_buffer_info(env, args[0], &bytes, &size) != napi_ok ||
        size < 8 || size > 4u * 1024u * 1024u) {
        napi_throw_range_error(env, nullptr, "WASM input must be 8..4194304 bytes");
        return nullptr;
    }
    int32_t level = 2;
    if (argc > 1 &&
        (napi_get_value_int32(env, args[1], &level) != napi_ok ||
         level < 0 || level > 3)) {
        napi_throw_range_error(env, nullptr, "optimization level must be 0..3");
        return nullptr;
    }
    const auto* input = static_cast<const uint8_t*>(bytes);
    std::vector<uint8_t> raw(input, input + size);
    std::vector<uint8_t> out;
    std::string error;
    if (!xyo::wasm::optimizeWithBinaryenCAPI(raw, level, out, error)) {
        napi_throw_error(env, nullptr, error.c_str());
        return nullptr;
    }
    napi_value result = nullptr;
    if (napi_create_buffer_copy(env, out.size(), out.data(), nullptr, &result) != napi_ok) {
        napi_throw_error(env, nullptr, "failed to allocate Binaryen output buffer");
        return nullptr;
    }
    return result;
}
napi_value available(napi_env env, napi_callback_info) {
    napi_value result = nullptr;
    napi_get_boolean(env, xyo::wasm::hasBinaryenCAPI(), &result);
    return result;
}
napi_value init(napi_env env, napi_value exports) {
    napi_property_descriptor descriptors[] = {
        {"optimize", nullptr, optimize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"available", nullptr, available, nullptr, nullptr, nullptr, napi_default, nullptr}
    };
    if (napi_define_properties(env, exports, 2, descriptors) != napi_ok)
        return nullptr;
    return exports;
}
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
