#pragma once
#include <chrono>
#include <cstdint>

// Lightweight RAII timer — item 28: replaces repeated t0/t1/elapsed_ns boilerplate.
struct ScopedTimer {
    std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
    uint64_t elapsed_ns() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - t0).count());
    }
};
