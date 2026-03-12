cmake_minimum_required(VERSION 3.20)
project(MatchingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)
# B2: -O2 removed — was overriding Debug builds. Release gets it via CMAKE_CXX_FLAGS_RELEASE.

# B8: verify std::format is available before any target uses shared_state.hpp.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
  #include <format>
  int main() { return std::format(\"{}\", 0).size() > 0 ? 0 : 1; }
" HAS_STD_FORMAT)
if(NOT HAS_STD_FORMAT)
  message(FATAL_ERROR
    "std::format is not available. "
    "Requires GCC >= 13 or Clang >= 14 with -std=c++20. "
    "Upgrade your compiler or use an alternative formatting library.")
endif()

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

# B4: merged standalone profiling binary replacing baseline_engine + optimized_engine.
# Usage: ./standalone_bench [--engine baseline|optimized] [total_events]
add_executable(standalone_bench bench/standalone_bench.cpp)
target_include_directories(standalone_bench PRIVATE bench)
target_link_libraries(standalone_bench PRIVATE engine_lib baseline_lib)

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

# B12: WinSock required by cpp-httplib on Windows.
if(WIN32)
  target_link_libraries(engine_server PRIVATE ws2_32)
endif()
