# Hand-written integer formatter is opt-in and target-specific. Validate
# its standalone i64 ABI (including INT64_MIN and 2-digit boundaries).
if(XYO_ASM_NUM_TO_STR)
  add_executable(xyo-num-to-str-asm-parity
    bench/num_to_str_asm_parity.cpp
    ../src/core/num_to_str_x86_64_sysv.S)
  add_test(NAME xyo-num-to-str-asm-parity
    COMMAND xyo-num-to-str-asm-parity)
  set_tests_properties(xyo-num-to-str-asm-parity PROPERTIES TIMEOUT 60)
endif()

# Scientific digit parsing + ECMAScript layout from the hand-written
# Linux x86-64 ASM. The generic C++ conversion remains the reference.
if(XYO_ASM_ECMA_FORMAT)
  add_executable(xyo-ecma-format-asm-parity
    bench/ecma_format_asm_parity.cpp
    ../src/core/ecma_format_x86_64_sysv.S)
  add_test(NAME xyo-ecma-format-asm-parity
    COMMAND xyo-ecma-format-asm-parity)
  set_tests_properties(xyo-ecma-format-asm-parity PROPERTIES TIMEOUT 60)
endif()

# Dragonbox backend: verify its ECMAScript output against actual Node V8.
# V8 is optional: only register this test when the backend and Node exist.
if(XYO_NUM_TO_STR_DRAGONBOX)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  find_program(XYO_DRAGONBOX_NODE_EXECUTABLE NAMES node)
  if(XYO_DRAGONBOX_NODE_EXECUTABLE AND Python3_EXECUTABLE)
    add_executable(xyo-v8-dragonbox-node-data
      bench/num_to_str_dragonbox_node_data.cpp)
    target_link_libraries(xyo-v8-dragonbox-node-data PRIVATE xyo)
    add_test(NAME xyo-v8-dragonbox-parity
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/num_to_str_dragonbox_node.py"
        "$<TARGET_FILE:xyo-v8-dragonbox-node-data>"
        "${XYO_DRAGONBOX_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/num_to_str_dragonbox_node.mjs")
    set_tests_properties(xyo-v8-dragonbox-parity PROPERTIES TIMEOUT 120)
  endif()
endif()

# Direct Dragonbox ASCII output must preserve actual GC SStr contents
# and the published number cache, not only jsnum::toString's bytes.
if(XYO_NUM_TO_STR_DRAGONBOX AND XYO_NUM_TO_STR_DIRECT_BUFFER)
  add_executable(xyo-v8-direct-sstr-parity
    bench/num_to_str_direct_sstr_parity.cpp)
  target_compile_definitions(xyo-v8-direct-sstr-parity PRIVATE
    XYO_NUM_TO_STR_DRAGONBOX=1)
  target_link_libraries(xyo-v8-direct-sstr-parity PRIVATE xyo)
  add_test(NAME xyo-v8-direct-sstr-parity
    COMMAND xyo-v8-direct-sstr-parity)
  set_tests_properties(xyo-v8-direct-sstr-parity PROPERTIES TIMEOUT 120)
endif()

# Measure the actual number->GC UTF-16 SStr path independently from
# process startup / JIT compile time (manual microbenchmark target).
if(XYO_NUM_TO_STR_DRAGONBOX)
  add_executable(xyo-num-to-str-sstr-micro
    bench/num_to_str_sstr_micro.cpp)
  target_link_libraries(xyo-num-to-str-sstr-micro PRIVATE xyo)
endif()

# Optional native yyjson JSON parser must preserve the legacy DOM.
# No system yyjson dependency is added to the normal test configuration.
if(XYO_JSON_YYJSON)
  add_executable(xyo-json-yyjson-parity platform/json_yyjson_parity.cpp)
  target_link_libraries(xyo-json-yyjson-parity PRIVATE xyo)
  add_test(NAME xyo-json-yyjson-parity COMMAND xyo-json-yyjson-parity)
  set_tests_properties(xyo-json-yyjson-parity PROPERTIES TIMEOUT 60)
endif()

# The fast direct-to-Block SB3 importer must produce the same block graph as
# the legacy BlockJson importer, including primitive IDs and insertion order.
add_executable(xyo-sb3-direct-load-parity
  platform/sb3_direct_load_parity.cpp)
target_link_libraries(xyo-sb3-direct-load-parity PRIVATE xyo)
add_test(NAME xyo-sb3-direct-load-parity
  COMMAND xyo-sb3-direct-load-parity
    "${CMAKE_SOURCE_DIR}/tests/fixtures/basic.sb3"
    "${CMAKE_SOURCE_DIR}/tests/fixtures/calculator_tests.sb3"
    "${CMAKE_SOURCE_DIR}/tests/fixtures/calculator_graph_tests.sb3"
    "${CMAKE_SOURCE_DIR}/tests/fixtures/scratch-parity-test.sb3")
set_tests_properties(xyo-sb3-direct-load-parity PROPERTIES TIMEOUT 120)

# xyo-cpp テスト
# - unit/: cast/compare/list/gc の table-driven テスト
# - fixtures/: refs/scratch-vm/test/fixtures の実行フィクスチャ（M2 以降）
# - differential/: node scratch-vm との差分ランナー
# Architecture-specific context switching must be exercised on macOS runners.
# The same binary also guards the existing Linux/Windows fiber implementations.
if(NOT EMSCRIPTEN)
  add_executable(xyo-fiber-smoke platform/fiber_switch.cpp)
  target_link_libraries(xyo-fiber-smoke PRIVATE xyo)
  add_test(NAME xyo-fiber-smoke COMMAND xyo-fiber-smoke)
endif()

# Manually run host-side numeric continuation A/B microbenchmark.
# Deliberately not part of CTest because timings depend on CPU load.
add_executable(xyo-numeric-continuation-overhead EXCLUDE_FROM_ALL
  bench/numeric_continuation_overhead.cpp)
target_link_libraries(xyo-numeric-continuation-overhead PRIVATE xyo)
# Generation-guarded variable binding cache A/B; manual benchmark only.
add_executable(xyo-numeric-binding-cache EXCLUDE_FROM_ALL
  bench/numeric_binding_cache.cpp)
target_link_libraries(xyo-numeric-binding-cache PRIVATE xyo)

add_executable(xyo-scratch-variable-slots-smoke
  platform/scratch_variable_slots_smoke.cpp)
target_link_libraries(xyo-scratch-variable-slots-smoke PRIVATE xyo)
add_test(NAME xyo-scratch-variable-slots-smoke
  COMMAND xyo-scratch-variable-slots-smoke)

add_executable(xyo-scratch-ir-smoke platform/scratch_ir_smoke.cpp)
target_link_libraries(xyo-scratch-ir-smoke PRIVATE xyo)
add_test(NAME xyo-scratch-ir-smoke COMMAND xyo-scratch-ir-smoke)
add_test(NAME xyo-scratch-ir-basic-sb3
  COMMAND xyo-scratch-ir-smoke
          "${CMAKE_SOURCE_DIR}/tests/fixtures/basic.sb3")
add_test(NAME xyo-scratch-ir-calculator-sb3
  COMMAND xyo-scratch-ir-smoke
          "${CMAKE_SOURCE_DIR}/tests/fixtures/calculator_tests.sb3")
add_test(NAME xyo-scratch-ir-graph-sb3
  COMMAND xyo-scratch-ir-smoke
          "${CMAKE_SOURCE_DIR}/tests/fixtures/calculator_graph_tests.sb3")
add_test(NAME xyo-scratch-ir-parfor-sb3
  COMMAND xyo-scratch-ir-smoke
          "${CMAKE_SOURCE_DIR}/tests/fixtures/parfor_fill.sb3")

add_executable(xyo-numeric-list-abi-smoke platform/numeric_list_abi_smoke.cpp)
target_link_libraries(xyo-numeric-list-abi-smoke PRIVATE xyo)
add_test(NAME xyo-numeric-list-abi-smoke COMMAND xyo-numeric-list-abi-smoke)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND NOT EMSCRIPTEN)
  add_test(NAME xyo-wasm-host-list-abi
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/wasm_host_list_bridge.py"
            "$<TARGET_FILE:xyo-numeric-list-abi-smoke>"
            "${CMAKE_CURRENT_BINARY_DIR}/wasm-list-abi")
endif()

add_executable(xyo-wasm-string-bridge-smoke platform/wasm_string_bridge_smoke.cpp)
target_link_libraries(xyo-wasm-string-bridge-smoke PRIVATE xyo)
add_test(NAME xyo-wasm-string-bridge-smoke COMMAND xyo-wasm-string-bridge-smoke)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND NOT EMSCRIPTEN)
  add_test(NAME xyo-wasm-host-string-abi
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/wasm_host_string_bridge.py"
            "$<TARGET_FILE:xyo-wasm-string-bridge-smoke>"
            "${CMAKE_CURRENT_BINARY_DIR}/wasm-string-abi")
endif()

add_executable(xyo-numeric-ir-smoke platform/numeric_ir_smoke.cpp)
target_compile_options(xyo-numeric-ir-smoke PRIVATE -fno-exceptions -fno-rtti)
add_test(NAME xyo-numeric-ir-smoke COMMAND xyo-numeric-ir-smoke)

# Portable procedure continuation frame layout, nested calls, yield,
# rollback, stale-commit rejection. Can be compiled for native and WASM.
add_executable(xyo-call-frame-abi-smoke platform/call_frame_abi_smoke.cpp)
target_compile_options(xyo-call-frame-abi-smoke PRIVATE -fno-exceptions -fno-rtti)
if(NOT EMSCRIPTEN)
  add_test(NAME xyo-call-frame-abi-smoke COMMAND xyo-call-frame-abi-smoke)
endif()


# IR-001: the native JIT embeds the immutable compilation revision in
# every opt-in procedure entry. A stale source must never reach RT deopt.
if(NOT EMSCRIPTEN)
  add_executable(xyo-native-callframe-revision-smoke
    platform/native_callframe_revision_smoke.cpp)
  target_link_libraries(xyo-native-callframe-revision-smoke PRIVATE xyo)
  target_compile_options(xyo-native-callframe-revision-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-native-callframe-revision-smoke
    COMMAND xyo-native-callframe-revision-smoke)
endif()

