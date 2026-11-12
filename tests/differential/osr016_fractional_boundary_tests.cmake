# OSR-016: Three-level counted repeat, independent fractional .5-boundary
# regressions. Included from tests/CMakeLists.txt inside the native LLVM
# headless test-registration guard; uses the OSR-013 native smoke target.
#
# Four pairwise cases with original Interpreter baseline + actual phase3
# native yield/edit + restored interpreter frames. Both block-count settings
# are tested. The "late" variants also edit on the fourth native yield.

set(_xyo_osr016_script
  "${CMAKE_CURRENT_SOURCE_DIR}/differential/jit_nested_warp_fractional_boundary.py")
set(_xyo_osr016_harness
  "$<TARGET_FILE:xyo-nested-warp-triple-yield-deopt-smoke>")

add_test(NAME xyo-nested-warp-fractional-boundary
  COMMAND "${Python3_EXECUTABLE}" "${_xyo_osr016_script}"
    --harness "${_xyo_osr016_harness}"
    --out "${CMAKE_BINARY_DIR}/nested-warp-fractional-boundary"
    --all-cases)
add_test(NAME xyo-nested-warp-fractional-boundary-fast
  COMMAND "${Python3_EXECUTABLE}" "${_xyo_osr016_script}"
    --harness "${_xyo_osr016_harness}"
    --out "${CMAKE_BINARY_DIR}/nested-warp-fractional-boundary-fast"
    --all-cases --without-block-count)

add_test(NAME xyo-nested-warp-fractional-boundary-late
  COMMAND "${Python3_EXECUTABLE}" "${_xyo_osr016_script}"
    --harness "${_xyo_osr016_harness}"
    --out "${CMAKE_BINARY_DIR}/nested-warp-fractional-boundary-late"
    --case both-tie --late)
add_test(NAME xyo-nested-warp-fractional-boundary-late-fast
  COMMAND "${Python3_EXECUTABLE}" "${_xyo_osr016_script}"
    --harness "${_xyo_osr016_harness}"
    --out "${CMAKE_BINARY_DIR}/nested-warp-fractional-boundary-late-fast"
    --case both-tie --late --without-block-count)

set_tests_properties(
  xyo-nested-warp-fractional-boundary
  xyo-nested-warp-fractional-boundary-fast
  xyo-nested-warp-fractional-boundary-late
  xyo-nested-warp-fractional-boundary-late-fast
  PROPERTIES TIMEOUT 600)

unset(_xyo_osr016_script)
unset(_xyo_osr016_harness)
