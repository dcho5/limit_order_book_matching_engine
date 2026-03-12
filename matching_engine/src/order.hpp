#pragma once
#include <cstdint>

enum class Side : uint8_t { Buy, Sell };

// Field order places intrusive pointers first so side (1 byte) trails after
// all 8-byte fields, with padding only at the struct tail.
// Total size: 6 × 8 + 1 + 7 pad = 48 bytes = ¾ of a 64-byte cache line.
struct Order {
    // Intrusive doubly-linked list pointers for O(1) removal from PriceLevel
    Order*   prev     = nullptr;
    Order*   next     = nullptr;

    uint64_t id       = 0;
    uint64_t price    = 0;  // integer price (e.g. cents)
    uint64_t quantity = 0;
    Side     side     = Side::Buy;
};

// Native 64-bit: 2×8 + 3×8 + 1 + 7pad = 48 bytes (¾ cache line)
// WASM 32-bit:   2×4 + 3×8 + 1 + 7pad = 40 bytes
static_assert(sizeof(Order) == 48 || sizeof(Order) == 40,
    "Order layout changed — verify cache-line impact before proceeding");
