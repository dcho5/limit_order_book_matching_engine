#pragma once
#include "fill.hpp"
#include "matching_engine.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ── EventLogger ───────────────────────────────────────────────────────────────
//
// Wraps MatchingEngine and records every observable event to a JSON array
// that can be loaded by the browser visualizer (docs/index.html).
//
// Event schema:
//   {"type":"add",    "t":N, "id":N, "side":"buy"|"sell", "price":N, "qty":N}
//   {"type":"trade",  "t":N, "price":N, "qty":N,
//                     "maker_id":N, "taker_id":N, "maker_side":"buy"|"sell"}
//   {"type":"cancel", "t":N, "id":N}
//
// Ordering guarantee: TRADEs from matching are emitted before the ADD for any
// resting remainder, matching the order in which the book state changes.

class EventLogger {
public:
    EventLogger() = default;

    // Submit a limit order.
    // Logs TRADE events for each fill, then ADD if any quantity rests.
    void submit_limit(Side side, uint64_t price, uint64_t qty) {
        uint64_t taker_id = engine_.peek_next_id();
        fills_.clear();
        engine_.submit_limit(side, price, qty, fills_);

        Side maker_side = opposite(side);
        uint64_t filled = 0;
        for (const auto& f : fills_) {
            emit_trade(f.maker_id, f.taker_id, maker_side, f.price, f.quantity);
            filled += f.quantity;
        }
        if (qty - filled > 0)
            emit_add(taker_id, side, price, qty - filled);
    }

    // Submit a market order (never rests — only TRADE events emitted).
    void submit_market(Side side, uint64_t qty) {
        fills_.clear();
        engine_.submit_market(side, qty, fills_);
        Side maker_side = opposite(side);
        for (const auto& f : fills_)
            emit_trade(f.maker_id, f.taker_id, maker_side, f.price, f.quantity);
    }

    // Cancel a resting order. Emits CANCEL event only if the order exists.
    bool cancel(uint64_t order_id) {
        bool ok = engine_.cancel(order_id);
        if (ok) {
            sep();
            buf_ << "  {\"type\":\"cancel\",\"t\":" << t_++
                 << ",\"id\":"   << order_id << "}";
        }
        return ok;
    }

    // Returns the number of events recorded so far.
    std::size_t event_count() const { return event_count_; }

    // Returns the ID that will be assigned to the next submitted order.
    uint64_t peek_next_id() const { return engine_.peek_next_id(); }

    // Write the accumulated event log as a JSON array to `path`.
    void save(const std::string& path) const {
        std::ofstream f(path);
        if (!f) throw std::runtime_error("EventLogger: cannot write " + path);
        f << "[\n" << buf_.str() << "\n]\n";
    }

private:
    MatchingEngine     engine_;
    std::vector<Fill>  fills_;       // reused across calls — no per-call allocation
    std::ostringstream buf_;
    uint64_t           t_           = 0;
    bool               first_       = true;
    std::size_t        event_count_ = 0;

    static Side opposite(Side s) {
        return (s == Side::Buy) ? Side::Sell : Side::Buy;
    }

    static const char* side_str(Side s) {
        return (s == Side::Buy) ? "buy" : "sell";
    }

    // Writes a comma separator between events (except before the first).
    void sep() {
        if (!first_) buf_ << ",\n";
        first_ = false;
        ++event_count_;
    }

    void emit_add(uint64_t id, Side side, uint64_t price, uint64_t qty) {
        sep();
        buf_ << "  {\"type\":\"add\",\"t\":"   << t_++
             << ",\"id\":"    << id
             << ",\"side\":\"" << side_str(side) << "\""
             << ",\"price\":" << price
             << ",\"qty\":"   << qty << "}";
    }

    void emit_trade(uint64_t maker_id, uint64_t taker_id,
                    Side maker_side, uint64_t price, uint64_t qty) {
        sep();
        buf_ << "  {\"type\":\"trade\",\"t\":" << t_++
             << ",\"price\":"      << price
             << ",\"qty\":"        << qty
             << ",\"maker_id\":"   << maker_id
             << ",\"taker_id\":"   << taker_id
             << ",\"maker_side\":\"" << side_str(maker_side) << "\"}";
    }
};
