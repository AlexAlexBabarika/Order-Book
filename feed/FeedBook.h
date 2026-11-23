#pragma once
#include <ankerl/unordered_dense.h>
#include <lob/CoreTypes.h>
#include <lob/OrderBook.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "Lobster.h"

namespace lob::feed {

struct Stats {
    std::size_t applied = 0;  // changed the book
    // Hidden executions, crosses and halts, which change nothing a visible book holds
    std::size_t ignored = 0;
    std::size_t unknown = 0;   // named an order the book doesn't hold
    std::size_t rejected = 0;  // adds turned away: off the tick, out of band, or the book is full
};

// A book built from a feed. The feed names orders by its own ids, and the book hands out its
// own, so this keeps the index from one to the other that the book itself dropped in step 9.
// A feed is what's left after matching, so nothing here matches: adds rest, and executions
// take quantity off the order the feed names
class FeedBook {
   public:
    // tick is the feed's price units per tick of the book. band is in the book's ticks
    FeedBook(std::uint32_t max_orders, PriceBand band, Price tick);

    void apply(const Event& event);

    // Rests quantity that stands in for orders older than the feed, which it never names.
    // An event for an order the index doesn't hold comes off the stand-in at its price.
    // False if the price is off the tick or out of band, or the book is full
    bool seed(Side side, Price feed_price, Qty qty);
    // Replaces every order at the price with one stand-in for qty, or with nothing if qty is
    // 0. For a level that may have changed where the feed doesn't report, so that which of
    // its orders are still there can't be known. Unless the level already is one stand-in, it
    // walks the whole index and allocates
    bool reset_level(Side side, Price feed_price, Qty qty);

    // Nothing if the price isn't a whole number of ticks
    std::optional<Price> to_ticks(const Price feed_price) const {
        if (feed_price % tick_ != 0) {
            return std::nullopt;
        }
        return feed_price / tick_;
    }

    const OrderBook& book() const { return book_; }
    PriceBand band() const { return band_; }
    Price tick() const { return tick_; }
    const Stats& stats() const { return stats_; }

   private:
    struct Resting {
        OrderId id;
        Qty remaining;
        Price price;  // in ticks
    };
    using Index = ankerl::unordered_dense::map<std::uint64_t, Resting>;

    // Stand-ins share the index with the feed's orders, under keys with the top bit set. The
    // feed's own ids must leave that bit clear
    static constexpr std::uint64_t STAND_IN = std::uint64_t{1} << 63;
    static std::uint64_t stand_in_key(const Side side, const Price ticks) {
        return STAND_IN | (static_cast<std::uint64_t>(side) << 62) |
               (static_cast<std::uint64_t>(ticks) & ((std::uint64_t{1} << 62) - 1));
    }

    void add(const Event& event);
    void take(const Event& event);
    Index::iterator stand_in_for(const Event& event);
    void remove(Index::iterator it);

    OrderBook book_;
    // Reserved for max_orders, so it doesn't allocate after construction
    Index index_;
    PriceBand band_;
    Price tick_;
    Stats stats_;
};

}  // namespace lob::feed
