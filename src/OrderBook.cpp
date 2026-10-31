#include "lob/OrderBook.h"

#include <cstddef>
#include <iterator>

#include "lob/CoreTypes.h"

namespace lob {

namespace {

template <typename Book>
OrderLocation add_to_level(Book& book, const Order& order) {
    Level& level = book[order.price];
    level.orders.push_back(order);
    level.total += order.remaining;
    return {.price = order.price,
            .level = &level,
            .iter = std::prev(level.orders.end()),
            .side = order.side,
            .tif = order.tif};
}

template <typename Book>
void erase_from_level(Book& book, const OrderLocation& loc) {
    Level& level = *loc.level;
    level.total -= loc.iter->remaining;
    level.orders.erase(loc.iter);
    if (level.orders.empty()) {
        book.erase(loc.price);
    }
}

template <typename Book>
const Level* find_level(const Book& book, const Price price) {
    const auto iter = book.find(price);
    return iter == book.end() ? nullptr : &iter->second;
}

}  // namespace

Result OrderBook::add(const Order& order) {
    if (const Result result = validate(order); result != Result::Ok) {
        return result;
    }

    const auto [iter, inserted] = order_lookup.try_emplace(order.id);
    if (!inserted) {
        return Result::DuplicateId;
    }

    iter->second =
        order.side == Side::Buy ? add_to_level(bid_map, order) : add_to_level(ask_map, order);
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

    Order& resting = *iter->second.iter;
    const Qty filled = resting.original - resting.remaining;

    if (newPrice == resting.price && newQty <= resting.remaining) {
        iter->second.level->total -= resting.remaining - newQty;
        resting.remaining = newQty;
        resting.original = filled + newQty;
        return Result::Ok;
    }

    Order updated = resting;
    updated.price = newPrice;
    updated.original = filled + newQty;
    updated.remaining = newQty;

    erase_order(iter);
    return add(updated);
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
        if (iter->second.tif == Tif::Day) {
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

    ids.reserve(level->orders.size());
    for (const Order& order : level->orders) {
        ids.push_back(order.id);
    }
    return ids;
}

OrderBook::LookupIter OrderBook::erase_order(LookupIter iter) {
    const auto& loc = iter->second;

    if (loc.side == Side::Buy) {
        erase_from_level(bid_map, loc);
    } else {
        erase_from_level(ask_map, loc);
    }

    return order_lookup.erase(iter);
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
