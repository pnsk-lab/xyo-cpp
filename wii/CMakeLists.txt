cmake_minimum_required(VERSION 3.24)
project(xyo-wii LANGUAGES C CXX ASM)

# Wii: 32-bit big-endian PowerPC with libogc.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Wii requires a 32-bit devkitPPC toolchain: -DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Wii.cmake")
endif()

set(XYO_JIT OFF CACHE BOOL "" FORCE)
set(XYO_SDL2 OFF CACHE BOOL "" FORCE)
set(XYO_BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(XYO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XYO_ALL_OPTS OFF CACHE BOOL "" FORCE)
set(XYO_BINARYEN_C_API OFF CACHE STRING "" FORCE)
set(XYO_NUM_TO_STR_DRAGONBOX OFF CACHE BOOL "" FORCE)
set(XYO_FULL_BUILD OFF CACHE BOOL "" FORCE)

add_subdirectory(".." "${CMAKE_CURRENT_BINARY_DIR}/core" EXCLUDE_FROM_ALL)
target_compile_definitions(xyo PUBLIC XYO_WII=1)

add_executable(xyo-wii main.cpp)
set_target_properties(xyo-wii PROPERTIES OUTPUT_NAME xyo-wii SUFFIX ".elf")
target_link_libraries(xyo-wii PRIVATE xyo fat wiiuse bte ogc m)

find_program(ELF2DOL NAMES elf2dol
  HINTS "$ENV{DEVKITPRO}/tools/bin" "$ENV{DEVKITPPC}/bin"
  REQUIRED)
add_custom_command(TARGET xyo-wii POST_BUILD
  COMMAND "${ELF2DOL}" "$<TARGET_FILE:xyo-wii>" "${CMAKE_CURRENT_BINARY_DIR}/boot.dol"
  COMMENT "Converting Wii ELF to Homebrew Channel boot.dol"
  VERBATIM)
