// export_events.cpp
//
// Generates a structured event log (web/data/events.json) that the browser
// visualizer (web/index.html) can replay interactively.
//
// The scenario tells a complete story:
//   Phase 1 — Build the book (both sides, 4 price levels each)
//   Phase 2 — Deepen inner levels (FIFO priority becomes visible)
//   Phase 3 — Market sweeps consume depth
//   Phase 4 — Cancellations create asymmetric depth
//   Phase 5 — Book replenishes
//   Phase 6 — Aggressive limit orders cross the spread
//   Phase 7 — Steady-state random activity
//   Phase 8 — Large final sweeps drain multiple levels
//
// Usage:
//   ./export_events [output_path]
//   Default output: web/data/events.json

#include "event_logger.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : "web/data/events.json";

    EventLogger log;

    // ── Phase 1: Build initial book ──────────────────────────────────────────
    // Establishes 4 ask levels (101–104) and 4 bid levels (97–100).
    // Order IDs 1–8.
    log.submit_limit(Side::Sell, 104,  80);
    log.submit_limit(Side::Sell, 103, 120);
    log.submit_limit(Side::Sell, 102, 200);
    log.submit_limit(Side::Sell, 101, 150);   // id=4 — best ask

    log.submit_limit(Side::Buy,   97,  80);
    log.submit_limit(Side::Buy,   98, 120);
    log.submit_limit(Side::Buy,   99, 200);
    log.submit_limit(Side::Buy,  100, 150);   // id=8 — best bid

    // ── Phase 2: Deepen inner levels (FIFO priority) ─────────────────────────
    // Multiple orders at price 101 and 100 — earlier orders fill first.
    // Order IDs 9–14.
    log.submit_limit(Side::Sell, 101, 100);   // id=9  (behind id=4 in queue)
    log.submit_limit(Side::Sell, 101,  80);   // id=10 (behind id=9)
    log.submit_limit(Side::Sell, 102, 100);   // id=11

    log.submit_limit(Side::Buy,  100, 100);   // id=12
    log.submit_limit(Side::Buy,  100,  80);   // id=13
    log.submit_limit(Side::Buy,   99, 120);   // id=14

    // Book state now:
    //   asks: 101→330(3 orders), 102→300, 103→120, 104→80
    //   bids: 100→330(3 orders), 99→320, 98→120, 97→80

    // ── Phase 3: Market sweeps ────────────────────────────────────────────────
    // Market buy 280: consumes FIFO from ask 101 (150+100+30 of 80).
    // Generates 3 TRADE events. Market orders consume ID 15 and 16.
    log.submit_market(Side::Buy,  280);

    // Market sell 280: consumes FIFO from bid 100 (150+100+30 of 80).
    // Generates 3 TRADE events.
    log.submit_market(Side::Sell, 280);

    // Book state now:
    //   asks: 101→50 (id=10, partial), 102→300, 103→120, 104→80
    //   bids: 100→50 (id=13, partial), 99→320,  98→120, 97→80

    // ── Phase 4: Cancellations ────────────────────────────────────────────────
    // Cancel id=2 (SELL 103, qty=120) — removes the 103 ask level entirely.
    // Cancel id=7 (BUY  99,  qty=200) — reduces the 99 bid level.
    log.cancel(2);
    log.cancel(7);

    // Book state now:
    //   asks: 101→50, 102→300, 104→80       (103 level removed)
    //   bids: 100→50, 99→120 (id=14 only), 98→120, 97→80

    // ── Phase 5: Book replenishes ─────────────────────────────────────────────
    // New orders rebuild both sides. IDs 17–22.
    log.submit_limit(Side::Sell, 101, 200);   // id=17
    log.submit_limit(Side::Sell, 102, 150);   // id=18
    log.submit_limit(Side::Sell, 103, 180);   // id=19
    log.submit_limit(Side::Buy,  100, 200);   // id=20
    log.submit_limit(Side::Buy,   99, 150);   // id=21
    log.submit_limit(Side::Buy,   98, 180);   // id=22

    // Book state now:
    //   asks: 101→250, 102→450, 103→180, 104→80
    //   bids: 100→250, 99→270,  98→300,  97→80

    // ── Phase 6: Aggressive limit orders cross the spread ────────────────────
    // Limit buy @ 102 for 300: crosses asks at 101 (all 250) then 102 (50 more).
    // Shows multi-level sweep from a single limit order. ID=23.
    log.submit_limit(Side::Buy, 102, 300);

    // Limit sell @ 99 for 300: crosses bids at 100 (all 250) then 99 (50 more).
    // ID=24.
    log.submit_limit(Side::Sell, 99, 300);

    // ── Phase 7: Steady-state random activity ────────────────────────────────
    // Simple deterministic random loop — 70% limit orders, 30% market orders.
    // Prices stay within 97–103 to keep the book depth visible.
    uint64_t rng = 0xC0FFEE42DEADULL;
    auto rand_next = [&]() -> uint64_t {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng;
    };

    for (int i = 0; i < 100; ++i) {
        uint64_t r   = rand_next();
        Side     side = ((r >> 8) & 1) ? Side::Buy : Side::Sell;
        uint64_t qty  = 30 + (rand_next() % 120);

        if ((r % 10) < 3) {
            // Market order (30%)
            log.submit_market(side, qty);
        } else {
            // Limit order at inner levels (70%)
            uint64_t level = (rand_next() % 3) + 1;   // 1, 2, or 3 ticks from mid
            uint64_t price = (side == Side::Buy)
                             ? 100 - level    // 97, 98, 99
                             : 100 + level;   // 101, 102, 103
            log.submit_limit(side, price, qty);
        }
    }

    // ── Phase 8: Large final sweeps ───────────────────────────────────────────
    // Drain multiple levels to show deep impact on the order book.
    log.submit_market(Side::Buy,  600);
    log.submit_market(Side::Sell, 600);

    // ── Output ────────────────────────────────────────────────────────────────
    std::filesystem::create_directories(
        std::filesystem::path(out).parent_path());
    log.save(out);
    std::printf("Wrote %zu events to %s\n", log.event_count(), out);

    return 0;
}
