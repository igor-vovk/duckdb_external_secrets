cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME waddle)

# Newer Apple Command Line Tools releases can place libc++ headers only inside the SDK.
# When that happens, add the SDK header path explicitly so DuckDB and the extension can compile.
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(CLT_LIBCXX_HEADER "/Library/Developer/CommandLineTools/usr/include/c++/v1/type_traits")
    if(NOT EXISTS "${CLT_LIBCXX_HEADER}")
        execute_process(
            COMMAND xcrun --show-sdk-path
            OUTPUT_VARIABLE CMAKE_DARWIN_SDK_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(SDK_LIBCXX_INCLUDE_DIR "${CMAKE_DARWIN_SDK_PATH}/usr/include/c++/v1")
        if(EXISTS "${SDK_LIBCXX_INCLUDE_DIR}/type_traits")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isysroot ${CMAKE_DARWIN_SDK_PATH} -stdlib=libc++ -I${SDK_LIBCXX_INCLUDE_DIR}")
        endif()
    endif()
endif()

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/waddle_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
