#include "matching_engine.hpp"
#include <algorithm>

// Match taker against the opposite side of the book.
// is_market == true  => cross at any price (taker_price ignored).
// Returns remaining unfilled quantity.
uint64_t MatchingEngine::match(Side taker_side, uint64_t taker_id,
                               uint64_t taker_price, bool is_market,
                               uint64_t taker_qty, std::vector<Fill>& out) {
    while (taker_qty > 0) {
        PriceLevel* lvl = nullptr;

        if (taker_side == Side::Buy) {
            auto& asks = book_.asks();
            if (asks.empty()) break;
            auto it = asks.begin();
            if (!is_market && it->first > taker_price) break;
            lvl = &it->second;   // direct reference — no second map lookup
        } else {
            auto& bids = book_.bids();
            if (bids.empty()) break;
            auto it = bids.begin();
            if (!is_market && it->first < taker_price) break;
            lvl = &it->second;
        }

        Order* maker      = lvl->front();
        uint64_t fill_qty = std::min(taker_qty, maker->quantity);
        out.push_back({maker->id, taker_id, maker->price, fill_qty});
        taker_qty        -= fill_qty;
        maker->quantity  -= fill_qty;
        lvl->total_volume -= fill_qty;   // O(1) via held reference (item 3 + 5)

        if (maker->quantity == 0) {
            book_.remove_order(maker);   // remove_order subtracts o->quantity (0), harmless
            pool_.release(maker);
        }
    }
    return taker_qty;
}

void MatchingEngine::submit_limit(Side side, uint64_t price, uint64_t qty, std::vector<Fill>& out) {
    uint64_t id        = new_id();
    uint64_t remaining = match(side, id, price, /*is_market=*/false, qty, out);

    if (remaining > 0) {
        Order* o = pool_.acquire();
        o->id       = id;
        o->price    = price;
        o->quantity = remaining;
        o->side     = side;
        book_.add_order(o);
    }
}

void MatchingEngine::submit_market(Side side, uint64_t qty, std::vector<Fill>& out) {
    uint64_t id = new_id();
    match(side, id, /*taker_price=*/0, /*is_market=*/true, qty, out);
}

bool MatchingEngine::cancel(uint64_t order_id) noexcept {
    Order* o = book_.cancel_and_return(order_id);  // single hash-map lookup
    if (!o) return false;
    pool_.release(o);
    return true;
}
