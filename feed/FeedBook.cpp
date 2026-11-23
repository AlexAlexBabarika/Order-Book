#include "FeedBook.h"

#include <cassert>
#include <cstddef>
#include <optional>

namespace lob::feed {

namespace {
// Nothing here can trade: a smaller quantity at the same price shrinks the order in place
constexpr auto NO_FILLS = [](const Fill&) {};
}  // namespace

FeedBook::FeedBook(const std::uint32_t max_orders, const PriceBand band, const Price tick)
    : book_(max_orders, band), band_(band), tick_(tick) {
    assert(tick > 0);
    // One more than can rest: an add takes its entry before the book can turn it away
    index_.reserve(std::size_t{max_orders} + 1);
}

void FeedBook::apply(const Event& event) {
    switch (event.type) {
        case EventType::Add:
            add(event);
            return;
        case EventType::Reduce:
        case EventType::Delete:
        case EventType::Execute:
            take(event);
            return;
        case EventType::HiddenExecute:
        case EventType::Cross:
        case EventType::Halt:
            ++stats_.ignored;
            return;
    }
}

void FeedBook::add(const Event& event) {
    const std::optional<Price> ticks = to_ticks(event.price);
    if (!ticks || (event.id & STAND_IN) != 0) {
        ++stats_.rejected;
        return;
    }
    const auto [it, inserted] = index_.try_emplace(event.id);
    if (!inserted) {
        ++stats_.rejected;
        return;
    }
    // add(), not submit(): the exchange matched the order before it reached the feed
    const Placed placed =
        book_.add({.price = *ticks, .original = event.size, .side = event.side, .tif = Tif::Day});
    if (placed.result != Result::Ok) {
        index_.erase(it);
        ++stats_.rejected;
        return;
    }
    it->second = {.id = placed.id, .remaining = event.size, .price = *ticks};
    ++stats_.applied;
}

void FeedBook::take(const Event& event) {
    auto it = index_.find(event.id);
    if (it == index_.end()) {
        it = stand_in_for(event);
        if (it == index_.end()) {
            ++stats_.unknown;
            return;
        }
    } else if (event.type == EventType::Delete) {
        remove(it);
        ++stats_.applied;
        return;
    }

    Resting& order = it->second;
    if (event.size >= order.remaining) {
        remove(it);
    } else if (event.size > 0) {
        order.remaining -= event.size;
        book_.modify(order.id, order.price, order.remaining, NO_FILLS);
    }
    ++stats_.applied;
}

FeedBook::Index::iterator FeedBook::stand_in_for(const Event& event) {
    const std::optional<Price> ticks = to_ticks(event.price);
    return ticks ? index_.find(stand_in_key(event.side, *ticks)) : index_.end();
}

void FeedBook::remove(const Index::iterator it) {
    book_.cancel(it->second.id);
    index_.erase(it);
}

bool FeedBook::seed(const Side side, const Price feed_price, const Qty qty) {
    const std::optional<Price> ticks = to_ticks(feed_price);
    if (!ticks || qty <= 0) {
        return false;
    }
    const auto [it, inserted] = index_.try_emplace(stand_in_key(side, *ticks));
    if (!inserted) {
        // More at the same price. Growing an order is a cancel and replace under the same id
        it->second.remaining += qty;
        book_.modify(it->second.id, *ticks, it->second.remaining, NO_FILLS);
        return true;
    }
    const Placed placed =
        book_.add({.price = *ticks, .original = qty, .side = side, .tif = Tif::Day});
    if (placed.result != Result::Ok) {
        index_.erase(it);
        return false;
    }
    it->second = {.id = placed.id, .remaining = qty, .price = *ticks};
    return true;
}

bool FeedBook::reset_level(const Side side, const Price feed_price, const Qty qty) {
    const std::optional<Price> ticks = to_ticks(feed_price);
    if (!ticks) {
        return false;
    }
    const Qty held = book_.level_qty(side, *ticks);
    const auto it = index_.find(stand_in_key(side, *ticks));
    if (it != index_.end() && it->second.remaining == held) {
        // The stand-in is alone at the price, so only its quantity has to change
        if (qty == 0) {
            remove(it);
        } else if (qty != held) {
            it->second.remaining = qty;
            book_.modify(it->second.id, *ticks, qty, NO_FILLS);
        }
        return true;
    }
    if (held > 0) {
        for (const OrderId id : book_.orders_at(side, *ticks)) {
            book_.cancel(id);
        }
        std::erase_if(index_,
                      [this](const auto& entry) { return !book_.contains(entry.second.id); });
    }
    return qty == 0 || seed(side, feed_price, qty);
}

}  // namespace lob::feed
