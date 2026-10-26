cmake_minimum_required(VERSION 3.24)
project(xyo-cpp VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(XYO_JIT "Enable LLVM JIT" ON)
option(XYO_BUILD_SHELL "Build xyo CLI shell" ON)
option(XYO_BUILD_TESTS "Build tests" ON)

# --- LLVM ---
option(XYO_STATIC_LLVM "Link LLVM statically (static archives required)" OFF)
set(XYO_LLVM_MODE "auto" CACHE STRING "LLVM の検出方法: auto | config | llvm-config")
option(XYO_STATIC "Link the executable fully static (-static)" OFF)
if(XYO_JIT)
  # auto: config モード（LLVMConfig.cmake）を優先する。静的アーカイブの
  #       依存関係（component → library の順序）を llvm-config より確実に解決できる。
  # config: config モードのみ / llvm-config: llvm-config のみ（--link-static を明示）
  if(XYO_LLVM_MODE STREQUAL "config")
    find_package(LLVM CONFIG QUIET)
  elseif(XYO_LLVM_MODE STREQUAL "auto")
    # 既定は従来どおり llvm-config（共有 libLLVM を優先）。
    # llvm-config が無い環境のみ config モードへフォールバックする。
    find_program(XYO_LLVM_CONFIG_PROBE llvm-config)
    if(NOT XYO_LLVM_CONFIG_PROBE)
      find_package(LLVM CONFIG QUIET)
    endif()
  endif()
  if(LLVM_FOUND)
    message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} (config mode): ${LLVM_INCLUDE_DIRS}")
    llvm_map_components_to_libnames(XYO_LLVM_LIBS core orcjit native passes)
    set(LLVM_LIBS ${XYO_LLVM_LIBS})
    # system-libs はプラットフォーム依存（Windows: psapi/shell32/...、
    # Linux: z/zstd/tinfo/xml2/m）。llvm-config があればそれに従う。
    if(NOT LLVM_SYSTEM_LIBS)
      find_program(XYO_LLVM_CONFIG_FOR_SYSLIBS llvm-config)
      if(XYO_LLVM_CONFIG_FOR_SYSLIBS)
        if(XYO_STATIC_LLVM OR XYO_STATIC)
          execute_process(COMMAND ${XYO_LLVM_CONFIG_FOR_SYSLIBS} --link-static --system-libs
                          OUTPUT_VARIABLE LLVM_SYSTEM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
        else()
          execute_process(COMMAND ${XYO_LLVM_CONFIG_FOR_SYSLIBS} --system-libs
                          OUTPUT_VARIABLE LLVM_SYSTEM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
        endif()
      endif()
    endif()
    if(NOT LLVM_SYSTEM_LIBS)
      if(WIN32)
        set(LLVM_SYSTEM_LIBS "-lpsapi -lshell32 -lole32 -luuid")
      else()
        set(LLVM_SYSTEM_LIBS "-lz -lzstd -ltinfo -lxml2 -lm")
      endif()
    endif()
    separate_arguments(LLVM_SYSTEM_LIBS)
    set(LLVM_LDFLAGS "")
    set(XYO_LLVM_LINK_DIRS ${LLVM_LIBRARY_DIRS})
    set(XYO_LLVM_CONFIG_MODE ON)
  else()
    # 2) 従来の llvm-config（--link-static 指定で静的アーカイブ）
    find_program(LLVM_CONFIG llvm-config REQUIRED)
    execute_process(COMMAND ${LLVM_CONFIG} --version OUTPUT_VARIABLE LLVM_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG} --includedir OUTPUT_VARIABLE LLVM_INCLUDE_DIRS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG} --ldflags OUTPUT_VARIABLE LLVM_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(XYO_STATIC_LLVM OR XYO_STATIC)
      execute_process(COMMAND ${LLVM_CONFIG} --link-static --system-libs OUTPUT_VARIABLE LLVM_SYSTEM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
      execute_process(COMMAND ${LLVM_CONFIG} --link-static --libs core orcjit native passes OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
      execute_process(COMMAND ${LLVM_CONFIG} --system-libs OUTPUT_VARIABLE LLVM_SYSTEM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
      execute_process(COMMAND ${LLVM_CONFIG} --libs core orcjit native passes OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    separate_arguments(LLVM_LIBS)
    separate_arguments(LLVM_SYSTEM_LIBS)
    separate_arguments(LLVM_LDFLAGS)
    message(STATUS "LLVM ${LLVM_VERSION} (llvm-config): ${LLVM_INCLUDE_DIRS}")
  endif()
endif()

if(WIN32)
  # GetCurrentThreadStackLimits など Win8+ API を使う（zig toolchain も同値を設定）
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# --- コアライブラリ ---
file(GLOB_RECURSE XYO_CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp
  src/analysis/*.cpp
  src/model/*.cpp
  src/engine/*.cpp
  src/blocks/*.cpp
  src/load/*.cpp
  src/project/*.cpp
  src/render/*.cpp
  src/rt/*.cpp
  src/ir/*.cpp
  src/interp/*.cpp
  src/audio/*.cpp
  src/gpu/*.cpp
  src/wasm/*.cpp
)
if(XYO_JIT)
  file(GLOB_RECURSE XYO_JIT_SOURCES CONFIGURE_DEPENDS src/jit/*.cpp)
  list(REMOVE_ITEM XYO_JIT_SOURCES
       ${CMAKE_CURRENT_SOURCE_DIR}/src/jit/nojit_stub.cpp)
  list(APPEND XYO_CORE_SOURCES ${XYO_JIT_SOURCES})
  # AOT（プロジェクト全体のネイティブコード事前生成）は LLVM 必須
  list(APPEND XYO_CORE_SOURCES src/aot/aot.cpp)
else()
  # JitEngine 空実装 — 依存コード側に ifdef を散らさないためのスタブ
  list(APPEND XYO_CORE_SOURCES src/jit/nojit_stub.cpp)
endif()

# ファイバ切替アセンブリ
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  if(WIN32)
    list(APPEND XYO_CORE_SOURCES src/engine/fiber_x86_64_win64.S)
  else()
    list(APPEND XYO_CORE_SOURCES src/engine/fiber_x86_64_sysv.S)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  list(APPEND XYO_CORE_SOURCES src/engine/fiber_aarch64.S)
endif()

add_library(xyo STATIC ${XYO_CORE_SOURCES})
target_include_directories(xyo PUBLIC src)
target_compile_options(xyo PRIVATE -fno-exceptions $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
if(EMSCRIPTEN)
  # Emscripten: zlib は ports（-sUSE_ZLIB=1）から供給される（zlib 1.3.x）。
  # find_package(ZLIB) は使わず、リンクフラグでポートを有効化する。
  # <zlib.h> は sysroot/include に配置され既定の system include に載る。
  target_link_options(xyo PUBLIC -sUSE_ZLIB=1)
  message(STATUS "zlib (Emscripten port) enabled")
else()
  find_package(ZLIB REQUIRED)
  if(XYO_STATIC)
    # 完全静的リンク: find_package(ZLIB) は .so を返すため .a を明示的に探す
    find_library(XYO_ZLIB_STATIC NAMES libz.a z REQUIRED)
    target_link_libraries(xyo PUBLIC ${XYO_ZLIB_STATIC})
    if(ZLIB_INCLUDE_DIRS)
      target_include_directories(xyo PUBLIC ${ZLIB_INCLUDE_DIRS})
    endif()
    message(STATUS "zlib: ${XYO_ZLIB_STATIC} (static)")
  else()
    target_link_libraries(xyo PUBLIC ZLIB::ZLIB)
  endif()
endif()
if(WIN32)
  # CommandLineToArgvW（UTF-8 引数の取り直し）
  target_link_libraries(xyo PUBLIC shell32)
endif()
if(XYO_JIT)
  target_compile_definitions(xyo PUBLIC XYO_JIT=1)
  target_include_directories(xyo SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
  if(XYO_LLVM_LINK_DIRS)
    target_link_directories(xyo PUBLIC ${XYO_LLVM_LINK_DIRS})
  endif()
  # LLVM headers require RTTI disabled-compatible flags; keep exceptions off in our code
  target_link_libraries(xyo PUBLIC ${LLVM_LIBS} ${LLVM_SYSTEM_LIBS})
  if(XYO_STATIC_LLVM OR XYO_STATIC)
    # 静的リンクでは -rpath を落とすが、ライブラリ探索パス(-L)は必要
    # （LLVM の静的アーカイブは /usr/lib/llvm-NN/lib にある）。
    separate_arguments(XYO_LLVM_LDFLAGS_LIST NATIVE_COMMAND "${LLVM_LDFLAGS}")
    foreach(_f ${XYO_LLVM_LDFLAGS_LIST})
      if(_f MATCHES "^-L")
        target_link_options(xyo PUBLIC ${_f})
      endif()
    endforeach()
  else()
    target_link_options(xyo PUBLIC ${LLVM_LDFLAGS})
  endif()
endif()

# --- Vulkan GPU executor（headless/Windows 用 compute dispatch）---
# loader は実行時に dlopen/LoadLibrary するため Vulkan SDK は不要 —
# 必要なのは glslc（SPIR-V 埋め込み）と vulkan.h のみ。
# ヘッダは third_party/Vulkan-Headers（include/ のみ vendored、
# v1.4.363）を既定で使い、-DXYO_VULKAN_HEADERS=... で上書きできる。
find_program(XYO_GLSLC glslc)
find_path(XYO_VULKAN_INCLUDE NAMES vulkan/vulkan.h
    HINTS ${XYO_VULKAN_HEADERS}
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include
          ENV VULKAN_SDK
    PATH_SUFFIXES include Include)
if(XYO_GLSLC AND XYO_VULKAN_INCLUDE AND NOT EMSCRIPTEN)
  set(XYO_GPU_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/gpu_spv)
  file(MAKE_DIRECTORY ${XYO_GPU_SPV_DIR})
  add_custom_command(
    OUTPUT ${XYO_GPU_SPV_DIR}/gpu_vm_spv.inc
    COMMAND ${XYO_GLSLC} -mfmt=c -o ${XYO_GPU_SPV_DIR}/gpu_vm_spv.inc
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/gpu_vm.comp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/gpu_vm.comp
    VERBATIM)
  target_sources(xyo PRIVATE ${XYO_GPU_SPV_DIR}/gpu_vm_spv.inc)
  target_compile_definitions(xyo PRIVATE XYO_GPU_VULKAN=1)
  target_include_directories(xyo PRIVATE
    ${XYO_GPU_SPV_DIR} ${XYO_VULKAN_INCLUDE})
  target_link_libraries(xyo PRIVATE ${CMAKE_DL_LIBS})
  message(STATUS "gpu: Vulkan compute executor enabled")
else()
  message(STATUS "gpu: Vulkan compute executor disabled (glslc/headers missing)")
endif()

# --- vendored deps ---
# third_party/: stb_image (PNG), stb_truetype (フォント), nanosvg (SVG)

# --- SDL2（xyo play 表示レンダラ・任意依存）---
# ヘッドレス用途（静的リンク・サーバ）では -DXYO_SDL2=OFF で完全に外せる。
option(XYO_SDL2 "Enable the SDL2 window renderer (play/gui)" ON)
# CI/リリース用のガード: SDL2 が無い環境で黙って headless ビルドになり、
# play/gui が消えた成果物が出る事故を configure 段階で止める。
option(XYO_REQUIRE_SDL2 "Fail the configure step if SDL2 is not found" OFF)
if(XYO_SDL2 AND NOT EMSCRIPTEN)
# Linux: pkg-config / Windows 等: SDL2Config.cmake（-DSDL2_DIR= か
# -DCMAKE_PREFIX_PATH= で mingw 用 SDL2 を指定すると play/gui が有効になる）。
option(XYO_RSVG "Use librsvg for SVG costumes (Windows では通常 OFF)" ON)
find_package(PkgConfig QUIET)
set(XYO_SDL2_LIB "")
# 1) config モード（Windows は SDL2::SDL2-static を取るために必須）。
if(WIN32 OR SDL2_DIR OR CMAKE_PREFIX_PATH)
  # クロスビルドではホストの SDL2 を拾わないよう、明示ヒントがあるときだけ探す。
  if(NOT CMAKE_CROSSCOMPILING OR SDL2_DIR OR CMAKE_PREFIX_PATH)
    find_package(SDL2 CONFIG QUIET)
  endif()
  if(SDL2_FOUND AND WIN32 AND TARGET SDL2::SDL2)
    # Windows 以外の SDL2 を掴んでいないことを検査する。MSYS2 の
    # SDL2Config.cmake は IMPORTED_IMPLIB を構成別（_RELEASE）にしか
    # 設定しないため、両方を見る（片方だけ見ると正当な mingw SDL2 を
    # 「Windows 用ではない」と誤判定して headless に落ちる）。
    get_target_property(_xyo_sdl2_implib SDL2::SDL2 IMPORTED_IMPLIB)
    get_target_property(_xyo_sdl2_implib_rel SDL2::SDL2 IMPORTED_IMPLIB_RELEASE)
    if(NOT _xyo_sdl2_implib AND NOT _xyo_sdl2_implib_rel)
      message(STATUS "SDL2 config is not for Windows — ignored "
                     "(pass -DSDL2_DIR=<mingw SDL2>)")
      set(SDL2_FOUND FALSE)
    endif()
  endif()
  if(SDL2_FOUND)
    if(WIN32 AND TARGET SDL2::SDL2-static)
      set(XYO_SDL2_LIB SDL2::SDL2-static)
    else()
      set(XYO_SDL2_LIB SDL2::SDL2)
    endif()
  endif()
endif()
# 2) pkg-config（Linux など）。上の config モードが失敗したときだけ使う。
#    注意: pkg_check_modules は <prefix>_FOUND を INTERNAL キャッシュへ残すので、
#    「SDL2_FOUND が既に立っている」ことをスキップ条件にしない（再 configure で
#    XYO_SDL2_LIB が未設定のままになり SDL2 の include が落ちる）。
if(NOT XYO_SDL2_LIB AND PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
  pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
  if(SDL2_FOUND)
    set(XYO_SDL2_LIB PkgConfig::SDL2)
  endif()
endif()
if(NOT XYO_SDL2_LIB)
  set(SDL2_FOUND FALSE)   # キャッシュ由来の FOUND を引きずらない
endif()
if(SDL2_FOUND)
  target_compile_definitions(xyo PUBLIC XYO_HAVE_SDL2=1 SDL_MAIN_HANDLED=1)
  target_link_libraries(xyo PUBLIC ${XYO_SDL2_LIB})
  if(PkgConfig_FOUND AND XYO_RSVG)
    pkg_check_modules(RSVG QUIET IMPORTED_TARGET librsvg-2.0>=2.52)
  endif()
  if(RSVG_FOUND)
    target_compile_definitions(xyo PUBLIC XYO_HAVE_RSVG=1)
    target_link_libraries(xyo PUBLIC PkgConfig::RSVG)
  else()
    message(STATUS "librsvg not found: SVG costumes use the nanosvg fallback "
                   "(shapes only, no <text>)")
  endif()
  if(PkgConfig_FOUND)
    pkg_check_modules(GLPC QUIET IMPORTED_TARGET gl)
  endif()
  if(GLPC_FOUND AND NOT WIN32)
    # GPU ペン層（OpenGL レンダラ時のみ有効 — FBO+シェーダで
    # カプセル距離を解析評価、SDL_Renderer では表現不可）。
    # Windows の opengl32.dll は GL 1.1 のみで FBO/シェーダ API を持たないため、
    # glCreateShader 等の直接呼出しはリンクできない — Windows は CPU ラスタ。
    target_compile_definitions(xyo PUBLIC XYO_PEN_GL=1)
    target_link_libraries(xyo PUBLIC PkgConfig::GLPC)
  endif()
  message(STATUS "SDL2 found: 'xyo play' window renderer enabled")
else()
  message(STATUS "SDL2 not found: building without window renderer")
  if(XYO_REQUIRE_SDL2)
    message(FATAL_ERROR
      "XYO_REQUIRE_SDL2=ON ですが SDL2 が見つかりません — play/gui 無しの "
      "バイナリが作られてしまうため停止しました。SDL2 開発パッケージを入れるか "
      "(Windows: mingw-w64-x86_64-SDL2 + -DSDL2_DIR=<...>/lib/cmake/SDL2)、"
      "意図的に headless にするなら -DXYO_SDL2=OFF を明示してください。")
  endif()
endif()

elseif(XYO_SDL2 AND EMSCRIPTEN)
  # Emscripten: SDL2 は ports（-sUSE_SDL=2、SDL 2.32.x）から供給される。
  # find_package では無いため明示的に有効化する。librsvg/Cairo/Pango は
  # 存在しないので SVG コスチュームはビットマップのみ（既存フォールバック）。
  # SDL2 ヘッダは ports 展開後 sysroot/include/SDL2 へ入る。
  set(SDL2_FOUND TRUE)
  target_compile_definitions(xyo PUBLIC XYO_HAVE_SDL2=1 SDL_MAIN_HANDLED=1)
  target_link_options(xyo PUBLIC -sUSE_SDL=2)
  target_include_directories(xyo PUBLIC ${EMSCRIPTEN_SYSROOT}/include/SDL2)
  message(STATUS "SDL2 (Emscripten port): 'xyo play' canvas renderer enabled")

else()
  message(STATUS "SDL2 disabled (XYO_SDL2=OFF): building headless")
endif()

# --- 同梱フォント（Scratch の Arial 系メトリクス: Liberation Sans, OFL）---
# TTF を C++ 配列へ埋め込む（XYO_FONT 未指定時に使う既定フォント）。
# 埋め込みは cmake/embed_binary.cmake が行う（python 不要 — MSYS2 の python3 は
# POSIX パスを解釈するため、ビルド時のパス変換に依存しない）。
set(XYO_EMBED_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake)
set(XYO_FONT_INC_DIR ${CMAKE_CURRENT_BINARY_DIR}/font_data)
file(MAKE_DIRECTORY ${XYO_FONT_INC_DIR})
set(XYO_FONT_REGULAR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LiberationSans-Regular.ttf)
set(XYO_FONT_BOLD ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LiberationSans-Bold.ttf)
if(SDL2_FOUND AND EXISTS ${XYO_FONT_REGULAR} AND EXISTS ${XYO_FONT_BOLD})
  add_custom_command(
    OUTPUT ${XYO_FONT_INC_DIR}/xyo_fonts.inc
    COMMAND ${CMAKE_COMMAND}
            "-DXYO_EMBED_OUT=${XYO_FONT_INC_DIR}/xyo_fonts.inc"
            "-DXYO_EMBED_ARGS=xyoFontRegular;${XYO_FONT_REGULAR};xyoFontBold;${XYO_FONT_BOLD}"
            -P ${XYO_EMBED_SCRIPT}
    DEPENDS ${XYO_FONT_REGULAR} ${XYO_FONT_BOLD} ${XYO_EMBED_SCRIPT}
    VERBATIM)
  target_sources(xyo PRIVATE ${XYO_FONT_INC_DIR}/xyo_fonts.inc)
  target_include_directories(xyo PRIVATE ${XYO_FONT_INC_DIR})
  target_compile_definitions(xyo PRIVATE XYO_HAVE_EMBEDDED_FONT=1)
  message(STATUS "font: bundled Liberation Sans (OFL) embedded")
elseif(NOT SDL2_FOUND)
  message(STATUS "font: text rendering disabled (no SDL2) — fonts not embedded")
else()
  message(STATUS "font: bundled fonts missing — using system fonts")
endif()

# --- シェル ---
if(XYO_BUILD_SHELL)
  # editor.cpp が本体（SDL2 が無いビルドでは editor.cpp 内の #else スタブ）。
  add_executable(xyo-cli src/shell/main.cpp src/shell/gui.cpp
                 src/shell/editor.cpp
                 src/blockly/shape.cpp src/blockly/layout.cpp
                 src/blockly/geom.cpp
                 src/blockly/label.cpp src/blockly/ops.cpp)
  target_link_libraries(xyo-cli PRIVATE xyo)
  # Native batched UI: no external UI framework, reuse SDL2 and stb.
  if(SDL2_FOUND AND NOT EMSCRIPTEN)
    target_sources(xyo-cli PRIVATE src/ui/draw.cpp src/ui/font.cpp)
    if(EXISTS "${XYO_FONT_REGULAR}" AND EXISTS "${XYO_FONT_BOLD}")
      target_sources(xyo-cli PRIVATE ${XYO_FONT_INC_DIR}/xyo_fonts.inc)
      target_include_directories(xyo-cli PRIVATE ${XYO_FONT_INC_DIR})
      target_compile_definitions(xyo-cli PRIVATE XYO_HAVE_EMBEDDED_FONT=1)
      add_dependencies(xyo-cli xyo)   # xyo_fonts.inc は xyo ビルドが生成
    endif()
    # --- デフォルトプロジェクト（File→New。scratch-gui の default-project と同一）---
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.sb3)
      set(XYO_DEFPROJ_INC ${CMAKE_CURRENT_BINARY_DIR}/font_data/xyo_default_project.inc)
      add_custom_command(
        OUTPUT ${XYO_DEFPROJ_INC}
        COMMAND ${CMAKE_COMMAND}
                "-DXYO_EMBED_OUT=${XYO_DEFPROJ_INC}"
                "-DXYO_EMBED_ARGS=xyoDefaultProject;${CMAKE_CURRENT_SOURCE_DIR}/assets/default.sb3"
                -P ${XYO_EMBED_SCRIPT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.sb3 ${XYO_EMBED_SCRIPT}
        VERBATIM)
      target_sources(xyo-cli PRIVATE ${XYO_DEFPROJ_INC})
      target_include_directories(xyo-cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/font_data)
      target_compile_definitions(xyo-cli PRIVATE XYO_HAVE_DEFAULT_PROJECT=1)
    endif()
    endif()
  set_target_properties(xyo-cli PROPERTIES OUTPUT_NAME xyo)
  if(EMSCRIPTEN)
    # WASM は 32bit 直線メモリ。大規模プロジェクト用にヒープ成長 +
    # 深い再帰/JSON パース用にスタックを増量する。
    target_link_options(xyo-cli PRIVATE
      -sALLOW_MEMORY_GROWTH=1
      -sSTACK_SIZE=5242880)
    # XYO_WASM_ASYNCIFY: sleep/poll をサスペンド点にする（ブラウザの
    #   'xyo play' メインループ用）。Node 実行でも有効化して検証できる。
    # XYO_WASM_BROWSER: HTML シェル + MEMFS のブラウザ出力にする
    #   （OFF = Node の headless CLI: NODERAWFS + EXIT_RUNTIME）。
    option(XYO_WASM_ASYNCIFY "Emscripten: enable Asyncify (browser play)" OFF)
    option(XYO_WASM_BROWSER "Emscripten: emit the browser HTML shell (play)" OFF)
    # XYO_WASM_JIT: 実行時 wasm 生成 JIT（Phase C）のブリッジを有効化する。
    #   -sIMPORTED_MEMORY で JS 提供の WebAssembly.Memory を共有し、
    #   -sMODULARIZE で createXyo(options) 形式にし、生成モジュールへ
    #   memory とヘルパ（_xyo_wasm_h）を渡せるようにする。
    option(XYO_WASM_JIT "Emscripten: enable the runtime wasm-module JIT bridge" OFF)
    if(XYO_WASM_JIT)
      target_link_options(xyo-cli PRIVATE
        -sIMPORTED_MEMORY=1
        -sMODULARIZE=1
        -sEXPORT_NAME=createXyo
        -sMAXIMUM_MEMORY=2147483648
        -sEXPORTED_FUNCTIONS=_main,_xyo_wasm_h,_xyo_wasm_selftest)
    endif()
    if(XYO_WASM_ASYNCIFY)
      target_link_options(xyo-cli PRIVATE -sASYNCIFY)
    endif()
    if(XYO_WASM_BROWSER)
      target_link_options(xyo-cli PRIVATE
        -sENVIRONMENT=web
        -sFORCE_FILESYSTEM=1
        -sINVOKE_RUN=0
        -sEXPORTED_RUNTIME_METHODS=callMain,FS
        --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
      set_target_properties(xyo-cli PROPERTIES SUFFIX ".html")
      # シェル変更で再リンクされるように依存を明示（emcc は追跡しない）
      set_property(TARGET xyo-cli APPEND PROPERTY
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
    else()
      # headless CLI（Node）: NODERAWFS で実ファイルを読み、main 復帰で終了。
      target_link_options(xyo-cli PRIVATE
        -sEXIT_RUNTIME=1
        -sNODERAWFS=1
        -sENVIRONMENT=node)
    endif()
  endif()
endif()

# --- 静的リンク ---
if(XYO_JIT)
  if(XYO_STATIC)
    target_link_options(xyo PUBLIC -static -static-libstdc++ -static-libgcc)
    message(STATUS "link: fully static")
  elseif(XYO_STATIC_LLVM)
    target_link_options(xyo PUBLIC -static-libstdc++ -static-libgcc)
    message(STATUS "link: static LLVM + static libstdc++/libgcc")
  endif()
endif()

# --- テスト ---
if(XYO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  if(SDL2_FOUND AND NOT EMSCRIPTEN)
    add_subdirectory(tests/ui)
  endif()
endif()
