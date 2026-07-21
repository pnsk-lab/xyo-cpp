CC = clang
CXX = clang++
LLVM_CONFIG ?= llvm-config
LLVM_LINK ?= llvm-link
LLVM_DIS ?= llvm-dis
LLC ?= llc
PKG_CONFIG ?= pkg-config

BUILD_DIR := build
RUNTIME_C := $(wildcard runtime/*.c)
# Native host uses the pthread pool; web stub is only for Emscripten builds.
RUNTIME_C := $(filter-out runtime/sjit_thread_pool_web.c,$(RUNTIME_C))
RUNTIME_HEADERS := $(wildcard runtime/*.h) $(wildcard include/sjit/*.hpp)
QUICKJS_SUBSET_C := quickjs/cutils.c quickjs/dtoa.c quickjs_subset/sjit_quickjs_string.c
CORE_CXX := src/compiler.cpp src/jit.cpp src/project_loader.cpp
HOST_CXX := src/host_app.cpp src/skia_pen_layer.cpp
APP_CXX := src/main.cpp
TEST_CXX := tests/unit/runtime_tests.cpp
SKIA_TEST_CXX := tests/unit/skia_pen_layer_tests.cpp

RUNTIME_OBJS := $(patsubst runtime/%.c,$(BUILD_DIR)/runtime/%.o,$(RUNTIME_C))
QUICKJS_SUBSET_OBJS := $(patsubst quickjs/%.c,$(BUILD_DIR)/quickjs/%.o,$(filter quickjs/%.c,$(QUICKJS_SUBSET_C))) \
	$(patsubst quickjs_subset/%.c,$(BUILD_DIR)/quickjs_subset/%.o,$(filter quickjs_subset/%.c,$(QUICKJS_SUBSET_C)))
CORE_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/src/%.o,$(CORE_CXX))
HOST_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/src/%.o,$(HOST_CXX))
APP_OBJ := $(patsubst src/%.cpp,$(BUILD_DIR)/src/%.o,$(APP_CXX))
TEST_OBJS := $(patsubst tests/unit/%.cpp,$(BUILD_DIR)/tests/unit/%.o,$(TEST_CXX))
SKIA_TEST_OBJS := $(patsubst tests/unit/%.cpp,$(BUILD_DIR)/tests/unit/%.o,$(SKIA_TEST_CXX))
# The persistent pthread pool must live in the host image rather than in an
# unloadable ORC module: idle worker instruction pointers then remain valid
# even if a JitEngine is destroyed before its runtime.  The LLVM scheduler
# keeps external declarations which JitEngine binds to these native symbols.
RUNTIME_BC_C := $(filter-out runtime/sjit_thread_pool.c,$(RUNTIME_C))
RUNTIME_BC := $(patsubst runtime/%.c,$(BUILD_DIR)/runtime_bc/%.bc,$(RUNTIME_BC_C))
QUICKJS_SUBSET_BC := $(patsubst quickjs/%.c,$(BUILD_DIR)/quickjs_bc/%.bc,$(filter quickjs/%.c,$(QUICKJS_SUBSET_C))) \
	$(patsubst quickjs_subset/%.c,$(BUILD_DIR)/quickjs_subset_bc/%.bc,$(filter quickjs_subset/%.c,$(QUICKJS_SUBSET_C)))
DEPS := $(RUNTIME_OBJS:.o=.d) $(QUICKJS_SUBSET_OBJS:.o=.d) $(CORE_OBJS:.o=.d) \
	$(HOST_OBJS:.o=.d) $(APP_OBJ:.o=.d) $(TEST_OBJS:.o=.d) $(SKIA_TEST_OBJS:.o=.d)

CPPFLAGS := -Iinclude -Iruntime -Iquickjs -Iquickjs_subset
DEPFLAGS := -MMD -MP
OPTFLAGS ?= -O2 -DNDEBUG
THREAD_FLAGS := -pthread
CFLAGS := $(OPTFLAGS) $(THREAD_FLAGS) -std=c11 -Wall -Wextra -Wpedantic
LLVM_CXXFLAGS := $(filter-out -fno-exceptions,$(shell $(LLVM_CONFIG) --cxxflags))
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LDFLAGS := $(shell $(PKG_CONFIG) --libs sdl2)
SVG_CFLAGS := $(shell $(PKG_CONFIG) --cflags librsvg-2.0)
SVG_LDFLAGS := $(shell $(PKG_CONFIG) --libs librsvg-2.0)
DEFAULT_SKIA_ROOT := $(CURDIR)/.deps/skia
ifeq ($(wildcard $(DEFAULT_SKIA_ROOT)/include/core/SkCanvas.h),)
DEFAULT_SKIA_ROOT :=
endif
SKIA_ROOT ?= $(DEFAULT_SKIA_ROOT)
SKIA_OUT ?= $(if $(SKIA_ROOT),$(SKIA_ROOT)/out/Static,)
SKIA_ROOT_CFLAGS :=
SKIA_LIBRARY_DEP :=
SKIA_GN_AUX_LIBRARIES :=
SKIA_GN_ARCHIVE_GROUP_BEGIN :=
SKIA_GN_ARCHIVE_GROUP_END :=
SKIA_GN_SYSTEM_LDFLAGS :=
ifeq ($(strip $(SKIA_ROOT)),)
SKIA_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags skia 2>/dev/null)
SKIA_LDFLAGS ?= $(shell $(PKG_CONFIG) --static --libs skia 2>/dev/null)
else
SKIA_ROOT_CFLAGS := -isystem $(SKIA_ROOT)
SKIA_CFLAGS ?=
SKIA_LIBRARY_DEP := $(SKIA_OUT)/libskia.a
SKIA_GN_AUX_LIBRARIES := $(foreach library,raw_ptr allocator_base allocator_core allocator_shim harfbuzz expat png zlib jpeg webp wuffs dng_sdk piex,$(wildcard $(SKIA_OUT)/lib$(library).a))
ifeq ($(shell uname -s),Linux)
# PartitionAlloc's static archives contain circular references.  Match GN's
# executable link rule and let the linker rescan the complete archive set.
SKIA_GN_ARCHIVE_GROUP_BEGIN := -Wl,--start-group
SKIA_GN_ARCHIVE_GROUP_END := -Wl,--end-group
SKIA_GN_SYSTEM_LDFLAGS := -pthread -lfontconfig -ldl -lfreetype -lGL
else
SKIA_GN_SYSTEM_LDFLAGS := -pthread -ldl
endif
SKIA_LDFLAGS ?= $(SKIA_GN_ARCHIVE_GROUP_BEGIN) $(SKIA_LIBRARY_DEP) $(SKIA_GN_AUX_LIBRARIES) $(SKIA_GN_ARCHIVE_GROUP_END) $(SKIA_GN_SYSTEM_LDFLAGS)
endif
SKIA_EXTRA_LDFLAGS ?=
ifneq ($(strip $(SKIA_ROOT)),)
SKIA_ALL_CFLAGS := $(SKIA_ROOT_CFLAGS) $(SKIA_CFLAGS)
else
SKIA_ALL_CFLAGS := $(SKIA_CFLAGS)
endif
CORE_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --system-libs --libs core executionengine orcjit native support) -lz $(THREAD_FLAGS)
CXXFLAGS := $(OPTFLAGS) $(THREAD_FLAGS) $(LLVM_CXXFLAGS) -std=c++20 -Wall -Wextra -Wpedantic
HOST_LDFLAGS := $(CORE_LDFLAGS) $(SDL_LDFLAGS) $(SVG_LDFLAGS)

.PHONY: all test test-skia check-skia clean runtime-bitcode runtime-object runtime-ll

all: $(BUILD_DIR)/scratch-llvm-vm runtime-object

$(BUILD_DIR)/scratch-llvm-vm: $(RUNTIME_OBJS) $(QUICKJS_SUBSET_OBJS) $(CORE_OBJS) $(HOST_OBJS) $(APP_OBJ) $(SKIA_LIBRARY_DEP) $(SKIA_GN_AUX_LIBRARIES) $(BUILD_DIR)/sjit_runtime.o | check-skia
	@mkdir -p $(@D)
	$(CXX) $(RUNTIME_OBJS) $(QUICKJS_SUBSET_OBJS) $(CORE_OBJS) $(HOST_OBJS) $(APP_OBJ) $(HOST_LDFLAGS) $(SKIA_LDFLAGS) $(SKIA_EXTRA_LDFLAGS) $(LDFLAGS) -o $@

$(BUILD_DIR)/tests/xyo-runtime-tests: $(RUNTIME_OBJS) $(QUICKJS_SUBSET_OBJS) $(CORE_OBJS) $(TEST_OBJS) | $(BUILD_DIR)/sjit_runtime.o
	@mkdir -p $(@D)
	$(CXX) $^ $(CORE_LDFLAGS) $(LDFLAGS) -o $@

$(BUILD_DIR)/tests/xyo-skia-pen-tests: $(BUILD_DIR)/src/skia_pen_layer.o $(SKIA_TEST_OBJS) $(SKIA_LIBRARY_DEP) $(SKIA_GN_AUX_LIBRARIES) | check-skia
	@mkdir -p $(@D)
	$(CXX) $(BUILD_DIR)/src/skia_pen_layer.o $(SKIA_TEST_OBJS) $(SKIA_LDFLAGS) $(SKIA_EXTRA_LDFLAGS) $(LDFLAGS) -o $@

$(BUILD_DIR)/runtime/%.o: runtime/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/quickjs/%.o: quickjs/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/quickjs_subset/%.o: quickjs_subset/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src/host_app.o $(APP_OBJ): CXXFLAGS += $(SDL_CFLAGS) $(SVG_CFLAGS) $(SKIA_ALL_CFLAGS)
$(BUILD_DIR)/src/skia_pen_layer.o: CPPFLAGS := $(SKIA_ALL_CFLAGS) $(CPPFLAGS)
$(BUILD_DIR)/src/skia_pen_layer.o: | check-skia

ifneq ($(strip $(SKIA_LIBRARY_DEP)),)
$(SKIA_LIBRARY_DEP):
	@echo "Skia library not found: $@. Build the GN 'skia' target first." >&2
	@exit 1
endif

$(BUILD_DIR)/src/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/unit/%.o: tests/unit/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/runtime_bc/%.bc: runtime/%.c $(RUNTIME_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(THREAD_FLAGS) -std=c11 -emit-llvm -c $< -o $@

$(BUILD_DIR)/quickjs_bc/%.bc: quickjs/%.c $(RUNTIME_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(THREAD_FLAGS) -std=c11 -emit-llvm -c $< -o $@

$(BUILD_DIR)/quickjs_subset_bc/%.bc: quickjs_subset/%.c $(RUNTIME_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(OPTFLAGS) $(THREAD_FLAGS) -std=c11 -emit-llvm -c $< -o $@

runtime-bitcode: $(BUILD_DIR)/sjit_runtime.bc

$(BUILD_DIR)/sjit_runtime.bc: $(RUNTIME_BC) $(QUICKJS_SUBSET_BC)
	$(LLVM_LINK) $^ -o $@

runtime-object: $(BUILD_DIR)/sjit_runtime.o

$(BUILD_DIR)/sjit_runtime.o: $(BUILD_DIR)/sjit_runtime.bc
	$(LLC) -O=3 -filetype=obj -relocation-model=pic $< -o $@

runtime-ll: $(BUILD_DIR)/sjit_runtime.ll

$(BUILD_DIR)/sjit_runtime.ll: $(BUILD_DIR)/sjit_runtime.bc
	$(LLVM_DIS) $< -o $@

test: runtime-object $(BUILD_DIR)/tests/xyo-runtime-tests
	$(BUILD_DIR)/tests/xyo-runtime-tests

test-skia: $(BUILD_DIR)/tests/xyo-skia-pen-tests
	$(BUILD_DIR)/tests/xyo-skia-pen-tests

check-skia:
	@if [ -z "$(strip $(SKIA_LDFLAGS))" ]; then \
		echo "Skia not found. Set SKIA_ROOT and SKIA_OUT, or provide SKIA_CFLAGS and SKIA_LDFLAGS." >&2; \
		exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
