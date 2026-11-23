// Its own executable, because replacing the global operator new affects the whole program
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <new>
#include <vector>

#include "FeedBook.h"
#include "Workload.h"
#include "lob/PriceArray.h"
#include "lob/PriceLadder.h"

namespace {
std::size_t g_allocs = 0;
}  // namespace

// The default operator new[] and the nothrow forms call this one, so it counts them too
void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace lob;

constexpr std::size_t WARM_UP = 100'000;
constexpr std::size_t MEASURED = 100'000;

TEST(HotPath, NoAllocationAfterWarmUp) {
    const std::vector<bench::Message> messages = bench::make_workload(WARM_UP + MEASURED, 42);
    OrderBook book;
    std::size_t fills = 0;
    // Only counts: storing the fills would allocate
    auto on_fill = [&fills](const Fill&) { ++fills; };

    for (std::size_t i = 0; i < WARM_UP; ++i) {
        bench::apply(book, messages[i], on_fill);
    }
    const std::size_t before = g_allocs;
    for (std::size_t i = WARM_UP; i < messages.size(); ++i) {
        bench::apply(book, messages[i], on_fill);
    }
    const std::size_t allocs = g_allocs - before;

    EXPECT_EQ(allocs, 0u) << static_cast<double>(allocs) / MEASURED << " allocations per message";
}

// Fills every price in the band in an order that lands at the front, middle and back, then
// erases half and pops the rest. Returns the allocations it made.
template <typename Side>
std::size_t allocations_while_churning(Side& side, const std::size_t levels) {
    const std::size_t before = g_allocs;
    for (std::size_t i = 0; i < levels; ++i) {
        // 7919 is prime, so this visits every price once
        const auto price = static_cast<Price>(i * 7919 % levels);
        side.try_emplace(price).first = static_cast<LevelSlot>(i);
    }
    for (Price price = 0; price < static_cast<Price>(levels); price += 2) {
        side.erase(price);
    }
    while (!side.empty()) {
        side.pop_best();
    }
    return g_allocs - before;
}

TEST(PriceLevels, NoAllocationAfterConstruction) {
    constexpr std::size_t LEVELS = 1000;
    constexpr PriceBand BAND{.lo = 0, .hi = static_cast<Price>(LEVELS)};
    PriceLadder<std::greater<Price>> ladder(BAND, LEVELS);
    PriceArray<std::greater<Price>> array(BAND, LEVELS);
    EXPECT_EQ(allocations_while_churning(ladder, LEVELS), 0u);
    EXPECT_EQ(allocations_while_churning(array, LEVELS), 0u);
}

// The array moves its slots within the vector it already has
TEST(OrderBook, MovingTheBandDoesNotAllocate) {
    OrderBook book(16, {.lo = 100, .hi = 200});
    book.add({.price = 150, .original = 5, .side = Side::Buy, .tif = Tif::Gtc});
    book.add({.price = 160, .original = 5, .side = Side::Sell, .tif = Tif::Gtc});
    // Checked after counting, since a failed EXPECT allocates its message
    const std::size_t before = g_allocs;
    const Result up = book.move_band(120);
    const Result down = book.move_band(80);
    const Result too_far = book.move_band(5000);
    const std::size_t allocs = g_allocs - before;
    EXPECT_EQ(up, Result::Ok);
    EXPECT_EQ(down, Result::Ok);
    EXPECT_EQ(too_far, Result::OutOfBand);
    EXPECT_EQ(allocs, 0u);
}

// The index from the feed's ids to the book's is reserved up front
TEST(FeedBook, NoAllocationAfterConstruction) {
    constexpr std::uint32_t ORDERS = 1000;
    feed::FeedBook book(ORDERS, {.lo = 0, .hi = 1000}, 100);
    const std::size_t before = g_allocs;
    for (int round = 0; round < 3; ++round) {
        // One too many, so the last add is turned away by a full book
        for (std::uint64_t id = 0; id <= ORDERS; ++id) {
            book.apply({.id = id,
                        .size = 10,
                        .price = static_cast<Price>(id % 500) * 100,
                        .type = feed::EventType::Add,
                        .side = Side::Buy});
        }
        for (std::uint64_t id = 0; id < ORDERS; ++id) {
            const auto type = id % 2 == 0 ? feed::EventType::Execute : feed::EventType::Delete;
            book.apply({.id = id,
                        .size = 4,
                        .price = static_cast<Price>(id % 500) * 100,
                        .type = type,
                        .side = Side::Buy});
            book.apply({.id = id,
                        .size = 6,
                        .price = static_cast<Price>(id % 500) * 100,
                        .type = feed::EventType::Execute,
                        .side = Side::Buy});
        }
    }
    const std::size_t allocs = g_allocs - before;
    const std::size_t resting = book.book().size();
    const std::size_t rejected = book.stats().rejected;
    EXPECT_EQ(allocs, 0u);
    EXPECT_EQ(resting, 0u);
    EXPECT_EQ(rejected, 3u);
}
}  // namespace
