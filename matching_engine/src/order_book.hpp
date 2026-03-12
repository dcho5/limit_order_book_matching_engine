#pragma once
#include "order.hpp"
#include "price_level.hpp"
#include <cstdint>
#include <map>
#include <unordered_map>

class OrderBook {
public:
    // Bids: highest price first
    using BidMap = std::map<uint64_t, PriceLevel, std::greater<uint64_t>>;
    // Asks: lowest price first
    using AskMap = std::map<uint64_t, PriceLevel, std::less<uint64_t>>;

    // Insert a resting order into the book. Caller owns the Order lifetime.
    void add_order(Order* o);

    // Remove an order by ID. Returns true if found and removed.
    bool cancel_order(uint64_t order_id);

    // Remove an order by ID and return its pointer (for pool release).
    // Returns nullptr if not found. Single hash-map lookup — use this in cancel paths.
    Order* cancel_and_return(uint64_t order_id);

    // Returns pointer to best bid PriceLevel, or nullptr if book is empty.
    PriceLevel* best_bid() noexcept;
    // Returns pointer to best ask PriceLevel, or nullptr if book is empty.
    PriceLevel* best_ask() noexcept;

    // Direct map access for matching engine iteration.
    BidMap& bids() { return bids_; }
    AskMap& asks() { return asks_; }

    friend class MatchingEngine;  // only engine may call remove_order directly

private:
    // Remove a specific order from its price level AND the index.
    // Does NOT release the order back to the pool — caller is responsible.
    void remove_order(Order* o);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<uint64_t, Order*> index_;  // O(1) lookup by id
};
