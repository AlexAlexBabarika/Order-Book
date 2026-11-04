#include "lob/OrderBook.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "lob/CoreTypes.h"

namespace lob {

namespace {

template <typename Book>
LevelSlot find_or_add_level(Book& book, LevelPool& levels, const Price price) {
    const auto [iter, inserted] = book.try_emplace(price, NIL);
    if (inserted) {
        const LevelSlot slot = levels.acquire();
        assert(slot != NIL && "level pool ran out: an emptied level wasn't released");
        // A fresh level, because a free one still holds its free-list link in head
        levels[slot] = Level{.price = price};
        iter->second = slot;
    }
    return iter->second;
}

template <typename Book>
const Level* find_level(const Book& book, const LevelPool& levels, const Price price) {
    const auto iter = book.find(price);
    return iter == book.end() ? nullptr : &levels[iter->second];
}

}  // namespace

// add() stops at a full pool, so the index never grows past this. Every level holds at least
// one order and is released when it empties, so there are never more levels than orders,
// and a level pool of the same size can't run out
OrderBook::OrderBook(const std::uint32_t max_orders)
    : order_pool(max_orders), level_pool(max_orders) {
    order_lookup.reserve(max_orders);
}

Result OrderBook::add(const Order& order) {
    if (const Result result = validate(order); result != Result::Ok) {
        return result;
    }
    if (order_pool.full()) {
        return Result::BookFull;
    }

    const auto [iter, inserted] = order_lookup.try_emplace(order.id);
    if (!inserted) {
        return Result::DuplicateId;
    }

    const Slot slot = order_pool.acquire();
    iter->second = slot;
    order_pool[slot] = {.id = order.id,
                        .remaining = order.remaining,
                        .original = order.original,
                        .side = order.side,
                        .tif = order.tif};
    link_back(level_for(order.side, order.price), slot);
    return Result::Ok;
}

Result OrderBook::cancel(const OrderId id) {
    const auto iter = order_lookup.find(id);
    if (iter == order_lookup.end()) {
        return Result::UnknownId;
    }

    erase_order(iter);
    return Result::Ok;
}

Result OrderBook::modify(const OrderId id, const Price newPrice, const Qty newQty) {
    auto iter = order_lookup.find(id);

    if (iter == order_lookup.end()) {
        return Result::UnknownId;
    }

    if (newQty <= 0) {
        erase_order(iter);
        return Result::Ok;
    }

    const Slot slot = iter->second;
    OrderNode& resting = order_pool[slot];
    const LevelSlot level_slot = resting.level;
    Level& level = level_pool[level_slot];
    const Qty filled = resting.original - resting.remaining;

    if (newPrice == level.price && newQty <= resting.remaining) {
        level.total -= resting.remaining - newQty;
        resting.remaining = newQty;
        resting.original = filled + newQty;
        return Result::Ok;
    }

    unlink(slot);
    resting.remaining = newQty;
    resting.original = filled + newQty;
    if (newPrice == level.price) {
        link_back(level_slot, slot);
    } else {
        // Release before acquiring, so a full book can still move its order to a new price
        erase_if_empty(level_slot, resting.side);
        link_back(level_for(resting.side, newPrice), slot);
    }
    return Result::Ok;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bid_map.empty()) {
        return std::nullopt;
    }
    return bid_map.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (ask_map.empty()) {
        return std::nullopt;
    }
    return ask_map.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    const std::optional<Price> bid = best_bid();
    const std::optional<Price> ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

void OrderBook::end_session() {
    for (auto iter = order_lookup.begin(); iter != order_lookup.end();) {
        if (order_pool[iter->second].tif == Tif::Day) {
            iter = erase_order(iter);
        } else {
            ++iter;
        }
    }
}

std::size_t OrderBook::size() const { return order_lookup.size(); }

bool OrderBook::contains(OrderId id) const { return order_lookup.contains(id); }

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
        ids.push_back(order_pool[slot].id);
    }
    return ids;
}

OrderBook::LookupIter OrderBook::erase_order(LookupIter iter) {
    const Slot slot = iter->second;
    const OrderNode& node = order_pool[slot];

    unlink(slot);
    erase_if_empty(node.level, node.side);
    order_pool.release(slot);
    return order_lookup.erase(iter);
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
    return side == Side::Buy ? find_or_add_level(bid_map, level_pool, price)
                             : find_or_add_level(ask_map, level_pool, price);
}

void OrderBook::erase_if_empty(const LevelSlot level_slot, const Side side) {
    const Level& level = level_pool[level_slot];
    if (level.head != NIL) {
        return;
    }
    if (side == Side::Buy) {
        bid_map.erase(level.price);
    } else {
        ask_map.erase(level.price);
    }
    level_pool.release(level_slot);
}

const Level* OrderBook::level_at(const Side side, const Price price) const {
    return side == Side::Buy ? find_level(bid_map, level_pool, price)
                             : find_level(ask_map, level_pool, price);
}

Result OrderBook::validate(const Order& order) {
    if (order.remaining <= 0 || order.remaining > order.original) {
        return Result::BadQty;
    }
    return Result::Ok;
}

bool OrderBook::crosses(const Order& incoming, const Price price) {
    if (incoming.type == OrderType::Market) {
        return true;
    }
    return incoming.side == Side::Buy ? incoming.price >= price : incoming.price <= price;
}

template <typename Book>
bool OrderBook::can_fill_from(const Book& book, const Order& incoming) const {
    Qty total = 0;
    for (const auto& [price, level_slot] : book) {
        if (!crosses(incoming, price)) {
            return false;
        }
        total += level_pool[level_slot].total;
        if (total >= incoming.remaining) {
            return true;
        }
    }
    return false;
}

bool OrderBook::can_fill(const Order& incoming) const {
    return incoming.side == Side::Buy ? can_fill_from(ask_map, incoming)
                                      : can_fill_from(bid_map, incoming);
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
