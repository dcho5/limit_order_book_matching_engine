// standalone_bench.cpp — B4: merged profiling binary replacing the near-identical
// baseline_engine.cpp and optimized_engine.cpp (which differed only in engine class).
//
// Usage:
//   ./standalone_bench [--engine baseline|optimized] [total_events]
//
// Examples:
//   ./standalone_bench                              # optimized, 1M events
//   ./standalone_bench --engine baseline 2000000   # baseline, 2M events
//   ./standalone_bench --engine optimized 500000   # optimized, 500K events

#include "baseline_book.hpp"
#include "fill.hpp"
#include "latency_stats.hpp"
#include "matching_engine.hpp"
#include "workload_generator.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ── Run helper templated on engine type ──────────────────────────────────────

template <typename Engine>
static void run(Engine& me, const std::vector<WorkloadEvent>& events,
                const char* label, const char* csv_name) {
    std::vector<uint64_t> latencies;
    latencies.reserve(events.size());

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
    stats.print(label);

    std::filesystem::create_directories("results");
    stats.write_csv(std::string("results/") + csv_name + ".csv", csv_name);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    bool        use_baseline  = false;
    uint32_t    total_events  = 1'000'000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
            ++i;
            if (std::strcmp(argv[i], "baseline") == 0)        use_baseline = true;
            else if (std::strcmp(argv[i], "optimized") == 0)  use_baseline = false;
            else { std::fprintf(stderr, "Unknown engine: %s\n", argv[i]); return 1; }
        } else {
            total_events = uint32_t(std::atoi(argv[i]));
        }
    }

    WorkloadConfig cfg;
    cfg.total_events = total_events;
    WorkloadGenerator gen{cfg};
    auto events = gen.generate();

    if (use_baseline) {
        BaselineMatchingEngine me;
        run(me, events, "Baseline: std::map + std::list", "baseline_standalone");
    } else {
        MatchingEngine me;
        run(me, events, "Optimized: intrusive list + object pool", "optimized_standalone");
    }

    return 0;
}
