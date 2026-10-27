# Convenience targets around CMake. The build system remains CMake; this Makefile
# only configures build/ and invokes cmake/ctest/clang-format from the tree root.

BUILD_DIR := build
CMAKE := cmake
CLANG_FORMAT ?= clang-format
FORMAT_FILES := SimpleIni.h $(wildcard tests/*.cpp)

CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Debug

JOBS := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all help configure build test format format-check check clean

all: build

help:
	@echo "SimpleIni developer targets:"
	@echo "  make configure     Configure the CMake build in $(BUILD_DIR)/"
	@echo "  make build         Build tests (warnings are errors)"
	@echo "  make test          Run tests via ctest"
	@echo "  make format        Apply clang-format to sources"
	@echo "  make format-check  Verify formatting (no writes)"
	@echo "  make check         format-check, build, and test"
	@echo "  make clean         Remove $(BUILD_DIR)/"

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS)

test: build
	$(CMAKE) -E chdir $(BUILD_DIR) ctest --output-on-failure

format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 \
		|| { echo "error: $(CLANG_FORMAT) not found; install clang-format" >&2; exit 1; }
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 \
		|| { echo "error: $(CLANG_FORMAT) not found; install clang-format" >&2; exit 1; }
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

check: format-check test

clean:
	rm -rf $(BUILD_DIR)
