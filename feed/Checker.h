#pragma once
#include <lob/CoreTypes.h>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
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

// Checks a FeedBook against the rows of an orderbook file, one row after each event.
//
// A message file only has the events inside the levels its orderbook file shows. Orders from
// before the feed starts are never named, and a level that drops out of view can change with
// no event to say so. So a price that the row before didn't show isn't checked: the checker
// takes the file's word for it and makes the book agree. A price the row before did show is
// checked, since every change to it since then was an event
class Checker {
   public:
    explicit Checker(FeedBook& book) : book_(book) {}

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
    // The furthest price the last row showed on each side, as a key that grows away from the
    // market: the price for asks, and its negative for bids
    std::array<Price, 2> shown_{NOTHING_SHOWN, NOTHING_SHOWN};
    std::size_t rows_ = 0;
    std::size_t levels_ = 0;
    std::size_t synced_ = 0;
    std::size_t mismatches_ = 0;
    std::optional<Mismatch> first_;
};

}  // namespace lob::feed
