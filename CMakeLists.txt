# ============================================================================
# KVault — Root CMake Configuration
# ============================================================================
#
# Targets:
#   kvault_engine  — Static library containing the LSM-Tree storage engine
#   kvault_tests   — GoogleTest executable for all unit tests
#
# Usage:
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build --parallel
#   cd build && ctest --output-on-failure
#
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(kvault
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "A high-performance LSM-Tree Key-Value Store"
)

# Generate compile_commands.json for IDE IntelliSense (removes false positive syntax errors)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----------------------------------------------------------------------------
# Compiler Settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)           # No GNU extensions — strict ISO C++20

# ---------------------------------------------------------------------------
# Compiler Warnings (treat warnings as quality signals, not errors by default)
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

# ============================================================================
# Dependencies — GoogleTest via FetchContent
# ============================================================================
# FetchContent downloads and builds GoogleTest as part of the project.
# No manual installation required — interviewers can clone and build in
# one step: `cmake -B build && cmake --build build`
# ============================================================================
include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)

# Prevent GoogleTest from overriding the parent project's compiler/linker
# settings on Windows (critical for MSVC builds).
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# --- Asio (Required by Crow) ---
FetchContent_Declare(
    asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
)
FetchContent_MakeAvailable(asio)

# --- Crow (HTTP REST Framework) ---
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    Crow
    URL https://github.com/CrowCpp/Crow/archive/refs/tags/v1.2.0.tar.gz
)
# Force Crow to use our fetched ASIO instead of system ASIO
set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include CACHE PATH "" FORCE)
FetchContent_MakeAvailable(Crow)

# --- Static Library: kvault_engine ---
# Contains the core engine logic (MemTable, WAL, SSTable, KVStore)
add_library(kvault_engine STATIC
    src/skiplist.cpp
    src/memtable.cpp
    src/wal.cpp
    src/bloom_filter.cpp
    src/sstable_writer.cpp
    src/sstable_reader.cpp
    src/sstable_manager.cpp
    src/kvstore.cpp
    src/api_routes.cpp
)

target_include_directories(kvault_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Link Crow to the engine (since api_routes uses it)
target_link_libraries(kvault_engine PUBLIC Crow::Crow)

if(WIN32)
    target_link_libraries(kvault_engine PUBLIC ws2_32 mswsock)
endif()

# --- Executable: kvault_server ---
add_executable(kvault_server
    src/main.cpp
)

target_link_libraries(kvault_server
    PRIVATE
        kvault_engine
)

target_compile_features(kvault_engine PUBLIC cxx_std_20)

# ============================================================================
# Testing
# ============================================================================
enable_testing()
add_subdirectory(tests)
