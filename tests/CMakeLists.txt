# xyo-cpp テスト
# - unit/: cast/compare/list/gc の table-driven テスト
# - fixtures/: refs/scratch-vm/test/fixtures の実行フィクスチャ（M2 以降）
# - differential/: node scratch-vm との差分ランナー
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS unit/*.cpp)
if(TEST_SOURCES)
  add_executable(xyo-tests ${TEST_SOURCES})
  target_link_libraries(xyo-tests PRIVATE xyo)
  add_test(NAME xyo-tests COMMAND xyo-tests)
endif()

file(GLOB RENDER_TEST_SOURCES CONFIGURE_DEPENDS render/*.cpp)
if(RENDER_TEST_SOURCES AND SDL2_FOUND)
  add_executable(xyo-render-tests ${RENDER_TEST_SOURCES})
  target_link_libraries(xyo-render-tests PRIVATE xyo)
  add_test(NAME xyo-render-tests COMMAND xyo-render-tests)
  set_tests_properties(xyo-render-tests PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
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
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
