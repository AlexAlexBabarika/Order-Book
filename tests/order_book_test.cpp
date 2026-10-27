#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/OrderBook.h"

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
}  // namespace
