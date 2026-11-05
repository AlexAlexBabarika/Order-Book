#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <vector>

#include "lob/PriceLadder.h"

namespace {
using namespace lob;

using BidLadder = PriceLadder<std::greater<Price>>;
using AskLadder = PriceLadder<std::less<Price>>;

// Prices from best to worst, the order matching walks them in
template <typename Better>
std::vector<Price> best_first(const PriceLadder<Better>& ladder) {
    std::vector<Price> prices;
    for (std::size_t i = ladder.size(); i-- > 0;) {
        prices.push_back(ladder.price_at(i));
    }
    return prices;
}

template <typename Better>
void add(PriceLadder<Better>& ladder, const Price price, const LevelSlot slot) {
    auto [level, inserted] = ladder.try_emplace(price);
    ASSERT_TRUE(inserted) << "price " << price << " was already there";
    level = slot;
}

// 101, then 103 (new best), then 100 (new worst), then 102 (in the middle)
template <typename Better>
void add_out_of_order(PriceLadder<Better>& ladder) {
    add(ladder, 101, 1);
    add(ladder, 103, 3);
    add(ladder, 100, 0);
    add(ladder, 102, 2);
}

TEST(PriceLadder, NewLadderIsEmpty) {
    const BidLadder ladder;
    EXPECT_TRUE(ladder.empty());
    EXPECT_EQ(ladder.size(), 0u);
    EXPECT_EQ(ladder.find(100), NIL);
}

TEST(PriceLadder, BidsKeepHighestPriceAtBack) {
    BidLadder ladder;
    add_out_of_order(ladder);
    EXPECT_EQ(ladder.best_price(), 103);
    EXPECT_EQ(best_first(ladder), (std::vector<Price>{103, 102, 101, 100}));
}

TEST(PriceLadder, AsksKeepLowestPriceAtBack) {
    AskLadder ladder;
    add_out_of_order(ladder);
    EXPECT_EQ(ladder.best_price(), 100);
    EXPECT_EQ(best_first(ladder), (std::vector<Price>{100, 101, 102, 103}));
}

// Inserting in the middle shifts both arrays, so each slot has to move with its price
TEST(PriceLadder, SlotsStayWithTheirPrices) {
    BidLadder ladder;
    add_out_of_order(ladder);
    EXPECT_EQ(ladder.best_slot(), 3u);
    for (Price price = 100; price <= 103; ++price) {
        EXPECT_EQ(ladder.find(price), static_cast<LevelSlot>(price - 100)) << "price " << price;
    }
}

TEST(PriceLadder, TryEmplaceOnExistingPriceReturnsItsSlot) {
    AskLadder ladder;
    add(ladder, 100, 7);
    const auto [level, inserted] = ladder.try_emplace(100);
    EXPECT_FALSE(inserted);
    EXPECT_EQ(level, 7u);
    EXPECT_EQ(ladder.size(), 1u);
}

TEST(PriceLadder, NewPriceStartsWithNoSlot) {
    BidLadder ladder;
    const auto [level, inserted] = ladder.try_emplace(100);
    EXPECT_TRUE(inserted);
    EXPECT_EQ(level, NIL);
}

TEST(PriceLadder, FindMissingPriceReturnsNil) {
    AskLadder ladder;
    add(ladder, 100, 0);
    add(ladder, 102, 2);
    EXPECT_EQ(ladder.find(99), NIL);
    EXPECT_EQ(ladder.find(101), NIL);
    EXPECT_EQ(ladder.find(103), NIL);
}

TEST(PriceLadder, EraseMiddleKeepsTheRestInOrder) {
    BidLadder ladder;
    add_out_of_order(ladder);
    ladder.erase(102);
    EXPECT_EQ(best_first(ladder), (std::vector<Price>{103, 101, 100}));
    EXPECT_EQ(ladder.find(102), NIL);
    EXPECT_EQ(ladder.find(101), 1u);
}

TEST(PriceLadder, EraseMissingPriceChangesNothing) {
    AskLadder ladder;
    add_out_of_order(ladder);
    ladder.erase(99);
    EXPECT_EQ(best_first(ladder), (std::vector<Price>{100, 101, 102, 103}));
}

TEST(PriceLadder, PopBestMakesNextPriceBest) {
    AskLadder ladder;
    add_out_of_order(ladder);
    ladder.pop_best();
    EXPECT_EQ(ladder.best_price(), 101);
    EXPECT_EQ(ladder.best_slot(), 1u);
    EXPECT_EQ(ladder.size(), 3u);
}

TEST(PriceLadder, ErasedPriceCanBeAddedAgain) {
    BidLadder ladder;
    add(ladder, 100, 0);
    ladder.erase(100);
    EXPECT_TRUE(ladder.empty());
    add(ladder, 100, 5);
    EXPECT_EQ(ladder.find(100), 5u);
}

}  // namespace
