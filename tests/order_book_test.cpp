#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <type_traits>
#include <utility>
#include <vector>

#include "lob/OrderBook.h"

namespace lob {
void PrintTo(const Fill& fill, std::ostream* os) {
    *os << "{maker " << fill.maker << ", taker " << fill.taker << ", price " << fill.price
        << ", qty " << fill.qty << "}";
}

void PrintTo(const Result result, std::ostream* os) {
    switch (result) {
        case Result::Ok:
            *os << "Ok";
            return;
        case Result::UnknownId:
            *os << "UnknownId";
            return;
        case Result::BadQty:
            *os << "BadQty";
            return;
        case Result::BookFull:
            *os << "BookFull";
            return;
        case Result::OutOfBand:
            *os << "OutOfBand";
            return;
    }
}

void PrintTo(const Placed& placed, std::ostream* os) {
    *os << "{";
    PrintTo(placed.result, os);
    *os << ", id " << placed.id << "}";
}
}  // namespace lob

namespace {
using namespace lob;

static_assert(!std::is_copy_constructible_v<OrderBook>);
static_assert(!std::is_copy_assignable_v<OrderBook>);

Order buy(Price price, Qty qty, Tif tif = Tif::Gtc) {
    return {.price = price, .original = qty, .side = Side::Buy, .tif = tif};
}

Order sell(Price price, Qty qty, Tif tif = Tif::Gtc) {
    return {.price = price, .original = qty, .side = Side::Sell, .tif = tif};
}

// Adds an order the book must take, and returns the id the book gave it
OrderId rest(OrderBook& book, const Order& order) {
    const Placed placed = book.add(order);
    EXPECT_EQ(placed.result, Result::Ok);
    return placed.id;
}

// A modify that mustn't trade
Result modify(OrderBook& book, const OrderId id, const Price price, const Qty qty) {
    return book.modify(id, price, qty, [](const Fill& fill) {
        ADD_FAILURE() << "unexpected fill at " << fill.price << " for " << fill.qty;
    });
}

TEST(OrderBook, EmptyBookHasNoOrders) {
    lob::OrderBook book;
    EXPECT_EQ(book.size(), 0u);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBook, BestPrices) {
    lob::OrderBook book;
    rest(book, buy(99, 1));
    rest(book, buy(101, 1));
    rest(book, buy(100, 1));

    rest(book, sell(105, 1));
    rest(book, sell(103, 1));
    rest(book, sell(104, 1));
    EXPECT_EQ(book.best_ask(), 103);
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.spread(), 2);
}

// With bids empty, best_ask() - INT64_MIN used to overflow
TEST(OrderBook, SpreadNeedsBothSides) {
    lob::OrderBook book;
    EXPECT_EQ(book.spread(), std::nullopt);
    const OrderId ask = rest(book, sell(103, 1));
    EXPECT_EQ(book.spread(), std::nullopt);
    book.cancel(ask);
    rest(book, buy(101, 1));
    EXPECT_EQ(book.spread(), std::nullopt);
}

TEST(OrderBook, SamePriceQueuesInArrivalOrder) {
    lob::OrderBook book;
    const Placed first = book.add(buy(100, 5));
    const Placed second = book.add(buy(100, 7));
    EXPECT_EQ(first.result, Result::Ok);
    EXPECT_EQ(second.result, Result::Ok);
    EXPECT_NE(first.id, second.id);
    EXPECT_EQ(book.size(), 2u);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 12);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{first.id, second.id}));
}

// Three bids resting at 100 with quantities 5, 7 and 1
class ThreeBidsAt100 : public ::testing::Test {
   protected:
    lob::OrderBook book;
    OrderId one = rest(book, buy(100, 5));
    OrderId two = rest(book, buy(100, 7));
    OrderId three = rest(book, buy(100, 1));
};

TEST_F(ThreeBidsAt100, CancelHeadKeepsRestInOrder) {
    EXPECT_EQ(book.cancel(one), Result::Ok);
    EXPECT_FALSE(book.contains(one));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{two, three}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 8);
}

TEST_F(ThreeBidsAt100, CancelMiddleKeepsRestInOrder) {
    book.cancel(two);
    EXPECT_FALSE(book.contains(two));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{one, three}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 6);
}

