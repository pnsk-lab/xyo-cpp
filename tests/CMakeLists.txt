# xyo-cpp テスト
# - unit/: cast/compare/list/gc の table-driven テスト
# - fixtures/: refs/scratch-vm/test/fixtures の実行フィクスチャ（M2 以降）
# - differential/: node scratch-vm との差分ランナー
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS unit/*.cpp)
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

    # Event-driven pure wait-until: interpreter, eager, HotSpot, parallel,
    # time-dependent/impure fallback, and multiple waiters with a writer.
    add_test(NAME xyo-event-driven-scheduler
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/event_scheduler.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-event-driven-scheduler PROPERTIES TIMEOUT 180)

    # Punctuation equality, immutable digit-set contains and warp recursion.
    add_test(NAME xyo-jit-ascii-fastpaths
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_ascii_fastpaths.py"
              "$<TARGET_FILE:xyo-cli>")
    set_tests_properties(xyo-jit-ascii-fastpaths PROPERTIES TIMEOUT 180)
  endif()
endif()

file(GLOB RENDER_TEST_SOURCES CONFIGURE_DEPENDS render/*.cpp)
if(RENDER_TEST_SOURCES AND SDL2_FOUND)
  add_executable(xyo-render-tests ${RENDER_TEST_SOURCES})
  target_link_libraries(xyo-render-tests PRIVATE xyo)
  add_test(NAME xyo-render-tests COMMAND xyo-render-tests)
  set_tests_properties(xyo-render-tests PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XYO_JIT_MODE=eager")
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
