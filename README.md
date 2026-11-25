# lob

A single-threaded limit order book in C++20. Orders match by price-time priority, and nothing
on the hot path allocates after construction. On the replay benchmark it handles a message in
about 10.9 ns on average (Apple M2 Pro).

## Features

- Limit and market orders.
- Time in force: GTC, Day, IOC and FOK.
- `submit()` matches an order, then rests what's left if its type and time in force allow it.
  Fills are reported through a callback the caller passes in.
- `cancel()`, and `modify()`. A smaller quantity at the same price keeps the order's place in
  its queue. Any other change is a cancel and replace under the same id, and it trades if the
  new price crosses.
- `end_session()` removes Day orders.
- `move_band()` moves the accepted price range while orders rest.
- Queries: `best_bid()`, `best_ask()`, `spread()`, `size()`, `contains()`, `level_qty()` and
  `orders_at()`.
- Errors are returned as `Result` codes: `UnknownId`, `BadQty`, `BookFull` and `OutOfBand`.

```cpp
#include <lob/OrderBook.h>

lob::OrderBook book;  // 65,536 resting orders, prices [0, 65536) in ticks
auto on_fill = [](const lob::Fill& fill) { /* fill.maker, fill.taker, fill.price, fill.qty */ };

const lob::Placed ask = book.submit(
    {.price = 10'050, .original = 100, .side = lob::Side::Sell, .tif = lob::Tif::Gtc}, on_fill);
book.submit({.price = 10'050, .original = 30, .side = lob::Side::Buy, .tif = lob::Tif::Ioc},
            on_fill);
book.modify(ask.id, 10'060, 50, on_fill);
book.cancel(ask.id);
```

## Design

| Structure | What it is |
|---|---|
| Order pool | One array of 40-byte `OrderNode`s, sized at construction, with the free list threaded through the free nodes. It holds one slot more than the capacity, so a full book can still take an incoming order while it matches. |
| Order ids | The book assigns ids: the node's slot in the low 32 bits and the slot's generation in the high 32. Finding an order needs no lookup table. The generation changes when an order leaves, so an old id stops matching once its slot is reused. |
| Price levels | A level pool of 32-byte `Level`s, each a FIFO linked by 32-bit slot indices, with a running total quantity. |
| Price index | One `PriceArray` per side with a slot for every tick in a fixed band, so finding, adding and removing a level is an array index. The best price is cached. Bids are keyed by `-price`, so both sides share one type and the book picks a side by index rather than by branch. |

A separate test executable replaces the global `operator new` and checks that replaying a
workload after warm-up, and moving the band, allocate nothing.

## Limitations

- Capacity and the price band are fixed at construction. A full book returns `BookFull`, and
  prices outside the band return `OutOfBand`. Nothing moves the band automatically.
- `add()` rests an order without matching it, so it can leave the book crossed. Use
  `submit()`.
- The book assigns ids, so a feed with its own ids needs an index from one to the other.
  `feed/` keeps one outside the book: see "Replaying a LOBSTER file".
- The 32-bit generation wraps after 2^31 orders through one slot.

## Building

Requires CMake 3.25, Ninja and a C++20 compiler. GoogleTest and Google Benchmark are fetched
at configure time.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/lob_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

Presets: `debug`, `release` (thin LTO), `profile` (RelWithDebInfo, frame pointers, thin LTO),
`gcc` (g++-14) and `asan` (address and undefined-behaviour sanitizers).

CMake options:

- `LOB_LTO=OFF|THIN|FULL`: link-time optimisation of the library and its executables.
- `LOB_PRICE_LADDER=ON`: use `PriceLadder`, two sorted vectors with no price band, instead
  of `PriceArray`.

## Layout

```
include/lob/   public headers: OrderBook, CoreTypes, Pool, PriceArray, PriceLadder
src/           OrderBook.cpp and helpers
tests/         behaviour tests, typed tests over both price structures, allocation tests
bench/         workload generator and the replay benchmark
feed/          LOBSTER file reader, a book built from a feed, and a checker
app/           lob_app, which replays a LOBSTER file
```

## Replaying a LOBSTER file