TEST_F(ThreeBidsAt100, CancelTailKeepsRestInOrder) {
    book.cancel(three);
    EXPECT_FALSE(book.contains(three));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{one, two}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 12);

    // A stale tail only shows up on the next append
    const OrderId four = rest(book, buy(100, 2));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{one, two, four}));
}

TEST(OrderBook, CancellingLastOrderAtBestPriceRemovesLevel) {
    lob::OrderBook book;
    const OrderId best_bid = rest(book, buy(101, 5));
    rest(book, buy(100, 5));
    const OrderId best_ask = rest(book, sell(103, 5));
    rest(book, sell(104, 5));

    book.cancel(best_bid);
    book.cancel(best_ask);
    EXPECT_EQ(book.level_qty(Side::Buy, 101), 0);
    EXPECT_TRUE(book.orders_at(Side::Buy, 101).empty());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.level_qty(Side::Sell, 103), 0);
    EXPECT_EQ(book.best_ask(), 104);
}

// The second order takes the slot the first one freed, but gets a different id, and the old
// id stops working
TEST(OrderBook, StaleIdDoesNotReachNewOrderInSameSlot) {
    lob::OrderBook book;
    const OrderId old_id = rest(book, buy(100, 5));
    book.cancel(old_id);
    const OrderId new_id = rest(book, buy(101, 3));
    ASSERT_EQ(static_cast<std::uint32_t>(new_id), static_cast<std::uint32_t>(old_id))
        << "the test needs the slot to be reused";

    EXPECT_NE(new_id, old_id);
    EXPECT_FALSE(book.contains(old_id));
    EXPECT_EQ(book.cancel(old_id), Result::UnknownId);
    EXPECT_EQ(modify(book, old_id, 102, 1), Result::UnknownId);
    EXPECT_TRUE(book.contains(new_id));
    EXPECT_EQ(book.orders_at(Side::Buy, 101), (std::vector<OrderId>{new_id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 101), 3);
}

TEST(OrderBook, NonPositiveQtyIsRejected) {
    lob::OrderBook book;
    EXPECT_EQ(book.add(buy(100, 0)), (Placed{.result = Result::BadQty}));
    EXPECT_EQ(book.add(buy(100, -5)), (Placed{.result = Result::BadQty}));
    EXPECT_EQ(book.size(), 0u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBook, RemainingAboveOriginalIsRejected) {
    lob::OrderBook book;
    Order order = buy(100, 5);
    order.remaining = 6;
    EXPECT_EQ(book.add(order), (Placed{.result = Result::BadQty}));
    EXPECT_EQ(book.size(), 0u);
}

// Ids the book never handed out: an even generation, a slot never used, a slot past the end
// of the pool, and NO_ORDER
TEST(OrderBook, UnknownIdCancelAndModifyChangeNothing) {
    lob::OrderBook book;
    const OrderId id = rest(book, buy(100, 5));
    constexpr OrderId GENERATION_ONE = OrderId{1} << 32;
    for (const OrderId unknown : {OrderId{0}, GENERATION_ONE | 5, GENERATION_ONE | 1'000'000,
                                  NO_ORDER}) {
        EXPECT_FALSE(book.contains(unknown)) << unknown;
        EXPECT_EQ(book.cancel(unknown), Result::UnknownId) << unknown;
        EXPECT_EQ(modify(book, unknown, 101, 10), Result::UnknownId) << unknown;
    }
    EXPECT_EQ(book.size(), 1u);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
    EXPECT_EQ(book.level_qty(Side::Buy, 101), 0);
}

TEST_F(ThreeBidsAt100, LowerQtyAtSamePriceKeepsQueuePosition) {
    EXPECT_EQ(modify(book, one, 100, 2), Result::Ok);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{one, two, three}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 10);
}

TEST_F(ThreeBidsAt100, UnchangedQtyAtSamePriceKeepsQueuePosition) {
    modify(book, one, 100, 5);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{one, two, three}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 13);
}

TEST_F(ThreeBidsAt100, HigherQtyMovesOrderToBackOfQueue) {
    modify(book, one, 100, 9);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{two, three, one}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 17);
}

