#include "Checker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lob::feed {

void Checker::check(const BookRow& row) {
    check_side(Side::Sell, row.asks);
    check_side(Side::Buy, row.bids);
    ++rows_;
}

void Checker::check_side(const Side side, const std::vector<Quote>& quotes) {
    const OrderBook& book = book_.book();
    const Price tick = book_.tick();
    const Price away = side == Side::Buy ? -1 : 1;
    const Price shown = shown_[static_cast<std::size_t>(side)];
    // The level before this one, if the book can hold it
    std::optional<Price> previous;
    bool first = true;

    for (const Quote& quote : quotes) {
        const std::optional<Price> ticks = book_.to_ticks(quote.price);
        if (!ticks || !book_.band().contains(*ticks)) {
            previous.reset();
            first = false;
            continue;
        }

        const bool was_shown = away * quote.price <= shown;
        if (was_shown) {
            ++levels_;
            expect(side, quote.price, quote.qty, book.level_qty(side, *ticks));
        } else {
            sync(side, quote.price, quote.qty);
        }

        if (first) {
            // Nothing in the book may be better than the file's best
            const std::optional<Price> best = side == Side::Buy ? book.best_bid() : book.best_ask();
            if (best && away * (*best - *ticks) < 0) {
                expect(side, *best * tick, 0, book.level_qty(side, *best));
            }
        } else if (previous && (!was_shown || away * (*ticks - *previous) <= MAX_GAP)) {
            // Nor may anything lie between two levels the file lists next to each other
            for (Price t = *previous + away; t != *ticks; t += away) {
                if (away * t * tick <= shown) {
                    expect(side, t * tick, 0, book.level_qty(side, t));
                } else {
                    sync(side, t * tick, 0);
                }
            }
        }
        previous = ticks;
        first = false;
    }

    shown_[static_cast<std::size_t>(side)] =
        quotes.empty() ? NOTHING_SHOWN : away * quotes.back().price;
}

// The book's orders at a price that was out of view can't be trusted, even when they add up
// to what the file shows: any of them may have left, and others come, with no event. So
// whatever is there becomes one stand-in for the file's quantity
void Checker::sync(const Side side, const Price feed_price, const Qty qty) {
    const Qty known = book_.book().level_qty(side, *book_.to_ticks(feed_price));
    if (known == 0 && qty == 0) {
        return;
    }
    std::vector<std::uint64_t>* const dropped =
        corrections_ == nullptr ? nullptr : &corrections_->dropped;
    const std::size_t before = dropped == nullptr ? 0 : dropped->size();
    const bool done = known == 0 ? book_.seed(side, feed_price, qty)
                                 : book_.reset_level(side, feed_price, qty, dropped);
    if (corrections_ != nullptr) {
        corrections_->levels.push_back({.row = rows_,
                                        .side = side,
                                        .price = feed_price,
                                        .qty = qty,
                                        .dropped = dropped->size() - before});
    }
    if (!done) {
        expect(side, feed_price, qty, known);
    } else if (known != qty) {
        ++synced_;
    }
}

void Checker::expect(const Side side, const Price feed_price, const Qty expected,
                     const Qty actual) {
    if (expected == actual) {
        return;
    }
    ++mismatches_;
    if (!first_) {
        first_ = {.row = rows_,
                  .side = side,
                  .price = feed_price,
                  .expected = expected,
                  .actual = actual};
    }
}

void replay(FeedBook& book, const std::span<const Event> events, const std::size_t first,
            const Corrections& corrections) {
    auto level = corrections.levels.begin();
    const std::span<const std::uint64_t> dropped = corrections.dropped;
    std::size_t taken = 0;
    const auto correct_up_to = [&](const std::size_t row) {
        for (; level != corrections.levels.end() && level->row <= row; ++level) {
            book.repeat_reset(level->side, level->price, level->qty,
                              dropped.subspan(taken, level->dropped));
            taken += level->dropped;
        }
    };

    if (first > 0) {
        correct_up_to(first - 1);
    }
    for (std::size_t row = first; row < events.size(); ++row) {
        book.apply(events[row]);
        correct_up_to(row);
    }
}

}  // namespace lob::feed
