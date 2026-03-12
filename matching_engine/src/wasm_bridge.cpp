// wasm_bridge.cpp — WASM/native C interface for MatchingEngine
// Compiled with Emscripten for browser use; also compiles natively for testing.

#include "shared_state.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#  define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#  define KEEPALIVE
#endif

// ── Bridge state ──────────────────────────────────────────────────────────────

static SharedState g_state;

// ── Exported functions ────────────────────────────────────────────────────────

extern "C" {

KEEPALIVE const char* wasm_submit_limit(int side_int, int price_i, int qty_i) {
    if (!g_state.eng) init_engine(g_state);
    Side     side  = side_int == 0 ? Side::Buy : Side::Sell;
    uint64_t price = static_cast<uint64_t>(price_i);
    uint64_t qty   = static_cast<uint64_t>(qty_i);

    int assigned_id = static_cast<int>(g_state.eng->peek_next_id());

    auto t0 = std::chrono::high_resolution_clock::now();
    g_state.fills.clear();
    g_state.eng->submit_limit(side, price, qty, g_state.fills);
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    ++g_state.total_orders;

    uint64_t total_filled = 0;
    for (auto& f : g_state.fills) {
        total_filled += f.quantity;
        add_trade(g_state, f.price, f.quantity, side == Side::Buy);
        remove_live(g_state, static_cast<int>(f.maker_id));
    }
    if (total_filled < qty) g_state.live_ids.push_back(assigned_id);

    g_state.result = build_response(g_state, assigned_id, elapsed_ns, g_state.fills);
    return g_state.result.c_str();
}

KEEPALIVE const char* wasm_submit_market(int side_int, int qty_i) {
    if (!g_state.eng) init_engine(g_state);
    Side     side = side_int == 0 ? Side::Buy : Side::Sell;
    uint64_t qty  = static_cast<uint64_t>(qty_i);

    auto t0 = std::chrono::high_resolution_clock::now();
    g_state.fills.clear();
    g_state.eng->submit_market(side, qty, g_state.fills);
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    ++g_state.total_orders;
    for (auto& f : g_state.fills) {
        add_trade(g_state, f.price, f.quantity, side == Side::Buy);
        remove_live(g_state, static_cast<int>(f.maker_id));
    }
    g_state.result = build_response(g_state, 0, elapsed_ns, g_state.fills);
    return g_state.result.c_str();
}

KEEPALIVE const char* wasm_cancel(int id) {
    if (!g_state.eng) init_engine(g_state);
    auto t0 = std::chrono::high_resolution_clock::now();
    bool ok = g_state.eng->cancel(static_cast<uint64_t>(id));
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    if (ok) remove_live(g_state, id);
    std::vector<Fill> no_fills;
    g_state.result = build_response(g_state, ok ? id : 0, elapsed_ns, no_fills);
    return g_state.result.c_str();
}

KEEPALIVE const char* wasm_tick() {
    if (!g_state.eng) init_engine(g_state);

    // Ensure minimum liquidity
    if (g_state.eng->book().bids().empty()) {
        g_state.fills.clear();
        int nid = static_cast<int>(g_state.eng->peek_next_id());
        g_state.eng->submit_limit(Side::Buy, g_state.tick_mid - 1, 200, g_state.fills);
        g_state.live_ids.push_back(nid);
    }
    if (g_state.eng->book().asks().empty()) {
        g_state.fills.clear();
        int nid = static_cast<int>(g_state.eng->peek_next_id());
        g_state.eng->submit_limit(Side::Sell, g_state.tick_mid + 1, 200, g_state.fills);
        g_state.live_ids.push_back(nid);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    int batch = 1 + static_cast<int>(next_rand(g_state) % 5);
    g_state.fills.clear();

    for (int i = 0; i < batch; ++i) {
        double r = double(next_rand(g_state) >> 11) * (1.0 / double(1ULL << 53));
        std::vector<Fill> step;

        if (r < 0.55) {
            Side side   = (next_rand(g_state) & 1) ? Side::Sell : Side::Buy;
            int  offset = static_cast<int>(next_rand(g_state) % 7);
            int  px     = (side == Side::Buy)
                          ? std::max(1, g_state.tick_mid - offset)
                          : g_state.tick_mid + offset;
            int  qty    = 10 + static_cast<int>(next_rand(g_state) % 241);
            int  nid    = static_cast<int>(g_state.eng->peek_next_id());
            g_state.eng->submit_limit(side, uint64_t(px), uint64_t(qty), step);
            ++g_state.total_orders;

            uint64_t filled = 0;
            for (auto& f : step) {
                filled += f.quantity;
                add_trade(g_state, f.price, f.quantity, side == Side::Buy);
                remove_live(g_state, static_cast<int>(f.maker_id));
                g_state.fills.push_back(f);
            }
            if (filled < uint64_t(qty)) g_state.live_ids.push_back(nid);

            double r2 = double(next_rand(g_state) >> 11) * (1.0 / double(1ULL << 53));
            if      (r2 < 0.30 && g_state.tick_mid > 60)  --g_state.tick_mid;
            else if (r2 > 0.70 && g_state.tick_mid < 160) ++g_state.tick_mid;

        } else if (r < 0.85) {
            if (!g_state.live_ids.empty()) {
                int idx = static_cast<int>(next_rand(g_state) % g_state.live_ids.size());
                int cid = g_state.live_ids[idx];
                if (g_state.eng->cancel(uint64_t(cid))) {
                    g_state.live_ids[idx] = g_state.live_ids.back();
                    g_state.live_ids.pop_back();
                }
            }
        } else {
            Side side = (next_rand(g_state) & 1) ? Side::Sell : Side::Buy;
            int  qty  = 10 + static_cast<int>(next_rand(g_state) % 91);
            g_state.eng->submit_market(side, uint64_t(qty), step);
            ++g_state.total_orders;
            for (auto& f : step) {
                add_trade(g_state, f.price, f.quantity, side == Side::Buy);
                remove_live(g_state, static_cast<int>(f.maker_id));
                g_state.fills.push_back(f);
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    g_state.result = build_response(g_state, 0, elapsed_ns, g_state.fills);
    return g_state.result.c_str();
}

KEEPALIVE const char* wasm_run_benchmark(int total_ops, int limit_pct, int cancel_pct, int market_pct) {
    std::unique_ptr<MatchingEngine> beng = std::make_unique<MatchingEngine>();
    std::vector<Fill>  bfills;
    std::vector<int>   blive;
    int bmid = 100;

    for (int d = 1; d <= 8; ++d) {
        bfills.clear(); beng->submit_limit(Side::Buy,  100-d, 200, bfills);
        bfills.clear(); beng->submit_limit(Side::Sell, 100+d, 200, bfills);
    }
    for (int i = 1; i <= 16; ++i) blive.push_back(i);

    double lf  = limit_pct  / 100.0;
    double cft = lf + cancel_pct / 100.0;

    uint64_t brng = 0xDEADBEEFULL;
    auto brand = [&]() -> uint64_t {
        brng = brng * 6364136223846793005ULL + 1442695040888963407ULL;
        return brng;
    };

    const int BATCH = 10000;
    std::vector<uint64_t> lats;
    int alt = 0;

    auto tstart = std::chrono::high_resolution_clock::now();

    for (int op = 0; op < total_ops; op += BATCH) {
        int bn = std::min(BATCH, total_ops - op);
        auto t0 = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < bn; ++i) {
            double r = double(brand() >> 11) * (1.0 / double(1ULL << 53));
            if (r < lf) {
                Side s   = (alt++ & 1) ? Side::Sell : Side::Buy;
                int  off = static_cast<int>(brand() % 7);
                int  px  = (s == Side::Buy) ? std::max(1, bmid-off) : bmid+off;
                int  qty = 1 + static_cast<int>(brand() % 10);
                int  nid = static_cast<int>(beng->peek_next_id());
                bfills.clear();
                beng->submit_limit(s, uint64_t(px), uint64_t(qty), bfills);
                // Don't remove filled maker IDs from blive — stale IDs cause a
                // fast failed-cancel (O(1) hash miss) and get cleaned up there.
                uint64_t filled = 0;
                for (auto& f : bfills) filled += f.quantity;
                if (filled < uint64_t(qty)) blive.push_back(nid);
                double r2 = double(brand() >> 11) * (1.0 / double(1ULL << 53));
                if      (r2 < 0.3 && bmid > 60)  --bmid;
                else if (r2 > 0.7 && bmid < 160) ++bmid;
            } else if (r < cft) {
                if (!blive.empty()) {
                    int idx = static_cast<int>(brand() % blive.size());
                    int cid = blive[idx];
                    // Always swap-erase: cleans up both live orders and stale
                    // (already-filled) entries. Engine cancel is O(1) either way.
                    beng->cancel(uint64_t(cid));
                    blive[idx] = blive.back(); blive.pop_back();
                }
            } else {
                Side s   = (alt++ & 1) ? Side::Sell : Side::Buy;
                int  qty = 1 + static_cast<int>(brand() % 10);
                bfills.clear();
                beng->submit_market(s, uint64_t(qty), bfills);
                if (beng->book().bids().empty()) {
                    bfills.clear();
                    int nid = static_cast<int>(beng->peek_next_id());
                    beng->submit_limit(Side::Buy, bmid-1, 100, bfills);
                    blive.push_back(nid);
                }
                if (beng->book().asks().empty()) {
                    bfills.clear();
                    int nid = static_cast<int>(beng->peek_next_id());
                    beng->submit_limit(Side::Sell, bmid+1, 100, bfills);
                    blive.push_back(nid);
                }
            }
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        lats.push_back(ns / uint64_t(bn));
    }

    auto tend = std::chrono::high_resolution_clock::now();
    double elapsed_ms = double(
        std::chrono::duration_cast<std::chrono::microseconds>(tend - tstart).count()) / 1000.0;

    std::vector<uint64_t> sorted = lats;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> uint64_t {
        if (sorted.empty()) return 0;
        size_t idx = std::min(
            static_cast<size_t>(std::lround(p * double(sorted.size()-1))),
            sorted.size()-1);
        return sorted[idx];
    };

    uint64_t throughput = elapsed_ms > 0.0
        ? static_cast<uint64_t>(double(total_ops) / (elapsed_ms / 1000.0)) : 0;

    // Histogram: raw per-batch per-op latencies
    std::string hist = "[";
    for (size_t i = 0; i < lats.size(); ++i) {
        if (i) hist += ',';
        hist += std::to_string(lats[i]);
    }
    hist += ']';

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"throughput\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,"
        "\"elapsed_ms\":%.1f,\"mode\":\"wasm\"",
        (unsigned long long)throughput,
        (unsigned long long)pct(0.50),
        (unsigned long long)pct(0.99),
        (unsigned long long)pct(0.999),
        elapsed_ms);
    g_state.result = std::string(buf) + ",\"histogram\":" + hist + '}';
    return g_state.result.c_str();
}

KEEPALIVE void wasm_reset() {
    init_engine(g_state);
}

} // extern "C"