`lob_app` replays a [LOBSTER](https://lobsterdata.com) message file, checks the book against
the matching orderbook file after every event, and times the replay:

```sh
./build/release/lob_app AMZN_2012-06-21_34200000_57600000_message_10.csv \
                        AMZN_2012-06-21_34200000_57600000_orderbook_10.csv
```

```
269748 events: 131954 adds, 2917 reductions, 123458 deletions, 8974 executions, 2445 other
checked 269748 rows, 5342751 levels (12761 synced to the file): 0 mismatches
applied 267303, ignored 2444, unknown id 0, rejected 0; 437 orders resting at the end
replay: 18.3 ns per event (median of 5 runs, best 18.2)
```

It exits with 1 if the book ever differs from the file. The orderbook file is optional, and
no data is in the repo.

- **A feed is already matched.** `FeedBook` rests adds with `add()` and takes executions off
  the order the feed names, so this replay exercises `add`, `cancel` and the in-place
  `modify`, not `submit` or matching.
- **The id index is back.** `FeedBook` maps the feed's order ids to the book's with
  `ankerl::unordered_dense`, reserved up front, so the replay still allocates nothing. The
  18 ns includes it, so it isn't comparable with the 10.9 ns of the generated workload,
  whose ids are the book's own.
- **A message file only covers the levels its orderbook file shows.** Orders from before
  09:30 are never named, and a level that drops out of view changes with no event. The
  checker takes the file's quantity for a price the previous row didn't show, as one
  stand-in order, and compares every price the previous row did show.
- **The timed runs replay the checker's corrections.** The checked run records every change
  the checker makes to the book, and the timed runs make each one after the same event, so
  they work on the same book without reading the orderbook file. A correction is timed as
  one step for each order it takes out and one for the level it leaves, each in constant
  time. `lob_app` exits with 2 if a timed run's counts differ from the checked run's.
  Seeded from the first row alone, the AMZN sample's book drifted: 2.5% of events named
  orders it didn't have, and about 8,300 orders were left resting instead of 437.
- Prices are taken as whole cents, in a band 65,536 cents wide around the first order.
  Anything else is counted as rejected, or skipped by the checker.

## Benchmark

`lob_bench` replays a generated workload (seed 42): 100,000 warm-up messages, then 1,000,000
timed ones. The mix is about 48% resting adds, 37% cancels, 5% modifies and 10% IOC takers,
with about 9,400 orders resting. `BM_Replay` gives the mean cost per message;
`BM_ReplayLatency` times each message for percentiles. `steady_clock` ticks every 41.67 ns on
the test machine, so the percentiles come in steps of that, and the mean is the more useful
number.

## Optimization history

All rows are from the release preset on an Apple M2 Pro with Apple Clang. Each change was
measured against the previous one in alternating runs on the same day. Absolute numbers drift
by about half a nanosecond between days, so the end-to-end ratio is approximate.

| Step | Commit | Change | Mean | p99 | Allocs/msg |
|---|---|---|---|---|---|
| 2 | `5f8e243` | Baseline: `std::map` of levels, `std::list` per level, `std::unordered_map` index | 73.4 ns | 167 ns | 1.05 |
| 3 | `af6e254` | `Result` codes instead of `std::cout`, one hash lookup in `add`, smaller types | 68.7 ns | 166 ns | 1.05 |
| 4 | `ad0066c` (`feat/pmr`, not merged) | Same containers on a `std::pmr` pool | 38.5 ns | 84 ns | 0 |
| 5 | `6629407` | Order pool and intrusive list instead of `std::list` | 51.6 ns | 125 ns | 0.49 |
| 6 | `841cfd3` | `ankerl::unordered_dense` instead of `std::unordered_map` | 27.3 ns | 83 ns | 0.013 |
| 7 | `41dae74`, `fed2dc0` | Level pool; sorted price ladder instead of `std::map` | 26.6 ns | 42 ns | 0 |
| 8 | `6dea803` | Price array indexed by tick | 22.4 ns | 42 ns | 0 |
| 9 | `0d97d55` | Book-assigned ids; hash index removed | 15.2 ns | 42 ns | 0 |
| 12 | `ba56d49` | Sides indexed instead of branched on | 12.2 ns | 42 ns | 0 |
| 14 | `57a062a` | Fix: a modify that crosses now trades | 13.3 ns | 42 ns | 0 |
| 15 | `7c62d0d` | `match` and `take_off_book` forced inline; `modify`'s no-trade path moved back to the `.cpp` | 12.4 ns | 42 ns | 0 |
| 16 | `3c869d4` | Thin LTO in the release and profile presets | 10.9 ns | 42 ns | 0 |
| 17 | `7477eff` | `move_band()`; nothing on the hot path changed | no change | 42 ns | 0 |

Steps 4 and 5 were each measured against step 3 on a different day (step 3 ran 73.5 ns on
one and 68.8 ns on the other), so compare them by their change against it: -48% and -25%. Step 10, a bitmap for finding the
next best price, was skipped because profiling found almost no time in that scan. Steps 11
and 13 were profiling runs with no code change.

Notes:

- **Allocation first.** The `std::pmr` experiment kept the original containers and removed
  only `malloc`. The mean fell 48%.
- **Layout second.** At step 6, the pool and the flat hash map beat the pmr build by 27% while
  still allocating level nodes, so removing pointer chasing was worth at least that much on
  top of removing `malloc`.
- **Not every change helps.** Shrinking the order node from 48 to 40 bytes (step 7a) made no
  measurable difference.
- **The id index was the largest remaining cost.** A profile put the hash index at 31% of
  the loop, about 6.8 ns. Having the book assign ids removed it and saved 6.5 ns.
- **Then branches, not memory.** CPU counters on step 9 showed the core waiting on
  mispredicted branches, not on memory. Order sides are random, so branching on side was
  mispredicted about half the time. Indexing by side saved 3.2 ns.
- **Inlining is fragile.** The step 14 fix only touched `modify` (5% of messages) but cost
  1.1 ns on every message: `match` gained a second caller and stopped being inlined into
  `submit`. Step 15 restored it. Thin LTO then let the helpers in `OrderBook.cpp` inline into
  the header templates, for another 1.7 ns. Full LTO was 0.5 to 1.2 ns slower than thin.
- **What remains is mostly unavoidable.** At step 16 the replay averages 0.66 mispredicted
  branches per message, about 6.3 ns each. Two thirds are the benchmark's own switch on
  message type. Most of the book's share comes from whether an order takes and whether a
  fill empties the maker, which are random in this workload.
