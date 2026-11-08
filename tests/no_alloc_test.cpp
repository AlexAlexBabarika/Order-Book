// Its own executable, because replacing the global operator new affects the whole program
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <new>
#include <vector>

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
}  // namespace
