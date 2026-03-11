#include "order_book.hpp"

void OrderBook::add_order(Order* o) {
    if (o->side == Side::Buy) {
        bids_[o->price].push_back(o);
    } else {
        asks_[o->price].push_back(o);
    }
    index_[o->id] = o;
}

Order* OrderBook::cancel_and_return(uint64_t order_id) {
    auto it = index_.find(order_id);
    if (it == index_.end()) return nullptr;
    Order* o = it->second;
    remove_order(o);
    return o;
}

bool OrderBook::cancel_order(uint64_t order_id) {
    return cancel_and_return(order_id) != nullptr;
}

void OrderBook::remove_order(Order* o) {
    index_.erase(o->id);
    if (o->side == Side::Buy) {
        auto it = bids_.find(o->price);
        if (it != bids_.end()) {
            it->second.total_volume -= o->quantity;
            it->second.remove(o);
            if (it->second.empty()) bids_.erase(it);
        }
    } else {
        auto it = asks_.find(o->price);
        if (it != asks_.end()) {
            it->second.total_volume -= o->quantity;
            it->second.remove(o);
            if (it->second.empty()) asks_.erase(it);
        }
    }
}

PriceLevel* OrderBook::best_bid() noexcept {
    if (bids_.empty()) return nullptr;
    return &bids_.begin()->second;
}

PriceLevel* OrderBook::best_ask() noexcept {
    if (asks_.empty()) return nullptr;
    return &asks_.begin()->second;
}