TEST_F(ThreeBidsAt100, NewPriceMovesOrderToNewLevel) {
    EXPECT_EQ(modify(book, two, 101, 7), Result::Ok);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{one, three}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 6);
    EXPECT_EQ(book.orders_at(Side::Buy, 101), (std::vector<OrderId>{two}));
    EXPECT_EQ(book.best_bid(), 101);
}

TEST(OrderBook, NewPriceRemovesEmptiedLevel) {
    lob::OrderBook book;
    const OrderId id = rest(book, buy(100, 5));
    modify(book, id, 99, 5);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
    EXPECT_EQ(book.orders_at(Side::Buy, 99), (std::vector<OrderId>{id}));
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.size(), 1u);
}

TEST_F(ThreeBidsAt100, NonPositiveQtyModifyCancels) {
    EXPECT_EQ(modify(book, two, 100, 0), Result::Ok);
    EXPECT_EQ(modify(book, three, 100, -1), Result::Ok);
    EXPECT_FALSE(book.contains(two));
    EXPECT_FALSE(book.contains(three));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{one}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 5);
}

TEST(OrderBook, EndSessionRemovesOnlyDayOrders) {
    lob::OrderBook book;
    const OrderId day_bid = rest(book, buy(100, 5, Tif::Day));
    const OrderId gtc_bid = rest(book, buy(100, 7));
    const OrderId day_ask = rest(book, sell(105, 2, Tif::Day));
    rest(book, sell(106, 3));

    book.end_session();
    EXPECT_EQ(book.size(), 2u);
    EXPECT_FALSE(book.contains(day_bid));
    EXPECT_FALSE(book.contains(day_ask));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{gtc_bid}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 7);
    EXPECT_EQ(book.level_qty(Side::Sell, 105), 0);
    EXPECT_EQ(book.best_ask(), 106);
}

// end_session walks the slots, and must reach the last one handed out too
TEST(OrderBook, EndSessionRemovesDayOrderAddedLast) {
    lob::OrderBook book;
    rest(book, buy(100, 5, Tif::Day));
    const OrderId gtc_bid = rest(book, buy(100, 7));
    rest(book, sell(105, 2, Tif::Day));

    book.end_session();
    EXPECT_EQ(book.size(), 1u);
    EXPECT_TRUE(book.contains(gtc_bid));
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBook, ManyOrdersAddedAndCancelledLeaveBookEmpty) {
    lob::OrderBook book;
    constexpr std::size_t COUNT = 10'000;
    std::vector<OrderId> ids;
    // Even orders are bids, odd ones asks, spread over 50 levels per side
    for (std::size_t i = 0; i < COUNT; ++i) {
        const Price offset = static_cast<Price>(i % 100);
        ids.push_back(rest(book, i % 2 == 0 ? buy(1000 + offset, 1) : sell(2000 + offset, 1)));
    }
    ASSERT_EQ(book.size(), COUNT);

    // Cancel every third order first, so orders leave from the head, middle and tail of levels
    for (std::size_t i = 0; i < COUNT; i += 3) {
        EXPECT_EQ(book.cancel(ids[i]), Result::Ok);
    }
    for (std::size_t i = 0; i < COUNT; ++i) {
        if (i % 3 != 0) {
            EXPECT_EQ(book.cancel(ids[i]), Result::Ok);
        }
    }
    EXPECT_EQ(book.size(), 0u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBook, MovedBookKeepsWorking) {
    lob::OrderBook book;
    const OrderId first = rest(book, buy(100, 5));
    const OrderId second = rest(book, buy(100, 7));
    lob::OrderBook moved = std::move(book);

    EXPECT_EQ(moved.cancel(first), Result::Ok);
    EXPECT_EQ(moved.orders_at(Side::Buy, 100), (std::vector<OrderId>{second}));
    EXPECT_EQ(moved.level_qty(Side::Buy, 100), 7);
    EXPECT_EQ(modify(moved, second, 99, 7), Result::Ok);
    EXPECT_EQ(moved.best_bid(), 99);
}

TEST(OrderBook, FullBookRejectsUntilASlotFrees) {
    lob::OrderBook book(2);
    const OrderId first = rest(book, buy(100, 5));
    const OrderId second = rest(book, buy(100, 7));
    EXPECT_EQ(book.add(sell(105, 1)), (Placed{.result = Result::BookFull}));
    EXPECT_EQ(book.size(), 2u);
    EXPECT_EQ(book.best_ask(), std::nullopt);

    book.cancel(first);
    rest(book, sell(105, 1));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{second}));
    EXPECT_EQ(book.best_ask(), 105);
}

