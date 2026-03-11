#pragma once
#include "order.hpp"
#include <cassert>
#include <cstddef>
#include <new>
#include <vector>

// Fixed-capacity object pool for Order reuse.
//
// Design:
//   - Storage is a heap-allocated std::vector<Order> (avoids stack overflow
//     with large N; a std::array<Order, 512K> would be ~20 MB on the stack).
//   - A singly-linked free-list threads through the pool using Order::next.
//   - acquire() pops from the free-list head — O(1), no malloc.
//   - release() pushes back to the free-list head — O(1), no free().
//
// Why this matters:
//   Without a pool, every limit order that rests in the book requires a
//   separate heap allocation.  At 1M orders/sec that is ~1M malloc/free
//   calls per second — a major bottleneck.  The pool pre-allocates all
//   storage upfront and recycles it via the free-list.

class OrderPool {
public:
    // capacity: number of Order slots pre-allocated on the heap.
    // Default 2^19 = 524 288 slots ≈ 20 MB.
    explicit OrderPool(std::size_t capacity = (1 << 19))
        : storage_(capacity), capacity_(capacity)
    {
        // Thread the free-list through pool_[0] → pool_[1] → … → pool_[N-1].
        for (std::size_t i = 0; i + 1 < capacity; ++i)
            storage_[i].next = &storage_[i + 1];
        if (capacity > 0) {
            storage_[capacity - 1].next = nullptr;
            free_head_ = &storage_[0];
        }
    }

    Order* acquire() {
        // Unconditional guard — works in Release (NDEBUG) builds too.
        if (!free_head_) [[unlikely]]
            throw std::bad_alloc{};
        assert(free_head_ && "OrderPool exhausted — increase N");
        Order* o    = free_head_;
        free_head_  = o->next;
        // Caller must set id, price, quantity, side before use.
        // Only clear intrusive pointers — they carry stale values from prior use.
        o->prev = o->next = nullptr;
        return o;
    }

    void release(Order* o) {
        o->next    = free_head_;
        free_head_ = o;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    std::vector<Order> storage_;
    std::size_t        capacity_  = 0;
    Order*             free_head_ = nullptr;
};
