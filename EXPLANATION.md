# How This Project Works — A Plain-English Explanation

This document explains the limit order book matching engine from the ground up.
No trading background or C++ experience required.

---

## Part 1 — What Problem Are We Solving?

When you buy a stock on a real exchange, there is no single seller sitting on
the other side waiting for you. Instead, the exchange runs a piece of software
called a **matching engine** that keeps track of everyone who wants to buy or
sell, and automatically pairs them up when their prices agree.

This project builds that software from scratch.

---

## Part 2 — The Basic Concepts

### Buyers, Sellers, and Prices

Imagine you want to buy one share of a company. You have two options:

- **Limit order**: "I will buy, but only at $100 or cheaper." You name your
  price. If nobody is selling at $100 yet, your order sits and waits.
- **Market order**: "I will buy right now at whatever the current price is."
  You accept the best available price immediately.

The same idea applies to sellers, just in reverse.

### The Order Book

The **order book** is a live list of all the waiting orders — everyone who
wants to buy (bids) and everyone who wants to sell (asks) but hasn't been
matched yet.

```
BIDS (buyers waiting)       ASKS (sellers waiting)
─────────────────────       ──────────────────────
$99  ·  300 shares          $101  ·  150 shares
$98  ·  200 shares          $102  ·  200 shares
$97  ·   80 shares          $103  ·   80 shares
```

The highest bid is $99. The lowest ask is $101. The gap between them ($2) is
called the **spread**. No trade happens yet because no buyer is willing to pay
as much as the cheapest seller is asking.

### When a Trade Happens

A trade happens the moment a new order crosses the spread. For example:

- A new buyer arrives and says "I'll pay $101." That crosses the lowest ask
  ($101), so the engine immediately matches them with the waiting seller at
  $101. A trade executes.
- Or a new seller arrives and says "I'll sell for $99." That crosses the
  highest bid ($99), so the engine matches them with the waiting buyer.

The buyer's order is called the **taker** (they took liquidity from the book).
The waiting order that got filled is called the **maker** (they made liquidity
available).

### Price-Time Priority

What if two sellers are both asking $101? The engine uses a simple rule:
**whoever arrived first gets filled first**. Same price, earlier time wins.
This is called price-time priority, or FIFO (first in, first out). It's the
standard rule used by most real exchanges like NASDAQ and the CME.

---

## Part 3 — The Data Structures

This is where the engineering begins. The order book needs to answer these
questions extremely fast — thousands to millions of times per second:

1. What is the best bid price right now?
2. What is the best ask price right now?
3. Does this new order cross anything?
4. Cancel order #4721 — where is it, remove it immediately.

The data structures we choose determine how fast each of those operations is.

### Sorted Maps for the Book

Bids need to be sorted highest-first (we always want the best — i.e. highest —
bid). Asks need to be sorted lowest-first (we always want the best — i.e.
lowest — ask).

The project uses `std::map` from the C++ standard library, which is a
**red-black tree** — a self-balancing sorted structure. Inserting a new price
level or finding the best price is O(log P), where P is the number of distinct
price levels currently in the book (typically fewer than 100).

```
bids_  →  map sorted high → low    best bid = bids_.begin()
asks_  →  map sorted low  → high   best ask = asks_.begin()
```

### FIFO Queues Within Each Price Level

At each price level, multiple orders can be waiting (all asking $101, for
example). They need to be served in arrival order. The project uses an
**intrusive doubly-linked list** for this — explained in detail below.

### A Hash Map for Cancellations

When a user cancels an order, they provide an order ID. The engine needs to
find that order instantly and remove it, without searching through the whole
book. A **hash map** (`std::unordered_map`) stores a mapping from order ID to
a direct pointer to the order in memory. Lookup is O(1) — constant time,
regardless of how many orders are in the book.

---

## Part 4 — The Intrusive Linked List

This is one of the more interesting engineering choices in the project, so it
deserves its own section.

### What is a Linked List?

A linked list is a chain of nodes where each node holds a value and a pointer
to the next node:

```
[Order A] → [Order B] → [Order C] → null
```

You can remove a node from the middle in O(1) if you already have a pointer to
it, because you just rewire the surrounding pointers:

```
Before:  [A] → [B] → [C]
Remove B:  A's "next" now points to C
           C's "prev" now points to A
After:   [A] → [C]
```

### The "Intrusive" Part

In a standard linked list, the chain-link pointers live in a separate
heap-allocated node that wraps your data:

```
heap node: { Order data | next pointer | prev pointer }
```

Every time you add an order, you allocate one of these wrapper nodes. Every
time you remove one, you deallocate it. Those allocations are slow and
unpredictable.

An **intrusive** list instead puts the `prev` and `next` pointers directly
inside the `Order` struct itself:

```cpp
struct Order {
    Order*   prev;       // ← lives inside the order
    Order*   next;       // ← lives inside the order
    uint64_t id;
    uint64_t price;
    uint64_t quantity;
    Side     side;
};
```

Now there is no separate allocation. The order IS the list node. When you have
a pointer to an order, you already have everything you need to splice it out of
the list in O(1).

This matters for **cancellations**: the hash map gives you a direct `Order*`.
That pointer is all you need — you use `order->prev` and `order->next` to
remove it immediately, with no searching and no extra allocation.

---

