#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Checker.h"
#include "FeedBook.h"
#include "Lobster.h"

namespace {
using namespace lob;
using feed::Event;
using feed::EventType;

constexpr Price TICK = 100;
constexpr PriceBand BAND{.lo = 0, .hi = 1000};

feed::FeedBook small_book() { return feed::FeedBook(16, BAND, TICK); }

Event event(const EventType type, const std::uint64_t id, const Qty size, const Price price,
            const Side side) {
    return {.id = id, .size = size, .price = price, .type = type, .side = side};
}

feed::BookRow row(std::vector<feed::Quote> asks, std::vector<feed::Quote> bids) {
    return {.asks = std::move(asks), .bids = std::move(bids)};
}

TEST(Lobster, ReadsEvents) {
    std::istringstream in(
        "34200.017459617,5,0,1,2238200,-1\n"
        "34200.18960767,1,11885113,21,2238100,1\r\n"
        "\n"
        "34200.5,3,11885113,21,2238100,1\n");
    const std::vector<Event> events = feed::read_events(in);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].type, EventType::HiddenExecute);
    EXPECT_EQ(events[0].side, Side::Sell);
    EXPECT_EQ(events[1].type, EventType::Add);
    EXPECT_EQ(events[1].id, 11885113u);
    EXPECT_EQ(events[1].size, 21);
    EXPECT_EQ(events[1].price, 2238100);
    EXPECT_EQ(events[1].side, Side::Buy);
    EXPECT_EQ(events[2].type, EventType::Delete);
}

TEST(Lobster, NamesTheLineItCannotRead) {
    for (const char* bad :
         {"1.0,1,7,ten,100,1\n", "1.0,9,7,10,100,1\n", "1.0,1,7,10,100,0\n", "1.0,1,7,10\n"}) {
        std::istringstream in(std::string("1.0,1,7,10,100,1\n") + bad);
        try {
            feed::read_events(in);
            ADD_FAILURE() << "read " << bad;
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("line 2"), std::string::npos) << error.what();
        }
    }
}

TEST(Lobster, ReadsRowsAndLeavesOutUnoccupiedLevels) {
    std::istringstream in(
        "2239500,100,2231800,50,2239900,70,-9999999999,0\n"
        "9999999999,0,-9999999999,0,9999999999,0,-9999999999,0\n");
    feed::RowReader rows(in);
    feed::BookRow first;
    ASSERT_TRUE(rows.next(first));
    ASSERT_EQ(first.asks.size(), 2u);
    EXPECT_EQ(first.asks[1].price, 2239900);
    EXPECT_EQ(first.asks[1].qty, 70);
    ASSERT_EQ(first.bids.size(), 1u);
    EXPECT_EQ(first.bids[0].price, 2231800);
    EXPECT_EQ(first.bids[0].qty, 50);

    feed::BookRow second;
    ASSERT_TRUE(rows.next(second));
    EXPECT_TRUE(second.asks.empty());
    EXPECT_TRUE(second.bids.empty());
    EXPECT_FALSE(rows.next(second));
}

TEST(Lobster, RejectsARowCutShort) {
    std::istringstream in("2239500,100,2231800\n");
    feed::RowReader rows(in);
    feed::BookRow out;
    EXPECT_THROW(rows.next(out), std::runtime_error);
}

TEST(FeedBook, AddsRestUnderTheFeedsIds) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 7, 30, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 8, 20, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 9, 10, 50'300, Side::Sell));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 50);
    EXPECT_EQ(book.book().best_bid(), 500);
    EXPECT_EQ(book.book().best_ask(), 503);
    EXPECT_EQ(book.stats().applied, 3u);
}

