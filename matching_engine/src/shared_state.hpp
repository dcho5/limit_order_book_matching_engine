// shared_state.hpp — Shared simulation state and JSON response builder.
// Both server.cpp (native HTTP) and wasm_bridge.cpp (WASM/native C interface)
// include this header and instantiate one SharedState each.

#pragma once

#include "fill.hpp"
#include "matching_engine.hpp"
#include "order.hpp"
#include "price_level.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

// ── Ring-buffer capacity constants (item 34) ──────────────────────────────────

static constexpr int TRADE_RING_CAP      = 20;
static constexpr int TRADE_RESPONSE_MAX  = 10;

// ── Shared state ──────────────────────────────────────────────────────────────

struct Trade {
    uint64_t t, price, qty;
    bool buy_aggressor;
};

struct SharedState {
    std::unique_ptr<MatchingEngine> eng;
    std::vector<Fill>               fills;
    std::string                     result;  // scratch buffer — wasm callers return .c_str()

    uint64_t total_orders = 0;
    uint64_t total_trades = 0;
    uint64_t sim_time     = 0;

    Trade trades[TRADE_RING_CAP]{};   // item 34: named constant
    int   trade_count = 0;

    std::unordered_set<int> live_ids;  // item 25: O(1) insert/erase
    int tick_mid = 100;

    uint64_t rng = 0xC0FFEE42ULL;
};

// ── State helpers ─────────────────────────────────────────────────────────────

// item 30: single canonical LCG step; all RNG sites use this.
inline uint64_t lcg_step(uint64_t& rng) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng;
}

inline uint64_t next_rand(SharedState& st) { return lcg_step(st.rng); }

inline uint64_t count_resting(SharedState& st) {
    uint64_t n = 0;
    for (auto& [p, lvl] : st.eng->book().bids())
        for (const Order* o = lvl.head; o; o = o->next) ++n;
    for (auto& [p, lvl] : st.eng->book().asks())
        for (const Order* o = lvl.head; o; o = o->next) ++n;
    return n;
}

inline double compute_mid(SharedState& st) {
    auto& b = st.eng->book().bids();
    auto& a = st.eng->book().asks();
    if (b.empty() || a.empty()) return 0.0;
    return (double(b.begin()->first) + double(a.begin()->first)) * 0.5;
}

inline void add_trade(SharedState& st, uint64_t price, uint64_t qty, bool buy_aggressor) {
    ++st.total_trades;
    ++st.sim_time;
    if (st.trade_count < TRADE_RING_CAP) ++st.trade_count;  // item 34
    for (int i = st.trade_count - 1; i > 0; --i) st.trades[i] = st.trades[i-1];
    st.trades[0] = { st.sim_time, price, qty, buy_aggressor };
}

// item 25: O(1) erase by value (unordered_set).
inline void remove_live(SharedState& st, int id) {
    st.live_ids.erase(id);
}

// ── JSON response builder ─────────────────────────────────────────────────────

inline std::string build_response(SharedState& st, int order_id, uint64_t elapsed_ns,
                                  const std::vector<Fill>& fills) {
    std::string s;
    s.reserve(1024);

    s += std::format("{{\"id\":{}", order_id);

    s += ",\"fills\":[";
    for (size_t i = 0; i < fills.size(); ++i) {
        if (i) s += ',';
        s += std::format("{{\"maker_id\":{},\"price\":{},\"qty\":{},\"elapsed_ns\":{}}}",
                         fills[i].maker_id, fills[i].price, fills[i].quantity, elapsed_ns);
    }
    s += ']';

    s += ",\"bids\":[";
    int n = 0;
    for (auto& [price, lvl] : st.eng->book().bids()) {
        if (n >= 8) break;
        if (n) s += ',';
        s += std::format("{{\"price\":{},\"vol\":{}}}", price, lvl.total_volume);
        ++n;
    }
    s += ']';

    s += ",\"asks\":[";
    n = 0;
    for (auto& [price, lvl] : st.eng->book().asks()) {
        if (n >= 8) break;
        if (n) s += ',';
        s += std::format("{{\"price\":{},\"vol\":{}}}", price, lvl.total_volume);
        ++n;
    }
    s += ']';

    s += ",\"trades\":[";
    int maxT = std::min(st.trade_count, TRADE_RESPONSE_MAX);  // item 34
    for (int i = 0; i < maxT; ++i) {
        if (i) s += ',';
        s += std::format("{{\"t\":{},\"price\":{},\"qty\":{},\"side\":\"{}\"}}",
                         st.trades[i].t, st.trades[i].price, st.trades[i].qty,
                         st.trades[i].buy_aggressor ? "buy" : "sell");
    }
    s += ']';

    s += std::format(",\"stats\":{{\"totalOrders\":{},\"totalTrades\":{},\"resting\":{},\"mid\":{:.1f}}}",
                     st.total_orders, st.total_trades, count_resting(st), compute_mid(st));

    s += std::format(",\"elapsed_ns\":{}}}", elapsed_ns);

    return s;
}

// ── Book seed helper (item 29) ────────────────────────────────────────────────

// Seeds 8 bid/ask levels around price 100. Extracted from 3 copy-paste sites.
inline void init_engine_and_book(MatchingEngine& eng, std::vector<Fill>& fills) {
    for (int d = 1; d <= 8; ++d) {
        fills.clear(); eng.submit_limit(Side::Buy,  100 - d, 200, fills);
        fills.clear(); eng.submit_limit(Side::Sell, 100 + d, 200, fills);
    }
}

// ── Engine initialiser ────────────────────────────────────────────────────────

inline void init_engine(SharedState& st) {
    st.eng = std::make_unique<MatchingEngine>();
    st.fills.clear();
    st.live_ids.clear();
    st.total_orders = 0;
    st.total_trades = 0;
    st.sim_time     = 0;
    st.trade_count  = 0;
    st.tick_mid     = 100;

    init_engine_and_book(*st.eng, st.fills);         // item 29
    for (int i = 1; i <= 16; ++i) st.live_ids.insert(i);  // item 25
}
