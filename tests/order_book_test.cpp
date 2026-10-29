#include <gtest/gtest.h>

#include <cstdint>
#include <ostream>
#include <vector>

#include "lob/OrderBook.h"

namespace lob {
void PrintTo(const Fill& fill, std::ostream* os) {
    *os << "{maker " << fill.maker << ", taker " << fill.taker << ", price " << fill.price
        << ", qty " << fill.qty << "}";
}
}  // namespace lob

namespace {
using namespace lob;

Order buy(OrderId id, Price price, Qty qty, Tif tif = Tif::Gtc) {
    return {.id = id, .side = Side::Buy, .price = price, .original = qty, .tif = tif};
}

Order sell(OrderId id, Price price, Qty qty, Tif tif = Tif::Gtc) {
    return {.id = id, .side = Side::Sell, .price = price, .original = qty, .tif = tif};
}

TEST(OrderBook, EmptyBookHasNoOrders) {
    lob::OrderBook book;
    EXPECT_EQ(book.size(), 0u);
    EXPECT_EQ(book.best_ask(), INT64_MAX);
    EXPECT_EQ(book.best_bid(), INT64_MIN);
}

TEST(OrderBook, BestPrices) {
    lob::OrderBook book;
    book.add(buy(0, 99, 1));
    book.add(buy(1, 101, 1));
    book.add(buy(2, 100, 1));

    book.add(sell(3, 105, 1));
    book.add(sell(4, 103, 1));
    book.add(sell(5, 104, 1));
    EXPECT_EQ(book.best_ask(), 103);
    EXPECT_EQ(book.best_bid(), 101);
}

TEST(OrderBook, SamePriceQueuesInArrivalOrder) {
    lob::OrderBook book;
    book.add(buy(1, 100, 5));
    book.add(buy(2, 100, 7));
    EXPECT_EQ(book.size(), 2u);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 12);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1, 2}));
}

// Orders 1, 2 and 3 resting at 100 with quantities 5, 7 and 1
class ThreeBidsAt100 : public ::testing::Test {
   protected:
    ThreeBidsAt100() {
        book.add(buy(1, 100, 5));
        book.add(buy(2, 100, 7));
        book.add(buy(3, 100, 1));
    }

    lob::OrderBook book;
};

TEST_F(ThreeBidsAt100, CancelHeadKeepsRestInOrder) {
    book.cancel(1);
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{2, 3}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 8);
}

TEST_F(ThreeBidsAt100, CancelMiddleKeepsRestInOrder) {
    book.cancel(2);
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 6);
}

TEST_F(ThreeBidsAt100, CancelTailKeepsRestInOrder) {
    book.cancel(3);
    EXPECT_FALSE(book.contains(3));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 12);
}

TEST(OrderBook, CancellingLastOrderAtBestPriceRemovesLevel) {
    lob::OrderBook book;
    book.add(buy(1, 101, 5));
    book.add(buy(2, 100, 5));
    book.add(sell(3, 103, 5));
    book.add(sell(4, 104, 5));

    book.cancel(1);
    book.cancel(3);
    EXPECT_EQ(book.level_qty(Side::Buy, 101), 0);
    EXPECT_TRUE(book.orders_at(Side::Buy, 101).empty());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.level_qty(Side::Sell, 103), 0);
    EXPECT_EQ(book.best_ask(), 104);
}

TEST(OrderBook, DuplicateIdIsRejected) {
    lob::OrderBook book;
    book.add(buy(1, 100, 5));
    book.add(sell(1, 105, 3));
    EXPECT_EQ(book.size(), 1u);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
    EXPECT_EQ(book.level_qty(Side::Sell, 105), 0);
    EXPECT_EQ(book.best_ask(), INT64_MAX);
}

TEST(OrderBook, NonPositiveQtyIsRejected) {
    lob::OrderBook book;
    book.add(buy(1, 100, 0));
    book.add(buy(2, 100, -5));
    EXPECT_EQ(book.size(), 0u);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.best_bid(), INT64_MIN);
}