// A feed is already matched, so an add that crosses still rests
TEST(FeedBook, AddsNeverTrade) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 1, 10, 50'000, Side::Sell));
    book.apply(event(EventType::Add, 2, 10, 50'100, Side::Buy));
    EXPECT_EQ(book.book().size(), 2u);
}

TEST(FeedBook, ReductionsAndExecutionsTakeTheirSizeOff) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 7, 30, 50'000, Side::Buy));
    book.apply(event(EventType::Reduce, 7, 5, 50'000, Side::Buy));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 25);
    book.apply(event(EventType::Execute, 7, 10, 50'000, Side::Buy));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 15);
    book.apply(event(EventType::Execute, 7, 15, 50'000, Side::Buy));
    EXPECT_EQ(book.book().size(), 0u);
    // The order is gone, so its id means nothing now
    book.apply(event(EventType::Reduce, 7, 1, 50'000, Side::Buy));
    EXPECT_EQ(book.stats().applied, 4u);
    EXPECT_EQ(book.stats().unknown, 1u);
}

TEST(FeedBook, AReductionKeepsTheOrdersPlaceInItsQueue) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 1, 30, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 2, 30, 50'000, Side::Buy));
    const std::vector<OrderId> before = book.book().orders_at(Side::Buy, 500);
    book.apply(event(EventType::Reduce, 1, 5, 50'000, Side::Buy));
    EXPECT_EQ(book.book().orders_at(Side::Buy, 500), before);
}

TEST(FeedBook, ADeletionRemovesTheOrderWhateverItsSize) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 7, 30, 50'000, Side::Buy));
    book.apply(event(EventType::Delete, 7, 1, 50'000, Side::Buy));
    EXPECT_EQ(book.book().size(), 0u);
}

TEST(FeedBook, AnIdCanBeUsedAgainOnceItsOrderIsGone) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 7, 30, 50'000, Side::Buy));
    book.apply(event(EventType::Delete, 7, 30, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 7, 12, 50'100, Side::Buy));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 501), 12);
    EXPECT_EQ(book.stats().rejected, 0u);
}

TEST(FeedBook, IgnoresWhatAVisibleBookDoesNotHold) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 7, 30, 50'000, Side::Buy));
    book.apply(event(EventType::HiddenExecute, 0, 10, 50'000, Side::Buy));
    book.apply(event(EventType::Cross, 0, 10, 50'000, Side::Buy));
    book.apply(event(EventType::Halt, 0, 0, -1, Side::Sell));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 30);
    EXPECT_EQ(book.stats().ignored, 3u);
}

TEST(FeedBook, RejectsAddsTheBookCannotHold) {
    feed::FeedBook book(2, BAND, TICK);
    book.apply(event(EventType::Add, 1, 10, 50'050, Side::Buy));     // off the tick
    book.apply(event(EventType::Add, 2, 10, 5'000'000, Side::Buy));  // out of band
    book.apply(event(EventType::Add, 3, 0, 50'000, Side::Buy));      // no quantity
    book.apply(event(EventType::Add, 4, 10, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 4, 10, 50'000, Side::Buy));  // the id is live
    book.apply(event(EventType::Add, 5, 10, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 6, 10, 50'000, Side::Buy));  // the book is full
    EXPECT_EQ(book.book().size(), 2u);
    EXPECT_EQ(book.stats().rejected, 5u);
    // A rejected add leaves nothing behind for a later event to find
    book.apply(event(EventType::Delete, 6, 10, 50'000, Side::Buy));
    EXPECT_EQ(book.stats().unknown, 1u);
    EXPECT_EQ(book.book().size(), 2u);
}

TEST(FeedBook, EventsForOrdersOlderThanTheFeedComeOffTheStandIn) {
    feed::FeedBook book = small_book();
    ASSERT_TRUE(book.seed(Side::Sell, 50'300, 100));
    ASSERT_TRUE(book.seed(Side::Sell, 50'300, 50));
    EXPECT_EQ(book.book().level_qty(Side::Sell, 503), 150);
    EXPECT_EQ(book.book().size(), 1u);

    book.apply(event(EventType::Execute, 111, 40, 50'300, Side::Sell));
    book.apply(event(EventType::Delete, 222, 60, 50'300, Side::Sell));
    EXPECT_EQ(book.book().level_qty(Side::Sell, 503), 50);
    // Not at another price, and not on the other side
    book.apply(event(EventType::Delete, 333, 10, 50'400, Side::Sell));
    book.apply(event(EventType::Delete, 444, 10, 50'300, Side::Buy));
    EXPECT_EQ(book.stats().unknown, 2u);

    book.apply(event(EventType::Delete, 555, 50, 50'300, Side::Sell));
    EXPECT_EQ(book.book().size(), 0u);
    book.apply(event(EventType::Delete, 666, 1, 50'300, Side::Sell));
    EXPECT_EQ(book.stats().unknown, 3u);
}

TEST(FeedBook, SeedingFailsWhereTheBookCannotHoldThePrice) {
    feed::FeedBook book = small_book();
    EXPECT_FALSE(book.seed(Side::Sell, 50'350, 100));
    EXPECT_FALSE(book.seed(Side::Sell, 5'000'000, 100));
    EXPECT_FALSE(book.seed(Side::Sell, 50'300, 0));
    EXPECT_EQ(book.book().size(), 0u);
}

TEST(FeedBook, ResettingALevelLeavesOneStandIn) {
    feed::FeedBook book = small_book();
    book.apply(event(EventType::Add, 1, 30, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 2, 20, 50'000, Side::Buy));
    book.apply(event(EventType::Add, 3, 10, 49'900, Side::Buy));
    ASSERT_TRUE(book.reset_level(Side::Buy, 50'000, 40));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 40);
    EXPECT_EQ(book.book().orders_at(Side::Buy, 500).size(), 1u);
    EXPECT_EQ(book.book().level_qty(Side::Buy, 499), 10);

    // The orders that were there are forgotten, so their events come off the stand-in
    book.apply(event(EventType::Delete, 1, 30, 50'000, Side::Buy));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 10);
    // Their ids are free again
    book.apply(event(EventType::Add, 2, 5, 50'100, Side::Buy));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 501), 5);
    EXPECT_EQ(book.stats().rejected, 0u);

    // A level that is one stand-in already
    ASSERT_TRUE(book.reset_level(Side::Buy, 50'000, 25));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 25);
    ASSERT_TRUE(book.reset_level(Side::Buy, 50'000, 0));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 500), 0);
    ASSERT_TRUE(book.reset_level(Side::Buy, 49'900, 0));
    EXPECT_EQ(book.book().size(), 1u);
    ASSERT_TRUE(book.reset_level(Side::Buy, 49'000, 15));
    EXPECT_EQ(book.book().level_qty(Side::Buy, 490), 15);
    EXPECT_FALSE(book.reset_level(Side::Buy, 49'050, 15));
}

TEST(Checker, TakesTheFilesWordForWhatItSeesFirstAndChecksItAfterwards) {
    feed::FeedBook book = small_book();
    feed::Checker checker(book);
    checker.check(row({{50'300, 100}, {50'500, 40}}, {{50'000, 30}, {49'800, 10}}));
    EXPECT_EQ(checker.synced(), 4u);
    EXPECT_EQ(checker.levels(), 0u);
    EXPECT_EQ(book.book().level_qty(Side::Sell, 503), 100);
    EXPECT_EQ(book.book().level_qty(Side::Buy, 498), 10);

    book.apply(event(EventType::Add, 1, 5, 50'300, Side::Sell));
    checker.check(row({{50'300, 105}, {50'500, 40}}, {{50'000, 30}, {49'800, 10}}));
    // An order older than the feed leaves
    book.apply(event(EventType::Delete, 99, 30, 50'000, Side::Buy));
    checker.check(row({{50'300, 105}, {50'500, 40}}, {{49'800, 10}, {49'500, 70}}));

    EXPECT_EQ(checker.rows(), 3u);
    EXPECT_EQ(checker.levels(), 7u);
    // 49,500 came into view once 50,000 emptied
    EXPECT_EQ(checker.synced(), 5u);
    EXPECT_EQ(book.book().level_qty(Side::Buy, 495), 70);
    EXPECT_EQ(checker.mismatches(), 0u);
    EXPECT_EQ(checker.first_mismatch(), std::nullopt);
}

// A level out of view changes with no event, so the book's orders there may be long gone
TEST(Checker, ALevelThatComesBackIntoViewIsTakenFromTheFile) {
    feed::FeedBook book = small_book();
    feed::Checker checker(book);
    checker.check(row({{50'300, 100}, {50'500, 40}}, {{50'000, 30}}));
    book.apply(event(EventType::Add, 1, 25, 50'500, Side::Sell));
    checker.check(row({{50'300, 100}, {50'500, 65}}, {{50'000, 30}}));
    // A better ask pushes 50,500 out of view. While it's out, order 1 leaves and another
    // comes, and a level appears at 50,400 and goes again
    book.apply(event(EventType::Add, 2, 10, 50'200, Side::Sell));
    checker.check(row({{50'200, 10}, {50'300, 100}}, {{50'000, 30}}));
    ASSERT_TRUE(book.seed(Side::Sell, 50'400, 9));
    book.apply(event(EventType::Delete, 2, 10, 50'200, Side::Sell));
    checker.check(row({{50'300, 100}, {50'500, 65}}, {{50'000, 30}}));
    EXPECT_EQ(checker.mismatches(), 0u);
    EXPECT_EQ(book.book().level_qty(Side::Sell, 504), 0);
    EXPECT_EQ(book.book().orders_at(Side::Sell, 505).size(), 1u);

    // Order 1 is forgotten, so the order that took its place can leave
    book.apply(event(EventType::Delete, 77, 25, 50'500, Side::Sell));
    checker.check(row({{50'300, 100}, {50'500, 40}}, {{50'000, 30}}));
    EXPECT_EQ(checker.mismatches(), 0u);
}

TEST(Checker, ReportsAQuantityThatDiffers) {
    feed::FeedBook book = small_book();
    feed::Checker checker(book);
    checker.check(row({{50'300, 100}}, {{50'000, 30}}));
    book.apply(event(EventType::Add, 1, 5, 50'300, Side::Sell));
    checker.check(row({{50'300, 100}}, {{50'000, 30}}));
    ASSERT_EQ(checker.mismatches(), 1u);
    const feed::Mismatch& first = *checker.first_mismatch();
    EXPECT_EQ(first.row, 1u);
    EXPECT_EQ(first.side, Side::Sell);
    EXPECT_EQ(first.price, 50'300);
    EXPECT_EQ(first.expected, 100);
    EXPECT_EQ(first.actual, 105);
}

TEST(Checker, ReportsALevelTheFileDoesNotHave) {
    feed::FeedBook book = small_book();
    feed::Checker checker(book);
    checker.check(row({{50'300, 100}, {50'600, 10}}, {{50'000, 30}}));

    // Better than the file's best
    book.apply(event(EventType::Add, 1, 5, 50'200, Side::Sell));
    checker.check(row({{50'300, 100}, {50'600, 10}}, {{50'000, 30}}));
    ASSERT_EQ(checker.mismatches(), 1u);
    EXPECT_EQ(checker.first_mismatch()->price, 50'200);
    EXPECT_EQ(checker.first_mismatch()->expected, 0);
    EXPECT_EQ(checker.first_mismatch()->actual, 5);
    book.apply(event(EventType::Delete, 1, 5, 50'200, Side::Sell));

    // Between two levels the file lists
    book.apply(event(EventType::Add, 2, 5, 50'400, Side::Sell));
    checker.check(row({{50'300, 100}, {50'600, 10}}, {{50'000, 30}}));
    EXPECT_EQ(checker.mismatches(), 2u);
}

TEST(Checker, ReportsALevelTheBookLacks) {
    feed::FeedBook book = small_book();
    feed::Checker checker(book);
    checker.check(row({{50'300, 100}}, {{50'000, 30}}));
    book.apply(event(EventType::Delete, 99, 100, 50'300, Side::Sell));
    checker.check(row({{50'300, 100}}, {{50'000, 30}}));
    ASSERT_EQ(checker.mismatches(), 1u);
    EXPECT_EQ(checker.first_mismatch()->actual, 0);
}

// Stub quotes far from the market are out of band: nothing to sync, nothing to compare
TEST(Checker, SkipsLevelsTheBookCannotHold) {
    feed::FeedBook book = small_book();
    feed::Checker checker(book);
    checker.check(row({{50'300, 100}, {1'999'999'900, 100}}, {{50'000, 30}, {50, 100}}));
    checker.check(row({{50'300, 100}, {1'999'999'900, 100}}, {{50'000, 30}, {50, 100}}));
    EXPECT_EQ(checker.synced(), 2u);
    EXPECT_EQ(checker.levels(), 2u);
    EXPECT_EQ(checker.mismatches(), 0u);
}
}  // namespace
