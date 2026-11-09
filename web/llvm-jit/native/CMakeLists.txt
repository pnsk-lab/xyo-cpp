cmake_minimum_required(VERSION 3.24)
project(xyo_browser_unified LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Emscripten required for browser compiler/runtime ABI")
endif()
get_filename_component(XYO_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
option(XYO_BROWSER_BUILD_COMPILER "Compile xyo's LLVM IR generator using wasm32 LLVM" OFF)
set(XYO_JIT OFF CACHE BOOL "" FORCE)
set(XYO_BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(XYO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XYO_SDL2 OFF CACHE BOOL "" FORCE)
add_subdirectory("${XYO_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/xyo")

# Use the no-JIT engine but real ABI helpers for LLVM side modules.
get_target_property(_sources xyo SOURCES)
list(FILTER _sources EXCLUDE REGEX "src/jit/nojit_stub\\.cpp$")
set_property(TARGET xyo PROPERTY SOURCES "${_sources}")
file(READ "${XYO_SOURCE}/src/jit/nojit_stub.cpp" _stub)
string(FIND "${_stub}" "// 生成コードからのみ呼ばれる" _end)
if(_end LESS 0)
  message(FATAL_ERROR "nojit_stub.cpp has changed: review the split")
endif()
string(SUBSTRING "${_stub}" 0 ${_end} _engine_stub)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nojit_engine.cpp" "${_engine_stub}")
target_include_directories(xyo PRIVATE "${XYO_SOURCE}/src/jit")
target_sources(xyo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nojit_engine.cpp" "${XYO_SOURCE}/src/jit/rt.cpp")

set(_exports "['_malloc','_free','_xyo_browser_create','_xyo_browser_destroy','_xyo_browser_load_sb3','_xyo_browser_green_flag','_xyo_browser_step','_xyo_browser_stop','_xyo_browser_snapshot','_xyo_browser_key','_xyo_browser_mouse','_xyo_browser_wheel','_xyo_browser_answer','_xyo_browser_audio_count','_xyo_browser_audio_kind','_xyo_browser_audio_id','_xyo_browser_audio_bytes','_xyo_browser_audio_size','_xyo_browser_audio_format','_xyo_browser_audio_volume','_xyo_browser_audio_pitch','_xyo_browser_audio_pan','_xyo_browser_audio_consume','_xyo_browser_audio_complete','_xyo_browser_error','_xyo_browser_abi_descriptor','_xyo_browser_internal_enter','_xyo_browser_internal_leave','_xyo_llvm_prepare','_xyo_llvm_expect_slots','_xyo_llvm_bind_slot','_xyo_llvm_is_sealed','_xyo_llvm_dlopen','_xyo_llvm_dlsym','_xyo_llvm_dlclose','_xyo_llvm_invoke','_xyo_llvm_release','_xyo_llvm_variable','_xyo_llvm_variable_bits','_xyo_varslot_init','_xyo_to_num','_xyo_var_set','_xyo_var_change']")
add_executable(xyo_browser_unified browser_runtime.cpp llvm_host_abi.cpp)
target_include_directories(xyo_browser_unified PRIVATE "${XYO_SOURCE}/src")
target_link_libraries(xyo_browser_unified PRIVATE xyo)
target_link_options(xyo_browser_unified PRIVATE
  --no-entry -sMAIN_MODULE=2 -sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1
  -sEXPORT_ES6=1 -sALLOW_TABLE_GROWTH=1 -sSTACK_SIZE=5242880
  "-sEXPORT_NAME=createXyoBrowserUnified"
  "-sENVIRONMENT=web,worker,node"
  "-sEXPORTED_FUNCTIONS=${_exports}"
  "-sEXPORTED_RUNTIME_METHODS=['FS','HEAPU8','UTF8ToString']")
set_target_properties(xyo_browser_unified PROPERTIES SUFFIX ".mjs")

if(XYO_BROWSER_BUILD_COMPILER)
  if(NOT LLVM_DIR)
    message(FATAL_ERROR "Specify LLVM_DIR from an Emscripten wasm32 LLVM build")
  endif()
  find_package(LLVM CONFIG REQUIRED NO_DEFAULT_PATH PATHS "${LLVM_DIR}")
  target_include_directories(xyo SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_sources(xyo PRIVATE "${XYO_SOURCE}/src/jit/codegen.cpp" "${XYO_SOURCE}/src/jit/parfor.cpp" browser_ir_api.cpp)
  if(LLVM_VERSION_MAJOR GREATER_EQUAL 23)
    target_compile_definitions(xyo PRIVATE XYO_LLVM_GET_OR_INSERT=1)
  endif()
  llvm_map_components_to_libnames(_llvm_libs core support analysis transformutils passes target mc codegen asmprinter webassemblycodegen webassemblydesc webassemblyinfo)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/compiler_entry.cpp" "int main() { return 0; }\n")
  add_executable(xyo_browser_ir "${CMAKE_CURRENT_BINARY_DIR}/compiler_entry.cpp")
  target_link_libraries(xyo_browser_ir PRIVATE xyo ${_llvm_libs})
  target_link_options(xyo_browser_ir PRIVATE
    --no-entry -sALLOW_MEMORY_GROWTH=1 -sMODULARIZE=1 -sEXPORT_ES6=1
    -sSTACK_SIZE=5242880 "-sEXPORT_NAME=createXyoBrowserIr"
    "-sENVIRONMENT=web,worker,node"
    "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','UTF8ToString']"
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_xyo_browser_ir_compile','_xyo_browser_ir_ok','_xyo_browser_ir_text','_xyo_browser_ir_metadata','_xyo_browser_ir_error','_xyo_browser_ir_destroy']")
  set_target_properties(xyo_browser_ir PROPERTIES SUFFIX ".mjs")
endif()
