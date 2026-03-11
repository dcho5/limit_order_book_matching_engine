#pragma once
#include "fill.hpp"
#include "order.hpp"
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

// ── BaselineBook ─────────────────────────────────────────────────────────────
//
// "Naive" order book using std::list<Order> per price level.
//
// Every add_order() call triggers a heap allocation for the std::list node.
// Cancel stores a std::list::iterator in a side-table so splice-out is O(1)
// without intrusive pointers — but the iterator is a fat object (typically a
// pointer to a heap-allocated node), and the nodes themselves are scattered
// across the heap, destroying cache locality on iteration.
//
// This is the implementation most engineers write first. Compare against the
// optimized intrusive-list + object-pool version to see measurable differences
// in throughput and tail latency.

class BaselineBook {
public:
    using OrderList = std::list<Order>;

    struct Level {
        OrderList orders;
        uint64_t  total_volume = 0;
    };

    using BidMap = std::map<uint64_t, Level, std::greater<uint64_t>>;
    using AskMap = std::map<uint64_t, Level, std::less<uint64_t>>;

    void add_order(uint64_t id, Side side, uint64_t price, uint64_t qty);
    bool cancel_order(uint64_t id);

    bool     asks_empty()     const { return asks_.empty(); }
    bool     bids_empty()     const { return bids_.empty(); }
    uint64_t best_ask_price() const { return asks_.begin()->first; }
    uint64_t best_bid_price() const { return bids_.begin()->first; }

    // Consume up to fill_qty from the front order of the best ask (or bid).
    // Removes the maker from the index if fully filled.
    struct ConsumeResult { uint64_t maker_id, price, filled_qty; };
    ConsumeResult consume_best_ask(uint64_t fill_qty);
    ConsumeResult consume_best_bid(uint64_t fill_qty);

private:
    BidMap bids_;
    AskMap asks_;

    // The iterator stored here is what enables O(1) cancel.
    // In the optimized version, this role is played by Order::prev/next —
    // no separate allocation required.
    struct IndexEntry {
        Side                side;
        uint64_t            price;
        OrderList::iterator it;
    };
    std::unordered_map<uint64_t, IndexEntry> index_;
};

// ── BaselineMatchingEngine ────────────────────────────────────────────────────
//
// Matching engine wrapping BaselineBook.
// Provides the same public interface as MatchingEngine for fair comparison.

class BaselineMatchingEngine {
public:
    void submit_limit(Side side, uint64_t price, uint64_t qty, std::vector<Fill>& out);
    void submit_market(Side side, uint64_t qty, std::vector<Fill>& out);
    bool              cancel(uint64_t order_id);

private:
    uint64_t     next_id_ = 1;
    BaselineBook book_;

    uint64_t new_id() { return next_id_++; }

    // Core matching loop. taker_price == 0 means market order.
    uint64_t do_match(Side side, uint64_t taker_id,
                      uint64_t taker_price, uint64_t taker_qty,
                      std::vector<Fill>& out);
};