TEST(OrderBook, ModifyOnFullBookKeepsItsSlot) {
    lob::OrderBook book(1);
    const OrderId id = rest(book, buy(100, 5));
    EXPECT_EQ(modify(book, id, 101, 5), Result::Ok);
    EXPECT_EQ(modify(book, id, 101, 9), Result::Ok);
    EXPECT_EQ(book.orders_at(Side::Buy, 101), (std::vector<OrderId>{id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 101), 9);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
}

// The order got an id before it could match, so submit returns it, but it doesn't rest
TEST(OrderBook, SubmitReportsFullBook) {
    lob::OrderBook book(1);
    rest(book, sell(101, 5));
    const Placed placed = book.submit(buy(100, 3), [](const Fill&) {});
    EXPECT_EQ(placed.result, Result::BookFull);
    EXPECT_NE(placed.id, NO_ORDER);
    EXPECT_FALSE(book.contains(placed.id));
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

// The pool keeps a slot spare, so a full book still gives the taker an id and trades
TEST(OrderBook, FullBookStillTrades) {
    lob::OrderBook book(1);
    const OrderId maker = rest(book, sell(100, 5));
    std::vector<Fill> fills;
    const Placed placed =
        book.submit(buy(100, 3, Tif::Ioc), [&fills](const Fill& fill) { fills.push_back(fill); });
    EXPECT_EQ(placed.result, Result::Ok);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = maker, .taker = placed.id, .price = 100, .qty = 3}}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 2);
    EXPECT_EQ(book.size(), 1u);
}

constexpr PriceBand BAND_100_200{.lo = 100, .hi = 200};

// The band includes lo and excludes hi
TEST(OrderBook, AddOutsideBandIsRejected) {
    lob::OrderBook book(16, BAND_100_200);
    EXPECT_EQ(book.add(buy(99, 5)), (Placed{.result = Result::OutOfBand}));
    EXPECT_EQ(book.add(sell(200, 5)), (Placed{.result = Result::OutOfBand}));
    EXPECT_EQ(book.add(buy(100, 5)).result, Result::Ok);
    EXPECT_EQ(book.add(sell(199, 5)).result, Result::Ok);
    EXPECT_EQ(book.size(), 2u);
}

TEST(OrderBook, SubmitOutsideBandDoesNotTrade) {
    lob::OrderBook book(16, BAND_100_200);
    rest(book, sell(150, 5));
    std::vector<Fill> fills;
    const auto on_fill = [&fills](const Fill& fill) { fills.push_back(fill); };
    EXPECT_EQ(book.submit(buy(250, 5), on_fill), (Placed{.result = Result::OutOfBand}));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.level_qty(Side::Sell, 150), 5);
}