# Passive interpreter -> native OSR state capture. Verifies source revision,
# exact GC argument roots and the nested warp/loop checkpoint without
# pretending an LLVM deep-frame continuation is executable yet.
if(NOT EMSCRIPTEN)
  add_executable(xyo-interpreter-osr-snapshot-smoke
    platform/interpreter_osr_snapshot_smoke.cpp)
  target_link_libraries(xyo-interpreter-osr-snapshot-smoke PRIVATE xyo)
  target_compile_options(xyo-interpreter-osr-snapshot-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-interpreter-osr-snapshot-smoke
    COMMAND xyo-interpreter-osr-snapshot-smoke)
  # Real live Block edit after compiled LLVM publication, using the
  # existing Runtime::step public API; no production instrumentation.
  add_executable(xyo-nested-warp-silent-edit-smoke
    platform/nested_warp_silent_edit_smoke.cpp)
  target_link_libraries(xyo-nested-warp-silent-edit-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-silent-edit-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-inflight-deopt-smoke
    platform/nested_warp_inflight_deopt_smoke.cpp)
  target_link_libraries(xyo-nested-warp-inflight-deopt-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-inflight-deopt-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-inflight-body-edit-smoke
    platform/nested_warp_inflight_body_edit_smoke.cpp)
  target_link_libraries(xyo-nested-warp-inflight-body-edit-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-inflight-body-edit-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-bounded-inner-deopt-smoke
    platform/nested_warp_bounded_inner_deopt_smoke.cpp)
  target_link_libraries(xyo-nested-warp-bounded-inner-deopt-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-bounded-inner-deopt-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-inner-yield-deopt-smoke
    platform/nested_warp_inner_yield_deopt_smoke.cpp)
  target_link_libraries(xyo-nested-warp-inner-yield-deopt-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-inner-yield-deopt-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-dynamic-inner-deopt-smoke
    platform/nested_warp_dynamic_inner_deopt_smoke.cpp)
  target_link_libraries(xyo-nested-warp-dynamic-inner-deopt-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-dynamic-inner-deopt-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-triple-yield-deopt-smoke
    platform/nested_warp_triple_yield_deopt_smoke.cpp)
  target_link_libraries(xyo-nested-warp-triple-yield-deopt-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-triple-yield-deopt-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-quad-yield-deopt-smoke
    platform/nested_warp_quad_yield_deopt_smoke.cpp)
  target_link_libraries(xyo-nested-warp-quad-yield-deopt-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-quad-yield-deopt-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_executable(xyo-nested-warp-topology-failclosed-smoke
    platform/nested_warp_topology_edit_failclosed_smoke.cpp)
  target_link_libraries(xyo-nested-warp-topology-failclosed-smoke PRIVATE xyo)
  target_compile_options(xyo-nested-warp-topology-failclosed-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  # A headless Runtime::step test; do not nest in SDL-only tests.
  if(XYO_JIT AND Python3_Interpreter_FOUND)
    add_test(NAME xyo-nested-warp-silent-edit
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_silent_edit.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-silent-edit-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-silent-edit")
    set_tests_properties(xyo-nested-warp-silent-edit
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-inflight-deopt
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_inflight_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-inflight-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-inflight-deopt")
    set_tests_properties(xyo-nested-warp-inflight-deopt
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-inflight-body-edit
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_inflight_body_edit.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-inflight-body-edit-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-inflight-body-edit")
    set_tests_properties(xyo-nested-warp-inflight-body-edit
      PROPERTIES TIMEOUT 600)
    # Keep the optimized, uninstrumented LLVM path under test too:
    # block counting disables some native scalar/loop optimizations.
    add_test(NAME xyo-nested-warp-inflight-body-edit-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_inflight_body_edit.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-inflight-body-edit-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-inflight-body-edit-fast"
        --without-block-count)
    set_tests_properties(xyo-nested-warp-inflight-body-edit-fast
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-bounded-inner-deopt
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_bounded_inner_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-bounded-inner-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-bounded-inner-deopt")
    set_tests_properties(xyo-nested-warp-bounded-inner-deopt
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-bounded-inner-deopt-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_bounded_inner_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-bounded-inner-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-bounded-inner-deopt-fast"
        --without-block-count)
    set_tests_properties(xyo-nested-warp-bounded-inner-deopt-fast
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-inner-yield-deopt
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_inner_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-inner-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-inner-yield-deopt")
    set_tests_properties(xyo-nested-warp-inner-yield-deopt
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-inner-yield-deopt-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_inner_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-inner-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-inner-yield-deopt-fast"
        --without-block-count)
    set_tests_properties(xyo-nested-warp-inner-yield-deopt-fast
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-dynamic-inner-deopt
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_dynamic_inner_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-dynamic-inner-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-dynamic-inner-deopt")
    set_tests_properties(xyo-nested-warp-dynamic-inner-deopt
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-dynamic-inner-deopt-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_dynamic_inner_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-dynamic-inner-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-dynamic-inner-deopt-fast"
        --without-block-count)
    set_tests_properties(xyo-nested-warp-dynamic-inner-deopt-fast
      PROPERTIES TIMEOUT 600)
    # Stronger OSR-012 condition: reset TIMES=17 before EVERY selected
    # native inner repeat, then mutate it to 7 inside its first body.
    # Ensures the mutation happens after actual LLVM promotion, rather
    # than only during the interpreter's warmup before OSR.
    add_test(NAME xyo-nested-warp-dynamic-inner-active
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_dynamic_inner_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-dynamic-inner-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-dynamic-inner-active"
        --outer-count 150000
        --stress-active-times)
    set_tests_properties(xyo-nested-warp-dynamic-inner-active
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-dynamic-inner-active-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_dynamic_inner_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-dynamic-inner-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-dynamic-inner-active-fast"
        --outer-count 150000
        --stress-active-times
        --without-block-count)
    set_tests_properties(xyo-nested-warp-dynamic-inner-active-fast
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-triple-yield-deopt
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_triple_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-triple-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-triple-yield-deopt")
    set_tests_properties(xyo-nested-warp-triple-yield-deopt
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-triple-yield-deopt-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_triple_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-triple-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-triple-yield-deopt-fast"
        --without-block-count)
    set_tests_properties(xyo-nested-warp-triple-yield-deopt-fast
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-triple-yield-fractional
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_triple_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-triple-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-triple-yield-fractional"
        --fractional)
    set_tests_properties(xyo-nested-warp-triple-yield-fractional
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-triple-yield-fractional-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_triple_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-triple-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-triple-yield-fractional-fast"
        --fractional
        --without-block-count)
    set_tests_properties(xyo-nested-warp-triple-yield-fractional-fast
      PROPERTIES TIMEOUT 600)
    # OSR-016 uses the same OSR-013 native smoke binary, with isolated test definitions.
    include("${CMAKE_CURRENT_SOURCE_DIR}/differential/osr016_fractional_boundary_tests.cmake")
    add_test(NAME xyo-nested-warp-quad-yield-deopt
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_quad_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-quad-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-quad-yield-deopt"
        --outer-count 100000)
    set_tests_properties(xyo-nested-warp-quad-yield-deopt
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-quad-yield-deopt-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_quad_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-quad-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-quad-yield-deopt-fast"
        --outer-count 100000
        --without-block-count)
    set_tests_properties(xyo-nested-warp-quad-yield-deopt-fast
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-quad-yield-fractional
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_quad_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-quad-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-quad-yield-fractional"
        --outer-count 100000
        --fractional)
    set_tests_properties(xyo-nested-warp-quad-yield-fractional
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-quad-yield-fractional-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_quad_yield_deopt.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-quad-yield-deopt-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-quad-yield-fractional-fast"
        --outer-count 100000
        --fractional --without-block-count)
    set_tests_properties(xyo-nested-warp-quad-yield-fractional-fast
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-topology-failclosed
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_topology_failclosed.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-topology-failclosed-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-topology-failclosed"
        --outer-count 100000)
    set_tests_properties(xyo-nested-warp-topology-failclosed
      PROPERTIES TIMEOUT 600)
    add_test(NAME xyo-nested-warp-topology-failclosed-fast
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_topology_failclosed.py"
        --harness "$<TARGET_FILE:xyo-nested-warp-topology-failclosed-smoke>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-topology-failclosed-fast"
        --outer-count 100000 --without-block-count)
    set_tests_properties(xyo-nested-warp-topology-failclosed-fast
      PROPERTIES TIMEOUT 600)
  endif()
endif()

# IR-002: GC-rooted handles, portable mixed-value CallFrame and commit-once
# observable effects. Both native and Emscripten link the identical sources;
# the WASM artifacts are additionally run under Node by the parity harness.
foreach(effect_smoke IN ITEMS
    host_effect_abi_smoke
    gc_value_handles_smoke
    rooted_call_stack_smoke
    commit_once_effects_smoke
    effect_callstack_transaction_smoke)
  string(REPLACE "_" "-" effect_test_name "xyo-${effect_smoke}")
  add_executable(${effect_test_name}
    platform/${effect_smoke}.cpp)
  target_link_libraries(${effect_test_name} PRIVATE xyo)
  target_compile_options(${effect_test_name}
    PRIVATE -fno-exceptions -fno-rtti)
  if(NOT EMSCRIPTEN)
    add_test(NAME ${effect_test_name} COMMAND ${effect_test_name})
  endif()
endforeach()

# Real Runtime::journalBroadcast delivery (with synchronous nested
# subscribers), followed by an actual opt-in LLVM/Interpreter SB3 check.
# Real renderer/looks callback integration, GC under delivery and exact
# once-per-primitive pen stamp. Operates without an SDL window.
add_executable(xyo-runtime-observable-journal-smoke
  platform/runtime_observable_journal_smoke.cpp)
target_link_libraries(xyo-runtime-observable-journal-smoke PRIVATE xyo)
target_compile_options(xyo-runtime-observable-journal-smoke
  PRIVATE -fno-exceptions -fno-rtti)
if(NOT EMSCRIPTEN)
  add_test(NAME xyo-runtime-observable-journal-smoke
    COMMAND xyo-runtime-observable-journal-smoke)
endif()

add_executable(xyo-runtime-broadcast-journal-smoke
  platform/runtime_broadcast_journal_smoke.cpp)
target_link_libraries(xyo-runtime-broadcast-journal-smoke PRIVATE xyo)
target_compile_options(xyo-runtime-broadcast-journal-smoke
  PRIVATE -fno-exceptions -fno-rtti)
if(NOT EMSCRIPTEN)
  add_test(NAME xyo-runtime-broadcast-journal-smoke
    COMMAND xyo-runtime-broadcast-journal-smoke)
endif()

if(Python3_Interpreter_FOUND AND XYO_JIT
   AND XYO_JIT_BACKEND STREQUAL "llvm" AND XYO_BUILD_SHELL
   AND NOT EMSCRIPTEN)
  add_test(NAME xyo-runtime-broadcast-journal-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/runtime_broadcast_journal.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/runtime-broadcast-journal")
  set_tests_properties(xyo-runtime-broadcast-journal-parity
    PROPERTIES TIMEOUT 120)
  # Effectful receiver startup is committed only once. Both recipient
  # scripts must finish their async control_wait before the parent resumes.
  add_test(NAME xyo-runtime-broadcast-wait-journal-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/runtime_broadcast_wait_journal.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/runtime-broadcast-journal")
  set_tests_properties(xyo-runtime-broadcast-wait-journal-parity
    PROPERTIES TIMEOUT 120)
  add_test(NAME xyo-runtime-looks-journal-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/runtime_looks_journal.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/runtime-looks-journal")
  set_tests_properties(xyo-runtime-looks-journal-parity
    PROPERTIES TIMEOUT 120)
  add_test(NAME xyo-runtime-pen-journal-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/runtime_pen_journal.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/runtime-pen-journal")
  set_tests_properties(xyo-runtime-pen-journal-parity
    PROPERTIES TIMEOUT 120)
  add_test(NAME xyo-runtime-clone-journal-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/runtime_clone_journal.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/runtime-clone-journal")
  set_tests_properties(xyo-runtime-clone-journal-parity
    PROPERTIES TIMEOUT 120)
endif()

# The executable CFG subset must generate valid LLVM IR and real WASM from
# the same graph, including a two-Phi backedge and a conditional merge.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  # Actual runtime Thread/Sequencer integration with an ORC-compiled
  # resumable numeric kernel, including a mid-loop interpreter deopt.
  add_executable(xyo-numeric-sequencer-smoke
    platform/numeric_sequencer_smoke.cpp)
  target_include_directories(xyo-numeric-sequencer-smoke PRIVATE
    ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-numeric-sequencer-smoke PRIVATE
    xyo ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  add_test(NAME xyo-numeric-sequencer-smoke
    COMMAND xyo-numeric-sequencer-smoke)
  add_test(NAME xyo-numeric-sequencer-auto
    COMMAND xyo-numeric-sequencer-smoke --auto)
  set_tests_properties(xyo-numeric-sequencer-auto PROPERTIES
    ENVIRONMENT "XYO_JIT_COMMON_CFG=1")
  add_executable(xyo-ir-cfg-smoke platform/cfg_lower_smoke.cpp)
  target_include_directories(xyo-ir-cfg-smoke PRIVATE ${LLVM_INCLUDE_DIRS})
  # The shared Scratch frontend now uses the runtime's exact JS/Scratch
  # numeric conversion (including strings), not header-only stubs.
  target_link_libraries(xyo-ir-cfg-smoke PRIVATE xyo ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-ir-cfg-backends
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/ir_cfg_backends.py"
              "$<TARGET_FILE:xyo-ir-cfg-smoke>"
              "${CMAKE_CURRENT_BINARY_DIR}/ir-cfg")
  endif()
endif()

# Optional microbenchmark. Explicit build only; never part of normal CI.
if(XYO_JIT AND NOT EMSCRIPTEN)
  add_executable(xyo-to-num-runtime-micro EXCLUDE_FROM_ALL
    bench/to_num_runtime_micro.cpp)
  target_link_libraries(xyo-to-num-runtime-micro PRIVATE xyo)
endif()

# Real SB3 runs compare interpreter, eager/hotspot JIT and CLI overrides.
if(Python3_Interpreter_FOUND AND XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm"
   AND XYO_BUILD_SHELL AND NOT EMSCRIPTEN)
  add_test(NAME xyo-string-storage-cli-parity
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/string_storage_modes.py"
            "$<TARGET_FILE:xyo-cli>")
  set_tests_properties(xyo-string-storage-cli-parity PROPERTIES TIMEOUT 180)

  # Scheduler-owned GC-rooted native source frame -> exact Scratch
  # interpreter loop restoration, including fail-closed recursive cases.
  add_test(NAME xyo-native-simple-deopt-parity
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/native_simple_resume.py"
            --native "$<TARGET_FILE:xyo-cli>"
            --out "${CMAKE_CURRENT_BINARY_DIR}/native-simple-resume")
  # This is a 915-run native frame/deopt parity matrix; the old 120s
  # timeout was exceeded after many passing cases on a loaded runner.
  set_tests_properties(xyo-native-simple-deopt-parity
    PROPERTIES TIMEOUT 600)
endif()

# Opt-in native allocation benchmark; not part of CTest.
add_executable(xyo-string-leaf-native-bench EXCLUDE_FROM_ALL
  bench/string_leaf_native.cpp)
target_link_libraries(xyo-string-leaf-native-bench PRIVATE xyo)

if(Python3_Interpreter_FOUND AND XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm"
   AND XYO_BUILD_SHELL AND NOT EMSCRIPTEN)
  add_test(NAME xyo-string-leaf-cli-parity
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/string_leaf_modes.py"
            "$<TARGET_FILE:xyo-cli>")
  set_tests_properties(xyo-string-leaf-cli-parity PROPERTIES TIMEOUT 180)
endif()

# Manually invoked short-result GC/latency A/B benchmark.
add_executable(xyo-string-short-intern-native-bench EXCLUDE_FROM_ALL
  bench/string_short_intern_native.cpp)
target_link_libraries(xyo-string-short-intern-native-bench PRIVATE xyo)

if(Python3_Interpreter_FOUND AND XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm"
   AND XYO_BUILD_SHELL AND NOT EMSCRIPTEN)
  add_test(NAME xyo-string-short-intern-sb3-parity
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/string_short_intern.py"
            "$<TARGET_FILE:xyo-cli>")
  set_tests_properties(xyo-string-short-intern-sb3-parity PROPERTIES TIMEOUT 180)
endif()

# Optional short ASCII interning: verify GC roots, concurrency, and UTF-16
# equivalence for both leaf representations.
add_executable(xyo-string-short-intern-smoke platform/string_short_intern.cpp)
target_link_libraries(xyo-string-short-intern-smoke PRIVATE xyo)
foreach(intern off on)
  foreach(leaf utf16 latin1)
    add_test(NAME xyo-string-short-intern-${intern}-${leaf}
      COMMAND xyo-string-short-intern-smoke ${intern} ${leaf})
    set_tests_properties(xyo-string-short-intern-${intern}-${leaf} PROPERTIES
      ENVIRONMENT "XYO_STRING_SHORT_INTERN=${intern}" TIMEOUT 60)
  endforeach()
endforeach()

# Compact Latin-1 leaves must work in isolation and across Rope flattening.
add_executable(xyo-string-leaf-smoke platform/string_leaf_modes.cpp)
target_link_libraries(xyo-string-leaf-smoke PRIVATE xyo)
foreach(leaf utf16 latin1)
  add_test(NAME xyo-string-leaf-${leaf}
    COMMAND xyo-string-leaf-smoke ${leaf})
  set_tests_properties(xyo-string-leaf-${leaf} PROPERTIES
    ENVIRONMENT "XYO_STRING_LEAF=${leaf}" TIMEOUT 60)
endforeach()

# The 3 storage policies must preserve the same Scratch string semantics.
add_executable(xyo-string-storage-smoke platform/string_storage_modes.cpp)
target_link_libraries(xyo-string-storage-smoke PRIVATE xyo)
foreach(mode rope flat append-log)
  add_test(NAME xyo-string-storage-${mode}
    COMMAND xyo-string-storage-smoke ${mode})
  set_tests_properties(xyo-string-storage-${mode} PROPERTIES
    ENVIRONMENT "XYO_STRING_STORAGE=${mode}" TIMEOUT 60)
endforeach()

# xyo-tests has one main() (unit/test_cast.cpp). Standalone smoke tests
# also live in unit/ and supply their own main(), so do not glob them.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS unit/test_*.cpp)
# MockRenderer subclasses the runtime's no-RTTI RendererAPI.
set_source_files_properties(unit/test_scheduler.cpp unit/test_motion_numeric.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-rtti;-fno-exceptions")
if(TEST_SOURCES)
  add_executable(xyo-tests ${TEST_SOURCES})
  target_link_libraries(xyo-tests PRIVATE xyo)
  if(XYO_STRING_CONS_POOL)
    target_compile_definitions(xyo-tests PRIVATE XYO_STRING_CONS_POOL=1)
  endif()
  add_test(NAME xyo-tests COMMAND xyo-tests)
  # 通常実行は HotSpot（既定）だが、テストは従来どおり全スクリプトを
  # 即コンパイルする eager で JIT の意味論を検証する。
  set_tests_properties(xyo-tests PROPERTIES
    ENVIRONMENT "XYO_JIT_MODE=eager")
endif()

# START-016: the real editor (software SDL renderer) must commit each
# semantic field edit through Blocks::setBlockField, advancing the AOT
# source revision before native code can be scheduled again.
if(TARGET xyo-cli AND SDL2_FOUND AND NOT EMSCRIPTEN)
  add_test(NAME xyo-start016-editor-source-revision
    COMMAND "${CMAKE_COMMAND}" -E env
      SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XYO_PEN_CPU=1
      XYO_EDIT_TEST=1
      "$<TARGET_FILE:xyo-cli>" edit
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/basic.sb3")
  set_tests_properties(xyo-start016-editor-source-revision
    PROPERTIES TIMEOUT 60
               PASS_REGULAR_EXPRESSION "EDIT-TEST: all checks passed")
endif()

# Portable running-state snapshots, including SDL audio and pen pixels.
# The regression harness uses the Linux SDL offscreen driver; do not add it
# to headless or non-Linux targets.
if(TARGET xyo-cli AND SDL2_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-hot-start
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/hot_start.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-hot-start PROPERTIES TIMEOUT 180)
    # Check the Scratch project's own test oracle under real SDL offscreen
    # playback. Cross-backend output equality is NOT a Scratch parity score.
    add_test(NAME xyo-gui-scratch-parity
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/gui_scratch_parity_regression.py"
        --native "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-gui-scratch-parity PROPERTIES TIMEOUT 400)
    # Run the full current ScratchBench parity suite whenever the generated
    # fixture is present. The smaller checked-in fixture stays mandatory for
    # clean CI clones without a generated scratch-bench/.cache directory.
    set(XYO_FULL_PARITY_SB3
      "${CMAKE_SOURCE_DIR}/scratch-bench/.cache/build/parity.sb3")
    if(EXISTS "${XYO_FULL_PARITY_SB3}")
      add_test(NAME xyo-gui-scratch-parity-full
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/gui_scratch_parity_regression.py"
          --native "$<TARGET_FILE:xyo-cli>"
          --project "${XYO_FULL_PARITY_SB3}"
          --minimum 2656
          --modes off hotspot eager)
      set_tests_properties(xyo-gui-scratch-parity-full PROPERTIES TIMEOUT 400)
    endif()
    # An opt-in worker warmup must not replay the prefix of a long warp
    # procedure. This is an isolated script, not a claim of global
    # scheduling parity when warmup changes cross-thread start ordering.
    add_test(NAME xyo-gui-async-jit-warmup
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/gui_async_jit_warmup_smoke.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/gui-async-warmup")
    set_tests_properties(xyo-gui-async-jit-warmup PROPERTIES TIMEOUT 150)
    add_test(NAME xyo-gui-async-jit-prefetch
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/gui_async_prefetch_smoke.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/gui-async-prefetch")
    set_tests_properties(xyo-gui-async-jit-prefetch PROPERTIES TIMEOUT 150)
    # Passive source-path+GC snapshot from a real nested warp Scratch SB3.
    # No native continuation is activated by this diagnostic-only test.
    add_test(NAME xyo-interpreter-osr-live-warp-snapshot
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/interpreter_osr_snapshot.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/interpreter-osr-snapshot")
    set_tests_properties(xyo-interpreter-osr-live-warp-snapshot PROPERTIES TIMEOUT 45)
    # A multi-argument warp procedure is not safe for the experimental
    # zero-argument nested LLVM loop entry: no partial SSA/GC frame transfer.
    add_test(NAME xyo-nested-warp-args-osr-refusal
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_args_refusal.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-args-osr-refusal")
    set_tests_properties(xyo-nested-warp-args-osr-refusal PROPERTIES TIMEOUT 180)
    # Native loop body independent of args may resume inside a parameterised
    # warp procedure; any actual arg reporter within that region fails closed.
    add_test(NAME xyo-nested-warp-args-preserved-osr
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_args_preserved.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-args-preserved-osr")
    set_tests_properties(xyo-nested-warp-args-preserved-osr PROPERTIES TIMEOUT 240)
    # Explicitly opt into native argument reads inside the selected loop.
    # Undefined/unknown arguments must preserve the interpreter fallback.
    add_test(NAME xyo-nested-warp-direct-arguments-osr
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_direct_args.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-direct-arguments-osr")
    set_tests_properties(xyo-nested-warp-direct-arguments-osr
      PROPERTIES TIMEOUT 360)
    # Separate opt-in: root->outer warp->inner warp->counted repeat.
    # Require real 3-frame promotion, two exact suffix returns and GC roots.
    add_test(NAME xyo-nested-warp-deep-osr
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_deep_osr.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-deep-osr")
    set_tests_properties(xyo-nested-warp-deep-osr PROPERTIES TIMEOUT 540)
    # Bounded generic source-call path (up to eight warp callers).
    add_test(NAME xyo-nested-warp-stack-osr
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_stack_osr.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-stack-osr")
    set_tests_properties(xyo-nested-warp-stack-osr PROPERTIES TIMEOUT 600)
    # Exact IF/IF-ELSE branch frames in the retained warp OSR ancestry.
    add_test(NAME xyo-nested-warp-branch-osr
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_branch_osr.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-branch-osr")
    set_tests_properties(xyo-nested-warp-branch-osr PROPERTIES TIMEOUT 600)
    # Retain live counted outer-loop counters across native inner-loop OSR.
    add_test(NAME xyo-nested-warp-parent-repeat-osr
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_parent_repeat_osr.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-parent-repeat-osr")
    set_tests_properties(xyo-nested-warp-parent-repeat-osr
      PROPERTIES TIMEOUT 600)
    # Independent IR-001 source PC / immutable return-site / typed loop
    # proof at a live nested-warp Interpreter->LLVM adoption checkpoint.
    add_test(NAME xyo-nested-warp-source-proof
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_source_proof.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-source-proof")
    set_tests_properties(xyo-nested-warp-source-proof
      PROPERTIES TIMEOUT 600)
    # Further opt-in: exact, declared arg reporters inside a native loop
    # remain backed by the live interpreter argument frame.
    add_test(NAME xyo-nested-warp-argument-reads
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_argreads.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-argument-reads")
    set_tests_properties(xyo-nested-warp-argument-reads PROPERTIES TIMEOUT 240)
    # Force GC through repeated pure string joins while arguments are live
    # across the interpreter -> native -> interpreter continuation.
    add_test(NAME xyo-nested-warp-argument-gc
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_argroots_gc.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-argument-gc")
    set_tests_properties(xyo-nested-warp-argument-gc PROPERTIES TIMEOUT 240)
    # Dynamic argument in repeat TIMES is evaluated once by the original
    # interpreter. Native OSR starts from the saved remaining counter.
    add_test(NAME xyo-nested-warp-argument-times
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_argtimes.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_BINARY_DIR}/nested-warp-argument-times")
    set_tests_properties(xyo-nested-warp-argument-times PROPERTIES TIMEOUT 240)
  endif()
endif()

# Focused interpreter-mode regressions: event-start reentrancy and
# broadcast/scalar ID collisions. Keep separate from xyo-tests, whose
# scheduler checks intentionally run in eager-JIT mode.
add_executable(xyo-interpreter-parity-regressions
  differential/interpreter_parity_regressions.cpp)
target_link_libraries(xyo-interpreter-parity-regressions PRIVATE xyo)
add_test(NAME xyo-interpreter-parity-regressions
  COMMAND xyo-interpreter-parity-regressions)
set_tests_properties(xyo-interpreter-parity-regressions PROPERTIES
  ENVIRONMENT "XYO_JIT_MODE=off")

# Production HotSpot chooses per script whether to compile procedures
# on first use. Verify the CLI policy, selected tier and Scratch output.
if(TARGET xyo-cli)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-jit-lazy-integration
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_lazy_integration.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-lazy-integration PROPERTIES TIMEOUT 180)

    # Preserve Scratch semantics with dynamic limits, fractional bounds, or
    # index mutation when optional static induction analysis is enabled.
    add_test(NAME xyo-jit-induction-ranges
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_induction_ranges.py"
              --binary "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}/jit-induction-range-fixtures"
              --quick)
    set_tests_properties(xyo-jit-induction-ranges PROPERTIES TIMEOUT 180)

    # Both optimizations are independent compile-time switches. Exercise
    # the combined for-each integer-counter / value-range lowering.
    add_test(NAME xyo-jit-foreach-i32
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_induction_ranges.py"
              --binary "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}/jit-foreach-i32-fixtures"
              --foreach-i32 --quick)
    set_tests_properties(xyo-jit-foreach-i32 PROPERTIES TIMEOUT 180)

    # Exact half-tie semantics, dynamic counts, nested repeats and fallback.
    add_test(NAME xyo-jit-repeat-i32
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_repeat_integer_counter.py"
              --binary "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}/jit-repeat-i32-fixtures")
    set_tests_properties(xyo-jit-repeat-i32 PROPERTIES TIMEOUT 180)

    # Verify transient string escape proofs using real Scratch SB3 programs.
    # Includes independent variable updates, nested joins, multiple builders,
    # Unicode, while/until/for-each, and proof rejection for side effects.
    add_test(NAME xyo-jit-transient-strings
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/bench/algos/bench_transient_extended.py"
              --binary "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}/jit-transient-fixtures"
              --n 257)
    set_tests_properties(xyo-jit-transient-strings PROPERTIES TIMEOUT 180)

    # Guarded resumption of promoted variables, including writes by another script.
    add_test(NAME xyo-jit-epoch-guard
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/epoch_guard.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-epoch-guard PROPERTIES TIMEOUT 180)

    # Event-driven pure wait-until: interpreter, eager, HotSpot, parallel,
    # time-dependent/impure fallback, and multiple waiters with a writer.
    add_test(NAME xyo-event-driven-scheduler
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/event_scheduler.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-event-driven-scheduler PROPERTIES TIMEOUT 180)


    # Real Scratch SB3 Unicode contains/equals parity with optimized and
    # legacy case-fold paths across interpreter, eager JIT and HotSpot.
    add_test(NAME xyo-string-unicode-fast
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/string_unicode_fast.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-string-unicode-fast PROPERTIES TIMEOUT 180)

    # Dynamic list comparisons, mixed types and changing list contents.
    add_test(NAME xyo-jit-plain-list-compare
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/plain_numeric_cmp.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-plain-list-compare PROPERTIES TIMEOUT 120)


    # Independently generated bubble/quick/merge sort SB3 output parity.
    add_test(NAME xyo-jit-plain-list-sort
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/plain_numeric_sort_algos.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-plain-list-sort PROPERTIES TIMEOUT 180)

    # Punctuation equality, immutable digit-set contains and warp-only
    # recursive list procedures: JIT/interpreter parity and fallback guards.
    add_test(NAME xyo-jit-ascii-fastpaths
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_ascii_fastpaths.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-ascii-fastpaths PROPERTIES TIMEOUT 180)

    # Preserve exact loop semantics across interpreter/JIT, warp/non-warp,
    # numeric strings, negatives, fractional values and precision fallback.
    add_test(NAME xyo-jit-warp-induction
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_warp_induction.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-warp-induction PROPERTIES TIMEOUT 180)


    # LLVM SSA arithmetic reductions in counted repeat and for-each. Check
    # Scratch parity across warp polls, fallback after type changes, numeric
    # strings, fractional bounds and both LLVM O2/O3 modes.
    add_test(NAME xyo-jit-repeat-ssa
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_repeat_ssa.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-repeat-ssa PROPERTIES TIMEOUT 180)

    # General lazy numeric type guards on SSA-promoted Scratch values.
    add_test(NAME xyo-jit-lazy-typed-ssa
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_lazy_typed_ssa.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-lazy-typed-ssa PROPERTIES TIMEOUT 180)

    # Mixed Scratch storage types, including Boolean/color/NaN/whitespace
    # values and repeated string flattening across warp/non-warp yields.
    add_test(NAME xyo-jit-type-flow-safety
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_type_flow_safety.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-type-flow-safety PROPERTIES TIMEOUT 180)

    # CFG-proven numeric-copy writes can select eager numeric SSA safely.
    add_test(NAME xyo-jit-cfg-type-hints
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_cfg_type_hints.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-cfg-type-hints PROPERTIES TIMEOUT 180)

    # Generated mixed-type programs, deterministic seed, interpreter parity
    # across eager/hotspot and experimental CFG numeric-shadow hints.
    add_test(NAME xyo-jit-type-flow-fuzz
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_type_flow_fuzz.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-type-flow-fuzz PROPERTIES TIMEOUT 180)

    # Mixed variable: boxed and numeric SSA PHIs on IF pass-through edge.
    add_test(NAME xyo-jit-if-phi
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_if_phi.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-if-phi PROPERTIES TIMEOUT 180)

    # Same LLVM basic block, repeated reads of a promotable mixed scalar.
    add_test(NAME xyo-jit-local-read-forward
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_local_read_forward.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-local-read-forward PROPERTIES TIMEOUT 180)

    # Same resolved scalar reporter on both arithmetic operands.
    add_test(NAME xyo-jit-duplicate-scalar-numeric
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_duplicate_scalar_numeric.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-duplicate-scalar-numeric PROPERTIES TIMEOUT 180)

    # Fresh numeric strings exercise xyo_to_num's cold parse and cache paths.
    add_test(NAME xyo-jit-to-num-fastpath
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_to_num_fastpath.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-to-num-fastpath PROPERTIES TIMEOUT 180)

    # Direct radix String->Number parsing with dynamically joined strings.
    add_test(NAME xyo-jit-to-num-radix
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_to_num_radix.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-to-num-radix PROPERTIES TIMEOUT 180)

    # Guarded LLVM SSA for branch-heavy, pure integer repeat bodies.
    add_test(NAME xyo-jit-conditional-repeat-ssa
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_conditional_repeat_ssa.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-conditional-repeat-ssa PROPERTIES TIMEOUT 180)

    # Prove selected i32 and rejected i64/fallback paths in optimized LLVM IR.
    add_test(NAME xyo-jit-conditional-i32-selection
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_conditional_i32_selection.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-conditional-i32-selection PROPERTIES TIMEOUT 180)


    # LLVM-IR scheduler polling for empty warp repeats.
    add_test(NAME xyo-jit-inline-empty-scheduler
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_empty_inline_scheduler.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-inline-empty-scheduler PROPERTIES TIMEOUT 180)

    # Native code optimization/materialization on a worker while
    # Scratch interprets; promote only at a safe script entry.
    add_test(NAME xyo-jit-async-compile
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_async_compile.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-async-compile PROPERTIES TIMEOUT 180)

    # Interpreter -> native on-stack replacement at a top-level loop head.
    # Captured remaining count, suffix execution, conditional loop opt-in,
    # and dynamic/procedure fallback must match the interpreter.
    add_test(NAME xyo-jit-osr
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_osr.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-osr PROPERTIES TIMEOUT 180)

    # Guarded SSA chunks share an LLVM-IR deadline poll after publishing
    # the exact Scratch yield counter phase; independently test ON/OFF.
    add_test(NAME xyo-jit-ssa-scheduler
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_ssa_scheduler.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-ssa-scheduler PROPERTIES TIMEOUT 180)

    add_test(NAME xyo-jit-foreach-ssa
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_foreach_ssa.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-foreach-ssa PROPERTIES TIMEOUT 180)


    # Pure scalar copy-chain + constant-change loops: LLVM integer SSA,
    # canonical NaN/number guards, warp timeout phase, non-warp fallback.
    add_test(NAME xyo-jit-repeat-copy-ssa
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_repeat_copy_ssa.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-repeat-copy-ssa PROPERTIES TIMEOUT 180)


    # Runtime SStr numeric cache reuse in literal equality, preserving
    # Scratch string identity, lexical fallback and warp shadow reloads.
    add_test(NAME xyo-jit-eq-numeric-cache
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_eq_numeric_cache.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-eq-numeric-cache PROPERTIES TIMEOUT 180)


    # Dynamic string/string =/< />: inline previously parsed numbers and
    # short ASCII case-insensitive lexical order, preserving Unicode fallback.
    add_test(NAME xyo-jit-string-pair-compare
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_numeric_string_pair_cmp.py"
              "$<TARGET_FILE:xyo-cli>")
    # This runs two generated projects across interpreter/JIT and multiple
    # optimization settings. On GitHub-hosted LLVM 23 runners, the previous
    # 180s budget timed out after valid partial results (15/24 cases).
    set_tests_properties(xyo-jit-string-pair-compare PROPERTIES TIMEOUT 480)
  endif()
endif()

# Display-only analytic pen rerasterization has an independent CPU unit.
add_executable(xyo-dlss-pen-replay-unit unit/dlss_pen_replay_unit.cpp)
add_test(NAME xyo-dlss-pen-replay-unit COMMAND xyo-dlss-pen-replay-unit)
set_tests_properties(xyo-dlss-pen-replay-unit PROPERTIES
  LABELS "render;dlss;pen;subpixel")

# Pure CPU Halton 2x3 jitter sequence (no SDL/Vulkan dependency).
add_executable(xyo-dlss-jitter-unit unit/dlss_jitter_unit.cpp)
add_test(NAME xyo-dlss-jitter-unit COMMAND xyo-dlss-jitter-unit)
set_tests_properties(xyo-dlss-jitter-unit PROPERTIES
  LABELS "render;dlss;jitter")

# Standalone conservative sprite motion metadata regression.
# Opt-in conservative PNG/SVG coverage tile metadata is independent of GPU.
add_executable(xyo-dlss-opaque-coverage-unit unit/dlss_opaque_coverage_unit.cpp)
add_test(NAME xyo-dlss-opaque-coverage-unit
  COMMAND xyo-dlss-opaque-coverage-unit)
set_tests_properties(xyo-dlss-opaque-coverage-unit PROPERTIES
  LABELS "render;dlss;alpha;coverage")

add_executable(xyo-dlss-rigid-motion-unit unit/dlss_rigid_motion_unit.cpp)
add_test(NAME xyo-dlss-rigid-motion-unit
  COMMAND xyo-dlss-rigid-motion-unit)
set_tests_properties(xyo-dlss-rigid-motion-unit PROPERTIES
  LABELS "render;dlss;rigid-motion")

# Pure CPU metadata regression: no NVIDIA SDK or SDL required.
add_executable(xyo-dlss-alpha-coverage-unit
  unit/dlss_alpha_coverage_unit.cpp)
add_test(NAME xyo-dlss-alpha-coverage-unit
  COMMAND xyo-dlss-alpha-coverage-unit)
set_tests_properties(xyo-dlss-alpha-coverage-unit PROPERTIES
  LABELS "render;dlss;alpha")

file(GLOB RENDER_TEST_SOURCES CONFIGURE_DEPENDS render/*.cpp)
# Standalone NVIDIA/Vulkan hardware smoke tests have their own main().
# Never glob them into the standard renderer parity executable.
list(FILTER RENDER_TEST_SOURCES EXCLUDE REGEX "/dlss_vk_.*\\.cpp$")
if(RENDER_TEST_SOURCES AND SDL2_FOUND)
  add_executable(xyo-render-tests ${RENDER_TEST_SOURCES})
  target_link_libraries(xyo-render-tests PRIVATE xyo)
  add_test(NAME xyo-render-tests COMMAND xyo-render-tests)
  set_tests_properties(xyo-render-tests PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XYO_JIT_MODE=eager")
endif()

# Hardware-only NVIDIA NGX / Vulkan + SDL OpenGL integration test.
# Keep it out of default build/CTest: CI runners may not have an NVIDIA GPU,
# the NGX runtime model, or an active GL display.
if(XYO_NVSDK_DLSS_VK AND SDL2_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(xyo-dlss-sdl-color-smoke EXCLUDE_FROM_ALL
    render/dlss_vk_sdl_color_smoke.cpp)
  target_link_libraries(xyo-dlss-sdl-color-smoke PRIVATE xyo)
  # Hardware-only cross-queue Optical Flow / RG16F conversion test,
  # independent of the proprietary NGX runtime.
  add_executable(xyo-dlss-optical-flow-bridge EXCLUDE_FROM_ALL
    render/dlss_vk_optical_flow_bridge_smoke.cpp)
  target_link_libraries(xyo-dlss-optical-flow-bridge PRIVATE xyo)
  # Independent compute-shader test: the R8 current-color-bias image must
  # contain the exact rectangles supplied, with no CPU-side pixel mask.
  add_executable(xyo-dlss-vk-bias-mask EXCLUDE_FROM_ALL
    render/dlss_vk_bias_mask_smoke.cpp)
  target_link_libraries(xyo-dlss-vk-bias-mask PRIVATE xyo)

  option(XYO_DLSS_REGISTER_GPU_TESTS
    "Register actual NVIDIA hardware NGX tests in CTest" OFF)
  if(XYO_DLSS_REGISTER_GPU_TESTS)
    add_test(NAME xyo-dlss-vk-bias-mask COMMAND xyo-dlss-vk-bias-mask)
    set_tests_properties(xyo-dlss-vk-bias-mask PROPERTIES
      TIMEOUT 30 LABELS "gpu;dlss;alpha;bias-mask;manual")
    if(Python3_Interpreter_FOUND AND TARGET xyo-cli)
      add_test(NAME xyo-dlss-rigid-project-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_rigid_project_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-rigid-project-smoke PROPERTIES
        TIMEOUT 100
        LABELS "gpu;dlss;rigid-motion;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    if(Python3_Interpreter_FOUND AND TARGET xyo-cli)
      if(Python3_Interpreter_FOUND AND TARGET xyo-cli)
      add_test(NAME xyo-dlss-adaptive-jitter-real-project
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_adaptive_jitter_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-adaptive-jitter-real-project PROPERTIES
        TIMEOUT 100 LABELS "gpu;dlss;jitter;adaptive;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-jitter-real-project
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_jitter_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-jitter-real-project PROPERTIES
        TIMEOUT 100 LABELS "gpu;dlss;jitter;pen;subpixel;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-primitive-real-project
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_primitive_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-primitive-real-project PROPERTIES
        TIMEOUT 120 LABELS "gpu;dlss;jitter;pen;primitive;subpixel;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-gpu-replay-real-project
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_gpu_replay_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-gpu-replay-real-project PROPERTIES
        TIMEOUT 120 LABELS "gpu;dlss;jitter;pen;primitive;gpu-replay;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-gpu-fence-real-project
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_gpu_fence_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-gpu-fence-real-project PROPERTIES
        TIMEOUT 120 LABELS "gpu;dlss;jitter;pen;primitive;gpu-fence;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-gpu-temporal-input
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_gpu_temporal_diagnostic.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-gpu-temporal-input PROPERTIES
        TIMEOUT 120 LABELS "gpu;dlss;jitter;pen;primitive;temporal-diagnostic;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-gpu-fp64-precision
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_gpu_precision_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-gpu-fp64-precision PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;jitter;pen;primitive;fp64;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-gpu-quantized-parity
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_gpu_quantized_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-gpu-quantized-parity PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;jitter;pen;primitive;fp64;quantized;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-pen-gpu-quantized-stress
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_gpu_quantized_stress.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-gpu-quantized-stress PROPERTIES
        TIMEOUT 160 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;jitter;pen;primitive;fp64;quantized;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-hidden-window-gpu-parity
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_hidden_window_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-hidden-window-gpu-parity PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;jitter;pen;headless;quantized;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-surfaceless-gpu-parity
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_surfaceless_gpu_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-surfaceless-gpu-parity PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;jitter;pen;headless;offscreen;quantized;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-frame-series
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_frame_series_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-frame-series PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;capture;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-parity
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-parity PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-pressure
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_pressure_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-pressure PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-rgb-pressure
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_pressure_smoke.py"
          "$<TARGET_FILE:xyo-cli>" --rgb)
      set_tests_properties(xyo-dlss-offscreen-pbo-rgb-pressure PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;rgb;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-pool-pressure
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_pressure_smoke.py"
          "$<TARGET_FILE:xyo-cli>" --pool)
      set_tests_properties(xyo-dlss-offscreen-pbo-pool-pressure PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;pool;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-rgb-pool-pressure
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_pressure_smoke.py"
          "$<TARGET_FILE:xyo-cli>" --rgb --pool)
      set_tests_properties(xyo-dlss-offscreen-pbo-rgb-pool-pressure PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;rgb;pool;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-direct-pressure
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_pressure_smoke.py"
          "$<TARGET_FILE:xyo-cli>" --direct-map)
      set_tests_properties(xyo-dlss-offscreen-pbo-direct-pressure PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;direct;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-rgb-direct-pressure
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_pressure_smoke.py"
          "$<TARGET_FILE:xyo-cli>" --rgb --direct-map)
      set_tests_properties(xyo-dlss-offscreen-pbo-rgb-direct-pressure PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;rgb;direct;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-failure-race
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_failure_race_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-failure-race PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;fault-injection;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-gl-fault
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_gl_fault_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-gl-fault PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;fault-injection;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-atomic
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_atomic_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-atomic PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;atomic;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-durable
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_durable_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-durable PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;atomic;durable;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-stale-recovery
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_stale_recovery_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-stale-recovery PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;durable;recovery;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-sigkill-recovery
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_sigkill_recovery_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-sigkill-recovery PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;durable;recovery;sigkill;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-concurrent-recovery
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_concurrent_recovery_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-concurrent-recovery PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;recovery;concurrency;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-series-lock
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_series_lock_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-series-lock PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;lock;concurrency;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-incremental-recovery
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_incremental_recovery_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-incremental-recovery PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;recovery;stress;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-background-recovery
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_background_recovery_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-background-recovery PROPERTIES
        TIMEOUT 240 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;capture;recovery;thread;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-dlss-offscreen-pbo-recovery-batch-sigkill
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_offscreen_pbo_recovery_batch_sigkill_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-offscreen-pbo-recovery-batch-sigkill PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;headless;offscreen;pbo;durable;recovery;sigkill;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      # ASan/UBSan can prevent NVIDIA NGX's Vulkan device-extension
      # interop from initializing. Still require a real SDL OpenGL context,
      # but allow explicit GL fallback only in sanitizer builds. Ordinary
      # release GPU tests continue requiring actual NGX initialization.
      set(_xyo_jit_teardown_sanitizer_args "")
      set(_xyo_jit_teardown_runs 12)
      if(CMAKE_CXX_FLAGS MATCHES "fsanitize=")
        list(APPEND _xyo_jit_teardown_sanitizer_args --allow-dlss-fallback)
        # Software instrumentation makes NGX/GL process startups slower.
        # Preserve the full 36-process test in ordinary native builds.
        set(_xyo_jit_teardown_runs 3)
      endif()
      add_test(NAME xyo-jit-offscreen-teardown
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/jit_destructor_offscreen_repro.py"
          "$<TARGET_FILE:xyo-cli>" --runs ${_xyo_jit_teardown_runs} --frames 16
          ${_xyo_jit_teardown_sanitizer_args})
      set_tests_properties(xyo-jit-offscreen-teardown PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "jit;gpu;dlss;headless;offscreen;teardown;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      add_test(NAME xyo-jit-offscreen-capture-teardown
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/jit_destructor_offscreen_repro.py"
          "$<TARGET_FILE:xyo-cli>" --runs 4 --frames 16 --capture
          ${_xyo_jit_teardown_sanitizer_args})
      set_tests_properties(xyo-jit-offscreen-capture-teardown PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77
        LABELS "jit;gpu;dlss;headless;offscreen;pbo;recovery;teardown;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
      unset(_xyo_jit_teardown_sanitizer_args)
      unset(_xyo_jit_teardown_runs)
      add_test(NAME xyo-dlss-pen-gpu-fp64-transparency
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_pen_gpu_transparency_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-pen-gpu-fp64-transparency PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77
        LABELS "gpu;dlss;jitter;pen;primitive;fp64;alpha;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    add_test(NAME xyo-dlss-raster-jitter-real-project
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_raster_jitter_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-raster-jitter-real-project PROPERTIES
        TIMEOUT 100 LABELS "gpu;dlss;jitter;subpixel;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    add_test(NAME xyo-dlss-optical-jitter-compensation
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-jitter-compensation PROPERTIES
      TIMEOUT 30 LABELS "gpu;dlss;optical-flow;jitter;manual"
      ENVIRONMENT "XYO_OPTICAL_SMOKE_JITTER=1")
    add_test(NAME xyo-dlss-optical-alpha-coverage
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-alpha-coverage PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;optical-flow;alpha;coverage;manual"
      ENVIRONMENT "XYO_OPTICAL_SMOKE_COVERAGE=1")
    if(Python3_Interpreter_FOUND AND TARGET xyo-cli)
      add_test(NAME xyo-dlss-coverage-project-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_coverage_project_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-coverage-project-smoke PROPERTIES
        TIMEOUT 100
        LABELS "gpu;dlss;alpha;coverage;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    add_test(NAME xyo-dlss-optical-affine-override
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-affine-override PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;optical-flow;affine-motion;manual"
      ENVIRONMENT "XYO_OPTICAL_SMOKE_AFFINE=1")
    if(Python3_Interpreter_FOUND AND TARGET xyo-cli)
      add_test(NAME xyo-dlss-affine-project-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_affine_project_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-affine-project-smoke PROPERTIES
        TIMEOUT 180
        LABELS "gpu;dlss;affine-motion;manual"
        ENVIRONMENT "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    add_test(NAME xyo-dlss-optical-rigid-override
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-rigid-override PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;optical-flow;rigid-motion;manual"
      ENVIRONMENT "XYO_OPTICAL_SMOKE_RIGID=1")
    add_test(NAME xyo-dlss-optical-flow-bridge
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-flow-bridge PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;optical-flow;manual")
    add_test(NAME xyo-dlss-optical-flow-scene-cut
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-flow-scene-cut PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;optical-flow;manual"
      ENVIRONMENT "XYO_OPTICAL_SMOKE_CUT=1")
    add_test(NAME xyo-dlss-optical-motion-mask
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-motion-mask PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;optical-flow;motion-mask;manual"
      ENVIRONMENT "XYO_OPTICAL_SMOKE_COST_CLIP=1")
    add_test(NAME xyo-dlss-optical-cut-mask
      COMMAND xyo-dlss-optical-flow-bridge)
    set_tests_properties(xyo-dlss-optical-cut-mask PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;optical-flow;motion-mask;manual"
      ENVIRONMENT "XYO_OPTICAL_SMOKE_COST_CLIP=1;XYO_OPTICAL_SMOKE_CUT=1")
    add_test(NAME xyo-dlss-sdl-optical-motion-mask
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-optical-motion-mask PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;optical-flow;motion-mask;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_SCENE_CUT_SYNC=1;XYO_DLSS_MOTION_COST_CLIP=1;XYO_DLSS_STRESS_NOISE_MOVE=1;XYO_DLSS_STRESS_FRAMES=24")
    add_test(NAME xyo-dlss-sdl-optical-flow-smoke
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-optical-flow-smoke PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;optical-flow;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_STRESS_FRAMES=120")
    add_test(NAME xyo-dlss-sdl-scene-cut
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-scene-cut PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;optical-flow;scene-cut;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_SCENE_CUT_SYNC=1;XYO_DLSS_STRESS_NOISE_CUT=1;XYO_DLSS_STRESS_FRAMES=12")
    add_test(NAME xyo-dlss-sdl-optical-motion
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-optical-motion PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;optical-flow;scene-cut;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_SCENE_CUT_SYNC=1;XYO_DLSS_STRESS_NOISE_MOVE=1;XYO_DLSS_STRESS_FRAMES=12")
    if(Python3_Interpreter_FOUND)
      add_test(NAME xyo-dlss-sdl-alpha-quality-comparison
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_alpha_safety_compare.py"
          "$<TARGET_FILE:xyo-dlss-sdl-color-smoke>")
      set_tests_properties(xyo-dlss-sdl-alpha-quality-comparison PROPERTIES
        TIMEOUT 95
        LABELS "gpu;dlss;alpha-occlusion;temporal-quality;manual"
        ENVIRONMENT
        "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    if(Python3_Interpreter_FOUND AND TARGET xyo-cli)
      add_test(NAME xyo-dlss-sdl-costume-alpha-detection
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_alpha_costume_smoke.py"
          "$<TARGET_FILE:xyo-cli>")
      set_tests_properties(xyo-dlss-sdl-costume-alpha-detection PROPERTIES
        TIMEOUT 130
        LABELS "gpu;dlss;alpha;costume;manual"
        ENVIRONMENT
        "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    # Moving silhouette compares temporal history vs a spatial baseline,
    # printing measured old-object trails rather than asserting one is better.
    if(Python3_Interpreter_FOUND)
      add_test(NAME xyo-dlss-sdl-alpha-bias-comparison
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/render/dlss_bias_quality_compare.py"
          "$<TARGET_FILE:xyo-dlss-sdl-color-smoke>")
      set_tests_properties(xyo-dlss-sdl-alpha-bias-comparison PROPERTIES
        TIMEOUT 180
        LABELS "gpu;dlss;alpha;bias-mask;temporal-quality;manual"
        ENVIRONMENT
        "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel")
    endif()
    add_test(NAME xyo-dlss-sdl-alpha-bias-smoke
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-alpha-bias-smoke PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;alpha;bias-mask;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_ALPHA_BIAS=1;XYO_DLSS_STRESS_ALPHA_BIAS=1;XYO_DLSS_STRESS_ALPHA_OCCLUSION=1;XYO_DLSS_STRESS_FRAMES=90")
    add_test(NAME xyo-dlss-sdl-alpha-occlusion-history
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-alpha-occlusion-history PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;alpha-occlusion;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_SCENE_CUT_SYNC=1;XYO_DLSS_STRESS_ALPHA_OCCLUSION=1;XYO_DLSS_STRESS_FRAMES=90")
    add_test(NAME xyo-dlss-sdl-alpha-occlusion-safe
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-alpha-occlusion-safe PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;alpha-occlusion;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_SCENE_CUT_SYNC=1;XYO_DLSS_STRESS_ALPHA_OCCLUSION=1;XYO_DLSS_STRESS_ALPHA_SAFE=1;XYO_DLSS_STRESS_FRAMES=90")
    add_test(NAME xyo-dlss-sdl-moving-square-spatial
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-moving-square-spatial PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;temporal-quality;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=0;XYO_DLSS_TEMPORAL=0;XYO_DLSS_MODE=performance;XYO_DLSS_PRESET=M;XYO_DLSS_STRESS_SQUARE_MOVE=1;XYO_DLSS_STRESS_FRAMES=30")
    add_test(NAME xyo-dlss-sdl-moving-square-temporal
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-moving-square-temporal PROPERTIES
      TIMEOUT 45
      LABELS "gpu;dlss;temporal-quality;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_OPTICAL_FLOW=1;XYO_DLSS_TEMPORAL=1;XYO_DLSS_SCENE_CUT_SYNC=1;XYO_DLSS_MODE=performance;XYO_DLSS_PRESET=M;XYO_DLSS_STRESS_SQUARE_MOVE=1;XYO_DLSS_STRESS_FRAMES=30")
    add_test(NAME xyo-dlss-sdl-color-smoke
      COMMAND xyo-dlss-sdl-color-smoke)
    set_tests_properties(xyo-dlss-sdl-color-smoke PROPERTIES
      TIMEOUT 30
      LABELS "gpu;dlss;manual"
      ENVIRONMENT
      "XYO_DLSS_FEATURE_DIR=${XYO_NVSDK_DLSS_SDK_ROOT}/lib/Linux_x86_64/rel;XYO_DLSS_STRESS_FRAMES=120")
  endif()
endif()

file(GLOB GPU_TEST_SOURCES CONFIGURE_DEPENDS gpu/*.cpp)
# test_parallel.cpp は -fno-rtti の SdlRenderer をサブクラス化するため
# 同じフラグが必要（vtable が基底の typeinfo を参照しないように）。
set_source_files_properties(gpu/test_parallel.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-rtti;-fno-exceptions")
if(GPU_TEST_SOURCES AND SDL2_FOUND)
  add_executable(xyo-gpu-tests ${GPU_TEST_SOURCES})
  target_link_libraries(xyo-gpu-tests PRIVATE xyo)
  add_test(NAME xyo-gpu-tests COMMAND xyo-gpu-tests)
  set_tests_properties(xyo-gpu-tests PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XYO_JIT_MODE=eager")
endif()

# ペン描画ベンチマーク（テストではなく計測用。ctest には登録しない）
add_executable(xyo-par-bench bench/par_bench.cpp)
target_compile_options(xyo-par-bench PRIVATE -fno-rtti -fno-exceptions)
target_link_libraries(xyo-par-bench PRIVATE xyo)
if(SDL2_FOUND)
  add_executable(xyo-pen-bench bench/pen_bench.cpp)
  # Project timing subclasses the runtime's no-RTTI renderer.
  target_compile_options(xyo-pen-bench PRIVATE -fno-rtti -fno-exceptions)
  target_link_libraries(xyo-pen-bench PRIVATE xyo)
endif()

# Standalone WASM output tests. Binaryen is optional; Node executes both.
if(TARGET xyo-cli)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  find_program(XYO_WASM_NODE node)
  find_program(XYO_WASM_OPT_PROGRAM wasm-opt
    HINTS "$ENV{HOME}/.emsdk/upstream/bin")
  if(Python3_Interpreter_FOUND AND XYO_WASM_NODE)
    add_test(NAME xyo-wasm-binaryen-backend
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/wasm_binaryen_backend.py"
              "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(xyo-wasm-binaryen-backend PROPERTIES TIMEOUT 120)
    if(XYO_WASM_OPT_PROGRAM)
      set_property(TEST xyo-wasm-binaryen-backend APPEND PROPERTY
        ENVIRONMENT "XYO_WASM_OPT=${XYO_WASM_OPT_PROGRAM}")
    endif()
    if(XYO_HAVE_BINARYEN_C)
      set_property(TEST xyo-wasm-binaryen-backend APPEND PROPERTY
        ENVIRONMENT "XYO_TEST_BINARYEN_NATIVE=1")
    endif()
  endif()
endif()

# Non-contiguous experimental stores cannot be tested by older xyo-tests
# cases that read private SList::items directly.
add_executable(xyo-list-backend-smoke
  bench/list_backend_smoke.cpp unit/test_list_limit.cpp)
target_link_libraries(xyo-list-backend-smoke PRIVATE xyo)
add_test(NAME xyo-list-backend-flat
  COMMAND ${CMAKE_COMMAND} -E env XYO_LIST_STORAGE=flat
          $<TARGET_FILE:xyo-list-backend-smoke>)
add_test(NAME xyo-list-backend-deque
  COMMAND ${CMAKE_COMMAND} -E env XYO_LIST_STORAGE=deque
          $<TARGET_FILE:xyo-list-backend-smoke>)
add_test(NAME xyo-list-backend-gap
  COMMAND ${CMAKE_COMMAND} -E env XYO_LIST_STORAGE=gap
          $<TARGET_FILE:xyo-list-backend-smoke>)
add_test(NAME xyo-list-backend-chunked
  COMMAND ${CMAKE_COMMAND} -E env XYO_LIST_STORAGE=chunked
          $<TARGET_FILE:xyo-list-backend-smoke>)
add_test(NAME xyo-list-backend-adaptive
  COMMAND ${CMAKE_COMMAND} -E env XYO_LIST_STORAGE=adaptive
          $<TARGET_FILE:xyo-list-backend-smoke>)

# Epoch-guarded adaptive JIT list read parity (yield, GC, resize, migration).
add_executable(xyo-list-epoch-smoke
  bench/list_epoch_smoke.cpp unit/test_list_read_memo.cpp)
target_link_libraries(xyo-list-epoch-smoke PRIVATE xyo)
add_test(NAME xyo-list-epoch-jit
  COMMAND ${CMAKE_COMMAND} -E env
          XYO_LIST_STORAGE=adaptive XYO_JIT_MODE=eager
          $<TARGET_FILE:xyo-list-epoch-smoke>)
set_tests_properties(xyo-list-epoch-jit PROPERTIES TIMEOUT 180)

# Focused microbenchmark for adaptive storage migration; not a ctest.
add_executable(xyo-list-migration-bench bench/list_migration_bench.cpp)
target_link_libraries(xyo-list-migration-bench PRIVATE xyo)


# Source edits must preserve cached operations referenced by live JIT
# Fibers and Interpreter StackFrames, including their GC-owned Values.
add_executable(xyo-blocks-cache-retirement-smoke
  platform/blocks_cache_retirement_smoke.cpp)
target_link_libraries(xyo-blocks-cache-retirement-smoke PRIVATE xyo)
add_test(NAME xyo-blocks-cache-retirement-smoke
  COMMAND xyo-blocks-cache-retirement-smoke)

# IR-001: typed, detached SSA descriptors independent of the runtime/LLVM.
# Includes exact float NaN bits, argument kinds and source/site binding.
add_executable(xyo-ir001-typed-ssa-manifest-smoke
  platform/typed_ssa_manifest_smoke.cpp)
target_compile_options(xyo-ir001-typed-ssa-manifest-smoke
  PRIVATE -fno-exceptions -fno-rtti)
add_test(NAME xyo-ir001-typed-ssa-manifest-smoke
  COMMAND xyo-ir001-typed-ssa-manifest-smoke)

# IR-001: verify RT-owned generated SSA string roots without creating
# roots from arbitrary detached LLVM words. Uses a real GC collection.
if(NOT EMSCRIPTEN)
  add_executable(xyo-ir001-ssa-gc-audit-smoke
    platform/source_ssa_gc_audit_smoke.cpp)
  target_link_libraries(xyo-ir001-ssa-gc-audit-smoke PRIVATE xyo)
  target_compile_options(xyo-ir001-ssa-gc-audit-smoke
    PRIVATE -fno-exceptions -fno-rtti -Wno-error=unused-parameter)
  add_test(NAME xyo-ir001-ssa-gc-audit-smoke
    COMMAND xyo-ir001-ssa-gc-audit-smoke)

  # IR-001: compiler-origin SSA argument aliases, native generated
  # string temporaries, and independent rooted source CallFrame.
  # Diagnostic only: no source-local mapping or executable deopt.
  add_executable(xyo-ir001-ssa-location-bridge-smoke
    platform/source_ssa_location_bridge_smoke.cpp)
  target_link_libraries(xyo-ir001-ssa-location-bridge-smoke PRIVATE xyo)
  target_compile_options(xyo-ir001-ssa-location-bridge-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-ir001-ssa-location-bridge-smoke
    COMMAND xyo-ir001-ssa-location-bridge-smoke)

  # IR-001: actual LLVM live instruction identity to frozen source
  # numeric slot IDs; source variables and GC state remain unproven.
  add_executable(xyo-ir001-source-numeric-coverage-smoke
    platform/source_numeric_resume_coverage_smoke.cpp)
  target_link_libraries(xyo-ir001-source-numeric-coverage-smoke
    PRIVATE xyo)
  target_compile_options(xyo-ir001-source-numeric-coverage-smoke
    PRIVATE -fno-exceptions -fno-rtti -Wno-error=unused-parameter)
  add_test(NAME xyo-ir001-source-numeric-coverage-smoke
    COMMAND xyo-ir001-source-numeric-coverage-smoke)
endif()

# IR-001: fail-closed source effect frontier bookkeeping and replay guards.
add_executable(xyo-ir001-effect-frontier-smoke
  platform/source_effect_frontier_smoke.cpp)
target_compile_options(xyo-ir001-effect-frontier-smoke
  PRIVATE -fno-exceptions -fno-rtti)
add_test(NAME xyo-ir001-effect-frontier-smoke
  COMMAND xyo-ir001-effect-frontier-smoke)

# IR-001: real scheduler-owned effect journal -> observed source frontier.
# Link against xyo for the GC used by CommitOnceEffects; native only.
add_executable(xyo-ir001-effect-journal-bridge-smoke
  platform/source_effect_journal_bridge_smoke.cpp)
target_link_libraries(xyo-ir001-effect-journal-bridge-smoke PRIVATE xyo)
target_compile_options(xyo-ir001-effect-journal-bridge-smoke
  PRIVATE -fno-exceptions -fno-rtti)
if(NOT EMSCRIPTEN)
  add_test(NAME xyo-ir001-effect-journal-bridge-smoke
    COMMAND xyo-ir001-effect-journal-bridge-smoke)
endif()

# IR-001: source-PC traces from real compiled broadcast/pen effects.
# The instrumented path is generated only with the test's opt-in flag.
if(TARGET xyo-cli AND Python3_Interpreter_FOUND AND NOT EMSCRIPTEN)
  add_test(NAME xyo-ir001-live-effect-source-broadcast
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/ir001_effect_source_trace.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/ir001-live-effects")
  add_test(NAME xyo-ir001-live-effect-source-pen
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/ir001_pen_effect_source_trace.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/ir001-live-effects")
  set_tests_properties(
    xyo-ir001-live-effect-source-broadcast
    xyo-ir001-live-effect-source-pen
    PROPERTIES TIMEOUT 180)
endif()

# IR-001: inspect real optimized LLVM stores/calls in CFGs. Diagnostic
# only; applies the same O2 pass pipeline as the JIT in unit tests.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-ir001-postopt-write-audit-smoke
    platform/llvm_effect_write_audit_smoke.cpp)
  target_include_directories(xyo-ir001-postopt-write-audit-smoke
    PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-ir001-postopt-write-audit-smoke
    PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  target_compile_options(xyo-ir001-postopt-write-audit-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-ir001-postopt-write-audit-smoke
    COMMAND xyo-ir001-postopt-write-audit-smoke)
endif()

# IR-001: real O2/O3 native JIT fail-closed write auditing, only when
# the CLI and LLVM backend are available. Does not authorize deopt.
if(TARGET xyo-cli AND Python3_Interpreter_FOUND AND XYO_JIT AND
   XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  # No SDL/NGX dependency: verify both ORC engines with >2 GiB-safe
  # JIT code model in a real 148-assertion Scratch CPU fixture.
  add_test(NAME xyo-jit-large-code-model-lazy
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_large_code_model_lazy.py"
      "$<TARGET_FILE:xyo-cli>")
  set_tests_properties(xyo-jit-large-code-model-lazy PROPERTIES
    TIMEOUT 180
    LABELS "jit;llvm;lazy;code-model;manual")
  add_test(NAME xyo-ir001-postopt-write-audit-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/ir001_postopt_write_audit.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/ir001-postopt-write-audit")
  set_tests_properties(xyo-ir001-postopt-write-audit-parity
    PROPERTIES TIMEOUT 180)
endif()

# IR-001: full native Scratch interpreter dispatch of all seven
# variable/list mutation opcodes; diagnostics must not change results.
if(TARGET xyo-cli AND Python3_Interpreter_FOUND AND NOT EMSCRIPTEN)
  add_test(NAME xyo-ir001-interpreter-mutation-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/ir001_interpreter_mutation_trace.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/ir001-interpreter-mutations")
  set_tests_properties(xyo-ir001-interpreter-mutation-parity
    PROPERTIES TIMEOUT 180)

  # HotSpot compiles the loop sender while the cold broadcast receiver
  # remains interpreted. An explicit yield makes the receiver's write
  # happen BEFORE the source loop checkpoint, which must be denied.
  add_test(NAME xyo-ir001-hotspot-interpreter-interleaving
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/ir001_hotspot_interpreter_interleaving.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/ir001-hotspot-interleaving")
  set_tests_properties(xyo-ir001-hotspot-interpreter-interleaving
    PROPERTIES TIMEOUT 180)

  # Production LLVM emitInputOf() preserves a frozen source reporter
  # ID/PC -> actual LLVM Value* trace across normal inlining. A boxed
  # Scratch i64 is NEVER called a raw numeric/GC-root proof.
  add_test(NAME xyo-ir001-source-reporter-origin-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/ir001_source_reporter_origin.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/ir001-source-reporter-origin")
  set_tests_properties(xyo-ir001-source-reporter-origin-parity
    PROPERTIES TIMEOUT 180)
endif()

# IR-001: exact source-event label attribution, nested scopes, and
# scheduler-owned, fail-closed diagnostic checkpoint (no host linkage).
add_executable(xyo-ir001-effect-dispatch-smoke
  platform/source_effect_dispatch_smoke.cpp)
target_compile_options(xyo-ir001-effect-dispatch-smoke
  PRIVATE -fno-exceptions -fno-rtti)
add_test(NAME xyo-ir001-effect-dispatch-smoke
  COMMAND xyo-ir001-effect-dispatch-smoke)

# IR-001: the REAL collector advances the atomic generation, and an
# actual worker Thread invalidates another Thread's source record.
# Neither test enables native source-level deoptimization.
if(NOT EMSCRIPTEN)
  find_package(Threads REQUIRED)
  add_executable(xyo-ir001-gc-cross-thread-smoke
    platform/source_effect_gc_epoch_smoke.cpp)
  target_link_libraries(xyo-ir001-gc-cross-thread-smoke
    PRIVATE xyo Threads::Threads)
  target_compile_options(xyo-ir001-gc-cross-thread-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-ir001-gc-cross-thread-smoke
    COMMAND xyo-ir001-gc-cross-thread-smoke)
endif()


# RT-001: live JIT-generated boxed SStr SSA is rooted only after
# independent compiler-producer and exact GC membership verification.
# Test the ROOT OWNERSHIP itself across a real full collection, including
# malformed pointers, rehashed SSA values and missing handles.
if(NOT EMSCRIPTEN)
  add_executable(xyo-rt001-generated-ssa-roots-smoke
    platform/rt_generated_ssa_roots_smoke.cpp)
  target_link_libraries(xyo-rt001-generated-ssa-roots-smoke PRIVATE xyo)
  add_test(NAME xyo-rt001-generated-ssa-roots-smoke
    COMMAND xyo-rt001-generated-ssa-roots-smoke)
endif()


# RT-001: only an exact LLVM i64 join-helper return (or every arm of a
# PHI/SELECT from such returns) certifies a newly produced boxed SStr.
# Name-only ABI forgery, mixed sources, arithmetic and deep SSA reject.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-generated-producer-smoke
    platform/rt_ssa_generated_producer_smoke.cpp)
  target_link_libraries(xyo-rt001-generated-producer-smoke PRIVATE xyo)
  target_compile_options(xyo-rt001-generated-producer-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-generated-producer-smoke
    COMMAND xyo-rt001-generated-producer-smoke)
endif()


# RT-001: classify live async-reporter i64 words separately from
# pointer SSA (argument aliases, host-created strings, unknowns).
# Even zero unclassified words never authorizes suspended-Fiber GC.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-ssa-i64-provenance-smoke
    platform/rt_ssa_rooted_alias_smoke.cpp)
  target_link_libraries(xyo-rt001-ssa-i64-provenance-smoke PRIVATE xyo)
  target_compile_options(xyo-rt001-ssa-i64-provenance-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-ssa-i64-provenance-smoke
    COMMAND xyo-rt001-ssa-i64-provenance-smoke)
endif()

# RT-001: exact native Variable::value LLVM i64 load origin.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-varvalue-origin-smoke
    platform/rt_ssa_varvalue_load_smoke.cpp)
  target_link_libraries(xyo-rt001-varvalue-origin-smoke PRIVATE xyo)
  target_compile_options(xyo-rt001-varvalue-origin-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-varvalue-origin-smoke
    COMMAND xyo-rt001-varvalue-origin-smoke)
endif()

# RT-001: malformed pre-wait heap-pointer words fail closed before
# an abortable Fiber can bypass its native C++ local destructors.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-async-root-invalid-smoke
    platform/rt_async_variable_root_invalid_smoke.cpp)
  target_link_libraries(xyo-rt001-async-root-invalid-smoke PRIVATE xyo)
  target_compile_options(xyo-rt001-async-root-invalid-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-async-root-invalid-smoke
    COMMAND xyo-rt001-async-root-invalid-smoke)
endif()

# RT-001: recognize exactly typed native async reporter LLVM i64
# returns, not arbitrary calls or forged function signatures.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-async-reporter-origin-smoke
    platform/rt_ssa_async_reporter_origin_smoke.cpp)
  target_link_libraries(xyo-rt001-async-reporter-origin-smoke PRIVATE xyo)
  target_compile_options(xyo-rt001-async-reporter-origin-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-async-reporter-origin-smoke
    COMMAND xyo-rt001-async-reporter-origin-smoke)
endif()

# RT-001: compiler-only LLVM live pointer address-region census.
# Native stack / host context / static globals are distinct from
# untrusted loaded/call/IntToPtr references; no GC permission is added.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-pointer-region-smoke
    platform/rt_pointer_region_smoke.cpp)
  target_include_directories(xyo-rt001-pointer-region-smoke
    PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-rt001-pointer-region-smoke
    PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  target_compile_options(xyo-rt001-pointer-region-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-pointer-region-smoke
    COMMAND xyo-rt001-pointer-region-smoke)
endif()

# RT-001: O3 native async translator root buffer coverage.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-postopt-async-roots-smoke
    platform/rt_postopt_async_roots_smoke.cpp)
  target_include_directories(xyo-rt001-postopt-async-roots-smoke
    PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-rt001-postopt-async-roots-smoke
    PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  target_compile_options(xyo-rt001-postopt-async-roots-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-postopt-async-roots-smoke
    COMMAND xyo-rt001-postopt-async-roots-smoke)
endif()

# RT-001: real paused native Fiber's rooted translator caller return.
# Verify the ABI frame and match its JIT PC with an ELF call relocation.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND
   CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND
   NOT EMSCRIPTEN)
  add_executable(xyo-rt001-native-async-paused-callsite-smoke
    platform/rt_native_async_paused_callsite_smoke.cpp)
  target_link_libraries(xyo-rt001-native-async-paused-callsite-smoke
    PRIVATE xyo ${CMAKE_DL_LIBS})
  target_compile_options(xyo-rt001-native-async-paused-callsite-smoke
    PRIVATE -fno-exceptions -fno-rtti)
endif()

# RT-001: the real LLVMBackend diagnostic ORC FDE plugin.
# Separate build and runtime opt-ins; never permits native-Fiber GC.
if(XYO_RT001_ORC_FDE_PLUGIN AND XYO_BUILD_SHELL AND
   TARGET xyo-cli AND Python3_EXECUTABLE)
  add_test(NAME xyo-rt001-orc-fde-runtime-plugin-eager-lazy
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_orc_fde_runtime_plugin.py"
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-orc-fde-runtime-plugin")
  add_test(NAME xyo-rt001-orc-fde-runtime-plugin-stackmap-eager-lazy
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_orc_fde_runtime_plugin.py"
      --stackmap
      --native "$<TARGET_FILE:xyo-cli>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-orc-fde-runtime-stackmap")
endif()

# RT-001: inspect optimized LLVM IR (not the pre-O3 source model).
# The standalone smoke proves live unrooted pointers survive O3 and
# forged native capture ABI declarations are rejected. This NEVER
# authorizes a native Fiber GC sweep.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-postopt-ssa-audit-smoke
    platform/rt_postopt_ssa_audit_smoke.cpp)
  target_include_directories(xyo-rt001-postopt-ssa-audit-smoke
    PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-rt001-postopt-ssa-audit-smoke
    PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  target_compile_options(xyo-rt001-postopt-ssa-audit-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-postopt-ssa-audit-smoke
    COMMAND xyo-rt001-postopt-ssa-audit-smoke)
  # Actual pre-yield call edge machine-position diagnostic,
  # explicitly distinct from GC root liveness/safepoint proof.
  add_executable(xyo-rt001-yield-edge-stackmap-smoke
    platform/rt_yield_edge_stackmap_smoke.cpp)
  target_include_directories(xyo-rt001-yield-edge-stackmap-smoke
    PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-rt001-yield-edge-stackmap-smoke
    PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  target_compile_options(xyo-rt001-yield-edge-stackmap-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-yield-edge-stackmap-smoke
    COMMAND xyo-rt001-yield-edge-stackmap-smoke)
  # Post-O3 native yield: actual live i64 + pointer location
  # census in a SECOND machine StackMap; no boxed-Value GC authority.
  add_executable(xyo-rt001-yield-ssa-stackmap-smoke
    platform/rt_yield_ssa_machine_stackmap_smoke.cpp)
  target_include_directories(xyo-rt001-yield-ssa-stackmap-smoke
    PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-rt001-yield-ssa-stackmap-smoke
    PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  target_compile_options(xyo-rt001-yield-ssa-stackmap-smoke
    PRIVATE -fno-exceptions -fno-rtti -UNDEBUG)
  add_test(NAME xyo-rt001-yield-ssa-stackmap-smoke
    COMMAND xyo-rt001-yield-ssa-stackmap-smoke)
  if(Python3_EXECUTABLE)
    # Codegen's source-of-truth may-yield catalog must not silently
    # drift from the fail-closed, unmapped JIT helper census.
    add_test(NAME xyo-rt001-yield-helper-catalog-smoke
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_helper_catalog.py")
    # Negative tests must refuse wrong/missing callee-saved locations,
    # forged JitCtx values, synthetic FDEs and erroneous GC permission.
    add_test(NAME xyo-rt001-yield-register-contract-smoke
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_register_contract.py")
    add_test(NAME xyo-rt001-yield-ssa-spill-contract-smoke
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_ssa_spill_contract.py")
  endif()
  # Machine-code records are validated against the compiler's
  # exact post-O3 source-site IDs and llvm-readobj --stackmap locations.
  find_program(XYO_RT001_LLVM_READOBJ
    NAMES llvm-readobj llvm-readobj-23 llvm-readobj-22)
  find_program(XYO_RT001_LLVM_OBJDUMP
    NAMES llvm-objdump llvm-objdump-23 llvm-objdump-22)
  # The current location-count oracle was validated for Linux
  # x86-64 ELF. Keep unvalidated Mach-O/COFF backends opt-out.
  if(Python3_EXECUTABLE AND XYO_RT001_LLVM_READOBJ AND
     TARGET xyo-cli AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    # RT-001: validate actual pre-translation reporter machine
    # locations (Eager + ORC Lazy) independently of source SSA maps.
    # The records are pre-call diagnostics, never paused-Fiber GC roots.
    if(XYO_RT001_LLVM_OBJDUMP)
      add_test(NAME xyo-rt001-async-machine-stackmap-eager-lazy-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_async_machine_stackmap.py"
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-async-machine-stackmap")
      set_tests_properties(
        xyo-rt001-async-machine-stackmap-eager-lazy-smoke
        PROPERTIES TIMEOUT 35)
      # When per-access native slot guarding is enabled, O3 must NOT
      # retain borrowed Variable* across an async translator call.
      # This is a liveness safety regression, NOT a GC certificate.
      add_test(NAME xyo-rt001-async-machine-stackmap-guarded-eager-lazy-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_async_machine_stackmap.py"
          --guarded
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-async-machine-stackmap-guarded")
      set_tests_properties(
        xyo-rt001-async-machine-stackmap-guarded-eager-lazy-smoke
        PROPERTIES TIMEOUT 35)
      add_test(NAME xyo-rt001-async-machine-stackmap-indexed-eager-lazy-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_async_machine_stackmap.py"
          --guarded --indexed
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-async-machine-stackmap-indexed")
      set_tests_properties(
        xyo-rt001-async-machine-stackmap-indexed-eager-lazy-smoke
        PROPERTIES TIMEOUT 35)
      # The source-level async reporter census must suppress inline
      # PHI roots (which are not yet supported by GC root audit).
      add_test(NAME xyo-rt001-async-machine-stackmap-inline-fallback-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_async_machine_stackmap.py"
          --guarded --indexed
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-async-inline-fallback")
      set_tests_properties(
        xyo-rt001-async-machine-stackmap-inline-fallback-smoke PROPERTIES
        ENVIRONMENT "XYO_JIT_VAR_GUARD_INLINE=1" TIMEOUT 35)
    endif()
    # This test needs the real libunwind cursor; omit it if the
    # optional unwind libraries are absent on this platform.
    find_library(XYO_RT001_UNWIND_LIBRARY NAMES unwind)
    find_library(XYO_RT001_UNWIND_X86_64_LIBRARY NAMES unwind-x86_64)
    find_program(XYO_RT001_LLVM_DWARFDUMP
      NAMES llvm-dwarfdump llvm-dwarfdump-23 llvm-dwarfdump-22)
    if(TARGET xyo-rt001-native-async-paused-callsite-smoke AND
       XYO_RT001_LLVM_OBJDUMP AND XYO_RT001_LLVM_DWARFDUMP AND
       XYO_RT001_UNWIND_LIBRARY AND
       XYO_RT001_UNWIND_X86_64_LIBRARY)
      add_test(NAME xyo-rt001-async-paused-caller-elf-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_async_paused_callsite_elf.py"
          --smoke "$<TARGET_FILE:xyo-rt001-native-async-paused-callsite-smoke>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --dwarfdump "${XYO_RT001_LLVM_DWARFDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-async-paused-caller")
      set_tests_properties(xyo-rt001-async-paused-caller-elf-smoke
        PROPERTIES TIMEOUT 35)
      if(XYO_RT001_ORC_FDE_PLUGIN)
        # Real ORC-owned FDEs, no temporary dynamic registration:
        # verify live JIT CFI while the Fiber is parked, and removal
        # after Runtime's ResourceTrackers have released native code.
        add_test(NAME xyo-rt001-async-paused-caller-orc-lifecycle-smoke
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_async_paused_callsite_elf.py"
            --orc-plugin
            --smoke "$<TARGET_FILE:xyo-rt001-native-async-paused-callsite-smoke>"
            --readobj "${XYO_RT001_LLVM_READOBJ}"
            --objdump "${XYO_RT001_LLVM_OBJDUMP}"
            --dwarfdump "${XYO_RT001_LLVM_DWARFDUMP}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-async-paused-orc-fde")
        set_tests_properties(xyo-rt001-async-paused-caller-orc-lifecycle-smoke
          PROPERTIES TIMEOUT 45)
        # Two independent LLVMBackend/ORC engines in ONE host process.
        # Confirms every FDE is removed before the next Runtime may
        # reuse the old executable memory address.
        add_test(NAME xyo-rt001-orc-fde-two-runtimes-smoke
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_orc_fde_two_runtimes.py"
            --smoke "$<TARGET_FILE:xyo-rt001-native-async-paused-callsite-smoke>"
            --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-orc-fde-two-runtimes")
        set_tests_properties(xyo-rt001-orc-fde-two-runtimes-smoke
          PROPERTIES TIMEOUT 45)
      endif()
    endif()
    add_test(NAME xyo-rt001-machine-stackmap-eager-lazy-smoke
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_machine_stackmap_probe.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --readobj "${XYO_RT001_LLVM_READOBJ}"
        --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-machine-stackmap")
    # Emit a location map immediately before each direct native
    # yield call, and match the generated x86-64 CALL relocations.
    # This proves call-edge geometry only, NOT GC root ownership.
    if(XYO_RT001_LLVM_OBJDUMP)
      add_test(NAME xyo-rt001-yield-edge-eager-lazy-machine-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_edge_machine_probe.py"
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-edge-machine")
      set_tests_properties(xyo-rt001-yield-edge-eager-lazy-machine-smoke
        PROPERTIES TIMEOUT 60)
      # Known regression: combining all live i64+pointer machine
      # operands corrupts real Unicode Scratch state. Require a
      # fail-closed refusal and separately verify i64 and pointer
      # ELF locations under Eager/Lazy, with runtime parity.
      add_test(NAME xyo-rt001-yield-ssa-machine-separated-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_live_ssa_machine.py"
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-ssa-machine")
      set_tests_properties(xyo-rt001-yield-ssa-machine-separated-smoke
        PROPERTIES TIMEOUT 120)
      # Compiler provenance evidence for EVERY emitted real i64
      # StackMap machine location. Candidate != GC-owned; all unknown
      # values remain untrusted. Reject altered/missing witness records.
      add_test(NAME xyo-rt001-yield-i64-origin-census-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_i64_origin_census.py"
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-i64-origins")
      set_tests_properties(xyo-rt001-yield-i64-origin-census-smoke
        PROPERTIES TIMEOUT 120)
      # Only the proven internal +1 JitCtx.yieldCheckN counter is
      # removed from one experimental SSA location map. The compiler
      # must retain all other i64 and still prohibit GC.
      add_test(NAME xyo-rt001-yield-tick-exclusion-machine-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_tick_exclusion.py"
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-tick-exclusion")
      set_tests_properties(xyo-rt001-yield-tick-exclusion-machine-smoke
        PROPERTIES TIMEOUT 120)
      # Exercise the real fast Fiber* ABI, not just xyo_yield(JitCtx*).
      add_test(NAME xyo-rt001-yield-edge-fast-eager-lazy-machine-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_edge_machine_probe.py"
          --fast
          --native "$<TARGET_FILE:xyo-cli>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-edge-machine-fast")
      set_tests_properties(
        xyo-rt001-yield-edge-fast-eager-lazy-machine-smoke
        PROPERTIES TIMEOUT 60)
      # Real Scratch wait-until can suspend INSIDE a helper.
      # Explicitly refuse a direct machine root for that call.
      add_test(NAME xyo-rt001-yield-wait-unmapped-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_wait_unmapped.py"
          --native "$<TARGET_FILE:xyo-cli>"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-wait-unmapped")
      set_tests_properties(xyo-rt001-yield-wait-unmapped-smoke
        PROPERTIES TIMEOUT 90)
    endif()
    # The Fiber's saved RIP is the host xyo_fiber_switch return, not
    # the StackMap instruction PC. This real two-hat JIT test checks
    # that fact and only reports bounded stack return candidates.
    # It NEVER upgrades a candidate to a verified GC frame/root.
    add_executable(xyo-rt001-native-fiber-pc-stackmap-smoke
      platform/rt_native_fiber_pc_stackmap_smoke.cpp)
    target_link_libraries(xyo-rt001-native-fiber-pc-stackmap-smoke
      PRIVATE xyo ${CMAKE_DL_LIBS})
    target_compile_options(xyo-rt001-native-fiber-pc-stackmap-smoke
      PRIVATE -fno-exceptions -fno-rtti)
    if(XYO_RT001_LLVM_OBJDUMP)
      add_test(NAME xyo-rt001-native-fiber-pc-stackmap-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_fiber_pc_stackmap.py"
          --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-pc-stackmap-smoke>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-fiber-pc-stackmap")
    endif()
    # RT-001: synthetic libunwind ucontext initialized from saved
    # FiberRegs. A one-byte fallback proc range is NOT a real JIT FDE.
    find_program(XYO_RT001_LLVM_DWARFDUMP
      NAMES llvm-dwarfdump llvm-dwarfdump-23 llvm-dwarfdump-22)
    find_library(XYO_RT001_UNWIND_LIBRARY NAMES unwind)
    find_library(XYO_RT001_UNWIND_X86_64_LIBRARY
      NAMES unwind-x86_64)
    if(TARGET xyo-rt001-native-async-paused-callsite-smoke AND
       XYO_RT001_UNWIND_LIBRARY AND
       XYO_RT001_UNWIND_X86_64_LIBRARY)
      target_link_libraries(xyo-rt001-native-async-paused-callsite-smoke
        PRIVATE ${XYO_RT001_UNWIND_LIBRARY}
          ${XYO_RT001_UNWIND_X86_64_LIBRARY})
      target_compile_definitions(
        xyo-rt001-native-async-paused-callsite-smoke
        PRIVATE XYO_RT001_HAVE_LIBUNWIND=1)
    endif()
    if(XYO_RT001_LLVM_OBJDUMP AND
       XYO_RT001_LLVM_DWARFDUMP AND
       XYO_RT001_UNWIND_LIBRARY AND
       XYO_RT001_UNWIND_X86_64_LIBRARY)
      add_executable(xyo-rt001-native-fiber-cfi-unwind-smoke
        platform/rt_native_fiber_cfi_unwind_smoke.cpp)
      target_link_libraries(xyo-rt001-native-fiber-cfi-unwind-smoke
        PRIVATE xyo ${CMAKE_DL_LIBS}
          ${XYO_RT001_UNWIND_LIBRARY}
          ${XYO_RT001_UNWIND_X86_64_LIBRARY})
      target_compile_options(xyo-rt001-native-fiber-cfi-unwind-smoke
        PRIVATE -fno-exceptions -fno-rtti)
      add_test(NAME xyo-rt001-native-fiber-cfi-unwind-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_fiber_cfi_unwind.py"
          --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-cfi-unwind-smoke>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --dwarfdump "${XYO_RT001_LLVM_DWARFDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-fiber-cfi-unwind")
      # Verify the unwound REAL paused JIT return PC is the
      # x86-64 return of the same call that immediately follows the
      # yield-edge StackMap. This still does not permit GC.
      add_test(NAME xyo-rt001-yield-edge-paused-fiber-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_edge_fiber_pc.py"
          --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-cfi-unwind-smoke>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-edge-paused")
      set_tests_properties(xyo-rt001-yield-edge-paused-fiber-smoke
        PROPERTIES TIMEOUT 60)
      if(XYO_RT001_ORC_FDE_PLUGIN)
        add_test(NAME xyo-rt001-yield-edge-paused-fiber-dwarf-smoke
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_edge_fiber_pc.py"
            --require-fde
            --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-cfi-unwind-smoke>"
            --readobj "${XYO_RT001_LLVM_READOBJ}"
            --objdump "${XYO_RT001_LLVM_OBJDUMP}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-edge-dwarf")
        set_tests_properties(
          xyo-rt001-yield-edge-paused-fiber-dwarf-smoke
          PROPERTIES TIMEOUT 60)
        # Real JIT DWARF FDE + StackMap location register + the
        # *same* live Thread::jitCtx value (not GC root completeness).
        add_test(NAME xyo-rt001-yield-edge-paused-register-smoke
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_edge_fiber_pc.py"
            --require-fde --require-register
            --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-cfi-unwind-smoke>"
            --readobj "${XYO_RT001_LLVM_READOBJ}"
            --objdump "${XYO_RT001_LLVM_OBJDUMP}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-edge-register")
        set_tests_properties(
          xyo-rt001-yield-edge-paused-register-smoke
          PROPERTIES TIMEOUT 60)
        # Real suspended Fiber machine-frame bounds for every
        # i64-only yield SSA StackMap home. A stack address inside a
        # Fiber is NOT a proven GC-managed object or owned root.
        add_test(NAME xyo-rt001-yield-ssa-paused-spill-bounds-smoke
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_ssa_spill_bounds.py"
            --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-cfi-unwind-smoke>"
            --readobj "${XYO_RT001_LLVM_READOBJ}"
            --objdump "${XYO_RT001_LLVM_OBJDUMP}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-ssa-spill-bounds")
        set_tests_properties(
          xyo-rt001-yield-ssa-paused-spill-bounds-smoke
          PROPERTIES TIMEOUT 60)
        # The known numeric scheduler tick was the ONLY volatile
        # SSA location in the real parked 2-Fiber fixture. Verify the
        # filtered machine StackMap removes it at O2/O3 without
        # trusting any other i64 as an owned GC root.
        add_test(NAME xyo-rt001-yield-tick-paused-fiber-smoke
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_yield_ssa_spill_bounds.py"
            --filter i64_no_tick
            --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-cfi-unwind-smoke>"
            --readobj "${XYO_RT001_LLVM_READOBJ}"
            --objdump "${XYO_RT001_LLVM_OBJDUMP}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-yield-tick-paused")
        set_tests_properties(
          xyo-rt001-yield-tick-paused-fiber-smoke
          PROPERTIES TIMEOUT 60)
      endif()
      # Exact GNU _Unwind_Find_FDE -> temporary libunwind dynamic
      # registration and cleanup. The live Fiber remains GC-unsafe.
      add_test(NAME xyo-rt001-native-fiber-cfi-dynamic-bridge-smoke
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_fiber_cfi_unwind.py"
          --bridge
          --smoke "$<TARGET_FILE:xyo-rt001-native-fiber-cfi-unwind-smoke>"
          --readobj "${XYO_RT001_LLVM_READOBJ}"
          --objdump "${XYO_RT001_LLVM_OBJDUMP}"
          --dwarfdump "${XYO_RT001_LLVM_DWARFDUMP}"
          --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-fiber-cfi-dynamic-bridge")
      # ResourceTracker owns actual ORC code allocations. A test-only
      # libunwind FDE lease must be canceled BEFORE tracker removal.
      # Covers two independent modules, transfer, replacement, and
      # four generational reloads; production JIT/GC remain untouched.
      add_executable(xyo-rt001-orc-fde-resource-tracker-smoke
        platform/rt_orc_fde_resource_tracker_smoke.cpp)
      target_include_directories(xyo-rt001-orc-fde-resource-tracker-smoke
        PRIVATE ${LLVM_INCLUDE_DIRS})
      target_link_libraries(xyo-rt001-orc-fde-resource-tracker-smoke
        PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS}
          ${XYO_RT001_UNWIND_LIBRARY}
          ${XYO_RT001_UNWIND_X86_64_LIBRARY}
          ${CMAKE_DL_LIBS})
      target_compile_options(xyo-rt001-orc-fde-resource-tracker-smoke
        PRIVATE -fno-exceptions -fno-rtti)
      add_test(NAME xyo-rt001-orc-fde-resource-tracker-smoke
        COMMAND xyo-rt001-orc-fde-resource-tracker-smoke)
      add_test(NAME xyo-rt001-orc-fde-resource-tracker-less-smoke
        COMMAND "${CMAKE_COMMAND}" -E env
          XYO_RT001_CODEGEN_OPT=less
          "$<TARGET_FILE:xyo-rt001-orc-fde-resource-tracker-smoke>")
      add_test(NAME xyo-rt001-orc-fde-resource-tracker-none-smoke
        COMMAND "${CMAKE_COMMAND}" -E env
          XYO_RT001_CODEGEN_OPT=none
          "$<TARGET_FILE:xyo-rt001-orc-fde-resource-tracker-smoke>")
      # Real ORC JITLink callback lifecycle (not manual leases):
      # PostFixup -> notifyEmitted -> notifyRemovingResources, including
      # transfer and generational reloads. GC is always prohibited.
      add_executable(xyo-rt001-orc-fde-link-plugin-smoke
        platform/rt_orc_fde_link_plugin_smoke.cpp)
      target_include_directories(xyo-rt001-orc-fde-link-plugin-smoke
        PRIVATE ${LLVM_INCLUDE_DIRS})
      target_link_libraries(xyo-rt001-orc-fde-link-plugin-smoke
        PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS}
          ${XYO_RT001_UNWIND_LIBRARY}
          ${XYO_RT001_UNWIND_X86_64_LIBRARY}
          ${CMAKE_DL_LIBS})
      target_compile_options(xyo-rt001-orc-fde-link-plugin-smoke
        PRIVATE -fno-exceptions -fno-rtti)
      add_test(NAME xyo-rt001-orc-fde-link-plugin-smoke
        COMMAND xyo-rt001-orc-fde-link-plugin-smoke)
    endif()
  endif()
  if(Python3_EXECUTABLE)
    add_test(NAME xyo-rt001-postopt-ssa-eager-lazy-differential
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_postopt_ssa_audit.py"
        --native "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-postopt-ssa-differential")
  endif()
endif()

# RT-001: new opt-in native per-access variable rebinding. The test
# generates a small SB3, suspends an ACTUAL compiled LLVM Fiber after
# a scheduler tick, edits Target::variables and resumes the same Fiber.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND
   Python3_EXECUTABLE AND NOT EMSCRIPTEN)
  add_executable(xyo-rt001-native-varslot-editor-rebind-smoke
    platform/rt_native_varslot_editor_rebind_smoke.cpp)
  target_link_libraries(xyo-rt001-native-varslot-editor-rebind-smoke
    PRIVATE xyo)
  target_compile_options(xyo-rt001-native-varslot-editor-rebind-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-native-varslot-editor-rebind-smoke
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_varslot_editor_rebind.py"
      --smoke "$<TARGET_FILE:xyo-rt001-native-varslot-editor-rebind-smoke>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-native-varslot-editor")
  # LLVM IR performs the hot generation/key check; on a miss the
  # original C++ checked helper rebinds. Instrument fast-path hits
  # only for this test. Editor mutation must force a slow rebind.
  add_test(NAME xyo-rt001-native-varslot-editor-stage-global-inline-smoke
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_varslot_editor_rebind.py"
      --fast --indexed --stage-global
      --smoke "$<TARGET_FILE:xyo-rt001-native-varslot-editor-rebind-smoke>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-native-varslot-editor-global-inline")
  set_tests_properties(
    xyo-rt001-native-varslot-editor-stage-global-inline-smoke PROPERTIES
    ENVIRONMENT "XYO_JIT_VAR_GUARD_INLINE=1")
  add_test(NAME xyo-rt001-native-varslot-editor-stage-shadow-inline-smoke
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_varslot_editor_rebind.py"
      --fast --indexed --stage-shadow
      --smoke "$<TARGET_FILE:xyo-rt001-native-varslot-editor-rebind-smoke>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-native-varslot-editor-shadow-inline")
  set_tests_properties(
    xyo-rt001-native-varslot-editor-stage-shadow-inline-smoke PROPERTIES
    ENVIRONMENT "XYO_JIT_VAR_GUARD_INLINE=1")
  add_test(NAME xyo-rt001-native-varslot-editor-rebind-inline-smoke
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_varslot_editor_rebind.py"
      --fast --indexed
      --smoke "$<TARGET_FILE:xyo-rt001-native-varslot-editor-rebind-smoke>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-native-varslot-editor-inline")
  set_tests_properties(
    xyo-rt001-native-varslot-editor-rebind-inline-smoke PROPERTIES
    ENVIRONMENT "XYO_JIT_VAR_GUARD_INLINE=1")
  add_test(NAME xyo-rt001-native-varslot-editor-rebind-indexed-smoke
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_varslot_editor_rebind.py"
      --fast --indexed
      --smoke "$<TARGET_FILE:xyo-rt001-native-varslot-editor-rebind-smoke>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-native-varslot-editor-indexed")
  add_test(NAME xyo-rt001-native-varslot-editor-rebind-fast-smoke
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/rt_native_varslot_editor_rebind.py"
      --fast
      --smoke "$<TARGET_FILE:xyo-rt001-native-varslot-editor-rebind-smoke>"
      --out "${CMAKE_CURRENT_BINARY_DIR}/rt001-native-varslot-editor-fast")
endif()

# RT-001: real JitCtx::varSlotData -> Thread::jitVarSlots ->
# Target::variables ownership. Verify exact GC root continuity
# AND invalidation on structural deletion; no GC permission granted.
if(NOT EMSCRIPTEN)
  add_executable(xyo-rt001-jitctx-varslot-gc-root-smoke
    platform/rt_jitctx_varslot_gc_root_smoke.cpp)
  target_link_libraries(xyo-rt001-jitctx-varslot-gc-root-smoke PRIVATE xyo)
  target_compile_options(xyo-rt001-jitctx-varslot-gc-root-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-jitctx-varslot-gc-root-smoke
    COMMAND xyo-rt001-jitctx-varslot-gc-root-smoke)
  add_test(NAME xyo-rt001-jitctx-varslot-gc-root-indexed-smoke
    COMMAND xyo-rt001-jitctx-varslot-gc-root-smoke)
  set_tests_properties(
    xyo-rt001-jitctx-varslot-gc-root-indexed-smoke PROPERTIES
    ENVIRONMENT "XYO_RT_GUARD_NATIVE_VAR_SLOTS=1;XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST=1;XYO_RT_GUARD_NATIVE_VAR_SLOTS_INDEXED=1")
endif()

# RT-001: heap-threshold collection must not run inside the
# experimental source-SSA Fiber activation. Allocate >2 MiB with
# threshold=1 MiB, verify DeferCollect suppresses automatic GC,
# then verify the very next allocation collects after scope exit.
if(NOT EMSCRIPTEN)
  add_executable(xyo-rt001-native-gc-deferral-smoke
    platform/rt_native_gc_deferral_smoke.cpp)
  target_link_libraries(xyo-rt001-native-gc-deferral-smoke PRIVATE xyo)
  target_compile_options(xyo-rt001-native-gc-deferral-smoke
    PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-rt001-native-gc-deferral-smoke
    COMMAND xyo-rt001-native-gc-deferral-smoke)
  # Run the same collector test with the experimental GC/LLVM SSA
  # protection enabled. It rents two independent Fibers and requires
  # both leases to end before a pending sweep may proceed.
  add_test(NAME xyo-rt001-native-gc-multifiber-barrier-smoke
    COMMAND xyo-rt001-native-gc-deferral-smoke)
  set_tests_properties(xyo-rt001-native-gc-multifiber-barrier-smoke
    PROPERTIES ENVIRONMENT "XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1")
endif()

# MIT TinySoundFont + original CC0 SF2 through SDL dummy.
if(SDL2_FOUND)
  add_executable(xyo-music-synth-smoke platform/music_synth_smoke.cpp)
  target_link_libraries(xyo-music-synth-smoke PRIVATE xyo)
  add_test(NAME xyo-music-synth-smoke COMMAND xyo-music-synth-smoke)
  set_tests_properties(xyo-music-synth-smoke PROPERTIES
    ENVIRONMENT "SDL_AUDIODRIVER=dummy;XYO_MUSIC_SOUNDFONT=${CMAKE_SOURCE_DIR}/assets/soundfonts/xyo-basic.sf2"
    TIMEOUT 30)
endif()

if(TARGET xyo-cli)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-music-extension
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/music_extension.py"
        "$<TARGET_FILE:xyo-cli>"
        "${CMAKE_CURRENT_BINARY_DIR}/music-extension")
    set_tests_properties(xyo-music-extension PROPERTIES TIMEOUT 120)
  endif()
endif()


# Scratch Translate: user-supplied local PLaMo API, no weights downloaded.
# Python's mock HTTP server binds ONLY to 127.0.0.1.
if(TARGET xyo-cli AND XYO_TRANSLATE_LIBCURL_FOUND)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-plamo-translate-local
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/translate_plamo_local.py"
        "$<TARGET_FILE:xyo-cli>"
        "${CMAKE_CURRENT_BINARY_DIR}/plamo-translate-local")
    set_tests_properties(xyo-plamo-translate-local PROPERTIES TIMEOUT 60)
  endif()
endif()

# Stale async translation results must not revive stopped/restarted scripts.
if(XYO_TRANSLATE_LIBCURL_FOUND AND NOT WIN32 AND NOT EMSCRIPTEN)
  add_executable(xyo-plamo-translation-lifecycle
    platform/plamo_translation_lifecycle.cpp)
  target_link_libraries(xyo-plamo-translation-lifecycle PRIVATE xyo)
  add_test(NAME xyo-plamo-translation-lifecycle
    COMMAND xyo-plamo-translation-lifecycle)
  set_tests_properties(xyo-plamo-translation-lifecycle PROPERTIES
    ENVIRONMENT "XYO_PLAMO_PORT=1"
    TIMEOUT 20)
endif()


# Late HTTP completions must not revive stopped/replaced Scratch threads.
if(TARGET xyo-plamo-translation-lifecycle AND Python3_Interpreter_FOUND)
  add_test(NAME xyo-plamo-lifecycle-delayed
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_lifecycle_delayed.py"
      "$<TARGET_FILE:xyo-plamo-translation-lifecycle>")
  set_tests_properties(xyo-plamo-lifecycle-delayed PROPERTIES TIMEOUT 20)
endif()

# A server accepting connections but withholding responses for three
# seconds must not make Scratch teardown wait for that response.
if(TARGET xyo-plamo-translation-lifecycle AND Python3_Interpreter_FOUND)
  add_test(NAME xyo-plamo-lifecycle-stalled
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_lifecycle_delayed.py"
      "$<TARGET_FILE:xyo-plamo-translation-lifecycle>")
  set_tests_properties(xyo-plamo-lifecycle-stalled PROPERTIES
    ENVIRONMENT "XYO_PLAMO_STALLED=1" TIMEOUT 15)
endif()

# A real Scratch .sb3 creates 32 clones; the bridge must accept only
# four simultaneous network translations and resolve the other 28
# through Scratch's original-string fallback.
if(TARGET xyo-cli AND XYO_TRANSLATE_LIBCURL_FOUND AND Python3_Interpreter_FOUND)
  add_test(NAME xyo-plamo-clone-stress
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_clone_stress.py"
      "$<TARGET_FILE:xyo-cli>"
      "${CMAKE_CURRENT_BINARY_DIR}/plamo-clone-stress")
  set_tests_properties(xyo-plamo-clone-stress PROPERTIES TIMEOUT 60)
endif()

# Async translation in a reachable procedure must not enter native JIT.
# Separate synchronous scripts must still compile normally.
if(TARGET xyo-cli AND XYO_JIT)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-plamo-jit-procedure-gate
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_jit_procedure_gate.py"
        "$<TARGET_FILE:xyo-cli>"
        "${CMAKE_CURRENT_BINARY_DIR}/plamo-jit-procedure-gate")
    set_tests_properties(xyo-plamo-jit-procedure-gate PROPERTIES TIMEOUT 90)
  endif()
endif()

# Experimental opt-in: LLVM Fibers may suspend at an async translate
# reporter and resume the original native callsite after localhost HTTP.
# This is intentionally not the default until GC and cancellation stress
# have broader coverage.
if(TARGET xyo-cli AND XYO_JIT AND XYO_TRANSLATE_LIBCURL_FOUND)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-plamo-native-fiber
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_native_fiber.py"
        "$<TARGET_FILE:xyo-cli>"
        "${CMAKE_CURRENT_BINARY_DIR}/plamo-native-fiber")
    set_tests_properties(xyo-plamo-native-fiber PROPERTIES TIMEOUT 90)
    add_test(NAME xyo-plamo-native-fiber-indexed
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_native_fiber.py"
        "$<TARGET_FILE:xyo-cli>"
        "${CMAKE_CURRENT_BINARY_DIR}/plamo-native-fiber-indexed")
    set_tests_properties(xyo-plamo-native-fiber-indexed PROPERTIES
      ENVIRONMENT "XYO_RT_GUARD_NATIVE_VAR_SLOTS_FAST=1;XYO_RT_GUARD_NATIVE_VAR_SLOTS_INDEXED=1"
      TIMEOUT 90)
  endif()
endif()

# RT-001: native translation cancellation with Thread-owned variable roots.
if(TARGET xyo-cli AND XYO_JIT AND XYO_TRANSLATE_LIBCURL_FOUND)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-rt001-async-variable-root-cancel
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_async_variable_root_cancel.py"
        "$<TARGET_FILE:xyo-cli>"
        "${CMAKE_CURRENT_BINARY_DIR}/plamo-async-variable-root-cancel")
    set_tests_properties(xyo-rt001-async-variable-root-cancel PROPERTIES
      TIMEOUT 20)
  endif()
endif()

# Native PLaMo Fiber can be retired while a slow localhost HTTP request
# is still running; the cancellation path must not resurrect the script.
if(TARGET xyo-cli AND XYO_JIT AND XYO_TRANSLATE_LIBCURL_FOUND)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME xyo-plamo-native-stop-all
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_native_stop_all.py"
        "$<TARGET_FILE:xyo-cli>" "${CMAKE_CURRENT_BINARY_DIR}/plamo-native-stop")
    set_tests_properties(xyo-plamo-native-stop-all PROPERTIES TIMEOUT 20)
    add_test(NAME xyo-plamo-native-clone-delete
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_native_clone_delete.py"
        "$<TARGET_FILE:xyo-cli>" "${CMAKE_CURRENT_BINARY_DIR}/plamo-native-clone-delete")
    set_tests_properties(xyo-plamo-native-clone-delete PROPERTIES TIMEOUT 20)
    add_test(NAME xyo-plamo-native-fiber-strict-gc
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_native_fiber.py"
        "$<TARGET_FILE:xyo-cli>" "${CMAKE_CURRENT_BINARY_DIR}/plamo-native-fiber-gc")
    set_tests_properties(xyo-plamo-native-fiber-strict-gc PROPERTIES
      ENVIRONMENT "XYO_RT_REQUIRE_GENERATED_SSA_ROOTS=1" TIMEOUT 90)
  endif()
endif()

# Force 128 explicit collections while a real LLVM translation Fiber owns
# uncheckpointed SSA, then verify GC drains only after Fiber retirement.
if(XYO_JIT AND XYO_TRANSLATE_LIBCURL_FOUND AND Python3_Interpreter_FOUND)
  add_executable(xyo-plamo-native-gc-wait
    platform/plamo_native_gc_wait.cpp)
  target_link_libraries(xyo-plamo-native-gc-wait PRIVATE xyo)
  add_test(NAME xyo-plamo-native-gc-wait
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_native_gc_wait.py"
      "$<TARGET_FILE:xyo-plamo-native-gc-wait>"
      "${CMAKE_CURRENT_BINARY_DIR}/plamo-native-gc-wait")
  set_tests_properties(xyo-plamo-native-gc-wait PROPERTIES TIMEOUT 25)
endif()

# Multiple native HTTP reporter Fibers: partial cancellation, repeated
# stopAll/greenFlag, and deferred GC must preserve all unsafe activations.
if(XYO_JIT AND XYO_TRANSLATE_LIBCURL_FOUND AND Python3_Interpreter_FOUND)
  add_executable(xyo-plamo-native-gc-multifiber
    platform/plamo_native_gc_multifiber.cpp)
  target_link_libraries(xyo-plamo-native-gc-multifiber PRIVATE xyo)
  add_test(NAME xyo-plamo-native-gc-multifiber
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/differential/plamo_native_gc_multifiber.py"
      "$<TARGET_FILE:xyo-plamo-native-gc-multifiber>"
      "${CMAKE_CURRENT_BINARY_DIR}/plamo-native-gc-multifiber")
  set_tests_properties(xyo-plamo-native-gc-multifiber PROPERTIES TIMEOUT 50)
endif()

# AOT v3 lazy BlockCached descriptor: thread-safe first use, cache retirement,
# source edit survival and GC-owned field preservation.
if(XYO_JIT)
  add_executable(xyo-aot-lazy-cache-smoke platform/aot_lazy_cache_smoke.cpp)
  target_link_libraries(xyo-aot-lazy-cache-smoke PRIVATE xyo)
  add_test(NAME xyo-aot-lazy-cache-smoke COMMAND xyo-aot-lazy-cache-smoke)
  set_tests_properties(xyo-aot-lazy-cache-smoke PROPERTIES TIMEOUT 120)
endif()

# START-012 AOT v3: independent native warp scripts remain worker-safe
# after caller-thread cache materialization, with serial result parity.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-aot-lazy-parallel-smoke platform/aot_lazy_parallel_smoke.cpp)
  target_link_libraries(xyo-aot-lazy-parallel-smoke PRIVATE xyo)
  # START-017: nested graph commits, cache-generation retention and native
  # source revision gating under a real 64-block bulk paste and deletion.
  add_executable(xyo-aot-structural-edit-smoke
    platform/aot_structural_edit_transaction_smoke.cpp)
  target_link_libraries(xyo-aot-structural-edit-smoke PRIVATE xyo)
  add_test(NAME xyo-aot-structural-edit-smoke
    COMMAND xyo-aot-structural-edit-smoke)
  set_tests_properties(xyo-aot-structural-edit-smoke PROPERTIES TIMEOUT 120)
  add_test(NAME xyo-aot-lazy-parallel-smoke COMMAND xyo-aot-lazy-parallel-smoke)
  set_tests_properties(xyo-aot-lazy-parallel-smoke PROPERTIES TIMEOUT 120)
  add_test(NAME xyo-aot-lazy-parallel-serial-smoke
           COMMAND cmake -E env XYO_PAR=0
                   $<TARGET_FILE:xyo-aot-lazy-parallel-smoke>)
  set_tests_properties(xyo-aot-lazy-parallel-serial-smoke PROPERTIES TIMEOUT 120)
  # START-014: same real native v3 project, metadata read directly by
  # optional yyjson without materializing the std::map-based JSON DOM.
  if(XYO_AOT_YYJSON)
    add_test(NAME xyo-aot-lazy-parallel-fastmeta-smoke
             COMMAND cmake -E env XYO_AOT_FAST_META=1 XYO_PAR=1
                     $<TARGET_FILE:xyo-aot-lazy-parallel-smoke>)
    set_tests_properties(xyo-aot-lazy-parallel-fastmeta-smoke
                         PROPERTIES TIMEOUT 120)
  endif()
  # START-013: glibc-only opt-in returns unused pages after lazy AOT load.
  # The same native worker/parity smoke must remain exact after malloc_trim.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME xyo-aot-lazy-parallel-trim-smoke
             COMMAND cmake -E env XYO_AOT_LAZY_MALLOC_TRIM=1 XYO_PAR=1
                     $<TARGET_FILE:xyo-aot-lazy-parallel-smoke>)
    set_tests_properties(xyo-aot-lazy-parallel-trim-smoke PROPERTIES TIMEOUT 120)
  endif()
endif()

# Audited external math helper effects: policy and actual optimized JIT/AOT IR.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-llvm-helper-attributes-smoke
    platform/llvm_helper_attributes_smoke.cpp)
  target_include_directories(xyo-llvm-helper-attributes-smoke PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-llvm-helper-attributes-smoke PRIVATE ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  target_compile_options(xyo-llvm-helper-attributes-smoke PRIVATE -fno-exceptions -fno-rtti)
  add_test(NAME xyo-llvm-helper-attributes-on COMMAND xyo-llvm-helper-attributes-smoke)
  add_test(NAME xyo-llvm-helper-attributes-off COMMAND xyo-llvm-helper-attributes-smoke off)
  set_tests_properties(xyo-llvm-helper-attributes-on PROPERTIES
    ENVIRONMENT "XYO_JIT_PURE_MATH=1")
  set_tests_properties(xyo-llvm-helper-attributes-off PROPERTIES
    ENVIRONMENT "XYO_JIT_PURE_MATH=0")
  if(TARGET xyo-cli AND Python3_Interpreter_FOUND)
    add_test(NAME xyo-jit-llvm-helper-attributes
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_llvm_helper_attributes.py"
        "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_CURRENT_BINARY_DIR}/llvm-helper-attributes" --aot --trip 10000)
    set_tests_properties(xyo-jit-llvm-helper-attributes PROPERTIES TIMEOUT 120)
  endif()
endif()

# Arithmetic list projections retain coercion, bounds and finite/range guards.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-llvm-numeric-projection-raw-smoke
    platform/llvm_numeric_projection_raw_smoke.cpp)
  target_link_libraries(xyo-llvm-numeric-projection-raw-smoke PRIVATE xyo)
  add_executable(xyo-numeric-region-list-checkpoint-smoke
    platform/numeric_region_list_checkpoint_smoke.cpp)
  target_link_libraries(xyo-numeric-region-list-checkpoint-smoke PRIVATE xyo)
  if(TARGET xyo-cli AND Python3_Interpreter_FOUND)
    add_test(NAME xyo-jit-numeric-coercion-guards
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_numeric_classify.py"
        "$<TARGET_FILE:xyo-cli>"
        --out "${CMAKE_CURRENT_BINARY_DIR}/numeric-coercion-guards" --quick --aot
        --raw-smoke "$<TARGET_FILE:xyo-llvm-numeric-projection-raw-smoke>"
        --checkpoint-smoke "$<TARGET_FILE:xyo-numeric-region-list-checkpoint-smoke>")
    set_tests_properties(xyo-jit-numeric-coercion-guards PROPERTIES TIMEOUT 180)
  endif()
endif()

# Sanitizer-only CPU runtime regression: driver-free LSan signal.
# GPU offscreen initializes external driver libraries that produce unrelated
# LSan reports; this test deliberately uses CLI run with no SDL initialization.
if(XYO_JIT AND TARGET xyo-cli AND Python3_Interpreter_FOUND AND
   NOT EMSCRIPTEN AND CMAKE_CXX_FLAGS MATCHES "sanitize=address")
  add_test(NAME xyo-jit-cpu-lsan-teardown
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/render/jit_cpu_lsan_teardown_smoke.py"
      "$<TARGET_FILE:xyo-cli>" --runs 4)
  set_tests_properties(xyo-jit-cpu-lsan-teardown PROPERTIES
    TIMEOUT 90
    LABELS "jit;sanitizer;asan;lsan;cpu;teardown"
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:exitcode=83;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endif()
