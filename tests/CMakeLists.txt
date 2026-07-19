# ============================================================================
# tests/CMakeLists.txt — Unit Test Configuration
# ============================================================================

add_executable(kvault_tests
    test_skiplist.cpp
    test_memtable.cpp
    test_wal.cpp
    test_sstable.cpp
    test_kvstore_integration.cpp
)

target_link_libraries(kvault_tests
    PRIVATE
        kvault_engine
        GTest::gtest_main      # Provides main() — no need to write our own
)

# Automatically discover and register all TEST() / TEST_F() macros with CTest.
# Tests can be run via `ctest` or directly via `./kvault_tests`.
include(GoogleTest)
gtest_discover_tests(kvault_tests)