TEST(OrderBook, MarketOrderIgnoresBand) {
    lob::OrderBook book(16, BAND_100_200);
    rest(book, sell(150, 5));
    Order taker = buy(0, 5);
    taker.type = OrderType::Market;
    std::size_t fills = 0;
    EXPECT_EQ(book.submit(taker, [&fills](const Fill&) { ++fills; }).result, Result::Ok);
    EXPECT_EQ(fills, 1u);
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBook, ModifyOutsideBandKeepsOrder) {
    lob::OrderBook book(16, BAND_100_200);
    const OrderId id = rest(book, buy(150, 5));
    EXPECT_EQ(modify(book, id, 200, 3), Result::OutOfBand);
    EXPECT_EQ(book.orders_at(Side::Buy, 150), (std::vector<OrderId>{id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 150), 5);
}

// Bids are kept under -price, so their band edges are where an off-by-one would show
TEST(OrderBook, BothSidesWorkAtBothBandEdges) {
    lob::OrderBook book(16, BAND_100_200);
    std::vector<Fill> fills;
    const auto on_fill = [&fills](const Fill& fill) { fills.push_back(fill); };

    const OrderId low_bid = rest(book, buy(100, 5));
    const OrderId high_bid = rest(book, buy(199, 5));
    EXPECT_EQ(book.best_bid(), 199);
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{low_bid}));
    EXPECT_EQ(book.level_qty(Side::Buy, 199), 5);
    const OrderId seller = book.submit(sell(100, 10, Tif::Ioc), on_fill).id;
    EXPECT_EQ(fills, (std::vector<Fill>{
                         {.maker = high_bid, .taker = seller, .price = 199, .qty = 5},
                         {.maker = low_bid, .taker = seller, .price = 100, .qty = 5},
                     }));
    EXPECT_EQ(book.best_bid(), std::nullopt);

    fills.clear();
    const OrderId low_ask = rest(book, sell(100, 5));
    const OrderId high_ask = rest(book, sell(199, 5));
    EXPECT_EQ(book.best_ask(), 100);
    EXPECT_EQ(book.orders_at(Side::Sell, 199), (std::vector<OrderId>{high_ask}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 5);
    const OrderId buyer = book.submit(buy(199, 10, Tif::Ioc), on_fill).id;
    EXPECT_EQ(fills, (std::vector<Fill>{
                         {.maker = low_ask, .taker = buyer, .price = 100, .qty = 5},
                         {.maker = high_ask, .taker = buyer, .price = 199, .qty = 5},
                     }));
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.size(), 0u);
}

TEST(OrderBook, ModifyToZeroQtyCancelsWhateverThePrice) {
    lob::OrderBook book(16, BAND_100_200);
    const OrderId id = rest(book, buy(150, 5));
    EXPECT_EQ(modify(book, id, 999, 0), Result::Ok);
    EXPECT_FALSE(book.contains(id));
}

TEST(OrderBook, FilledMakerFreesSlotForRemainder) {
    lob::OrderBook book(1);
    const OrderId maker = rest(book, sell(100, 5));
    const Placed placed = book.submit(buy(100, 8), [](const Fill&) {});
    EXPECT_EQ(placed.result, Result::Ok);
    EXPECT_FALSE(book.contains(maker));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{placed.id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 3);
}

// Each of these empties a level at a new price 100 times on a 4-order book. If emptying a
// level doesn't release its slot, the level pool runs out at the fifth new price.
TEST(OrderBook, CancelReleasesEmptiedLevels) {
    lob::OrderBook book(4);
    for (Price price = 101; price <= 200; ++price) {
        const Placed placed = book.add(buy(price, 5));
        ASSERT_EQ(placed.result, Result::Ok);
        ASSERT_EQ(book.cancel(placed.id), Result::Ok);
    }
    EXPECT_EQ(book.size(), 0u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(OrderBook, MatchReleasesEmptiedLevels) {
    lob::OrderBook book(4);
    for (Price price = 101; price <= 200; price += 2) {
        ASSERT_EQ(book.add(sell(price, 5)).result, Result::Ok);
        ASSERT_EQ(book.submit(buy(price, 5, Tif::Ioc), [](const Fill&) {}).result, Result::Ok);
    }
    EXPECT_EQ(book.size(), 0u);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(OrderBook, ModifyReleasesEmptiedLevels) {
    lob::OrderBook book(4);
    const OrderId id = rest(book, buy(100, 5));
    for (Price price = 101; price <= 200; ++price) {
        ASSERT_EQ(modify(book, id, price, 5), Result::Ok);
    }
    EXPECT_EQ(book.orders_at(Side::Buy, 200), (std::vector<OrderId>{id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 199), 0);
    EXPECT_EQ(book.best_bid(), 200);
}

// A modify that crosses is a cancel and replace: the order trades under its own id, at the
// makers' prices, best first, and what's left rests at its new price
TEST(OrderBook, ModifyThatCrossesTrades) {
    lob::OrderBook book;
    const OrderId near = rest(book, sell(101, 2));
    const OrderId far = rest(book, sell(102, 2));
    const OrderId bid = rest(book, buy(100, 5));
    std::vector<Fill> fills;
    const auto on_fill = [&fills](const Fill& fill) { fills.push_back(fill); };

    EXPECT_EQ(book.modify(bid, 102, 5, on_fill), Result::Ok);
    EXPECT_EQ(fills, (std::vector<Fill>{
                         {.maker = near, .taker = bid, .price = 101, .qty = 2},
                         {.maker = far, .taker = bid, .price = 102, .qty = 2},
                     }));
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.orders_at(Side::Buy, 102), (std::vector<OrderId>{bid}));
    EXPECT_EQ(book.level_qty(Side::Buy, 102), 1);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
    EXPECT_EQ(book.size(), 1u);
}

TEST(OrderBook, ModifyThatFillsCompletelyRemovesTheOrder) {
    lob::OrderBook book;
    const OrderId bid = rest(book, buy(100, 5));
    const OrderId ask = rest(book, sell(102, 3));
    std::vector<Fill> fills;
    const auto on_fill = [&fills](const Fill& fill) { fills.push_back(fill); };

    EXPECT_EQ(book.modify(ask, 99, 3, on_fill), Result::Ok);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = bid, .taker = ask, .price = 100, .qty = 3}}));
    EXPECT_FALSE(book.contains(ask));
    EXPECT_EQ(book.cancel(ask), Result::UnknownId);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 2);
    EXPECT_EQ(book.size(), 1u);
}

