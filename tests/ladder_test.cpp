#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "lob/PriceArray.h"
#include "lob/PriceLadder.h"

namespace {
using namespace lob;

struct LadderLevels {
    template <typename Better>
    using Side = PriceLadder<Better>;
};
struct ArrayLevels {
    template <typename Better>
    using Side = PriceArray<Better>;
};

constexpr PriceBand BAND{.lo = 0, .hi = 1000};
constexpr std::size_t MAX_LEVELS = 64;

template <typename Levels>
class PriceLevels : public ::testing::Test {
   protected:
    typename Levels::template Side<std::greater<Price>> bids{BAND, MAX_LEVELS};
    typename Levels::template Side<std::less<Price>> asks{BAND, MAX_LEVELS};
};

struct LevelsName {
    template <typename Levels>
    static std::string GetName(int) {
        return std::is_same_v<Levels, LadderLevels> ? "Ladder" : "Array";
    }
};

using Implementations = ::testing::Types<LadderLevels, ArrayLevels>;
TYPED_TEST_SUITE(PriceLevels, Implementations, LevelsName);

// Prices from best to worst, the order matching walks them in
template <typename Side>
std::vector<Price> best_first(const Side& side) {
    std::vector<Price> prices;
    side.for_each_from_best([&prices](const Price price, LevelSlot) {
        prices.push_back(price);
        return true;
    });
    return prices;
}

template <typename Side>
void add(Side& side, const Price price, const LevelSlot slot) {
    auto [level, inserted] = side.try_emplace(price);
    ASSERT_TRUE(inserted) << "price " << price << " was already there";
    level = slot;
}

// 101, then 103 (new best), then 100 (new worst), then 102 (in the middle)
template <typename Side>
void add_out_of_order(Side& side) {
    add(side, 101, 1);
    add(side, 103, 3);
    add(side, 100, 0);
    add(side, 102, 2);
}

// Gaps of 5 ticks between levels, added as worst, best, middle
template <typename Side>
void add_with_gaps(Side& side) {
    add(side, 100, 0);
    add(side, 110, 2);
    add(side, 105, 1);
}

TYPED_TEST(PriceLevels, NewSideIsEmpty) {
    EXPECT_TRUE(this->bids.empty());
    EXPECT_EQ(this->bids.size(), 0u);
    EXPECT_EQ(this->bids.find(100), NIL);
    EXPECT_EQ(best_first(this->bids), std::vector<Price>{});
}

TYPED_TEST(PriceLevels, BidsPutHighestPriceFirst) {
    add_out_of_order(this->bids);
    EXPECT_EQ(this->bids.best_price(), 103);
    EXPECT_EQ(best_first(this->bids), (std::vector<Price>{103, 102, 101, 100}));
}

TYPED_TEST(PriceLevels, AsksPutLowestPriceFirst) {
    add_out_of_order(this->asks);
    EXPECT_EQ(this->asks.best_price(), 100);
    EXPECT_EQ(best_first(this->asks), (std::vector<Price>{100, 101, 102, 103}));
}

// The ladder shifts both arrays on an insert in the middle, so each slot has to move with
// its price
TYPED_TEST(PriceLevels, SlotsStayWithTheirPrices) {
    add_out_of_order(this->bids);
    EXPECT_EQ(this->bids.best_slot(), 3u);
    for (Price price = 100; price <= 103; ++price) {
        EXPECT_EQ(this->bids.find(price), static_cast<LevelSlot>(price - 100)) << "price " << price;
    }
}

TYPED_TEST(PriceLevels, TryEmplaceOnExistingPriceReturnsItsSlot) {
    add(this->asks, 100, 7);
    const auto [level, inserted] = this->asks.try_emplace(100);
    EXPECT_FALSE(inserted);
    EXPECT_EQ(level, 7u);
    EXPECT_EQ(this->asks.size(), 1u);
}

TYPED_TEST(PriceLevels, NewPriceStartsWithNoSlot) {
    const auto [level, inserted] = this->bids.try_emplace(100);
    EXPECT_TRUE(inserted);
    EXPECT_EQ(level, NIL);
}

TYPED_TEST(PriceLevels, FindMissingPriceReturnsNil) {
    add(this->asks, 100, 0);
    add(this->asks, 102, 2);
    EXPECT_EQ(this->asks.find(99), NIL);
    EXPECT_EQ(this->asks.find(101), NIL);
    EXPECT_EQ(this->asks.find(103), NIL);
}

TYPED_TEST(PriceLevels, FindAndEraseOutsideBandDoNothing) {
    add(this->bids, 100, 0);
    EXPECT_EQ(this->bids.find(BAND.lo - 1), NIL);
    EXPECT_EQ(this->bids.find(BAND.hi), NIL);
    this->bids.erase(BAND.lo - 1);
    this->bids.erase(BAND.hi);
    EXPECT_EQ(best_first(this->bids), std::vector<Price>{100});
}

TYPED_TEST(PriceLevels, EraseMiddleKeepsTheRestInOrder) {
    add_out_of_order(this->bids);
    this->bids.erase(102);
    EXPECT_EQ(best_first(this->bids), (std::vector<Price>{103, 101, 100}));
    EXPECT_EQ(this->bids.find(102), NIL);
    EXPECT_EQ(this->bids.find(101), 1u);
    EXPECT_EQ(this->bids.size(), 3u);
}

TYPED_TEST(PriceLevels, EraseMissingPriceChangesNothing) {
    add_out_of_order(this->asks);
    this->asks.erase(99);
    EXPECT_EQ(best_first(this->asks), (std::vector<Price>{100, 101, 102, 103}));
}

TYPED_TEST(PriceLevels, PopBestMakesNextPriceBest) {
    add_out_of_order(this->asks);
    this->asks.pop_best();
    EXPECT_EQ(this->asks.best_price(), 101);
    EXPECT_EQ(this->asks.best_slot(), 1u);
    EXPECT_EQ(this->asks.size(), 3u);
}

TYPED_TEST(PriceLevels, ErasedPriceCanBeAddedAgain) {
    add(this->bids, 100, 0);
    this->bids.erase(100);
    EXPECT_TRUE(this->bids.empty());
    add(this->bids, 100, 5);
    EXPECT_EQ(this->bids.find(100), 5u);
}

TYPED_TEST(PriceLevels, WalkSkipsGapsBetweenPrices) {
    add_with_gaps(this->bids);
    add_with_gaps(this->asks);
    EXPECT_EQ(best_first(this->bids), (std::vector<Price>{110, 105, 100}));
    EXPECT_EQ(best_first(this->asks), (std::vector<Price>{100, 105, 110}));
}

TYPED_TEST(PriceLevels, PopBestSkipsGaps) {
    add_with_gaps(this->asks);
    this->asks.pop_best();
    EXPECT_EQ(this->asks.best_price(), 105);
    EXPECT_EQ(this->asks.best_slot(), 1u);
    this->asks.pop_best();
    EXPECT_EQ(this->asks.best_price(), 110);
    this->asks.pop_best();
    EXPECT_TRUE(this->asks.empty());
}

TYPED_TEST(PriceLevels, EraseBestSkipsGaps) {
    add_with_gaps(this->bids);
    this->bids.erase(110);
    EXPECT_EQ(this->bids.best_price(), 105);
    EXPECT_EQ(this->bids.best_slot(), 1u);
}

TYPED_TEST(PriceLevels, EmptiedSideTakesAnyPriceAsBest) {
    add(this->bids, 500, 0);
    this->bids.pop_best();
    add(this->bids, 100, 1);
    EXPECT_EQ(this->bids.best_price(), 100);

    add(this->asks, 100, 0);
    this->asks.erase(100);
    add(this->asks, 500, 1);
    EXPECT_EQ(this->asks.best_price(), 500);
}

TYPED_TEST(PriceLevels, WorksAtTheBandEdges) {
    add(this->bids, BAND.lo, 0);
    add(this->bids, BAND.hi - 1, 1);
    add(this->asks, BAND.lo, 0);
    add(this->asks, BAND.hi - 1, 1);
    EXPECT_EQ(best_first(this->bids), (std::vector<Price>{BAND.hi - 1, BAND.lo}));
    EXPECT_EQ(best_first(this->asks), (std::vector<Price>{BAND.lo, BAND.hi - 1}));

    this->bids.pop_best();
    EXPECT_EQ(this->bids.best_price(), BAND.lo);
    this->asks.pop_best();
    EXPECT_EQ(this->asks.best_price(), BAND.hi - 1);
}

TYPED_TEST(PriceLevels, WalkStopsWhenVisitSaysSo) {
    add_out_of_order(this->bids);
    std::vector<Price> visited;
    this->bids.for_each_from_best([&visited](const Price price, LevelSlot) {
        visited.push_back(price);
        return visited.size() < 2;
    });
    EXPECT_EQ(visited, (std::vector<Price>{103, 102}));
}

}  // namespace
