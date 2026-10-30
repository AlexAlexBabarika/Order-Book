// Its own executable, because replacing the global operator new affects the whole program
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include "Workload.h"

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
}  // namespace