TEST(OrderBook, ModifiedOrderKeepsItsTif) {
    lob::OrderBook book;
    rest(book, sell(101, 2));
    const OrderId day = rest(book, buy(100, 5, Tif::Day));
    std::size_t fills = 0;

    EXPECT_EQ(book.modify(day, 101, 5, [&fills](const Fill&) { ++fills; }), Result::Ok);
    EXPECT_EQ(fills, 1u);
    EXPECT_EQ(book.level_qty(Side::Buy, 101), 3);
    book.end_session();
    EXPECT_FALSE(book.contains(day));
}

// The order gives up its place before it trades, so a full book doesn't stop it
TEST(OrderBook, ModifyCanTradeOnAFullBook) {
    lob::OrderBook book(2);
    const OrderId ask = rest(book, sell(101, 5));
    const OrderId bid = rest(book, buy(100, 5));
    std::vector<Fill> fills;
    const auto on_fill = [&fills](const Fill& fill) { fills.push_back(fill); };

    EXPECT_EQ(book.modify(bid, 101, 5, on_fill), Result::Ok);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = ask, .taker = bid, .price = 101, .qty = 5}}));
    EXPECT_EQ(book.size(), 0u);
    rest(book, buy(100, 1));
    rest(book, sell(101, 1));
    EXPECT_EQ(book.size(), 2u);
}

// Asks: ask1 (5) and ask2 (7) at 100, ask3 (4) at 101, ask4 (6) at 103
// Bids: bid1 (3) at 98, bid2 (2) at 97
class Ladder : public ::testing::Test {
   protected:
    lob::OrderBook book;
    std::vector<Fill> fills;
    OrderId ask1 = rest(book, sell(100, 5));
    OrderId ask2 = rest(book, sell(100, 7));
    OrderId ask3 = rest(book, sell(101, 4));
    OrderId ask4 = rest(book, sell(103, 6));
    OrderId bid1 = rest(book, buy(98, 3));
    OrderId bid2 = rest(book, buy(97, 2));
    OrderId taker = NO_ORDER;  // the id take() got

    // Matches the order as IOC, so nothing rests, and returns how much was left unfilled
    Qty take(Order incoming) {
        incoming.tif = Tif::Ioc;
        const std::size_t before = fills.size();
        const Placed placed = submit(incoming);
        EXPECT_EQ(placed.result, Result::Ok);
        taker = placed.id;
        Qty left = incoming.remaining;
        for (std::size_t i = before; i < fills.size(); ++i) {
            left -= fills[i].qty;
        }
        return left;
    }

    Placed submit(const Order& incoming) {
        return book.submit(incoming, [this](const Fill& fill) { fills.push_back(fill); });
    }
};

TEST_F(Ladder, FullFillRemovesMaker) {
    EXPECT_EQ(take(buy(100, 5)), 0);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = ask1, .taker = taker, .price = 100, .qty = 5}}));
    EXPECT_FALSE(book.contains(ask1));
    EXPECT_EQ(book.cancel(ask1), Result::UnknownId);
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{ask2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 7);
    EXPECT_EQ(book.size(), 5u);
}

