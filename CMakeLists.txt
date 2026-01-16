cmake_minimum_required(VERSION 3.20)
project(orderbook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type flag sets
set(CMAKE_CXX_FLAGS_DEBUG
    "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    CACHE STRING "Debug flags" FORCE)

set(CMAKE_CXX_FLAGS_RELEASE
    "-O3 -march=native -DNDEBUG"
    CACHE STRING "Release flags" FORCE)

# Benchmark: maximum performance, no sanitizers, full native codegen.
set(CMAKE_CXX_FLAGS_BENCHMARK
    "-O3 -march=native -DNDEBUG"
    CACHE STRING "Benchmark flags" FORCE)

# Validate: correctness replay with heap-allocation shim enabled.
set(CMAKE_CXX_FLAGS_VALIDATE
    "-O2 -DNDEBUG -DREPLAY_VALIDATE"
    CACHE STRING "Validate flags" FORCE)

# Profile: optimize with frame pointers for perf/flamegraph.
set(CMAKE_CXX_FLAGS_PROFILE
    "-O2 -g -fno-omit-frame-pointer"
    CACHE STRING "Profile flags" FORCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING
        "Build type (Debug|Release|Benchmark|Validate|Profile)" FORCE)
endif()

# Dependencies (fetched at configure time)
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.4
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest benchmark)

# Core library
add_library(orderbook_lib
    src/feed_handler.cpp
    src/order_book.cpp
    src/slab_allocator.cpp
    src/fill_simulator.cpp
    src/pnl_tracker.cpp
    src/replay_session.cpp
)
target_include_directories(orderbook_lib PUBLIC include/)
target_compile_options(orderbook_lib PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=return-type
    -Wno-unused-parameter
)

# Standalone replay binary
add_executable(replay src/main_replay.cpp)
target_link_libraries(replay orderbook_lib)

# Benchmarks
add_executable(bench_orderbook benchmarks/bench_orderbook.cpp)
target_link_libraries(bench_orderbook orderbook_lib benchmark::benchmark)

# Tests
enable_testing()

# Optional Arrow/Parquet dependency (for ParquetEventSource adapter)
find_package(Arrow QUIET)
find_package(Parquet QUIET)

add_executable(test_orderbook
    tests/test_book_invariants.cpp
    tests/fuzz_book.cpp
    tests/test_adapters.cpp
    adapters/csv_adapter.cpp
)
target_link_libraries(test_orderbook orderbook_lib GTest::gtest_main)
target_compile_options(test_orderbook PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(test_orderbook PRIVATE adapters/)

# Optionally compile Parquet adapter into the test binary if Arrow is found.
if(Arrow_FOUND AND Parquet_FOUND)
    target_sources(test_orderbook PRIVATE adapters/parquet_adapter.cpp)
    target_link_libraries(test_orderbook
        Arrow::arrow_shared Parquet::parquet_shared)
    target_compile_definitions(test_orderbook PRIVATE HAVE_ARROW=1)
endif()

include(GoogleTest)
gtest_discover_tests(test_orderbook)

# libFuzzer harness (only built when ENABLE_FUZZING=ON with Clang)
option(ENABLE_FUZZING "Build libFuzzer harness (requires Clang)" OFF)
if(ENABLE_FUZZING)
    add_executable(itch_parser_fuzz fuzz/itch_parser_fuzz.cpp)
    target_link_libraries(itch_parser_fuzz orderbook_lib)
    target_compile_options(itch_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(itch_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# Summary
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler   : ${CMAKE_CXX_COMPILER}")

# Python bindings (pybind11)
option(ORDERBOOK_PYTHON "Build pybind11 Python module" OFF)
if(ORDERBOOK_PYTHON)
    find_package(pybind11 REQUIRED)

    pybind11_add_module(orderbook
        src/python_bindings.cpp
        adapters/csv_adapter.cpp
    )
    target_link_libraries(orderbook PRIVATE orderbook_lib)
    target_include_directories(orderbook PRIVATE adapters/ include/)

    # Arrow/Parquet are optional for the Python module too.
    if(Arrow_FOUND AND Parquet_FOUND)
        target_sources(orderbook PRIVATE adapters/parquet_adapter.cpp)
        target_link_libraries(orderbook PRIVATE
            Arrow::arrow_shared Parquet::parquet_shared)
        target_compile_definitions(orderbook PRIVATE HAVE_ARROW=1)
    endif()
endif()
