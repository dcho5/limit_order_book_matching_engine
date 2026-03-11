// baseline_engine.cpp
//
// Standalone benchmark for the baseline (std::map + std::list) implementation.
// Use this binary for profiling — it contains only the baseline code path,
// making it easier to isolate with tools like Instruments or perf.
//
// Usage:
//   ./baseline_engine [total_events]

#include "baseline_book.hpp"
#include "fill.hpp"
#include "latency_stats.hpp"
#include "workload_generator.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

int main(int argc, char** argv) {
    WorkloadConfig cfg;
    cfg.total_events = (argc > 1) ? uint32_t(std::atoi(argv[1])) : 1'000'000;

    WorkloadGenerator gen{cfg};
    auto events = gen.generate();

    std::vector<uint64_t> latencies;
    latencies.reserve(events.size());

    BaselineMatchingEngine me;
    std::vector<Fill> fills;
    fills.reserve(16);
    auto wall_start = std::chrono::high_resolution_clock::now();

    for (const auto& e : events) {
        auto t0 = std::chrono::high_resolution_clock::now();
        switch (e.type) {
            case WorkloadEvent::Type::Limit:
                fills.clear();
                me.submit_limit(e.side, e.price, e.qty, fills);
                break;
            case WorkloadEvent::Type::Market:
                fills.clear();
                me.submit_market(e.side, e.qty, fills);
                break;
            case WorkloadEvent::Type::Cancel:
                me.cancel(e.id);
                break;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        latencies.push_back(uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }

    double elapsed = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - wall_start).count();

    auto stats = LatencyStats::compute(latencies, elapsed);
    stats.print("Baseline: std::map + std::list");

    std::filesystem::create_directories("results");
    stats.write_csv("results/baseline_standalone.csv", "baseline");
    return 0;
}
