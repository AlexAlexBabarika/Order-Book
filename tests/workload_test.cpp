#include "Workload.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace {
using namespace lob;

constexpr std::size_t COUNT = 100'000;

struct Outcome {
    std::size_t size;
    std::size_t fills;
};

Outcome replay(const std::vector<bench::Message>& messages) {
    OrderBook book;
    std::size_t fills = 0;
    auto on_fill = [&fills](const Fill&) { ++fills; };
    for (const bench::Message& msg : messages) {
        bench::apply(book, msg, on_fill);
    }
    return {.size = book.size(), .fills = fills};
}

TEST(Workload, CancelsAndModifiesTargetLiveOrders) {
    const std::vector<bench::Message> messages = bench::make_workload(COUNT, 1);
    OrderBook book;
    auto ignore = [](const Fill&) {};
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].type != bench::MsgType::Submit) {
            ASSERT_TRUE(book.contains(messages[i].id)) << "message " << i;
        }
        bench::apply(book, messages[i], ignore);
    }
}

TEST(Workload, SameSeedGivesSameStream) {
    const Outcome first = replay(bench::make_workload(COUNT, 7));
    const Outcome second = replay(bench::make_workload(COUNT, 7));
    EXPECT_EQ(first.size, second.size);
    EXPECT_EQ(first.fills, second.fills);
}

TEST(Workload, DifferentSeedGivesDifferentStream) {
    EXPECT_NE(replay(bench::make_workload(COUNT, 7)).fills,
              replay(bench::make_workload(COUNT, 8)).fills);
}

TEST(Workload, DepthStaysNearTarget) {
    const std::vector<bench::Message> messages = bench::make_workload(COUNT, 1);
    OrderBook book;
    auto ignore = [](const Fill&) {};
    for (std::size_t i = 0; i < messages.size(); ++i) {
        bench::apply(book, messages[i], ignore);
        ASSERT_LE(book.size(), bench::TARGET_DEPTH) << "message " << i;
        // The book fills up within the first half
        if (i >= COUNT / 2) {
            ASSERT_GE(book.size(), bench::TARGET_DEPTH / 2) << "message " << i;
        }
    }
}

TEST(Workload, TakersTrade) { EXPECT_GT(replay(bench::make_workload(COUNT, 1)).fills, COUNT / 20); }
}  // namespace
