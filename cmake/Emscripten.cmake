set(WEB_RUNTIME_C_SOURCES
    ${RUNTIME_C_SOURCES}
    ${QUICKJS_SUBSET_C_SOURCES}
)

# The native scheduler can use a persistent pthread pool.  Web builds run the
# same scheduler sequentially on the browser thread so that the generated
# module also works without cross-origin isolation headers.
list(REMOVE_ITEM WEB_RUNTIME_C_SOURCES runtime/sjit_thread_pool.c)
list(APPEND WEB_RUNTIME_C_SOURCES runtime/sjit_thread_pool_web.c)

add_library(sjit_runtime_web STATIC ${WEB_RUNTIME_C_SOURCES})
target_include_directories(sjit_runtime_web PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs_subset
)
target_compile_definitions(sjit_runtime_web PUBLIC
    SJIT_WEB=1
    _POSIX_C_SOURCE=200809L
)

add_executable(xyo-web
    src/project_loader.cpp
    src/web_main.cpp
)

option(SJIT_WEB_LLVM_JIT
    "Compile eligible Web scripts to WebAssembly modules with LLVM and instantiate them dynamically"
    OFF)
set(SJIT_WEB_LLVM_SOURCE "" CACHE PATH
    "LLVM source tree used by the Web LLVM JIT")
set(SJIT_WEB_LLVM_BUILD "" CACHE PATH
    "LLVM WebAssembly cross-build containing libLLVM*.a and liblldWasm.a")

if(SJIT_WEB_LLVM_JIT)
    if(NOT SJIT_WEB_LLVM_SOURCE AND DEFINED ENV{SJIT_WEB_LLVM_SOURCE})
        set(SJIT_WEB_LLVM_SOURCE "$ENV{SJIT_WEB_LLVM_SOURCE}")
    endif()
    if(NOT SJIT_WEB_LLVM_BUILD AND DEFINED ENV{SJIT_WEB_LLVM_BUILD})
        set(SJIT_WEB_LLVM_BUILD "$ENV{SJIT_WEB_LLVM_BUILD}")
    endif()
    if(NOT IS_DIRECTORY "${SJIT_WEB_LLVM_SOURCE}" OR
       NOT IS_DIRECTORY "${SJIT_WEB_LLVM_BUILD}")
        message(FATAL_ERROR
            "SJIT_WEB_LLVM_JIT requires SJIT_WEB_LLVM_SOURCE and "
            "SJIT_WEB_LLVM_BUILD. Build LLVM with the WebAssembly target "
            "and pass both paths to CMake.")
    endif()

    file(GLOB _SJIT_WEB_LLVM_ARCHIVES CONFIGURE_DEPENDS
        "${SJIT_WEB_LLVM_BUILD}/lib/libLLVM*.a"
        "${SJIT_WEB_LLVM_BUILD}/lib/liblld*.a")
    if(NOT _SJIT_WEB_LLVM_ARCHIVES)
        message(FATAL_ERROR
            "No LLVM/lld static archives found under "
            "${SJIT_WEB_LLVM_BUILD}/lib")
    endif()
    target_sources(xyo-web PRIVATE
        src/jit.cpp
        src/web_jit_bridge.cpp
    )
    target_include_directories(xyo-web PRIVATE
        ${SJIT_WEB_LLVM_SOURCE}/llvm/include
        ${SJIT_WEB_LLVM_SOURCE}/lld/include
        ${SJIT_WEB_LLVM_BUILD}/include
        ${SJIT_WEB_LLVM_BUILD}/tools/lld/include
    )
    target_compile_definitions(xyo-web PRIVATE SJIT_WEB_LLVM_JIT=1)
    target_link_libraries(xyo-web PRIVATE
        "-Wl,--start-group"
        ${_SJIT_WEB_LLVM_ARCHIVES}
        "-Wl,--end-group"
    )
else()
    target_sources(xyo-web PRIVATE src/web_jit_stub.cpp)
endif()

target_include_directories(xyo-web PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs_subset
)
target_compile_definitions(xyo-web PRIVATE SJIT_WEB=1)
target_compile_options(xyo-web PRIVATE
    -fexceptions
    -sUSE_ZLIB=1
)
if(SJIT_WEB_LLVM_JIT)
    # Dynamic script modules import runtime leaves from the main wasm. Keep
    # the complete C runtime alive so --export-all/EXPORT_ALL can expose them to
    # those modules even when a particular project has not called them yet.
    target_link_libraries(xyo-web PRIVATE
        "-Wl,--whole-archive"
        sjit_runtime_web
        "-Wl,--no-whole-archive"
    )
else()
    target_link_libraries(xyo-web PRIVATE sjit_runtime_web)
endif()
target_link_options(xyo-web PRIVATE
    --no-entry
    -fexceptions
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=XyoModule
    -sENVIRONMENT=web
    -sFORCE_FILESYSTEM=1
    -sUSE_ZLIB=1
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8388608
    -sINITIAL_MEMORY=33554432
    -sMAXIMUM_MEMORY=536870912
    -sNO_EXIT_RUNTIME=1
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_sjit_web_load_project_bytes','_sjit_web_last_error','_sjit_web_start','_sjit_web_stop','_sjit_web_tick','_sjit_web_set_mouse','_sjit_web_set_key','_sjit_web_blur','_sjit_web_draw_count','_sjit_web_draw_command','_sjit_web_pen_count','_sjit_web_pen_command','_sjit_web_target_count','_sjit_web_target_id','_sjit_web_target_is_stage','_sjit_web_target_name','_sjit_web_render_target_id','_sjit_web_current_costume','_sjit_web_costume_count','_sjit_web_costume_format','_sjit_web_costume_data','_sjit_web_costume_data_size','_sjit_web_costume_width','_sjit_web_costume_height','_sjit_web_costume_rotation_center_x','_sjit_web_costume_rotation_center_y','_sjit_web_answer_pending','_sjit_web_question','_sjit_web_answer','_sjit_web_bubble_text','_sjit_web_bubble_thought']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPF64','wasmMemory']"
)

if(SJIT_WEB_LLVM_JIT)
    target_link_options(xyo-web PRIVATE
        -sEXPORT_ALL=1
        -Wl,--export-all
    )
endif()

set_target_properties(xyo-web PROPERTIES
    OUTPUT_NAME "xyo-web"
)