TEST(OrderBook, RemainingAboveOriginalIsRejected) {
    lob::OrderBook book;
    Order order = buy(1, 100, 5);
    order.remaining = 6;
    book.add(order);
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBook, UnknownIdCancelAndModifyChangeNothing) {
    lob::OrderBook book;
    book.add(buy(1, 100, 5));
    book.cancel(99);
    book.modify(99, 101, 10);
    EXPECT_EQ(book.size(), 1u);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
    EXPECT_EQ(book.level_qty(Side::Buy, 101), 0);
}

TEST_F(ThreeBidsAt100, LowerQtyAtSamePriceKeepsQueuePosition) {
    book.modify(1, 100, 2);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 10);
}

TEST_F(ThreeBidsAt100, UnchangedQtyAtSamePriceKeepsQueuePosition) {
    book.modify(1, 100, 5);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 13);
}

TEST_F(ThreeBidsAt100, HigherQtyMovesOrderToBackOfQueue) {
    book.modify(1, 100, 9);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{2, 3, 1}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 17);
}

TEST_F(ThreeBidsAt100, NewPriceMovesOrderToNewLevel) {
    book.modify(2, 101, 7);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 6);
    EXPECT_EQ(book.orders_at(Side::Buy, 101), (std::vector<OrderId>{2}));
    EXPECT_EQ(book.best_bid(), 101);
}

TEST(OrderBook, NewPriceRemovesEmptiedLevel) {
    lob::OrderBook book;
    book.add(buy(1, 100, 5));
    book.modify(1, 99, 5);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
    EXPECT_EQ(book.orders_at(Side::Buy, 99), (std::vector<OrderId>{1}));
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.size(), 1u);
}

TEST_F(ThreeBidsAt100, NonPositiveQtyModifyCancels) {
    book.modify(2, 100, 0);
    book.modify(3, 100, -1);
    EXPECT_FALSE(book.contains(2));
    EXPECT_FALSE(book.contains(3));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
}

TEST(OrderBook, EndSessionRemovesOnlyDayOrders) {
    lob::OrderBook book;
    book.add(buy(1, 100, 5, Tif::Day));
    book.add(buy(2, 100, 7));
    book.add(sell(3, 105, 2, Tif::Day));
    book.add(sell(4, 106, 3));

    book.end_session();
    EXPECT_EQ(book.size(), 2u);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(3));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{2}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 7);
    EXPECT_EQ(book.level_qty(Side::Sell, 105), 0);
    EXPECT_EQ(book.best_ask(), 106);
}

TEST(OrderBook, IdCanBeReusedAfterCancel) {
    lob::OrderBook book;
    book.add(buy(1, 100, 5));
    book.cancel(1);
    book.add(buy(1, 100, 3));
    EXPECT_TRUE(book.contains(1));
    EXPECT_EQ(book.size(), 1u);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{1}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 3);
}

TEST(OrderBook, ManyOrdersAddedAndCancelledLeaveBookEmpty) {
    lob::OrderBook book;
    const OrderId count = 10'000;
    // Even ids are bids, odd ids are asks, spread over 50 levels per side
    for (OrderId id = 0; id < count; ++id) {
        const Price offset = static_cast<Price>(id % 100);
        book.add(id % 2 == 0 ? buy(id, 1000 + offset, 1) : sell(id, 2000 + offset, 1));
    }
    ASSERT_EQ(book.size(), count);

    // Cancel every third id first, so orders leave from the head, middle and tail of levels
    for (OrderId id = 0; id < count; id += 3) {
        book.cancel(id);
    }
    for (OrderId id = 0; id < count; ++id) {
        if (id % 3 != 0) {
            book.cancel(id);
        }
    }
    EXPECT_EQ(book.size(), 0u);
    EXPECT_EQ(book.best_bid(), INT64_MIN);
    EXPECT_EQ(book.best_ask(), INT64_MAX);
}

// Asks: 1 (5) and 2 (7) at 100, 3 (4) at 101, 4 (6) at 103
// Bids: 5 (3) at 98, 6 (2) at 97
class Ladder : public ::testing::Test {
   protected:
    Ladder() {
        book.add(sell(1, 100, 5));
        book.add(sell(2, 100, 7));
        book.add(sell(3, 101, 4));
        book.add(sell(4, 103, 6));
        book.add(buy(5, 98, 3));
        book.add(buy(6, 97, 2));
    }

