# Limit Order Book Matching Engine

A high-performance C++20 limit order book and matching engine built to demonstrate
systems design, data-structure trade-offs, and performance engineering — topics
commonly explored in FAANG and quant finance technical interviews.

No external dependencies. Pure STL + C++20.

---

## Architecture

```
  ┌───────────────────────────────────────────────────┐
  │               WorkloadGenerator                   │
  │  Produces a stream of: Limit | Market | Cancel    │
  │  Clustered prices · configurable mix · bursty mode│
  └────────────────────────┬──────────────────────────┘
                           │  WorkloadEvent[]
                           ▼
  ┌───────────────────────────────────────────────────┐
  │               MatchingEngine                      │
  │  · Assigns monotonic order IDs                    │
  │  · Routes to match() or rests in the book         │
  │  · Owns: OrderBook  +  OrderPool                  │
  └──────────────┬────────────────────┬───────────────┘
                 │                    │
    ┌────────────▼────────┐  ┌────────▼──────────────┐
    │     OrderPool        │  │      OrderBook         │
    │  pre-alloc 512K slots│  │  add / cancel / match  │
    │  free-list, O(1)     │  │  index: O(1) by ID     │
    └─────────────────────┘  └────────┬───────────────┘
                                      │
                    ┌─────────────────┴──────────────────┐
                    │                                    │
       ┌────────────▼──────────┐   ┌────────────────────▼──────┐
       │         Bids           │   │           Asks             │
       │  map<price, PriceLevel,│   │  map<price, PriceLevel,    │
       │       greater<>>       │   │       less<>>              │
       │  highest price first   │   │  lowest price first        │
       └────────────┬───────────┘   └───────────────┬───────────┘
                    │                               │
                    └───────────────┬───────────────┘
                                    │
                       ┌────────────▼──────────────┐
                       │         PriceLevel         │
                       │  intrusive doubly-linked   │
                       │  list of Order*  (FIFO)    │
                       │                            │
                       │  head ──▶ [O1] ──▶ [O2]   │
                       │          ◀──       ◀──     │
                       └───────────────────────────┘
```

---

## Components

### `Order` (`matching_engine/src/order.hpp`)
The fundamental unit. Fields: `id`, `price`, `quantity`, `side`, and two
intrusive linked-list pointers `prev`/`next`.

Prices are stored as integers (e.g. cents) to eliminate floating-point
comparison bugs — a common source of subtle correctness issues in financial
systems.

### `OrderPool` (`matching_engine/src/order_pool.hpp`)
A free-list allocator with runtime-configurable capacity. Pre-allocates 512K
`Order` slots on the heap at construction time. `acquire()` / `release()` are
O(1) pointer operations — no calls to `malloc`/`free` in the hot path.

### `PriceLevel` (`matching_engine/src/price_level.hpp`)
A FIFO queue of orders at the same price, implemented as an intrusive
doubly-linked list. All operations are O(1):
- `push_back(Order*)` — enqueue at tail
- `remove(Order*)` — splice out using `Order::prev`/`Order::next`
- `front()` — peek at the oldest (highest priority) order

### `OrderBook` (`matching_engine/src/order_book.hpp`)
- **Bids**: `std::map<price, PriceLevel, greater<>>` — highest bid at `begin()`
- **Asks**: `std::map<price, PriceLevel, less<>>` — lowest ask at `begin()`
- **Index**: `std::unordered_map<id, Order*>` — O(1) cancel lookup by order ID
- Automatically removes empty price levels from the map

### `MatchingEngine` (`matching_engine/src/matching_engine.hpp`)
Owns the `OrderBook` and `OrderPool`. Public interface:
```cpp
void submit_limit(Side, uint64_t price, uint64_t qty, std::vector<Fill>& out);
void submit_market(Side, uint64_t qty, std::vector<Fill>& out);
bool cancel(uint64_t order_id) noexcept;
```

### `BaselineMatchingEngine` (`matching_engine/src/baseline_book.hpp`)
A "naive" alternative implementation using `std::list<Order>` per price level.
Every `add_order` triggers a heap allocation. Provided specifically for
benchmarking — demonstrating what the optimizations actually buy.

---

## Complexity

| Operation | Complexity | Notes |
|---|---|---|
| `submit_limit` (no match) | O(log P) | `std::map` insert, P = price levels |
| `submit_limit` (matches K fills) | O(K) | each fill is O(1) |
| `submit_market` sweeping K levels | O(K log P) | one `map::erase` per level |
| `cancel` | **O(1)** | hash map lookup + intrusive list splice |
| `best_bid` / `best_ask` | O(1) | `map::begin()` |

---

## Design Decisions

### 1. Single-threaded, no locking
A real exchange has one matching engine thread per instrument, surrounded by
I/O threads that handle network, sequencing, and persistence. Locking inside
the matching engine would be a bottleneck and is architecturally unnecessary.
The single-threaded design also makes the state machine trivially correct —
no torn reads, no ABA problems, no need for memory fences.

