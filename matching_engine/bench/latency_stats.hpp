#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// ── LatencyStats ──────────────────────────────────────────────────────────────
//
// Computes throughput and latency distribution from a vector of per-operation
// nanosecond measurements.
//
// Measurement note:
//   clock_gettime / std::chrono::high_resolution_clock typically have 20–50 ns
//   of overhead per call.  For very fast operations (<100 ns) this overhead is
//   visible in the raw distribution.  The comparison between baseline and
//   optimized is still valid because the overhead is constant across both.

struct LatencyStats {
    uint64_t p50_ns  = 0;
    uint64_t p99_ns  = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns  = 0;
    double   mean_ns = 0.0;

    uint64_t total_orders = 0;
    double   elapsed_sec  = 0.0;
    double   throughput   = 0.0;   // orders / second

    // Takes the latency vector by value (sorts internally — caller's copy untouched).
    static LatencyStats compute(std::vector<uint64_t> ns, double elapsed_sec) {
        LatencyStats s;
        s.total_orders = ns.size();
        s.elapsed_sec  = elapsed_sec;
        s.throughput   = (elapsed_sec > 0.0)
                         ? double(s.total_orders) / elapsed_sec
                         : 0.0;

        if (ns.empty()) return s;

        std::sort(ns.begin(), ns.end());

        // Nearest-rank percentile with std::lround to avoid truncation bias.
        auto pct = [&](double p) -> uint64_t {
            auto idx = static_cast<std::size_t>(
                std::lround(p * double(ns.size() - 1)));
            return ns[std::min(idx, ns.size() - 1)];
        };

        s.p50_ns  = pct(0.500);
        s.p99_ns  = pct(0.990);
        s.p999_ns = pct(0.999);
        s.max_ns  = ns.back();

        double sum = 0.0;
        for (auto v : ns) sum += double(v);
        s.mean_ns = sum / double(ns.size());

        return s;
    }

    void print(const char* label) const {
        std::printf("\n%s\n", label);
        std::printf("  Orders     : %llu\n",
                    (unsigned long long)total_orders);
        std::printf("  Elapsed    : %.3f s\n",    elapsed_sec);
        std::printf("  Throughput : %.2fM ops/sec\n", throughput / 1e6);
        std::printf("  Latency (includes ~20-50ns measurement overhead):\n");
        std::printf("    mean : %.0f ns\n",   mean_ns);
        std::printf("    p50  : %llu ns\n",   (unsigned long long)p50_ns);
        std::printf("    p99  : %llu ns\n",   (unsigned long long)p99_ns);
        std::printf("    p999 : %.1f us\n",   double(p999_ns) / 1000.0);
        std::printf("    max  : %.1f us\n",   double(max_ns)  / 1000.0);
    }

    // Appends one CSV row.  Creates the file with a header if it does not exist.
    void write_csv(const std::string& path, const char* label) const {
        bool needs_header = false;
        { std::ifstream probe(path); needs_header = !probe.good(); }

        std::ofstream f(path, std::ios::app);
        if (!f) {
            std::fprintf(stderr, "warning: could not open %s\n", path.c_str());
            return;
        }
        if (needs_header)
            f << "label,total_orders,elapsed_sec,throughput_ops_per_sec,"
                 "mean_ns,p50_ns,p99_ns,p999_ns,max_ns\n";

        f << label       << ','
          << total_orders << ','
          << elapsed_sec  << ','
          << throughput   << ','
          << mean_ns      << ','
          << p50_ns       << ','
          << p99_ns       << ','
          << p999_ns      << ','
          << max_ns       << '\n';
    }
};
