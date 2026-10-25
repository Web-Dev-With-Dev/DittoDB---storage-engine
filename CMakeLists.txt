cmake_minimum_required(VERSION 3.20)

project(DittoDB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

# ==================== ThreadSanitizer (TSan) ====================
option(ENABLE_TSAN "Enable ThreadSanitizer (-fsanitize=thread)" OFF)

if(ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(WIN32)
            message(WARNING "ThreadSanitizer is not supported on Windows/MinGW. Ignoring ENABLE_TSAN.")
        else()
            message(STATUS "Enabling ThreadSanitizer (-fsanitize=thread)")
            add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
            add_link_options(-fsanitize=thread)
        endif()
    endif()
endif()

# ==================== Include Directories ====================
include_directories(include)

# ==================== Source Files ====================
set(COMMON_SOURCES
    src/lru_k_replacer.cpp
    src/page.cpp
    src/disk_manager.cpp
    src/buffer_pool_manager.cpp
    src/table_page.cpp
    src/table_heap.cpp
    src/extendible_htable_directory_page.cpp
)

set(COMMON_HEADERS
    include/config.h
    include/lru_k_replacer.h
    include/page.h
    include/disk_manager.h
    include/buffer_pool_manager.h
    include/rid.h
    include/tuple.h
    include/table_page.h
    include/table_heap.h
    include/extendible_htable_directory_page.h
    include/extendible_htable_bucket_page.h
    include/extendible_hash_table.h
)

# ==================== Main Executable ====================
add_executable(dittodb
    src/main.cpp
    ${COMMON_SOURCES}
)

# Compatibility alias for database target
add_executable(database
    src/main.cpp
    ${COMMON_SOURCES}
)

# ==================== Testing Setup ====================
enable_testing()

# Find Google Test (use system package if found, otherwise auto-fetch via FetchContent)
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# Create a library with common sources for tests
add_library(db_common STATIC
    ${COMMON_SOURCES}
)

target_include_directories(db_common PUBLIC
    include
)

# ==================== Test Executables ====================

# 1. LRU-K Replacer Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/lru_k_replacer_test.cpp)
    message(STATUS "Found lru_k_replacer_test.cpp")
    add_executable(lru_k_replacer_test
        tests/lru_k_replacer_test.cpp
    )
    
    target_link_libraries(lru_k_replacer_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(lru_k_replacer_test PRIVATE
        include
    )
    
    gtest_discover_tests(lru_k_replacer_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "lru_k_replacer_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/lru_k_replacer_test.cpp")
endif()

# 2. Page Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/page_test.cpp)
    message(STATUS "Found page_test.cpp")
    add_executable(page_test
        tests/page_test.cpp
    )
    
    target_link_libraries(page_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(page_test PRIVATE
        include
    )
    
    gtest_discover_tests(page_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "page_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/page_test.cpp")
endif()

# 3. Disk Manager Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/disk_manager_test.cpp)
    message(STATUS "Found disk_manager_test.cpp")
    add_executable(disk_manager_test
        tests/disk_manager_test.cpp
    )
    
    target_link_libraries(disk_manager_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(disk_manager_test PRIVATE
        include
    )
    
    gtest_discover_tests(disk_manager_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "disk_manager_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/disk_manager_test.cpp")
endif()

# 4. Integration Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.cpp)
    message(STATUS "Found integration_test.cpp")
    add_executable(integration_test
        tests/integration_test.cpp
    )
    
    target_link_libraries(integration_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(integration_test PRIVATE
        include
    )
    
    gtest_discover_tests(integration_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "integration_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.cpp")
endif()

# 5. Buffer Pool Manager Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/buffer_pool_manager_test.cpp)
    message(STATUS "Found buffer_pool_manager_test.cpp")
    add_executable(buffer_pool_manager_test
        tests/buffer_pool_manager_test.cpp
    )
    
    target_link_libraries(buffer_pool_manager_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(buffer_pool_manager_test PRIVATE
        include
    )
    
    gtest_discover_tests(buffer_pool_manager_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "buffer_pool_manager_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/buffer_pool_manager_test.cpp")
endif()

# 6. Table Page Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_page_test.cpp)
    message(STATUS "Found table_page_test.cpp")
    add_executable(table_page_test
        tests/table_page_test.cpp
    )
    
    target_link_libraries(table_page_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(table_page_test PRIVATE
        include
    )
    
    gtest_discover_tests(table_page_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "table_page_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_page_test.cpp")
endif()

# 7. RID Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/rid_test.cpp)
    message(STATUS "Found rid_test.cpp")
    add_executable(rid_test
        tests/rid_test.cpp
    )
    
    target_link_libraries(rid_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(rid_test PRIVATE
        include
    )
    
    gtest_discover_tests(rid_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "rid_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/rid_test.cpp")
endif()

# 8. Table Heap Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_heap_test.cpp)
    message(STATUS "Found table_heap_test.cpp")
    add_executable(table_heap_test
        tests/table_heap_test.cpp
    )
    
    target_link_libraries(table_heap_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(table_heap_test PRIVATE
        include
    )
    
    gtest_discover_tests(table_heap_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "table_heap_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/table_heap_test.cpp")
endif()

# 9. Extendible Hash Table Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/extendible_hash_table_test.cpp)
    message(STATUS "Found extendible_hash_table_test.cpp")
    add_executable(extendible_hash_table_test
        tests/extendible_hash_table_test.cpp
    )
    
    target_link_libraries(extendible_hash_table_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(extendible_hash_table_test PRIVATE
        include
    )
    
    gtest_discover_tests(extendible_hash_table_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "extendible_hash_table_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/extendible_hash_table_test.cpp")
endif()

# 10. End-to-End Storage Integration Test (DiskManager + LRUK + BPM + TablePage + TableHeap)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_storage_test.cpp)
    message(STATUS "Found e2e_storage_test.cpp")
    add_executable(e2e_storage_test
        tests/e2e_storage_test.cpp
    )
    
    target_link_libraries(e2e_storage_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(e2e_storage_test PRIVATE
        include
    )
    
    gtest_discover_tests(e2e_storage_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "e2e_storage_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_storage_test.cpp")
endif()

# 11. Buffer Pool Manager Concurrent Stress Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/buffer_pool_manager_concurrent_test.cpp)
    message(STATUS "Found buffer_pool_manager_concurrent_test.cpp")
    add_executable(buffer_pool_manager_concurrent_test
        tests/buffer_pool_manager_concurrent_test.cpp
    )
    
    target_link_libraries(buffer_pool_manager_concurrent_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(buffer_pool_manager_concurrent_test PRIVATE
        include
    )
    
    gtest_discover_tests(buffer_pool_manager_concurrent_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "buffer_pool_manager_concurrent_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/buffer_pool_manager_concurrent_test.cpp")
endif()

# 12. Common Page Header Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/common_page_header_test.cpp)
    message(STATUS "Found common_page_header_test.cpp")
    add_executable(common_page_header_test
        tests/common_page_header_test.cpp
    )
    
    target_link_libraries(common_page_header_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(common_page_header_test PRIVATE
        include
    )
    
    gtest_discover_tests(common_page_header_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "common_page_header_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/common_page_header_test.cpp")
endif()

# 13. B+ Tree Internal Page Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/b_plus_tree_internal_page_test.cpp)
    message(STATUS "Found b_plus_tree_internal_page_test.cpp")
    add_executable(b_plus_tree_internal_page_test
        tests/b_plus_tree_internal_page_test.cpp
    )
    
    target_link_libraries(b_plus_tree_internal_page_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(b_plus_tree_internal_page_test PRIVATE
        include
    )
    
    gtest_discover_tests(b_plus_tree_internal_page_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "b_plus_tree_internal_page_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/b_plus_tree_internal_page_test.cpp")
endif()

# 14. B+ Tree Leaf Page Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/b_plus_tree_leaf_page_test.cpp)
    message(STATUS "Found b_plus_tree_leaf_page_test.cpp")
    add_executable(b_plus_tree_leaf_page_test
        tests/b_plus_tree_leaf_page_test.cpp
    )
    
    target_link_libraries(b_plus_tree_leaf_page_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(b_plus_tree_leaf_page_test PRIVATE
        include
    )
    
    gtest_discover_tests(b_plus_tree_leaf_page_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "b_plus_tree_leaf_page_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/b_plus_tree_leaf_page_test.cpp")
endif()

# 15. Generic Key Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_key_test.cpp)
    message(STATUS "Found generic_key_test.cpp")
    add_executable(generic_key_test
        tests/generic_key_test.cpp
    )
    
    target_link_libraries(generic_key_test PRIVATE
        db_common
        GTest::gtest_main
    )
    
    target_include_directories(generic_key_test PRIVATE
        include
    )
    
    gtest_discover_tests(generic_key_test
        DISCOVERY_MODE PRE_TEST
    )
else()
    message(WARNING "generic_key_test.cpp not found at ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_key_test.cpp")
endif()

# ==================== Test Runner Targets ====================

# Run LRU-K tests specifically
add_custom_target(run_lru_k_test
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo "Running LRU-K Replacer Tests"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ./lru_k_replacer_test
    DEPENDS lru_k_replacer_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running LRU-K replacer tests"
)

# Run all tests
add_custom_target(run_all_tests
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo "Running ALL Tests"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "1. Running LRU-K Replacer Tests..."
    COMMAND ./lru_k_replacer_test || echo "LRU-K tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "2. Running Page Tests..."
    COMMAND ./page_test || echo "Page tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "3. Running Disk Manager Tests..."
    COMMAND ./disk_manager_test || echo "Disk Manager tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "4. Running Integration Tests..."
    COMMAND ./integration_test || echo "Integration tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "5. Running Buffer Pool Manager Tests..."
    COMMAND ./buffer_pool_manager_test || echo "Buffer Pool Manager tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "6. Running Table Page Tests..."
    COMMAND ./table_page_test || echo "Table Page tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "7. Running RID Tests..."
    COMMAND ./rid_test || echo "RID tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "8. Running Table Heap Tests..."
    COMMAND ./table_heap_test || echo "Table Heap tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "9. Running Extendible Hash Table Tests..."
    COMMAND ./extendible_hash_table_test || echo "Extendible Hash Table tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "10. Running End-to-End Storage Integration Tests..."
    COMMAND ./e2e_storage_test || echo "E2E Storage tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "11. Running Buffer Pool Manager Concurrent Stress Tests..."
    COMMAND ./buffer_pool_manager_concurrent_test || echo "BPM Concurrent tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "12. Running Common Page Header Tests..."
    COMMAND ./common_page_header_test || echo "Common Page Header tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "13. Running B+ Tree Internal Page Tests..."
    COMMAND ./b_plus_tree_internal_page_test || echo "B+ Tree Internal Page tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "14. Running B+ Tree Leaf Page Tests..."
    COMMAND ./b_plus_tree_leaf_page_test || echo "B+ Tree Leaf Page tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "15. Running Generic Key and Serialization Tests..."
    COMMAND ./generic_key_test || echo "Generic Key tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo "All tests completed"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    DEPENDS 
        lru_k_replacer_test
        page_test
        disk_manager_test
        integration_test
        buffer_pool_manager_test
        table_page_test
        rid_test
        table_heap_test
        extendible_hash_table_test
        e2e_storage_test
        buffer_pool_manager_concurrent_test
        common_page_header_test
        b_plus_tree_internal_page_test
        b_plus_tree_leaf_page_test
        generic_key_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all tests"
)

# ==================== Code Coverage (Optional) ====================
option(ENABLE_COVERAGE "Enable coverage reporting" OFF)

if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "Enabling code coverage")
        target_compile_options(lru_k_replacer_test PRIVATE --coverage)
        target_link_options(lru_k_replacer_test PRIVATE --coverage)
        
        target_compile_options(page_test PRIVATE --coverage)
        target_link_options(page_test PRIVATE --coverage)
        
        target_compile_options(disk_manager_test PRIVATE --coverage)
        target_link_options(disk_manager_test PRIVATE --coverage)
        
        target_compile_options(integration_test PRIVATE --coverage)
        target_link_options(integration_test PRIVATE --coverage)
        
        # Add a target for generating coverage report
        find_program(GCOVR gcovr)
        if(GCOVR)
            add_custom_target(coverage
                COMMAND ${GCOVR} --root .. --html --output coverage.html
                COMMAND ${CMAKE_COMMAND} -E echo "Coverage report generated at coverage.html"
                DEPENDS run_all_tests
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating coverage report"
            )
        endif()
    endif()
endif()

# ==================== Verbose Output Target ====================
add_custom_target(show_tests
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo "Available Test Executables:"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo "  1. lru_k_replacer_test.exe - LRU-K Replacer unit tests"
    COMMAND ${CMAKE_COMMAND} -E echo "  2. page_test.exe - Page management tests"
    COMMAND ${CMAKE_COMMAND} -E echo "  3. disk_manager_test.exe - Disk I/O tests"
    COMMAND ${CMAKE_COMMAND} -E echo "  4. integration_test.exe - Integration tests"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
    COMMAND ${CMAKE_COMMAND} -E echo "Commands:"
    COMMAND ${CMAKE_COMMAND} -E echo "  make run_lru_k_test    - Run LRU-K tests only"
    COMMAND ${CMAKE_COMMAND} -E echo "  make run_all_tests     - Run all tests"
    COMMAND ${CMAKE_COMMAND} -E echo "  make show_tests        - Show this message"
    COMMAND ${CMAKE_COMMAND} -E echo "========================================"
)