### 2. Price-time priority (FIFO within a level)
Orders at the same price are matched in arrival order. This is the standard
for most equity and futures exchanges (NASDAQ, CME). The intrusive linked list
maintains insertion order naturally: `push_back` appends to the tail;
matching always consumes from the `head`.

### 3. Intrusive linked list for O(1) cancel
In a `std::list<Order>`, the list's internal node pointers live in a separate
heap allocation. To cancel by ID in O(1), you must store a `list::iterator`
in a side-table — an extra allocation and an extra pointer indirection.

With an intrusive list, `Order::prev` and `Order::next` live inside the
`Order` struct itself. The cancel path is:
```
1. unordered_map lookup → Order*          O(1)
2. order->prev->next = order->next        O(1)  no searching
3. pool.release(order)                    O(1)
```
No iterator storage. No extra allocation. The `Order*` from the hash map IS
the iterator.

### 4. `std::map` vs. array-based price ladder
`std::map` is a red-black tree: each node is a separate heap allocation.
For P price levels, insert and lookup are O(log P) with poor cache locality
(pointer chasing across the heap).

A flat sorted array (or a skip list) would be more cache-friendly and could
give O(1) access to the best price. The trade-off is complexity: you need to
manage a dense array indexed by `(price - min_price) / tick_size`, which
requires knowing the price range upfront and handling range expansions.

`std::map` is chosen here because:
- Handles arbitrary price ranges without pre-allocation
- O(log P) is negligible when P is small (typical books have <1000 active levels)
- Code is clean and interview-discussable

A production HFT engine would use a `std::array<PriceLevel, MAX_LEVELS>` with
a fixed tick size, gaining O(1) level access.

### 5. Object pool eliminates malloc from the hot path
`malloc`/`free` are not just slow (~100–200 ns each); they are also
**non-deterministic** — their latency can spike due to lock contention in the
allocator, fragmentation-driven consolidation, or OS page faults.

The `OrderPool` eliminates this variability. All `Order` memory is allocated
once at engine startup. In steady state, `acquire` and `release` are two
pointer assignments each.

### 6. Integer prices, not floating-point
`double` comparison is unreliable for financial data:
```cpp
double a = 1.005;
double b = 1.005;
a != b;   // may be true due to representation error
```
Using `uint64_t` (e.g. price in cents, or price × 10000 for 4 decimal places)
makes equality and ordering deterministic and branch-predictor-friendly.

### 7. Explicit `bool is_market` instead of a sentinel price
The private `match()` method takes a `bool is_market` flag rather than using
`taker_price == 0` as a magic value. Price 0 would be a permanently invalid
order price and leaks an implementation detail into the signature. The explicit
flag makes the intent self-documenting and keeps price 0 available as a valid
tick if the instrument ever needed it.

---

## Optimization Timeline

### Version 1 — `std::map + std::list` (baseline)
```
OrderBook: map<price, list<Order>>
Cancel:    list::iterator stored in unordered_map
```
Every `add_order` allocates a `list` node on the heap. Cancel is O(1) via
the stored iterator, but the iterator is a pointer to a heap-allocated node.
List nodes are scattered across the heap → poor cache locality on iteration.

This is `BaselineMatchingEngine` in the codebase.

### Version 2 — Intrusive doubly-linked list
```
Order: adds prev* and next* fields
PriceLevel: head/tail pointers into the pool
Cancel: Order* (from unordered_map) IS the iterator
```
Eliminates the `list` node allocation entirely. `Order::prev`/`next` take the
place of `list`'s internal node pointers, stored directly inside the `Order`
struct. Cancel no longer needs to store or chase a separate iterator.

### Version 3 — Object pool allocator
```
OrderPool: pre-allocates N Order slots; free-list for O(1) acquire/release
```
Removes all `malloc`/`free` calls from the hot path. Orders come from a
contiguous memory region, improving cache behavior on iteration compared to
scattered heap allocations. Also eliminates allocator latency spikes.

### Version 4 — Pool storage on the heap (`std::vector`)
```
OrderPool: storage_ changed from std::array<Order, N> to std::vector<Order>
```
`std::array<Order, 512K>` would be ~24 MB on the stack → stack overflow.
Moving to `std::vector` heap-allocates the pool storage at construction,
enabling large pool sizes without increasing the stack frame. `acquire` and
`release` remain pure pointer operations — no behavioral change.

---

## Benchmark Results

Workload: 1M events · 60% limits · 30% cancels · 10% market orders
Platform: Apple M-series · clang++ 17 · `-O2`

```
Baseline  — std::map + std::list
  Throughput :  8.5M ops/sec
  p50        :  42 ns
  p99        : 417 ns
  p999       : 1.2 µs

Optimized — intrusive list + object pool
  Throughput : 13.0M ops/sec
  p50        :  42 ns
  p99        : 292 ns
  p999       : 0.8 µs

Speedup: 1.5x throughput · 1.4x p99 · 1.5x p999
```

The p50 is identical — the common path (a cancel that hits the hash map and
returns false) fits in cache for both implementations. The improvements
appear in the tail: the baseline's `list` node allocations cause periodic
allocator stalls that inflate p99 and p999.

---

## Interactive Visualization

