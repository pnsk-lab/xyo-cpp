cmake_minimum_required(VERSION 3.20)

project(xyo-cpp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SJIT_BUILD_HOST "Build the SDL window host with the Skia pen renderer" ON)
option(SJIT_PROFILE_RUNTIME "Enable non-atomic runtime profiling counters" OFF)
set(SKIA_ROOT "" CACHE PATH "Skia source root containing include/core/SkCanvas.h")
set(SKIA_OUT "" CACHE PATH "Skia GN output directory containing libskia")
set(SKIA_EXTRA_LIBRARIES "" CACHE STRING "Additional libraries required by the configured Skia build")

set(RUNTIME_C_SOURCES
    runtime/sjit_string.c
    runtime/sjit_value.c
    runtime/sjit_number.c
    runtime/sjit_compare.c
    runtime/sjit_operator.c
    runtime/sjit_pen.c
    runtime/sjit_list.c
    runtime/sjit_variable.c
    runtime/sjit_frame.c
    runtime/sjit_thread.c
    runtime/sjit_draw.c
    runtime/sjit_target.c
    runtime/sjit_sprite.c
    runtime/sjit_clone.c
    runtime/sjit_scheduler.c
    runtime/sjit_event.c
    runtime/sjit_script.c
    runtime/sjit_opcode_effects.c
    runtime/sjit_ownership.c
    runtime/sjit_thread_pool.c
    runtime/sjit_jit_bridge.c
    runtime/sjit_control.c
    runtime/sjit_data.c
    runtime/sjit_motion.c
    runtime/sjit_looks.c
    runtime/sjit_runtime.c
    runtime/sjit_host_api.c
)

set(QUICKJS_SUBSET_C_SOURCES
    quickjs/cutils.c
    quickjs/dtoa.c
    quickjs_subset/sjit_quickjs_string.c
)

set(CORE_CXX_SOURCES
    src/compiler.cpp
    src/jit.cpp
    src/project_loader.cpp
)

set(HOST_CXX_SOURCES
    src/host_app.cpp
)

