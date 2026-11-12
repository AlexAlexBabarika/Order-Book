#include "lob/OrderBook.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "lob/CoreTypes.h"

namespace lob {

namespace {

// Bids are keyed by -price, so their keys run over the band negated: prices [lo, hi) become
// keys (-hi, -lo], which is [1 - hi, 1 - lo)
PriceBand bid_key_band(const PriceBand band) { return {.lo = 1 - band.hi, .hi = 1 - band.lo}; }

}  // namespace

// No more than max_orders orders rest, plus one in submit while it matches. Every level holds
// at least one resting order and is released when it empties, so there are never more levels
// than max_orders, and a level pool of that size can't run out. Neither can a ladder, since
// one side never has more levels than the whole book has orders
OrderBook::OrderBook(const std::uint32_t max_orders, const PriceBand band)
    : order_pool(max_orders + 1),
      level_pool(max_orders),
      band(band),
      levels{PriceLevels(bid_key_band(band), max_orders), PriceLevels(band, max_orders)},
      max_orders(max_orders) {
    // Slot NIL can't be handed out
    assert(max_orders < NIL - 1);
}

Placed OrderBook::add(const Order& order) {
    if (const Result result = validate(order); result != Result::Ok) {
        return {.result = result};
    }
    if (!band.contains(order.price)) {
        return {.result = Result::OutOfBand};
    }
    if (full()) {
        return {.result = Result::BookFull};
    }

    const Slot slot = open_slot();
    rest(slot, order);
    return {.result = Result::Ok, .id = id_of(slot)};
}

Result OrderBook::cancel(const OrderId id) {
    const Slot slot = find(id);
    if (slot == NIL) {
        return Result::UnknownId;
    }

    erase_order(slot);
    return Result::Ok;
}

OrderBook::Amended OrderBook::amend(const OrderId id, const Price newPrice, const Qty newQty) {
    const Slot slot = find(id);
    if (slot == NIL) {
        return {.result = Result::UnknownId};
    }
    if (newQty <= 0) {
        erase_order(slot);
        return {.result = Result::Ok};
    }
    if (!band.contains(newPrice)) {
        return {.result = Result::OutOfBand};
    }
    if (shrink_in_place(slot, newPrice, newQty)) {
        return {.result = Result::Ok};
    }

    const Order order = replacement(slot, newPrice, newQty);
    if (crosses(order)) {
        return {.result = Result::Ok, .crossing = slot};
    }
    // Nothing to trade, so this is the whole cancel and replace. Taking the order off first
    // releases its level before rest() asks for one, so a full book can still move it
    take_off_book(slot);
    rest(slot, order);
    return {.result = Result::Ok};
}

Order OrderBook::replacement(const Slot slot, const Price newPrice, const Qty newQty) const {
    const OrderNode& node = order_pool[slot];
    return {.price = newPrice,
            .original = node.original - node.remaining + newQty,
            .remaining = newQty,
            .side = node.side,
            .tif = node.tif};
}

bool OrderBook::shrink_in_place(const Slot slot, const Price newPrice, const Qty newQty) {
    OrderNode& node = order_pool[slot];
    Level& level = level_pool[node.level];
    if (newPrice != level.price || newQty > node.remaining) {
        return false;
    }
    level.total -= node.remaining - newQty;
    node.original -= node.remaining - newQty;
    node.remaining = newQty;
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    const PriceLevels& bids = levels_of(Side::Buy);
    if (bids.empty()) {
        return std::nullopt;
    }
    return key(Side::Buy, bids.best_price());
}

std::optional<Price> OrderBook::best_ask() const {
    const PriceLevels& asks = levels_of(Side::Sell);
    if (asks.empty()) {
        return std::nullopt;
    }
    return key(Side::Sell, asks.best_price());
}

std::optional<Price> OrderBook::spread() const {
    const std::optional<Price> bid = best_bid();
    const std::optional<Price> ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

// Walks every slot, which is fine once a session
void OrderBook::end_session() {
    for (Slot slot = 0; slot < order_pool.capacity(); ++slot) {
        const OrderNode& node = order_pool[slot];
        if (node.generation % 2 == 1 && node.tif == Tif::Day) {
            erase_order(slot);
        }
    }
}

std::size_t OrderBook::size() const { return resting_orders; }

bool OrderBook::contains(const OrderId id) const { return find(id) != NIL; }

Qty OrderBook::level_qty(const Side side, const Price price) const {
    const Level* level = level_at(side, price);
    return level == nullptr ? 0 : level->total;
}

std::vector<OrderId> OrderBook::orders_at(const Side side, const Price price) const {
    std::vector<OrderId> ids;
    const Level* level = level_at(side, price);
    if (level == nullptr) {
        return ids;
    }

    ids.reserve(level->count);
    for (Slot slot = level->head; slot != NIL; slot = order_pool[slot].next) {
        ids.push_back(id_of(slot));
    }
    return ids;
}

Slot OrderBook::find(const OrderId id) const {
    const auto slot = static_cast<Slot>(id);
    const auto generation = static_cast<std::uint32_t>(id >> 32);
    // An even generation is never handed out, and a free slot's generation is always even
    if (generation % 2 == 0 || slot >= order_pool.capacity() ||
        order_pool[slot].generation != generation) {
        return NIL;
    }
    return slot;
}

// The generation turns odd, which brings the slot's next id to life
Slot OrderBook::open_slot() {
    const Slot slot = order_pool.acquire();
    assert(slot != NIL && "the order pool keeps a slot spare for submit");
    ++order_pool[slot].generation;
    return slot;
}

// The generation turns even, so the id stops matching before the slot can be reused
void OrderBook::close_slot(const Slot slot) {
    ++order_pool[slot].generation;
    order_pool.release(slot);
}

void OrderBook::rest(const Slot slot, const Order& order) {
    OrderNode& node = order_pool[slot];
    node.remaining = order.remaining;
    node.original = order.original;
    node.side = order.side;
    node.tif = order.tif;
    link_back(level_for(order.side, order.price), slot);
    ++resting_orders;
}

void OrderBook::erase_order(const Slot slot) {
    take_off_book(slot);
    close_slot(slot);
}

void OrderBook::link_back(const LevelSlot level_slot, const Slot slot) {
    OrderNode& node = order_pool[slot];
    Level& level = level_pool[level_slot];
    node.level = level_slot;
    node.prev = level.tail;
    node.next = NIL;
    if (level.tail == NIL) {
        level.head = slot;
    } else {
        order_pool[level.tail].next = slot;
    }
    level.tail = slot;
    ++level.count;
    level.total += node.remaining;
}

void OrderBook::unlink(const Slot slot) {
    const OrderNode& node = order_pool[slot];
    Level& level = level_pool[node.level];
    if (node.prev == NIL) {
        level.head = node.next;
    } else {
        order_pool[node.prev].next = node.next;
    }
    if (node.next == NIL) {
        level.tail = node.prev;
    } else {
        order_pool[node.next].prev = node.prev;
    }
    --level.count;
    level.total -= node.remaining;
}

LevelSlot OrderBook::level_for(const Side side, const Price price) {
    auto [level_slot, inserted] = levels_of(side).try_emplace(key(side, price));
    if (inserted) {
        const LevelSlot slot = level_pool.acquire();
        assert(slot != NIL && "level pool ran out: an emptied level wasn't released");
        // A fresh level, because a free one still holds its free-list link in head
        level_pool[slot] = Level{.price = price};
        level_slot = slot;
    }
    return level_slot;
}

void OrderBook::erase_if_empty(const LevelSlot level_slot, const Side side) {
    const Level& level = level_pool[level_slot];
    if (level.head != NIL) {
        return;
    }
    levels_of(side).erase(key(side, level.price));
    level_pool.release(level_slot);
}

const Level* OrderBook::level_at(const Side side, const Price price) const {
    // No level lies outside the band, and checking first keeps key() from negating INT64_MIN
    if (!band.contains(price)) {
        return nullptr;
    }
    const LevelSlot slot = levels_of(side).find(key(side, price));
    return slot == NIL ? nullptr : &level_pool[slot];
}

Result OrderBook::validate(const Order& order) {
    if (order.remaining <= 0 || order.remaining > order.original) {
        return Result::BadQty;
    }
    return Result::Ok;
}

bool OrderBook::can_fill(const Order& incoming) const {
    const Price limit = limit_key(incoming);
    Qty total = 0;
    levels_of(opposite(incoming.side))
        .for_each_from_best([&](const Price maker_key, const LevelSlot level_slot) {
            if (maker_key > limit) {
                return false;
            }
            total += level_pool[level_slot].total;
            return total < incoming.remaining;
        });
    return total >= incoming.remaining;
}

bool OrderBook::should_rest(const Order& order) {
    if (order.remaining <= 0) {
        return false;
    }
    if (order.type != OrderType::Limit) {
        return false;
    }
    switch (order.tif) {
        case Tif::Gtc:
            return true;
        case Tif::Day:
            return true;
        case Tif::Ioc:
            return false;
        case Tif::Fok:
            return false;
    }
    return false;
}
}  // namespace lob
