// benchmark_runner.cpp
//
// Runs the baseline and optimized engines against an identical workload,
// measures per-order latency, and writes a CSV comparison to results/.
//
// Usage:
//   ./benchmark_runner [total_events] [bursty]
//
// Examples:
//   ./benchmark_runner              — 1M events, uniform price distribution
//   ./benchmark_runner 2000000      — 2M events
//   ./benchmark_runner 1000000 bursty  — 1M events with bursty arrival

#include "baseline_book.hpp"
#include "latency_stats.hpp"
#include "matching_engine.hpp"
#include "workload_generator.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

// ── Generic benchmark driver ──────────────────────────────────────────────────
//
// Works with any engine that exposes:
//   submit_limit(Side, uint64_t price, uint64_t qty, std::vector<Fill>& out)
//   submit_market(Side, uint64_t qty, std::vector<Fill>& out)
//   cancel(uint64_t id) -> bool
//
// Latency is sampled per batch of BATCH_SIZE operations (one Clock::now() pair
// per batch, amortized over all ops in the batch). This reduces timer overhead
// from ~40–100 ns per op to < 1 ns per op, giving accurate distributions for
// sub-100 ns operations. Each batch contributes one sample = batch_ns / batch_size.

static constexpr std::size_t BATCH_SIZE = 100;

template <typename Engine>
LatencyStats run_engine(Engine& engine, const std::vector<WorkloadEvent>& events) {
    std::vector<uint64_t> latencies;
    latencies.reserve(events.size() / BATCH_SIZE + 1);

    std::vector<Fill> fills;   // reused across calls — no per-call allocation
    fills.reserve(16);

    auto wall_start = Clock::now();

    for (std::size_t i = 0; i < events.size(); ) {
        const std::size_t batch_end = std::min(i + BATCH_SIZE, events.size());

        auto t0 = Clock::now();
        for (std::size_t j = i; j < batch_end; ++j) {
            const auto& e = events[j];
            switch (e.type) {
                case WorkloadEvent::Type::Limit:
                    fills.clear();
                    engine.submit_limit(e.side, e.price, e.qty, fills);
                    break;
                case WorkloadEvent::Type::Market:
                    fills.clear();
                    engine.submit_market(e.side, e.qty, fills);
                    break;
                case WorkloadEvent::Type::Cancel:
                    engine.cancel(e.id);
                    break;
            }
        }
        auto t1 = Clock::now();

        uint64_t batch_ns = uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        latencies.push_back(batch_ns / (batch_end - i));

        i = batch_end;
    }

    double elapsed = std::chrono::duration<double>(
        Clock::now() - wall_start).count();
    return LatencyStats::compute(latencies, elapsed);
}

// ── Workload summary ──────────────────────────────────────────────────────────

static void print_workload_summary(const WorkloadConfig& cfg,
                                   const std::vector<WorkloadEvent>& events) {
    uint32_t limits = 0, cancels = 0, markets = 0;
    for (const auto& e : events) {
        switch (e.type) {
            case WorkloadEvent::Type::Limit:  ++limits;  break;
            case WorkloadEvent::Type::Cancel: ++cancels; break;
            case WorkloadEvent::Type::Market: ++markets; break;
        }
    }
    std::printf("Workload : %u events  "
                "[%u limits  /  %u cancels  /  %u markets]%s\n",
                uint32_t(events.size()), limits, cancels, markets,
                cfg.bursty ? "  [BURSTY]" : "");
    std::printf("Prices   : mid=%llu, %u levels/side, tick=%llu\n",
                (unsigned long long)cfg.mid_price,
                cfg.price_levels,
                (unsigned long long)cfg.tick_size);
    std::printf("Sizes    : qty in [%llu, %llu]\n",
                (unsigned long long)cfg.qty_min,
                (unsigned long long)cfg.qty_max);
    std::printf("Memory   : sizeof(Order) = %zu bytes  "
                "(%zu per 64-byte cache line)\n\n",
                sizeof(Order), 64 / sizeof(Order));
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    WorkloadConfig cfg;
    cfg.total_events = (argc > 1) ? uint32_t(std::atoi(argv[1])) : 1'000'000;
    cfg.bursty       = (argc > 2) && (std::string(argv[2]) == "bursty");

    std::printf("=== Matching Engine Benchmark Runner ===\n\n");

    // Both engines process the same event sequence for a fair comparison.
    std::printf("Generating workload...\n");
    WorkloadGenerator gen{cfg};
    auto events = gen.generate();
    print_workload_summary(cfg, events);

    // ── Baseline: std::map + std::list ────────────────────────────────────────
    std::printf("Running baseline  (std::map + std::list)...\n");
    BaselineMatchingEngine baseline_me;
    auto baseline = run_engine(baseline_me, events);
    baseline.print("Baseline  —  std::map + std::list (per-order heap alloc)");

    // ── Optimized: std::map + intrusive list + object pool ────────────────────
    std::printf("\nRunning optimized (std::map + intrusive list + pool)...\n");
    MatchingEngine optimized_me;
    auto optimized = run_engine(optimized_me, events);
    optimized.print("Optimized —  intrusive list + object pool");

    // ── Side-by-side comparison ───────────────────────────────────────────────
    auto ratio = [](double a, double b) { return (b > 0.0) ? a / b : 0.0; };

    std::printf("\n");
    std::printf("┌────────────────────────────────────────────────┐\n");
    std::printf("│               Comparison Summary               │\n");
    std::printf("├────────────────────────────────────────────────┤\n");
    std::printf("│  Throughput speedup   : %6.2fx               │\n",
                ratio(optimized.throughput, baseline.throughput));
    std::printf("│  p50  improvement     : %6.2fx               │\n",
                ratio(double(baseline.p50_ns),  double(optimized.p50_ns)));
    std::printf("│  p99  improvement     : %6.2fx               │\n",
                ratio(double(baseline.p99_ns),  double(optimized.p99_ns)));
    std::printf("│  p999 improvement     : %6.2fx               │\n",
                ratio(double(baseline.p999_ns), double(optimized.p999_ns)));
    std::printf("└────────────────────────────────────────────────┘\n");

    // ── CSV output ────────────────────────────────────────────────────────────
    std::filesystem::create_directories("results");
    const std::string csv = "results/benchmark_results.csv";
    baseline.write_csv(csv, "baseline");
    optimized.write_csv(csv, "optimized");
    std::printf("\nResults written to %s\n", csv.c_str());

    return 0;
}
