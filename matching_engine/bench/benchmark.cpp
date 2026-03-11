#include "matching_engine.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

// Simple LCG for fast, deterministic pseudo-random numbers.
static uint64_t lcg_state = 12345678901234567ULL;
static uint64_t lcg_next() {
    lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lcg_state;
}

int main() {
    constexpr int N = 2'000'000;

    MatchingEngine me;
    std::vector<uint64_t> live_ids;
    live_ids.reserve(1024);

    std::vector<Fill> fills;   // reused across calls — no per-call allocation
    fills.reserve(16);

    uint64_t total_fills = 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < N; ++i) {
        uint64_t r = lcg_next();

        // Mix of order types: ~60% limit, ~20% market, ~20% cancel.
        int type = static_cast<int>(r % 10);

        if (type < 6) {
            // Limit order: price in [99, 102], qty in [1, 20].
            Side     side  = (r >> 8) & 1 ? Side::Buy : Side::Sell;
            uint64_t price = 99 + (r >> 4) % 4;
            uint64_t qty   = 1  + (r >> 12) % 20;
            fills.clear();
            me.submit_limit(side, price, qty, fills);
            total_fills += fills.size();
        } else if (type < 8) {
            // Market order.
            Side     side = (r >> 8) & 1 ? Side::Buy : Side::Sell;
            uint64_t qty  = 1 + (r >> 12) % 10;
            fills.clear();
            me.submit_market(side, qty, fills);
            total_fills += fills.size();
        } else {
            // Cancel: try a plausible recent id.
            if (i > 10) {
                uint64_t attempt_id = 1 + (r % static_cast<uint64_t>(i));
                me.cancel(attempt_id);
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double throughput = N / elapsed_s;

    std::printf("Orders submitted : %d\n", N);
    std::printf("Total fills      : %llu\n", static_cast<unsigned long long>(total_fills));
    std::printf("Elapsed          : %.3f s\n", elapsed_s);
    std::printf("Throughput       : %.0f orders/sec\n", throughput);

    return 0;
}
