#pragma once
#include <lob/CoreTypes.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "FeedBook.h"
#include "Lobster.h"

namespace lob::feed {

// A price where the book and the orderbook file disagree. The price is in the feed's units
struct Mismatch {
    std::size_t row;  // counted from 0
    Side side;
    Price price;
    Qty expected;
    Qty actual;
};

struct Correction {
    std::size_t row;  // counted from 0. It follows the event on that line of the message file
    Side side;
    Price price;          // in the feed's units
    Qty qty;              // what the level holds afterwards, as one stand-in
    std::size_t dropped;  // how many of Corrections::dropped it takes out first
};

struct Corrections {
    std::vector<Correction> levels;
    // The FeedBook's keys for the orders the levels took out, in the order they use them
    std::vector<std::uint64_t> dropped;
};

// Checks a FeedBook against the rows of an orderbook file, one row after each event.
//
// A message file only has the events inside the levels its orderbook file shows. Orders from
// before the feed starts are never named, and a level that drops out of view can change with
// no event to say so. So a price that the row before didn't show isn't checked: the checker
// takes the file's word for it and makes the book agree. A price the row before did show is
// checked, since every change to it since then was an event
class Checker {
   public:
    // The changes it makes to the book are added to corrections, if given
    explicit Checker(FeedBook& book, Corrections* corrections = nullptr)
        : book_(book), corrections_(corrections) {}

    void check(const BookRow& row);

    std::size_t rows() const { return rows_; }
    std::size_t levels() const { return levels_; }  // compared with the file
    std::size_t synced() const { return synced_; }  // changed to agree with the file
    std::size_t mismatches() const { return mismatches_; }
    const std::optional<Mismatch>& first_mismatch() const { return first_; }

   private:
    // Gaps between listed levels wider than this aren't searched for levels that shouldn't
    // be there, which keeps a stub quote far from the market from costing a scan per row
    static constexpr Price MAX_GAP = 256;
    static constexpr Price NOTHING_SHOWN = std::numeric_limits<Price>::min();

    void check_side(Side side, const std::vector<Quote>& quotes);
    void expect(Side side, Price feed_price, Qty expected, Qty actual);
    void sync(Side side, Price feed_price, Qty qty);

    FeedBook& book_;
    Corrections* corrections_;
    // The furthest price the last row showed on each side, as a key that grows away from the
    // market: the price for asks, and its negative for bids
    std::array<Price, 2> shown_{NOTHING_SHOWN, NOTHING_SHOWN};
    std::size_t rows_ = 0;
    std::size_t levels_ = 0;
    std::size_t synced_ = 0;
    std::size_t mismatches_ = 0;
    std::optional<Mismatch> first_;
};

// Applies the events from first on and, after each, the corrections a checker made at its
// row, having started with those of the rows before first. On a fresh book that leaves what
// the checker left, with no orderbook file to read. Each step takes constant time and
// allocates nothing
void replay(FeedBook& book, std::span<const Event> events, std::size_t first,
            const Corrections& corrections);

}  // namespace lob::feed
