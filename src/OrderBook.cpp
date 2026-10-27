#include "lob/OrderBook.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>

#include "lob/CoreTypes.h"

namespace lob {

namespace {

template <typename Book>
OrderLocation add_to_level(Book& book, const Order& order) {
    Level& level = book[order.price];
    level.orders.push_back(order);
    level.total += order.remaining;
    return {.side = order.side,
            .price = order.price,
            .tif = order.tif,
            .level = &level,
            .iter = std::prev(level.orders.end())};
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

void OrderBook::add(const Order& order) {
    if (!checkOrder(order)) {
        return;
    }

    if (order.side == Side::Buy) {
        order_lookup[order.id] = add_to_level(bid_map, order);
    } else {
        order_lookup[order.id] = add_to_level(ask_map, order);
    }
}

void OrderBook::cancel(const OrderId id) { remove(id); }

void OrderBook::modify(const OrderId id, const Price newPrice, const Qty newQty) {
    auto iter = order_lookup.find(id);

    if (iter == order_lookup.end()) {
        std::cout << "Error: Order ID " << id << " doesn't exist.\n";
        return;
    }

    if (newQty <= 0) {
        erase_order(iter);
        return;
    }

    Order& resting = *iter->second.iter;
    const Qty filled = resting.original - resting.remaining;

    if (newPrice == resting.price && newQty <= resting.remaining) {
        iter->second.level->total -= resting.remaining - newQty;
        resting.remaining = newQty;
        resting.original = filled + newQty;
        return;
    }

    Order updated = resting;
    updated.price = newPrice;
    updated.original = filled + newQty;
    updated.remaining = newQty;

    erase_order(iter);
    add(updated);
}

Price OrderBook::best_bid() {
    if (bid_map.empty()) {
        std::cout << "Bids are empty. Returning INT64_MIN.\n";
        return INT64_MIN;
    }

    return bid_map.begin()->first;
}

Price OrderBook::best_ask() {
    if (ask_map.empty()) {
        std::cout << "Asks are empty. Returning INT64_MAX.\n";
        return INT64_MAX;
    }

    return ask_map.begin()->first;
}

Price OrderBook::spread() { return best_ask() - best_bid(); }

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

void OrderBook::remove(const OrderId id) {
    auto iter = order_lookup.find(id);

    if (iter == order_lookup.end()) {
        std::cout << "Error: Order ID " << id << " doesn't exist.\n";
        return;
    }

    erase_order(iter);
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

bool OrderBook::checkOrder(const Order& order) {
    if (order_lookup.find(order.id) != order_lookup.end()) {
        std::cout << "Error: Order ID " << order.id << " already exists.\n";
        return false;
    }

    if (order.remaining <= 0) {
        std::cout << "Error: Order Id " << order.id
                  << " Can't create an order with quantity smaller or equal to zero.\n";
        return false;
    }

    if (order.remaining > order.original) {
        std::cout << "Error: Order Id " << order.id
                  << " Remaining quantity can't be larger than the original quantity.\n";
        return false;
    }

    return true;
}

bool OrderBook::crosses(const Order& incoming, const Price price) {
    if (incoming.type == OrderType::Market) {
        return true;
    }
    return incoming.side == Side::Buy ? incoming.price >= price : incoming.price <= price;
}
}  // namespace lob
