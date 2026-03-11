#pragma once
#include "fill.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "order_pool.hpp"
#include <cstdint>
#include <vector>

class MatchingEngine {
public:
    MatchingEngine()                               = default;
    MatchingEngine(const MatchingEngine&)            = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&)                 = delete;
    MatchingEngine& operator=(MatchingEngine&&)      = delete;

    // Submit a limit order. Fills appended to out (caller clears between calls).
    void submit_limit(Side side, uint64_t price, uint64_t qty, std::vector<Fill>& out);

    // Submit a market order (no resting remainder). Fills appended to out.
    void submit_market(Side side, uint64_t qty, std::vector<Fill>& out);

    // Cancel a resting order. Returns true if found.
    bool cancel(uint64_t order_id) noexcept;

    OrderBook& book() { return book_; }

    // Returns the ID that will be assigned to the next submitted order.
    // Used by EventLogger to track taker IDs without modifying the engine.
    uint64_t peek_next_id() const { return next_id_; }

private:
    uint64_t next_id_ = 1;
    OrderBook book_;
    OrderPool pool_;     // default capacity = 1<<19 (524 288 slots)

    uint64_t new_id() noexcept { return next_id_++; }

    // Match taker_qty against the opposite side.
    // If is_market is true, taker_price is ignored (crosses at any price).
    // Fills appended to out. Returns unfilled remainder.
    uint64_t match(Side taker_side, uint64_t taker_id,
                   uint64_t taker_price, bool is_market,
                   uint64_t taker_qty, std::vector<Fill>& out);
};