## Part 5 — The Object Pool

### The Problem With Normal Memory Allocation

Every time C++ code does `new Order(...)`, it calls `malloc` under the hood.
`malloc` maintains a complex internal bookkeeping structure to track which
chunks of memory are free. At 1 million orders per second, those allocations
add up — and worse, they are **unpredictable**: occasionally `malloc` needs
to do housekeeping that stalls your thread for microseconds.

### The Solution: Pre-Allocate Everything Upfront

The `OrderPool` class allocates all the memory it will ever need at startup —
512,000 `Order` slots at once. It then manages those slots itself using a
**free list**: a chain of currently-unused slots linked together through their
`next` pointers.

```
free_head_ → [slot 0] → [slot 1] → [slot 2] → ... → null
```

- **`acquire()`**: Take the slot at the front of the free list. Two pointer
  operations. No `malloc`.
- **`release(slot)`**: Push the slot back onto the front of the free list.
  Two pointer operations. No `free`.

The hot path — submitting and filling thousands of orders per second — never
touches the system allocator at all. This makes latency both lower and more
consistent.

---

## Part 6 — The Matching Loop

When a new order arrives, here is what the engine does, step by step:

### Limit Order (e.g. "Buy 100 shares at $101")

1. **Check for a cross**: Is the best ask ≤ $101? If not, the order rests in
   the book and we're done.
2. **If yes, match**: Take the front order from the best ask level (FIFO).
   Fill as much as possible.
3. **Repeat**: If the taker still has quantity left and the next ask is still
   ≤ $101, continue filling.
4. **Rest the remainder**: If some quantity is unfilled after matching, add
   it to the bid side of the book at $101.

### Market Order (e.g. "Buy 100 shares at any price")

Same as above, but skip the price check entirely. Keep filling until the
quantity is exhausted or the book is empty.

### The Fill Record

Every time a match occurs, the engine records a **Fill**:
```
maker_id: 4      ← which resting order got filled
taker_id: 7      ← the incoming order that triggered the fill
price:    101    ← at what price
quantity: 50     ← how many shares changed hands
```

These fill records are how the outside world knows trades happened — they
would be sent to clearing systems, position trackers, and market data feeds
in a real exchange.

---

## Part 7 — Performance

### How Fast Is It?

Running on an Apple M-series chip with 1 million operations (60% limit orders,
30% cancellations, 10% market orders):

| | Baseline | Optimized |
|---|---|---|
| Throughput | 8.5M ops/sec | 13.0M ops/sec |
| Median latency (p50) | 42 ns | 42 ns |
| 99th percentile (p99) | 417 ns | 292 ns |
| 99.9th percentile (p999) | 1.2 µs | 0.8 µs |

42 nanoseconds is about 130 clock cycles on a modern CPU. In that time, light
travels about 12 meters.

### What Do p50, p99, p999 Mean?

- **p50** (median): Half of all operations completed faster than this. The
  "typical" case.
- **p99**: 99% of operations completed faster than this. 1 in 100 was slower.
- **p999**: 99.9% completed faster than this. 1 in 1000 was slower.

The p50 is identical between baseline and optimized — the common path fits in
cache for both. The gains show up in the **tail** (p99, p999), which is where
the baseline's `malloc`/`free` calls occasionally stall.

### Why Does Tail Latency Matter?

In financial systems, a single slow operation can delay every order that comes
after it. A matching engine that is fast 99% of the time but slow 1% of the
time is still a problem — that 1% affects real trades. Reducing tail latency is
often more valuable than reducing median latency.

---

## Part 8 — The Baseline Engine

The project includes a second, simpler implementation (`BaselineMatchingEngine`)
that uses `std::list<Order>` — the standard library's linked list — instead of
the intrusive one.

Every time an order is added, `std::list` allocates a new heap node. This is
what most engineers write first, and it works correctly. The baseline exists
purely to make the performance comparison concrete: you can run both engines
against the same workload and see the exact speedup the intrusive list and
object pool provide.

---

## Part 9 — The Visualizer

The project includes a browser-based visualization of the order book. The C++
engine runs a scripted scenario and writes every event (add, trade, cancel) to
a JSON file. The browser then replays those events one at a time, updating
three panels:

- **Order book depth**: The bid and ask levels with volume bars, colored green
  and red.
- **Price ladder heatmap**: All active price levels at once, with bar opacity
  proportional to the volume sitting at each level.
- **Trade ticker**: A live feed of recent fills showing price, quantity, and
  which side was the aggressor.

The visualization also has a fully functional in-browser matching engine —
you can place your own limit and market orders and watch them interact with
the simulated flow in real time.

---

## Summary

| Concept | What it is | Why it matters |
|---|---|---|
| Order book | Sorted list of waiting buy/sell orders | Central data structure of any exchange |
| Price-time priority | Same price → earlier order wins | Fair, standard, deterministic |
| Intrusive linked list | `prev`/`next` pointers inside the Order struct | O(1) cancel with no extra allocation |
| Object pool | Pre-allocated block of Order slots | Eliminates `malloc` from the hot path |
| Hash map index | `order_id → Order*` | O(1) cancel lookup |
| Fill record | Log of every matched trade | How the outside world learns trades happened |
| Tail latency | p99/p999 — the slow outliers | More important than median in financial systems |
