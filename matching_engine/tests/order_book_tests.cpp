#include "matching_engine.hpp"
#include <cstdio>
#include <vector>

// Accumulating failure count — all tests run regardless of earlier failures.
static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while(0)

// ─── Test 1: Basic limit order matching ─────────────────────────────────────
// Post a sell limit at 100, then a buy limit at 100 → one fill.
static void test_basic_match() {
    MatchingEngine me;
    std::vector<Fill> fills;

    me.submit_limit(Side::Sell, 100, 10, fills);
    CHECK(fills.empty());  // no counter-party yet

    fills.clear();
    me.submit_limit(Side::Buy, 100, 10, fills);
    CHECK(fills.size() == 1);
    CHECK(fills[0].price    == 100);
    CHECK(fills[0].quantity == 10);

    // Book should be empty after full fill.
    CHECK(me.book().best_bid() == nullptr);
    CHECK(me.book().best_ask() == nullptr);
    std::puts("PASS test_basic_match");
}

// ─── Test 2: FIFO priority ────────────────────────────────────────────────────
// Two sell orders at the same price; buy should fill the earlier one first.
static void test_fifo_priority() {
    MatchingEngine me;
    std::vector<Fill> fills;

    me.submit_limit(Side::Sell, 100, 5, fills);   // maker A
    me.submit_limit(Side::Sell, 100, 5, fills);   // maker B
    CHECK(fills.empty());

    fills.clear();
    me.submit_limit(Side::Buy, 100, 5, fills);
    CHECK(fills.size() == 1);
    // Maker A had id=1, Maker B had id=2. First fill should be maker A (id 1).
    CHECK(fills[0].maker_id == 1);
    CHECK(fills[0].quantity == 5);

    // Maker B should still be in the book.
    CHECK(me.book().best_ask() != nullptr);
    CHECK(me.book().best_ask()->total_volume == 5);
    std::puts("PASS test_fifo_priority");
}

// ─── Test 3: Partial fill ─────────────────────────────────────────────────────
// Taker buys 3; maker has 10 → 3 filled, 7 remains.
static void test_partial_fill() {
    MatchingEngine me;
    std::vector<Fill> fills;

    me.submit_limit(Side::Sell, 100, 10, fills);

    fills.clear();
    me.submit_limit(Side::Buy, 100, 3, fills);
    CHECK(fills.size() == 1);
    CHECK(fills[0].quantity == 3);

    // 7 should remain on the ask side.
    CHECK(me.book().best_ask() != nullptr);
    CHECK(me.book().best_ask()->total_volume == 7);
    std::puts("PASS test_partial_fill");
}

// ─── Test 4: Cancel order ─────────────────────────────────────────────────────
// Post a sell, cancel it, then send a matching buy → no fill.
static void test_cancel() {
    MatchingEngine me;
    std::vector<Fill> fills;

    me.submit_limit(Side::Sell, 100, 10, fills);   // id = 1
    bool cancelled = me.cancel(1);
    CHECK(cancelled);
    CHECK(me.book().best_ask() == nullptr);

    fills.clear();
    me.submit_limit(Side::Buy, 100, 10, fills);
    CHECK(fills.empty());   // nothing to match against
    std::puts("PASS test_cancel");
}

// ─── Test 5: Market order sweeps multiple levels ──────────────────────────────
// Three ask levels: 100@5, 101@5, 102@5. Market buy 12 → fills 100@5, 101@5, 102@2.
static void test_market_sweep() {
    MatchingEngine me;
    std::vector<Fill> fills;

    me.submit_limit(Side::Sell, 100, 5, fills);
    me.submit_limit(Side::Sell, 101, 5, fills);
    me.submit_limit(Side::Sell, 102, 5, fills);

    fills.clear();
    me.submit_market(Side::Buy, 12, fills);
    CHECK(fills.size() == 3);
    CHECK(fills[0].price == 100 && fills[0].quantity == 5);
    CHECK(fills[1].price == 101 && fills[1].quantity == 5);
    CHECK(fills[2].price == 102 && fills[2].quantity == 2);

    // 3 units should remain at price 102.
    CHECK(me.book().best_ask() != nullptr);
    CHECK(me.book().best_ask()->total_volume == 3);
    std::puts("PASS test_market_sweep");
}

// ─── Test 6: Limit order does not cross worse prices ─────────────────────────
// Ask at 105; buy limit at 100 → no match, rests in book.
static void test_limit_no_cross() {
    MatchingEngine me;
    std::vector<Fill> fills;

    me.submit_limit(Side::Sell, 105, 10, fills);
    fills.clear();
    me.submit_limit(Side::Buy, 100, 10, fills);
    CHECK(fills.empty());
    CHECK(me.book().best_bid()  != nullptr);
    CHECK(me.book().best_ask()  != nullptr);
    std::puts("PASS test_limit_no_cross");
}

// ─── Test 7: total_volume accounting ─────────────────────────────────────────
// Verifies total_volume stays correct after partial fills, cancels, and
// interleaved operations at the same level — the field most prone to staleness.
static void test_volume_accounting() {
    MatchingEngine me;
    std::vector<Fill> fills;

    // Three asks at price 100: qty 10, 20, 30 → total_volume = 60
    me.submit_limit(Side::Sell, 100, 10, fills);  // id 1
    me.submit_limit(Side::Sell, 100, 20, fills);  // id 2
    me.submit_limit(Side::Sell, 100, 30, fills);  // id 3
    CHECK(me.book().best_ask()->total_volume == 60);

    // (a) Partial fill: buy 5 → total_volume should become 55
    fills.clear();
    me.submit_limit(Side::Buy, 100, 5, fills);
    CHECK(fills.size() == 1 && fills[0].quantity == 5);
    CHECK(me.book().best_ask()->total_volume == 55);

    // (b) Cancel id 2 (qty 20 remaining) → total_volume = 35
    CHECK(me.cancel(2));
    CHECK(me.book().best_ask()->total_volume == 35);

    // (c) Cancel id 3 (qty 30) → only id 1's residual (5) remains
    CHECK(me.cancel(3));
    CHECK(me.book().best_ask() != nullptr);
    CHECK(me.book().best_ask()->total_volume == 5);

    // (d) Fill the last 5 → book empty
    fills.clear();
    me.submit_market(Side::Buy, 5, fills);
    CHECK(fills.size() == 1 && fills[0].quantity == 5);
    CHECK(me.book().best_ask() == nullptr);

    std::puts("PASS test_volume_accounting");
}

int main() {
    test_basic_match();
    test_fifo_priority();
    test_partial_fill();
    test_cancel();
    test_market_sweep();
    test_limit_no_cross();
    test_volume_accounting();

    if (g_failures == 0) {
        std::puts("\nAll tests passed.");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", g_failures);
    return 1;
}
