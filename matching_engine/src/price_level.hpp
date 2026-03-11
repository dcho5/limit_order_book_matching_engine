#pragma once
#include "order.hpp"
#include <cstdint>

// FIFO queue of orders at the same price level.
// All operations are O(1) thanks to intrusive linked list pointers on Order.
struct PriceLevel {
    Order*   head         = nullptr;  // oldest (first to match)
    Order*   tail         = nullptr;  // newest
    uint64_t total_volume = 0;

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else      head = o;
        tail = o;
        total_volume += o->quantity;
    }

    // O(1) removal using the order's own prev/next pointers.
    // Does NOT update total_volume — caller must do so explicitly.
    void remove(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next;
        else         head = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail = o->prev;
        o->prev = o->next = nullptr;
    }

    Order* front() const noexcept { return head; }
    bool   empty() const noexcept { return head == nullptr; }
};
