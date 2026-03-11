#include "baseline_book.hpp"
#include <algorithm>

// ── BaselineBook ──────────────────────────────────────────────────────────────

void BaselineBook::add_order(uint64_t id, Side side, uint64_t price, uint64_t qty) {
    Order o{};
    o.id = id;  o.price = price;  o.quantity = qty;  o.side = side;

    if (side == Side::Buy) {
        auto& level = bids_[price];
        level.orders.push_back(o);          // heap allocation for list node
        level.total_volume += qty;
        index_[id] = {side, price, std::prev(level.orders.end())};
    } else {
        auto& level = asks_[price];
        level.orders.push_back(o);
        level.total_volume += qty;
        index_[id] = {side, price, std::prev(level.orders.end())};
    }
}

bool BaselineBook::cancel_order(uint64_t id) {
    auto idx = index_.find(id);
    if (idx == index_.end()) return false;

    auto& entry = idx->second;
    if (entry.side == Side::Buy) {
        auto mit = bids_.find(entry.price);
        if (mit != bids_.end()) {
            mit->second.total_volume -= entry.it->quantity;
            mit->second.orders.erase(entry.it);   // O(1) via stored iterator
            if (mit->second.orders.empty()) bids_.erase(mit);
        }
    } else {
        auto mit = asks_.find(entry.price);
        if (mit != asks_.end()) {
            mit->second.total_volume -= entry.it->quantity;
            mit->second.orders.erase(entry.it);
            if (mit->second.orders.empty()) asks_.erase(mit);
        }
    }
    index_.erase(idx);
    return true;
}

BaselineBook::ConsumeResult BaselineBook::consume_best_ask(uint64_t fill_qty) {
    auto it    = asks_.begin();
    auto& lvl  = it->second;
    Order& front = lvl.orders.front();

    uint64_t actual = std::min(fill_qty, front.quantity);
    ConsumeResult r{front.id, it->first, actual};

    front.quantity    -= actual;
    lvl.total_volume  -= actual;

    if (front.quantity == 0) {
        index_.erase(front.id);
        lvl.orders.pop_front();                     // O(1) removal
        if (lvl.orders.empty()) asks_.erase(it);
    }
    return r;
}

BaselineBook::ConsumeResult BaselineBook::consume_best_bid(uint64_t fill_qty) {
    auto it    = bids_.begin();
    auto& lvl  = it->second;
    Order& front = lvl.orders.front();

    uint64_t actual = std::min(fill_qty, front.quantity);
    ConsumeResult r{front.id, it->first, actual};

    front.quantity    -= actual;
    lvl.total_volume  -= actual;

    if (front.quantity == 0) {
        index_.erase(front.id);
        lvl.orders.pop_front();
        if (lvl.orders.empty()) bids_.erase(it);
    }
    return r;
}

// ── BaselineMatchingEngine ────────────────────────────────────────────────────

uint64_t BaselineMatchingEngine::do_match(Side taker_side, uint64_t taker_id,
                                           uint64_t taker_price, uint64_t taker_qty,
                                           std::vector<Fill>& out) {
    while (taker_qty > 0) {
        if (taker_side == Side::Buy) {
            if (book_.asks_empty()) break;
            if (taker_price != 0 && book_.best_ask_price() > taker_price) break;
            auto r = book_.consume_best_ask(taker_qty);
            out.push_back({r.maker_id, taker_id, r.price, r.filled_qty});
            taker_qty -= r.filled_qty;
        } else {
            if (book_.bids_empty()) break;
            if (taker_price != 0 && book_.best_bid_price() < taker_price) break;
            auto r = book_.consume_best_bid(taker_qty);
            out.push_back({r.maker_id, taker_id, r.price, r.filled_qty});
            taker_qty -= r.filled_qty;
        }
    }
    return taker_qty;
}

void BaselineMatchingEngine::submit_limit(Side side, uint64_t price,
                                           uint64_t qty, std::vector<Fill>& out) {
    uint64_t id        = new_id();
    uint64_t remaining = do_match(side, id, price, qty, out);
    if (remaining > 0) book_.add_order(id, side, price, remaining);
}

void BaselineMatchingEngine::submit_market(Side side, uint64_t qty, std::vector<Fill>& out) {
    do_match(side, new_id(), /*taker_price=*/0, qty, out);
}

bool BaselineMatchingEngine::cancel(uint64_t order_id) {
    return book_.cancel_order(order_id);
}
