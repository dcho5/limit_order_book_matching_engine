cmake_minimum_required(VERSION 3.20)
project(MatchingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -O2)

# ── Optimized engine (intrusive list + object pool) ───────────────────────────
add_library(engine_lib
  src/order_book.cpp
  src/matching_engine.cpp
)
target_include_directories(engine_lib PUBLIC src)

# ── Baseline engine (std::map + std::list, per-order heap alloc) ──────────────
add_library(baseline_lib
  src/baseline_book.cpp
)
target_include_directories(baseline_lib PUBLIC src)

# ── Tests ─────────────────────────────────────────────────────────────────────
add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE engine_lib)

# ── Benchmarks ────────────────────────────────────────────────────────────────

# Original quick benchmark (kept for reference)
add_executable(benchmark bench/benchmark.cpp)
target_link_libraries(benchmark PRIVATE engine_lib)

# Standalone baseline (for profiling in isolation)
add_executable(baseline_engine bench/baseline_engine.cpp)
target_include_directories(baseline_engine PRIVATE bench)
target_link_libraries(baseline_engine PRIVATE baseline_lib)

# Standalone optimized (for profiling in isolation)
add_executable(optimized_engine bench/optimized_engine.cpp)
target_include_directories(optimized_engine PRIVATE bench)
target_link_libraries(optimized_engine PRIVATE engine_lib)

# Comparison runner: runs both engines on the same workload, writes CSV
add_executable(benchmark_runner bench/benchmark_runner.cpp)
target_include_directories(benchmark_runner PRIVATE bench)
target_link_libraries(benchmark_runner PRIVATE engine_lib baseline_lib)

# Event exporter: generates web/data/events.json for the browser visualizer
add_executable(export_events bench/export_events.cpp)
target_link_libraries(export_events PRIVATE engine_lib)

# ── Native HTTP server ────────────────────────────────────────────────────────

include(FetchContent)
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(httplib)

add_executable(engine_server src/server.cpp)
target_link_libraries(engine_server PRIVATE engine_lib httplib::httplib)
target_include_directories(engine_server PRIVATE src)
