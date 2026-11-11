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
  # OSR-010: a statically bounded inner repeat has a certified outer
  # body checkpoint; the native inner loop itself must never yield.
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
  # OSR-015: Source topology edit must fail closed after phase-4 yield.
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
    foreach(dynamic_mode IN ITEMS default active)
      foreach(dynamic_fast IN ITEMS on off)
        set(dynamic_name "xyo-nested-warp-dynamic-inner-deopt")
        set(dynamic_args)
        if(dynamic_mode STREQUAL "active")
          string(APPEND dynamic_name "-active")
          list(APPEND dynamic_args --stress-active-times)
        endif()
        if(dynamic_fast STREQUAL "off")
          string(APPEND dynamic_name "-fast")
          list(APPEND dynamic_args --without-block-count)
        endif()
        add_test(NAME "${dynamic_name}"
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_dynamic_inner_deopt.py"
            --harness "$<TARGET_FILE:xyo-nested-warp-dynamic-inner-deopt-smoke>"
            --out "${CMAKE_BINARY_DIR}/${dynamic_name}"
            ${dynamic_args})
        set_tests_properties("${dynamic_name}"
          PROPERTIES TIMEOUT 600)
      endforeach()
    endforeach()
    foreach(triple_fraction IN ITEMS off on)
      foreach(triple_fast IN ITEMS off on)
        set(triple_name "xyo-nested-warp-triple-yield")
        set(triple_args)
        if(triple_fraction STREQUAL "on")
          string(APPEND triple_name "-fractional")
          list(APPEND triple_args --fractional)
        else()
          string(APPEND triple_name "-deopt")
        endif()
        if(triple_fast STREQUAL "on")
          string(APPEND triple_name "-fast")
          list(APPEND triple_args --without-block-count)
        endif()
        add_test(NAME "${triple_name}"
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_triple_yield_deopt.py"
            --harness "$<TARGET_FILE:xyo-nested-warp-triple-yield-deopt-smoke>"
            --out "${CMAKE_BINARY_DIR}/${triple_name}"
            ${triple_args})
        set_tests_properties("${triple_name}" PROPERTIES TIMEOUT 600)
      endforeach()
    endforeach()
    # OSR-016: independent half-integer rounding boundary tests
    # reuse the source-frozen real phase-3 native-yield harness.
    include("${CMAKE_CURRENT_SOURCE_DIR}/differential/osr016_fractional_boundary_tests.cmake")
    foreach(quad_fraction IN ITEMS off on)
      foreach(quad_fast IN ITEMS off on)
        set(quad_name "xyo-nested-warp-quad-yield")
        set(quad_args --outer-count 100000)
        if(quad_fraction STREQUAL "on")
          string(APPEND quad_name "-fractional")
          list(APPEND quad_args --fractional)
        else()
          string(APPEND quad_name "-deopt")
        endif()
        if(quad_fast STREQUAL "on")
          string(APPEND quad_name "-fast")
          list(APPEND quad_args --without-block-count)
        endif()
        add_test(NAME "${quad_name}"
          COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_quad_yield_deopt.py"
            --harness "$<TARGET_FILE:xyo-nested-warp-quad-yield-deopt-smoke>"
            --out "${CMAKE_BINARY_DIR}/${quad_name}"
            ${quad_args})
        set_tests_properties("${quad_name}"
          PROPERTIES TIMEOUT 600)
      endforeach()
    endforeach()
    foreach(topology_fast IN ITEMS off on)
      set(topology_name "xyo-nested-warp-topology-failclosed")
      set(topology_args --outer-count 100000)
      if(topology_fast STREQUAL "on")
        string(APPEND topology_name "-fast")
        list(APPEND topology_args --without-block-count)
      endif()
      add_test(NAME "${topology_name}"
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_topology_failclosed.py"
          --harness "$<TARGET_FILE:xyo-nested-warp-topology-failclosed-smoke>"
          --out "${CMAKE_BINARY_DIR}/${topology_name}"
          ${topology_args})
      set_tests_properties("${topology_name}"
        PROPERTIES TIMEOUT 600)
    endforeach()
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
  # Native/GC stress on shared runners can exceed two minutes even if valid.
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

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS unit/*.cpp)
# Standalone DLSS alpha metadata test owns its own main().
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/unit/dlss_alpha_coverage_unit.cpp")
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

# v1.2.2: same millisecond wall-clock reporter, boxed vs direct-f64 JIT path.
# Check time-invariant arithmetic in the Interpreter, Eager and HotSpot
# tiers without mocking the system clock.
if(TARGET xyo-cli AND XYO_JIT AND Python3_Interpreter_FOUND AND NOT EMSCRIPTEN)
  add_test(NAME xyo-days-f64-parity
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_days_f64.py"
      "$<TARGET_FILE:xyo-cli>"
      "${CMAKE_BINARY_DIR}/days-f64-differential")
  set_tests_properties(xyo-days-f64-parity PROPERTIES TIMEOUT 240)
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

# The ORC par-for co-materialization policy must reject pointer escapes,
# indirect callbacks and address sharing before an experimental lazy tier.
if(XYO_JIT AND XYO_JIT_BACKEND STREQUAL "llvm" AND NOT EMSCRIPTEN)
  add_executable(xyo-jit-parfor-partition-policy
    platform/jit_parfor_partition_policy_smoke.cpp)
  target_include_directories(xyo-jit-parfor-partition-policy PRIVATE
    ${LLVM_INCLUDE_DIRS})
  target_link_libraries(xyo-jit-parfor-partition-policy PRIVATE
    xyo ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  add_test(NAME xyo-jit-parfor-partition-policy
    COMMAND xyo-jit-parfor-partition-policy)
  set_tests_properties(xyo-jit-parfor-partition-policy PROPERTIES TIMEOUT 30)
endif()

# Production HotSpot chooses per script whether to compile procedures
# on first use. Verify the CLI policy, selected tier and Scratch output.
if(TARGET xyo-cli)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    # Lightweight, platform-independent validation of JIT phase-log parsing
    # and Scratch script topology. Timing measurements remain opt-in.
    add_test(NAME xyo-jit-phase-profile-parser
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/bench/test_jit_compile_phase_bench.py")
    set_tests_properties(xyo-jit-phase-profile-parser PROPERTIES TIMEOUT 30)

    add_test(NAME xyo-jit-lazy-integration
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_lazy_integration.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-lazy-integration PROPERTIES TIMEOUT 180)

    # BUG-001: address-taken internal par-for functions must be fully
    # materialized before the C++ callback invokes their native pointer.
    add_test(NAME xyo-jit-lazy-parfor-callback
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_lazy_parfor_callback.py"
              --native "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}/jit-lazy-parfor-fixtures")
    set_tests_properties(xyo-jit-lazy-parfor-callback PROPERTIES TIMEOUT 120)

    # Experimental per-script ORC partitioning is opt-in and fail-closed.
    # Check real sequential and worker-pool callback paths against eager
    # and interpreter, including two distinct address-taken body functions.
    add_test(NAME xyo-jit-lazy-parfor-partition
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_lazy_parfor_partition.py"
              --native "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}/jit-lazy-parfor-partition-fixtures")
    set_tests_properties(xyo-jit-lazy-parfor-partition PROPERTIES TIMEOUT 180)

    # Multiple fresh processes exercise native worker completion and
    # JIT teardown repeatedly. This does not claim safety for concurrent
    # external destruction of an active Runtime.
    add_test(NAME xyo-jit-lazy-parfor-partition-stress
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_lazy_parfor_partition.py"
              --native "$<TARGET_FILE:xyo-cli>"
              --out "${CMAKE_CURRENT_BINARY_DIR}/jit-lazy-parfor-stress-fixtures"
              --stress-runs 64)
    set_tests_properties(xyo-jit-lazy-parfor-partition-stress
      PROPERTIES TIMEOUT 180)

    add_test(NAME xyo-jit-parfor-parity-merge-unit
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/test_jit_lazy_parfor_parity_integration.py")
    set_tests_properties(xyo-jit-parfor-parity-merge-unit
      PROPERTIES TIMEOUT 30)

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

  option(XYO_DLSS_REGISTER_GPU_TESTS
    "Register actual NVIDIA hardware NGX tests in CTest" OFF)
  if(XYO_DLSS_REGISTER_GPU_TESTS)
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