    Qty take(Order incoming) {
        book.match(incoming, [this](const Fill& fill) { fills.push_back(fill); });
        return incoming.remaining;
    }

    void submit(const Order& incoming) {
        book.submit(incoming, [this](const Fill& fill) { fills.push_back(fill); });
    }

    lob::OrderBook book;
    std::vector<Fill> fills;
};

TEST_F(Ladder, FullFillRemovesMaker) {
    EXPECT_EQ(take(buy(10, 100, 5)), 0);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 1, .taker = 10, .price = 100, .qty = 5}}));
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 7);
    EXPECT_EQ(book.size(), 5u);
}

TEST_F(Ladder, PartialFillKeepsMakerAtHead) {
    EXPECT_EQ(take(buy(10, 100, 3)), 0);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 1, .taker = 10, .price = 100, .qty = 3}}));
    EXPECT_TRUE(book.contains(1));
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 9);
}

TEST_F(Ladder, CancelAfterPartialFillRemovesOnlyWhatWasLeft) {
    ASSERT_EQ(take(buy(10, 100, 3)), 0);
    book.cancel(1);
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 7);
}

TEST_F(Ladder, FillsInArrivalOrderWithinLevel) {
    EXPECT_EQ(take(buy(10, 100, 8)), 0);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 1, .taker = 10, .price = 100, .qty = 5},
                                        {.maker = 2, .taker = 10, .price = 100, .qty = 3}}));
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 4);
}

TEST_F(Ladder, FillsAtMakerPrice) {
    EXPECT_EQ(take(buy(10, 102, 3)), 0);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 1, .taker = 10, .price = 100, .qty = 3}}));
}

TEST_F(Ladder, NoCrossNoFills) {
    EXPECT_EQ(take(buy(10, 99, 5)), 5);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.size(), 6u);
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 12);
    EXPECT_EQ(book.best_ask(), 100);
}

TEST_F(Ladder, EmptyingLevelRemovesIt) {
    EXPECT_EQ(take(buy(10, 100, 12)), 0);
    EXPECT_EQ(fills.size(), 2u);
    EXPECT_TRUE(book.orders_at(Side::Sell, 100).empty());
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 0);
    EXPECT_EQ(book.best_ask(), 101);
}

TEST_F(Ladder, StopsAtLimitPriceAndDoesNotRest) {
    EXPECT_EQ(take(buy(10, 100, 15)), 3);
    EXPECT_EQ(fills.size(), 2u);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.level_qty(Side::Sell, 101), 4);
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, SellMatchesAgainstBids) {
    std::vector<Fill> got;
    auto record = [&got](const Fill& fill) { got.push_back(fill); };
    Order taker = sell(10, 98, 2);
    book.match(taker, record);

    EXPECT_EQ(taker.remaining, 0);
    EXPECT_EQ(got, (std::vector<Fill>{{.maker = 5, .taker = 10, .price = 98, .qty = 2}}));
    EXPECT_EQ(book.level_qty(Side::Buy, 98), 1);
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 12);
}

TEST_F(Ladder, BuySweepsLevelsUpToLimit) {
    EXPECT_EQ(take(buy(10, 101, 20)), 4);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 1, .taker = 10, .price = 100, .qty = 5},
                                        {.maker = 2, .taker = 10, .price = 100, .qty = 7},
                                        {.maker = 3, .taker = 10, .price = 101, .qty = 4}}));
    EXPECT_EQ(book.best_ask(), 103);
    EXPECT_EQ(book.level_qty(Side::Sell, 103), 6);
    EXPECT_EQ(book.size(), 3u);
}

TEST_F(Ladder, SellSweepsBidsFromHighest) {
    EXPECT_EQ(take(sell(10, 97, 4)), 0);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 5, .taker = 10, .price = 98, .qty = 3},
                                        {.maker = 6, .taker = 10, .price = 97, .qty = 1}}));
    EXPECT_EQ(book.best_bid(), 97);
    EXPECT_EQ(book.level_qty(Side::Buy, 97), 1);
}

