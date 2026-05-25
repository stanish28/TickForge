cmake_minimum_required(VERSION 3.20)

project(TickForge
    VERSION 0.1.0
    DESCRIPTION "Deterministic C++20 market data replay and limit order book engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TICKFORGE_BUILD_TESTS "Build TickForge unit tests" ON)
option(TICKFORGE_BUILD_BENCHMARKS "Build TickForge benchmark binary" ON)

if(APPLE)
    # Some Command Line Tools installs expose libc++ headers only through the SDK path.
    # This fallback is Apple-only and leaves the normal Linux include path untouched.
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE TICKFORGE_MACOS_SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    set(TICKFORGE_MACOS_LIBCXX "${TICKFORGE_MACOS_SDK}/usr/include/c++/v1")
    if(EXISTS "${TICKFORGE_MACOS_LIBCXX}/cstddef")
        include_directories(SYSTEM BEFORE "${TICKFORGE_MACOS_LIBCXX}")
    endif()
endif()

add_library(tickforge_core
    src/order_book.cpp
    src/replay_engine.cpp
    src/csv_parser.cpp
    src/latency_profiler.cpp
    src/benchmark_runner.cpp
)

target_include_directories(tickforge_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(tickforge_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(tickforge_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(tickforge_replay src/main.cpp)
target_link_libraries(tickforge_replay PRIVATE tickforge_core)

if(MSVC)
    target_compile_options(tickforge_replay PRIVATE /W4 /permissive-)
else()
    target_compile_options(tickforge_replay PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

if(TICKFORGE_BUILD_TESTS AND BUILD_TESTING)
    include(FetchContent)

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(tickforge_tests
        tests/test_order_book.cpp
        tests/test_replay_determinism.cpp
        tests/test_spsc_ring_buffer.cpp
        tests/test_csv_parser.cpp
    )

    target_link_libraries(tickforge_tests
        PRIVATE
            tickforge_core
            GTest::gtest_main
    )

    if(MSVC)
        target_compile_options(tickforge_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tickforge_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    include(GoogleTest)
    gtest_discover_tests(tickforge_tests)
endif()

if(TICKFORGE_BUILD_BENCHMARKS)
    add_executable(tickforge_bench
        benchmarks/bench_order_book.cpp
        benchmarks/bench_ring_buffer.cpp
        benchmarks/bench_replay.cpp
    )

    target_link_libraries(tickforge_bench PRIVATE tickforge_core)

    if(MSVC)
        target_compile_options(tickforge_bench PRIVATE /W4 /permissive-)
    else()
        target_compile_options(tickforge_bench PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