find_package(LLVM REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

if(SJIT_BUILD_HOST)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(RSVG REQUIRED librsvg-2.0)

    set(SJIT_SKIA_TARGET "")
    set(_SJIT_SKIA_ROOT "${SKIA_ROOT}")
    set(_SJIT_SKIA_OUT "${SKIA_OUT}")
    if(NOT _SJIT_SKIA_ROOT AND DEFINED ENV{SKIA_ROOT})
        set(_SJIT_SKIA_ROOT "$ENV{SKIA_ROOT}")
    endif()
    if(NOT _SJIT_SKIA_OUT AND DEFINED ENV{SKIA_OUT})
        set(_SJIT_SKIA_OUT "$ENV{SKIA_OUT}")
    endif()
    if(_SJIT_SKIA_ROOT AND NOT _SJIT_SKIA_OUT)
        set(_SJIT_SKIA_OUT "${_SJIT_SKIA_ROOT}/out/Static")
    endif()

    # An explicit GN tree is authoritative. Do not combine its headers with a
    # package from a different Skia revision: Skia exposes a C++ ABI.
    if(_SJIT_SKIA_ROOT OR _SJIT_SKIA_OUT)
        if(NOT _SJIT_SKIA_ROOT OR NOT _SJIT_SKIA_OUT)
            message(FATAL_ERROR "SKIA_ROOT and SKIA_OUT must identify the same Skia GN build.")
        endif()
        unset(SKIA_INCLUDE_DIR CACHE)
        unset(SKIA_LIBRARY CACHE)
        find_path(SKIA_INCLUDE_DIR
            NAMES include/core/SkCanvas.h
            PATHS "${_SJIT_SKIA_ROOT}"
            NO_DEFAULT_PATH
        )
        find_library(SKIA_LIBRARY
            NAMES skia
            PATHS "${_SJIT_SKIA_OUT}"
            NO_DEFAULT_PATH
        )
        if(NOT SKIA_INCLUDE_DIR OR NOT SKIA_LIBRARY)
            message(FATAL_ERROR
                "The requested Skia GN build was not found under "
                "SKIA_ROOT=${_SJIT_SKIA_ROOT}, SKIA_OUT=${_SJIT_SKIA_OUT}."
            )
        endif()
        set(_SJIT_SKIA_GN_ARCHIVES "${SKIA_LIBRARY}")
        foreach(_SJIT_SKIA_AUX_NAME raw_ptr allocator_base allocator_core allocator_shim)
            set(_SJIT_SKIA_AUX_LIBRARY
                "${_SJIT_SKIA_OUT}/${CMAKE_STATIC_LIBRARY_PREFIX}${_SJIT_SKIA_AUX_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}")
            if(EXISTS "${_SJIT_SKIA_AUX_LIBRARY}")
                list(APPEND _SJIT_SKIA_GN_ARCHIVES "${_SJIT_SKIA_AUX_LIBRARY}")
            endif()
        endforeach()

        set(_SJIT_SKIA_GN_SYSTEM_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # GN brackets the complete static archive set so PartitionAlloc's
            # circular archive references are rescanned by the ELF linker.
            list(PREPEND _SJIT_SKIA_GN_ARCHIVES "-Wl,--start-group")
            list(APPEND _SJIT_SKIA_GN_ARCHIVES "-Wl,--end-group")
            find_package(Fontconfig REQUIRED)
            find_package(Freetype REQUIRED)
            find_package(OpenGL REQUIRED)
            set(_SJIT_SKIA_GN_SYSTEM_LIBRARIES
                Threads::Threads
                Fontconfig::Fontconfig
                ${CMAKE_DL_LIBS}
                Freetype::Freetype
                OpenGL::GL
            )
        endif()

        add_library(sjit_skia_external INTERFACE IMPORTED)
        set_target_properties(sjit_skia_external PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SKIA_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_SJIT_SKIA_GN_ARCHIVES};${_SJIT_SKIA_GN_SYSTEM_LIBRARIES}"
        )
        set(SJIT_SKIA_TARGET sjit_skia_external)
    else()
        find_package(Skia CONFIG QUIET)
        foreach(candidate Skia::Skia skia::skia skia)
            if(TARGET ${candidate})
                set(SJIT_SKIA_TARGET ${candidate})
                break()
            endif()
        endforeach()

        if(NOT SJIT_SKIA_TARGET)
            find_package(unofficial-skia CONFIG QUIET)
            if(TARGET unofficial::skia::skia)
                set(SJIT_SKIA_TARGET unofficial::skia::skia)
            endif()
        endif()

        if(NOT SJIT_SKIA_TARGET)
            pkg_check_modules(SKIA_PC QUIET skia)
            if(SKIA_PC_FOUND)
                add_library(sjit_skia_pkg INTERFACE)
                target_include_directories(sjit_skia_pkg INTERFACE ${SKIA_PC_INCLUDE_DIRS})
                target_compile_options(sjit_skia_pkg INTERFACE ${SKIA_PC_CFLAGS_OTHER})
                target_link_directories(sjit_skia_pkg INTERFACE ${SKIA_PC_STATIC_LIBRARY_DIRS})
                if(SKIA_PC_STATIC_LINK_LIBRARIES)
                    target_link_libraries(sjit_skia_pkg INTERFACE ${SKIA_PC_STATIC_LINK_LIBRARIES})
                    target_link_options(sjit_skia_pkg INTERFACE ${SKIA_PC_STATIC_LDFLAGS_OTHER})
                else()
                    target_link_libraries(sjit_skia_pkg INTERFACE ${SKIA_PC_LINK_LIBRARIES})
                    target_link_options(sjit_skia_pkg INTERFACE ${SKIA_PC_LDFLAGS_OTHER})
                endif()
                set(SJIT_SKIA_TARGET sjit_skia_pkg)
            endif()
        endif()
    endif()

    if(NOT SJIT_SKIA_TARGET)
        message(FATAL_ERROR
            "Skia was not found. Set SKIA_ROOT and SKIA_OUT to a GN build, "
            "provide a Skia package, or configure with -DSJIT_BUILD_HOST=OFF."
        )
    endif()
endif()

llvm_map_components_to_libnames(LLVM_LIBS
    Core
    ExecutionEngine
    OrcJIT
    Linker
    Native
    Support
)

add_library(sjit_runtime STATIC ${RUNTIME_C_SOURCES} ${QUICKJS_SUBSET_C_SOURCES})
target_include_directories(sjit_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs_subset
)
target_link_libraries(sjit_runtime PUBLIC Threads::Threads)
if(SJIT_PROFILE_RUNTIME)
    # This definition changes the tail layout of SRuntime and must therefore
    # propagate to every C/C++ consumer as well as the separately built LLVM
    # runtime bitcode below.
    target_compile_definitions(sjit_runtime PUBLIC SJIT_PROFILE_RUNTIME)
endif()

add_library(xyo_core STATIC ${CORE_CXX_SOURCES})
target_include_directories(xyo_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(xyo_core PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(xyo_core PUBLIC sjit_runtime ${LLVM_LIBS} ZLIB::ZLIB)

if(SJIT_BUILD_HOST)
    add_library(sjit_skia_pen STATIC src/skia_pen_layer.cpp)
    target_include_directories(sjit_skia_pen PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SKIA_INCLUDE_DIR}
    )
    target_link_libraries(sjit_skia_pen PUBLIC
        ${SJIT_SKIA_TARGET}
        ${SKIA_EXTRA_LIBRARIES}
    )

    add_library(xyo_host STATIC ${HOST_CXX_SOURCES})
    target_include_directories(xyo_host PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SDL2_INCLUDE_DIRS}
        ${RSVG_INCLUDE_DIRS}
    )
    target_compile_options(xyo_host PUBLIC ${SDL2_CFLAGS_OTHER} ${RSVG_CFLAGS_OTHER})
    target_link_libraries(xyo_host PUBLIC xyo_core PRIVATE
        sjit_skia_pen
        ${SDL2_LIBRARIES}
        ${RSVG_LIBRARIES}
    )
    target_link_directories(xyo_host PUBLIC ${SDL2_LIBRARY_DIRS} ${RSVG_LIBRARY_DIRS})

    add_executable(scratch-llvm-vm src/main.cpp)
    target_link_libraries(scratch-llvm-vm PRIVATE xyo_host)
endif()

find_program(CLANG_EXECUTABLE clang HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
find_program(LLVM_LINK_EXECUTABLE llvm-link HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
find_program(LLVM_DIS_EXECUTABLE llvm-dis HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
find_program(LLC_EXECUTABLE llc HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)

set(SJIT_RUNTIME_BITCODE_COMPILE_FLAGS)
if(SJIT_PROFILE_RUNTIME)
    list(APPEND SJIT_RUNTIME_BITCODE_COMPILE_FLAGS -DSJIT_PROFILE_RUNTIME)
endif()

set(RUNTIME_BC_OBJECTS)
set(RUNTIME_BITCODE_C_SOURCES ${RUNTIME_C_SOURCES} ${QUICKJS_SUBSET_C_SOURCES})
# Keep persistent pthread worker code in the host image.  The LLVM scheduler
# calls these as external symbols explicitly attested by JitEngine, so worker
# instruction pointers never depend on an unloadable ORC module's lifetime.
list(REMOVE_ITEM RUNTIME_BITCODE_C_SOURCES runtime/sjit_thread_pool.c)
file(GLOB RUNTIME_BITCODE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/sjit/*.hpp"
)
foreach(runtime_src IN LISTS RUNTIME_BITCODE_C_SOURCES)
    get_filename_component(runtime_name "${runtime_src}" NAME_WE)
    get_filename_component(runtime_dir "${runtime_src}" DIRECTORY)
    string(REPLACE "/" "_" runtime_bc_dir "${runtime_dir}")
    set(runtime_bc "${CMAKE_CURRENT_BINARY_DIR}/runtime_bc/${runtime_bc_dir}/${runtime_name}.bc")
    add_custom_command(
        OUTPUT "${runtime_bc}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/runtime_bc/${runtime_bc_dir}"
        COMMAND ${CLANG_EXECUTABLE}
            -std=c11
            -O2
            -pthread
            ${SJIT_RUNTIME_BITCODE_COMPILE_FLAGS}
            -I${CMAKE_CURRENT_SOURCE_DIR}/runtime
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/quickjs
            -I${CMAKE_CURRENT_SOURCE_DIR}/quickjs_subset
            -emit-llvm
            -c "${CMAKE_CURRENT_SOURCE_DIR}/${runtime_src}"
            -o "${runtime_bc}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${runtime_src}" ${RUNTIME_BITCODE_HEADERS}
        VERBATIM
    )
    list(APPEND RUNTIME_BC_OBJECTS "${runtime_bc}")
endforeach()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.bc"
    COMMAND ${LLVM_LINK_EXECUTABLE} ${RUNTIME_BC_OBJECTS}
        -o "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.bc"
    DEPENDS ${RUNTIME_BC_OBJECTS}
    VERBATIM
)

add_custom_target(runtime-bitcode ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.bc"
)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.o"
    COMMAND ${LLC_EXECUTABLE}
        -O=3
        -filetype=obj
        -relocation-model=pic
        "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.bc"
        -o "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.o"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.bc"
    VERBATIM
)

add_custom_target(runtime-object ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.o"
)

if(SJIT_BUILD_HOST)
    add_dependencies(scratch-llvm-vm runtime-object)
endif()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.ll"
    COMMAND ${LLVM_DIS_EXECUTABLE}
        "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.bc"
        -o "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.ll"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.bc"
    VERBATIM
)

add_custom_target(runtime-ll
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sjit_runtime.ll"
)

enable_testing()

add_executable(xyo-runtime-tests tests/unit/runtime_tests.cpp)
add_dependencies(xyo-runtime-tests runtime-object)
target_include_directories(xyo-runtime-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(xyo-runtime-tests PRIVATE xyo_core)
add_test(NAME xyo-runtime-tests COMMAND xyo-runtime-tests)

if(SJIT_BUILD_HOST)
    add_executable(xyo-skia-pen-tests tests/unit/skia_pen_layer_tests.cpp)
    target_link_libraries(xyo-skia-pen-tests PRIVATE sjit_skia_pen)
    add_test(NAME xyo-skia-pen-tests COMMAND xyo-skia-pen-tests)
endif()