TEST_F(Ladder, PartialFillKeepsMakerAtHead) {
    EXPECT_EQ(take(buy(100, 3)), 0);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = ask1, .taker = taker, .price = 100, .qty = 3}}));
    EXPECT_TRUE(book.contains(ask1));
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{ask1, ask2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 9);
}

TEST_F(Ladder, CancelAfterPartialFillRemovesOnlyWhatWasLeft) {
    ASSERT_EQ(take(buy(100, 3)), 0);
    book.cancel(ask1);
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{ask2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 7);
}

TEST_F(Ladder, FillsInArrivalOrderWithinLevel) {
    EXPECT_EQ(take(buy(100, 8)), 0);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = ask1, .taker = taker, .price = 100, .qty = 5},
                                        {.maker = ask2, .taker = taker, .price = 100, .qty = 3}}));
    EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<OrderId>{ask2}));
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 4);
}

TEST_F(Ladder, FillsAtMakerPrice) {
    EXPECT_EQ(take(buy(102, 3)), 0);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = ask1, .taker = taker, .price = 100, .qty = 3}}));
}

TEST_F(Ladder, NoCrossNoFills) {
    EXPECT_EQ(take(buy(99, 5)), 5);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.size(), 6u);
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 12);
    EXPECT_EQ(book.best_ask(), 100);
}

TEST_F(Ladder, EmptyingLevelRemovesIt) {
    EXPECT_EQ(take(buy(100, 12)), 0);
    EXPECT_EQ(fills.size(), 2u);
    EXPECT_TRUE(book.orders_at(Side::Sell, 100).empty());
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 0);
    EXPECT_EQ(book.best_ask(), 101);
}

TEST_F(Ladder, StopsAtLimitPrice) {
    EXPECT_EQ(take(buy(100, 15)), 3);
    EXPECT_EQ(fills.size(), 2u);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.level_qty(Side::Sell, 101), 4);
    EXPECT_FALSE(book.contains(taker));
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, SellMatchesAgainstBids) {
    EXPECT_EQ(take(sell(98, 2)), 0);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = bid1, .taker = taker, .price = 98, .qty = 2}}));
    EXPECT_EQ(book.level_qty(Side::Buy, 98), 1);
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 12);
}

TEST_F(Ladder, BuySweepsLevelsUpToLimit) {
    EXPECT_EQ(take(buy(101, 20)), 4);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = ask1, .taker = taker, .price = 100, .qty = 5},
                                        {.maker = ask2, .taker = taker, .price = 100, .qty = 7},
                                        {.maker = ask3, .taker = taker, .price = 101, .qty = 4}}));
    EXPECT_EQ(book.best_ask(), 103);
    EXPECT_EQ(book.level_qty(Side::Sell, 103), 6);
    EXPECT_EQ(book.size(), 3u);
}

// The levels arrive as new worst, new best and in between, so the sweep only comes out in
// price order if the book keeps them sorted
TEST(OrderBook, SweepFillsLevelsAddedOutOfOrder) {
    lob::OrderBook book;
    rest(book, sell(102, 1));
    rest(book, sell(104, 1));
    rest(book, sell(100, 1));
    rest(book, sell(103, 1));
    rest(book, sell(101, 1));
    Order taker = buy(0, 5);
    taker.type = OrderType::Market;

    std::vector<Price> prices;
    book.submit(taker, [&prices](const Fill& fill) { prices.push_back(fill.price); });
    EXPECT_EQ(prices, (std::vector<Price>{100, 101, 102, 103, 104}));
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST_F(Ladder, SellSweepsBidsFromHighest) {
    EXPECT_EQ(take(sell(97, 4)), 0);
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = bid1, .taker = taker, .price = 98, .qty = 3},
                                        {.maker = bid2, .taker = taker, .price = 97, .qty = 1}}));
    EXPECT_EQ(book.best_bid(), 97);
    EXPECT_EQ(book.level_qty(Side::Buy, 97), 1);
}

TEST_F(Ladder, MarketOrderStopsWhenSideIsEmpty) {
    Order incoming = buy(0, 25);
    incoming.type = OrderType::Market;
    EXPECT_EQ(take(incoming), 3);
    EXPECT_EQ(fills.size(), 4u);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.size(), 2u);
}