TEST_F(Ladder, MarketOrderStopsWhenSideIsEmpty) {
    Order taker = buy(10, 0, 25);
    taker.type = OrderType::Market;
    EXPECT_EQ(take(taker), 3);
    EXPECT_EQ(fills.size(), 4u);
    EXPECT_EQ(book.best_ask(), INT64_MAX);
    EXPECT_EQ(book.size(), 2u);
}

TEST_F(Ladder, SubmitGtcRestsWhatsLeft) {
    submit(buy(10, 100, 15));
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 1, .taker = 10, .price = 100, .qty = 5},
                                        {.maker = 2, .taker = 10, .price = 100, .qty = 7}}));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{10}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 3);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
}

TEST_F(Ladder, SubmitGtcThatDoesNotCrossRestsInFull) {
    submit(buy(10, 99, 5));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.orders_at(Side::Buy, 99), (std::vector<OrderId>{10}));
    EXPECT_EQ(book.level_qty(Side::Buy, 99), 5);
    EXPECT_EQ(book.best_bid(), 99);
}

TEST_F(Ladder, SubmitDayRestsUntilEndOfSession) {
    submit(buy(10, 100, 15, Tif::Day));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 3);
    book.end_session();
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, RestedRemainderCanBeHitAndCancelled) {
    submit(buy(10, 100, 15));
    submit(sell(11, 100, 2));
    EXPECT_EQ(fills.back(), (Fill{.maker = 10, .taker = 11, .price = 100, .qty = 2}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 1);
    book.cancel(10);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, SubmitIocDropsWhatsLeft) {
    submit(buy(10, 100, 15, Tif::Ioc));
    EXPECT_EQ(fills.size(), 2u);
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.best_bid(), 98);
    EXPECT_EQ(book.size(), 4u);
}

TEST_F(Ladder, SubmitIocThatDoesNotCrossDoesNothing) {
    submit(buy(10, 99, 5, Tif::Ioc));
    EXPECT_TRUE(fills.empty());
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.size(), 6u);
}

TEST_F(Ladder, SubmitFokWithExactlyEnoughFills) {
    submit(buy(10, 101, 16, Tif::Fok));
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = 1, .taker = 10, .price = 100, .qty = 5},
                                        {.maker = 2, .taker = 10, .price = 100, .qty = 7},
                                        {.maker = 3, .taker = 10, .price = 101, .qty = 4}}));
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.best_ask(), 103);
}

TEST_F(Ladder, SubmitFokOneShortDoesNothing) {
    submit(buy(10, 101, 17, Tif::Fok));
    EXPECT_TRUE(fills.empty());
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.size(), 6u);
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 12);
    EXPECT_EQ(book.level_qty(Side::Sell, 101), 4);
}

// The asks would cover it, so checking the wrong side lets this one trade
TEST_F(Ladder, SubmitSellFokChecksBids) {
    submit(sell(10, 98, 4, Tif::Fok));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.level_qty(Side::Buy, 98), 3);
}

TEST_F(Ladder, SubmitMarketDoesNotRest) {
    Order taker = buy(10, 0, 25);
    taker.type = OrderType::Market;
    submit(taker);
    EXPECT_EQ(fills.size(), 4u);
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.best_ask(), INT64_MAX);
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, SubmitMarketFokCountsWholeSide) {
    Order taker = buy(10, 0, 23, Tif::Fok);
    taker.type = OrderType::Market;
    submit(taker);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.size(), 6u);
}

TEST_F(Ladder, SubmitDuplicateIdDoesNotTrade) {
    submit(buy(1, 100, 5));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.size(), 6u);
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 12);
}

TEST_F(Ladder, SubmitZeroQtyDoesNotTrade) {
    submit(buy(10, 100, 0));
    EXPECT_TRUE(fills.empty());
    EXPECT_FALSE(book.contains(10));
    EXPECT_EQ(book.size(), 6u);
}
}  // namespace
