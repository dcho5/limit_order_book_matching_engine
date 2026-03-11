#pragma once
#include <cstdint>

// A single matched trade between a resting maker and an incoming taker.
// Shared by both the optimized and baseline matching engines.
struct Fill {
    uint64_t maker_id;
    uint64_t taker_id;
    uint64_t price;
    uint64_t quantity;
};
