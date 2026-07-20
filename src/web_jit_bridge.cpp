#include "web_jit_bridge.h"

#include "sjit_script.h"
#include "sjit_thread.h"

#include <emscripten/emscripten.h>

#include <cstdint>

EM_JS(int, sjit_web_jit_register_module, (const void *bytes, int size, const char *entry_name, int stack_base), {
    try {
        const memory = Module.wasmMemory ||
            (typeof wasmMemory !== 'undefined' ? wasmMemory : null) ||
            (Module.asm && Module.asm.memory);
        if (!memory) {
            throw new Error('main wasm memory is not exposed');
        }

        const source = new Uint8Array(Module.HEAPU8.buffer, bytes, size).slice();
        const wasmModule = new WebAssembly.Module(source);
        const imports = {};
        const getMainFunction = (name) => {
            const candidates = [
                Module['_' + name],
                Module[name],
                Module.asm && Module.asm['_' + name],
                Module.asm && Module.asm[name]
            ];
            for (const candidate of candidates) {
                if (typeof candidate === 'function') {
                    return candidate;
                }
            }
            return null;
        };

        for (const descriptor of WebAssembly.Module.imports(wasmModule)) {
            const namespace = imports[descriptor.module] ||
                (imports[descriptor.module] = {});
            if (descriptor.kind === 'memory') {
                namespace[descriptor.name] = memory;
                continue;
            }
            if (descriptor.kind === 'function') {
                const functionImport = getMainFunction(descriptor.name);
                if (!functionImport) {
                    throw new Error('missing main wasm import: ' + descriptor.name);
                }
                namespace[descriptor.name] = functionImport;
                continue;
            }
            if (descriptor.kind === 'table') {
                const table = Module.wasmTable ||
                    (Module.asm && Module.asm.__indirect_function_table);
                if (!table) {
                    throw new Error('missing main wasm table import: ' + descriptor.name);
                }
                namespace[descriptor.name] = table;
                continue;
            }
            throw new Error('unsupported dynamic wasm import: ' + descriptor.kind +
                ' ' + descriptor.module + '.' + descriptor.name);
        }

        const instance = new WebAssembly.Instance(wasmModule, imports);
        const stack = instance.exports.__stack_pointer;
        if (stack && typeof stack.value === 'number') {
            stack.value = stack_base;
        }
        const entry = UTF8ToString(entry_name);
        if (typeof instance.exports[entry] !== 'function') {
            throw new Error('dynamic wasm entry is missing: ' + entry);
        }

        const registry = Module.sjitWebJitModules ||
            (Module.sjitWebJitModules = new Map());
        const handle = Module.sjitWebJitNextHandle || 1;
        Module.sjitWebJitNextHandle = handle + 1;
        registry.set(handle, {instance, entry: instance.exports[entry]});
        return handle;
    } catch (error) {
        console.error('sjit: dynamic wasm instantiation failed', error);
        return 0;
    }
});

EM_JS(void, sjit_web_jit_unregister_module, (int handle), {
    if (Module.sjitWebJitModules) {
        Module.sjitWebJitModules.delete(handle);
    }
});

EM_JS(int, sjit_web_jit_invoke, (int handle, int runtime, int thread, int frame), {
    try {
        const record = Module.sjitWebJitModules &&
            Module.sjitWebJitModules.get(handle);
        if (!record) {
            return 5;
        }
        return record.entry(runtime, thread, frame) | 0;
    } catch (error) {
        console.error('sjit: dynamic wasm invocation failed', error);
        return 5;
    }
});

extern "C" SRuntimeStatus sjit_web_wasm_entry(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame) {
    if (!thread || !thread->script_data) {
        return SJIT_STATUS_ERROR;
    }
    SCompiledScript *script = static_cast<SCompiledScript *>(thread->script_data);
    if (script->web_jit_handle == 0) {
        return sjit_script_interpreter_entry(runtime, thread, frame);
    }
    return static_cast<SRuntimeStatus>(sjit_web_jit_invoke(
        static_cast<int>(script->web_jit_handle),
        static_cast<int>(reinterpret_cast<uintptr_t>(runtime)),
        static_cast<int>(reinterpret_cast<uintptr_t>(thread)),
        static_cast<int>(reinterpret_cast<uintptr_t>(frame))));
}
