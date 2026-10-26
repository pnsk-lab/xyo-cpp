add_executable(xyo-native-ui-tests
  test_native_ui.cpp
  ${PROJECT_SOURCE_DIR}/src/ui/draw.cpp
  ${PROJECT_SOURCE_DIR}/src/ui/font.cpp
)
target_link_libraries(xyo-native-ui-tests PRIVATE xyo)
target_compile_features(xyo-native-ui-tests PRIVATE cxx_std_20)
add_test(NAME native-ui COMMAND xyo-native-ui-tests)
set_tests_properties(native-ui PROPERTIES
  ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
)

if(TARGET xyo-cli)
  add_test(NAME native-editor-operations
    COMMAND xyo-cli edit ${PROJECT_SOURCE_DIR}/examples/play_demo.sb3)
  set_tests_properties(native-editor-operations PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XYO_EDIT_TEST=1")
endif()

add_executable(xyo-native-font-tests
  test_native_font.cpp
  ${PROJECT_SOURCE_DIR}/src/ui/draw.cpp
  ${PROJECT_SOURCE_DIR}/src/ui/font.cpp)
target_link_libraries(xyo-native-font-tests PRIVATE xyo)
target_compile_features(xyo-native-font-tests PRIVATE cxx_std_20)
target_compile_definitions(xyo-native-font-tests PRIVATE
  NATIVE_UI_TEST_FONT_PATH="${PROJECT_SOURCE_DIR}/third_party/fonts/LiberationSans-Regular.ttf")
add_test(NAME native-font COMMAND xyo-native-font-tests)
set_tests_properties(native-font PROPERTIES
  ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
