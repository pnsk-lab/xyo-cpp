# zig c++ による x86_64-windows-gnu クロスコンパイル用 toolchain。
# 使い方: cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/zig-windows-x86_64.cmake \
#           -DXYO_JIT=OFF -DXYO_BUILD_TESTS=OFF \
#           -DZLIB_LIBRARY=<zlib>/lib/libz.a -DZLIB_INCLUDE_DIR=<zlib>/include
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(_tools "${CMAKE_CURRENT_LIST_DIR}/../tools" ABSOLUTE)
set(CMAKE_C_COMPILER   "${_tools}/zig-cc")
set(CMAKE_CXX_COMPILER "${_tools}/zig-cxx")
set(CMAKE_ASM_COMPILER "${_tools}/zig-cxx")
set(CMAKE_AR           "${_tools}/zig-ar")
set(CMAKE_RANLIB       "${_tools}/zig-ranlib")

# libc++/lld は zig が内蔵。Windows API の新旧は 0x0A00 で固定。
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)

# ホストの pkg-config が Linux 用 SDL2/librsvg を誤検出するのを防ぐ
# （Windows 向け SDL2 を用意すれば外して play 対応も可能）。
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
