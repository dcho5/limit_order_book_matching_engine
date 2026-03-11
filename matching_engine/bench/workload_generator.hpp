#pragma once
#include "order.hpp"
#include <cstdint>
#include <stdexcept>
#include <vector>

// ── WorkloadConfig ────────────────────────────────────────────────────────────

struct WorkloadConfig {
    uint32_t total_events = 1'000'000;

    // Order-type mix (fractions must sum to ≤ 1.0).
    double limit_frac  = 0.60;   // resting limit orders
    double cancel_frac = 0.30;   // cancellations of live orders
    double market_frac = 0.10;   // market orders (no resting remainder)

    // Price structure.
    // Buys cluster below mid_price; sells cluster above mid_price.
    uint64_t mid_price    = 100;  // e.g. $100 represented as integer
    uint32_t price_levels = 10;   // distinct price points per side
    uint64_t tick_size    = 1;    // price increment between levels

    // Order size range [qty_min, qty_max].
    uint64_t qty_min = 1;
    uint64_t qty_max = 50;

    // Bursty mode: periodic spikes of concentrated order flow near the spread.
    // Models real market behaviour (e.g. economic data releases).
    bool     bursty      = false;
    uint32_t burst_every = 50'000;  // normal events between each burst
    uint32_t burst_len   = 5'000;   // duration of each burst (events)

    uint64_t seed = 0xC0FFEE42ULL;

    // Validate configuration; throws std::invalid_argument on bad config.
    void validate() const {
        if (limit_frac + cancel_frac + market_frac > 1.0 + 1e-9)
            throw std::invalid_argument(
                "WorkloadConfig: limit_frac + cancel_frac + market_frac > 1.0");
        if (qty_min > qty_max)
            throw std::invalid_argument(
                "WorkloadConfig: qty_min > qty_max");
        if (price_levels == 0)
            throw std::invalid_argument(
                "WorkloadConfig: price_levels must be > 0");
        if (tick_size == 0)
            throw std::invalid_argument(
                "WorkloadConfig: tick_size must be > 0");
    }
};

// ── WorkloadEvent ─────────────────────────────────────────────────────────────

struct WorkloadEvent {
    enum class Type : uint8_t { Limit, Market, Cancel };

    Type     type  = Type::Limit;
    Side     side  = Side::Buy;
    uint64_t price = 0;    // limit price; 0 for Market/Cancel
    uint64_t qty   = 0;    // 0 for Cancel
    uint64_t id    = 0;    // Limit: assigned order id; Cancel: id to cancel
};

// ── WorkloadGenerator ─────────────────────────────────────────────────────────
//
// Generates a fully deterministic, reproducible sequence of WorkloadEvents.
//
// Cancel events are built against a live-set of submitted order IDs so that
// cancellations reference real orders — matching how a market-data replay
// works in production systems.  (At runtime some of those IDs may already
// have been filled by market orders, so the actual cancel hit-rate is
// slightly below 100%, which is realistic.)

class WorkloadGenerator {
public:
    explicit WorkloadGenerator(WorkloadConfig cfg = {})
        : cfg_(cfg), rng_(cfg.seed) {}

    std::vector<WorkloadEvent> generate() {
        std::vector<WorkloadEvent> events;
        events.reserve(cfg_.total_events);

        // live_ids: orders known to have been submitted but not yet cancelled.
        // swap-erase gives O(1) removal when we pick a cancel target.
        std::vector<uint64_t> live_ids;
        live_ids.reserve(4096);

        uint64_t     next_id        = 1;
        const double cancel_thresh  = cfg_.limit_frac + cfg_.cancel_frac;

        for (uint32_t i = 0; i < cfg_.total_events; ++i) {

            // ── Burst mode ─────────────────────────────────────────────────
            // During a burst window, emit limit orders at the nearest price
            // level to the mid, simulating concentrated aggressive flow.
            if (cfg_.bursty && (i % cfg_.burst_every) < cfg_.burst_len) {
                WorkloadEvent e;
                e.type  = WorkloadEvent::Type::Limit;
                e.side  = rand_bool() ? Side::Buy : Side::Sell;
                e.price = (e.side == Side::Buy)
                          ? cfg_.mid_price - cfg_.tick_size
                          : cfg_.mid_price + cfg_.tick_size;
                e.qty   = rand_qty();
                e.id    = next_id++;
                live_ids.push_back(e.id);
                events.push_back(e);
                continue;
            }

            // ── Normal distribution ────────────────────────────────────────
            double r = rand_unit();
            WorkloadEvent e;

            if (r < cfg_.limit_frac) {
                e.type  = WorkloadEvent::Type::Limit;
                e.side  = rand_bool() ? Side::Buy : Side::Sell;
                e.price = clustered_price(e.side);
                e.qty   = rand_qty();
                e.id    = next_id++;
                live_ids.push_back(e.id);

            } else if (r < cancel_thresh && !live_ids.empty()) {
                // Pick a random live order to cancel.
                uint64_t idx = rand_bounded(live_ids.size());
                e.type = WorkloadEvent::Type::Cancel;
                e.id   = live_ids[idx];
                live_ids[idx] = live_ids.back();   // swap-erase: O(1)
                live_ids.pop_back();

            } else {
                e.type = WorkloadEvent::Type::Market;
                e.side = rand_bool() ? Side::Buy : Side::Sell;
                e.qty  = rand_qty();
            }

            events.push_back(e);
        }

        return events;
    }

    const WorkloadConfig& config() const { return cfg_; }

private:
    WorkloadConfig cfg_;
    uint64_t       rng_;

    // LCG: fast, period 2^64, fully deterministic given seed.
    uint64_t next_rand() {
        rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng_;
    }

    // Uniform double in [0, 1).
    double rand_unit() {
        return double(next_rand() >> 11) * (1.0 / double(1ULL << 53));
    }

    bool rand_bool() { return (next_rand() >> 63) != 0; }

    // Lemire's nearly-divisionless method: uniform in [0, n) with negligible bias.
    uint64_t rand_bounded(uint64_t n) {
        uint64_t x = next_rand();
        __uint128_t m = static_cast<__uint128_t>(x) * static_cast<__uint128_t>(n);
        return static_cast<uint64_t>(m >> 64);
    }

    uint64_t rand_qty() {
        return cfg_.qty_min + rand_bounded(cfg_.qty_max - cfg_.qty_min + 1);
    }

    // Prices cluster near the spread: level 0 = closest to mid (most liquid),
    // level N-1 = furthest from mid (least liquid).
    uint64_t clustered_price(Side side) {
        uint64_t level = rand_bounded(cfg_.price_levels);
        if (side == Side::Buy)
            return cfg_.mid_price - (level + 1) * cfg_.tick_size;
        else
            return cfg_.mid_price + (level + 1) * cfg_.tick_size;
    }
};
