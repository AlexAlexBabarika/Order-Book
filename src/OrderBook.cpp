#include "lob/OrderBook.h"

#include <cstddef>
#include <cstdint>

#include "lob/CoreTypes.h"

namespace lob {

namespace {

template <typename Book>
Level& find_or_add_level(Book& book, const Price price) {
    return book.try_emplace(price, Level{.price = price}).first->second;
}

template <typename Book>
const Level* find_level(const Book& book, const Price price) {
    const auto iter = book.find(price);
    return iter == book.end() ? nullptr : &iter->second;
}

}  // namespace

OrderBook::OrderBook(const std::uint32_t max_orders) : order_pool(max_orders) {}

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
    Level& level = *resting.level;
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
        link_back(level, slot);
    } else {
        erase_if_empty(level, resting.side);
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
    const Level& level = *node.level;

    unlink(slot);
    erase_if_empty(level, node.side);
    order_pool.release(slot);
    return order_lookup.erase(iter);
}

void OrderBook::link_back(Level& level, const Slot slot) {
    OrderNode& node = order_pool[slot];
    node.level = &level;
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
    Level& level = *node.level;
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

Level& OrderBook::level_for(const Side side, const Price price) {
    return side == Side::Buy ? find_or_add_level(bid_map, price)
                             : find_or_add_level(ask_map, price);
}

void OrderBook::erase_if_empty(const Level& level, const Side side) {
    if (level.head != NIL) {
        return;
    }
    // A copy, because erasing destroys the level that holds it
    const Price price = level.price;
    if (side == Side::Buy) {
        bid_map.erase(price);
    } else {
        ask_map.erase(price);
    }
}

const Level* OrderBook::level_at(const Side side, const Price price) const {
    return side == Side::Buy ? find_level(bid_map, price) : find_level(ask_map, price);
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
bool OrderBook::can_fill_from(const Book& book, const Order& incoming) {
    Qty total = 0;
    for (const auto& [price, level] : book) {
        if (!crosses(incoming, price)) {
            return false;
        }
        total += level.total;
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