The engine exports a structured event log that replays live in a browser —
useful for demonstrating the system to recruiters, hiring managers, and
interviewers without needing a server or any setup beyond opening a file.

### What it shows

```
┌─────────────────────────────────────────────────────────────┐
│  ▲ BIDS              ▼ ASKS              PRICE LADDER       │
│                                                             │
│  330 ████████ 100    101 ████████ 250    103 ████           │
│  320 ███████   99    102 ████████ 450    102 ████████████   │
│  120 ██        98    103 ████     180    101 ██████         │
│   80 █         97    104 ██        80    100 ████████████   │
│                                          99 ████████       │
│  RECENT TRADES                           98 ████████████   │
│  T=14  101  150  BUY                     97 █████          │
│  T=17  100  150  SELL                                       │
└─────────────────────────────────────────────────────────────┘
```

Three live panels update after every event:
- **Order book depth** — bid and ask levels with volume bars (green/red)
- **Price ladder heatmap** — all active price levels; bar opacity scales with liquidity
- **Trade ticker** — recent fills with aggressor side

### How it works

The C++ engine (`export_events`) runs a scripted scenario through 8 phases:

```
Phase 1  Build the book — adds orders at 4 price levels each side
Phase 2  Deepen inner levels — multiple orders at same price (FIFO visible)
Phase 3  Market sweeps — buy/sell 280 units, consumes FIFO queue order
Phase 4  Cancellations — two orders removed, book becomes asymmetric
Phase 5  Replenish — new orders rebuild both sides
Phase 6  Aggressive limits — crossing orders sweep through multiple levels
Phase 7  Steady-state — 100 random limit + market orders
Phase 8  Large sweeps — drains multiple levels, shows deep impact
```

Each step produces one of three JSON event types:
```json
{"type":"add",    "t":0, "id":4,  "side":"sell", "price":101, "qty":150}
{"type":"trade",  "t":14,"price":101,"qty":150,"maker_id":4,"maker_side":"sell"}
{"type":"cancel", "t":20,"id":2}
```

The browser loads this file and maintains its own order book model, applying
events one at a time and re-rendering after each step — exactly mirroring the
engine's internal state changes.

### Run the demo locally

```bash
# 1. Build and generate the event log
cmake -S matching_engine -B matching_engine/build && cmake --build matching_engine/build
./matching_engine/build/export_events   # writes matching_engine/web/data/events.json

# 2. Serve the web directory (fetch() requires HTTP, not file://)
cd matching_engine/web && python3 -m http.server 8080

# 3. Open in browser
open http://localhost:8080
```

### Deploy to GitHub Pages

The `matching_engine/web/` directory is self-contained static HTML — no build
step, no server.

1. Push the repo to GitHub
2. Go to **Settings → Pages → Source** and set the branch/folder to `matching_engine/web/`
3. GitHub Pages serves `index.html` at `https://<user>.github.io/<repo>/`

Add the URL to your resume. Anyone who clicks it sees a live order book replay
without installing anything.

---

## Build & Run

```bash
# Configure and build (Release by default)
cmake -S matching_engine -B matching_engine/build && cmake --build matching_engine/build

# Unit tests
./matching_engine/build/order_book_tests

# Generate visualization event log → matching_engine/web/data/events.json
./matching_engine/build/export_events

# Quick throughput benchmark
./matching_engine/build/benchmark

# Comparison runner: baseline vs optimized, writes matching_engine/bench/results/
./matching_engine/build/benchmark_runner

# Standalone profiling targets
./matching_engine/build/baseline_engine  [total_events]
./matching_engine/build/optimized_engine [total_events]

# Bursty workload
./matching_engine/build/benchmark_runner 1000000 bursty
```

---

## Project Structure

```
matching_engine/
  src/
    order.hpp               Order struct with intrusive prev/next pointers
    fill.hpp                Fill result struct (shared by both engines)
    price_level.hpp         FIFO intrusive queue per price level
    order_pool.hpp          Free-list object pool (heap-backed, runtime capacity)
    order_book.hpp/.cpp     Sorted bid/ask maps + O(1) cancel index
    matching_engine.hpp/.cpp Optimized engine (intrusive list + pool)
    baseline_book.hpp/.cpp  Baseline engine (std::list, for comparison)
    event_logger.hpp        Wraps engine; records events to JSON for visualizer

  bench/
    workload_generator.hpp  Deterministic workload: clustered prices, cancels
    latency_stats.hpp       p50/p99/p999 computation + CSV output
    benchmark_runner.cpp    Comparison runner (both engines, same workload)
    baseline_engine.cpp     Standalone baseline binary (for profiling)
    optimized_engine.cpp    Standalone optimized binary (for profiling)
    export_events.cpp       Generates web/data/events.json for the visualizer
    benchmark.cpp           Quick throughput benchmark

  tests/
    order_book_tests.cpp    7 unit tests (no framework)

  web/
    index.html              Browser visualizer (plain HTML, no frameworks)
    script.js               In-browser matching engine + rendering
    style.css               Dark trading terminal theme
    data/events.json        Pre-generated event log (output of export_events)

  CMakeLists.txt

README.md
```
