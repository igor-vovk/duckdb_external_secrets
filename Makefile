PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=external_secrets
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

BUILD_THREADS ?= $(shell sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)
export CMAKE_BUILD_PARALLEL_LEVEL ?= $(BUILD_THREADS)
ENABLE_EXTENSION_AUTOLOADING ?= 1
ENABLE_EXTENSION_AUTOINSTALL ?= 1
FMT_UV_DEPS := --with 'black>=24' --with 'clang-format==11.0.1' --with 'cmake-format'

.PHONY: fmt
fmt:
	uv run --isolated $(FMT_UV_DEPS) python duckdb/scripts/format.py --all --fix --noconfirm --directories src test

TEST_ENV_MAKEFILE ?= $(PROJ_DIR)test/local_test_env.mk
TEST_GOALS := test test_release test_release_internal test_debug test_debug_internal test_reldebug test_reldebug_internal
ifneq ($(filter $(TEST_GOALS),$(MAKECMDGOALS)),)
-include $(TEST_ENV_MAKEFILE)
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
CLT_LIBCXX_HEADER := /Library/Developer/CommandLineTools/usr/include/c++/v1/type_traits
ifeq ($(wildcard $(CLT_LIBCXX_HEADER)),)
MACOS_SDK_PATH := $(shell xcrun --show-sdk-path 2>/dev/null)
SDK_LIBCXX_HEADER := $(MACOS_SDK_PATH)/usr/include/c++/v1/type_traits
ifneq ($(wildcard $(SDK_LIBCXX_HEADER)),)
MACOS_SDK_CXX_FLAGS := -isysroot $(MACOS_SDK_PATH) -stdlib=libc++ -I$(MACOS_SDK_PATH)/usr/include/c++/v1
EXT_FLAGS += -DCMAKE_CXX_FLAGS='$(MACOS_SDK_CXX_FLAGS)'
endif
endif
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