TEST_F(Ladder, SubmitGtcRestsWhatsLeft) {
    const Placed placed = submit(buy(100, 15));
    EXPECT_EQ(placed.result, Result::Ok);
    EXPECT_EQ(fills,
              (std::vector<Fill>{{.maker = ask1, .taker = placed.id, .price = 100, .qty = 5},
                                 {.maker = ask2, .taker = placed.id, .price = 100, .qty = 7}}));
    EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<OrderId>{placed.id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 3);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
}

TEST_F(Ladder, SubmitGtcThatDoesNotCrossRestsInFull) {
    const OrderId id = submit(buy(99, 5)).id;
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.orders_at(Side::Buy, 99), (std::vector<OrderId>{id}));
    EXPECT_EQ(book.level_qty(Side::Buy, 99), 5);
    EXPECT_EQ(book.best_bid(), 99);
}

TEST_F(Ladder, SubmitDayRestsUntilEndOfSession) {
    const OrderId id = submit(buy(100, 15, Tif::Day)).id;
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 3);
    book.end_session();
    EXPECT_FALSE(book.contains(id));
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, RestedRemainderCanBeHitAndCancelled) {
    const OrderId rested = submit(buy(100, 15)).id;
    const OrderId hit = submit(sell(100, 2)).id;
    EXPECT_EQ(fills.back(), (Fill{.maker = rested, .taker = hit, .price = 100, .qty = 2}));
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 1);
    book.cancel(rested);
    EXPECT_EQ(book.level_qty(Side::Buy, 100), 0);
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, SubmitIocDropsWhatsLeft) {
    const OrderId id = submit(buy(100, 15, Tif::Ioc)).id;
    EXPECT_EQ(fills.size(), 2u);
    EXPECT_FALSE(book.contains(id));
    EXPECT_EQ(book.best_bid(), 98);
    EXPECT_EQ(book.size(), 4u);
}

TEST_F(Ladder, SubmitIocThatDoesNotCrossDoesNothing) {
    const OrderId id = submit(buy(99, 5, Tif::Ioc)).id;
    EXPECT_TRUE(fills.empty());
    EXPECT_FALSE(book.contains(id));
    EXPECT_EQ(book.size(), 6u);
}

TEST_F(Ladder, SubmitFokWithExactlyEnoughFills) {
    const OrderId id = submit(buy(101, 16, Tif::Fok)).id;
    EXPECT_EQ(fills, (std::vector<Fill>{{.maker = ask1, .taker = id, .price = 100, .qty = 5},
                                        {.maker = ask2, .taker = id, .price = 100, .qty = 7},
                                        {.maker = ask3, .taker = id, .price = 101, .qty = 4}}));
    EXPECT_FALSE(book.contains(id));
    EXPECT_EQ(book.best_ask(), 103);
}

TEST_F(Ladder, SubmitFokOneShortDoesNothing) {
    const Placed placed = submit(buy(101, 17, Tif::Fok));
    EXPECT_EQ(placed.result, Result::Ok);
    EXPECT_TRUE(fills.empty());
    EXPECT_FALSE(book.contains(placed.id));
    EXPECT_EQ(book.size(), 6u);
    EXPECT_EQ(book.level_qty(Side::Sell, 100), 12);
    EXPECT_EQ(book.level_qty(Side::Sell, 101), 4);
}

TEST_F(Ladder, SubmitSellFokChecksBids) {
    submit(sell(98, 4, Tif::Fok));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.level_qty(Side::Buy, 98), 3);
}

TEST_F(Ladder, SubmitMarketDoesNotRest) {
    Order incoming = buy(0, 25);
    incoming.type = OrderType::Market;
    const OrderId id = submit(incoming).id;
    EXPECT_EQ(fills.size(), 4u);
    EXPECT_FALSE(book.contains(id));
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.best_bid(), 98);
}

TEST_F(Ladder, SubmitMarketFokCountsWholeSide) {
    Order incoming = buy(0, 23, Tif::Fok);
    incoming.type = OrderType::Market;
    submit(incoming);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.size(), 6u);
}

TEST_F(Ladder, SubmitZeroQtyDoesNotTrade) {
    EXPECT_EQ(submit(buy(100, 0)), (Placed{.result = Result::BadQty}));
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.size(), 6u);
}
}  // namespace
